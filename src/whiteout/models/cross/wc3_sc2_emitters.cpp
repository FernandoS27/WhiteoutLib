// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/cross/wc3_sc2_emitters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <numbers>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/anim/pose.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/inline_models.h>

#include "../wem/converters/m3_emitters.h"
#include "../wem/converters/m3_track_sink.h"
#include "../wem/materials/m3_core.h"

namespace whiteout {
namespace models {
namespace cross {

namespace {

namespace wem = ::whiteout::models::wem;
namespace geom = wem::geom;
using wem::AnimChannel;
using wem::Channel;
using wem::DiagCode;
using wem::Document;
using wem::ElementKind;
using wem::ElementRef;
using wem::Interpolation;
using wem::kInvalidIndex;
using wem::kInvalidNode;
using wem::Model;
using wem::Node;
using wem::NodeKind;
using wem::SubTrack;
using wem::TrackTarget;

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDegToRad = kPi / 180.0f;

/// The runtime's pool cap: 0x200000 bytes of vertex arena over a 464-byte
/// element (SC2_PARTICLE_RE §3.1). An emitter asking for more gets fewer.
constexpr u32 kMaxParticles = 0x200000u / 464u;

/// A mid time the clone path clamps to (`InitCopy`), written as Blizzard's
/// tools write every unkeyed one.
constexpr f32 kMidTimeRest = 0.996f;

/// The node flag bit PRE2's particle system reads as the space its particles
/// live in (`mdx::Node::NodeFlag::ModelSpace`).
constexpr i64 kModelSpaceBit = 0x80000;

/// Warcraft III's squirt edge: the rate crosses this on its way up
/// (`actor_eval.cpp`, `ps.emissionRate > 0.02f && last <= 0.02f`).
constexpr f32 kSquirtEdge = 0.02f;

/// The native key on the helper `PrepareWc3Effects` plants under an
/// off-centre ribbon: the ribbon's record rides it.
constexpr const char* kRibbonOffset = "wc3RibbonOffset";

/// Half a height difference below this, in Warcraft III units, is a centred
/// ribbon.
constexpr f32 kRibbonOffsetEpsilon = 1e-4f;

template <class T>
void Rest(m3::AnimRef<T>& ref, const T& value) {
    ref.initValue = value;
    ref.nullValue = value;
}

/// The emission rate's rest, with its null at 0. The runtime takes an unkeyed
/// emitter whose rate and squirt both sit at their nulls for one that emits
/// nothing of its own (`CParticleSystem_UpdateEmitterState` raises `emitFlags`
/// 0x10 and `GetLifeState` retires it once empty), so a null restating the rest
/// kept every crossed emitter from starting in the game and the editor.
/// Blizzard's tools write 0 on all 3,650 War3 `PAR_`.
void RestEmission(m3::AnimRef<f32>& rate, f32 value) {
    rate.initValue = value;
    rate.nullValue = 0.0f;
}

/// A byte the way the Warcraft III renderer quantises a segment colour: the
/// product truncated, clamped (`particle_adapters.cpp`, `Quant8`).
u8 ColorByte(f32 unit) {
    const f32 v = unit * 255.0f;
    return v <= 0.0f ? 0 : v >= 255.0f ? 255 : static_cast<u8>(v);
}

m3::ColorBGRA Color(const Vector3f& rgb, u8 alpha) {
    m3::ColorBGRA c;
    c.r = ColorByte(rgb.x);
    c.g = ColorByte(rgb.y);
    c.b = ColorByte(rgb.z);
    c.a = alpha;
    return c;
}

/// Whether a Warcraft III particle emitter's particles live in its node's
/// space. The import states the bit as `NodeFlags::ModelSpace`, which
/// `PrepareWc3Effects` clears; the node's raw flag word keeps it.
bool ParticlesInModelSpace(const Node& node) {
    return (node.native.value("mdxFlagBits", 0) & kModelSpaceBit) != 0 ||
           hasFlag(node.flags, wem::NodeFlags::ModelSpace);
}

// ----------------------------------------------------------------------------
// Keys: a property's per-clip sub-tracks, and what they become
// ----------------------------------------------------------------------------

/// One Warcraft III property's keys, as the import cut them into the clips
/// (§10.8): per clip that keys it, that clip's sub-track.
struct Cuts {
    geom::AttrType type = geom::AttrType::F32;
    std::vector<std::pair<u32, const SubTrack*>> clips;

    bool keyed() const {
        return !clips.empty();
    }

    const SubTrack* in(u32 clip) const {
        for (const auto& [index, track] : clips) {
            if (index == clip) {
                return track;
            }
        }
        return nullptr;
    }
};

/// A StarCraft II property's keys, per clip, in the property's own type.
using Keyed = std::vector<std::pair<u32, SubTrack>>;

u32 Components(geom::AttrType type) {
    switch (type) {
    case geom::AttrType::F32x2:
        return 2;
    case geom::AttrType::F32x3:
        return 3;
    case geom::AttrType::F32x4:
        return 4;
    default:
        return 1;
    }
}

/// Element @p c of key @p k's value -- the value itself, never a tangent.
f32 ValueOf(const SubTrack& track, geom::AttrType type, std::size_t k, u32 c) {
    const std::size_t stride = wem::ValuesPerKey(track.interp) * geom::AttrTypeSize(type);
    f32 value = 0.0f;
    std::memcpy(&value, track.values.data() + k * stride + c * sizeof(f32), sizeof(f32));
    return value;
}

/// @p track at @p time, the value element only: held before the first key and
/// after the last, held across a step, a straight line otherwise. A smooth
/// key's tangents are not read -- no M3 stream carries them.
void Sample(const SubTrack& track, geom::AttrType type, f32 time, f32* out) {
    const u32 comps = Components(type);
    const std::vector<f32>& t = track.times;
    const auto take = [&](std::size_t k) {
        for (u32 c = 0; c < comps; ++c) {
            out[c] = ValueOf(track, type, k, c);
        }
    };
    if (time <= t.front()) {
        take(0);
        return;
    }
    if (time >= t.back()) {
        take(t.size() - 1);
        return;
    }
    std::size_t hi = 1;
    while (hi < t.size() && t[hi] < time) {
        ++hi;
    }
    if (track.interp == Interpolation::Step || t[hi] == t[hi - 1]) {
        take(t[hi] <= time ? hi : hi - 1);
        return;
    }
    const f32 f = (time - t[hi - 1]) / (t[hi] - t[hi - 1]);
    for (u32 c = 0; c < comps; ++c) {
        const f32 lo = ValueOf(track, type, hi - 1, c);
        const f32 up = ValueOf(track, type, hi, c);
        out[c] = lo + (up - lo) * f;
    }
}

template <class T>
void AppendValue(std::vector<u8>& bytes, const T& value) {
    const u8* raw = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

/// @p cuts with every value mapped through @p fn, clip by clip. A step stays a
/// step and a smooth key becomes a straight one.
template <class R, class Fn>
Keyed MapKeys(const Cuts& cuts, Fn fn) {
    Keyed out;
    for (const auto& [clip, track] : cuts.clips) {
        SubTrack mapped;
        mapped.interp =
            track->interp == Interpolation::Step ? Interpolation::Step : Interpolation::Linear;
        mapped.times = track->times;
        for (std::size_t k = 0; k < track->times.size(); ++k) {
            AppendValue(mapped.values, static_cast<R>(fn(ValueOf(*track, cuts.type, k, 0))));
        }
        out.emplace_back(clip, std::move(mapped));
    }
    return out;
}

/// @p cuts as they are, for a property whose values cross unchanged.
Keyed CopyKeys(const Cuts& cuts) {
    Keyed out;
    for (const auto& [clip, track] : cuts.clips) {
        out.emplace_back(clip, *track);
    }
    return out;
}

/// The clips either of two properties keys, in order.
std::vector<u32> ClipsOf(const Cuts& a, const Cuts& b) {
    std::vector<u32> clips;
    for (const Cuts* cuts : {&a, &b}) {
        for (const auto& entry : cuts->clips) {
            clips.push_back(entry.first);
        }
    }
    std::sort(clips.begin(), clips.end());
    clips.erase(std::unique(clips.begin(), clips.end()), clips.end());
    return clips;
}

/// The key times two sub-tracks share between them, in order.
std::vector<f32> TimesOf(const SubTrack* a, const SubTrack* b) {
    std::vector<f32> times;
    for (const SubTrack* track : {a, b}) {
        if (track != nullptr) {
            times.insert(times.end(), track->times.begin(), track->times.end());
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

/// Two properties as one, clip by clip: keyed on the union of their times,
/// each key the combine of both sampled there, a property the clip does not
/// key read at its rest. What a merged StarCraft II property needs when
/// Warcraft III keys its halves apart -- speed and its variation, width and
/// length, above and below. The result steps only where both do.
template <class R, class Fn>
Keyed MergeKeys(const Cuts& a, f32 restA, const Cuts& b, f32 restB, Fn combine) {
    Keyed out;
    for (const u32 clip : ClipsOf(a, b)) {
        const SubTrack* ta = a.in(clip);
        const SubTrack* tb = b.in(clip);
        SubTrack merged;
        const bool stepped = (ta == nullptr || ta->interp == Interpolation::Step) &&
                             (tb == nullptr || tb->interp == Interpolation::Step);
        merged.interp = stepped ? Interpolation::Step : Interpolation::Linear;
        for (const f32 time : TimesOf(ta, tb)) {
            f32 va = restA;
            f32 vb = restB;
            if (ta != nullptr) {
                Sample(*ta, a.type, time, &va);
            }
            if (tb != nullptr) {
                Sample(*tb, b.type, time, &vb);
            }
            merged.times.push_back(time);
            AppendValue(merged.values, static_cast<R>(combine(va, vb)));
        }
        out.emplace_back(clip, std::move(merged));
    }
    return out;
}

/// The largest value @p cuts ever keys, or @p rest.
f32 MaxOf(const Cuts& cuts, f32 rest) {
    if (!cuts.keyed()) {
        return rest;
    }
    f32 best = -1e30f;
    for (const auto& [clip, track] : cuts.clips) {
        for (std::size_t k = 0; k < track->times.size(); ++k) {
            best = (std::max)(best, ValueOf(*track, cuts.type, k, 0));
        }
    }
    return best;
}

/// Warcraft III's squirt, as StarCraft II's: a burst key at every RISING edge
/// of the stepped emission rate inside each clip, its count the rate the edge
/// rises to. A flat key would fire again every time a playhead crosses it,
/// where Warcraft III bursts only on the way up. @p largest takes the largest
/// burst.
Keyed Squirt(const Cuts& rate, const Document& document, u32& largest) {
    Keyed out;
    largest = 0;
    for (const auto& [clip, track] : rate.clips) {
        const f32 duration = document.clips[clip].duration;
        // Only the keys the window plays: Warcraft III's engine reads no
        // bracket key, and holds the last one across the wrap.
        std::vector<std::pair<f32, f32>> keys;
        for (std::size_t k = 0; k < track->times.size(); ++k) {
            const f32 t = track->times[k];
            if (t < -1e-4f || t > duration + 1e-4f) {
                continue;
            }
            keys.emplace_back((std::max)(t, 0.0f), ValueOf(*track, rate.type, k, 0));
        }
        SubTrack bursts;
        bursts.interp = Interpolation::Step;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const f32 before = k == 0 ? keys.back().second : keys[k - 1].second;
            if (!(before <= kSquirtEdge && keys[k].second > kSquirtEdge)) {
                continue;
            }
            const u32 count = static_cast<u32>(keys[k].second);
            if (count == 0) {
                continue;
            }
            largest = (std::max)(largest, count);
            bursts.times.push_back(keys[k].first);
            AppendValue(bursts.values, count);
        }
        if (!bursts.times.empty()) {
            out.emplace_back(clip, std::move(bursts));
        }
    }
    return out;
}

/// A colour and an alpha as one `RGBA` property, clip by clip; either may be
/// unkeyed, and then rests.
Keyed ColorKeys(const Cuts& color, const Cuts& alpha, const Vector3f& restColor,
                f32 restAlpha) {
    Keyed out;
    for (const u32 clip : ClipsOf(color, alpha)) {
        out.emplace_back(clip, wem::m3_sink::MergeColorAlpha(color.in(clip), alpha.in(clip),
                                                             restColor, restAlpha));
    }
    return out;
}

// An AnimRef's rest as its channel's `initValue` (§10.8.1), in the channel
// convention `m3_emitters` crosses the records by.
std::vector<u8> RestBytes(f32 value) {
    std::vector<u8> bytes;
    AppendValue(bytes, value);
    return bytes;
}
std::vector<u8> RestBytes(const Vector2f& value) {
    std::vector<u8> bytes;
    AppendValue(bytes, value);
    return bytes;
}
std::vector<u8> RestBytes(const Vector3f& value) {
    std::vector<u8> bytes;
    AppendValue(bytes, value);
    return bytes;
}
std::vector<u8> RestBytes(const m3::ColorBGRA& value) {
    std::vector<u8> bytes;
    AppendValue(bytes, Vector4f{static_cast<f32>(value.r) / 255.0f,
                                static_cast<f32>(value.g) / 255.0f,
                                static_cast<f32>(value.b) / 255.0f,
                                static_cast<f32>(value.a) / 255.0f});
    return bytes;
}
std::vector<u8> RestBytes(u16 value) {
    std::vector<u8> bytes;
    AppendValue(bytes, static_cast<u32>(value));
    return bytes;
}
std::vector<u8> RestBytes(u32 value) {
    std::vector<u8> bytes;
    AppendValue(bytes, value);
    return bytes;
}

// ----------------------------------------------------------------------------
// Materials
// ----------------------------------------------------------------------------

enum class Filter : u8 { Blend, Additive, Modulate, Modulate2x, AlphaKey };

struct MaterialKey {
    std::string texture;
    Filter filter = Filter::Blend;
    bool unshaded = false;
    bool unfogged = false;
    bool twoSided = false;
    i32 priority = 0;
    bool particle = true;
    /// The emitter draws Warcraft III's team glow: `texture` lit by the team.
    bool teamGlow = false;
    /// `texture` is the glow's own mask, whose alpha is the colour's weight
    /// alone -- the glow draws with a flat alpha in Warcraft III.
    bool glowMask = false;
    /// The source texture's address mode, `TEXS` 0x1 wrap U and 0x2 wrap V --
    /// the only word Warcraft III reads, where a clear bit is a clamp.
    u32 wrap = 0;
    /// A StarCraft II blend named outright, over `filter` (a placed PopcornFX
    /// record's look, `PlaceSc2Records`).
    i32 blend = -1;
    /// The depth blend's falloff; 0 draws hard.
    f32 softness = 0.04f;
    /// The diffuse's multiply; above 1 its clamp is off.
    f32 gain = 1.0f;
    /// A baked sprite's quad (`Sc2BakedSprite`): drawn by its own UVs, as
    /// geometry is.
    bool mesh = false;
    /// `Sc2Look::uvTiling`.
    f32 tileU = 1.0f, tileV = 1.0f;
    /// `Sc2Look::displace`.
    f32 displace = 0.0f;

    auto tie() const {
        return std::tie(texture, filter, unshaded, unfogged, twoSided, priority, particle, teamGlow,
                        glowMask, wrap, blend, softness, gain, mesh, tileU, tileV, displace);
    }
    bool operator<(const MaterialKey& other) const {
        return tie() < other.tie();
    }
};

/// A textured layer at the rests every shipped textured layer states, addressed
/// the way the source's texture @p wrap word says.
m3::TextureLayer TexturedLayer(const std::string& path, bool particle, u32 wrap,
                               Vector2f tiling = Vector2f(1.0f, 1.0f)) {
    m3::TextureLayer layer;
    layer.texturePath = path;
    layer.color.initValue = m3::ColorBGRA{255, 255, 255, 255};
    layer.color.nullValue = layer.color.initValue;
    layer.rgbMultiply.initValue = 1.0f;
    layer.mapAlpha.initValue = 1.0f;
    layer.uvTiling.initValue = Vector2f(1.0f, 1.0f);
    layer.uvTiling.nullValue = Vector2f(1.0f, 1.0f);
    layer.wTiling.initValue = 1.0f;
    layer.wTiling.nullValue = 1.0f;
    layer.triplanarScale.initValue = Vector3f{1.0f, 1.0f, 1.0f};
    // What Blizzard's own particle layers state: the clamp and the pair its
    // precondition names, and -- on a particle -- the flipbook marker beside
    // the flipbook mapping, which is what uploads the frames at all
    // (`m3_surface_table.cpp`: the first active layer on ParticleFlipbook).
    // Their layers also all wrap; ours wrap where the source texture does, as
    // the geoset layers do (`mdx_core`), since a clamped sheet is what the
    // emitter drew in Warcraft III.
    layer.flags = m3::TextureLayerFlag::ColorClamp | m3::TextureLayerFlag::ColorAdd |
                  m3::TextureLayerFlag::ColorMultiply;
    if ((wrap & 0x1u) != 0) {
        layer.flags |= m3::TextureLayerFlag::UVWrapX;
    }
    if ((wrap & 0x2u) != 0) {
        layer.flags |= m3::TextureLayerFlag::UVWrapY;
    }
    if (particle && tiling.x == 1.0f && tiling.y == 1.0f) {
        layer.flags |= m3::TextureLayerFlag::ParticleUVFlipbook;
        layer.uvMapping = m3::UVMappingMode::ParticleFlipbook;
    } else if (particle) {
        // Mirrored: the explicit mapping reads the quad's own corners, and the
        // transform scales about the centre. The null stays at one, so the game
        // writes the mirror (it writes only an init off its null).
        layer.uvMapping = m3::UVMappingMode::ExplicitUV0;
        layer.uvTiling.initValue = tiling;
    }
    return layer;
}

/// The StarCraft II material each look of an effect needs, one per look, each
/// a material slot of the model with that material bound in the profile's set.
class Materials {
public:
    Materials(Document& document, Model& model, wem::ProfileId profile,
              Wc3EmitterReport& report)
        : document_(document), model_(model), profile_(profile), report_(report) {}

    /// The slot for @p key, made on first use.
    u32 slot(const MaterialKey& key) {
        const auto found = cache_.find(key);
        if (found != cache_.end()) {
            return found->second;
        }
        const u32 added = key.displace > 0.0f ? bindDisplacement(displacement(key)) : bind(build(key));
        cache_.emplace(key, added);
        ++report_.materials;
        return added;
    }

    /// The `DIS_` @p key asks for: its texture the normal map, read through
    /// the particle's flipbook as a standard layer is.
    m3::DisplacementMaterial displacement(const MaterialKey& key) const {
        m3::DisplacementMaterial mat;
        mat.name = "Displace_" + std::to_string(cache_.size());
        mat.strength.initValue = key.displace;
        mat.strength.nullValue = key.displace;
        if (!key.texture.empty()) {
            mat.normalMap = TexturedLayer(key.texture, key.particle && !key.mesh, key.wrap);
        }
        mat.priority = static_cast<u32>(std::max(key.priority, 0));
        return mat;
    }

    /// A slot of its own for @p mat, which no other record shares: a baked
    /// sprite's, whose multiplies are keyed.
    u32 own(m3::StandardMaterial mat) {
        ++report_.materials;
        return bind(std::move(mat));
    }

    /// The material @p key asks for.
    m3::StandardMaterial build(const MaterialKey& key) const {
        m3::StandardMaterial mat;
        mat.name = std::string(key.mesh       ? "Bake_"
                               : key.particle ? "Particle_"
                                              : "Ribbon_") +
                   std::to_string(cache_.size());
        // Geometry visible, and out of shadows and hit tests both ways -- the
        // header every one of Blizzard's own effect materials states -- plus
        // what the Warcraft III record said about light and fog.
        mat.flags = m3::MaterialFlag::GeometryVisible | m3::MaterialFlag::TransparentShadows |
                    m3::MaterialFlag::NoShadowsReceive | m3::MaterialFlag::NoHitTest |
                    m3::MaterialFlag::NoShadowsCast;
        if (key.unshaded) {
            mat.flags |= m3::MaterialFlag::Unshaded;
        }
        if (key.unfogged) {
            mat.flags |= m3::MaterialFlag::Unfogged;
        }
        if (key.twoSided) {
            mat.flags |= m3::MaterialFlag::TwoSided;
        }
        mat.additionalFlags = m3::MaterialAdditionalFlag::VertexColor |
                              m3::MaterialAdditionalFlag::VertexAlpha;
        if (key.softness > 0.0f) {
            mat.additionalFlags |= m3::MaterialAdditionalFlag::DepthBlendFalloff;
        }
        mat.materialClass = m3::MaterialClass::SpecialFX;
        mat.priority = key.priority;
        mat.specularExponent = 20.0f;
        mat.hdrSpecularMultiplier = 1.5f;
        mat.hdrEmissiveMultiplier = 1.5f;
        mat.depthBlendFalloff = key.softness > 0.0f ? key.softness : 0.0f;

        const bool textured = !key.texture.empty();
        const bool flipbook = key.particle && !key.mesh;
        if (textured && key.teamGlow) {
            // The team's colour, weighted by the texture's alpha on the emissive
            // team op, and nothing under it: Blizzard's own Wisp spells its
            // team-glow emitters this way, on each emitter's own texture. The
            // export hands over the glow itself as a mask instead, its red moved
            // into alpha: the glow file keeps its shape on red over a flat
            // alpha, and an emissive slot reads that red gamma-decoded (War3
            // Wisp's glow came out far dimmer than Warcraft III's).
            mat.emissiveLayer1 =
                TexturedLayer(key.texture, flipbook, key.wrap, Vector2f(key.tileU, key.tileV));
            mat.emissiveLayer1->colorType = m3::ColorChannelSelect::Alpha;
            mat.emissiveBlendMode1 = m3::LayerBlendOp::TeamColorEmissiveAdd;
        } else if (textured) {
            mat.diffuseLayer =
                TexturedLayer(key.texture, flipbook, key.wrap, Vector2f(key.tileU, key.tileV));
        }
        // The filter as a blend, and the mask the blend reads. StarCraft II
        // blends and tests by the COMPOSED mask alpha, never the diffuse's own,
        // so an alpha-weighted filter with no alpha layer never sees the
        // texture's alpha at all: the mask is the diffuse's texture on its
        // alpha channel (§4.4 row 20).
        bool mask = false;
        switch (key.filter) {
        case Filter::Blend:
            mat.blendMode = m3::BlendMode::AlphaBlend;
            mask = true;
            break;
        case Filter::Additive:
            mat.blendMode = m3::BlendMode::AlphaAdd;
            mask = true;
            break;
        case Filter::Modulate:
            mat.blendMode = m3::BlendMode::Mod;
            break;
        case Filter::Modulate2x:
            mat.blendMode = m3::BlendMode::Mod2x;
            break;
        case Filter::AlphaKey:
            mat.blendMode = m3::BlendMode::Opaque;
            // 0.75 of 256, the Warcraft III Transparent cut.
            mat.alphaTestThreshold = 192;
            mask = true;
            break;
        }
        if (key.blend >= 0) {
            // Every blend that weighs by alpha reads the texture's: AlphaBlend,
            // AlphaAdd, and 6 and 7 (Mod with alpha, premultiplied).
            mat.blendMode = static_cast<m3::BlendMode>(key.blend);
            mat.alphaTestThreshold = 0;
            mask = key.blend == 1 || key.blend == 3 || key.blend == 6 || key.blend == 7;
        }
        if (mask && textured && !key.glowMask) {
            mat.alphaLayer1 = key.teamGlow ? *mat.emissiveLayer1 : *mat.diffuseLayer;
            mat.alphaLayer1->colorType = m3::ColorChannelSelect::Alpha;
        }
        // After the mask's copy, which keeps its weight. The layer computes
        // `sample * multiply`, then clamps (psmateriallayer.fx), so a gain
        // above 1 needs the clamp off.
        if (key.gain != 1.0f && mat.diffuseLayer.has_value()) {
            mat.diffuseLayer->rgbMultiply.initValue = key.gain;
            mat.diffuseLayer->flags = mat.diffuseLayer->flags & ~m3::TextureLayerFlag::ColorClamp;
        }
        // A placed record's team glow: the team op reads only the layer's
        // alpha, so its gain is the emissive multiplier, the colour the
        // vertex's. The game scales every emissive by the lighting's
        // HDREmisMultiplier (0.56 in the core rig) where a diffuse shows
        // as it is, so the multiplier undoes it to show as the other records
        // do. PopcornFX draws the texture's rgb times the colour, its alpha
        // the coverage the mask already takes: the op weighs by red.
        if (key.blend >= 0 && key.teamGlow && mat.emissiveLayer1.has_value()) {
            constexpr f32 kCoreEmissive = 0.56f;
            mat.hdrEmissiveMultiplier = key.gain / kCoreEmissive;
            mat.emissiveLayer1->colorType = m3::ColorChannelSelect::Red;
        }
        if (!key.particle && !key.mesh && mat.diffuseLayer.has_value()) {
            // Warcraft III runs a ribbon's U along its length; StarCraft II its
            // V (§4.5 row 10). A quarter turn lays one on the other. Its null
            // stays at zero: the game builds a layer's UV transform only from
            // an init off its null, as Blizzard's conversions write it.
            const Vector3f turn{0.0f, 0.0f, -kPi * 0.5f};
            mat.diffuseLayer->uvAngle.initValue = turn;
            mat.diffuseLayer->uvAngle.nullValue = Vector3f{0.0f, 0.0f, 0.0f};
            if (mat.alphaLayer1.has_value()) {
                mat.alphaLayer1->uvAngle.initValue = turn;
                mat.alphaLayer1->uvAngle.nullValue = Vector3f{0.0f, 0.0f, 0.0f};
            }
        }
        return mat;
    }

private:
    /// @p mat as a slot of its own, bound at every look of the profile's set.
    /// It enters WEM the way an `.m3` import brings one -- the record itself as
    /// the native block, in sync -- so `toM3` writes it back as it is.
    u32 bind(m3::StandardMaterial mat) {
        std::string name = mat.name;
        m3::Model scratch;
        scratch.standardMaterials.push_back(std::move(mat));
        return bind(std::move(name), scratch, m3::MaterialType::Standard);
    }

    u32 bindDisplacement(m3::DisplacementMaterial mat) {
        std::string name = mat.name;
        m3::Model scratch;
        scratch.displacementMaterials.push_back(std::move(mat));
        return bind(std::move(name), scratch, m3::MaterialType::Displacement);
    }

    /// @p scratch's one material of @p type as a slot of its own.
    u32 bind(std::string name, const m3::Model& scratch, m3::MaterialType type) {
        while (model_.slotIndex(name) != kInvalidIndex) {
            name += "_";
        }
        m3::MaterialMap entry;
        entry.materialType = type;
        entry.materialIndex = 0;
        wem::m3_core::Context context;
        context.modelVersion = profile_ == wem::ProfileId::Heroes ? 30u : 29u;
        for (u32 t = 0; t < document_.textures.size(); ++t) {
            context.texturesByPath.emplace_back(document_.textures[t].path, t);
        }
        wem::Diagnostics imported;
        wem::Material material =
            wem::m3_core::ImportMaterial(scratch, entry, profile_, context, imported);
        for (const wem::Diagnostic& said : imported.all()) {
            if (said.severity != wem::Severity::Info) {
                report_.diagnostics.add(said.severity, said.code, said.message, said.where,
                                        said.profile);
            }
        }

        const u32 index = model_.addSlot(name);
        for (wem::ProfileMaterialSet& set : model_.profileSets) {
            set.resizeBindings(model_.materialSlots.size());
        }
        wem::ProfileMaterialSet* set = model_.setFor(profile_);
        if (set == nullptr) {
            report_.diagnostics.warn(DiagCode::SlotNotBound,
                                     "the document carries no " +
                                         std::string(wem::Profile(profile_).displayName) +
                                         " material set; effect slot " + name + " is unbound",
                                     ElementRef(ElementKind::Slot, index), profile_);
            return index;
        }
        const u32 bound = static_cast<u32>(set->materials.size());
        set->materials.push_back(std::move(material));
        for (u32& pick : set->slotBindings[index].byLook) {
            pick = bound;
        }
        return index;
    }

    Document& document_;
    Model& model_;
    wem::ProfileId profile_;
    Wc3EmitterReport& report_;
    std::map<MaterialKey, u32> cache_;
};

Filter ParticleFilter(wem::Wc3ParticleFilter filter) {
    switch (filter) {
    case wem::Wc3ParticleFilter::Additive:
        return Filter::Additive;
    case wem::Wc3ParticleFilter::Modulate:
        return Filter::Modulate;
    case wem::Wc3ParticleFilter::Modulate2x:
        return Filter::Modulate2x;
    case wem::Wc3ParticleFilter::AlphaKey:
        return Filter::AlphaKey;
    default:
        return Filter::Blend;
    }
}

Filter LayerFilter(wem::native::MdxFilterMode mode) {
    switch (mode) {
    case wem::native::MdxFilterMode::Transparent:
        return Filter::AlphaKey;
    case wem::native::MdxFilterMode::Additive:
    case wem::native::MdxFilterMode::AddAlpha:
        return Filter::Additive;
    case wem::native::MdxFilterMode::Modulate:
        return Filter::Modulate;
    case wem::native::MdxFilterMode::Modulate2x:
        return Filter::Modulate2x;
    default:
        return Filter::Blend;
    }
}

// ----------------------------------------------------------------------------
// PRE2 -> PAR_ (§4.4)
// ----------------------------------------------------------------------------

/// A `PAR_` at the rests Blizzard's tools write for every field no §4.4 row
/// names (§4.6).
m3::ParticleEmitter RestParticle() {
    m3::ParticleEmitter p;
    p.mass = 1.0f;
    p.massRandom = 1.0f;
    p.worldForcesMassMultiplier = 1.0f;
    p.noiseEdge = 0.1f;
    Rest(p.shapeInner, Vector3f{0.1f, 0.1f, 0.1f});
    Rest(p.outerRadius, 0.1f);
    Rest(p.innerRadius, 0.1f);
    p.friction = 1.0f;
    p.instanceAngle = Vector3f{0.0f, 0.0f, 1.0f};
    p.instanceDistance = 1.0f;
    Rest(p.alphaThreshold, 192.0f);
    Rest(p.uvTiling, Vector2f(1.0f, 1.0f));
    p.lodReduce = 2;
    Rest(p.upperBound, 1.0f);
    p.rotationMidTime = kMidTimeRest;
    p.rotationFlags = m3::ParticleRotationFlag::Relative | m3::ParticleRotationFlag::AlwaysSet;
    p.flags = m3::ParticleFlag::UseVertexAlpha;
    p.collisionSpawnIndex = -1;
    p.trailLinkIndex = -1;
    p.splatProjectionIndex = -1;
    p.ribbonLinkIndex = -1;
    p.tailLength = 1.0f;
    return p;
}

void Flipbook(m3::ParticleEmitter& p, const wem::Wc3ParticleEmitter2Payload& pe,
              const wem::Wc3ParticleInterval& life, const wem::Wc3ParticleInterval& decay) {
    const u32 columns = (std::max)(pe.columns, u32{1});
    const u32 rows = (std::max)(pe.rows, u32{1});
    p.flipbookColumns = static_cast<u16>(columns);
    p.flipbookRows = static_cast<u16>(rows);
    p.flipbookColumnFraction = 1.0f / static_cast<f32>(columns);
    p.flipbookRowFraction = 1.0f / static_cast<f32>(rows);
    const u32 last = columns * rows - 1;
    const auto cell = [last](u32 v) { return static_cast<u8>((std::min)((std::min)(v, last), u32{255})); };
    // StarCraft II has one stop where Warcraft III has a life end and a decay
    // start. Where the decay picks up at the next cell -- 80% of shipped head
    // sheets -- the stop is that cell, so the end run starts where the decay
    // does; E2 measures it closer to Warcraft III's cells than the life end.
    p.flipbookStartInitIndex = cell(life.start);
    p.flipbookStartStopIndex = cell(decay.start == life.end + 1 ? decay.start : life.end);
    p.flipbookEndInitIndex = cell(decay.end);
    p.flipbookEndStopIndex = cell(decay.end);
    p.flipbookMidTime = (std::min)(pe.time, kMidTimeRest);
}

/// The share of an XY quad emitter's particles Warcraft III draws facing the
/// camera. It turns a quad by the velocity's XY only while that is longer than
/// 1e-3 Warcraft III units -- a squared 1e-6 (`particle_geometry.cpp`) -- and
/// takes the camera's axes below it, so a slow, narrow cone draws a share of
/// its quads as billboards: ChineseFairy's crown rays (speed 2, latitude 0.1
/// degrees) face the camera 29% of the time. The polar and speed draws are
/// uniform on their ranges (`PlaneShape`, `DrawSpeed`), so the share is the
/// measure of the draws that stay under it. @p lengthScale restates the
/// payload's speed in Warcraft III units.
f32 CameraFacingShare(const wem::Wc3ParticleEmitter2Payload& pe, f32 lengthScale) {
    constexpr f64 kTurningSpeed = 1e-3;
    constexpr f64 kHalfTurn = 3.14159265358979323846;
    constexpr int kSpeedSteps = 32;
    const f64 speed0 = static_cast<f64>(pe.speed) / static_cast<f64>(lengthScale);
    const f64 latitude = std::fabs(static_cast<f64>(pe.latitude)) * kHalfTurn / 180.0;
    f64 share = 0.0;
    for (int i = 0; i < kSpeedSteps; ++i) {
        const f64 draw = -1.0 + (2.0 * i + 1.0) / kSpeedSteps;
        const f64 speed = std::fabs(speed0 * (1.0 + pe.variation * draw));
        if (speed <= kTurningSpeed || latitude <= 0.0) {
            share += 1.0;
            continue;
        }
        // |sin(polar)| stays under the bound within asin(bound) of a multiple
        // of pi; the polar angle is uniform on [0, latitude].
        const f64 band = std::asin(kTurningSpeed / speed);
        f64 inside = 0.0;
        for (f64 centre = 0.0; centre - band < latitude; centre += kHalfTurn) {
            inside += (std::max)(0.0, (std::min)(latitude, centre + band) -
                                          (std::max)(0.0, centre - band));
        }
        share += inside / latitude;
    }
    return static_cast<f32>(share / kSpeedSteps);
}

// ----------------------------------------------------------------------------
// RIBB -> RIB_ (§4.5)
// ----------------------------------------------------------------------------

m3::RibbonEmitter RestRibbon() {
    m3::RibbonEmitter r;
    r.drag = 0.0f;
    r.mass = 1.0f;
    r.massRandom = 0.0f;
    r.bounce = 0.0f;
    Rest(r.lifetimeRandom, 0.0f);
    r.sizeMidTime = kMidTimeRest;
    r.colorMidTime = kMidTimeRest;
    r.alphaMidTime = kMidTimeRest;
    r.rotationMidTime = kMidTimeRest;
    Rest(r.maxLength, 1.0f);
    r.noiseEdge = 0.1f;
    r.edges = 5;
    r.innerRadius = 0.5f;
    // The enable flag's rest, with its null at 0, for the reason
    // `RestEmission` carries: an unkeyed ribbon whose `active` sits at its null
    // emits nothing of its own. `CRibbon_UpdateAnimatedDirtyState` raises bit
    // 0x4 unless `(null != 0) != (init != 0)` or the ref animates, and
    // `CRibbon_GetLifeState` then retires the emitter the moment it holds no
    // segments -- before it lays its first. Blizzard's tools write null 0 on
    // all 5,135 shipped `RIB_`.
    r.active.initValue = 1;
    r.active.nullValue = 0;
    r.lodReduce = 2;
    r.lodCut = 0;
    // +0x190 is the cross-section (planar) and +0x194 the cull method
    // (SC2_RIBBON_RE §1.1); the struct still carries the pre-RE names.
    r.emitterShape = 1;
    r.ribbonType = static_cast<m3::RibbonType>(0);
    // Pre-roll on (R4: `CRibbon_CatchUpEmission` runs only under 0x800, at the
    // frozen pose, so the strip it lays is coincident and invisible), and the
    // two bits inert at zero speed, yaw and pitch -- Blizzard's shape.
    r.flags = static_cast<m3::RibbonFlag>(0xC800u);
    r.additionalFlags = m3::RibbonAdditionalFlag::WorldSpace;
    return r;
}

// ----------------------------------------------------------------------------
// The crossing
// ----------------------------------------------------------------------------

class Crossing {
public:
    Crossing(Document& document, const Wc3EmitterOptions& options, Wc3EmitterReport& report)
        : document_(document), model_(document.models.front()), options_(options),
          report_(report), materials_(document, model_, options.profile, report),
          // From 1: a record's AnimRef names its stream by this id, and 0 is
          // none.
          nextId_(std::max<u32>(1, model_.animChannels.nextFreeId())) {}

    /// A `CORN` node's fitted records (`PlaceSc2Records`).
    void place(const Sc2CornCrossing& c) {
        if (c.node >= model_.nodes.size() ||
            (c.particles.empty() && c.sprites.empty() && c.ribbons.empty())) {
            return;
        }
        const NodeKind kind = model_.nodes.nodes[c.node].kind;
        if (kind != NodeKind::Wc3CornEmitter && kind != NodeKind::ParticleEmitter) {
            return;
        }
        // The node keeps its placement and children as a helper; what the
        // game drew it with goes with its kind.
        dropSystemChannels(c.node);
        {
            Node& host = model_.nodes.nodes[c.node];
            host.kind = NodeKind::Helper;
            host.resetPayloadForKind();
        }
        const u32 frame = addFrameNode(c.node);
        std::vector<u32> placed;
        for (std::size_t r = 0; r < c.particles.size(); ++r) {
            placed.push_back(
                placeRecord(c.particles[r], c.node, frame, "_" + std::to_string(r), r == 0));
        }
        for (std::size_t r = 0; r < c.particles.size(); ++r) {
            const i32 child = c.particles[r].trailChild;
            if (child >= 0 && static_cast<std::size_t>(child) < placed.size()) {
                std::get<wem::Sc2ParticleEmitterPayload>(model_.nodes.nodes[placed[r]].payload)
                    .trailLink = placed[static_cast<std::size_t>(child)];
            }
        }
        for (std::size_t s = 0; s < c.sprites.size(); ++s) {
            placeSprite(c.sprites[s], c.node, frame, "_bake" + std::to_string(s));
        }
        for (std::size_t r = 0; r < c.ribbons.size(); ++r) {
            placeRibbon(c.ribbons[r], frame, "_rib" + std::to_string(r));
        }
    }

    /// @p spec on a child of @p parent named for @p suffix, moved to its
    /// offset.
    void placeRibbon(const Sc2RibbonSpec& spec, u32 parent, const std::string& suffix) {
        m3::RibbonEmitter r = spec.record;
        wem::m3_emitters::ForEachRibbonRef(r, [&](u32, auto& ref) {
            const u32 id = ref.animId;
            ref.animId = 0;
            if (id != 0 && id <= spec.keys.size() && !spec.keys[id - 1].empty()) {
                pool_.push_back(spec.keys[id - 1]);
                ref.animId = static_cast<u32>(pool_.size());
            }
        });
        MaterialKey key = keyOf(spec.look);
        key.particle = false;
        r.materialIndex = materials_.slot(key);
        const u32 target = addRecordNode(parent, suffix.c_str());
        if (spec.offset.x != 0.0f || spec.offset.y != 0.0f || spec.offset.z != 0.0f) {
            moveNode(target, spec.offset);
        }
        keyChannels(target, NodeKind::Sc2RibbonEmitter, r,
                    [](auto& record, auto&& f) { wem::m3_emitters::ForEachRibbonRef(record, f); });
        Node& node = model_.nodes.nodes[target];
        node.kind = NodeKind::Sc2RibbonEmitter;
        node.payload = wem::m3_emitters::ImportRibbon(r, wem::m3_emitters::ImportLinks{});
        ++report_.ribbonRecords;
    }

    /// After every crossing: the sprites' quads as one mesh.
    void finish() {
        if (bakedQuads_ == 0) {
            return;
        }
        geom::MeshBuilder::BuildOutcome built = baked_.build();
        built.mesh.name = "CornBake";
        built.mesh.recomputeBounds();
        model_.meshes.push_back(std::move(built.mesh));
    }

    /// @p spec on a child of @p parent named for @p suffix, or on @p parent
    /// itself where it is the first record (@p first) and neither moves nor
    /// keeps the world's axes; @p host is the `CORN` node whose turn a
    /// `worldAxes` record undoes. Returns the node it is on.
    u32 placeRecord(const Sc2ParticleSpec& spec, u32 host, u32 parent, const std::string& suffix,
                    bool first) {
        m3::ParticleEmitter p = spec.record;
        // The caller's key ids, 1-based into its own list, become the pool's.
        wem::m3_emitters::ForEachParticleRef(p, [&](u32, auto& ref) {
            const u32 id = ref.animId;
            ref.animId = 0;
            if (id != 0 && id <= spec.keys.size() && !spec.keys[id - 1].empty()) {
                pool_.push_back(spec.keys[id - 1]);
                ref.animId = static_cast<u32>(pool_.size());
            }
        });
        // A displacement draws no colour, so a fade has nothing to key.
        p.materialIndex =
            (spec.colourKeys.empty() && spec.coverageKeys.empty()) || spec.look.displace > 0.0f
                ? materials_.slot(keyOf(spec.look))
                : ownMaterial(spec);
        const bool moved = spec.offset.x != 0.0f || spec.offset.y != 0.0f || spec.offset.z != 0.0f;
        u32 target = parent;
        // Above its offset, so the place off the axis turns with it.
        if (spec.spin != 0.0f) {
            target = spinNode(parent, spec.spin);
        }
        if (moved || spec.spin != 0.0f || (!first && !spec.worldAxes)) {
            target = addRecordNode(target, suffix.c_str());
            if (moved) {
                moveNode(target, spec.offset);
            }
        }
        if (spec.worldAxes) {
            // Below the move, so the place off the node turns with it.
            const Keyed& turns = worldTurns(host);
            target = addWorldNode(target, target == parent ? suffix + "w" : "w", turns);
            if (spec.coneTurns && p.velocityType == 0) {
                keyCone(p, turns);
            }
        }
        placeParticle(target, std::move(p));
        return target;
    }

    static MaterialKey keyOf(const Sc2Look& look) {
        MaterialKey key;
        key.texture = look.texture;
        key.unshaded = look.unshaded;
        key.unfogged = look.unfogged;
        key.twoSided = look.twoSided;
        key.priority = look.priority;
        key.wrap = look.wrap;
        key.blend = static_cast<i32>(look.blend);
        key.softness = look.softness;
        key.gain = look.gain;
        key.teamGlow = look.teamGlow;
        key.tileU = look.uvTiling.x;
        key.tileV = look.uvTiling.y;
        key.displace = look.displace;
        return key;
    }

    /// @p spec's look on a slot of its own: its colour's and coverage's
    /// multiplies keyed per clip, each key a factor on the layer's rest.
    u32 ownMaterial(const Sc2ParticleSpec& spec) {
        m3::StandardMaterial mat = materials_.build(keyOf(spec.look));
        m3::TextureLayer* colour = spec.look.teamGlow && mat.emissiveLayer1.has_value()
                                       ? &*mat.emissiveLayer1
                                   : mat.diffuseLayer.has_value() ? &*mat.diffuseLayer
                                                                  : nullptr;
        m3::TextureLayer* coverage = mat.alphaLayer1.has_value() ? &*mat.alphaLayer1 : nullptr;
        const auto keyLayer = [&](m3::TextureLayer* layer, const Sc2Keyed& keyed) {
            if (layer == nullptr || keyed.empty()) {
                return;
            }
            const f32 rest = layer->rgbMultiply.initValue;
            AnimChannel channel;
            channel.id = nextId_++;
            channel.target.kind = TrackTarget::Kind::Native;
            channel.target.sub = 5; // SD R3: an f32 stream
            channel.valueType = geom::AttrType::F32;
            channel.initValue.assign(reinterpret_cast<const u8*>(&rest),
                                     reinterpret_cast<const u8*>(&rest) + sizeof rest);
            model_.animChannels.add(channel);
            layer->rgbMultiply.animId = channel.id;
            for (const auto& [clip, track] : keyed) {
                if (clip >= document_.clips.size()) {
                    continue;
                }
                SubTrack copy = track;
                copy.channel = channel.id;
                for (std::size_t at = 0; at + sizeof(f32) <= copy.values.size();
                     at += sizeof(f32)) {
                    f32 v = 0.0f;
                    std::memcpy(&v, copy.values.data() + at, sizeof v);
                    v *= rest;
                    std::memcpy(copy.values.data() + at, &v, sizeof v);
                }
                wem::Clip& target = document_.clips[clip];
                if (target.containers.empty()) {
                    target.containers.emplace_back();
                }
                target.containers.front().subTracks.push_back(std::move(copy));
            }
            ++report_.keyedChannels;
        };
        keyLayer(colour, spec.colourKeys);
        keyLayer(coverage, spec.coverageKeys);
        return materials_.own(std::move(mat));
    }

    // --- Baked sprites (D§12.4) ---------------------------------------------

    /// The global loop of @p period seconds the sprites key on, made on first
    /// use: StarCraft II's auto-played loop (SEQS `AlwaysGlobal`), its one
    /// container concurrent, so it plays under whatever the host plays.
    u32 loopClip(f32 period) {
        for (const auto& [length, clip] : loops_) {
            if (std::abs(length - period) < 1e-4f) {
                return clip;
            }
        }
        const auto taken = [&](const std::string& name) {
            for (const wem::Clip& clip : document_.clips) {
                if (clip.name == name) {
                    return true;
                }
            }
            return false;
        };
        std::string name = "GLstand";
        for (u32 n = 2; taken(name); ++n) {
            name = n < 10 ? "GLstand 0" + std::to_string(n) : "GLstand " + std::to_string(n);
        }
        wem::Clip clip;
        clip.name = name;
        clip.model = 0;
        clip.duration = period;
        clip.looping = true;
        clip.flags =
            wem::ClipFlags::AutoPlay | wem::ClipFlags::Persistent | wem::ClipFlags::WorldClocked;
        clip.readRule = wem::ReadRule::Sc2;
        wem::SubTrackContainer container;
        container.name = name;
        container.concurrent = true;
        clip.containers.push_back(std::move(container));
        document_.clips.push_back(std::move(clip));
        const u32 index = static_cast<u32>(document_.clips.size() - 1);
        loops_.emplace_back(period, index);
        return index;
    }

    /// @p values (@p width floats a key, one key a step of @p period from 0)
    /// as @p channel's track on the loop of @p period, closed on its first
    /// key; the keys a line through their neighbours draws within @p tolerance
    /// dropped.
    void keyLoop(AnimChannel channel, u32 width, const std::vector<f32>& values, f32 period,
                 f32 tolerance) {
        const std::size_t count = values.size() / width;
        channel.id = nextId_++;
        model_.animChannels.add(channel);
        std::vector<f32> times(count + 1);
        std::vector<f32> keys(values);
        keys.insert(keys.end(), values.begin(), values.begin() + width);
        for (std::size_t k = 0; k <= count; ++k) {
            times[k] = period * static_cast<f32>(k) / static_cast<f32>(count);
        }
        // Greedy: from each kept key, as far as a line to a later one draws
        // every key between within the tolerance.
        std::vector<std::size_t> kept{0};
        for (std::size_t a = 0; a < count;) {
            std::size_t b = a + 1;
            for (std::size_t c = a + 2; c <= count; ++c) {
                bool fits = true;
                for (std::size_t m = a + 1; m < c && fits; ++m) {
                    const f32 f = (times[m] - times[a]) / (times[c] - times[a]);
                    for (u32 l = 0; l < width && fits; ++l) {
                        const f32 line =
                            keys[a * width + l] + (keys[c * width + l] - keys[a * width + l]) * f;
                        fits = std::abs(line - keys[m * width + l]) <= tolerance;
                    }
                }
                if (!fits) {
                    break;
                }
                b = c;
            }
            kept.push_back(b);
            a = b;
        }
        SubTrack track;
        track.channel = channel.id;
        track.interp = Interpolation::Linear;
        for (const std::size_t k : kept) {
            track.times.push_back(times[k]);
            const auto* raw = reinterpret_cast<const u8*>(keys.data() + k * width);
            track.values.insert(track.values.end(), raw, raw + width * sizeof(f32));
        }
        document_.clips[loopClip(period)].containers.front().subTracks.push_back(std::move(track));
        ++report_.keyedChannels;
    }

    /// A tolerance for keys spanning @p values: a five-hundredth of their
    /// range, and never under a millionth.
    static f32 toleranceOf(const std::vector<f32>& values, u32 width) {
        f32 range = 0.0f;
        for (u32 l = 0; l < width; ++l) {
            f32 lo = 1e30f, hi = -1e30f;
            for (std::size_t k = l; k < values.size(); k += width) {
                lo = std::min(lo, values[k]);
                hi = std::max(hi, values[k]);
            }
            range = std::max(range, hi - lo);
        }
        return std::max(range * 0.002f, 1e-6f);
    }

    /// Whether @p values (@p width floats a key) change over the loop.
    static bool moves(const std::vector<f32>& values, u32 width) {
        for (std::size_t k = width; k < values.size(); ++k) {
            if (values[k] != values[k % width]) {
                return true;
            }
        }
        return false;
    }

    /// @p node turning about its z at @p speed radians a second: keyed over
    /// two turns on that loop, so the half angle closes on itself and each
    /// key lerps the short way.
    void keySpin(u32 node, f32 speed) {
        constexpr u32 kKeys = 32;
        constexpr f32 kTwoTurns = 4.0f * std::numbers::pi_v<f32>;
        std::vector<f32> turns;
        for (u32 k = 0; k < kKeys; ++k) {
            const f32 half =
                0.5f * kTwoTurns * static_cast<f32>(k) / kKeys * (speed > 0.0f ? 1.0f : -1.0f);
            turns.insert(turns.end(), {0.0f, 0.0f, std::sin(half), std::cos(half)});
        }
        keyLoop(nodeChannel(node, Channel::Rotation, geom::AttrType::Quat), 4, turns,
                kTwoTurns / std::abs(speed), toleranceOf(turns, 4));
    }

    /// The bone under @p parent spinning at @p speed (`Sc2ParticleSpec::
    /// spin`), made on first use: the records of one speed share it.
    u32 spinNode(u32 parent, f32 speed) {
        for (const auto& [from, at, node] : spins_) {
            if (from == parent && at == speed) {
                return node;
            }
        }
        const u32 node =
            addRecordNode(parent, ("_spin" + std::to_string(spins_.size())).c_str());
        keySpin(node, speed);
        spins_.emplace_back(parent, speed, node);
        return node;
    }

    /// A node channel of @p node.
    static AnimChannel nodeChannel(u32 node, Channel which, geom::AttrType type) {
        AnimChannel channel;
        channel.target.kind = TrackTarget::Kind::Node;
        channel.target.node = node;
        channel.target.channel = which;
        channel.valueType = type;
        return channel;
    }

    /// @p sprite: its place (`P`, its riders' bone), its facing (`B`: a
    /// billboard, or a plane's turn; hidden where the effect is off) and its
    /// spin and size (`Q`, the quad's bone), each keyed on its period's loop
    /// where it moves.
    void placeSprite(const Sc2BakedSprite& sprite, u32 host, u32 frame, const std::string& name) {
        if (sprite.position.keys.empty() ||
            (sprite.draws &&
             (sprite.size.keys.empty() || sprite.rotation.keys.empty() ||
              sprite.brightness.keys.empty() ||
              sprite.alpha.keys.size() != sprite.brightness.keys.size() ||
              ((sprite.facing == Sc2BakedSprite::Facing::Plane ||
                sprite.facing == Sc2BakedSprite::Facing::Axis) &&
               sprite.turn.keys.empty())))) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "a baked sprite lacks a stream; not placed",
                                     ElementRef(ElementKind::Node, host));
            return;
        }
        const auto flat = [](const auto& from, auto get, u32 width) {
            std::vector<f32> out;
            out.reserve(from.size() * width);
            for (const auto& v : from) {
                const std::array<f32, 4> lanes = get(v);
                out.insert(out.end(), lanes.begin(), lanes.begin() + width);
            }
            return out;
        };
        // One hemisphere throughout, the closing key too, so each key lerps
        // the short way.
        const auto hemisphere = [](std::vector<Quaternion> q) {
            for (std::size_t k = 1; k <= q.size(); ++k) {
                Quaternion& b = q[k % q.size()];
                const Quaternion& a = q[k - 1];
                if (k < q.size() && a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
                    b = Quaternion{-b.x, -b.y, -b.z, -b.w};
                }
            }
            return q;
        };
        const auto key = [&](u32 node, Channel which, geom::AttrType type, u32 width,
                             const std::vector<f32>& values, f32 period) {
            if (period > 0.0f && values.size() > width && moves(values, width)) {
                keyLoop(nodeChannel(node, which, type), width, values, period,
                        toleranceOf(values, width));
            }
        };

        // Its place, in the world's axes above a child undoing the host's turn.
        const u32 place = sprite.worldAxes
                              ? addRecordNode(addWorldNode(frame, name, worldTurns(host)), "p")
                              : addRecordNode(frame, name.c_str());
        moveNode(place, sprite.position.keys.front());
        if (!sprite.path.empty()) {
            // Keyed per clip: a head that lives and dies.
            AnimChannel channel = nodeChannel(place, Channel::Translation, geom::AttrType::F32x3);
            channel.id = nextId_++;
            model_.animChannels.add(channel);
            for (const auto& [c, track] : sprite.path) {
                if (c >= document_.clips.size()) {
                    continue;
                }
                wem::Clip& clip = document_.clips[c];
                if (clip.containers.empty()) {
                    clip.containers.emplace_back();
                }
                SubTrack copy = track;
                copy.channel = channel.id;
                clip.containers.front().subTracks.push_back(std::move(copy));
            }
            ++report_.keyedChannels;
        } else {
            key(place, Channel::Translation, geom::AttrType::F32x3, 3,
                flat(
                    sprite.position.keys,
                    [](const Vector3f& v) { return std::array<f32, 4>{v.x, v.y, v.z, 0}; }, 3),
                sprite.position.period);
        }

        // Its orbit: a bone turning about the frame's z from its place, and
        // the arm out to where it draws.
        u32 drawnAt = place;
        if (sprite.orbit != 0.0f) {
            const u32 spin = addRecordNode(place, "_orbit");
            keySpin(spin, sprite.orbit);
            drawnAt = addRecordNode(spin, "_arm");
            moveNode(drawnAt, sprite.arm);
        }
        if (!sprite.draws) {
            placeRiders(sprite, host, drawnAt);
            return;
        }

        // Its facing.
        const u32 facing = addRecordNode(drawnAt, "_face", sprite.hidden.empty());
        {
            Node& node = model_.nodes.nodes[facing];
            if (sprite.facing == Sc2BakedSprite::Facing::Plane ||
                sprite.facing == Sc2BakedSprite::Facing::Axis) {
                const std::vector<Quaternion> turns = hemisphere(sprite.turn.keys);
                node.local.rotation = turns.front();
                node.poses.assign(model_.nodes.poseSchema.size(), node.local);
                key(facing, Channel::Rotation, geom::AttrType::Quat, 4,
                    flat(
                        turns,
                        [](const Quaternion& q) { return std::array<f32, 4>{q.x, q.y, q.z, q.w}; },
                        4),
                    sprite.turn.period);
                // About its axis to the eye: type 5 makes the bone's own
                // StarCraft II Y (WEM's -x) its z, and faces its StarCraft II
                // +Y (WEM's -x) away from the eye; the quad is two-sided.
                if (sprite.facing == Sc2BakedSprite::Facing::Axis) {
                    Node& axial = model_.nodes.nodes[facing];
                    wem::Sc2Billboard billboard;
                    billboard.type = 5;
                    billboard.cameraLookAt = true;
                    axial.sc2Billboards.push_back(billboard);
                    axial.flags = axial.flags | wem::Sc2BillboardFlags(5);
                }
            } else {
                wem::Sc2Billboard billboard;
                billboard.type = 6;
                billboard.cameraLookAt = sprite.facing == Sc2BakedSprite::Facing::Eye;
                node.sc2Billboards.push_back(billboard);
                node.flags = node.flags | wem::Sc2BillboardFlags(6);
            }
        }
        // Hidden in the clips the effect is off in: a step to 0 there.
        if (!sprite.hidden.empty()) {
            AnimChannel channel = nodeChannel(facing, Channel::Visibility, geom::AttrType::F32);
            channel.id = nextId_++;
            const f32 one = 1.0f;
            channel.initValue.assign(reinterpret_cast<const u8*>(&one),
                                     reinterpret_cast<const u8*>(&one) + sizeof one);
            model_.animChannels.add(channel);
            for (const u32 c : sprite.hidden) {
                if (c >= document_.clips.size()) {
                    continue;
                }
                wem::Clip& clip = document_.clips[c];
                if (clip.containers.empty()) {
                    clip.containers.emplace_back();
                }
                SubTrack track;
                track.channel = channel.id;
                track.interp = Interpolation::Step;
                track.times = {0.0f};
                const f32 zero = 0.0f;
                track.values.assign(reinterpret_cast<const u8*>(&zero),
                                    reinterpret_cast<const u8*>(&zero) + sizeof zero);
                clip.containers.front().subTracks.push_back(std::move(track));
            }
            ++report_.keyedChannels;
        }

        // Its spin and size: the quad lies in the bone's y-z plane, facing +x
        // (a billboard's eye side), width on y and height on z, so the spin
        // is about x. The bind is the facing's: the size and spin are keys.
        const u32 quad = addRecordNode(facing, "_quad");
        std::vector<Quaternion> spins;
        spins.reserve(sprite.rotation.keys.size());
        for (const f32 r : sprite.rotation.keys) {
            spins.push_back(Quaternion{std::sin(0.5f * r), 0.0f, 0.0f, std::cos(0.5f * r)});
        }
        spins = hemisphere(std::move(spins));
        {
            Node& node = model_.nodes.nodes[quad];
            node.local.rotation = spins.front();
            node.local.scale =
                Vector3f{1.0f, sprite.size.keys.front().x, sprite.size.keys.front().y};
            node.poses.assign(model_.nodes.poseSchema.size(), node.local);
        }
        key(quad, Channel::Rotation, geom::AttrType::Quat, 4,
            flat(
                spins, [](const Quaternion& q) { return std::array<f32, 4>{q.x, q.y, q.z, q.w}; },
                4),
            sprite.rotation.period);
        key(quad, Channel::Scale, geom::AttrType::F32x3, 3,
            flat(
                sprite.size.keys,
                [](const Vector2f& s) { return std::array<f32, 4>{1.0f, s.x, s.y, 0}; }, 3),
            sprite.size.period);

        // Its material: the look on its own slot, the colour's multiply and
        // the coverage's keyed on the layers that carry them.
        MaterialKey look = keyOf(sprite.look);
        look.mesh = true;
        look.gain = 1.0f;
        look.twoSided = true;
        m3::StandardMaterial mat = materials_.build(look);
        m3::TextureLayer* colour = mat.emissiveLayer1.has_value() && look.teamGlow
                                       ? &*mat.emissiveLayer1
                                   : mat.diffuseLayer.has_value() ? &*mat.diffuseLayer
                                                                  : nullptr;
        m3::TextureLayer* coverage = mat.alphaLayer1.has_value() ? &*mat.alphaLayer1 : nullptr;
        if (look.teamGlow) {
            // As a placed record's team glow (`Materials::build`), at gain 1:
            // the multiply is keyed.
            constexpr f32 kCoreEmissive = 0.56f;
            mat.hdrEmissiveMultiplier = 1.0f / kCoreEmissive;
        }
        const auto keyLayer = [&](m3::TextureLayer* layer, const Sc2Loop<f32>& loop) {
            const std::vector<f32>& values = loop.keys;
            if (layer == nullptr) {
                return;
            }
            layer->rgbMultiply.initValue = values.front();
            f32 peak = 0.0f;
            for (const f32 v : values) {
                peak = std::max(peak, v);
            }
            // The layer clamps after its multiply (psmateriallayer.fx).
            if (peak > 1.0f) {
                layer->flags = layer->flags & ~m3::TextureLayerFlag::ColorClamp;
            }
            if (loop.period <= 0.0f || values.size() < 2 || !moves(values, 1)) {
                return;
            }
            AnimChannel channel;
            channel.target.kind = TrackTarget::Kind::Native;
            channel.target.sub = 5; // SD R3: an f32 stream
            channel.valueType = geom::AttrType::F32;
            const f32 rest = values.front();
            channel.initValue.assign(reinterpret_cast<const u8*>(&rest),
                                     reinterpret_cast<const u8*>(&rest) + sizeof rest);
            layer->rgbMultiply.animId = nextId_;
            keyLoop(channel, 1, values, loop.period, toleranceOf(values, 1));
        };
        keyLayer(colour, sprite.brightness);
        keyLayer(coverage, sprite.alpha);
        const u32 slot = materials_.own(std::move(mat));

        // Its quad, at the bind: the facing's, which the quad's bone binds at.
        const Matrix44f bind = Matrix44f::inverse(model_.nodes.inverseBindMatrix(quad));
        const auto atBind = [&](const Vector3f& p) {
            Vector3f out;
            const f32 in[3] = {p.x, p.y, p.z};
            f32* o[3] = {&out.x, &out.y, &out.z};
            for (int k = 0; k < 3; ++k) {
                *o[k] = in[0] * bind.data[0][k] + in[1] * bind.data[1][k] +
                        in[2] * bind.data[2][k] + bind.data[3][k];
            }
            return out;
        };
        const auto direction = [&](const Vector3f& d) {
            Vector3f out{d.x * bind.data[0][0] + d.y * bind.data[1][0] + d.z * bind.data[2][0],
                         d.x * bind.data[0][1] + d.y * bind.data[1][1] + d.z * bind.data[2][1],
                         d.x * bind.data[0][2] + d.y * bind.data[1][2] + d.z * bind.data[2][2]};
            const f32 l = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
            return l > 0.0f ? Vector3f{out.x / l, out.y / l, out.z / l}
                            : Vector3f{1.0f, 0.0f, 0.0f};
        };
        wem::MeshSection section;
        section.name = model_.nodes.nodes[quad].name;
        section.materialSlot = slot;
        section.profiles = wem::ProfileBit(options_.profile);
        section.native.set(wem::kSectionVisibilityNode, static_cast<i64>(facing));
        const u32 at = baked_.addSection(std::move(section));
        // Corner by corner as cornflakes lays a quad: less width less height,
        // then round; the image's top row at the top (v 0).
        const Vector4f& cell = sprite.cell;
        const std::array<Vector3f, 4> corners{
            Vector3f{0.0f, -1.0f, -1.0f}, Vector3f{0.0f, 1.0f, -1.0f}, Vector3f{0.0f, 1.0f, 1.0f},
            Vector3f{0.0f, -1.0f, 1.0f}};
        const std::array<Vector2f, 4> uvs{Vector2f{cell.x, cell.w}, Vector2f{cell.z, cell.w},
                                          Vector2f{cell.z, cell.y}, Vector2f{cell.x, cell.y}};
        const std::array<u8, 4> tint{ColorByte(sprite.color.x), ColorByte(sprite.color.y),
                                     ColorByte(sprite.color.z), 255};
        const Vector3f normal = direction(Vector3f{1.0f, 0.0f, 0.0f});
        std::array<geom::VertexId, 4> v{};
        for (std::size_t k = 0; k < 4; ++k) {
            v[k] = baked_.addVertex(atBind(corners[k]));
            baked_.addInfluence(v[k], quad, 1.0f);
        }
        for (const std::array<std::size_t, 3>& t :
             {std::array<std::size_t, 3>{0, 1, 2}, std::array<std::size_t, 3>{0, 2, 3}}) {
            const geom::FaceId face = baked_.addTriangle(v[t[0]], v[t[1]], v[t[2]], at);
            for (u32 c = 0; c < 3; ++c) {
                baked_.setCornerAttr(face, c, geom::names::uv(0), uvs[t[c]]);
                baked_.setCornerAttr(face, c, geom::names::kNormal, normal);
                baked_.setCornerAttr(face, c, geom::names::color(0), tint);
            }
        }
        ++bakedQuads_;
        placeRiders(sprite, host, drawnAt);
    }

    /// @p sprite's riders and strips, where it draws (@p at).
    void placeRiders(const Sc2BakedSprite& sprite, u32 host, u32 at) {
        for (std::size_t r = 0; r < sprite.riders.size(); ++r) {
            placeRecord(sprite.riders[r], host, at, "_ride" + std::to_string(r), false);
        }
        for (std::size_t r = 0; r < sprite.ribbons.size(); ++r) {
            placeRibbon(sprite.ribbons[r], at, "_rib" + std::to_string(r));
        }
    }

    void run() {
        // The records a system adds are appended, so the count taken first
        // visits the source's nodes only.
        const u32 count = model_.nodes.size();
        std::vector<NodeKind> kinds(count);
        for (u32 n = 0; n < count; ++n) {
            kinds[n] = model_.nodes.nodes[n].kind;
        }
        u32 popcorn = 0;
        for (u32 n = 0; n < count; ++n) {
            if (model_.nodes.nodes[n].removed) {
                continue;
            }
            switch (model_.nodes.nodes[n].kind) {
            case NodeKind::Wc3ParticleEmitter2:
                if (options_.particles) {
                    particle(n);
                }
                break;
            case NodeKind::Wc3ParticleEmitter1:
                if (options_.particles) {
                    modelParticle(n);
                }
                break;
            case NodeKind::Wc3RibbonEmitter:
                if (options_.ribbons) {
                    ribbon(n);
                }
                break;
            case NodeKind::Attachment:
                if (options_.particles) {
                    attachedModel(n);
                }
                break;
            // A `.wem` written before `CORN` had a kind of its own holds one
            // as the generic reference.
            case NodeKind::Wc3CornEmitter:
            case NodeKind::ParticleEmitter:
                ++popcorn;
                break;
            default:
                break;
            }
        }
        // C8.4: a Reforged model's PopcornFX systems live in `.pkfx` files no
        // M3 record can name.
        if (popcorn != 0) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     std::to_string(popcorn) +
                                         " PopcornFX emitter(s) (CORN) did not cross",
                                     ElementRef());
        }
        // What a system became is held by the profile it was crossed for, whatever
        // the Warcraft III node's mask said: a document carrying both games keeps
        // the original out of the StarCraft II one (wem/nodes/presence.h).
        for (u32 n = 0; n < model_.nodes.size(); ++n) {
            if (n >= count || kinds[n] != model_.nodes.nodes[n].kind) {
                model_.nodes.nodes[n].profiles |= wem::ProfileBit(options_.profile);
            }
        }
    }

private:
    // --- PRE2 ----------------------------------------------------------------

    void particle(u32 n) {
        const wem::Wc3ParticleEmitter2Payload pe =
            std::get<wem::Wc3ParticleEmitter2Payload>(model_.nodes.nodes[n].payload);
        const std::string name = model_.nodes.nodes[n].name;
        const ElementRef where(ElementKind::Node, n);
        const bool modelSpace = ParticlesInModelSpace(model_.nodes.nodes[n]);
        using P = wem::Wc3Particle2Property;
        const Cuts speedKeys = cutsOf(n, P::Speed);
        const Cuts variationKeys = cutsOf(n, P::Variation);
        const Cuts latitudeKeys = cutsOf(n, P::Latitude);
        const Cuts gravityKeys = cutsOf(n, P::Gravity);
        const Cuts rateKeys = cutsOf(n, P::EmissionRate);
        const Cuts widthKeys = cutsOf(n, P::Width);
        const Cuts lengthKeys = cutsOf(n, P::Length);
        const bool squirt = pe.squirt;
        const bool line = pe.lineEmitter;
        const bool xyQuad = pe.xyQuad;

        m3::ParticleEmitter p = RestParticle();

        // Row 27: a Warcraft III particle leaves its emitter unless the node
        // says model space.
        if (!modelSpace) {
            p.additionalFlags |= m3::ParticleAdditionalFlag::WorldSpace;
        }

        // Rows 1-2: speed and its variation are both endpoints of one uniform
        // draw, s(1-v) to s(1+v), in either engine.
        const f32 s = pe.speed;
        const f32 v = pe.variation;
        Rest(p.initialSpeed, s * (1.0f - v));
        Rest(p.initialSpeedRandom, s * (1.0f + v));
        if (v > 0.0f || variationKeys.keyed()) {
            p.additionalFlags |= m3::ParticleAdditionalFlag::EmitSpeedRandomize;
        }
        if (speedKeys.keyed() || variationKeys.keyed()) {
            bind(MergeKeys<f32>(speedKeys, s, variationKeys, v,
                                [](f32 speed, f32 var) { return speed * (1.0f - var); }),
                 {&p.initialSpeed});
            bind(MergeKeys<f32>(speedKeys, s, variationKeys, v,
                                [](f32 speed, f32 var) { return speed * (1.0f + var); }),
                 {&p.initialSpeedRandom});
        }

        // Rows 3-4: the cone. Warcraft III's latitude is a polar half-angle
        // over a full circle of azimuth; StarCraft II's cone tilts two
        // independent half-ranges, so the square spread whose RMS polar angle
        // matches is lat/sqrt(2) a side (E2). A line emitter's zero azimuth is
        // exactly one side at the full latitude and the other at none (§A13).
        // Past a third of a turn the square spread folds over itself and the
        // uniform sphere is nearer Warcraft III's wide cone (E2); a line
        // emitter's fan stays exact at any width.
        constexpr f32 kSphereLatitude = 120.0f;
        const bool sphere = !line && pe.latitude > kSphereLatitude;
        p.velocityType = sphere ? 3u : 0u;
        if (!line && latitudeKeys.keyed() &&
            (MaxOf(latitudeKeys, pe.latitude) > kSphereLatitude) != sphere) {
            report_.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                     "particle emitter '" + name +
                                         "' keys its latitude across a third of a turn; the "
                                         "spread's shape is chosen once, from its rest",
                                     where);
        }
        const f32 spread = line ? 1.0f : 1.0f / std::sqrt(2.0f);
        const auto horizontalOf = [spread](f32 degrees) {
            return (std::min)(degrees * kDegToRad * spread, kPi);
        };
        Rest(p.initialHorizontal, horizontalOf(pe.latitude));
        Rest(p.initialVertical, line ? 0.0f : horizontalOf(pe.latitude));
        if (latitudeKeys.keyed()) {
            Keyed cone = MapKeys<f32>(latitudeKeys, horizontalOf);
            if (line) {
                bind(std::move(cone), {&p.initialHorizontal});
            } else {
                bind(std::move(cone), {&p.initialHorizontal, &p.initialVertical});
            }
        }

        // Rows 5-6: Warcraft III pulls down by `gravity`; StarCraft II adds its
        // gravity along +Z, both closed forms `1/2 a t^2`, and the scene scale
        // the runtime multiplies in is 1 in a game (R1).
        p.gravity = -pe.gravity;
        if (gravityKeys.keyed()) {
            report_.diagnostics.warn(DiagCode::AnimTrackDropped,
                                     "particle emitter '" + name +
                                         "' keys its gravity; PAR_ gravity is not animated and "
                                         "rests at the record's value",
                                     where);
        }

        // Row 7.
        Rest(p.lifetime, pe.lifespan);
        Rest(p.lifetimeRandom, pe.lifespan);

        // Row 16's other half: Warcraft III faces an XY quad to the camera while
        // its velocity's XY is too short to turn it (`CameraFacingShare`). A
        // continuous head-only emitter draws a share that is neither none nor
        // all as a second, billboard record on its bone, at that share of the
        // rate; a squirt or a tail keeps its larger half.
        constexpr f32 kFacingEdge = 0.02f;
        const f32 facing = xyQuad ? CameraFacingShare(pe, options_.lengthScale) : 0.0f;
        const bool whole = squirt || pe.headOrTail != wem::Wc3ParticleHeadOrTail::Head;
        const bool split =
            xyQuad && !whole && facing > kFacingEdge && facing < 1.0f - kFacingEdge;
        const bool allFacing =
            xyQuad && !split && facing >= (whole ? 0.5f : 1.0f - kFacingEdge);
        const f32 planarShare = split ? 1.0f - facing : 1.0f;

        // Rows 8-10: continuous emission, or bursts.
        f32 maxRate = 0.0f;
        u32 maxSquirt = 0;
        const auto restRate = [&](m3::ParticleEmitter& record, f32 share) {
            RestEmission(record.emissionRate, (std::max)(pe.emissionRate, 0.0f) * share);
            if (rateKeys.keyed()) {
                bind(MapKeys<f32>(rateKeys, [share](f32 r) { return (std::max)(r, 0.0f) * share; }),
                     {&record.emissionRate});
            }
        };
        if (squirt) {
            RestEmission(p.emissionRate, 0.0f);
            if (rateKeys.keyed()) {
                bind(Squirt(rateKeys, document_, maxSquirt), {&p.squirtAmount});
            } else if (pe.emissionRate > kSquirtEdge) {
                report_.diagnostics.warn(DiagCode::AnimTrackDropped,
                                         "particle emitter '" + name +
                                             "' squirts from a rate no track steps up to; "
                                             "StarCraft II has no key to burst on",
                                         where);
            }
        } else {
            restRate(p, planarShare);
            maxRate = (std::max)(pe.emissionRate, 0.0f);
            if (rateKeys.keyed()) {
                maxRate = (std::max)(maxRate, MaxOf(rateKeys, 0.0f));
            }
        }
        // The Warcraft III pool grows to 1.15x its steady state; a cap the
        // alive curve can touch would thin the effect.
        const auto capFor = [&](f32 share) {
            const f32 steady = maxRate * share * (std::max)(pe.lifespan, 0.0f) * 1.15f;
            const u64 wanted = static_cast<u64>(std::ceil(steady)) + maxSquirt + 16u;
            return static_cast<u32>((std::min)(wanted, static_cast<u64>(kMaxParticles)));
        };
        p.maxParticles = capFor(planarShare);

        // Row 11: the spawn plane, width across and length along.
        p.emitterShape = m3::EmitterShape::Plane;
        Rest(p.shapeOuter, Vector3f{pe.width, pe.length, 0.0f});
        if (widthKeys.keyed() || lengthKeys.keyed()) {
            bind(MergeKeys<Vector3f>(widthKeys, pe.width, lengthKeys, pe.length,
                                     [](f32 w, f32 l) { return Vector3f{w, l, 0.0f}; }),
                 {&p.shapeOuter});
        }

        // Row 12: Warcraft III's scale is a half extent and StarCraft II halves
        // its key, so the key is the full width.
        Rest(p.sizeAnimation, Vector3f{2.0f * pe.start.scaling, 2.0f * pe.middle.scaling,
                                       2.0f * pe.end.scaling});
        p.sizeMidTime = (std::min)(pe.time, kMidTimeRest);

        // Row 13.
        Rest(p.colorStart, Color(pe.start.color, pe.start.alpha));
        Rest(p.colorMid, Color(pe.middle.color, pe.middle.alpha));
        Rest(p.colorEnd, Color(pe.end.color, pe.end.alpha));
        p.colorMidTime = (std::min)(pe.time, kMidTimeRest);
        p.alphaMidTime = p.colorMidTime;

        // Rows 23-26: light, fog, sorting, priority.
        if (!pe.unshaded) {
            p.flags |= m3::ParticleFlag::LitParts;
        }
        if (pe.sortPrimsFarZ) {
            p.flags |= m3::ParticleFlag::Sort;
            // Sorting demotes the emitter to the Euler step, which reads mass
            // and drag where the closed form does not.
            p.mass = 1.0f;
            p.drag = 0.0f;
        }

        // Rows 20-22: the material. Warcraft III binds a replaceable in the
        // texture's place; the team glow (2, 13 shipped emitters) crosses as
        // the team colour through the glow's mask -- or the emitter's own
        // texture when the export has none -- and a team colour (1, none
        // shipped) does not cross.
        MaterialKey key;
        const std::string glowMask = texturePath(options_.teamGlowMask);
        key.teamGlow = pe.replaceableId == 2;
        key.glowMask = key.teamGlow && !glowMask.empty();
        key.texture = key.glowMask ? glowMask : texturePath(pe.texture);
        if (key.teamGlow && !key.glowMask) {
            report_.diagnostics.info(DiagCode::LossyKindConversion,
                                     "particle emitter '" + name +
                                         "' draws the team glow, which the export has no mask "
                                         "for; its own texture's alpha shapes the team colour",
                                     where);
        } else if (pe.replaceableId != 0 && !key.teamGlow) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "particle emitter '" + name + "' draws replaceable " +
                                         std::to_string(pe.replaceableId) +
                                         ", which does not cross; it draws " +
                                         textureName(pe.texture) + " instead",
                                     where);
        }
        key.wrap = textureWrap(pe.texture);
        key.filter = ParticleFilter(pe.filter);
        key.unshaded = pe.unshaded;
        key.unfogged = pe.unfogged;
        key.priority = pe.priorityPlane + 1;
        key.particle = true;
        if (key.texture.empty()) {
            report_.diagnostics.warn(DiagCode::TextureUnresolved,
                                     "particle emitter '" + name + "' names " +
                                         textureName(pe.texture) +
                                         ", which has no path; it draws untextured",
                                     where);
        }
        p.materialIndex = materials_.slot(key);

        // Row 18's losses, said once per emitter.
        if (pe.headLife.repeat > 1 || pe.headDecay.repeat > 1 || pe.tailLife.repeat > 1 ||
            pe.tailDecay.repeat > 1) {
            report_.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                     "particle emitter '" + name +
                                         "' repeats a flipbook run; StarCraft II plays each run "
                                         "once per life",
                                     where);
        }

        // Row 14: head, tail, or one record for each.
        const bool head = pe.headOrTail == wem::Wc3ParticleHeadOrTail::Head ||
                          pe.headOrTail == wem::Wc3ParticleHeadOrTail::Both;
        const bool tail = pe.headOrTail == wem::Wc3ParticleHeadOrTail::Tail ||
                          pe.headOrTail == wem::Wc3ParticleHeadOrTail::Both;
        std::vector<std::pair<const char*, m3::ParticleEmitter>> records;
        if (head) {
            m3::ParticleEmitter h = p;
            Flipbook(h, pe, pe.headLife, pe.headDecay);
            if (xyQuad && !allFacing && modelSpace) {
                // Row 16, model space: Warcraft III turns the quad by the
                // velocity's XY in the particle's own frame, which here is the
                // node's (`particle_geometry.cpp`), so a turned node stands the
                // quads up; the ground plane laid ChineseFairy's crown rays
                // flat. The emitter-oriented quad lies in the bone's XY on the
                // local arm, and a random spin stands in for the cone's uniform
                // azimuth, which a line emitter does not have.
                h.instanceType = m3::ParticleInstanceType::EmitterOriented;
                if (!line) {
                    h.rotationRandomEnable = 1;
                    Rest(h.rotationRandomAnimation, Vector3f{2.0f * kPi, 0.0f, 0.0f});
                }
            } else if (xyQuad && !allFacing) {
                // Row 16: a quad in the ground plane, turned to its velocity.
                h.instanceType = m3::ParticleInstanceType::TerrainDirOriented;
                h.flags |= m3::ParticleFlag::FixTailLengthOnCreation;
                h.tailLength = 1.0f;
                h.mass = 1.0f;
                h.drag = 0.0f;
            } else {
                h.instanceType = m3::ParticleInstanceType::Billboard;
            }
            records.emplace_back("", std::move(h));
            if (split) {
                // The share that faces the camera, as the billboard it is.
                m3::ParticleEmitter b = p;
                Flipbook(b, pe, pe.headLife, pe.headDecay);
                b.instanceType = m3::ParticleInstanceType::Billboard;
                restRate(b, facing);
                b.maxParticles = capFor(facing);
                records.emplace_back("_Facing", std::move(b));
                report_.diagnostics.info(DiagCode::LossyKindConversion,
                                         "particle emitter '" + name +
                                             "' turns only part of its XY quads by their "
                                             "velocity; the rest face the camera as a second, "
                                             "billboard emitter",
                                         where);
            }
        }
        if (tail) {
            m3::ParticleEmitter t = p;
            Flipbook(t, pe, pe.tailLife, pe.tailDecay);
            t.instanceType = m3::ParticleInstanceType::Trail;
            // Row 15: Warcraft III's tail spans `v * T`. StarCraft II's speed
            // branch lays `2 * halfSize * |v| * tail` of length in its own
            // units, where the half size is half the key and the speed is
            // already scaled -- so the tail is the time over the key.
            const f32 sizeMid = 2.0f * pe.middle.scaling;
            t.tailLength = sizeMid > 0.0f ? pe.tailLength / sizeMid : 1.0f;
            records.emplace_back("_Tail", std::move(t));
            if (head) {
                report_.diagnostics.info(DiagCode::AnimTrackApproximated,
                                         "particle emitter '" + name +
                                             "' draws a head and a tail; StarCraft II draws them "
                                             "as two emitters whose particles are not the same",
                                         where);
            }
        }
        if (records.empty()) {
            report_.diagnostics.info(DiagCode::FeatureDropped,
                                     "particle emitter '" + name +
                                         "' draws neither a head nor a tail",
                                     where);
            retire(n);
            return;
        }
        dropSystemChannels(n);
        for (std::size_t r = 0; r < records.size(); ++r) {
            const u32 node = r == 0 ? n : addRecordNode(n, records[r].first);
            placeParticle(node, records[r].second);
        }
    }

    // --- PREM (C8.1) --------------------------------------------------------

    /// Warcraft III's model-spawning emitter: a cone of spawned models, each
    /// stamped into the world at birth unturned and at scale 1, falling under
    /// its gravity for its lifespan (`ChildModelEmitter`). The cone is PRE2's
    /// with the latitude in radians and the azimuth spread `longitude` around
    /// the node's +X, which is a full circle at pi -- every shipped emitter but
    /// five.
    void modelParticle(u32 n) {
        const wem::Wc3ParticleEmitter1Payload pe =
            std::get<wem::Wc3ParticleEmitter1Payload>(model_.nodes.nodes[n].payload);
        const std::string name = model_.nodes.nodes[n].name;
        const ElementRef where(ElementKind::Node, n);
        const auto spawned = options_.spawnedModels.find(pe.spawnModel.path);
        if (spawned == options_.spawnedModels.end() || spawned->second.empty()) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "model-spawning emitter '" + name + "' spawns '" +
                                         pe.spawnModel.path +
                                         "', which the export did not write; only its "
                                         "placement was",
                                     where);
            retire(n);
            return;
        }
        using P = wem::Wc3Particle1Property;
        const Cuts speedKeys = cutsOf(n, P::Speed);
        const Cuts latitudeKeys = cutsOf(n, P::Latitude);
        const Cuts longitudeKeys = cutsOf(n, P::Longitude);
        const Cuts gravityKeys = cutsOf(n, P::Gravity);
        const Cuts lifespanKeys = cutsOf(n, P::Lifespan);
        const Cuts rateKeys = cutsOf(n, P::EmissionRate);

        m3::ParticleEmitter p = RestParticle();
        p.flags |= m3::ParticleFlag::ModelParticles;
        p.additionalFlags |= m3::ParticleAdditionalFlag::WorldSpace;
        p.modelPaths = {spawned->second};
        // A model facing a fixed world direction of -Y is turned no way at all
        // (`FacingBasis`), and a model particle's scale is its size key.
        p.instanceType = m3::ParticleInstanceType::FaceWorldDir;
        p.instanceAngle = Vector3f{0.0f, -1.0f, 0.0f};
        p.emitterShape = m3::EmitterShape::Point;
        Rest(p.sizeAnimation, Vector3f{1.0f, 1.0f, 1.0f});
        const m3::ColorBGRA white = Color(Vector3f{1.0f, 1.0f, 1.0f}, 255);
        Rest(p.colorStart, white);
        Rest(p.colorMid, white);
        Rest(p.colorEnd, white);

        // One speed, no variation.
        Rest(p.initialSpeed, pe.speed);
        Rest(p.initialSpeedRandom, pe.speed);
        if (speedKeys.keyed()) {
            bind(MapKeys<f32>(speedKeys, [](f32 speed) { return speed; }),
                 {&p.initialSpeed, &p.initialSpeedRandom});
        }

        // The cone, as PRE2's (rows 3 and E2): a square spread of lat/sqrt(2)
        // a side, or the sphere past a third of a turn.
        constexpr f32 kSphereLatitude = 120.0f * kDegToRad;
        const bool sphere = pe.latitude > kSphereLatitude;
        p.velocityType = sphere ? 3u : 0u;
        const auto spreadOf = [](f32 radians) {
            return (std::min)(radians / std::sqrt(2.0f), kPi);
        };
        Rest(p.initialHorizontal, spreadOf(pe.latitude));
        Rest(p.initialVertical, spreadOf(pe.latitude));
        if (latitudeKeys.keyed()) {
            bind(MapKeys<f32>(latitudeKeys, spreadOf), {&p.initialHorizontal, &p.initialVertical});
        }
        if (pe.longitude < kPi - 1e-3f || longitudeKeys.keyed()) {
            report_.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                     "model-spawning emitter '" + name +
                                         "' spreads over part of a circle of azimuth; StarCraft "
                                         "II's cone spreads over all of it",
                                     where);
        }

        // Row 5 (R1), as PRE2's.
        p.gravity = -pe.gravity;
        if (gravityKeys.keyed()) {
            report_.diagnostics.warn(DiagCode::AnimTrackDropped,
                                     "model-spawning emitter '" + name +
                                         "' keys its gravity; PAR_ gravity is not animated and "
                                         "rests at the record's value",
                                     where);
        }

        Rest(p.lifetime, pe.lifespan);
        Rest(p.lifetimeRandom, pe.lifespan);
        if (lifespanKeys.keyed()) {
            bind(CopyKeys(lifespanKeys), {&p.lifetime, &p.lifetimeRandom});
        }

        RestEmission(p.emissionRate, (std::max)(pe.emissionRate, 0.0f));
        f32 maxRate = (std::max)(pe.emissionRate, 0.0f);
        if (rateKeys.keyed()) {
            bind(MapKeys<f32>(rateKeys, [](f32 r) { return (std::max)(r, 0.0f); }),
                 {&p.emissionRate});
            maxRate = (std::max)(maxRate, MaxOf(rateKeys, 0.0f));
        }
        const f32 maxLife = (std::max)((std::max)(pe.lifespan, 0.0f), MaxOf(lifespanKeys, 0.0f));
        const u64 wanted = static_cast<u64>(std::ceil(maxRate * maxLife * 1.15f)) + 16u;
        p.maxParticles = static_cast<u32>((std::min)(wanted, static_cast<u64>(kMaxParticles)));

        // The record names a material; the models draw their own.
        MaterialKey key;
        key.particle = true;
        p.materialIndex = materials_.slot(key);

        dropSystemChannels(n);
        placeParticle(n, std::move(p));
        ++report_.modelParticleRecords;
    }

    // --- ATCH with a model --------------------------------------------------

    /// A model riding an attachment point. Warcraft III starts it from its
    /// first frame each time the point shows and hides it with the point
    /// (`ApplyAttachmentStates`); a StarCraft II point carries no model. So a
    /// model particle on the point bursts one copy where each visible span of
    /// it starts, living that span: the game plays a model particle's first
    /// sequence from its birth (4.8 `ProcessPendingSpawns` -> the instance's
    /// bind, `sub_1028DD860`). Emitter-oriented in the emitter's space, the
    /// copy moves and turns with the point.
    void attachedModel(u32 n) {
        const auto* payload = std::get_if<wem::AttachmentPayload>(&model_.nodes.nodes[n].payload);
        if (payload == nullptr || payload->asset.path.empty()) {
            return;
        }
        const std::string path = payload->asset.path;
        const std::string name = model_.nodes.nodes[n].name;
        const ElementRef where(ElementKind::Node, n);
        const auto written = options_.attachedModels.find(path);
        if (written == options_.attachedModels.end() || written->second.empty()) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "attachment '" + name + "' carries '" + path +
                                         "', which the export did not write; it is not drawn",
                                     where);
            return;
        }
        Keyed bursts;
        Keyed lives;
        visibleSpans(n, bursts, lives);
        if (bursts.empty()) {
            report_.diagnostics.info(DiagCode::FeatureDropped,
                                     "attachment '" + name + "' carries '" + path +
                                         "' but shows in no clip",
                                     where);
            return;
        }

        m3::ParticleEmitter p = RestParticle();
        p.flags |= m3::ParticleFlag::ModelParticles;
        p.modelPaths = {written->second};
        p.instanceType = m3::ParticleInstanceType::EmitterOriented;
        p.emitterShape = m3::EmitterShape::Point;
        Rest(p.sizeAnimation, Vector3f{1.0f, 1.0f, 1.0f});
        const m3::ColorBGRA white = Color(Vector3f{1.0f, 1.0f, 1.0f}, 255);
        Rest(p.colorStart, white);
        Rest(p.colorMid, white);
        Rest(p.colorEnd, white);
        RestEmission(p.emissionRate, 0.0f);
        bind(std::move(bursts), {&p.squirtAmount});
        Rest(p.lifetime, 1.0f);
        Rest(p.lifetimeRandom, 1.0f);
        bind(std::move(lives), {&p.lifetime, &p.lifetimeRandom});
        // One copy a span; a loop's next copy is born the frame the last dies.
        p.maxParticles = 4;
        MaterialKey key;
        key.particle = true;
        p.materialIndex = materials_.slot(key);

        // The pose takes the model's axes from the emitter's rows as (x, -z,
        // y), a quarter turn the record bone's +90 degrees about its x undoes:
        // WEM's y, the bases a quarter turn about z apart (`m3_converter`).
        const u32 record = addRecordNode(n, "_Model", false);
        Node& node = model_.nodes.nodes[record];
        const f32 h = std::sqrt(0.5f);
        node.local.rotation = Quaternion{0.0f, h, 0.0f, h};
        node.poses.assign(model_.nodes.poseSchema.size(), node.local);
        {
            // A row vector's quarter turn about y, and its inverse.
            Matrix44f turn = Matrix44f::identity();
            turn.data[0][0] = 0.0f;
            turn.data[0][2] = -1.0f;
            turn.data[2][0] = 1.0f;
            turn.data[2][2] = 0.0f;
            Matrix44f back = turn;
            back.data[0][2] = 1.0f;
            back.data[2][0] = -1.0f;
            bindUnder(node, turn, back);
        }
        placeParticle(record, std::move(p));
        ++report_.attachedModelRecords;
    }

    /// Per clip, a burst of one where each span @p node is visible starts
    /// (`wem::ShownSpans`: Warcraft III's test, above 0, a Death shown into its
    /// Decay one copy) and the life of what is born there.
    void visibleSpans(u32 node, Keyed& bursts, Keyed& lives) const {
        std::map<u32, std::pair<SubTrack, SubTrack>> keyed;
        for (const wem::ShownSpan& span : wem::ShownSpans(document_, 0, node)) {
            if (span.played > 0.0f) {
                continue; // the Death's copy, playing on
            }
            auto& [burst, life] = keyed[span.clip];
            burst.interp = Interpolation::Step;
            life.interp = Interpolation::Step;
            burst.times.push_back(span.start);
            AppendValue(burst.values, u32{1});
            life.times.push_back(span.start);
            AppendValue(life.values, span.life);
        }
        for (auto& [clip, tracks] : keyed) {
            bursts.emplace_back(clip, std::move(tracks.first));
            lives.emplace_back(clip, std::move(tracks.second));
        }
    }

    // --- RIBB ---------------------------------------------------------------

    void ribbon(u32 n) {
        const wem::Wc3RibbonEmitterPayload rb =
            std::get<wem::Wc3RibbonEmitterPayload>(model_.nodes.nodes[n].payload);
        const std::string name = model_.nodes.nodes[n].name;
        const ElementRef where(ElementKind::Node, n);
        using P = wem::Wc3RibbonProperty;
        const Cuts aboveKeys = cutsOf(n, P::HeightAbove);
        const Cuts belowKeys = cutsOf(n, P::HeightBelow);
        const Cuts colorKeys = cutsOf(n, Channel::Color);
        const Cuts alphaKeys = cutsOf(n, Channel::Alpha);
        const Cuts cellKeys = cutsOf(n, Channel::TextureIndex);

        m3::RibbonEmitter r = RestRibbon();

        // Rows 1-3: the width. StarCraft II's full width is half its size key.
        const f32 width = 2.0f * (rb.heightAbove + rb.heightBelow);
        Rest(r.sizeAnimation, Vector3f{width, width, width});
        if (aboveKeys.keyed() || belowKeys.keyed()) {
            bind(MergeKeys<Vector3f>(aboveKeys, rb.heightAbove, belowKeys, rb.heightBelow,
                                     [](f32 above, f32 below) {
                                         const f32 w = 2.0f * (above + below);
                                         return Vector3f{w, w, w};
                                     }),
                 {&r.sizeAnimation});
            // Row 3: the offset helper holds the rest difference; a difference
            // that animates slides the strip where StarCraft II cannot follow.
            const f32 rest = rb.heightAbove - rb.heightBelow;
            const f32 tolerance = 2.0f * kRibbonOffsetEpsilon * options_.lengthScale;
            bool moves = false;
            for (const u32 clip : ClipsOf(aboveKeys, belowKeys)) {
                const SubTrack* above = aboveKeys.in(clip);
                const SubTrack* below = belowKeys.in(clip);
                for (const f32 time : TimesOf(above, below)) {
                    f32 a = rb.heightAbove;
                    f32 b = rb.heightBelow;
                    if (above != nullptr) {
                        Sample(*above, aboveKeys.type, time, &a);
                    }
                    if (below != nullptr) {
                        Sample(*below, belowKeys.type, time, &b);
                    }
                    moves = moves || std::fabs((a - b) - rest) > tolerance;
                }
            }
            if (moves) {
                report_.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                         "ribbon '" + name +
                                             "' keys heights whose difference moves; the strip "
                                             "stays offset by the rest difference",
                                         where);
            }
        }

        // Rows 4-5: Warcraft III floors a lifespan at a quarter second and lays
        // `rate` edges a second; StarCraft II lays `divisions / lifetime`.
        const f32 lifetime = (std::max)(rb.lifespan, 0.25f);
        Rest(r.lifetime, lifetime);
        r.divisions = static_cast<f32>(rb.emissionRate) * lifetime;

        // Rows 6-7: the colour, alpha kept.
        const m3::ColorBGRA color = Color(rb.color, ColorByte(rb.alpha));
        Rest(r.colorStart, color);
        Rest(r.colorMid, color);
        Rest(r.colorEnd, color);
        if (bind(ColorKeys(colorKeys, alphaKeys, rb.color, rb.alpha),
                 {&r.colorStart, &r.colorMid, &r.colorEnd})) {
            report_.diagnostics.info(DiagCode::AnimTrackApproximated,
                                     "ribbon '" + name +
                                         "' keys its colour; StarCraft II tints each segment as "
                                         "it is laid, Warcraft III the whole strip",
                                     where);
        }

        // Row 8: Warcraft III drops an edge `g t^2`; StarCraft II moves one
        // `1/2 G t^2`, raw (R3). A falling strip keeps its width only on
        // accurate tangents: the GPU technique crosses its tangent, which turns
        // down the fall, with up, and the strip lies over unless the bone's Z
        // stands upright (E3).
        r.gravity = -2.0f * rb.gravity;
        if (rb.gravity != 0.0f) {
            r.flags |= m3::RibbonFlag::AccurateGPUTangents;
        }

        // Row 9.
        if (rb.textureSlot != 0 || rb.rows > 1 || rb.columns > 1 || cellKeys.keyed()) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "ribbon '" + name +
                                         "' picks a texture slot out of a sheet; StarCraft II "
                                         "draws its whole texture",
                                     where);
        }

        // Rows 10-11: the material, from the Warcraft III record the slot draws.
        MaterialKey key;
        key.particle = false;
        if (const wem::native::MdxMaterial* material = warcraftMaterial(rb.materialSlot);
            material != nullptr && !material->layers.empty()) {
            const wem::native::MdxLayer& layer = material->layers.front();
            u32 textureId = layer.textureId;
            for (const wem::native::MdxSubTexture& sub : layer.subTextures) {
                if (sub.slot == wem::native::MdxSlotType::DiffuseMap) {
                    textureId = sub.textureId;
                    break;
                }
            }
            key.texture = texturePath(textureId);
            key.wrap = textureWrap(textureId);
            key.filter = LayerFilter(layer.filterMode);
            const u32 shading = static_cast<u32>(layer.shadingFlags);
            using Shading = wem::native::MdxShadingFlag;
            key.unshaded = (shading & static_cast<u32>(Shading::Unshaded)) != 0;
            key.unfogged = (shading & static_cast<u32>(Shading::Unfogged)) != 0;
            key.twoSided = (shading & static_cast<u32>(Shading::TwoSided)) != 0;
            key.priority = material->priorityPlane + 1;
            if (material->layers.size() > 1) {
                report_.diagnostics.warn(DiagCode::LayerDropped,
                                         "ribbon '" + name +
                                             "' draws a material of several layers; its first "
                                             "crosses",
                                         where);
            }
        }
        r.materialIndex = materials_.slot(key);

        // Row 2: an off-centre ribbon's record rides the helper
        // `PrepareWc3Effects` planted across its width, which holds its
        // visibility too; the node keeps its placement. Row 14: either bone
        // unturned -- the strip's width is the bone's X, the node's Y (E3).
        u32 carrier = n;
        for (u32 c = 0; c < model_.nodes.size(); ++c) {
            const Node& child = model_.nodes.nodes[c];
            if (child.parent == n && child.kind == NodeKind::Helper &&
                child.native.find(kRibbonOffset) != nullptr) {
                carrier = c;
                break;
            }
        }
        if (carrier != n) {
            retire(n);
        } else {
            dropSystemChannels(n);
        }
        keyChannels(carrier, NodeKind::Sc2RibbonEmitter, r,
                    [](auto& record, auto&& f) { wem::m3_emitters::ForEachRibbonRef(record, f); });
        Node& node = model_.nodes.nodes[carrier];
        node.kind = NodeKind::Sc2RibbonEmitter;
        node.payload = wem::m3_emitters::ImportRibbon(r, wem::m3_emitters::ImportLinks{});
        ++report_.ribbonRecords;
    }

    // --- the document -------------------------------------------------------

    template <class Property>
    Cuts cutsOf(u32 node, Property property) const {
        return cutsOf(node, Channel::EmitterProperty,
                      wem::EmitterPropertySub(static_cast<u32>(property)));
    }

    Cuts cutsOf(u32 node, Channel channel, u32 sub = 0) const {
        Cuts cuts;
        for (const AnimChannel& entry : model_.animChannels.channels) {
            if (entry.target.kind != TrackTarget::Kind::Node || entry.target.node != node ||
                entry.target.channel != channel || entry.target.sub != sub) {
                continue;
            }
            cuts.type = entry.valueType;
            for (u32 c = 0; c < document_.clips.size(); ++c) {
                const wem::Clip& clip = document_.clips[c];
                if (clip.model != 0) {
                    continue;
                }
                for (const wem::SubTrackContainer& container : clip.containers) {
                    const SubTrack* track = container.find(entry.id);
                    if (track != nullptr && !track->times.empty() &&
                        track->wellSized(entry.valueType)) {
                        cuts.clips.emplace_back(c, track);
                        break;
                    }
                }
            }
            break;
        }
        return cuts;
    }

    std::string texturePath(u32 texture) const {
        return texture < document_.textures.size() ? document_.textures[texture].path
                                                   : std::string();
    }

    std::string textureName(u32 texture) const {
        return texture == kInvalidIndex ? std::string("no texture")
                                        : "texture " + std::to_string(texture);
    }

    /// @p texture's `TEXS` address word (0x1 wrap U, 0x2 wrap V), which the
    /// import keeps as the texture's flags.
    u32 textureWrap(u32 texture) const {
        return texture < document_.textures.size() ? document_.textures[texture].flags & 0x3u
                                                   : 0u;
    }

    /// The Warcraft III record @p slot draws, as the document's own profile
    /// states it; null when it carries none.
    const wem::native::MdxMaterial* warcraftMaterial(u32 slot) const {
        const wem::ProfileMaterialSet* set = model_.setFor(document_.assetSource());
        const wem::Material* material =
            set != nullptr ? wem::Resolve(model_, slot, document_.assetSource(), set->defaultLook)
                           : nullptr;
        if (material == nullptr || material->nativeKind() != wem::NativeKind::Mdx) {
            return nullptr;
        }
        return &std::get<wem::native::MdxMaterial>(material->Native());
    }

    /// Binds every AnimRef in @p refs to @p keyed: each record carrying one of
    /// them keys that property with these sub-tracks once it is placed. The id
    /// is the crossing's own, never written. False when nothing is keyed.
    template <class Ref>
    bool bind(Keyed keyed, std::initializer_list<m3::AnimRef<Ref>*> refs) {
        if (keyed.empty()) {
            return false;
        }
        pool_.push_back(std::move(keyed));
        const u32 id = static_cast<u32>(pool_.size());
        for (m3::AnimRef<Ref>* ref : refs) {
            ref->animId = id;
        }
        return true;
    }

    /// Every bound AnimRef of @p record as an `EmitterProperty` channel of
    /// @p node, which is becoming @p kind, keyed in the clips it was bound
    /// with; the record keeps no id (the export names its own).
    template <class Record, class Visit>
    void keyChannels(u32 node, NodeKind kind, Record& record, Visit visit) {
        visit(record, [&](u32 sub, auto& ref) {
            const u32 id = ref.animId;
            ref.animId = 0;
            const wem::EmitterPropertyDesc* desc = wem::FindEmitterProperty(kind, sub);
            if (id == 0 || id > pool_.size() || desc == nullptr) {
                return;
            }
            AnimChannel channel;
            channel.id = nextId_++;
            channel.target.kind = TrackTarget::Kind::Node;
            channel.target.node = node;
            channel.target.channel = Channel::EmitterProperty;
            channel.target.sub = sub;
            channel.valueType = desc->type;
            channel.initValue = RestBytes(ref.initValue);
            model_.animChannels.add(channel);
            for (const auto& [clip, track] : pool_[id - 1]) {
                wem::Clip& target = document_.clips[clip];
                if (target.containers.empty()) {
                    target.containers.emplace_back();
                }
                SubTrack copy = track;
                copy.channel = channel.id;
                target.containers.front().subTracks.push_back(std::move(copy));
            }
            ++report_.keyedChannels;
        });
    }

    void placeParticle(u32 node, m3::ParticleEmitter record) {
        // The game uploads a record's cells only when both counts are set, and
        // otherwise its flipbook layers read the cells some other emitter left
        // in the batch slot: a texture with no sheet is one whole cell.
        if (record.flipbookColumns == 0 || record.flipbookRows == 0) {
            record.flipbookColumns = 1;
            record.flipbookRows = 1;
            record.flipbookColumnFraction = 1.0f;
            record.flipbookRowFraction = 1.0f;
            record.flipbookStartInitIndex = record.flipbookStartStopIndex = 0;
            record.flipbookEndInitIndex = record.flipbookEndStopIndex = 0;
            record.flipbookMidTime = kMidTimeRest;
        }
        keyChannels(node, NodeKind::Sc2ParticleEmitter, record,
                    [](auto& r, auto&& f) { wem::m3_emitters::ForEachParticleRef(r, f); });
        Node& target = model_.nodes.nodes[node];
        target.kind = NodeKind::Sc2ParticleEmitter;
        target.payload = wem::m3_emitters::ImportParticle(record, wem::m3_emitters::ImportLinks{});
        ++report_.particleRecords;
    }

    /// A second record of @p parent's system: a child at identity that draws
    /// only while its parent does, so it rides the parent's bone; with
    /// @p shares false, one with a visibility of its own.
    u32 addRecordNode(u32 parent, const char* suffix, bool shares = true) {
        const Node& source = model_.nodes.nodes[parent];
        Node node;
        node.name = source.name + suffix;
        node.kind = NodeKind::Helper;
        node.resetPayloadForKind();
        node.parent = parent;
        node.pivot = source.pivot;
        node.poses.assign(model_.nodes.poseSchema.size(), node.local);
        // An identity child binds where its parent does.
        node.poseMatrices = source.poseMatrices;
        if (shares) {
            node.native.set(wem::kNodeSharesParentVisibility, static_cast<i64>(1));
        }
        return model_.nodes.add(std::move(node));
    }

    /// The frame a `CORN` effect is drawn in: an identity child of @p parent
    /// turned +90 degrees about z (`mdx_model_adapter.cpp` puts the quarter
    /// turn in front of the node's world matrix).
    u32 addFrameNode(u32 parent) {
        const u32 child = addRecordNode(parent, "_corn");
        Node& node = model_.nodes.nodes[child];
        const f32 h = std::sqrt(0.5f);
        node.local.rotation = Quaternion{0.0f, 0.0f, h, h};
        node.poses.assign(model_.nodes.poseSchema.size(), node.local);
        {
            // A row vector's quarter turn, and its inverse.
            Matrix44f turn = Matrix44f::identity();
            turn.data[0][0] = 0.0f;
            turn.data[0][1] = 1.0f;
            turn.data[1][0] = -1.0f;
            turn.data[1][1] = 0.0f;
            Matrix44f back = turn;
            back.data[0][1] = -1.0f;
            back.data[1][0] = 1.0f;
            bindUnder(node, turn, back);
        }
        return child;
    }

    /// Per clip, the rotation that undoes the turn of @p host and its
    /// ancestors in a child of its `CORN` frame: `q_T⁻¹ q_F(t)⁻¹ q_R q_T`, q_F
    /// the host's model-space rotation, q_R the same at rest and q_T the
    /// frame's quarter turn, keyed where the chain keys a rotation. A clip
    /// that turns nothing keys nothing: the child's rest is the identity.
    const Keyed& worldTurns(u32 host) {
        if (const auto it = worldTurns_.find(host); it != worldTurns_.end()) {
            return it->second;
        }
        std::vector<bool> chain(model_.nodes.size(), false);
        for (u32 n = host; n < model_.nodes.size(); n = model_.nodes.nodes[n].parent) {
            chain[n] = true;
        }
        std::vector<u32> rotations;
        for (const AnimChannel& channel : model_.animChannels.channels) {
            if (channel.target.kind == TrackTarget::Kind::Node &&
                channel.target.channel == Channel::Rotation && channel.target.node < chain.size() &&
                chain[channel.target.node]) {
                rotations.push_back(channel.id);
            }
        }
        const f32 h = std::sqrt(0.5f);
        const Quaternion turn{0.0f, 0.0f, h, h};
        Quaternion rest{0.0f, 0.0f, 0.0f, 1.0f};
        if (!rotations.empty()) {
            const wem::Animator animator(document_, 0);
            wem::Mix still;
            still.globals = false;
            wem::Pose pose;
            animator.evaluate(still, pose);
            if (host < pose.frame.size()) {
                rest = wem::FromMatrix(pose.frame[host]).rotation;
            }
        }
        Keyed keyed;
        for (u32 c = 0; c < document_.clips.size() && !rotations.empty(); ++c) {
            if (document_.clips[c].model != 0) {
                continue;
            }
            std::vector<f32> times;
            for (const wem::SubTrackContainer& container : document_.clips[c].containers) {
                for (const u32 id : rotations) {
                    if (const SubTrack* track = container.find(id)) {
                        times.insert(times.end(), track->times.begin(), track->times.end());
                    }
                }
            }
            if (times.empty()) {
                continue;
            }
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());
            const wem::ClipPose pose(document_, 0, c);
            SubTrack track;
            track.interp = Interpolation::Linear;
            track.times = times;
            Quaternion last{0.0f, 0.0f, 0.0f, 1.0f};
            for (const f32 t : times) {
                const Quaternion q = wem::FromMatrix(pose.frame(host, t)).rotation;
                Quaternion undo = turn.conjugate() * (q.conjugate() * (rest * turn));
                // One hemisphere throughout, so a key lerps the short way.
                if (undo.x * last.x + undo.y * last.y + undo.z * last.z + undo.w * last.w < 0.0f) {
                    undo = Quaternion{-undo.x, -undo.y, -undo.z, -undo.w};
                }
                last = undo;
                const f32 v[4] = {undo.x, undo.y, undo.z, undo.w};
                const auto* raw = reinterpret_cast<const u8*>(v);
                track.values.insert(track.values.end(), raw, raw + sizeof v);
            }
            keyed.emplace_back(c, std::move(track));
        }
        return worldTurns_.emplace(host, std::move(keyed)).first->second;
    }

    /// A child of @p parent whose rotation is keyed with @p turns
    /// (`worldTurns`), so what hangs on it keeps the axes it has while the
    /// host does not turn and follows the host's place only.
    u32 addWorldNode(u32 parent, const std::string& suffix, const Keyed& turns) {
        const u32 child = addRecordNode(parent, suffix.c_str());
        if (turns.empty()) {
            return child;
        }
        AnimChannel channel;
        channel.id = nextId_++;
        channel.target.kind = TrackTarget::Kind::Node;
        channel.target.node = child;
        channel.target.channel = Channel::Rotation;
        channel.valueType = geom::AttrType::Quat;
        model_.animChannels.add(channel);
        for (const auto& [clip, track] : turns) {
            wem::Clip& target = document_.clips[clip];
            if (target.containers.empty()) {
                target.containers.emplace_back();
            }
            SubTrack copy = track;
            copy.channel = channel.id;
            target.containers.front().subTracks.push_back(std::move(copy));
        }
        return child;
    }

    /// @p record's cone axis turned back by each of @p turns' keys: the
    /// direction a birth drawn in the host's frame leaves in, in the world
    /// child's axes, as the yaw and pitch `SampleSpawnVelocity` reads
    /// (axis (sin b, −sin a cos b, cos a cos b), in StarCraft II's basis).
    /// The spread keeps its roll.
    void keyCone(m3::ParticleEmitter& record, const Keyed& turns) {
        if (turns.empty()) {
            return;
        }
        constexpr f32 kDeg = 3.14159265358979f / 180.0f;
        const f32 a0 = record.initialYaw.initValue * kDeg;
        const f32 b0 = record.initialPitch.initValue * kDeg;
        const Vector3f d0{std::sin(b0), -std::sin(a0) * std::cos(b0), std::cos(a0) * std::cos(b0)};
        Keyed yaws, pitches;
        for (const auto& [clip, track] : turns) {
            SubTrack yaw, pitch;
            yaw.interp = pitch.interp = Interpolation::Linear;
            yaw.times = pitch.times = track.times;
            f32 lastA = record.initialYaw.initValue, lastB = record.initialPitch.initValue;
            const f32* q = reinterpret_cast<const f32*>(track.values.data());
            for (std::size_t k = 0; k < track.times.size(); ++k, q += 4) {
                // The angles are the record's, in StarCraft II's basis: the key
                // in it (as the export rebases a bone's), inverted by the
                // column-vector matrix's transpose.
                const Matrix44f m = Matrix44f::rotation(Quaternion{q[1], -q[0], q[2], q[3]});
                const auto column = [&](int i) {
                    return m.data[0][i] * d0.x + m.data[1][i] * d0.y + m.data[2][i] * d0.z;
                };
                const Vector3f d{column(0), column(1), column(2)};
                // Two angle pairs draw each axis; the one nearer the last key,
                // each angle unwrapped toward it.
                const f32 b1 = std::asin(std::clamp(d.x, -1.0f, 1.0f)) / kDeg;
                const f32 a1 = std::atan2(-d.y, d.z) / kDeg;
                const auto toward = [](f32 x, f32 to) {
                    return x + 360.0f * std::round((to - x) / 360.0f);
                };
                f32 a = toward(a1, lastA), b = toward(b1, lastB);
                const f32 a2 = toward(a1 + 180.0f, lastA), b2 = toward(180.0f - b1, lastB);
                if (std::abs(a2 - lastA) + std::abs(b2 - lastB) <
                    std::abs(a - lastA) + std::abs(b - lastB)) {
                    a = a2;
                    b = b2;
                }
                if (std::abs(std::cos(b * kDeg)) < 1e-4f) {
                    a = lastA; // along x: any yaw draws it
                }
                lastA = a;
                lastB = b;
                const auto* ra = reinterpret_cast<const u8*>(&a);
                const auto* rb = reinterpret_cast<const u8*>(&b);
                yaw.values.insert(yaw.values.end(), ra, ra + sizeof a);
                pitch.values.insert(pitch.values.end(), rb, rb + sizeof b);
            }
            yaws.emplace_back(clip, std::move(yaw));
            pitches.emplace_back(clip, std::move(pitch));
        }
        bind(std::move(yaws), {&record.initialYaw});
        bind(std::move(pitches), {&record.initialPitch});
    }

    /// Moves an identity child by @p offset in its parent's frame.
    void moveNode(u32 child, const Vector3f& offset) {
        Node& node = model_.nodes.nodes[child];
        node.local.translation = offset;
        node.poses.assign(model_.nodes.poseSchema.size(), node.local);
        const f32 at[3] = {offset.x, offset.y, offset.z};
        Matrix44f move = Matrix44f::identity();
        Matrix44f back = Matrix44f::identity();
        for (int k = 0; k < 3; ++k) {
            move.data[3][k] = at[k];
            back.data[3][k] = -at[k];
        }
        bindUnder(node, move, back);
    }

    /// Puts @p local, a row-vector matrix, under each of @p node's matrix
    /// binds, which start as its parent's: a bind is `local · parent`, and a
    /// schema that stores the inverse holds `parent⁻¹ · local⁻¹`
    /// (@p inverse is `local⁻¹`).
    void bindUnder(Node& node, const Matrix44f& local, const Matrix44f& inverse) {
        const std::vector<wem::PoseSchema>& schema = model_.nodes.poseSchema;
        for (std::size_t i = 0; i < node.poseMatrices.size(); ++i) {
            Matrix44f& m = node.poseMatrices[i];
            m = i < schema.size() && schema[i].inverse ? m * inverse : local * m;
        }
    }

    /// Drops @p node's Warcraft III system channels -- its properties, and a
    /// ribbon's colour, alpha and cell -- with their keys. Its visibility and
    /// its transform stay: they are the node's.
    void dropSystemChannels(u32 node) {
        std::vector<u32> dropped;
        auto& channels = model_.animChannels.channels;
        for (auto it = channels.begin(); it != channels.end();) {
            const bool own = it->target.kind == TrackTarget::Kind::Node &&
                             it->target.node == node &&
                             (it->target.channel == Channel::EmitterProperty ||
                              it->target.channel == Channel::Color ||
                              it->target.channel == Channel::Alpha ||
                              it->target.channel == Channel::TextureIndex);
            if (own) {
                dropped.push_back(it->id);
                it = channels.erase(it);
            } else {
                ++it;
            }
        }
        if (dropped.empty()) {
            return;
        }
        for (wem::Clip& clip : document_.clips) {
            if (clip.model != 0) {
                continue;
            }
            for (wem::SubTrackContainer& container : clip.containers) {
                auto& tracks = container.subTracks;
                tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                            [&](const SubTrack& t) {
                                                return std::find(dropped.begin(), dropped.end(),
                                                                 t.channel) != dropped.end();
                                            }),
                             tracks.end());
            }
        }
    }

    /// @p node as a helper: the placement of a system nothing draws.
    void retire(u32 node) {
        dropSystemChannels(node);
        Node& target = model_.nodes.nodes[node];
        target.kind = NodeKind::Helper;
        target.resetPayloadForKind();
    }

    Document& document_;
    Model& model_;
    const Wc3EmitterOptions& options_;
    Wc3EmitterReport& report_;
    Materials materials_;
    std::vector<Keyed> pool_;
    std::map<u32, Keyed> worldTurns_;
    u32 nextId_;
    /// The baked sprites' quads, built into one mesh by `finish`.
    geom::MeshBuilder baked_;
    u32 bakedQuads_ = 0;
    /// Each global loop made, by its length.
    std::vector<std::pair<f32, u32>> loops_;
    /// Each spinning bone made: its parent, its speed, the bone.
    std::vector<std::tuple<u32, f32, u32>> spins_;
};

} // namespace

void PrepareWc3Effects(wem::Document& document, wem::Diagnostics& diagnostics,
                       bool ribbonOffsets) {
    if (document.models.empty()) {
        return;
    }
    Model& model = document.models.front();
    // Warcraft III's hierarchy composes every node onto its parent whatever
    // bit 0x80000 says (`MdxHierarchy`, and Blizzard's own conversions keep an
    // emitter under the bone it was linked to: War3_Wisp's SC2Particles001).
    // Only the particle system reads the bit.
    for (Node& node : model.nodes.nodes) {
        node.flags = static_cast<wem::NodeFlags>(static_cast<u32>(node.flags) &
                                                 ~static_cast<u32>(wem::NodeFlags::ModelSpace));
    }
    // §A9: a squirting emitter's bursts ignore its visibility in Warcraft III
    // and would obey the bone's in StarCraft II, so the track that would hide
    // them does not cross. Nor does a `CORN` node's (`KPPV`): the game never
    // reads it, and its guide and `KPPE` say when the effect plays.
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        const Node& node = model.nodes.nodes[n];
        const auto* pe = std::get_if<wem::Wc3ParticleEmitter2Payload>(&node.payload);
        const bool corn = node.kind == NodeKind::Wc3CornEmitter;
        if (!corn && (pe == nullptr || !pe->squirt)) {
            continue;
        }
        std::vector<u32> dropped;
        auto& channels = model.animChannels.channels;
        for (auto it = channels.begin(); it != channels.end();) {
            if (it->target.kind == TrackTarget::Kind::Node && it->target.node == n &&
                it->target.channel == Channel::Visibility) {
                dropped.push_back(it->id);
                it = channels.erase(it);
            } else {
                ++it;
            }
        }
        if (dropped.empty()) {
            continue;
        }
        for (wem::Clip& clip : document.clips) {
            for (wem::SubTrackContainer& container : clip.containers) {
                auto& tracks = container.subTracks;
                tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                            [&](const SubTrack& t) {
                                                return std::find(dropped.begin(), dropped.end(),
                                                                 t.channel) != dropped.end();
                                            }),
                             tracks.end());
            }
        }
        diagnostics.info(DiagCode::AnimTrackDropped,
                         corn ? "PopcornFX emitter '" + model.nodes.nodes[n].name +
                                    "' keys a visibility Warcraft III never reads; not carried"
                              : "particle emitter '" + model.nodes.nodes[n].name +
                                    "' squirts; its bursts ignore its visibility in Warcraft III, "
                                    "so the track that would hide them in StarCraft II is not "
                                    "carried",
                         ElementRef(ElementKind::Node, n));
    }
    // §4.5 row 2: Warcraft III lays a strip from `heightBelow` under its node
    // to `heightAbove` over it, StarCraft II centres one on its bone. A helper
    // half the difference along the node's +Y -- the bone's +X, the strip's
    // width (E3) -- puts both edges back. It takes the node's visibility with
    // it: the record rides it, and on the node the visibility would also hide
    // the node's other children, which Warcraft III's never does.
    if (!ribbonOffsets) {
        return;
    }
    const u32 count = model.nodes.size();
    for (u32 n = 0; n < count; ++n) {
        const auto* rb = std::get_if<wem::Wc3RibbonEmitterPayload>(&model.nodes.nodes[n].payload);
        if (rb == nullptr) {
            continue;
        }
        const f32 offset = 0.5f * (rb->heightAbove - rb->heightBelow);
        if (!(std::fabs(offset) > kRibbonOffsetEpsilon)) {
            continue;
        }
        const std::string name = model.nodes.nodes[n].name;
        const Vector3f pivot = model.nodes.nodes[n].pivot;
        Node helper;
        helper.name = name + "_Offset";
        helper.kind = NodeKind::Helper;
        helper.resetPayloadForKind();
        helper.parent = n;
        helper.pivot = Vector3f{pivot.x, pivot.y + offset, pivot.z};
        helper.local.translation = Vector3f{0.0f, offset, 0.0f};
        helper.poses.assign(model.nodes.poseSchema.size(), helper.local);
        helper.native.set(kRibbonOffset, static_cast<i64>(n));
        const u32 index = model.nodes.add(std::move(helper));
        for (AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == n &&
                channel.target.channel == Channel::Visibility) {
                channel.target.node = index;
            }
        }
        diagnostics.info(DiagCode::RigConventionChanged,
                         "ribbon emitter '" + name + "' rides a helper " + std::to_string(offset) +
                             " units across its width: StarCraft II centres the strip",
                         ElementRef(ElementKind::Node, index));
    }
}

m3::ParticleEmitter Sc2RestParticle() {
    return RestParticle();
}

m3::RibbonEmitter Sc2RestRibbon() {
    return RestRibbon();
}

Wc3EmitterReport PlaceSc2Records(wem::Document& staged, const std::vector<Sc2CornCrossing>& crossings,
                                 const Wc3EmitterOptions& options) {
    Wc3EmitterReport report;
    if (staged.models.empty()) {
        return report;
    }
    Crossing crossing(staged, options, report);
    for (const Sc2CornCrossing& c : crossings) {
        crossing.place(c);
    }
    crossing.finish();
    return report;
}

Wc3EmitterReport CrossWc3Emitters(wem::Document& staged, const Wc3EmitterOptions& options) {
    Wc3EmitterReport report;
    if (staged.models.empty()) {
        return report;
    }
    if (!wem::CarriesNodeKind(options.profile, NodeKind::Sc2ParticleEmitter)) {
        report.diagnostics.error(DiagCode::NodeKindNotCarried,
                                 std::string(wem::Profile(options.profile).displayName) +
                                     " carries no StarCraft II emitter; nothing crossed",
                                 ElementRef(), options.profile);
        return report;
    }
    Crossing(staged, options, report).run();
    return report;
}

} // namespace cross
} // namespace models
} // namespace whiteout

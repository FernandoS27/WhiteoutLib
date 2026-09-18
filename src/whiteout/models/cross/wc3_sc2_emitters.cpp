// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/cross/wc3_sc2_emitters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/wem/converters.h>

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

    auto tie() const {
        return std::tie(texture, filter, unshaded, unfogged, twoSided, priority, particle,
                        teamGlow, glowMask, wrap);
    }
    bool operator<(const MaterialKey& other) const {
        return tie() < other.tie();
    }
};

/// A textured layer at the rests every shipped textured layer states, addressed
/// the way the source's texture @p wrap word says.
m3::TextureLayer TexturedLayer(const std::string& path, bool particle, u32 wrap) {
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
    if (particle) {
        layer.flags |= m3::TextureLayerFlag::ParticleUVFlipbook;
        layer.uvMapping = m3::UVMappingMode::ParticleFlipbook;
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
        m3::StandardMaterial mat;
        mat.name = std::string(key.particle ? "Particle_" : "Ribbon_") +
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
        mat.additionalFlags = m3::MaterialAdditionalFlag::DepthBlendFalloff |
                              m3::MaterialAdditionalFlag::VertexColor |
                              m3::MaterialAdditionalFlag::VertexAlpha;
        mat.materialClass = m3::MaterialClass::SpecialFX;
        mat.priority = key.priority;
        mat.specularExponent = 20.0f;
        mat.hdrSpecularMultiplier = 1.5f;
        mat.hdrEmissiveMultiplier = 1.5f;
        mat.depthBlendFalloff = 0.04f;

        const bool textured = !key.texture.empty();
        if (textured && key.teamGlow) {
            // The team's colour, weighted by the texture's alpha on the emissive
            // team op, and nothing under it: Blizzard's own Wisp spells its
            // team-glow emitters this way, on each emitter's own texture. The
            // export hands over the glow itself as a mask instead, its red moved
            // into alpha: the glow file keeps its shape on red over a flat
            // alpha, and an emissive slot reads that red gamma-decoded (War3
            // Wisp's glow came out far dimmer than Warcraft III's).
            mat.emissiveLayer1 = TexturedLayer(key.texture, key.particle, key.wrap);
            mat.emissiveLayer1->colorType = m3::ColorChannelSelect::Alpha;
            mat.emissiveBlendMode1 = m3::LayerBlendOp::TeamColorEmissiveAdd;
        } else if (textured) {
            mat.diffuseLayer = TexturedLayer(key.texture, key.particle, key.wrap);
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
        if (mask && textured && !key.glowMask) {
            mat.alphaLayer1 = key.teamGlow ? *mat.emissiveLayer1 : *mat.diffuseLayer;
            mat.alphaLayer1->colorType = m3::ColorChannelSelect::Alpha;
        }
        if (!key.particle && mat.diffuseLayer.has_value()) {
            // Warcraft III runs a ribbon's U along its length; StarCraft II its
            // V (§4.5 row 10). A quarter turn lays one on the other.
            Rest(mat.diffuseLayer->uvAngle, Vector3f{0.0f, 0.0f, -kPi * 0.5f});
            if (mat.alphaLayer1.has_value()) {
                Rest(mat.alphaLayer1->uvAngle, Vector3f{0.0f, 0.0f, -kPi * 0.5f});
            }
        }

        const u32 added = bind(std::move(mat));
        cache_.emplace(key, added);
        ++report_.materials;
        return added;
    }

private:
    /// @p mat as a slot of its own, bound at every look of the profile's set.
    /// It enters WEM the way an `.m3` import brings one -- the record itself as
    /// the native block, in sync -- so `toM3` writes it back as it is.
    u32 bind(m3::StandardMaterial mat) {
        std::string name = mat.name;
        while (model_.slotIndex(name) != kInvalidIndex) {
            name += "_";
        }
        m3::Model scratch;
        scratch.standardMaterials.push_back(std::move(mat));
        m3::MaterialMap entry;
        entry.materialType = m3::MaterialType::Standard;
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
          nextId_(model_.animChannels.nextFreeId()) {}

    void run() {
        // The records a system adds are appended, so the count taken first
        // visits the source's nodes only.
        const u32 count = model_.nodes.size();
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
        const wem::ProfileMaterialSet* set = model_.setFor(document_.defaultProfile);
        const wem::Material* material =
            set != nullptr ? wem::Resolve(model_, slot, document_.defaultProfile, set->defaultLook)
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
        keyChannels(node, NodeKind::Sc2ParticleEmitter, record,
                    [](auto& r, auto&& f) { wem::m3_emitters::ForEachParticleRef(r, f); });
        Node& target = model_.nodes.nodes[node];
        target.kind = NodeKind::Sc2ParticleEmitter;
        target.payload = wem::m3_emitters::ImportParticle(record, wem::m3_emitters::ImportLinks{});
        ++report_.particleRecords;
    }

    /// A second record of @p parent's system: a child at identity that draws
    /// only while its parent does, so it rides the parent's bone.
    u32 addRecordNode(u32 parent, const char* suffix) {
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
        node.native.set(wem::kNodeSharesParentVisibility, static_cast<i64>(1));
        return model_.nodes.add(std::move(node));
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
    u32 nextId_;
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
    // them does not cross.
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        const auto* pe = std::get_if<wem::Wc3ParticleEmitter2Payload>(&model.nodes.nodes[n].payload);
        if (pe == nullptr || !pe->squirt) {
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
                         "particle emitter '" + model.nodes.nodes[n].name +
                             "' squirts; its bursts ignore its visibility in Warcraft III, so the "
                             "track that would hide them in StarCraft II is not carried",
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

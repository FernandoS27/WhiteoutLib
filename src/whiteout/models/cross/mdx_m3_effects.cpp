// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/cross/mdx_m3_effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <tuple>

#include "../wem/converters/m3_anim.h"
#include "../wem/converters/m3_track_sink.h"
#include "../wem/converters/mdx_track_slicer.h"

namespace whiteout {
namespace models {
namespace cross {

namespace {

namespace wem = ::whiteout::models::wem;
namespace geom = wem::geom;
namespace m3_sink = wem::m3_sink;
namespace mdx_slice = wem::mdx_slice;
using wem::DiagCode;
using wem::ElementKind;
using wem::ElementRef;
using wem::Interpolation;
using wem::kInvalidIndex;
using wem::SubTrack;
using wem::ValuesPerKey;

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDegToRad = kPi / 180.0f;

/// The runtime's pool cap: 0x200000 bytes of vertex arena over a 464-byte
/// element (SC2_PARTICLE_RE §3.1). An emitter asking for more gets fewer.
constexpr u32 kMaxParticles = 0x200000u / 464u;

/// A mid time the clone path clamps to (`InitCopy`), written as Blizzard's
/// tools write every unkeyed one.
constexpr f32 kMidTimeRest = 0.996f;

// PRE2's own bits in the node flag word (`mdx::Node::NodeFlag`'s PE2 aliases).
constexpr u32 kUnshaded = 0x8000u;
constexpr u32 kSortPrimsFarZ = 0x10000u;
constexpr u32 kLineEmitter = 0x20000u;
constexpr u32 kUnfogged = 0x40000u;
constexpr u32 kModelSpace = 0x80000u;
constexpr u32 kXYQuad = 0x100000u;

/// Warcraft III's squirt edge: the rate crosses this on its way up
/// (`actor_eval.cpp`, `ps.emissionRate > 0.02f && last <= 0.02f`).
constexpr f32 kSquirtEdge = 0.02f;

/// The native key on the helper `PrepareWc3Effects` plants under an
/// off-centre ribbon: the ribbon's `objectId`.
constexpr const char* kRibbonOffsetOf = "wc3RibbonOffsetOf";

/// Half a height difference below this is a centred ribbon.
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

/// A Warcraft III colour track read red first: keyed colours are stored blue
/// first beside a red-first static (`mdx_anim.cpp`, `SwapRedBlue`).
mdx::Track<Vector3f> RedFirst(mdx::Track<Vector3f> track) {
    for (Vector3f& value : track.keys_data) {
        std::swap(value.x, value.z);
    }
    return track;
}

m3::ColorBGRA Color(const Vector3f& rgb, u8 alpha) {
    m3::ColorBGRA c;
    c.r = ColorByte(rgb.x);
    c.g = ColorByte(rgb.y);
    c.b = ColorByte(rgb.z);
    c.a = alpha;
    return c;
}

/// The first node whose native @p key holds @p objectId.
u32 NodeOfNative(const wem::Document& document, const char* key, u32 objectId) {
    if (document.models.empty()) {
        return kInvalidIndex;
    }
    const wem::NodeTree& nodes = document.models.front().nodes;
    for (u32 n = 0; n < nodes.size(); ++n) {
        const wem::NodeNative& native = nodes.nodes[n].native;
        if (native.find(key) != nullptr && native.value(key, -1) == static_cast<i64>(objectId)) {
            return n;
        }
    }
    return kInvalidIndex;
}

u32 NodeOfObjectId(const wem::Document& document, u32 objectId) {
    return NodeOfNative(document, "objectId", objectId);
}

/// @p track at @p timeMs on its own timeline, the value element only: held
/// before the first key and after the last, held across a step span, a
/// straight line otherwise. A smooth key's tangents are not read -- no M3
/// stream carries them.
template <class T>
T SampleTrack(const mdx::Track<T>& track, f32 timeMs, const T& rest) {
    if (!track.isUsed || track.timestamps.empty() || !mdx_slice::WellFormed(track)) {
        return rest;
    }
    const u32 perKey = ValuesPerKey(
        mdx_slice::InterpOf(track.interpolationType, mdx_slice::ValueTrait<T>::kType));
    const auto value = [&](std::size_t k) { return track.keys_data[k * perKey]; };
    const std::vector<u32>& t = track.timestamps;
    if (timeMs <= static_cast<f32>(t.front())) {
        return value(0);
    }
    if (timeMs >= static_cast<f32>(t.back())) {
        return value(t.size() - 1);
    }
    std::size_t hi = 1;
    while (hi < t.size() && static_cast<f32>(t[hi]) < timeMs) {
        ++hi;
    }
    if (track.interpolationType == mdx::InterpolationType::None || t[hi] == t[hi - 1]) {
        return static_cast<f32>(t[hi]) <= timeMs ? value(hi) : value(hi - 1);
    }
    const f32 a = static_cast<f32>(t[hi - 1]);
    const f32 f = (timeMs - a) / (static_cast<f32>(t[hi]) - a);
    const T lo = value(hi - 1);
    const T up = value(hi);
    return lo + (up - lo) * f;
}

bool Keyed(const auto& track) {
    return track.isUsed && !track.timestamps.empty();
}

/// Two tracks as one, keyed on the union of their times: each key the combine
/// of both tracks sampled there. What a merged StarCraft II property needs
/// when Warcraft III keys its halves apart -- speed and its variation, width
/// and length, above and below. The result steps only when both did. A track
/// on a global sequence merges only with one on the same clock; otherwise the
/// first track's keys are kept and the second is read at its rest.
template <class A, class B, class R, class Fn>
mdx::Track<R> MergeTracks(const mdx::Track<A>& a, const A& restA, const mdx::Track<B>& b,
                          const B& restB, Fn combine) {
    mdx::Track<R> out;
    const bool keyedA = Keyed(a);
    const bool keyedB = Keyed(b) && (!keyedA || a.globalSequenceId == b.globalSequenceId);
    if (!keyedA && !keyedB) {
        return out;
    }
    std::vector<u32> times;
    if (keyedA) {
        times.insert(times.end(), a.timestamps.begin(), a.timestamps.end());
    }
    if (keyedB) {
        times.insert(times.end(), b.timestamps.begin(), b.timestamps.end());
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    const bool stepped = (!keyedA || a.interpolationType == mdx::InterpolationType::None) &&
                         (!keyedB || b.interpolationType == mdx::InterpolationType::None);
    out.isUsed = true;
    out.interpolationType =
        stepped ? mdx::InterpolationType::None : mdx::InterpolationType::Linear;
    out.globalSequenceId = keyedA ? a.globalSequenceId : b.globalSequenceId;
    for (const u32 time : times) {
        const f32 at = static_cast<f32>(time);
        out.timestamps.push_back(time);
        out.keys_data.push_back(combine(keyedA ? SampleTrack(a, at, restA) : restA,
                                        keyedB ? SampleTrack(b, at, restB) : restB));
    }
    out.keyCount = out.timestamps.size();
    return out;
}

/// @p track with every value mapped through @p fn, keys and clock kept; a
/// smooth track becomes linear, since no M3 stream carries tangents.
template <class T, class R, class Fn>
mdx::Track<R> MapTrack(const mdx::Track<T>& track, Fn fn) {
    mdx::Track<R> out;
    if (!Keyed(track) || !mdx_slice::WellFormed(track)) {
        return out;
    }
    const u32 perKey = ValuesPerKey(
        mdx_slice::InterpOf(track.interpolationType, mdx_slice::ValueTrait<T>::kType));
    out.isUsed = true;
    out.interpolationType = track.interpolationType == mdx::InterpolationType::None
                                ? mdx::InterpolationType::None
                                : mdx::InterpolationType::Linear;
    out.globalSequenceId = track.globalSequenceId;
    out.timestamps = track.timestamps;
    for (std::size_t k = 0; k < track.timestamps.size(); ++k) {
        out.keys_data.push_back(fn(track.keys_data[k * perKey]));
    }
    out.keyCount = out.timestamps.size();
    return out;
}

/// The largest value @p track ever keys, or @p rest.
template <class T>
f32 MaxOf(const mdx::Track<T>& track, f32 rest) {
    if (!Keyed(track) || !mdx_slice::WellFormed(track)) {
        return rest;
    }
    const u32 perKey = ValuesPerKey(
        mdx_slice::InterpOf(track.interpolationType, mdx_slice::ValueTrait<T>::kType));
    f32 best = -1e30f;
    for (std::size_t k = 0; k < track.timestamps.size(); ++k) {
        best = (std::max)(best, static_cast<f32>(track.keys_data[k * perKey]));
    }
    return best;
}

// ----------------------------------------------------------------------------
// Streams: an mdx track, cut into the clips, into the file
// ----------------------------------------------------------------------------

class Streams {
public:
    Streams(const wem::Document& staged, const wem::M3ExportMap& map, m3::Model& out,
            Wc3EffectReport& report)
        : staged_(staged), map_(map), out_(out), report_(report), next_(map.nextAnimId) {
        // Past every id the file already names, whatever the map said.
        for (const m3::SubTrackContainer& stc : out.subTrackCollections) {
            for (const u32 id : stc.animIds) {
                next_ = (std::max)(next_, id + 1);
            }
        }
        next_ = (std::max)(next_, u32{1});
    }

    /// Writes @p track (values already in the StarCraft II property's units
    /// and layout, `f32`, `u32` or `Vector3f`) into every clip that plays it,
    /// and binds every AnimRef in @p refs to the one stream. False when no clip
    /// plays a key of it -- the AnimRefs then rest at their own values.
    template <class T, class Ref>
    bool keyed(const mdx::Track<T>& track, m3_sink::Stream stream,
               std::initializer_list<m3::AnimRef<Ref>*> refs,
               const Quaternion* restQuaternion = nullptr) {
        const std::vector<mdx_slice::ClipCut> cuts =
            mdx_slice::CutForClips(track, staged_, 0, 0);
        if (cuts.empty()) {
            return false;
        }
        const u32 id = next_;
        bool written = false;
        for (const mdx_slice::ClipCut& cut : cuts) {
            u32 stc = kInvalidIndex;
            u32 sequence = kInvalidIndex;
            if (!containerOf(cut.clip, sequence, stc)) {
                continue;
            }
            m3_sink::StreamSpec spec;
            spec.stream = stream;
            spec.type = mdx_slice::ValueTrait<T>::kType;
            spec.restQuaternion = restQuaternion;
            const u32 ref = m3_sink::WriteStream(
                out_.subTrackCollections[stc], spec, cut.track,
                static_cast<i32>(out_.sequences[sequence].startFrame),
                staged_.clips[cut.clip].duration,
                m3_sink::WarcraftWindow(staged_.clips[cut.clip]));
            if (ref == kInvalidIndex) {
                continue;
            }
            m3_sink::AddToContainer(out_, stc, id, ref);
            for (m3::AnimRef<Ref>* anim : refs) {
                wiring_.wire(*anim, id, cut.track.interp);
            }
            ++report_.keyedStreams;
            written = true;
        }
        if (written) {
            ++next_;
        }
        return written;
    }

    /// An `RGBA` stream out of a colour track and an alpha track (either may be
    /// unkeyed), bound to every colour AnimRef in @p refs.
    bool keyedColor(const mdx::Track<Vector3f>& color, const mdx::Track<f32>& alpha,
                    const Vector3f& restColor, f32 restAlpha,
                    std::initializer_list<m3::AnimRef<m3::ColorBGRA>*> refs) {
        if (!Keyed(color) && !Keyed(alpha)) {
            return false;
        }
        const u32 id = next_;
        bool written = false;
        for (std::size_t c = 0; c < staged_.clips.size(); ++c) {
            u32 stc = kInvalidIndex;
            u32 sequence = kInvalidIndex;
            if (!containerOf(static_cast<u32>(c), sequence, stc)) {
                continue;
            }
            const auto colorCut = cutFor(color, static_cast<u32>(c));
            const auto alphaCut = cutFor(alpha, static_cast<u32>(c));
            if (!colorCut && !alphaCut) {
                continue;
            }
            const SubTrack merged = m3_sink::MergeColorAlpha(
                colorCut ? &*colorCut : nullptr, alphaCut ? &*alphaCut : nullptr, restColor,
                restAlpha);
            m3_sink::StreamSpec spec;
            spec.stream = m3_sink::Stream::Sdcc;
            spec.type = geom::AttrType::F32x4;
            const u32 ref = m3_sink::WriteStream(
                out_.subTrackCollections[stc], spec, merged,
                static_cast<i32>(out_.sequences[sequence].startFrame), staged_.clips[c].duration,
                m3_sink::WarcraftWindow(staged_.clips[c]));
            if (ref == kInvalidIndex) {
                continue;
            }
            m3_sink::AddToContainer(out_, stc, id, ref);
            for (m3::AnimRef<m3::ColorBGRA>* anim : refs) {
                wiring_.wire(*anim, id, merged.interp);
            }
            ++report_.keyedStreams;
            written = true;
        }
        if (written) {
            ++next_;
        }
        return written;
    }

    /// Warcraft III's squirt, as StarCraft II's: a burst key at every RISING
    /// edge of the stepped emission rate inside each clip, its count the rate
    /// the edge rises to. A flat key would fire again every time a playhead
    /// crosses it, where Warcraft III bursts only on the way up. Returns the
    /// largest burst.
    u32 squirt(const mdx::Track<f32>& rate, m3::AnimRef<u16>& ref) {
        const std::vector<mdx_slice::ClipCut> cuts = mdx_slice::CutForClips(rate, staged_, 0, 0);
        const u32 id = next_;
        u32 largest = 0;
        bool written = false;
        for (const mdx_slice::ClipCut& cut : cuts) {
            u32 stc = kInvalidIndex;
            u32 sequence = kInvalidIndex;
            if (!containerOf(cut.clip, sequence, stc)) {
                continue;
            }
            const f32 duration = staged_.clips[cut.clip].duration;
            // Only the keys the window plays: Warcraft III's engine reads no
            // bracket key, and holds the last one across the wrap.
            std::vector<std::pair<f32, f32>> keys;
            const std::size_t stride = ValuesPerKey(cut.track.interp) * sizeof(f32);
            for (std::size_t k = 0; k < cut.track.times.size(); ++k) {
                const f32 t = cut.track.times[k];
                if (t < -1e-4f || t > duration + 1e-4f) {
                    continue;
                }
                f32 v = 0.0f;
                std::memcpy(&v, cut.track.values.data() + k * stride, sizeof(f32));
                keys.emplace_back((std::max)(t, 0.0f), v);
            }
            if (keys.empty()) {
                continue;
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
                const u8* bytes = reinterpret_cast<const u8*>(&count);
                bursts.values.insert(bursts.values.end(), bytes, bytes + sizeof(u32));
            }
            if (bursts.times.empty()) {
                continue;
            }
            m3_sink::StreamSpec spec;
            spec.stream = m3_sink::Stream::Sds6;
            spec.type = geom::AttrType::U32;
            // Never a Warcraft window: a wrap key would be a burst nobody keyed.
            const u32 block = m3_sink::WriteStream(
                out_.subTrackCollections[stc], spec, bursts,
                static_cast<i32>(out_.sequences[sequence].startFrame), duration, false);
            if (block == kInvalidIndex) {
                continue;
            }
            m3_sink::AddToContainer(out_, stc, id, block);
            wiring_.wire(ref, id, Interpolation::Step);
            ++report_.keyedStreams;
            written = true;
        }
        if (written) {
            ++next_;
        }
        return largest;
    }

private:
    bool containerOf(u32 clip, u32& sequence, u32& stc) const {
        if (clip >= map_.clipSequence.size()) {
            return false;
        }
        sequence = map_.clipSequence[clip];
        if (sequence >= map_.sequenceStc.size() || sequence >= out_.sequences.size()) {
            return false;
        }
        stc = map_.sequenceStc[sequence];
        return stc < out_.subTrackCollections.size();
    }

    template <class T>
    std::optional<SubTrack> cutFor(const mdx::Track<T>& track, u32 clip) const {
        for (mdx_slice::ClipCut& cut : mdx_slice::CutForClips(track, staged_, 0, 0)) {
            if (cut.clip == clip) {
                return std::move(cut.track);
            }
        }
        return std::nullopt;
    }

    const wem::Document& staged_;
    const wem::M3ExportMap& map_;
    m3::Model& out_;
    Wc3EffectReport& report_;
    u32 next_ = 1;
    m3_sink::Wiring wiring_;
};

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

class Materials {
public:
    Materials(m3::Model& out, Wc3EffectReport& report) : out_(out), report_(report) {}

    /// The material map entry for @p key, built on first use.
    u32 entry(const MaterialKey& key) {
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

        m3::MaterialMap map;
        map.materialType = m3::MaterialType::Standard;
        map.materialIndex = static_cast<u32>(out_.standardMaterials.size());
        out_.standardMaterials.push_back(std::move(mat));
        const u32 index = static_cast<u32>(out_.materialMaps.size());
        out_.materialMaps.push_back(map);
        cache_.emplace(key, index);
        ++report_.materials;
        return index;
    }

private:
    m3::Model& out_;
    Wc3EffectReport& report_;
    std::map<MaterialKey, u32> cache_;
};

Filter ParticleFilter(u32 filterMode) {
    switch (filterMode) {
    case 1:
        return Filter::Additive;
    case 2:
        return Filter::Modulate;
    case 3:
        return Filter::Modulate2x;
    case 4:
        return Filter::AlphaKey;
    default:
        return Filter::Blend;
    }
}

Filter LayerFilter(mdx::Layer::FilterMode mode) {
    switch (mode) {
    case mdx::Layer::FilterMode::Transparent:
        return Filter::AlphaKey;
    case mdx::Layer::FilterMode::Additive:
    case mdx::Layer::FilterMode::AddAlpha:
        return Filter::Additive;
    case mdx::Layer::FilterMode::Modulate:
        return Filter::Modulate;
    case mdx::Layer::FilterMode::Modulate2x:
        return Filter::Modulate2x;
    default:
        return Filter::Blend;
    }
}

std::string TexturePath(const Wc3EffectOptions& options, u32 textureId) {
    return textureId < options.texturePaths.size() ? options.texturePaths[textureId]
                                                   : std::string();
}

/// @p textureId's `TEXS` address word (0x1 wrap U, 0x2 wrap V).
u32 TextureWrap(const mdx::Model& source, u32 textureId) {
    return textureId < source.textures.size()
               ? static_cast<u32>(source.textures[textureId].flags) & 0x3u
               : 0u;
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

struct Intervals {
    std::array<u32, 3> life{};
    std::array<u32, 3> decay{};
};

void Flipbook(m3::ParticleEmitter& p, const mdx::ParticleEmitter2& pe, const Intervals& cells) {
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
    p.flipbookStartInitIndex = cell(cells.life[0]);
    p.flipbookStartStopIndex =
        cell(cells.decay[0] == cells.life[1] + 1 ? cells.decay[0] : cells.life[1]);
    p.flipbookEndInitIndex = cell(cells.decay[1]);
    p.flipbookEndStopIndex = cell(cells.decay[1]);
    p.flipbookMidTime = (std::min)(pe.time, kMidTimeRest);
}

/// The share of an XY quad emitter's particles Warcraft III draws facing the
/// camera. It turns a quad by the velocity's XY only while that is longer than
/// 1e-3 -- a squared 1e-6 (`particle_geometry.cpp`) -- and takes the camera's
/// axes below it, so a slow, narrow cone draws a share of its quads as
/// billboards: ChineseFairy's crown rays (speed 2, latitude 0.1 degrees) face
/// the camera 29% of the time. The polar and speed draws are uniform on their
/// ranges (`PlaneShape`, `DrawSpeed`), so the share is the measure of the draws
/// that stay under it.
f32 CameraFacingShare(const mdx::ParticleEmitter2& pe) {
    constexpr f64 kTurningSpeed = 1e-3;
    constexpr f64 kHalfTurn = 3.14159265358979323846;
    constexpr int kSpeedSteps = 32;
    const f64 latitude = std::fabs(static_cast<f64>(pe.latitude)) * kHalfTurn / 180.0;
    f64 share = 0.0;
    for (int i = 0; i < kSpeedSteps; ++i) {
        const f64 draw = -1.0 + (2.0 * i + 1.0) / kSpeedSteps;
        const f64 speed = std::fabs(static_cast<f64>(pe.speed) * (1.0 + pe.variation * draw));
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

class ParticleCrossing {
public:
    ParticleCrossing(const mdx::Model& source, const wem::Document& staged,
                     const wem::M3ExportMap& map, const Wc3EffectOptions& options, m3::Model& out,
                     Streams& streams, Materials& materials, Wc3EffectReport& report)
        : source_(source), staged_(staged), map_(map), options_(options), out_(out),
          streams_(streams), materials_(materials), report_(report) {}

    void run() {
        for (std::size_t i = 0; i < source_.particleEmitters2.size(); ++i) {
            cross(source_.particleEmitters2[i], static_cast<u32>(i));
        }
    }

private:
    void cross(const mdx::ParticleEmitter2& pe, u32 index) {
        const std::string name = pe.node.name;
        const ElementRef where(ElementKind::Node, pe.node.objectId);
        const u32 node = NodeOfObjectId(staged_, pe.node.objectId);
        const u32 bone = node < map_.nodeVisBone.size() ? map_.nodeVisBone[node] : kInvalidIndex;
        if (bone == kInvalidIndex || bone >= out_.bones.size()) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "particle emitter '" + name +
                                         "' has no bone to ride; it was not written",
                                     where);
            return;
        }
        const f32 L = options_.lengthScale;
        const u32 nf = static_cast<u32>(pe.node.flags);
        const bool squirt = pe.squirt != 0;
        const bool line = (nf & kLineEmitter) != 0;
        const bool xyQuad = (nf & kXYQuad) != 0;
        const bool modelSpace = (nf & kModelSpace) != 0;

        m3::ParticleEmitter p = RestParticle();
        p.boneIndex = bone;

        // Row 27: a Warcraft III particle leaves its emitter unless the node
        // says model space.
        if (!modelSpace) {
            p.additionalFlags |= m3::ParticleAdditionalFlag::WorldSpace;
        }

        // Rows 1-2: speed and its variation are both endpoints of one uniform
        // draw, s(1-v) to s(1+v), in either engine.
        const f32 s = pe.speed;
        const f32 v = pe.variation;
        Rest(p.initialSpeed, s * (1.0f - v) * L);
        Rest(p.initialSpeedRandom, s * (1.0f + v) * L);
        if (v > 0.0f || Keyed(pe.variationTracks)) {
            p.additionalFlags |= m3::ParticleAdditionalFlag::EmitSpeedRandomize;
        }
        if (Keyed(pe.speedTracks) || Keyed(pe.variationTracks)) {
            const auto low = MergeTracks<f32, f32, f32>(
                pe.speedTracks, s, pe.variationTracks, v,
                [L](f32 speed, f32 var) { return speed * (1.0f - var) * L; });
            const auto high = MergeTracks<f32, f32, f32>(
                pe.speedTracks, s, pe.variationTracks, v,
                [L](f32 speed, f32 var) { return speed * (1.0f + var) * L; });
            streams_.keyed(low, m3_sink::Stream::Sdr3, {&p.initialSpeed});
            streams_.keyed(high, m3_sink::Stream::Sdr3, {&p.initialSpeedRandom});
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
        if (!line && Keyed(pe.latitudeTracks) &&
            (MaxOf(pe.latitudeTracks, pe.latitude) > kSphereLatitude) != sphere) {
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
        if (Keyed(pe.latitudeTracks)) {
            const auto cone =
                MapTrack<f32, f32>(pe.latitudeTracks, [&](f32 d) { return horizontalOf(d); });
            if (line) {
                streams_.keyed(cone, m3_sink::Stream::Sdr3, {&p.initialHorizontal});
            } else {
                streams_.keyed(cone, m3_sink::Stream::Sdr3,
                               {&p.initialHorizontal, &p.initialVertical});
            }
        }

        // Rows 5-6: Warcraft III pulls down by `gravity`; StarCraft II adds its
        // gravity along +Z, both closed forms `1/2 a t^2`, and the scene scale
        // the runtime multiplies in is 1 in a game (R1).
        p.gravity = -pe.gravity * L;
        if (Keyed(pe.gravityTracks)) {
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
        const f32 facing = xyQuad ? CameraFacingShare(pe) : 0.0f;
        const bool whole = squirt || pe.headOrTail != 0;
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
            if (Keyed(pe.emissionRateTracks)) {
                const auto rate = MapTrack<f32, f32>(
                    pe.emissionRateTracks, [share](f32 r) { return (std::max)(r, 0.0f) * share; });
                streams_.keyed(rate, m3_sink::Stream::Sdr3, {&record.emissionRate});
            }
        };
        if (squirt) {
            RestEmission(p.emissionRate, 0.0f);
            if (Keyed(pe.emissionRateTracks)) {
                maxSquirt = streams_.squirt(pe.emissionRateTracks, p.squirtAmount);
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
            if (Keyed(pe.emissionRateTracks)) {
                maxRate = (std::max)(maxRate, MaxOf(pe.emissionRateTracks, 0.0f));
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
        Rest(p.shapeOuter, Vector3f{pe.width * L, pe.length * L, 0.0f});
        if (Keyed(pe.widthTracks) || Keyed(pe.lengthTracks)) {
            const auto outer = MergeTracks<f32, f32, Vector3f>(
                pe.widthTracks, pe.width, pe.lengthTracks, pe.length,
                [L](f32 w, f32 l) { return Vector3f{w * L, l * L, 0.0f}; });
            streams_.keyed(outer, m3_sink::Stream::Sd3v, {&p.shapeOuter});
        }

        // Row 12: Warcraft III's scale is a half extent and StarCraft II halves
        // its key, so the key is the full width.
        Rest(p.sizeAnimation, Vector3f{2.0f * pe.segmentScaling[0] * L,
                                       2.0f * pe.segmentScaling[1] * L,
                                       2.0f * pe.segmentScaling[2] * L});
        p.sizeMidTime = (std::min)(pe.time, kMidTimeRest);

        // Row 13.
        Rest(p.colorStart, Color(pe.segmentColor[0], pe.segmentAlpha[0]));
        Rest(p.colorMid, Color(pe.segmentColor[1], pe.segmentAlpha[1]));
        Rest(p.colorEnd, Color(pe.segmentColor[2], pe.segmentAlpha[2]));
        p.colorMidTime = (std::min)(pe.time, kMidTimeRest);
        p.alphaMidTime = p.colorMidTime;

        // Rows 23-26: light, fog, sorting, priority.
        const bool unshaded = (nf & kUnshaded) != 0;
        if (!unshaded) {
            p.flags |= m3::ParticleFlag::LitParts;
        }
        if ((nf & kSortPrimsFarZ) != 0) {
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
        key.teamGlow = pe.replaceableId == 2;
        key.glowMask = key.teamGlow && !options_.teamGlowMaskPath.empty();
        key.texture =
            key.glowMask ? options_.teamGlowMaskPath : TexturePath(options_, pe.textureId);
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
                                         ", which does not cross; it draws texture " +
                                         std::to_string(pe.textureId) + " instead",
                                     where);
        }
        key.wrap = TextureWrap(source_, pe.textureId);
        key.filter = ParticleFilter(pe.filterMode);
        key.unshaded = unshaded;
        key.unfogged = (nf & kUnfogged) != 0;
        key.priority = static_cast<i32>(pe.priorityPlane) + 1;
        key.particle = true;
        if (key.texture.empty()) {
            report_.diagnostics.warn(DiagCode::TextureUnresolved,
                                     "particle emitter '" + name + "' names texture " +
                                         std::to_string(pe.textureId) +
                                         ", which the export has no path for; it draws untextured",
                                     where);
        }
        p.materialIndex = materials_.entry(key);

        // Row 18's losses, said once per emitter.
        if (pe.headInterval[2] > 1 || pe.headDecayInterval[2] > 1 || pe.tailInterval[2] > 1 ||
            pe.tailDecayInterval[2] > 1) {
            report_.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                     "particle emitter '" + name +
                                         "' repeats a flipbook run; StarCraft II plays each run "
                                         "once per life",
                                     where);
        }

        // Row 14: head, tail, or one record for each.
        const bool head = pe.headOrTail == 0 || pe.headOrTail == 2;
        const bool tail = pe.headOrTail == 1 || pe.headOrTail == 2;
        if (head) {
            m3::ParticleEmitter h = p;
            Flipbook(h, pe, Intervals{pe.headInterval, pe.headDecayInterval});
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
            out_.particleEmitters.push_back(std::move(h));
            ++report_.particleRecords;
            if (split) {
                // The share that faces the camera, as the billboard it is.
                m3::ParticleEmitter b = p;
                Flipbook(b, pe, Intervals{pe.headInterval, pe.headDecayInterval});
                b.instanceType = m3::ParticleInstanceType::Billboard;
                restRate(b, facing);
                b.maxParticles = capFor(facing);
                out_.particleEmitters.push_back(std::move(b));
                ++report_.particleRecords;
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
            Flipbook(t, pe, Intervals{pe.tailInterval, pe.tailDecayInterval});
            t.instanceType = m3::ParticleInstanceType::Trail;
            // Row 15: Warcraft III's tail spans `v * T`. StarCraft II's speed
            // branch lays `2 * halfSize * |v| * tail` of length in its own
            // units, where the half size is half the key and the speed is
            // already scaled -- so the tail is the time over the key.
            const f32 sizeMid = 2.0f * pe.segmentScaling[1] * L;
            t.tailLength = sizeMid > 0.0f ? pe.tailLength / sizeMid : 1.0f;
            out_.particleEmitters.push_back(std::move(t));
            ++report_.particleRecords;
            if (head) {
                report_.diagnostics.info(DiagCode::AnimTrackApproximated,
                                         "particle emitter '" + name +
                                             "' draws a head and a tail; StarCraft II draws them "
                                             "as two emitters whose particles are not the same",
                                         where);
            }
        }
        if (!head && !tail) {
            report_.diagnostics.info(DiagCode::FeatureDropped,
                                     "particle emitter '" + name +
                                         "' draws neither a head nor a tail",
                                     where);
        }
        (void)index;
    }

    const mdx::Model& source_;
    const wem::Document& staged_;
    const wem::M3ExportMap& map_;
    const Wc3EffectOptions& options_;
    m3::Model& out_;
    Streams& streams_;
    Materials& materials_;
    Wc3EffectReport& report_;
};

// ----------------------------------------------------------------------------
// PREM -> PAR_ model particles (C8.1)
// ----------------------------------------------------------------------------

/// Warcraft III's model-spawning emitter: a cone of spawned models, each
/// stamped into the world at birth unturned and at scale 1, falling under its
/// gravity for its lifespan (`ChildModelEmitter`). The cone is PRE2's with the
/// latitude in radians and the azimuth spread `longitude` around the node's
/// +X, which is a full circle at pi -- every shipped emitter but five.
class ModelParticleCrossing {
public:
    ModelParticleCrossing(const mdx::Model& source, const wem::Document& staged,
                          const wem::M3ExportMap& map, const Wc3EffectOptions& options,
                          m3::Model& out, Streams& streams, Materials& materials,
                          Wc3EffectReport& report)
        : source_(source), staged_(staged), map_(map), options_(options), out_(out),
          streams_(streams), materials_(materials), report_(report) {}

    void run() {
        for (std::size_t i = 0; i < source_.particleEmitters.size(); ++i) {
            cross(source_.particleEmitters[i], i);
        }
    }

private:
    void cross(const mdx::ParticleEmitter& pe, std::size_t index) {
        const std::string& name = pe.node.name;
        const ElementRef where(ElementKind::Node, pe.node.objectId);
        const std::string path =
            index < options_.modelParticlePaths.size() ? options_.modelParticlePaths[index] : "";
        if (path.empty()) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "model-spawning emitter '" + name + "' spawns '" +
                                         pe.spawnModelFileName +
                                         "', which the export did not write; it was not written",
                                     where);
            return;
        }
        const u32 node = NodeOfObjectId(staged_, pe.node.objectId);
        const u32 bone = node < map_.nodeVisBone.size() ? map_.nodeVisBone[node] : kInvalidIndex;
        if (bone == kInvalidIndex || bone >= out_.bones.size()) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "model-spawning emitter '" + name +
                                         "' has no bone to ride; it was not written",
                                     where);
            return;
        }
        const f32 L = options_.lengthScale;
        m3::ParticleEmitter p = RestParticle();
        p.boneIndex = bone;
        p.flags |= m3::ParticleFlag::ModelParticles;
        p.additionalFlags |= m3::ParticleAdditionalFlag::WorldSpace;
        p.modelPaths = {path};
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
        Rest(p.initialSpeed, pe.initialVelocity * L);
        Rest(p.initialSpeedRandom, pe.initialVelocity * L);
        if (Keyed(pe.speedTracks)) {
            const auto speed = MapTrack<f32, f32>(pe.speedTracks, [L](f32 v) { return v * L; });
            streams_.keyed(speed, m3_sink::Stream::Sdr3, {&p.initialSpeed, &p.initialSpeedRandom});
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
        if (Keyed(pe.latitudeTracks)) {
            const auto cone = MapTrack<f32, f32>(pe.latitudeTracks, spreadOf);
            streams_.keyed(cone, m3_sink::Stream::Sdr3,
                           {&p.initialHorizontal, &p.initialVertical});
        }
        if (pe.longitude < kPi - 1e-3f || Keyed(pe.longitudeTracks)) {
            report_.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                     "model-spawning emitter '" + name +
                                         "' spreads over part of a circle of azimuth; StarCraft "
                                         "II's cone spreads over all of it",
                                     where);
        }

        // Row 5 (R1), as PRE2's.
        p.gravity = -pe.gravity * L;
        if (Keyed(pe.gravityTracks)) {
            report_.diagnostics.warn(DiagCode::AnimTrackDropped,
                                     "model-spawning emitter '" + name +
                                         "' keys its gravity; PAR_ gravity is not animated and "
                                         "rests at the record's value",
                                     where);
        }

        Rest(p.lifetime, pe.lifespan);
        Rest(p.lifetimeRandom, pe.lifespan);
        if (Keyed(pe.lifespanTracks)) {
            streams_.keyed(pe.lifespanTracks, m3_sink::Stream::Sdr3,
                           {&p.lifetime, &p.lifetimeRandom});
        }

        RestEmission(p.emissionRate, (std::max)(pe.emissionRate, 0.0f));
        f32 maxRate = (std::max)(pe.emissionRate, 0.0f);
        if (Keyed(pe.emissionRateTracks)) {
            const auto rate = MapTrack<f32, f32>(pe.emissionRateTracks,
                                                 [](f32 r) { return (std::max)(r, 0.0f); });
            streams_.keyed(rate, m3_sink::Stream::Sdr3, {&p.emissionRate});
            maxRate = (std::max)(maxRate, MaxOf(pe.emissionRateTracks, 0.0f));
        }
        f32 maxLife = (std::max)(pe.lifespan, 0.0f);
        if (Keyed(pe.lifespanTracks)) {
            maxLife = (std::max)(maxLife, MaxOf(pe.lifespanTracks, 0.0f));
        }
        const u64 wanted = static_cast<u64>(std::ceil(maxRate * maxLife * 1.15f)) + 16u;
        p.maxParticles = static_cast<u32>((std::min)(wanted, static_cast<u64>(kMaxParticles)));

        // The record names a material; the models draw their own.
        MaterialKey key;
        key.particle = true;
        p.materialIndex = materials_.entry(key);

        out_.particleEmitters.push_back(std::move(p));
        ++report_.particleRecords;
        ++report_.modelParticleRecords;
    }

    const mdx::Model& source_;
    const wem::Document& staged_;
    const wem::M3ExportMap& map_;
    const Wc3EffectOptions& options_;
    m3::Model& out_;
    Streams& streams_;
    Materials& materials_;
    Wc3EffectReport& report_;
};

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

class RibbonCrossing {
public:
    RibbonCrossing(const mdx::Model& source, const wem::Document& staged,
                   const wem::M3ExportMap& map, const Wc3EffectOptions& options, m3::Model& out,
                   Streams& streams, Materials& materials, Wc3EffectReport& report)
        : source_(source), staged_(staged), map_(map), options_(options), out_(out),
          streams_(streams), materials_(materials), report_(report) {}

    void run() {
        for (const mdx::RibbonEmitter& ribbon : source_.ribbonEmitters) {
            cross(ribbon);
        }
    }

private:
    void cross(const mdx::RibbonEmitter& rb) {
        const std::string name = rb.node.name;
        const ElementRef where(ElementKind::Node, rb.node.objectId);
        // Row 2: an off-centre ribbon rides the helper `PrepareWc3Effects`
        // planted across its width, which holds its visibility too. Row 14:
        // either bone unturned -- the strip's width is the bone's X, the node's
        // Y (E3).
        u32 node = NodeOfNative(staged_, kRibbonOffsetOf, rb.node.objectId);
        if (node == kInvalidIndex) {
            node = NodeOfObjectId(staged_, rb.node.objectId);
        }
        const u32 bone = node < map_.nodeVisBone.size() ? map_.nodeVisBone[node] : kInvalidIndex;
        if (bone == kInvalidIndex || bone >= out_.bones.size()) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "ribbon emitter '" + name +
                                         "' has no bone to ride; it was not written",
                                     where);
            return;
        }
        const f32 L = options_.lengthScale;
        m3::RibbonEmitter r = RestRibbon();
        r.boneIndex = static_cast<u16>(bone);
        // The editor reads these two u16s as ONE u32 and resolves it with no
        // range check (`bone = bones + 160 * u32`), so repeating the index
        // states 0x002F002F and walks 470 MB off the array -- an
        // ACCESS_VIOLATION the moment a ribbon's visibility is classified.
        // Every shipped RIB_ leaves the high half zero.
        r.boneIndexFallback = 0;

        // Rows 1-3: the width. StarCraft II's full width is half its size key.
        const f32 width = 2.0f * (rb.heightAbove + rb.heightBelow) * L;
        Rest(r.sizeAnimation, Vector3f{width, width, width});
        if (Keyed(rb.heightAboveTracks) || Keyed(rb.heightBelowTracks)) {
            const auto size = MergeTracks<f32, f32, Vector3f>(
                rb.heightAboveTracks, rb.heightAbove, rb.heightBelowTracks, rb.heightBelow,
                [L](f32 above, f32 below) {
                    const f32 w = 2.0f * (above + below) * L;
                    return Vector3f{w, w, w};
                });
            streams_.keyed(size, m3_sink::Stream::Sd3v, {&r.sizeAnimation});
        }
        // Row 3: the helper holds the rest difference; a difference that
        // animates slides the strip where StarCraft II cannot follow.
        if (Keyed(rb.heightAboveTracks) || Keyed(rb.heightBelowTracks)) {
            std::vector<u32> times = rb.heightAboveTracks.timestamps;
            times.insert(times.end(), rb.heightBelowTracks.timestamps.begin(),
                         rb.heightBelowTracks.timestamps.end());
            const f32 rest = rb.heightAbove - rb.heightBelow;
            const bool moves = std::any_of(times.begin(), times.end(), [&](u32 time) {
                const f32 t = static_cast<f32>(time);
                const f32 a = SampleTrack(rb.heightAboveTracks, t, rb.heightAbove);
                const f32 b = SampleTrack(rb.heightBelowTracks, t, rb.heightBelow);
                return std::fabs((a - b) - rest) > 2.0f * kRibbonOffsetEpsilon;
            });
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
        const u8 alpha = ColorByte(rb.alpha);
        const m3::ColorBGRA color = Color(rb.color, alpha);
        Rest(r.colorStart, color);
        Rest(r.colorMid, color);
        Rest(r.colorEnd, color);
        if (streams_.keyedColor(RedFirst(rb.colorTracks), rb.alphaTracks, rb.color, rb.alpha,
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
        r.gravity = -2.0f * rb.gravity * L;
        if (rb.gravity != 0.0f) {
            r.flags |= m3::RibbonFlag::AccurateGPUTangents;
        }

        // Row 9.
        if (rb.textureSlot != 0 || rb.rows > 1 || rb.columns > 1 || Keyed(rb.textureSlotTracks)) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "ribbon '" + name +
                                         "' picks a texture slot out of a sheet; StarCraft II "
                                         "draws its whole texture",
                                     where);
        }

        // Rows 10-11: the material.
        MaterialKey key;
        key.particle = false;
        if (rb.materialId < source_.materials.size() &&
            !source_.materials[rb.materialId].layers.empty()) {
            const mdx::Material& material = source_.materials[rb.materialId];
            const mdx::Layer& layer = material.layers.front();
            u32 textureId = layer.textureId;
            for (const mdx::Layer::SubTexture& sub : layer.subTextures) {
                if (sub.slot == mdx::Layer::SlotType::DiffuseMap) {
                    textureId = sub.textureId;
                    break;
                }
            }
            key.texture = TexturePath(options_, textureId);
            key.wrap = TextureWrap(source_, textureId);
            key.filter = LayerFilter(layer.filterMode);
            const u32 shading = static_cast<u32>(layer.shadingFlags);
            key.unshaded = (shading & static_cast<u32>(mdx::Layer::ShadingFlag::Unshaded)) != 0;
            key.unfogged = (shading & static_cast<u32>(mdx::Layer::ShadingFlag::Unfogged)) != 0;
            key.twoSided = (shading & static_cast<u32>(mdx::Layer::ShadingFlag::TwoSided)) != 0;
            key.priority = material.priorityPlane + 1;
            if (material.layers.size() > 1) {
                report_.diagnostics.warn(DiagCode::LayerDropped,
                                         "ribbon '" + name +
                                             "' draws a material of several layers; its first "
                                             "crosses",
                                         where);
            }
        }
        r.materialIndex = materials_.entry(key);
        out_.ribbonEmitters.push_back(std::move(r));
        ++report_.ribbonRecords;
    }

    const mdx::Model& source_;
    const wem::Document& staged_;
    const wem::M3ExportMap& map_;
    const Wc3EffectOptions& options_;
    m3::Model& out_;
    Streams& streams_;
    Materials& materials_;
    Wc3EffectReport& report_;
};

// ----------------------------------------------------------------------------
// Cameras
// ----------------------------------------------------------------------------

/// A Warcraft III point in the written model's basis and units.
Vector3f Restate(const Vector3f& v, f32 lengthScale) {
    return Vector3f{v.y * lengthScale, -v.x * lengthScale, v.z * lengthScale};
}

Vector3f Cross3(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

f32 Length3(const Vector3f& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// The bone frame a StarCraft II camera at @p position sees @p target from:
/// it looks down -Z, X lies level (world +Z up), Y = Z x X, and the whole
/// frame turns @p roll radians about Z. Blizzard's conversions state exactly
/// this -- Z on 398 of 400 portraits, the whole frame on 368, and the roll with
/// Warcraft III's sign on every rolled camera. Rows X, Y, Z and the position,
/// the way `ToMatrix` lays a transform out. False when the two points meet.
bool CameraFrame(const Vector3f& position, const Vector3f& target, f32 roll, Matrix44f& out) {
    const Vector3f toward{target.x - position.x, target.y - position.y, target.z - position.z};
    const f32 length = Length3(toward);
    if (!(length > 1e-6f)) {
        return false;
    }
    const Vector3f z{-toward.x / length, -toward.y / length, -toward.z / length};
    Vector3f x = Cross3(Vector3f{0, 0, 1}, z);
    if (Length3(x) < 1e-6f) {
        // Straight up or down: level has no direction, so +Y's cross stands in.
        x = Cross3(Vector3f{0, 1, 0}, z);
    }
    const f32 xl = Length3(x);
    x = Vector3f{x.x / xl, x.y / xl, x.z / xl};
    const Vector3f y = Cross3(z, x);
    const f32 c = std::cos(roll);
    const f32 s = std::sin(roll);
    const Vector3f rx{x.x * c + y.x * s, x.y * c + y.y * s, x.z * c + y.z * s};
    const Vector3f ry{y.x * c - x.x * s, y.y * c - x.y * s, y.z * c - x.z * s};
    out = Matrix44f::identity();
    const std::array<Vector3f, 4> rows{rx, ry, z, position};
    for (int r = 0; r < 4; ++r) {
        out.data[r][0] = rows[r].x;
        out.data[r][1] = rows[r].y;
        out.data[r][2] = rows[r].z;
    }
    return true;
}

class CameraCrossing {
public:
    CameraCrossing(const mdx::Model& source, const wem::Document& staged,
                   const wem::M3ExportMap& map, const Wc3EffectOptions& options, m3::Model& out,
                   Streams& streams, Wc3EffectReport& report)
        : source_(source), staged_(staged), map_(map), options_(options), out_(out),
          streams_(streams), report_(report) {}

    void run() {
        // `toM3` writes a CAM_ per camera node in node order, and the import
        // appends the `.mdx` cameras as the document's camera nodes in theirs.
        std::vector<u32> nodes;
        if (!staged_.models.empty()) {
            const wem::NodeTree& tree = staged_.models.front().nodes;
            for (u32 n = 0; n < tree.size(); ++n) {
                if (tree.nodes[n].kind == wem::NodeKind::Camera) {
                    nodes.push_back(n);
                }
            }
        }
        for (std::size_t c = 0; c < source_.cameras.size(); ++c) {
            if (c >= nodes.size() || c >= out_.cameras.size()) {
                break;
            }
            cross(source_.cameras[c], nodes[c], out_.cameras[c]);
        }
    }

private:
    void cross(const mdx::Camera& camera, u32 node, m3::Camera& record) {
        const f32 L = options_.lengthScale;
        const ElementRef where(ElementKind::Node, node);

        // Warcraft III states a horizontal field of view over a 4:3 frame;
        // Blizzard's conversions write the vertical one it implies, and say so.
        Rest(record.fieldOfView, 2.0f * std::atan(0.75f * std::tan(camera.fieldOfView * 0.5f)));
        record.useVerticalFOV = 1;
        Rest(record.nearClip, camera.nearClippingPlane * L);
        Rest(record.farClip, camera.farClippingPlane * L);
        // The depth-of-field rests Blizzard's tool writes on every one of them,
        // in the written model's units.
        Rest(record.shadowClipDistance, 20.0f);
        Rest(record.focusDistance, 5.0f * L);
        Rest(record.farFocusRange, 1.0f * L);
        Rest(record.nearFocusRange, 2.0f * L);

        const u32 bone = node < map_.nodeBone.size() ? map_.nodeBone[node] : kInvalidIndex;
        if (bone == kInvalidIndex || bone >= out_.bones.size() || record.boneIndex != bone ||
            out_.bones[bone].parentIndex != 0xFFFFu) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "camera '" + camera.name +
                                         "' has no bone of its own; it looks down the one it "
                                         "rides, not at its target",
                                     where);
            return;
        }
        m3::Bone& carrier = out_.bones[bone];

        // At rest: the bone `toM3` placed, turned toward the target.
        const Vector3f position = carrier.position.initValue;
        Matrix44f frame;
        if (!CameraFrame(position, Restate(camera.targetPosition, L), 0.0f, frame)) {
            report_.diagnostics.warn(DiagCode::FeatureDropped,
                                     "camera '" + camera.name +
                                         "' sits on its own target; it keeps the bone's rotation",
                                     where);
            return;
        }
        const wem::Transform rest = wem::FromMatrix(frame);
        Rest(carrier.rotation, rest.rotation);
        if (bone < out_.initialReference.size()) {
            out_.initialReference[bone].matrix = Matrix44f::inverse(frame);
        }
        ++report_.cameraRecords;

        // Moving: the frame sampled on the union of the keys that move either
        // end or the roll, one rotation key each.
        u32 clock = mdx_slice::kNoGlobalSequence;
        bool clockSet = false;
        bool approximated = false;
        std::vector<u32> times;
        const auto gather = [&](const auto& track) {
            if (!Keyed(track) || !mdx_slice::WellFormed(track)) {
                return false;
            }
            if (clockSet && track.globalSequenceId != clock) {
                approximated = true;
                return false;
            }
            clock = track.globalSequenceId;
            clockSet = true;
            times.insert(times.end(), track.timestamps.begin(), track.timestamps.end());
            return true;
        };
        const bool usePosition = gather(camera.positionTracks);
        const bool useTarget = gather(camera.targetPositionTracks);
        const bool useRoll = gather(camera.targetRotationTracks);
        if (approximated) {
            report_.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                     "camera '" + camera.name +
                                         "' keys its position, target and roll on different "
                                         "clocks; the aim follows the first",
                                     where);
        }
        if (times.empty()) {
            return;
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        const auto stepped = [](bool used, const auto& track) {
            return !used || track.interpolationType == mdx::InterpolationType::None;
        };

        mdx::Track<Quaternion> aim;
        aim.isUsed = true;
        aim.globalSequenceId = clock;
        aim.interpolationType = stepped(usePosition, camera.positionTracks) &&
                                        stepped(useTarget, camera.targetPositionTracks) &&
                                        stepped(useRoll, camera.targetRotationTracks)
                                    ? mdx::InterpolationType::None
                                    : mdx::InterpolationType::Linear;
        const Vector3f still{0, 0, 0};
        for (const u32 time : times) {
            const f32 at = static_cast<f32>(time);
            // KCTR and KTTR move each end off its rest; KCRL is the roll itself.
            const Vector3f moved = usePosition ? SampleTrack(camera.positionTracks, at, still) : still;
            const Vector3f aimed =
                useTarget ? SampleTrack(camera.targetPositionTracks, at, still) : still;
            const f32 roll = useRoll ? SampleTrack(camera.targetRotationTracks, at, 0.0f) : 0.0f;
            Matrix44f keyFrame;
            const bool framed = CameraFrame(Restate(camera.position + moved, L),
                                            Restate(camera.targetPosition + aimed, L), roll,
                                            keyFrame);
            aim.timestamps.push_back(time);
            aim.keys_data.push_back(framed ? wem::FromMatrix(keyFrame).rotation : rest.rotation);
        }
        aim.keyCount = aim.timestamps.size();
        // WEM holds no camera target, so this is the one place a crossing
        // turns a bone `toM3` made: the rest, its IREF and its rotation stream.
        // The file latches its bone flags as solved, so a newly keyed bone is
        // solved again or the engine plays it frozen.
        if (streams_.keyed(aim, m3_sink::Stream::Sd4q, {&carrier.rotation},
                           &carrier.rotation.initValue)) {
            wem::m3_anim::SolveBoneAnimFlags(out_);
        }
    }

    const mdx::Model& source_;
    const wem::Document& staged_;
    const wem::M3ExportMap& map_;
    const Wc3EffectOptions& options_;
    m3::Model& out_;
    Streams& streams_;
    Wc3EffectReport& report_;
};

// ----------------------------------------------------------------------------
// CLID -> MODL fuzzy hit tests (C8.2)
// ----------------------------------------------------------------------------

/// Each Warcraft III collision shape as the fuzzy hit test StarCraft II picks a
/// model with, on the shape node's own bone, which rests at its pivot. A
/// sphere states its centre in model space and a box its corners about the
/// pivot (AzureDragon's three boxes all centre on (0, 0, z), one per pivot).
/// Blizzard's conversions put 523 of 550 sphere tests on a bone at the centre
/// sized by the radius (532), and 151 of 174 box tests on a bone at the pivot
/// with sizes scaled by hand in Max. An M3 box's sizes are half extents.
void CrossHitTests(const mdx::Model& source, const wem::Document& staged,
                   const wem::M3ExportMap& map, const Wc3EffectOptions& options, m3::Model& out,
                   Wc3EffectReport& report) {
    const f32 L = options.lengthScale;
    for (const mdx::CollisionShape& shape : source.collisionShapes) {
        const std::string& name = shape.node.name;
        const ElementRef where(ElementKind::Node, shape.node.objectId);
        const u32 node = NodeOfObjectId(staged, shape.node.objectId);
        const u32 bone = node < map.nodeBone.size() ? map.nodeBone[node] : kInvalidIndex;
        if (bone == kInvalidIndex || bone >= out.bones.size()) {
            report.diagnostics.warn(DiagCode::FeatureDropped,
                                    "collision shape '" + name +
                                        "' has no bone to ride; its hit test was not written",
                                    where);
            continue;
        }
        const Vector3f pivot = shape.node.objectId < source.pivotPoints.size()
                                   ? source.pivotPoints[shape.node.objectId]
                                   : Vector3f{0, 0, 0};
        m3::HitTestShape hit;
        hit.boneIndex = static_cast<u16>(bone);
        Vector3f offset{0, 0, 0};
        if (shape.type == mdx::CollisionShape::ShapeType::Sphere && !shape.vertices.empty()) {
            hit.shapeType = m3::HitTestShapeType::Sphere;
            hit.sizeX = shape.radius * L;
            offset = Vector3f{shape.vertices[0].x - pivot.x, shape.vertices[0].y - pivot.y,
                              shape.vertices[0].z - pivot.z};
        } else if (shape.type == mdx::CollisionShape::ShapeType::Box &&
                   shape.vertices.size() >= 2) {
            const Vector3f& a = shape.vertices[0];
            const Vector3f& b = shape.vertices[1];
            hit.shapeType = m3::HitTestShapeType::Box;
            // The basis change swaps the two ground axes.
            hit.sizeX = 0.5f * std::fabs(b.y - a.y) * L;
            hit.sizeY = 0.5f * std::fabs(b.x - a.x) * L;
            hit.sizeZ = 0.5f * std::fabs(b.z - a.z) * L;
            offset = Vector3f{0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z)};
        } else {
            report.diagnostics.warn(DiagCode::FeatureDropped,
                                    "collision shape '" + name +
                                        "' is not a sphere or a box; no hit test was written",
                                    where);
            continue;
        }
        const Vector3f at = Restate(offset, L);
        hit.transform.data[3][0] = at.x;
        hit.transform.data[3][1] = at.y;
        hit.transform.data[3][2] = at.z;
        out.fuzzyHitTestObjects.push_back(std::move(hit));
        ++report.hitTests;
    }
}

} // namespace

void PrepareWc3Effects(const mdx::Model& source, wem::Document& document,
                       wem::Diagnostics& diagnostics, bool ribbonOffsets) {
    if (document.models.empty()) {
        return;
    }
    wem::Model& model = document.models.front();
    // Warcraft III's hierarchy composes every node onto its parent whatever
    // bit 0x80000 says (`MdxHierarchy`, and Blizzard's own conversions keep an
    // emitter under the bone it was linked to: War3_Wisp's SC2Particles001).
    // Only the particle system reads the bit.
    for (wem::Node& node : model.nodes.nodes) {
        node.flags = static_cast<wem::NodeFlags>(static_cast<u32>(node.flags) &
                                                 ~static_cast<u32>(wem::NodeFlags::ModelSpace));
    }
    // §A9: a squirting emitter's bursts ignore its visibility in Warcraft III
    // and would obey the bone's in StarCraft II, so the track that would hide
    // them does not cross.
    for (const mdx::ParticleEmitter2& pe : source.particleEmitters2) {
        if (pe.squirt == 0 || !Keyed(pe.visibilityTracks)) {
            continue;
        }
        const u32 node = NodeOfObjectId(document, pe.node.objectId);
        if (node == kInvalidIndex) {
            continue;
        }
        std::vector<u32> dropped;
        auto& channels = model.animChannels.channels;
        for (auto it = channels.begin(); it != channels.end();) {
            if (it->target.kind == wem::TrackTarget::Kind::Node && it->target.node == node &&
                it->target.channel == wem::Channel::Visibility) {
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
                         "particle emitter '" + pe.node.name +
                             "' squirts; its bursts ignore its visibility in Warcraft III, so the "
                             "track that would hide them in StarCraft II is not carried",
                         ElementRef(ElementKind::Node, node));
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
    for (const mdx::RibbonEmitter& rb : source.ribbonEmitters) {
        const f32 offset = 0.5f * (rb.heightAbove - rb.heightBelow);
        const u32 node = NodeOfObjectId(document, rb.node.objectId);
        if (!(std::fabs(offset) > kRibbonOffsetEpsilon) || node == kInvalidIndex) {
            continue;
        }
        wem::Node helper;
        helper.name = model.nodes.nodes[node].name + "_Offset";
        helper.kind = wem::NodeKind::Helper;
        helper.resetPayloadForKind();
        helper.parent = node;
        const Vector3f& pivot = model.nodes.nodes[node].pivot;
        helper.pivot = Vector3f{pivot.x, pivot.y + offset, pivot.z};
        helper.local.translation = Vector3f{0.0f, offset, 0.0f};
        helper.poses.assign(model.nodes.poseSchema.size(), helper.local);
        helper.native.set(kRibbonOffsetOf, static_cast<i64>(rb.node.objectId));
        const u32 index = model.nodes.add(std::move(helper));
        for (wem::AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind == wem::TrackTarget::Kind::Node &&
                channel.target.node == node && channel.target.channel == wem::Channel::Visibility) {
                channel.target.node = index;
            }
        }
        diagnostics.info(DiagCode::RigConventionChanged,
                         "ribbon emitter '" + rb.node.name + "' rides a helper " +
                             std::to_string(offset) +
                             " units across its width: StarCraft II centres the strip",
                         ElementRef(ElementKind::Node, index));
    }
}

Wc3EffectReport CrossWc3Effects(const mdx::Model& source, const wem::Document& staged,
                                const wem::M3ExportMap& map, const Wc3EffectOptions& options,
                                m3::Model& out) {
    Wc3EffectReport report;
    Streams streams(staged, map, out, report);
    Materials materials(out, report);
    if (options.particles) {
        ParticleCrossing(source, staged, map, options, out, streams, materials, report).run();
        ModelParticleCrossing(source, staged, map, options, out, streams, materials, report)
            .run();
    }
    if (options.ribbons) {
        RibbonCrossing(source, staged, map, options, out, streams, materials, report).run();
    }
    if (options.cameras) {
        CameraCrossing(source, staged, map, options, out, streams, report).run();
    }
    if (options.hitTests) {
        CrossHitTests(source, staged, map, options, out, report);
    }
    // C8.3: Warcraft III's event objects are sound, splat and footprint
    // triggers the game looks up by name in its SLKs. They cross as named event
    // keys (the timing survives), but StarCraft II plays sounds and splats from
    // actor data, so they fire nothing on their own; Blizzard's conversions
    // dropped every one (0 of 912 models keep a source event).
    if (!source.eventObjects.empty()) {
        report.diagnostics.info(DiagCode::FeatureDropped,
                                std::to_string(source.eventObjects.size()) +
                                    " event object(s) (EVTS) crossed as named model events; "
                                    "StarCraft II plays sounds and splats from actor data",
                                ElementRef());
    }
    // C8.4: a Reforged model's PopcornFX systems live in `.pkfx` files no M3
    // record can name.
    if (!source.cornEmitters.empty()) {
        report.diagnostics.warn(DiagCode::FeatureDropped,
                                std::to_string(source.cornEmitters.size()) +
                                    " PopcornFX emitter(s) (CORN) did not cross",
                                ElementRef());
    }
    // A head and a tail record, or a split's billboard half, are copies of one
    // record and read its streams.
    m3_sink::UnshareAnimIds(out);
    return report;
}

} // namespace cross
} // namespace models
} // namespace whiteout

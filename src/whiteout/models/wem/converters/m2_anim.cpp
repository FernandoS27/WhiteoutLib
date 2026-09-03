// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m2_anim.h"

#include <cstring>
#include <string>
#include <variant>

#include <whiteout/models/wem/anim/clip.h>
#include "../../m2/legacy.h"
#include <whiteout/models/wem/native/m2_tables.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m2_anim {

namespace {

/// Whether an extent says anything. A default-constructed one is all zeros, and
/// `Extent::valid()` answers "min <= max", which all zeros satisfies -- so the
/// question a caller means by "did the source give me one" is volume, not
/// validity.
bool HasExtent(const Extent& extent) {
    return extent.sphereRadius > 0.0f || extent.maximum.x > extent.minimum.x ||
           extent.maximum.y > extent.minimum.y || extent.maximum.z > extent.minimum.z;
}

constexpr u16 kNoGlobalSequence = 0xFFFF;

constexpr f32 Seconds(f32 milliseconds) {
    return milliseconds / 1000.0f;
}

/// How one source value becomes the bytes a channel holds.
///
/// The `i16` case is the one worth naming: WoW stores weights and colour
/// components as `x / 32767`, and the `u8` beside it is a flag rather than a
/// byte to scale.
template <class T>
void AppendBytes(const T& value, std::vector<u8>& out) {
    const u8* raw = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), raw, raw + sizeof(T));
}

void Append(const Vector3f& value, std::vector<u8>& out) {
    AppendBytes(value, out);
}
void Append(const Quaternion& value, std::vector<u8>& out) {
    AppendBytes(value, out);
}
void Append(const m2::CompatQuaternion& value, std::vector<u8>& out) {
    // `decompressQuat`, NOT `CompatQuaternion::operator Quaternion`. The two
    // disagree: the operator reads x/y/z as a plain snorm16 and w as a unorm16,
    // while a `.m2` compressed quaternion is WoW's own biased encoding —
    // `(v < 0 ? v + 32768 : v - 32767) / 32767` on all four — which is what the
    // writer half of this file already round-trips through and what the renderer
    // decodes with. Off by that bias every component lands about a unit away, so
    // every key comes back with |q| near 2 and a chain of them multiplies
    // magnitudes by four per level: a fourteen-deep WoW arm ended 32 units out.
    const Vector4f decoded = m2::decompressQuat(value);
    AppendBytes(Quaternion{decoded.x, decoded.y, decoded.z, decoded.w}, out);
}
void Append(f32 value, std::vector<u8>& out) {
    AppendBytes(value, out);
}
void Append(i16 value, std::vector<u8>& out) {
    const f32 scaled = static_cast<f32>(value) / 32767.0f;
    AppendBytes(scaled, out);
}
void Append(u8 value, std::vector<u8>& out) {
    // A flag, not a byte: `animate_attached` and a light's visibility are 0 or 1.
    const f32 flag = value != 0 ? 1.0f : 0.0f;
    AppendBytes(flag, out);
}
void Append(u16 value, std::vector<u8>& out) {
    const u32 widened = value;
    AppendBytes(widened, out);
}
void Append(const m2::CameraSpline& value, std::vector<u8>& out) {
    // Value, in-tangent, out-tangent — the same three-wide key MDX's Hermite
    // tracks use, which is why the sub-track carries `Interpolation::Hermite`.
    AppendBytes(value.value, out);
    AppendBytes(value.inTangent, out);
    AppendBytes(value.outTangent, out);
}

template <class T>
struct ValueTrait;
template <>
struct ValueTrait<Vector3f> {
    static constexpr geom::AttrType kType = geom::AttrType::F32x3;
    static constexpr bool kSpline = false;
};
template <>
struct ValueTrait<Quaternion> {
    static constexpr geom::AttrType kType = geom::AttrType::Quat;
    static constexpr bool kSpline = false;
};
template <>
struct ValueTrait<m2::CompatQuaternion> {
    static constexpr geom::AttrType kType = geom::AttrType::Quat;
    static constexpr bool kSpline = false;
};
template <>
struct ValueTrait<f32> {
    static constexpr geom::AttrType kType = geom::AttrType::F32;
    static constexpr bool kSpline = false;
};
template <>
struct ValueTrait<i16> {
    static constexpr geom::AttrType kType = geom::AttrType::F32;
    static constexpr bool kSpline = false;
};
template <>
struct ValueTrait<u8> {
    static constexpr geom::AttrType kType = geom::AttrType::F32;
    static constexpr bool kSpline = false;
};
template <>
struct ValueTrait<u16> {
    static constexpr geom::AttrType kType = geom::AttrType::U32;
    static constexpr bool kSpline = false;
};
template <>
struct ValueTrait<m2::CameraSpline> {
    static constexpr geom::AttrType kType = geom::AttrType::F32x3;
    static constexpr bool kSpline = true;
};

Interpolation InterpOf(m2::InterpolationType type, geom::AttrType valueType, bool spline) {
    if (spline) {
        return Interpolation::Hermite;
    }
    switch (type) {
    case m2::InterpolationType::None:
        return Interpolation::Step;
    case m2::InterpolationType::Linear:
        return valueType == geom::AttrType::Quat ? Interpolation::Slerp : Interpolation::Linear;
    case m2::InterpolationType::Hermite:
        return Interpolation::Hermite;
    case m2::InterpolationType::Bezier:
        return Interpolation::Bezier;
    }
    return Interpolation::Linear;
}

class Builder {
public:
    Builder(const m2::Model& source, const Context& context, Document& document, u32 modelIndex,
            Diagnostics& out)
        : source_(source), context_(context), document_(document),
          model_(document.models[modelIndex]), modelIndex_(modelIndex), out_(out) {}

    void run() {
        buildSequenceClips();
        addBoneTracks();
        addSatelliteTracks();
        addMaterialTracks();
        addEvents();
        for (Clip& clip : clips_) {
            document_.clips.push_back(std::move(clip));
        }
    }

private:
    static SubTrackContainer baseContainer() {
        SubTrackContainer container;
        container.name = "base";
        return container;
    }

    void buildSequenceClips() {
        for (std::size_t i = 0; i < source_.sequences.size(); ++i) {
            const m2::Sequence& sequence = source_.sequences[i];
            Clip clip;
            clip.name = "sequence_" + std::to_string(sequence.id) + "_" +
                        std::to_string(sequence.variationIndex);
            clip.model = modelIndex_;
            clip.duration = Seconds(static_cast<f32>(sequence.duration));
            clip.looping = m2::hasFlag(sequence.flags, m2::SequenceFlag::Looping);
            clip.native.set("animationId", static_cast<i64>(sequence.id));
            clip.native.set("variationIndex", static_cast<i64>(sequence.variationIndex));
            clip.native.set("flagBits", static_cast<i64>(static_cast<u32>(sequence.flags)));
            clip.native.set("movespeed", sequence.movespeed);
            clip.native.set("frequency", static_cast<i64>(sequence.frequency));
            clip.native.set("blendTimeIn", static_cast<i64>(sequence.blendTimeIn));
            clip.native.set("blendTimeOut", static_cast<i64>(sequence.blendTimeOut));
            // The chains §10.8.3 sends here: following one is playback policy,
            // and WEM does not own the blender.
            clip.native.set("variationNext", static_cast<i64>(sequence.variationNext));
            clip.native.set("aliasNext", static_cast<i64>(sequence.aliasNext));
            // The sequence's own bound: the posed model over this clip, which
            // nothing in the file lets a reader derive back.
            clip.bounds.minimum = sequence.bounding.minimum;
            clip.bounds.maximum = sequence.bounding.maximum;
            clip.bounds.sphereRadius = sequence.bounding.sphereRadius;
            clip.containers.push_back(baseContainer());
            clips_.push_back(std::move(clip));
        }
    }

    /// The auto-play clip for global loop @p index, made on first use.
    u32 globalClipFor(u16 index) {
        for (const auto& entry : globalClips_) {
            if (entry.first == index) {
                return entry.second;
            }
        }
        if (index >= source_.globalLoops.size()) {
            return kInvalidIndex;
        }
        Clip clip;
        clip.name = "globalLoop_" + std::to_string(index);
        clip.model = modelIndex_;
        clip.duration = Seconds(static_cast<f32>(source_.globalLoops[index].timestamp));
        clip.looping = true;
        clip.flags = ClipFlags::AutoPlay | ClipFlags::WorldClocked;
        clip.native.set("globalLoop", static_cast<i64>(index));
        clip.containers.push_back(baseContainer());

        const u32 slot = static_cast<u32>(clips_.size());
        clips_.push_back(std::move(clip));
        globalClips_.emplace_back(index, slot);
        return slot;
    }

    template <class T>
    void addTrack(const m2::AnimationTrack<T>& track, const TrackTarget& target) {
        if (track.timestamps.empty()) {
            return;
        }
        constexpr geom::AttrType kType = ValueTrait<T>::kType;
        const Interpolation interp =
            InterpOf(track.interpolationType, kType, ValueTrait<T>::kSpline);
        const u32 id = model_.animChannels.nextFreeId();
        bool used = false;

        if (track.globalSequenceId != kNoGlobalSequence) {
            const u32 clip = globalClipFor(track.globalSequenceId);
            if (clip == kInvalidIndex) {
                out_.warn(DiagCode::AnimTrackDropped,
                          "a track names global loop " + std::to_string(track.globalSequenceId) +
                              ", which the model lacks",
                          ElementRef(), ProfileId::Wow);
                return;
            }
            used = emit(track, 0, interp, id, clips_[clip]);
        } else {
            for (std::size_t s = 0; s < clips_.size() && s < source_.sequences.size(); ++s) {
                used = emit(track, s, interp, id, clips_[s]) || used;
            }
        }

        if (!used) {
            return;
        }
        AnimChannel channel;
        channel.id = id;
        channel.target = target;
        channel.valueType = kType;
        model_.animChannels.add(channel);
    }

    template <class T>
    static bool emit(const m2::AnimationTrack<T>& track, std::size_t index, Interpolation interp,
                     u32 id, Clip& clip) {
        if (index >= track.timestamps.size() || index >= track.values.size()) {
            return false;
        }
        const std::vector<u32>& times = track.timestamps[index];
        const std::vector<T>& values = track.values[index];
        if (times.empty() || values.size() < times.size()) {
            return false;
        }
        SubTrack sub;
        sub.channel = id;
        sub.interp = interp;
        sub.times.reserve(times.size());
        for (std::size_t k = 0; k < times.size(); ++k) {
            sub.times.push_back(Seconds(static_cast<f32>(times[k])));
            Append(values[k], sub.values);
        }
        clip.containers[0].subTracks.push_back(std::move(sub));
        return true;
    }

    static TrackTarget nodeTarget(u32 node, Channel channel, u32 sub = 0) {
        TrackTarget target;
        target.kind = TrackTarget::Kind::Node;
        target.node = node;
        target.channel = channel;
        target.sub = sub;
        return target;
    }

    TrackTarget materialTarget(u32 slot, Channel channel, u32 ordinal) {
        TrackTarget target;
        target.kind = TrackTarget::Kind::MaterialLayer;
        target.material.profile = ProfileId::Wow;
        target.material.slot = slot;
        target.material.look = 0;
        target.sub = ordinal;
        target.channel = channel;
        return target;
    }

    void addBoneTracks() {
        for (std::size_t b = 0; b < source_.bones.size(); ++b) {
            const u32 node = context_.bases.bone + static_cast<u32>(b);
            if (node >= model_.nodes.size()) {
                continue;
            }
            addTrack(source_.bones[b].translation, nodeTarget(node, Channel::Translation));
            addTrack(source_.bones[b].rotation, nodeTarget(node, Channel::Rotation));
            addTrack(source_.bones[b].scale, nodeTarget(node, Channel::Scale));
        }
    }

    void addSatelliteTracks() {
        for (std::size_t a = 0; a < source_.attachments.size(); ++a) {
            const u32 node = context_.bases.attachment + static_cast<u32>(a);
            if (node < model_.nodes.size()) {
                addTrack(source_.attachments[a].animate, nodeTarget(node, Channel::Visibility));
            }
        }
        for (std::size_t l = 0; l < source_.lights.size(); ++l) {
            const u32 node = context_.bases.light + static_cast<u32>(l);
            if (node >= model_.nodes.size()) {
                continue;
            }
            const m2::Light& light = source_.lights[l];
            addTrack(light.diffuseColor, nodeTarget(node, Channel::Color));
            addTrack(light.diffuseIntensity, nodeTarget(node, Channel::Intensity));
            // Sub 1 is the ambient term — the second colour and intensity a WoW
            // light carries beside its diffuse pair.
            addTrack(light.ambientColor, nodeTarget(node, Channel::Color, 1));
            addTrack(light.ambientIntensity, nodeTarget(node, Channel::Intensity, 1));
            addTrack(light.attenuationStart, nodeTarget(node, Channel::AttenuationStart));
            addTrack(light.attenuationEnd, nodeTarget(node, Channel::AttenuationEnd));
            addTrack(light.visibility, nodeTarget(node, Channel::Visibility));
        }
        for (std::size_t r = 0; r < source_.ribbonEmitters.size(); ++r) {
            const u32 node = context_.bases.ribbon + static_cast<u32>(r);
            if (node >= model_.nodes.size()) {
                continue;
            }
            const m2::RibbonEmitter& ribbon = source_.ribbonEmitters[r];
            addTrack(ribbon.colorTrack, nodeTarget(node, Channel::Color));
            addTrack(ribbon.alphaTrack, nodeTarget(node, Channel::Alpha));
            addTrack(ribbon.texSlot, nodeTarget(node, Channel::TextureIndex));
            addTrack(ribbon.visibility, nodeTarget(node, Channel::Visibility));
            // `heightAbove` / `heightBelow` shape the ribbon itself, which §18
            // keeps out of WEM along with the rest of the system.
        }
        for (std::size_t c = 0; c < source_.cameras.size(); ++c) {
            const u32 node = context_.bases.camera + static_cast<u32>(c);
            if (node < model_.nodes.size()) {
                addTrack(source_.cameras[c].positions, nodeTarget(node, Channel::Translation));
            }
        }
    }

    /// The three material tracks, reached through the resolved native block.
    void addMaterialTracks() {
        ProfileMaterialSet* set = model_.setFor(ProfileId::Wow);
        if (set == nullptr) {
            return;
        }
        for (std::size_t s = 0; s < source_.skinProfiles.size(); ++s) {
            if (s >= context_.slotOfBatch.size()) {
                break;
            }
            const m2::SkinProfile& skin = source_.skinProfiles[s];
            for (std::size_t b = 0; b < skin.batches.size(); ++b) {
                if (b >= context_.slotOfBatch[s].size()) {
                    break;
                }
                const u32 slot = context_.slotOfBatch[s][b];
                if (slot >= set->slotBindings.size() || set->slotBindings[slot].byLook.empty()) {
                    continue;
                }
                const u32 material = set->slotBindings[slot].byLook[0];
                if (material >= set->materials.size()) {
                    continue;
                }
                addBatchTracks(skin.batches[b], slot, set->materials[material]);
            }
        }
    }

    void addBatchTracks(const m2::Batch& batch, u32 slot, Material& material) {
        // `M2Color` multiplies the whole batch rather than one stage, and its
        // index is signed: -1 is "no colour", not colour 65535.
        if (batch.colorIndex >= 0 &&
            static_cast<std::size_t>(batch.colorIndex) < source_.colors.size()) {
            const m2::ColorAnimation& color =
                source_.colors[static_cast<std::size_t>(batch.colorIndex)];
            addTrack(color.color, materialTarget(slot, Channel::Color, kWholeMaterial));
            addTrack(color.alpha, materialTarget(slot, Channel::Alpha, kWholeMaterial));
        }

        const native::M2Material* block = std::get_if<native::M2Material>(&material.Native());
        if (block == nullptr) {
            return;
        }
        // The block keeps every resolved unit; the material keeps only the
        // stages its `shaderId`'s chain reads, and `m2_core` already reports the
        // rest as `LayerDropped`. A track on a dropped unit has no ordinal to
        // name — measured on the corpus, where blood elves key three of them.
        const std::size_t stages = material.Common().ordinalCount();
        for (std::size_t unit = 0; unit < block->units.size(); ++unit) {
            if (unit >= stages) {
                if (source_.textureWeights.size() > block->units[unit].weight ||
                    source_.textureTransforms.size() > block->units[unit].transform) {
                    out_.info(DiagCode::AnimTrackDropped,
                              "unit " + std::to_string(unit) + " is past the " +
                                  std::to_string(stages) + " stages this shader reads",
                              ElementRef(ElementKind::Slot, slot), ProfileId::Wow);
                }
                continue;
            }
            const native::M2TextureUnit& resolved = block->units[unit];
            const u32 ordinal = static_cast<u32>(unit);

            if (resolved.weight < source_.textureWeights.size()) {
                addTrack(source_.textureWeights[resolved.weight].weight,
                         materialTarget(slot, Channel::Alpha, ordinal));
            }
            if (resolved.transform >= source_.textureTransforms.size()) {
                continue;
            }
            const m2::TextureTransform& transform = source_.textureTransforms[resolved.transform];
            const u32 feature = FeatureIdFor(material, ordinal);
            TrackTarget target;
            target.kind = TrackTarget::Kind::MaterialFeature;
            target.material.profile = ProfileId::Wow;
            target.material.slot = slot;
            target.material.look = 0;
            target.sub = feature;

            target.channel = Channel::UvTranslate;
            addTrack(transform.translation, target);
            target.channel = Channel::UvRotate;
            addTrack(transform.rotation, target);
            target.channel = Channel::UvScale;
            addTrack(transform.scaling, target);
        }
    }

    /// The `UvAnimation` feature on @p ordinal, created if absent. `InitCommon`
    /// rather than `MutableCommon`: this is still the import deriving common
    /// from native, which is the one edit that does not make the block stale.
    static u32 FeatureIdFor(Material& material, u32 ordinal) {
        CommonMaterial& common = material.InitCommon();
        for (const MaterialFeature& feature : common.features) {
            if (feature.kind() == FeatureKind::UvAnimation && feature.layer == ordinal) {
                return feature.id;
            }
        }
        MaterialFeature feature;
        feature.id = NextFeatureId(common.features);
        feature.layer = ordinal;
        feature.payload = UvAnimationFeature{};
        common.features.push_back(feature);
        return feature.id;
    }

    /// An `.m2` event carries timestamps and no values — the node is the where,
    /// the key is the when (§10.8).
    void addEvents() {
        for (std::size_t e = 0; e < source_.events.size(); ++e) {
            const u32 node = context_.bases.event + static_cast<u32>(e);
            if (node >= model_.nodes.size()) {
                continue;
            }
            const m2::Event& event = source_.events[e];
            const std::string name = model_.nodes.nodes[node].name;

            if (event.enabled.globalSequenceId != kNoGlobalSequence) {
                const u32 clip = globalClipFor(event.enabled.globalSequenceId);
                if (clip != kInvalidIndex && !event.enabled.timestamps.empty()) {
                    pushEvents(clips_[clip], event.enabled.timestamps[0], node, name,
                               event.identifier);
                }
                continue;
            }
            for (std::size_t s = 0; s < event.enabled.timestamps.size() && s < clips_.size(); ++s) {
                pushEvents(clips_[s], event.enabled.timestamps[s], node, name, event.identifier);
            }
        }
    }

    static void pushEvents(Clip& clip, const std::vector<u32>& times, u32 node,
                           const std::string& name, u32 identifier) {
        for (u32 time : times) {
            clip.events.push_back(
                ClipEvent{Seconds(static_cast<f32>(time)), node, name, identifier});
        }
    }

    const m2::Model& source_;
    const Context& context_;
    Document& document_;
    Model& model_;
    u32 modelIndex_;
    Diagnostics& out_;

    std::vector<Clip> clips_;
    std::vector<std::pair<u16, u32>> globalClips_;
};

} // namespace

NodeBases NodeBases::Of(const m2::Model& source) {
    NodeBases bases;
    bases.bone = 0;
    bases.attachment = static_cast<u32>(source.bones.size());
    bases.light = bases.attachment + static_cast<u32>(source.attachments.size());
    bases.event = bases.light + static_cast<u32>(source.lights.size());
    bases.ribbon = bases.event + static_cast<u32>(source.events.size());
    bases.camera = bases.ribbon + static_cast<u32>(source.ribbonEmitters.size());
    return bases;
}

void Import(const m2::Model& source, const Context& context, Document& document, u32 model,
            Diagnostics& out) {
    if (model >= document.models.size()) {
        return;
    }
    Builder(source, context, document, model, out).run();
}

// ============================================================================
// Export — clips back into `.m2`'s per-sequence tracks (§10.8.3)
//
// The easy direction, for the reason the import was easy: an `.m2` track is
// already one inner array per sequence, so a clip IS an inner array and there
// is nothing to slice or merge. What takes the work is the other end — where
// each track has to be written back to.
//
// Bones, attachments, lights, events and cameras are reached through the node
// map `toM2` filled. The three material tracks are reached through the native
// block, which keeps the RESOLVED weight, transform and colour indices — the
// same indices `appendCombos` writes back into the combo tables — so putting a
// track at `textureWeights[block.units[ordinal].weight]` is all the join needs.
// ============================================================================

namespace {

u32 MillisecondsOf(f32 seconds) {
    const f32 ms = seconds * 1000.0f;
    return ms <= 0.0f ? 0u : static_cast<u32>(ms + 0.5f);
}

m2::InterpolationType M2Interp(Interpolation interp) {
    switch (interp) {
    case Interpolation::Step:
        return m2::InterpolationType::None;
    case Interpolation::Hermite:
        return m2::InterpolationType::Hermite;
    case Interpolation::Bezier:
        return m2::InterpolationType::Bezier;
    case Interpolation::Linear:
    case Interpolation::Slerp:
    case Interpolation::Count:
        break;
    }
    return m2::InterpolationType::Linear;
}

/// The inverse of the `Append` overloads above: WEM holds every one of these as
/// `f32` or `u32`, and the destination type decides how it goes back.
template <class T>
struct Decoder;
template <>
struct Decoder<Vector3f> {
    static Vector3f From(const u8* raw) {
        Vector3f value{};
        std::memcpy(&value, raw, sizeof(value));
        return value;
    }
};
template <>
struct Decoder<Quaternion> {
    static Quaternion From(const u8* raw) {
        Quaternion value{};
        std::memcpy(&value, raw, sizeof(value));
        return value;
    }
};
template <>
struct Decoder<m2::CompatQuaternion> {
    static m2::CompatQuaternion From(const u8* raw) {
        Quaternion value{};
        std::memcpy(&value, raw, sizeof(value));
        // The inverse of `Append`'s `decompressQuat`, for the same reason.
        return m2::compressQuat(Vector4f{value.x, value.y, value.z, value.w});
    }
};
template <>
struct Decoder<f32> {
    static f32 From(const u8* raw) {
        f32 value = 0.0f;
        std::memcpy(&value, raw, sizeof(value));
        return value;
    }
};
template <>
struct Decoder<i16> {
    /// `x / 32767` on the way in, so `x * 32767` on the way out.
    static i16 From(const u8* raw) {
        f32 value = 0.0f;
        std::memcpy(&value, raw, sizeof(value));
        const f32 scaled = value * 32767.0f;
        return static_cast<i16>(scaled < -32767.0f  ? -32767.0f
                                : scaled > 32767.0f ? 32767.0f
                                                    : scaled);
    }
};
template <>
struct Decoder<u8> {
    /// A flag, not a byte. The import wrote 0 or 1 and this reads it back the
    /// same way — scaling it by 255 is the bug that makes a model draw at
    /// 1/255 opacity and look invisible rather than broken.
    static u8 From(const u8* raw) {
        f32 value = 0.0f;
        std::memcpy(&value, raw, sizeof(value));
        return value != 0.0f ? u8{1} : u8{0};
    }
};
template <>
struct Decoder<u16> {
    static u16 From(const u8* raw) {
        u32 value = 0;
        std::memcpy(&value, raw, sizeof(value));
        return static_cast<u16>(value);
    }
};
template <>
struct Decoder<m2::CameraSpline> {
    static m2::CameraSpline From(const u8* raw) {
        m2::CameraSpline value{};
        std::memcpy(&value.value, raw, sizeof(Vector3f));
        std::memcpy(&value.inTangent, raw + sizeof(Vector3f), sizeof(Vector3f));
        std::memcpy(&value.outTangent, raw + 2 * sizeof(Vector3f), sizeof(Vector3f));
        return value;
    }
};

class Exporter {
public:
    Exporter(const Document& document, u32 modelIndex, const ExportContext& context, m2::Model& out,
             Diagnostics& diagnostics)
        : document_(document), model_(document.models[modelIndex]), modelIndex_(modelIndex),
          context_(context), out_(out), diagnostics_(diagnostics) {}

    void run() {
        buildSequences();
        for (const AnimChannel& channel : model_.animChannels.channels) {
            emitChannel(channel);
        }
        emitEvents();
    }

private:
    struct Slotting {
        u32 clip = kInvalidIndex;
        u32 sequence = kInvalidIndex; ///< Inner-array index, or invalid for a global loop.
        u16 globalLoop = 0xFFFF;
    };

    static bool IsGlobalClip(const Clip& clip) {
        return hasFlag(clip.flags, ClipFlags::AutoPlay) &&
               hasFlag(clip.flags, ClipFlags::WorldClocked);
    }

    void buildSequences() {
        for (std::size_t c = 0; c < document_.clips.size(); ++c) {
            const Clip& clip = document_.clips[c];
            if (clip.model != modelIndex_) {
                continue;
            }
            Slotting slot;
            slot.clip = static_cast<u32>(c);

            if (IsGlobalClip(clip)) {
                const i64 stored = clip.native.value("globalLoop", -1);
                const u32 index = stored >= 0 ? static_cast<u32>(stored)
                                              : static_cast<u32>(out_.globalLoops.size());
                if (out_.globalLoops.size() <= index) {
                    out_.globalLoops.resize(index + 1);
                }
                out_.globalLoops[index].timestamp = MillisecondsOf(clip.duration);
                slot.globalLoop = static_cast<u16>(index);
                slots_.push_back(slot);
                continue;
            }

            m2::Sequence sequence;
            sequence.id = static_cast<u16>(clip.native.value("animationId", 0));
            sequence.variationIndex = static_cast<u16>(clip.native.value("variationIndex", 0));
            sequence.duration = MillisecondsOf(clip.duration);
            sequence.movespeed = static_cast<f32>(clip.native.value("movespeed", 0));
            sequence.flags = static_cast<m2::SequenceFlag>(clip.native.value("flagBits", 0));
            if (clip.looping) {
                sequence.flags = sequence.flags | m2::SequenceFlag::Looping;
            }
            sequence.frequency = static_cast<i16>(clip.native.value("frequency", 0));
            sequence.blendTimeIn = static_cast<u16>(clip.native.value("blendTimeIn", 0));
            sequence.blendTimeOut = static_cast<u16>(clip.native.value("blendTimeOut", 0));
            sequence.variationNext = static_cast<i16>(clip.native.value("variationNext", -1));
            sequence.aliasNext = static_cast<u16>(clip.native.value("aliasNext", 0));
            const Extent& extent = HasExtent(clip.bounds) ? clip.bounds : model_.bounds;
            sequence.bounding.minimum = extent.minimum;
            sequence.bounding.maximum = extent.maximum;
            sequence.bounding.sphereRadius = extent.sphereRadius;

            slot.sequence = static_cast<u32>(out_.sequences.size());
            out_.sequences.push_back(std::move(sequence));
            slots_.push_back(slot);
        }
    }

    /// The sub-track driving @p channel in @p clip's first container that has
    /// one. MDX, `.m2` and D3 all write exactly one container, so "the first"
    /// is the only one on anything this converter produced.
    const SubTrack* find(const Clip& clip, u32 channelId) const {
        for (const SubTrackContainer& container : clip.containers) {
            if (const SubTrack* track = container.find(channelId)) {
                return track;
            }
        }
        return nullptr;
    }

    /// Fills @p dst with one inner array per sequence. Returns false when no
    /// clip drives this channel at all.
    template <class T>
    bool build(const AnimChannel& channel, m2::AnimationTrack<T>& dst) const {
        const std::size_t elementSize = geom::AttrTypeSize(channel.valueType);
        bool any = false;
        bool interpSet = false;

        dst.timestamps.assign(out_.sequences.size(), {});
        dst.values.assign(out_.sequences.size(), {});

        for (const Slotting& slot : slots_) {
            const Clip& clip = document_.clips[slot.clip];
            const SubTrack* track = find(clip, channel.id);
            if (track == nullptr || track->times.empty()) {
                continue;
            }
            if (!track->wellSized(channel.valueType)) {
                diagnostics_.warn(DiagCode::AnimTrackDropped,
                                  "a sub-track of clip '" + clip.name +
                                      "' is not sized for its channel",
                                  ElementRef(ElementKind::Track, channel.id), ProfileId::Wow);
                continue;
            }
            if (!interpSet) {
                dst.interpolationType = M2Interp(track->interp);
                interpSet = true;
            }

            std::vector<u32> times;
            std::vector<T> values;
            const u32 stride = ValuesPerKey(track->interp) * static_cast<u32>(elementSize);
            times.reserve(track->times.size());
            values.reserve(track->times.size());
            for (std::size_t k = 0; k < track->times.size(); ++k) {
                times.push_back(MillisecondsOf(track->times[k]));
                values.push_back(Decoder<T>::From(track->values.data() + k * stride));
            }

            if (slot.globalLoop != 0xFFFF) {
                // A global loop keeps its keys in inner array 0 and runs on the
                // world clock — the same shape the import read.
                dst.globalSequenceId = slot.globalLoop;
                dst.timestamps.assign(1, std::move(times));
                dst.values.assign(1, std::move(values));
                return true;
            }
            if (slot.sequence < dst.timestamps.size()) {
                dst.timestamps[slot.sequence] = std::move(times);
                dst.values[slot.sequence] = std::move(values);
                any = true;
            }
        }
        if (!any) {
            dst.timestamps.clear();
            dst.values.clear();
        }
        return any;
    }

    template <class T>
    void emit(const AnimChannel& channel, m2::AnimationTrack<T>& dst) const {
        m2::AnimationTrack<T> built;
        if (build(channel, built)) {
            dst = std::move(built);
        }
    }

    void emitChannel(const AnimChannel& channel) {
        switch (channel.target.kind) {
        case TrackTarget::Kind::Node:
            emitNodeChannel(channel);
            return;
        case TrackTarget::Kind::MaterialLayer:
        case TrackTarget::Kind::MaterialFeature:
            emitMaterialChannel(channel);
            return;
        case TrackTarget::Kind::Section:
            // WoW keys a batch, never a geoset: a section channel is WC3's
            // GeosetAnimation and there is nothing here it can drive.
            diagnostics_.warn(DiagCode::AnimTrackDropped,
                              "an `.m2` has no per-geoset animation to write a section track to",
                              ElementRef(ElementKind::Mesh, channel.target.mesh), ProfileId::Wow);
            return;
        case TrackTarget::Kind::Count:
            return;
        }
    }

    void emitNodeChannel(const AnimChannel& channel) {
        const u32 node = channel.target.node;
        if (node >= context_.nodeSlots.size()) {
            return;
        }
        const ExportContext::NodeSlot& slot = context_.nodeSlots[node];
        switch (slot.slot) {
        case ExportContext::Slot::Bone: {
            if (slot.index >= out_.bones.size()) {
                return;
            }
            m2::Bone& bone = out_.bones[slot.index];
            switch (channel.target.channel) {
            case Channel::Translation:
                emit(channel, bone.translation);
                return;
            case Channel::Rotation:
                emit(channel, bone.rotation);
                return;
            case Channel::Scale:
                emit(channel, bone.scale);
                return;
            default:
                break;
            }
            break;
        }
        case ExportContext::Slot::Attachment:
            if (channel.target.channel == Channel::Visibility &&
                slot.index < out_.attachments.size()) {
                emit(channel, out_.attachments[slot.index].animate);
                return;
            }
            break;
        case ExportContext::Slot::Light: {
            if (slot.index >= out_.lights.size()) {
                return;
            }
            m2::Light& light = out_.lights[slot.index];
            const bool ambient = channel.target.sub == 1;
            switch (channel.target.channel) {
            case Channel::Color:
                emit(channel, ambient ? light.ambientColor : light.diffuseColor);
                return;
            case Channel::Intensity:
                emit(channel, ambient ? light.ambientIntensity : light.diffuseIntensity);
                return;
            case Channel::AttenuationStart:
                emit(channel, light.attenuationStart);
                return;
            case Channel::AttenuationEnd:
                emit(channel, light.attenuationEnd);
                return;
            case Channel::Visibility:
                emit(channel, light.visibility);
                return;
            default:
                break;
            }
            break;
        }
        case ExportContext::Slot::Camera:
            if (channel.target.channel == Channel::Translation &&
                slot.index < out_.cameras.size()) {
                emit(channel, out_.cameras[slot.index].positions);
                return;
            }
            break;
        default:
            break;
        }
        diagnostics_.warn(DiagCode::AnimTrackDropped,
                          std::string("no `.m2` record animates ") +
                              ToString(channel.target.channel) + " on this node",
                          ElementRef(ElementKind::Node, node), ProfileId::Wow);
    }

    /// The block is the join: it kept the resolved weight, transform and colour
    /// indices, and `appendCombos` writes those same numbers back into the
    /// combo tables — so the arrays only have to be long enough to hold them.
    const native::M2Material* blockFor(const MaterialChannelRef& ref) const {
        if (ref.profile != ProfileId::Wow) {
            return nullptr;
        }
        const Material* material = Resolve(model_, ref.slot, ref.profile, ref.look);
        if (material == nullptr || material->nativeKind() != NativeKind::M2) {
            return nullptr;
        }
        return std::get_if<native::M2Material>(&material->Native());
    }

    void emitMaterialChannel(const AnimChannel& channel) {
        const native::M2Material* block = blockFor(channel.target.material);
        if (block == nullptr) {
            if (channel.target.material.profile == ProfileId::Wow) {
                diagnostics_.warn(DiagCode::AnimTrackDropped,
                                  "a material track has no `.m2` block to place it through",
                                  ElementRef(ElementKind::Slot, channel.target.material.slot),
                                  ProfileId::Wow);
            }
            return;
        }

        // `M2Color` multiplies the whole batch rather than one stage, and the
        // block keeps the index it was read from.
        if (channel.target.kind == TrackTarget::Kind::MaterialLayer &&
            channel.target.sub == kWholeMaterial) {
            if (block->colorIndex < 0) {
                return;
            }
            const auto index = static_cast<std::size_t>(block->colorIndex);
            if (out_.colors.size() <= index) {
                out_.colors.resize(index + 1);
            }
            if (channel.target.channel == Channel::Color) {
                emit(channel, out_.colors[index].color);
            } else if (channel.target.channel == Channel::Alpha) {
                emit(channel, out_.colors[index].alpha);
            }
            return;
        }

        const u32 ordinal = channel.target.kind == TrackTarget::Kind::MaterialLayer
                                ? channel.target.sub
                                : featureLayer(channel.target.material, channel.target.sub);
        if (ordinal >= block->units.size()) {
            diagnostics_.warn(DiagCode::AnimTrackDropped,
                              "a material track names unit " + std::to_string(ordinal) +
                                  ", which this batch does not have",
                              ElementRef(ElementKind::Slot, channel.target.material.slot),
                              ProfileId::Wow);
            return;
        }
        const native::M2TextureUnit& unit = block->units[ordinal];

        if (channel.target.kind == TrackTarget::Kind::MaterialLayer) {
            if (channel.target.channel != Channel::Alpha) {
                return;
            }
            if (out_.textureWeights.size() <= unit.weight) {
                out_.textureWeights.resize(unit.weight + 1);
            }
            emit(channel, out_.textureWeights[unit.weight].weight);
            return;
        }

        if (out_.textureTransforms.size() <= unit.transform) {
            out_.textureTransforms.resize(unit.transform + 1);
        }
        m2::TextureTransform& transform = out_.textureTransforms[unit.transform];
        switch (channel.target.channel) {
        case Channel::UvTranslate:
            emit(channel, transform.translation);
            return;
        case Channel::UvRotate:
            emit(channel, transform.rotation);
            return;
        case Channel::UvScale:
            emit(channel, transform.scaling);
            return;
        default:
            break;
        }
    }

    /// Which layer ordinal a `MaterialFeature` id sits on.
    u32 featureLayer(const MaterialChannelRef& ref, u32 featureId) const {
        const Material* material = Resolve(model_, ref.slot, ref.profile, ref.look);
        if (material == nullptr) {
            return kInvalidIndex;
        }
        for (const MaterialFeature& feature : material->Common().features) {
            if (feature.id == featureId) {
                return feature.layer;
            }
        }
        return kInvalidIndex;
    }

    void emitEvents() {
        for (const Slotting& slot : slots_) {
            const Clip& clip = document_.clips[slot.clip];
            for (const ClipEvent& event : clip.events) {
                if (event.node >= context_.nodeSlots.size()) {
                    continue;
                }
                const ExportContext::NodeSlot& node = context_.nodeSlots[event.node];
                if (node.slot != ExportContext::Slot::Event || node.index >= out_.events.size()) {
                    diagnostics_.warn(DiagCode::AnimTrackDropped,
                                      "event '" + event.name +
                                          "' fires at a node that is not an `.m2` event",
                                      ElementRef(ElementKind::Node, event.node), ProfileId::Wow);
                    continue;
                }
                m2::Event& target = out_.events[node.index];
                if (target.enabled.timestamps.size() < out_.sequences.size()) {
                    target.enabled.timestamps.resize(out_.sequences.size());
                }
                if (slot.globalLoop != 0xFFFF) {
                    target.enabled.globalSequenceId = slot.globalLoop;
                    if (target.enabled.timestamps.empty()) {
                        target.enabled.timestamps.resize(1);
                    }
                    target.enabled.timestamps[0].push_back(MillisecondsOf(event.time));
                    continue;
                }
                if (slot.sequence < target.enabled.timestamps.size()) {
                    target.enabled.timestamps[slot.sequence].push_back(MillisecondsOf(event.time));
                }
            }
        }
    }

    const Document& document_;
    const Model& model_;
    u32 modelIndex_;
    const ExportContext& context_;
    m2::Model& out_;
    Diagnostics& diagnostics_;
    std::vector<Slotting> slots_;
};

} // namespace

void Export(const Document& document, u32 model, const ExportContext& context, m2::Model& out,
            Diagnostics& diagnostics) {
    if (model >= document.models.size()) {
        return;
    }
    Exporter(document, model, context, out, diagnostics).run();
}

} // namespace m2_anim
} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m2_anim.h"

#include <cstring>
#include <string>
#include <variant>

#include <whiteout/models/wem/anim/clip.h>
#include <whiteout/models/wem/native/m2_tables.h>

namespace whiteout {
namespace models {
namespace wem {
namespace m2_anim {

namespace {

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
    const Quaternion decoded = value;
    AppendBytes(decoded, out);
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

} // namespace m2_anim
} // namespace wem
} // namespace models
} // namespace whiteout

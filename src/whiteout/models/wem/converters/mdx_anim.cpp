// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdx_anim.h"

#include <cstring>
#include <string>

#include <whiteout/models/wem/anim/clip.h>

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_anim {

namespace {

constexpr u32 kNoGlobalSequence = mdx::Track<f32>::kNoGlobalSequence;

/// MDX counts in milliseconds; WEM counts in seconds.
///
/// A division, not a multiply by 1e-3: `1500 * 0.001f` is 1.5000001 and
/// `1500 / 1000.0f` is exactly 1.5, and a key at a round frame should land on a
/// round second.
constexpr f32 kMillisecondsPerSecond = 1000.0f;

constexpr f32 Seconds(f32 milliseconds) {
    return milliseconds / kMillisecondsPerSecond;
}

template <class T>
struct ValueTrait;
template <>
struct ValueTrait<f32> {
    static constexpr geom::AttrType kType = geom::AttrType::F32;
};
template <>
struct ValueTrait<u32> {
    static constexpr geom::AttrType kType = geom::AttrType::U32;
};
template <>
struct ValueTrait<Vector3f> {
    static constexpr geom::AttrType kType = geom::AttrType::F32x3;
};
template <>
struct ValueTrait<Quaternion> {
    static constexpr geom::AttrType kType = geom::AttrType::Quat;
};

/// MDX's `Linear` over a quaternion **is** a shortest-arc slerp in the engine,
/// so the two are the same statement and WEM says the more specific one.
Interpolation InterpOf(mdx::InterpolationType type, geom::AttrType valueType) {
    switch (type) {
    case mdx::InterpolationType::None:
        return Interpolation::Step;
    case mdx::InterpolationType::Linear:
        return valueType == geom::AttrType::Quat ? Interpolation::Slerp : Interpolation::Linear;
    case mdx::InterpolationType::Hermite:
        return Interpolation::Hermite;
    case mdx::InterpolationType::Bezier:
        return Interpolation::Bezier;
    }
    return Interpolation::Linear;
}

/// The `Fresnel` or `UvAnimation` feature on @p ordinal, creating a
/// `UvAnimation` if the material has none.
///
/// Through `InitCommon`, which is the one edit that does not make the native
/// block stale (§7.1): this is still the import deriving common from native, and
/// a keyed UV transform is precisely the feature §7.2.5 says a keyed source
/// leaves with zero rates for the sub-tracks to drive.
u32 FeatureIdFor(Material& material, FeatureKind kind, u32 ordinal) {
    CommonMaterial& common = material.InitCommon();
    for (const MaterialFeature& feature : common.features) {
        if (feature.kind() == kind && feature.layer == ordinal) {
            return feature.id;
        }
    }
    if (kind != FeatureKind::UvAnimation) {
        return kInvalidIndex;
    }
    MaterialFeature feature;
    feature.id = NextFeatureId(common.features);
    feature.layer = ordinal;
    feature.payload = UvAnimationFeature{};
    common.features.push_back(feature);
    return feature.id;
}

class Builder {
public:
    Builder(const mdx::Model& source, const Context& context, Document& document, u32 modelIndex,
            Diagnostics& out)
        : source_(source), context_(context), document_(document),
          model_(document.models[modelIndex]), modelIndex_(modelIndex), out_(out) {}

    void run() {
        buildSequenceClips();
        addNodeTracks();
        addKindTracks();
        addLayerTracks();
        addGeosetAnimationTracks();
        addEvents();
        commit();
    }

private:
    /// One sequence's window on the global timeline, and the clip it became.
    struct Window {
        f32 start = 0; ///< Milliseconds.
        f32 end = 0;
        u32 clip = kInvalidIndex;
    };

    void buildSequenceClips() {
        windows_.reserve(source_.sequences.size());
        for (const mdx::Sequence& sequence : source_.sequences) {
            Clip clip;
            clip.name = sequence.name;
            clip.model = modelIndex_;
            clip.duration =
                Seconds(static_cast<f32>(sequence.intervalEnd - sequence.intervalStart));
            clip.looping = !mdx::hasFlag(sequence.flags, mdx::Sequence::Flag::NonLooping);
            clip.native.set("intervalStart", static_cast<i64>(sequence.intervalStart));
            clip.native.set("intervalEnd", static_cast<i64>(sequence.intervalEnd));
            clip.native.set("moveSpeed", sequence.moveSpeed);
            clip.native.set("rarity", sequence.rarity);
            clip.native.set("syncPoint", static_cast<i64>(sequence.syncPoint));
            clip.containers.push_back(baseContainer());

            Window window;
            window.start = static_cast<f32>(sequence.intervalStart);
            window.end = static_cast<f32>(sequence.intervalEnd);
            window.clip = static_cast<u32>(clips_.size());
            windows_.push_back(window);
            clips_.push_back(std::move(clip));
        }
    }

    static SubTrackContainer baseContainer() {
        SubTrackContainer container;
        container.name = "base";
        container.priority = 0;
        container.concurrent = false; // MDX has one layer, and one layer is opaque.
        return container;
    }

    /// The auto-play clip for @p globalSequenceId, made on first use.
    u32 globalClipFor(u32 globalSequenceId) {
        for (const auto& entry : globalClips_) {
            if (entry.first == globalSequenceId) {
                return entry.second;
            }
        }
        if (globalSequenceId >= source_.globalSequences.size()) {
            return kInvalidIndex;
        }
        Clip clip;
        clip.name = "globalSequence_" + std::to_string(globalSequenceId);
        clip.model = modelIndex_;
        clip.duration = Seconds(static_cast<f32>(source_.globalSequences[globalSequenceId]));
        clip.looping = true;
        // The three-format unification: a global sequence, an `.m2` global
        // sequence and M3's SEQS flag 0x2 are all a loop the model runs itself.
        clip.flags = ClipFlags::AutoPlay | ClipFlags::WorldClocked;
        clip.native.set("globalSequenceId", static_cast<i64>(globalSequenceId));
        clip.containers.push_back(baseContainer());

        const u32 index = static_cast<u32>(clips_.size());
        clips_.push_back(std::move(clip));
        globalClips_.emplace_back(globalSequenceId, index);
        return index;
    }

    template <class T>
    static void appendKey(const mdx::Track<T>& track, std::size_t key, u32 perKey,
                          std::vector<u8>& values) {
        const std::size_t base = key * perKey;
        for (u32 v = 0; v < perKey; ++v) {
            const T& value = track.keys_data[base + v];
            const u8* bytes = reinterpret_cast<const u8*>(&value);
            values.insert(values.end(), bytes, bytes + sizeof(T));
        }
    }

    /// One track, into every clip that plays part of it.
    template <class T>
    void addTrack(const mdx::Track<T>& track, const TrackTarget& target) {
        if (!track.isUsed || track.timestamps.empty()) {
            return;
        }
        constexpr geom::AttrType kType = ValueTrait<T>::kType;
        const Interpolation interp = InterpOf(track.interpolationType, kType);
        const u32 perKey = ValuesPerKey(interp);
        if (track.keys_data.size() < track.timestamps.size() * perKey) {
            out_.warn(DiagCode::AnimTrackDropped,
                      std::string("a ") + ToString(interp) + " track holds " +
                          std::to_string(track.keys_data.size()) + " values for " +
                          std::to_string(track.timestamps.size()) + " keys",
                      ElementRef(ElementKind::Track, kInvalidIndex));
            return;
        }

        // Reserved rather than committed: a channel nothing drives is noise in a
        // format whose whole animation model is "the table declares, the tracks
        // move". `nextFreeId` is pure, so not committing costs nothing.
        const u32 id = model_.animChannels.nextFreeId();
        bool used = false;

        if (track.globalSequenceId != kNoGlobalSequence) {
            const u32 clip = globalClipFor(track.globalSequenceId);
            if (clip == kInvalidIndex) {
                out_.warn(DiagCode::AnimTrackDropped,
                          "a track names global sequence " +
                              std::to_string(track.globalSequenceId) + ", which the model lacks",
                          ElementRef(ElementKind::Track, kInvalidIndex));
                return;
            }
            SubTrack sub;
            sub.channel = id;
            sub.interp = interp;
            for (std::size_t k = 0; k < track.timestamps.size(); ++k) {
                sub.times.push_back(Seconds(static_cast<f32>(track.timestamps[k])));
                appendKey(track, k, perKey, sub.values);
            }
            clips_[clip].containers[0].subTracks.push_back(std::move(sub));
            used = true;
        } else {
            for (const Window& window : windows_) {
                if (sliceInto(track, interp, perKey, id, window)) {
                    used = true;
                }
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

    /// The window's keys plus the two that bracket it — see the file comment.
    template <class T>
    bool sliceInto(const mdx::Track<T>& track, Interpolation interp, u32 perKey, u32 id,
                   const Window& window) {
        const std::vector<u32>& times = track.timestamps;

        std::size_t first = 0;
        while (first < times.size() && static_cast<f32>(times[first]) < window.start) {
            ++first;
        }
        std::size_t last = first;
        while (last < times.size() && static_cast<f32>(times[last]) <= window.end) {
            ++last;
        }
        // One before and one after, so the spans at both edges interpolate the
        // way the global track does.
        const std::size_t lo = first > 0 ? first - 1 : first;
        const std::size_t hi = last < times.size() ? last + 1 : last;
        if (lo >= hi) {
            return false;
        }

        SubTrack sub;
        sub.channel = id;
        sub.interp = interp;
        for (std::size_t k = lo; k < hi; ++k) {
            sub.times.push_back(Seconds(static_cast<f32>(times[k]) - window.start));
            appendKey(track, k, perKey, sub.values);
        }
        clips_[window.clip].containers[0].subTracks.push_back(std::move(sub));
        return true;
    }

    u32 nodeOf(u32 objectId) const {
        if (context_.byObjectId == nullptr) {
            return kInvalidNode;
        }
        const auto found = context_.byObjectId->find(objectId);
        return found == context_.byObjectId->end() ? kInvalidNode : found->second;
    }

    static TrackTarget nodeTarget(u32 node, Channel channel, u32 sub = 0) {
        TrackTarget target;
        target.kind = TrackTarget::Kind::Node;
        target.node = node;
        target.channel = channel;
        target.sub = sub;
        return target;
    }

    /// The TRS every node kind carries.
    void addNodeTracks() {
        const auto visit = [this](const mdx::Node& node) {
            const u32 index = nodeOf(node.objectId);
            if (index == kInvalidNode) {
                return;
            }
            addTrack(node.translationTracks, nodeTarget(index, Channel::Translation));
            addTrack(node.rotationTracks, nodeTarget(index, Channel::Rotation));
            addTrack(node.scalingTracks, nodeTarget(index, Channel::Scale));
        };
        for (const mdx::Bone& item : source_.bones) {
            visit(item.node);
        }
        for (const mdx::Helper& item : source_.helpers) {
            visit(item.node);
        }
        for (const mdx::Light& item : source_.lights) {
            visit(item.node);
        }
        for (const mdx::Attachment& item : source_.attachments) {
            visit(item.node);
        }
        for (const mdx::ParticleEmitter& item : source_.particleEmitters) {
            visit(item.node);
        }
        for (const mdx::ParticleEmitter2& item : source_.particleEmitters2) {
            visit(item.node);
        }
        for (const mdx::CornEmitter& item : source_.cornEmitters) {
            visit(item.node);
        }
        for (const mdx::RibbonEmitter& item : source_.ribbonEmitters) {
            visit(item.node);
        }
        for (const mdx::EventObject& item : source_.eventObjects) {
            visit(item.node);
        }
        for (const mdx::CollisionShape& item : source_.collisionShapes) {
            visit(item.node);
        }
    }

    /// What each node kind animates beyond its transform.
    ///
    /// The emitters contribute **visibility only**: §18 keeps particle and
    /// ribbon systems out of WEM, so animating an emission rate would be storing
    /// the motion of something the document does not contain.
    void addKindTracks() {
        for (const mdx::Light& light : source_.lights) {
            const u32 node = nodeOf(light.node.objectId);
            if (node == kInvalidNode) {
                continue;
            }
            addTrack(light.colorTracks, nodeTarget(node, Channel::Color));
            addTrack(light.intensityTracks, nodeTarget(node, Channel::Intensity));
            addTrack(light.attenuationStartTracks, nodeTarget(node, Channel::AttenuationStart));
            addTrack(light.attenuationEndTracks, nodeTarget(node, Channel::AttenuationEnd));
            addTrack(light.visibilityTracks, nodeTarget(node, Channel::Visibility));
            // A WC3 light carries a second colour and intensity for its ambient
            // term. Same channel, `sub` 1 — which is what `sub` is for on a node.
            addTrack(light.ambientColorTracks, nodeTarget(node, Channel::Color, 1));
            addTrack(light.ambientIntensityTracks, nodeTarget(node, Channel::Intensity, 1));
        }
        for (const mdx::Attachment& attachment : source_.attachments) {
            const u32 node = nodeOf(attachment.node.objectId);
            if (node != kInvalidNode) {
                addTrack(attachment.visibilityTracks, nodeTarget(node, Channel::Visibility));
            }
        }
        for (const mdx::ParticleEmitter& emitter : source_.particleEmitters) {
            const u32 node = nodeOf(emitter.node.objectId);
            if (node != kInvalidNode) {
                addTrack(emitter.visibilityTracks, nodeTarget(node, Channel::Visibility));
            }
        }
        for (const mdx::ParticleEmitter2& emitter : source_.particleEmitters2) {
            const u32 node = nodeOf(emitter.node.objectId);
            if (node != kInvalidNode) {
                addTrack(emitter.visibilityTracks, nodeTarget(node, Channel::Visibility));
            }
        }
        for (const mdx::RibbonEmitter& ribbon : source_.ribbonEmitters) {
            const u32 node = nodeOf(ribbon.node.objectId);
            if (node == kInvalidNode) {
                continue;
            }
            addTrack(ribbon.colorTracks, nodeTarget(node, Channel::Color));
            addTrack(ribbon.alphaTracks, nodeTarget(node, Channel::Alpha));
            addTrack(ribbon.textureSlotTracks, nodeTarget(node, Channel::TextureIndex));
            addTrack(ribbon.visibilityTracks, nodeTarget(node, Channel::Visibility));
        }
        for (std::size_t c = 0; c < source_.cameras.size(); ++c) {
            if (c >= context_.cameraNodes.size() || context_.cameraNodes[c] == kInvalidNode) {
                continue;
            }
            addTrack(source_.cameras[c].positionTracks,
                     nodeTarget(context_.cameraNodes[c], Channel::Translation));
        }
    }

    /// Per-layer material tracks, once per profile the layer belongs to.
    void addLayerTracks() {
        for (const Context::ProfileLayers& profileLayers : context_.layerOrdinals) {
            ProfileMaterialSet* set = model_.setFor(profileLayers.profile);
            if (set == nullptr) {
                continue;
            }
            for (std::size_t m = 0; m < source_.materials.size(); ++m) {
                if (m >= profileLayers.byMaterial.size() || m >= set->slotBindings.size()) {
                    continue;
                }
                const u32 material = set->slotBindings[m].byLook.empty()
                                         ? kInvalidIndex
                                         : set->slotBindings[m].byLook[0];
                if (material >= set->materials.size()) {
                    continue;
                }
                addOneMaterial(source_.materials[m], profileLayers.byMaterial[m],
                               profileLayers.profile, static_cast<u32>(m),
                               set->materials[material]);
            }
        }
    }

    void addOneMaterial(const mdx::Material& source, const std::vector<u32>& ordinals,
                        ProfileId profile, u32 slot, Material& material) {
        for (std::size_t l = 0; l < source.layers.size() && l < ordinals.size(); ++l) {
            const u32 ordinal = ordinals[l];
            if (ordinal == kInvalidIndex) {
                continue;
            }
            const mdx::Layer& layer = source.layers[l];

            TrackTarget target;
            target.kind = TrackTarget::Kind::MaterialLayer;
            target.material.profile = profile;
            target.material.slot = slot;
            target.material.look = 0;
            target.sub = ordinal;

            target.channel = Channel::Alpha;
            addTrack(layer.alphaTracks, target);
            target.channel = Channel::TextureIndex;
            addTrack(layer.textureIdTracks, target);
            target.channel = Channel::Emissive;
            addTrack(layer.emissiveGainTracks, target);

            addFresnelTracks(layer, profile, slot, ordinal, material);
            addUvTracks(layer, profile, slot, ordinal, material);
        }
    }

    void addFresnelTracks(const mdx::Layer& layer, ProfileId profile, u32 slot, u32 ordinal,
                          Material& material) {
        const bool keyed = layer.fresnelColorTracks.isUsed || layer.fresnelAlphaTracks.isUsed ||
                           layer.fresnelTeamColorTracks.isUsed;
        if (!keyed) {
            return;
        }
        const u32 feature = FeatureIdFor(material, FeatureKind::Fresnel, ordinal);
        if (feature == kInvalidIndex) {
            // The material import only creates a fresnel feature where the static
            // strength is non-zero, so a file that keys one from zero has nothing
            // to hang the track on. Reported rather than invented.
            out_.warn(DiagCode::AnimTrackDropped,
                      "a fresnel track has no feature on layer " + std::to_string(ordinal),
                      ElementRef(ElementKind::Slot, slot), profile);
            return;
        }
        TrackTarget target;
        target.kind = TrackTarget::Kind::MaterialFeature;
        target.material.profile = profile;
        target.material.slot = slot;
        target.material.look = 0;
        target.sub = feature;

        target.channel = Channel::Color;
        addTrack(layer.fresnelColorTracks, target);
        target.channel = Channel::Alpha;
        addTrack(layer.fresnelAlphaTracks, target);
        target.channel = Channel::Weight;
        addTrack(layer.fresnelTeamColorTracks, target);
    }

    void addUvTracks(const mdx::Layer& layer, ProfileId profile, u32 slot, u32 ordinal,
                     Material& material) {
        if (layer.textureAnimationId >= source_.textureAnimations.size()) {
            return; // Including MDX's own -1 for "none".
        }
        const mdx::TextureAnimation& animation =
            source_.textureAnimations[layer.textureAnimationId];
        if (!animation.translationTracks.isUsed && !animation.rotationTracks.isUsed &&
            !animation.scalingTracks.isUsed) {
            return;
        }
        const u32 feature = FeatureIdFor(material, FeatureKind::UvAnimation, ordinal);

        TrackTarget target;
        target.kind = TrackTarget::Kind::MaterialFeature;
        target.material.profile = profile;
        target.material.slot = slot;
        target.material.look = 0;
        target.sub = feature;

        target.channel = Channel::UvTranslate;
        addTrack(animation.translationTracks, target);
        target.channel = Channel::UvRotate;
        addTrack(animation.rotationTracks, target);
        target.channel = Channel::UvScale;
        addTrack(animation.scalingTracks, target);
    }

    /// `GeosetAnimation` keys a **geoset**, which is a section (§5.5). This
    /// converter makes one mesh per geoset, so the mesh index is the geoset id
    /// and the section is its only one.
    void addGeosetAnimationTracks() {
        for (const mdx::GeosetAnimation& animation : source_.geosetAnimations) {
            if (animation.geosetId >= model_.meshes.size() ||
                model_.meshes[animation.geosetId].sections.empty()) {
                out_.warn(DiagCode::AnimTrackDropped,
                          "a geoset animation names geoset " + std::to_string(animation.geosetId) +
                              ", which the model does not have",
                          ElementRef(ElementKind::Mesh, animation.geosetId));
                continue;
            }
            TrackTarget target;
            target.kind = TrackTarget::Kind::Section;
            target.mesh = animation.geosetId;
            target.sub = 0;

            target.channel = Channel::Alpha;
            addTrack(animation.alphaTracks, target);
            target.channel = Channel::Color;
            addTrack(animation.colorTracks, target);
        }
    }

    /// An `EventObject`'s KEVT times, sorted into the clip whose window holds
    /// each one. The node is the where, the key is the when (§10.8).
    void addEvents() {
        for (const mdx::EventObject& event : source_.eventObjects) {
            const u32 node = nodeOf(event.node.objectId);
            if (node == kInvalidNode) {
                continue;
            }
            if (event.globalSequenceId != kNoGlobalSequence) {
                const u32 clip = globalClipFor(event.globalSequenceId);
                if (clip == kInvalidIndex) {
                    continue;
                }
                for (u32 time : event.eventTrackTimes) {
                    clips_[clip].events.push_back(
                        ClipEvent{Seconds(static_cast<f32>(time)), node, event.node.name, 0});
                }
                continue;
            }
            for (u32 time : event.eventTrackTimes) {
                for (const Window& window : windows_) {
                    if (static_cast<f32>(time) < window.start ||
                        static_cast<f32>(time) > window.end) {
                        continue;
                    }
                    clips_[window.clip].events.push_back(ClipEvent{
                        Seconds(static_cast<f32>(time) - window.start), node, event.node.name, 0});
                }
            }
        }
    }

    void commit() {
        for (Clip& clip : clips_) {
            document_.clips.push_back(std::move(clip));
        }
    }

    const mdx::Model& source_;
    const Context& context_;
    Document& document_;
    Model& model_;
    u32 modelIndex_;
    Diagnostics& out_;

    std::vector<Clip> clips_;
    std::vector<Window> windows_;
    std::vector<std::pair<u32, u32>> globalClips_;
};

} // namespace

void Import(const mdx::Model& source, const Context& context, Document& document, u32 model,
            Diagnostics& out) {
    if (model >= document.models.size()) {
        return;
    }
    Builder(source, context, document, model, out).run();
}

} // namespace mdx_anim
} // namespace wem
} // namespace models
} // namespace whiteout

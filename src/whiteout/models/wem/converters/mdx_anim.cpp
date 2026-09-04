// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdx_anim.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include <whiteout/models/wem/anim/clip.h>

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_anim {

namespace {

constexpr u32 kNoGlobalSequence = mdx::Track<f32>::kNoGlobalSequence;

/// Whether an extent says anything. A default-constructed one is all zeros, and
/// `Extent::valid()` answers "min <= max", which all zeros satisfies — so the
/// question a caller means by "did the source give me one" is volume, not
/// validity.
bool HasExtent(const Extent& extent) {
    return extent.sphereRadius > 0.0f || extent.maximum.x > extent.minimum.x ||
           extent.maximum.y > extent.minimum.y || extent.maximum.z > extent.minimum.z;
}

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
            // The sequence's own extent, which is the one bound WEM stores
            // rather than recomputes: it is the posed model over this clip, and
            // nothing in the file lets it be derived back.
            clip.bounds.minimum = sequence.extent.minimum;
            clip.bounds.maximum = sequence.extent.maximum;
            clip.bounds.sphereRadius = sequence.extent.boundsRadius;
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

// ============================================================================
// Export — clips back onto MDX's one global timeline (§10.8.3)
//
// The inverse of everything above, and the asymmetry is the whole of it. Import
// sliced one timeline into a clip per sequence and kept the bracketing keys;
// export merges those clips back, and a key two clips share — which is what a
// bracket key IS — has to be written once. So the merge is keyed on the
// ABSOLUTE time, and the first clip to claim a timestamp keeps it.
//
// What cannot round-trip, said once: a clip that carries no `intervalStart` did
// not come from an `.mdx`, and MDX has nowhere to put it but a fresh window at
// the end of the timeline. That is a real re-timing and it is reported.
// ============================================================================

namespace {

constexpr f32 kMilliseconds = 1000.0f;

u32 Milliseconds(f32 seconds) {
    const f32 ms = seconds * kMilliseconds;
    return ms <= 0.0f ? 0u : static_cast<u32>(ms + 0.5f);
}

mdx::InterpolationType MdxInterp(Interpolation interp) {
    switch (interp) {
    case Interpolation::Step:
        return mdx::InterpolationType::None;
    case Interpolation::Hermite:
        return mdx::InterpolationType::Hermite;
    case Interpolation::Bezier:
        return mdx::InterpolationType::Bezier;
    case Interpolation::Linear:
    case Interpolation::Slerp:
    case Interpolation::Count:
        break;
    }
    // A quaternion's Linear IS a shortest-arc slerp in the engine, which is why
    // the import collapsed the two — this is that statement read backwards.
    return mdx::InterpolationType::Linear;
}

/// One channel's keys, gathered off every clip that drives it and rebased onto
/// the global timeline.
struct MergedTrack {
    bool used = false;
    Interpolation interp = Interpolation::Linear;
    u32 globalSequenceId = mdx::Track<f32>::kNoGlobalSequence;
    std::vector<u32> times;      ///< Absolute milliseconds, ascending, unique.
    std::vector<const u8*> keys; ///< One pointer per key, into the sub-track's bytes.
    u32 valuesPerKey = 1;
    std::size_t valueSize = 0;

    /// Claim @p time for @p key. The FIRST claim wins, which is the whole of the
    /// bracket-key rule: the two clips either side of a boundary both carry the
    /// key that sits on it, and it belongs on the timeline once.
    void add(u32 time, const u8* key) {
        pending.emplace_back(time, key);
    }

    /// Ascending by time, one key per time. Stable, so "first claim wins" means
    /// the clip that appeared first in the document.
    void finish() {
        std::stable_sort(pending.begin(), pending.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        times.clear();
        keys.clear();
        for (const auto& [time, key] : pending) {
            if (!times.empty() && times.back() == time) {
                continue;
            }
            times.push_back(time);
            keys.push_back(key);
        }
        used = !times.empty();
    }

    std::vector<std::pair<u32, const u8*>> pending;
};

/// Writes a merged track into `dst`, decoding `T` out of the raw value bytes.
template <class T>
void Emit(const MergedTrack& merged, mdx::Track<T>& dst) {
    if (!merged.used || merged.times.empty()) {
        return;
    }
    dst.isUsed = true;
    dst.interpolationType = MdxInterp(merged.interp);
    dst.globalSequenceId = merged.globalSequenceId;
    dst.timestamps = merged.times;
    dst.keyCount = merged.times.size();
    dst.keys_data.clear();
    dst.keys_data.reserve(merged.times.size() * merged.valuesPerKey);
    for (const u8* key : merged.keys) {
        for (u32 v = 0; v < merged.valuesPerKey; ++v) {
            T value{};
            std::memcpy(&value, key + v * sizeof(T), sizeof(T));
            dst.keys_data.push_back(value);
        }
    }
}

class Exporter {
public:
    Exporter(const Document& document, u32 modelIndex, ProfileId profile,
             const ExportContext& context, mdx::Model& out, Diagnostics& diagnostics)
        : document_(document), model_(document.models[modelIndex]), modelIndex_(modelIndex),
          profile_(profile), context_(context), out_(out), diagnostics_(diagnostics) {}

    void run() {
        buildWindows();
        for (const AnimChannel& channel : model_.animChannels.channels) {
            emitChannel(channel);
        }
        emitEvents();
    }

private:
    /// Where one clip sits on the global timeline, or which global sequence it
    /// IS. Exactly one of the two.
    struct Window {
        u32 clip = kInvalidIndex;
        u32 start = 0; ///< Milliseconds. Meaningless for a global-sequence clip.
        u32 globalSequenceId = mdx::Track<f32>::kNoGlobalSequence;
    };

    static bool IsGlobalClip(const Clip& clip) {
        // The three-format unification, read backwards: an auto-play clip on a
        // clock that is not the host play's is what a global sequence is.
        return hasFlag(clip.flags, ClipFlags::AutoPlay) &&
               hasFlag(clip.flags, ClipFlags::WorldClocked);
    }

    void buildWindows() {
        u32 nextFree = 0;
        for (std::size_t c = 0; c < document_.clips.size(); ++c) {
            const Clip& clip = document_.clips[c];
            if (clip.model != modelIndex_) {
                continue;
            }
            Window window;
            window.clip = static_cast<u32>(c);

            if (IsGlobalClip(clip)) {
                const i64 stored = clip.native.value("globalSequenceId", -1);
                const u32 id = stored >= 0 ? static_cast<u32>(stored)
                                           : static_cast<u32>(out_.globalSequences.size());
                if (out_.globalSequences.size() <= id) {
                    out_.globalSequences.resize(id + 1, 0);
                }
                out_.globalSequences[id] = Milliseconds(clip.duration);
                window.globalSequenceId = id;
                windows_.push_back(window);
                continue;
            }

            mdx::Sequence sequence;
            sequence.name = clip.name;
            const i64 start = clip.native.value("intervalStart", -1);
            const i64 end = clip.native.value("intervalEnd", -1);
            if (start >= 0 && end >= start) {
                sequence.intervalStart = static_cast<u32>(start);
                sequence.intervalEnd = static_cast<u32>(end);
            } else {
                // No window on the clip: it did not come from an `.mdx`. The
                // timeline is the only clock MDX has, so one is allocated after
                // everything already placed — a real re-timing, reported.
                sequence.intervalStart = nextFree;
                sequence.intervalEnd = nextFree + Milliseconds(clip.duration);
                diagnostics_.info(DiagCode::AnimClipRetimed,
                                  "clip '" + clip.name + "' had no MDX interval; placed at " +
                                      std::to_string(sequence.intervalStart) + "ms",
                                  ElementRef(ElementKind::Clip, static_cast<u32>(c)), profile_);
            }
            if (!clip.looping) {
                sequence.flags = mdx::Sequence::Flag::NonLooping;
            }
            sequence.moveSpeed = static_cast<f32>(clip.native.value("moveSpeed", 0));
            sequence.rarity = static_cast<f32>(clip.native.value("rarity", 0));
            sequence.syncPoint = static_cast<u32>(clip.native.value("syncPoint", 0));
            // The clip's own extent when it has one, and the model's when it
            // does not — a clip from another format, or one an editor made.
            // Conservative either way, which is the direction a bound may err.
            const Extent& extent = HasExtent(clip.bounds) ? clip.bounds : model_.bounds;
            sequence.extent.minimum = extent.minimum;
            sequence.extent.maximum = extent.maximum;
            sequence.extent.boundsRadius = extent.sphereRadius;

            nextFree = std::max(nextFree, sequence.intervalEnd + 1000u);
            window.start = sequence.intervalStart;
            windows_.push_back(window);
            out_.sequences.push_back(std::move(sequence));
        }
    }

    /// Every sub-track driving `channel`, merged onto the timeline.
    MergedTrack gather(const AnimChannel& channel) const {
        MergedTrack merged;
        merged.valueSize = geom::AttrTypeSize(channel.valueType);
        bool first = true;

        for (const Window& window : windows_) {
            const Clip& clip = document_.clips[window.clip];
            for (const SubTrackContainer& container : clip.containers) {
                const SubTrack* track = container.find(channel.id);
                if (track == nullptr || track->times.empty()) {
                    continue;
                }
                if (!track->wellSized(channel.valueType)) {
                    diagnostics_.warn(DiagCode::AnimTrackDropped,
                                      "a sub-track of clip '" + clip.name +
                                          "' is not sized for its channel",
                                      ElementRef(ElementKind::Track, channel.id), profile_);
                    continue;
                }
                if (first) {
                    merged.interp = track->interp;
                    merged.valuesPerKey = ValuesPerKey(track->interp);
                    merged.globalSequenceId = window.globalSequenceId;
                    first = false;
                } else if (track->interp != merged.interp) {
                    // One MDX track has one interpolation type, so two clips
                    // that disagree cannot both be written. The first wins and
                    // the rest are named.
                    diagnostics_.warn(DiagCode::AnimTrackApproximated,
                                      "clip '" + clip.name + "' interpolates a shared channel as " +
                                          ToString(track->interp) + "; written as " +
                                          ToString(merged.interp),
                                      ElementRef(ElementKind::Track, channel.id), profile_);
                }

                const u32 stride = merged.valuesPerKey * static_cast<u32>(merged.valueSize);
                for (std::size_t k = 0; k < track->times.size(); ++k) {
                    // Back onto the timeline. A bracket key's time is negative
                    // or past the clip, which is exactly what puts it back where
                    // the neighbouring window's key already is.
                    const f32 absolute =
                        track->times[k] * kMilliseconds + static_cast<f32>(window.start);
                    const u32 time = absolute <= 0.0f ? 0u : static_cast<u32>(absolute + 0.5f);
                    merged.add(time, track->values.data() + k * stride);
                }
            }
        }
        merged.finish();
        return merged;
    }

    void emitChannel(const AnimChannel& channel) {
        MergedTrack merged = gather(channel);
        if (!merged.used) {
            return;
        }
        switch (channel.target.kind) {
        case TrackTarget::Kind::Node:
            emitNodeChannel(channel, merged);
            break;
        case TrackTarget::Kind::MaterialLayer:
            emitLayerChannel(channel, merged);
            break;
        case TrackTarget::Kind::MaterialFeature:
            emitFeatureChannel(channel, merged);
            break;
        case TrackTarget::Kind::Section:
            emitSectionChannel(channel, merged);
            break;
        case TrackTarget::Kind::Count:
            break;
        }
    }

    // ---- node ---------------------------------------------------------------

    mdx::Node* nodeRecord(u32 wemNode) {
        if (wemNode >= context_.nodeSlots.size()) {
            return nullptr;
        }
        const ExportContext::NodeSlot& slot = context_.nodeSlots[wemNode];
        const u32 i = slot.index;
        switch (slot.slot) {
        case ExportContext::Slot::Bone:
            return i < out_.bones.size() ? &out_.bones[i].node : nullptr;
        case ExportContext::Slot::Helper:
            return i < out_.helpers.size() ? &out_.helpers[i].node : nullptr;
        case ExportContext::Slot::Light:
            return i < out_.lights.size() ? &out_.lights[i].node : nullptr;
        case ExportContext::Slot::Attachment:
            return i < out_.attachments.size() ? &out_.attachments[i].node : nullptr;
        case ExportContext::Slot::ParticleEmitter:
            return i < out_.particleEmitters.size() ? &out_.particleEmitters[i].node : nullptr;
        case ExportContext::Slot::ParticleEmitter2:
            return i < out_.particleEmitters2.size() ? &out_.particleEmitters2[i].node : nullptr;
        case ExportContext::Slot::RibbonEmitter:
            return i < out_.ribbonEmitters.size() ? &out_.ribbonEmitters[i].node : nullptr;
        case ExportContext::Slot::CornEmitter:
            return i < out_.cornEmitters.size() ? &out_.cornEmitters[i].node : nullptr;
        case ExportContext::Slot::EventObject:
            return i < out_.eventObjects.size() ? &out_.eventObjects[i].node : nullptr;
        case ExportContext::Slot::CollisionShape:
            return i < out_.collisionShapes.size() ? &out_.collisionShapes[i].node : nullptr;
        case ExportContext::Slot::Camera:
        case ExportContext::Slot::None:
            break;
        }
        return nullptr;
    }

    void emitNodeChannel(const AnimChannel& channel, const MergedTrack& merged) {
        const u32 wemNode = channel.target.node;
        if (wemNode >= context_.nodeSlots.size()) {
            return;
        }
        const ExportContext::NodeSlot& slot = context_.nodeSlots[wemNode];

        // A camera's position is the one node track that is not on a node
        // chunk, because a camera is not one.
        if (slot.slot == ExportContext::Slot::Camera) {
            if (slot.index < out_.cameras.size() &&
                channel.target.channel == Channel::Translation) {
                Emit(merged, out_.cameras[slot.index].positionTracks);
            }
            return;
        }

        if (mdx::Node* node = nodeRecord(wemNode)) {
            switch (channel.target.channel) {
            case Channel::Translation:
                Emit(merged, node->translationTracks);
                return;
            case Channel::Rotation:
                Emit(merged, node->rotationTracks);
                return;
            case Channel::Scale:
                Emit(merged, node->scalingTracks);
                return;
            default:
                break;
            }
        }

        // Everything else is a property of the record the node became.
        switch (slot.slot) {
        case ExportContext::Slot::Light: {
            if (slot.index >= out_.lights.size()) {
                return;
            }
            mdx::Light& light = out_.lights[slot.index];
            // `sub` 1 is the ambient half of the same pair — the one place a
            // node animates two things of the same name.
            const bool ambient = channel.target.sub == 1;
            switch (channel.target.channel) {
            case Channel::Color:
                Emit(merged, ambient ? light.ambientColorTracks : light.colorTracks);
                return;
            case Channel::Intensity:
                Emit(merged, ambient ? light.ambientIntensityTracks : light.intensityTracks);
                return;
            case Channel::AttenuationStart:
                Emit(merged, light.attenuationStartTracks);
                return;
            case Channel::AttenuationEnd:
                Emit(merged, light.attenuationEndTracks);
                return;
            case Channel::Visibility:
                Emit(merged, light.visibilityTracks);
                return;
            default:
                break;
            }
            break;
        }
        case ExportContext::Slot::Attachment:
            if (channel.target.channel == Channel::Visibility &&
                slot.index < out_.attachments.size()) {
                Emit(merged, out_.attachments[slot.index].visibilityTracks);
                return;
            }
            break;
        case ExportContext::Slot::ParticleEmitter:
            if (channel.target.channel == Channel::Visibility &&
                slot.index < out_.particleEmitters.size()) {
                Emit(merged, out_.particleEmitters[slot.index].visibilityTracks);
                return;
            }
            break;
        case ExportContext::Slot::ParticleEmitter2:
            if (channel.target.channel == Channel::Visibility &&
                slot.index < out_.particleEmitters2.size()) {
                Emit(merged, out_.particleEmitters2[slot.index].visibilityTracks);
                return;
            }
            break;
        case ExportContext::Slot::RibbonEmitter: {
            if (slot.index >= out_.ribbonEmitters.size()) {
                return;
            }
            mdx::RibbonEmitter& ribbon = out_.ribbonEmitters[slot.index];
            switch (channel.target.channel) {
            case Channel::Color:
                Emit(merged, ribbon.colorTracks);
                return;
            case Channel::Alpha:
                Emit(merged, ribbon.alphaTracks);
                return;
            case Channel::TextureIndex:
                Emit(merged, ribbon.textureSlotTracks);
                return;
            case Channel::Visibility:
                Emit(merged, ribbon.visibilityTracks);
                return;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }

        diagnostics_.warn(DiagCode::AnimTrackDropped,
                          std::string("no MDX record animates ") +
                              ToString(channel.target.channel) + " on this node",
                          ElementRef(ElementKind::Node, wemNode), profile_);
    }

    // ---- materials ----------------------------------------------------------

    mdx::Layer* layerRecord(const MaterialChannelRef& ref, u32 ordinal) {
        if (ref.profile != profile_ || ref.slot >= out_.materials.size()) {
            return nullptr;
        }
        mdx::Material& material = out_.materials[ref.slot];
        u32 layer = ordinal;
        if (ref.slot < context_.layerOfOrdinal.size()) {
            const std::vector<u32>& map = context_.layerOfOrdinal[ref.slot];
            if (ordinal < map.size()) {
                layer = map[ordinal];
            }
        }
        return layer < material.layers.size() ? &material.layers[layer] : nullptr;
    }

    void emitLayerChannel(const AnimChannel& channel, const MergedTrack& merged) {
        mdx::Layer* layer = layerRecord(channel.target.material, channel.target.sub);
        if (layer == nullptr) {
            // Not this profile's set, or a layer this export filtered out. The
            // first is normal — a document with two sets has tracks for both.
            if (channel.target.material.profile == profile_) {
                diagnostics_.warn(
                    DiagCode::AnimTrackDropped,
                    "a layer track names ordinal " + std::to_string(channel.target.sub) +
                        ", which this material "
                        "did not write",
                    ElementRef(ElementKind::Slot, channel.target.material.slot), profile_);
            }
            return;
        }
        switch (channel.target.channel) {
        case Channel::Alpha:
            Emit(merged, layer->alphaTracks);
            return;
        case Channel::TextureIndex:
            Emit(merged, layer->textureIdTracks);
            return;
        case Channel::Emissive:
            Emit(merged, layer->emissiveGainTracks);
            return;
        default:
            break;
        }
        diagnostics_.warn(DiagCode::AnimTrackDropped,
                          std::string("an MDX layer has no ") + ToString(channel.target.channel) +
                              " track",
                          ElementRef(ElementKind::Slot, channel.target.material.slot), profile_);
    }

    /// A feature is either the layer's fresnel — which lives on the layer — or
    /// its UV animation, which lives in a `TextureAnimation` the layer names.
    void emitFeatureChannel(const AnimChannel& channel, const MergedTrack& merged) {
        const MaterialChannelRef& ref = channel.target.material;
        if (ref.profile != profile_) {
            return;
        }
        const Material* material = Resolve(model_, ref.slot, ref.profile, ref.look);
        if (material == nullptr) {
            return;
        }
        const MaterialFeature* feature = nullptr;
        for (const MaterialFeature& candidate : material->Common().features) {
            if (candidate.id == channel.target.sub) {
                feature = &candidate;
                break;
            }
        }
        if (feature == nullptr) {
            diagnostics_.warn(DiagCode::AnimTrackDropped,
                              "a feature track names feature " +
                                  std::to_string(channel.target.sub) +
                                  ", which the material "
                                  "does not carry",
                              ElementRef(ElementKind::Slot, ref.slot), profile_);
            return;
        }
        mdx::Layer* layer = layerRecord(ref, feature->layer);
        if (layer == nullptr) {
            return;
        }

        if (feature->kind() == FeatureKind::Fresnel) {
            switch (channel.target.channel) {
            case Channel::Color:
                Emit(merged, layer->fresnelColorTracks);
                return;
            case Channel::Alpha:
                Emit(merged, layer->fresnelAlphaTracks);
                return;
            case Channel::Weight:
                Emit(merged, layer->fresnelTeamColorTracks);
                return;
            default:
                break;
            }
            return;
        }
        if (feature->kind() != FeatureKind::UvAnimation) {
            return;
        }

        // The layer names a TXAN, or gets one: MDX keeps UV motion in a shared
        // table rather than on the layer, and a keyed feature is exactly what
        // needs an entry.
        if (layer->textureAnimationId >= out_.textureAnimations.size()) {
            layer->textureAnimationId = static_cast<u32>(out_.textureAnimations.size());
            out_.textureAnimations.emplace_back();
        }
        mdx::TextureAnimation& animation = out_.textureAnimations[layer->textureAnimationId];
        switch (channel.target.channel) {
        case Channel::UvTranslate:
            Emit(merged, animation.translationTracks);
            return;
        case Channel::UvRotate:
            Emit(merged, animation.rotationTracks);
            return;
        case Channel::UvScale:
            Emit(merged, animation.scalingTracks);
            return;
        default:
            break;
        }
    }

    // ---- sections -----------------------------------------------------------

    /// A `GeosetAnimation` keys a geoset. One record per geoset, created on
    /// first use — alpha and colour are two channels sharing it, and a hidden
    /// section already made one before the animation export ran.
    mdx::GeosetAnimation& geosetAnimationFor(u32 geoset) {
        for (mdx::GeosetAnimation& existing : out_.geosetAnimations) {
            if (existing.geosetId == geoset) {
                return existing;
            }
        }
        mdx::GeosetAnimation created;
        created.geosetId = geoset;
        created.flags = mdx::GeosetAnimation::Flag::Color;
        out_.geosetAnimations.push_back(std::move(created));
        return out_.geosetAnimations.back();
    }

    /// A section channel names a MESH, and a mesh is now several geosets — so
    /// the curve is written onto each of them. Equal on a document that came
    /// from `.mdx`, where a mesh is one geoset by construction.
    void emitSectionChannel(const AnimChannel& channel, const MergedTrack& merged) {
        if (channel.target.mesh >= context_.geosetsOfMesh.size()) {
            return;
        }
        for (const u32 geoset : context_.geosetsOfMesh[channel.target.mesh]) {
            if (geoset >= out_.geosets.size()) {
                continue;
            }
            mdx::GeosetAnimation& animation = geosetAnimationFor(geoset);
            switch (channel.target.channel) {
            case Channel::Alpha:
                Emit(merged, animation.alphaTracks);
                break;
            case Channel::Color:
                Emit(merged, animation.colorTracks);
                break;
            default:
                break;
            }
        }
    }

    // ---- events -------------------------------------------------------------

    void emitEvents() {
        for (const Window& window : windows_) {
            const Clip& clip = document_.clips[window.clip];
            for (const ClipEvent& event : clip.events) {
                if (event.node >= context_.nodeSlots.size()) {
                    continue;
                }
                const ExportContext::NodeSlot& slot = context_.nodeSlots[event.node];
                if (slot.slot != ExportContext::Slot::EventObject ||
                    slot.index >= out_.eventObjects.size()) {
                    // The node's kind is not fixed across formats (§10.8): an
                    // `.m3` names the bone its SDEV key sits on. MDX has only
                    // the EventObject, so anything else has nowhere to land.
                    diagnostics_.warn(DiagCode::AnimTrackDropped,
                                      "event '" + event.name +
                                          "' fires at a node that is not an MDX event object",
                                      ElementRef(ElementKind::Node, event.node), profile_);
                    continue;
                }
                mdx::EventObject& object = out_.eventObjects[slot.index];
                object.globalSequenceId = window.globalSequenceId;
                const f32 absolute = event.time * kMilliseconds + static_cast<f32>(window.start);
                object.eventTrackTimes.push_back(
                    absolute <= 0.0f ? 0u : static_cast<u32>(absolute + 0.5f));
            }
        }
        for (mdx::EventObject& object : out_.eventObjects) {
            std::sort(object.eventTrackTimes.begin(), object.eventTrackTimes.end());
            object.eventTrackTimes.erase(
                std::unique(object.eventTrackTimes.begin(), object.eventTrackTimes.end()),
                object.eventTrackTimes.end());
        }
    }

    const Document& document_;
    const Model& model_;
    u32 modelIndex_;
    ProfileId profile_;
    const ExportContext& context_;
    mdx::Model& out_;
    Diagnostics& diagnostics_;
    std::vector<Window> windows_;
};

} // namespace

void Export(const Document& document, u32 model, ProfileId profile, const ExportContext& context,
            mdx::Model& out, Diagnostics& diagnostics) {
    if (model >= document.models.size()) {
        return;
    }
    Exporter(document, model, profile, context, out, diagnostics).run();
}

} // namespace mdx_anim
} // namespace wem
} // namespace models
} // namespace whiteout

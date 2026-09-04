// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_anim.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <whiteout/models/wem/anim/clip.h>

#include "../materials/m3_core.h"

namespace whiteout {
namespace models {
namespace wem {
namespace m3_anim {

namespace {

/// STC `animRefs[j]` packs the slot in the high half and the block index in the
/// low one — the 13 typed `AnimBlock` arrays are addressed no other way.
constexpr u32 SlotOf(u32 animRef) {
    return animRef >> 16;
}
constexpr u32 BlockOf(u32 animRef) {
    return animRef & 0xFFFFu;
}

/// The slot each source type lives in. Named because two of them are traps:
/// **11 is where every keyed discrete channel is**, never 10, and 0 is events
/// rather than a value stream.
enum Slot : u32 {
    kSdev = 0,
    kSd2v = 1,
    kSd3v = 2,
    kSd4q = 3,
    kSdcc = 4,
    kSdr3 = 5,
    kSdu8 = 6,
    kSds6 = 7,
    kSdu6 = 8,
    kSds3 = 9,
    kSdu3 = 10,
    kSdfg = 11,
    kSdmb = 12,
};

constexpr f32 Seconds(f32 milliseconds) {
    return milliseconds / 1000.0f;
}

/// SC2's basis into WEM's canonical one — the same change `m3_converter` applies
/// to a bone's rest transform. A key is a vector in the basis being changed, so
/// a track that skipped this would play in SC2's basis over WEM's geometry.
Vector3f Rebase(const Vector3f& v) {
    return Vector3f{-v.y, v.x, v.z};
}

/// An extent through the same change of basis. The rebase is a rotation, so a
/// corner can swap sides and min/max are recomputed rather than mapped -- the
/// same rule `m3_converter`'s `ToExtent` follows.
Extent RebaseExtent(const m3::Extent& source) {
    const Vector3f a = Rebase(source.min);
    const Vector3f b = Rebase(source.max);
    Extent out;
    out.minimum = Vector3f{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    out.maximum = Vector3f{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    out.sphereRadius = source.radius;
    return out;
}

Quaternion RebaseRotation(const Quaternion& q) {
    return Quaternion{-q.y, q.x, q.z, q.w};
}

/// BGRA bytes into RGBA floats. M3 stores a colour the way the hardware wanted
/// it in 2010, and the byte order is the half worth writing down.
Vector4f ToRgba(const m3::ColorBGRA& color) {
    return Vector4f{static_cast<f32>(color.r) / 255.0f, static_cast<f32>(color.g) / 255.0f,
                    static_cast<f32>(color.b) / 255.0f, static_cast<f32>(color.a) / 255.0f};
}

std::string TrimNuls(std::string value) {
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

template <class T>
void AppendBytes(const T& value, std::vector<u8>& out) {
    const u8* raw = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), raw, raw + sizeof(T));
}

/// What one channel is, once. `interp` is the AnimRef's — **flags bit 4** — and
/// it belongs to the channel rather than the sub-track because M3 cannot express
/// a mix across one channel's sub-tracks.
struct Declared {
    u32 animId = 0;
    geom::AttrType type = geom::AttrType::F32;
    Interpolation interp = Interpolation::Linear;
    bool rebaseVector = false;
    bool rebaseQuaternion = false;
};

/// AnimRef flags bit 4: step iff set.
constexpr bool StepsOn(u16 flags) {
    return (flags & 0x10u) != 0;
}

Interpolation InterpOf(u16 flags, geom::AttrType type) {
    if (StepsOn(flags)) {
        return Interpolation::Step;
    }
    return type == geom::AttrType::Quat ? Interpolation::Slerp : Interpolation::Linear;
}

/// One SD block, decoded into the channel's declared type. False when the slot
/// and the type disagree, or the block index is past its array.
bool Decode(const m3::SubTrackContainer& stc, u32 animRef, const Declared& channel, f32 originMs,
            std::vector<f32>& times, std::vector<u8>& values) {
    const u32 slot = SlotOf(animRef);
    const u32 block = BlockOf(animRef);

    const auto stamp = [&](const std::vector<i32>& source) {
        times.reserve(source.size());
        for (i32 time : source) {
            times.push_back(Seconds(static_cast<f32>(time) - originMs));
        }
    };

    switch (slot) {
    case kSd2v: {
        if (block >= stc.sd2v.size() || channel.type != geom::AttrType::F32x2) {
            return false;
        }
        stamp(stc.sd2v[block].timestamps);
        for (const Vector2f& key : stc.sd2v[block].keys) {
            AppendBytes(key, values);
        }
        break;
    }
    case kSd3v: {
        if (block >= stc.sd3v.size() || channel.type != geom::AttrType::F32x3) {
            return false;
        }
        stamp(stc.sd3v[block].timestamps);
        for (const Vector3f& key : stc.sd3v[block].keys) {
            AppendBytes(channel.rebaseVector ? Rebase(key) : key, values);
        }
        break;
    }
    case kSd4q: {
        if (block >= stc.sd4q.size() || channel.type != geom::AttrType::Quat) {
            return false;
        }
        stamp(stc.sd4q[block].timestamps);
        for (const Quaternion& key : stc.sd4q[block].keys) {
            AppendBytes(channel.rebaseQuaternion ? RebaseRotation(key) : key, values);
        }
        break;
    }
    case kSdcc: {
        if (block >= stc.sdcc.size() || channel.type != geom::AttrType::F32x4) {
            return false;
        }
        stamp(stc.sdcc[block].timestamps);
        for (const m3::ColorBGRA& key : stc.sdcc[block].keys) {
            AppendBytes(ToRgba(key), values);
        }
        break;
    }
    case kSdr3: {
        if (block >= stc.sdr3.size() || channel.type != geom::AttrType::F32) {
            return false;
        }
        stamp(stc.sdr3[block].timestamps);
        for (f32 key : stc.sdr3[block].keys) {
            AppendBytes(key, values);
        }
        break;
    }
    case kSdu6: {
        if (block >= stc.sdu6.size() || channel.type != geom::AttrType::U32) {
            return false;
        }
        stamp(stc.sdu6[block].timestamps);
        for (u16 key : stc.sdu6[block].keys) {
            const u32 widened = key;
            AppendBytes(widened, values);
        }
        break;
    }
    case kSdu3: {
        if (block >= stc.sdu3.size() || channel.type != geom::AttrType::U32) {
            return false;
        }
        stamp(stc.sdu3[block].timestamps);
        for (u32 key : stc.sdu3[block].keys) {
            AppendBytes(key, values);
        }
        break;
    }
    case kSdfg: {
        // Slot 11. Every keyed visibility and dynamic state in shipped content
        // is here; reading slot 10 for it returns `initValue` forever.
        if (block >= stc.sdfg.size() || channel.type != geom::AttrType::F32) {
            return false;
        }
        stamp(stc.sdfg[block].timestamps);
        for (const m3::Flag& key : stc.sdfg[block].keys) {
            const f32 value = key.value != 0 ? 1.0f : 0.0f;
            AppendBytes(value, values);
        }
        break;
    }
    default:
        return false;
    }

    if (values.size() != times.size() * geom::AttrTypeSize(channel.type)) {
        // A block whose key count disagrees with its timestamp count. Shipped
        // files do not do this, and a half-length stream is worse than none.
        times.clear();
        values.clear();
        return false;
    }
    return !times.empty();
}

class Builder {
public:
    /// No `Diagnostics`: everything this builder would report it deliberately
    /// does not — see the file comment on the AnimRef long tail.
    Builder(const m3::Model& source, const Context& context, Document& document, u32 modelIndex)
        : source_(source), context_(context), document_(document),
          model_(document.models[modelIndex]), modelIndex_(modelIndex) {}

    void run() {
        declareBoneChannels();
        declareLightChannels();
        declareMaterialChannels();
        buildClips();
        for (Clip& clip : clips_) {
            document_.clips.push_back(std::move(clip));
        }
    }

private:
    /// Declares a channel for @p ref if it is animated, and remembers how to
    /// decode it. An id that is already declared is not declared twice — two
    /// AnimRefs sharing an id share a stream, which is legal.
    template <class T>
    void declare(const m3::AnimRef<T>& ref, geom::AttrType type, const TrackTarget& target,
                 bool rebaseVector = false, bool rebaseQuaternion = false) {
        if (!ref.isAnimated() || model_.animChannels.find(ref.animId) != nullptr) {
            return;
        }
        AnimChannel channel;
        channel.id = ref.animId;
        channel.target = target;
        channel.valueType = type;
        channel.initValue = restValue(ref.initValue, type, rebaseVector, rebaseQuaternion);
        model_.animChannels.add(channel);

        Declared declared;
        declared.animId = ref.animId;
        declared.type = type;
        declared.interp = InterpOf(ref.flags, type);
        declared.rebaseVector = rebaseVector;
        declared.rebaseQuaternion = rebaseQuaternion;
        declared_.push_back(declared);
    }

    /// The AnimRef's `initValue`, in the channel's own type and basis — the rest
    /// value an opaque container contributes for an un-keyed channel (§10.8.1).
    template <class T>
    static std::vector<u8> restValue(const T& value, geom::AttrType type, bool rebaseVector,
                                     bool rebaseQuaternion) {
        std::vector<u8> bytes;
        if constexpr (std::is_same_v<T, Vector3f>) {
            if (type == geom::AttrType::F32x3) {
                AppendBytes(rebaseVector ? Rebase(value) : value, bytes);
            }
        } else if constexpr (std::is_same_v<T, Quaternion>) {
            if (type == geom::AttrType::Quat) {
                AppendBytes(rebaseQuaternion ? RebaseRotation(value) : value, bytes);
            }
        } else if constexpr (std::is_same_v<T, Vector2f>) {
            if (type == geom::AttrType::F32x2) {
                AppendBytes(value, bytes);
            }
        } else if constexpr (std::is_same_v<T, f32>) {
            if (type == geom::AttrType::F32) {
                AppendBytes(value, bytes);
            }
        } else if constexpr (std::is_same_v<T, m3::ColorBGRA>) {
            if (type == geom::AttrType::F32x4) {
                AppendBytes(ToRgba(value), bytes);
            }
        } else if constexpr (std::is_same_v<T, u32>) {
            if (type == geom::AttrType::F32) {
                const f32 flag = value != 0 ? 1.0f : 0.0f;
                AppendBytes(flag, bytes);
            } else if (type == geom::AttrType::U32) {
                AppendBytes(value, bytes);
            }
        } else if constexpr (std::is_same_v<T, u16>) {
            if (type == geom::AttrType::U32) {
                const u32 widened = value;
                AppendBytes(widened, bytes);
            }
        }
        return bytes;
    }

    static TrackTarget nodeTarget(u32 node, Channel channel) {
        TrackTarget target;
        target.kind = TrackTarget::Kind::Node;
        target.node = node;
        target.channel = channel;
        return target;
    }

    void declareBoneChannels() {
        for (std::size_t b = 0; b < source_.bones.size(); ++b) {
            const u32 node = context_.bases.bone + static_cast<u32>(b);
            if (node >= model_.nodes.size()) {
                continue;
            }
            const m3::Bone& bone = source_.bones[b];
            declare(bone.position, geom::AttrType::F32x3, nodeTarget(node, Channel::Translation),
                    true, false);
            declare(bone.rotation, geom::AttrType::Quat, nodeTarget(node, Channel::Rotation), false,
                    true);
            // Scale is not rebased, matching the rest transform the node import
            // wrote: a componentwise scale in a frame the basis change permutes
            // is a question `m3_converter` answers, and answering it differently
            // here would make the rest pose and the motion disagree.
            declare(bone.scale, geom::AttrType::F32x3, nodeTarget(node, Channel::Scale));
            declare(bone.visibility, geom::AttrType::F32, nodeTarget(node, Channel::Visibility));
        }
    }

    void declareLightChannels() {
        for (std::size_t l = 0; l < source_.lights.size(); ++l) {
            const u32 node = context_.bases.light + static_cast<u32>(l);
            if (node >= model_.nodes.size()) {
                continue;
            }
            const m3::Light& light = source_.lights[l];
            declare(light.diffuseColor, geom::AttrType::F32x4, nodeTarget(node, Channel::Color));
            declare(light.intensityMultiplier, geom::AttrType::F32,
                    nodeTarget(node, Channel::Intensity));
            declare(light.attenuationStart, geom::AttrType::F32,
                    nodeTarget(node, Channel::AttenuationStart));
        }
    }

    /// Standard materials only. A `MADD` restores to a *temporary*
    /// `StandardMaterial`, so its AnimRefs have no home in the parsed model to
    /// point back at — the blob keeps them and WEM's channel table cannot.
    void declareMaterialChannels() {
        ProfileMaterialSet* set = model_.setFor(context_.profile);
        if (set == nullptr) {
            return;
        }
        for (std::size_t m = 0; m < source_.materialMaps.size(); ++m) {
            if (source_.materialMaps[m].materialType != m3::MaterialType::Standard ||
                m >= context_.layerOrdinals.size()) {
                continue;
            }
            const std::size_t index = source_.materialMaps[m].materialIndex;
            if (index >= source_.standardMaterials.size()) {
                continue;
            }
            const m3::StandardMaterial& material = source_.standardMaterials[index];
            const std::vector<u32>& ordinals = context_.layerOrdinals[m];

            for (u32 s = 0; s < static_cast<u32>(m3_core::StandardLayer::Count); ++s) {
                if (s >= ordinals.size() || ordinals[s] == kInvalidIndex) {
                    continue;
                }
                const auto& layer =
                    m3_core::LayerOf(material, static_cast<m3_core::StandardLayer>(s));
                if (!layer.has_value()) {
                    continue;
                }
                declareLayerChannels(*layer, static_cast<u32>(m), ordinals[s], *set);
            }
        }
    }

    void declareLayerChannels(const m3::TextureLayer& layer, u32 slot, u32 ordinal,
                              const ProfileMaterialSet& set) {
        TrackTarget layerTarget;
        layerTarget.kind = TrackTarget::Kind::MaterialLayer;
        layerTarget.material.profile = context_.profile;
        layerTarget.material.slot = slot;
        layerTarget.material.look = 0;
        layerTarget.sub = ordinal;

        layerTarget.channel = Channel::Alpha;
        declare(layer.mapAlpha, geom::AttrType::F32, layerTarget);
        layerTarget.channel = Channel::Color;
        declare(layer.color, geom::AttrType::F32x4, layerTarget);
        layerTarget.channel = Channel::TextureIndex;
        declare(layer.currentFrame, geom::AttrType::U32, layerTarget);
        layerTarget.channel = Channel::Weight;
        declare(layer.rgbMultiply, geom::AttrType::F32, layerTarget);

        const bool uvKeyed = layer.uvOffset.isAnimated() || layer.uvAngle.isAnimated() ||
                             layer.uvTiling.isAnimated();
        if (!uvKeyed) {
            return;
        }
        TrackTarget uvTarget = layerTarget;
        uvTarget.kind = TrackTarget::Kind::MaterialFeature;
        uvTarget.sub = featureIdFor(slot, ordinal, set);
        if (uvTarget.sub == kInvalidIndex) {
            return;
        }
        uvTarget.channel = Channel::UvTranslate;
        declare(layer.uvOffset, geom::AttrType::F32x2, uvTarget);
        uvTarget.channel = Channel::UvRotate;
        declare(layer.uvAngle, geom::AttrType::F32x3, uvTarget);
        uvTarget.channel = Channel::UvScale;
        declare(layer.uvTiling, geom::AttrType::F32x2, uvTarget);
    }

    /// The `UvAnimation` feature on @p ordinal, created if the material import
    /// left none. `InitCommon`, so the native block does not go stale (§7.1).
    u32 featureIdFor(u32 slot, u32 ordinal, const ProfileMaterialSet& set) {
        if (slot >= set.slotBindings.size() || set.slotBindings[slot].byLook.empty()) {
            return kInvalidIndex;
        }
        const u32 index = set.slotBindings[slot].byLook[0];
        ProfileMaterialSet* mutableSet = model_.setFor(context_.profile);
        if (mutableSet == nullptr || index >= mutableSet->materials.size()) {
            return kInvalidIndex;
        }
        CommonMaterial& common = mutableSet->materials[index].InitCommon();
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

    void buildClips() {
        for (std::size_t s = 0; s < source_.sequences.size(); ++s) {
            const m3::Sequence& sequence = source_.sequences[s];
            Clip clip;
            clip.name = TrimNuls(sequence.name);
            clip.model = modelIndex_;
            clip.duration = Seconds(static_cast<f32>(sequence.endFrame) -
                                    static_cast<f32>(sequence.startFrame));
            clip.looping = !m3::hasFlag(sequence.flags, m3::SequenceFlag::NotLooping);
            if (m3::hasFlag(sequence.flags, m3::SequenceFlag::AlwaysGlobal)) {
                // SEQS 0x2 — started at anim-state init, persistent, world
                // clocked. Never identified by a "GL" name.
                clip.flags = ClipFlags::AutoPlay | ClipFlags::Persistent | ClipFlags::WorldClocked;
            }
            clip.native.set("sequenceId", static_cast<i64>(sequence.id));
            clip.native.set("m3SeqFlags", static_cast<i64>(static_cast<u32>(sequence.flags)));
            clip.native.set("m3Frequency", static_cast<i64>(sequence.frequency));
            clip.native.set("blendTime", static_cast<i64>(sequence.blendTime));
            clip.native.set("startFrame", static_cast<i64>(sequence.startFrame));
            // The sequence's own bound: the posed model over this clip, which
            // nothing in the file lets a reader derive back.
            clip.bounds = RebaseExtent(sequence.bounds);

            const f32 origin = static_cast<f32>(sequence.startFrame);
            if (s < source_.animationGroups.size()) {
                for (u32 stcIndex : source_.animationGroups[s].subtrackIndices) {
                    if (stcIndex >= source_.subTrackCollections.size()) {
                        continue;
                    }
                    clip.containers.push_back(
                        buildContainer(source_.subTrackCollections[stcIndex], origin, clip));
                }
            }
            if (clip.containers.empty()) {
                SubTrackContainer container;
                container.name = "base";
                clip.containers.push_back(std::move(container));
            }
            clips_.push_back(std::move(clip));
        }
    }

    SubTrackContainer buildContainer(const m3::SubTrackContainer& stc, f32 origin, Clip& clip) {
        SubTrackContainer container;
        container.name = TrimNuls(stc.name);
        container.priority = static_cast<i32>(stc.animPriority);
        container.concurrent = stc.runsConcurrent != 0;
        container.native.set("animationStateIndex", static_cast<i64>(stc.animationStateIndex));

        for (std::size_t j = 0; j < stc.animIds.size() && j < stc.animRefs.size(); ++j) {
            const u32 animId = stc.animIds[j];
            const u32 animRef = stc.animRefs[j];

            if (SlotOf(animRef) == kSdev) {
                addEvents(stc, animRef, origin, clip);
                continue;
            }
            const Declared* declared = declaredFor(animId);
            if (declared == nullptr) {
                continue; // A target WEM does not store — see the file comment.
            }
            SubTrack track;
            track.channel = animId;
            track.interp = declared->interp;
            if (!Decode(stc, animRef, *declared, origin, track.times, track.values)) {
                continue;
            }
            container.subTracks.push_back(std::move(track));
        }
        return container;
    }

    void addEvents(const m3::SubTrackContainer& stc, u32 animRef, f32 origin, Clip& clip) {
        const u32 block = BlockOf(animRef);
        if (block >= stc.sdev.size()) {
            return;
        }
        const auto& keys = stc.sdev[block];
        for (std::size_t k = 0; k < keys.timestamps.size() && k < keys.keys.size(); ++k) {
            const m3::Event& event = keys.keys[k];
            const u32 node = context_.bases.bone + event.boneIndex;
            clip.events.push_back(ClipEvent{Seconds(static_cast<f32>(keys.timestamps[k]) - origin),
                                            node < model_.nodes.size() ? node : kInvalidNode,
                                            TrimNuls(event.name), event.eventType});
        }
    }

    const Declared* declaredFor(u32 animId) const {
        for (const Declared& entry : declared_) {
            if (entry.animId == animId) {
                return &entry;
            }
        }
        return nullptr;
    }

    const m3::Model& source_;
    const Context& context_;
    Document& document_;
    Model& model_;
    u32 modelIndex_;

    std::vector<Clip> clips_;
    std::vector<Declared> declared_;
};

} // namespace

NodeBases NodeBases::Of(const m3::Model& source) {
    NodeBases bases;
    bases.bone = 0;
    bases.attachment = static_cast<u32>(source.bones.size());
    bases.light = bases.attachment + static_cast<u32>(source.attachmentPoints.size());
    bases.camera = bases.light + static_cast<u32>(source.lights.size());
    return bases;
}

void Import(const m3::Model& source, const Context& context, Document& document, u32 model,
            Diagnostics&) {
    if (model >= document.models.size()) {
        return;
    }
    Builder(source, context, document, model).run();
}

u32 Merge(const m3::Model& external, Document& document, u32 model, Diagnostics& out) {
    if (model >= document.models.size()) {
        return 0;
    }
    Model& target = document.models[model];
    const AnimChannelTable& table = target.animChannels;

    // The interpolation a channel already plays with. M3 cannot express a mix
    // across one channel's sub-tracks, so the base model's answer is the answer
    // — and the `.m3a` carries no AnimRef of its own to ask.
    const auto interpOf = [&](u32 animId) {
        for (const Clip& clip : document.clips) {
            if (clip.model != model) {
                continue;
            }
            for (const SubTrackContainer& container : clip.containers) {
                if (const SubTrack* found = container.find(animId); found != nullptr) {
                    return found->interp;
                }
            }
        }
        return Interpolation::Linear;
    };

    u32 added = 0;
    u32 unknown = 0;
    for (std::size_t s = 0; s < external.sequences.size(); ++s) {
        const m3::Sequence& sequence = external.sequences[s];
        Clip clip;
        clip.name = TrimNuls(sequence.name);
        clip.model = model;
        clip.duration =
            Seconds(static_cast<f32>(sequence.endFrame) - static_cast<f32>(sequence.startFrame));
        clip.looping = !m3::hasFlag(sequence.flags, m3::SequenceFlag::NotLooping);
        if (m3::hasFlag(sequence.flags, m3::SequenceFlag::AlwaysGlobal)) {
            clip.flags = ClipFlags::AutoPlay | ClipFlags::Persistent | ClipFlags::WorldClocked;
        }
        clip.native.set("sequenceId", static_cast<i64>(sequence.id));
        clip.native.set("external", static_cast<i64>(1));

        const f32 origin = static_cast<f32>(sequence.startFrame);
        if (s >= external.animationGroups.size()) {
            continue;
        }
        for (u32 stcIndex : external.animationGroups[s].subtrackIndices) {
            if (stcIndex >= external.subTrackCollections.size()) {
                continue;
            }
            const m3::SubTrackContainer& stc = external.subTrackCollections[stcIndex];
            SubTrackContainer container;
            container.name = TrimNuls(stc.name);
            container.priority = static_cast<i32>(stc.animPriority);
            container.concurrent = stc.runsConcurrent != 0;

            for (std::size_t j = 0; j < stc.animIds.size() && j < stc.animRefs.size(); ++j) {
                const AnimChannel* channel = table.find(stc.animIds[j]);
                if (channel == nullptr) {
                    // An `.m3a` cannot introduce a target, only new motion for
                    // one the base model already declares.
                    ++unknown;
                    continue;
                }
                Declared declared;
                declared.animId = channel->id;
                declared.type = channel->valueType;
                declared.interp = interpOf(channel->id);
                declared.rebaseVector = channel->target.channel == Channel::Translation;
                declared.rebaseQuaternion = channel->target.channel == Channel::Rotation;

                SubTrack track;
                track.channel = channel->id;
                track.interp = declared.interp;
                if (!Decode(stc, stc.animRefs[j], declared, origin, track.times, track.values)) {
                    continue;
                }
                container.subTracks.push_back(std::move(track));
            }
            clip.containers.push_back(std::move(container));
        }
        if (clip.containers.empty()) {
            continue;
        }
        document.clips.push_back(std::move(clip));
        ++added;
    }

    if (unknown != 0) {
        out.info(DiagCode::AnimTrackDropped,
                 std::to_string(unknown) + " external sub-tracks name channels this model does "
                                           "not declare",
                 ElementRef(ElementKind::Document, model));
    }
    return added;
}

// ============================================================================
// Export — clips back into SEQS / STG_ / STC_ and the SD blocks (§10.8.3)
//
// The 1:1 direction, and it stays 1:1: a clip is a SEQS plus the STG_ at the
// same index, a container is an STC_, a sub-track is one typed SD block plus
// the `(slot << 16) | block` reference that names it. What the import read out
// of `animRefs`, this writes back into them.
//
// Three of the import's four traps apply unchanged and are handled here:
//
//   - **Interpolation is AnimRef flags bit 4**, so a `Step` channel sets it and
//     nothing else does. `interpType` stays 0 — it is the row that lies.
//   - **A keyed discrete channel goes back to SDFG, slot 11.** The rule that
//     picks it is the channel's own: visibility and dynamic state are flags,
//     and the value stream WEM holds for them is 0-or-1.
//   - **The basis change is part of the value**, so a translation key is
//     un-rebased on the way out exactly as the bone's rest transform is.
//
// The fourth — `animId` is the join and is kept verbatim — is what makes this
// possible at all: the channel table already holds the ids the STC has to name.
// ============================================================================

namespace {

i32 Ticks(f32 seconds) {
    return static_cast<i32>(seconds * 1000.0f + (seconds < 0.0f ? -0.5f : 0.5f));
}

/// The inverse of `Rebase` — WEM's canonical basis back into SC2's.
Vector3f Unrebase(const Vector3f& v) {
    return Vector3f{v.y, -v.x, v.z};
}

Quaternion UnrebaseRotation(const Quaternion& q) {
    return Quaternion{q.y, -q.x, q.z, q.w};
}

/// Whether an extent says anything. A default-constructed one is all zeros, and
/// `Extent::valid()` answers "min <= max", which all zeros satisfies -- so the
/// question a caller means by "did the source give me one" is volume, not
/// validity.
bool HasExtent(const Extent& extent) {
    return extent.sphereRadius > 0.0f || extent.maximum.x > extent.minimum.x ||
           extent.maximum.y > extent.minimum.y || extent.maximum.z > extent.minimum.z;
}

m3::Extent UnrebaseExtent(const Extent& source) {
    const Vector3f a = Unrebase(source.minimum);
    const Vector3f b = Unrebase(source.maximum);
    m3::Extent out;
    out.min = Vector3f{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    out.max = Vector3f{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    out.radius = source.sphereRadius;
    return out;
}

m3::ColorBGRA FromRgba(const Vector4f& value) {
    const auto byteOf = [](f32 v) {
        const f32 scaled = v * 255.0f;
        return static_cast<u8>(scaled <= 0.0f ? 0.0f : scaled >= 255.0f ? 255.0f : scaled + 0.5f);
    };
    m3::ColorBGRA color;
    color.r = byteOf(value.x);
    color.g = byteOf(value.y);
    color.b = byteOf(value.z);
    color.a = byteOf(value.w);
    return color;
}

class Exporter {
public:
    Exporter(const Document& document, u32 modelIndex, const ExportContext& context, m3::Model& out,
             Diagnostics& diagnostics)
        : document_(document), model_(document.models[modelIndex]), modelIndex_(modelIndex),
          context_(context), out_(out), diagnostics_(diagnostics) {}

    void run() {
        buildMaterialOrdinals();
        for (std::size_t c = 0; c < document_.clips.size(); ++c) {
            if (document_.clips[c].model == modelIndex_) {
                buildClip(document_.clips[c]);
            }
        }
    }

private:
    /// Where one channel's keys go: which typed array, and how a value is
    /// written into it.
    enum class Stream : u32 {
        None = 0,
        Sd2v = 1,
        Sd3v = 2,
        Sd4q = 3,
        Sdcc = 4,
        Sdr3 = 5,
        Sdu3 = 10,
        Sdfg = 11,
    };

    /// The slot a channel's values belong in.
    ///
    /// The one judgement here is `F32`: a **flag** goes back to SDFG (slot 11,
    /// where every keyed visibility in shipped content lives) and a real scalar
    /// to SDR3. Which it is, is a property of the channel — `Visibility` is the
    /// only F32 the import read out of SDFG.
    static Stream StreamFor(const AnimChannel& channel) {
        switch (channel.valueType) {
        case geom::AttrType::F32x2:
            return Stream::Sd2v;
        case geom::AttrType::F32x3:
            return Stream::Sd3v;
        case geom::AttrType::Quat:
            return Stream::Sd4q;
        case geom::AttrType::F32x4:
            return Stream::Sdcc;
        case geom::AttrType::U32:
            return Stream::Sdu3;
        case geom::AttrType::F32:
            return channel.target.channel == Channel::Visibility ? Stream::Sdfg : Stream::Sdr3;
        default:
            return Stream::None;
        }
    }

    /// Whether a translation / rotation channel's values were rebased on import
    /// — the same test `declareBoneChannels` made, which is what keeps the two
    /// directions symmetric.
    static bool RebasesVector(const AnimChannel& channel) {
        return channel.target.kind == TrackTarget::Kind::Node &&
               channel.target.channel == Channel::Translation;
    }
    static bool RebasesQuaternion(const AnimChannel& channel) {
        return channel.target.kind == TrackTarget::Kind::Node &&
               channel.target.channel == Channel::Rotation;
    }

    void buildClip(const Clip& clip) {
        m3::Sequence sequence;
        sequence.name = clip.name;
        sequence.id = static_cast<i32>(clip.native.value("sequenceId", 0));
        sequence.index = static_cast<i32>(out_.sequences.size());
        sequence.flags = static_cast<m3::SequenceFlag>(clip.native.value("m3SeqFlags", 0));
        if (!clip.looping) {
            sequence.flags = static_cast<m3::SequenceFlag>(
                static_cast<u32>(sequence.flags) | static_cast<u32>(m3::SequenceFlag::NotLooping));
        }
        if (hasFlag(clip.flags, ClipFlags::AutoPlay)) {
            sequence.flags =
                static_cast<m3::SequenceFlag>(static_cast<u32>(sequence.flags) |
                                              static_cast<u32>(m3::SequenceFlag::AlwaysGlobal));
        }
        sequence.frequency = static_cast<u32>(clip.native.value("m3Frequency", 0));
        sequence.blendTime = static_cast<u32>(clip.native.value("blendTime", 0));
        const i32 origin = static_cast<i32>(clip.native.value("startFrame", 0));
        sequence.startFrame = static_cast<u32>(origin);
        sequence.endFrame = static_cast<u32>(origin + Ticks(clip.duration));
        sequence.bounds = UnrebaseExtent(HasExtent(clip.bounds) ? clip.bounds : model_.bounds);

        m3::AnimationGroup group;
        group.name = clip.name;
        bool eventsPending = !clip.events.empty();
        for (const SubTrackContainer& container : clip.containers) {
            const u32 index = buildContainer(container, clip, origin, eventsPending);
            if (index != kInvalidIndex) {
                group.subtrackIndices.push_back(index);
                eventsPending = false;
            }
        }

        out_.sequences.push_back(std::move(sequence));
        out_.animationGroups.push_back(std::move(group));
    }

    u32 buildContainer(const SubTrackContainer& source, const Clip& clip, i32 origin,
                       bool takeEvents) {
        m3::SubTrackContainer stc;
        stc.name = source.name;
        stc.animPriority = static_cast<u16>(source.priority);
        stc.runsConcurrent = source.concurrent ? 1u : 0u;
        stc.animationStateIndex = static_cast<u16>(source.native.value("animationStateIndex", 0));
        stc.padding = 0;
        stc.unknown = 0;

        for (const SubTrack& track : source.subTracks) {
            const AnimChannel* channel = model_.animChannels.find(track.channel);
            if (channel == nullptr) {
                continue;
            }
            if (!track.wellSized(channel->valueType)) {
                diagnostics_.warn(DiagCode::AnimTrackDropped,
                                  "a sub-track of clip '" + clip.name +
                                      "' is not sized for its channel",
                                  ElementRef(ElementKind::Track, channel->id), profile());
                continue;
            }
            const u32 animRef = writeStream(stc, *channel, track, origin);
            if (animRef == kInvalidIndex) {
                continue;
            }
            stc.animIds.push_back(channel->id);
            stc.animRefs.push_back(animRef);
            wireAnimRef(*channel, track);
        }

        // Events are the slot-0 stream, and they belong to the CLIP rather than
        // to a container — so they go into the first container that is written
        // and not into every one of them, which is how a three-layer clip would
        // otherwise fire each of its events three times.
        if (takeEvents && !clip.events.empty()) {
            const u32 animRef = writeEvents(stc, clip, origin);
            if (animRef != kInvalidIndex) {
                stc.animIds.push_back(0);
                stc.animRefs.push_back(animRef);
            }
        }

        if (stc.animRefs.empty()) {
            return kInvalidIndex;
        }
        out_.subTrackCollections.push_back(std::move(stc));
        return static_cast<u32>(out_.subTrackCollections.size() - 1);
    }

    ProfileId profile() const {
        return context_.profile;
    }

    /// Fills the typed block and returns `(slot << 16) | block`.
    u32 writeStream(m3::SubTrackContainer& stc, const AnimChannel& channel, const SubTrack& track,
                    i32 origin) {
        const Stream stream = StreamFor(channel);
        if (stream == Stream::None) {
            return kInvalidIndex;
        }
        const std::size_t count = track.times.size();
        const std::size_t size = geom::AttrTypeSize(channel.valueType);
        const std::size_t stride = ValuesPerKey(track.interp) * size;

        std::vector<i32> stamps;
        stamps.reserve(count);
        for (f32 time : track.times) {
            stamps.push_back(origin + Ticks(time));
        }
        const auto endFrame = stamps.empty() ? 0 : static_cast<u32>(stamps.back());

        const auto read = [&](std::size_t k) { return track.values.data() + k * stride; };

        u32 block = 0;
        switch (stream) {
        case Stream::Sd2v: {
            m3::AnimBlock<Vector2f> entry;
            entry.timestamps = stamps;
            entry.flags = 0;
            entry.endFrame = endFrame;
            for (std::size_t k = 0; k < count; ++k) {
                Vector2f value{};
                std::memcpy(&value, read(k), sizeof(value));
                entry.keys.push_back(value);
            }
            block = static_cast<u32>(stc.sd2v.size());
            stc.sd2v.push_back(std::move(entry));
            break;
        }
        case Stream::Sd3v: {
            m3::AnimBlock<Vector3f> entry;
            entry.timestamps = stamps;
            entry.flags = 0;
            entry.endFrame = endFrame;
            const bool rebase = RebasesVector(channel);
            for (std::size_t k = 0; k < count; ++k) {
                Vector3f value{};
                std::memcpy(&value, read(k), sizeof(value));
                entry.keys.push_back(rebase ? Unrebase(value) : value);
            }
            block = static_cast<u32>(stc.sd3v.size());
            stc.sd3v.push_back(std::move(entry));
            break;
        }
        case Stream::Sd4q: {
            m3::AnimBlock<Quaternion> entry;
            entry.timestamps = stamps;
            entry.flags = 0;
            entry.endFrame = endFrame;
            const bool rebase = RebasesQuaternion(channel);
            for (std::size_t k = 0; k < count; ++k) {
                Quaternion value{};
                std::memcpy(&value, read(k), sizeof(value));
                entry.keys.push_back(rebase ? UnrebaseRotation(value) : value);
            }
            block = static_cast<u32>(stc.sd4q.size());
            stc.sd4q.push_back(std::move(entry));
            break;
        }
        case Stream::Sdcc: {
            m3::AnimBlock<m3::ColorBGRA> entry;
            entry.timestamps = stamps;
            entry.flags = 0;
            entry.endFrame = endFrame;
            for (std::size_t k = 0; k < count; ++k) {
                Vector4f value{};
                std::memcpy(&value, read(k), sizeof(value));
                entry.keys.push_back(FromRgba(value));
            }
            block = static_cast<u32>(stc.sdcc.size());
            stc.sdcc.push_back(std::move(entry));
            break;
        }
        case Stream::Sdr3: {
            m3::AnimBlock<f32> entry;
            entry.timestamps = stamps;
            entry.flags = 0;
            entry.endFrame = endFrame;
            for (std::size_t k = 0; k < count; ++k) {
                f32 value = 0.0f;
                std::memcpy(&value, read(k), sizeof(value));
                entry.keys.push_back(value);
            }
            block = static_cast<u32>(stc.sdr3.size());
            stc.sdr3.push_back(std::move(entry));
            break;
        }
        case Stream::Sdu3: {
            m3::AnimBlock<u32> entry;
            entry.timestamps = stamps;
            entry.flags = 0;
            entry.endFrame = endFrame;
            for (std::size_t k = 0; k < count; ++k) {
                u32 value = 0;
                std::memcpy(&value, read(k), sizeof(value));
                entry.keys.push_back(value);
            }
            block = static_cast<u32>(stc.sdu3.size());
            stc.sdu3.push_back(std::move(entry));
            break;
        }
        case Stream::Sdfg: {
            m3::AnimBlock<m3::Flag> entry;
            entry.timestamps = stamps;
            entry.flags = 0;
            entry.endFrame = endFrame;
            for (std::size_t k = 0; k < count; ++k) {
                f32 value = 0.0f;
                std::memcpy(&value, read(k), sizeof(value));
                m3::Flag flag{};
                flag.value = value != 0.0f ? 1u : 0u;
                entry.keys.push_back(flag);
            }
            block = static_cast<u32>(stc.sdfg.size());
            stc.sdfg.push_back(std::move(entry));
            break;
        }
        case Stream::None:
            return kInvalidIndex;
        }
        return (static_cast<u32>(stream) << 16) | block;
    }

    u32 writeEvents(m3::SubTrackContainer& stc, const Clip& clip, i32 origin) {
        m3::AnimBlock<m3::Event> entry;
        for (const ClipEvent& event : clip.events) {
            m3::Event key{};
            key.name = event.name;
            key.eventType = event.value;
            // The node is a bone here, because that is what an `.m3` event
            // names — the kind is not fixed across formats, and this is the one
            // that has to be a bone index.
            key.boneIndex = 0;
            if (event.node < context_.nodeSlots.size()) {
                const ExportContext::NodeSlot& slot = context_.nodeSlots[event.node];
                if (slot.slot == ExportContext::Slot::Bone) {
                    key.boneIndex = static_cast<u16>(slot.index);
                }
            }
            entry.timestamps.push_back(origin + Ticks(event.time));
            entry.keys.push_back(std::move(key));
        }
        if (entry.keys.empty()) {
            return kInvalidIndex;
        }
        entry.flags = 0;
        entry.endFrame = static_cast<u32>(entry.timestamps.back());
        const u32 block = static_cast<u32>(stc.sdev.size());
        stc.sdev.push_back(std::move(entry));
        return block; // slot 0, so the reference is the block index itself
    }

    // ---- the AnimRefs the STC's ids have to match ---------------------------

    /// `flags` bit 4 is step; `interpType` stays 0 because the row lies at
    /// runtime and the import never read it.
    template <class T>
    static void Wire(m3::AnimRef<T>& ref, u32 animId, Interpolation interp) {
        ref.animId = animId;
        ref.flags = static_cast<u16>(interp == Interpolation::Step ? 0x10u : 0x0u);
    }

    void wireAnimRef(const AnimChannel& channel, const SubTrack& track) {
        switch (channel.target.kind) {
        case TrackTarget::Kind::Node:
            wireNode(channel, track);
            return;
        case TrackTarget::Kind::MaterialLayer:
        case TrackTarget::Kind::MaterialFeature:
            wireMaterial(channel, track);
            return;
        default:
            return;
        }
    }

    void wireNode(const AnimChannel& channel, const SubTrack& track) {
        const u32 node = channel.target.node;
        if (node >= context_.nodeSlots.size()) {
            return;
        }
        const ExportContext::NodeSlot& slot = context_.nodeSlots[node];
        if (slot.slot == ExportContext::Slot::Bone && slot.index < out_.bones.size()) {
            m3::Bone& bone = out_.bones[slot.index];
            switch (channel.target.channel) {
            case Channel::Translation:
                Wire(bone.position, channel.id, track.interp);
                return;
            case Channel::Rotation:
                Wire(bone.rotation, channel.id, track.interp);
                return;
            case Channel::Scale:
                Wire(bone.scale, channel.id, track.interp);
                return;
            case Channel::Visibility:
                Wire(bone.visibility, channel.id, track.interp);
                return;
            default:
                return;
            }
        }
        if (slot.slot == ExportContext::Slot::Light && slot.index < out_.lights.size()) {
            m3::Light& light = out_.lights[slot.index];
            switch (channel.target.channel) {
            case Channel::Color:
                Wire(light.diffuseColor, channel.id, track.interp);
                return;
            case Channel::Intensity:
                Wire(light.intensityMultiplier, channel.id, track.interp);
                return;
            case Channel::AttenuationStart:
                Wire(light.attenuationStart, channel.id, track.interp);
                return;
            default:
                return;
            }
        }
    }

    /// Per exported material map entry, the ordinal each `StandardLayer` became
    /// — recovered by re-importing what was just written, so the append order
    /// has exactly one authority (`m3_core`'s).
    void buildMaterialOrdinals() {
        materialOrdinals_.assign(out_.materialMaps.size(), {});
        m3_core::Context context;
        Diagnostics ignored;
        for (std::size_t m = 0; m < out_.materialMaps.size(); ++m) {
            if (out_.materialMaps[m].materialType != m3::MaterialType::Standard) {
                continue;
            }
            m3_core::ImportMaterial(out_, out_.materialMaps[m], profile(), context, ignored,
                                    &materialOrdinals_[m]);
        }
    }

    void wireMaterial(const AnimChannel& channel, const SubTrack& track) {
        const MaterialChannelRef& ref = channel.target.material;
        if (ref.profile != profile() || ref.slot >= materialOrdinals_.size() ||
            ref.slot >= out_.materialMaps.size()) {
            return;
        }
        const u32 ordinal = channel.target.kind == TrackTarget::Kind::MaterialLayer
                                ? channel.target.sub
                                : featureLayer(ref, channel.target.sub);
        if (ordinal == kInvalidIndex) {
            return;
        }

        // Which StandardLayer that ordinal is — the inverse of the map the
        // re-import just produced.
        const std::vector<u32>& ordinals = materialOrdinals_[ref.slot];
        std::size_t layerSlot = ordinals.size();
        for (std::size_t s = 0; s < ordinals.size(); ++s) {
            if (ordinals[s] == ordinal) {
                layerSlot = s;
                break;
            }
        }
        if (layerSlot >= ordinals.size()) {
            return;
        }
        const std::size_t index = out_.materialMaps[ref.slot].materialIndex;
        if (index >= out_.standardMaterials.size()) {
            return;
        }
        std::optional<m3::TextureLayer>& layer = m3_core::MutableLayerOf(
            out_.standardMaterials[index], static_cast<m3_core::StandardLayer>(layerSlot));
        if (!layer.has_value()) {
            return;
        }

        if (channel.target.kind == TrackTarget::Kind::MaterialLayer) {
            switch (channel.target.channel) {
            case Channel::Alpha:
                Wire(layer->mapAlpha, channel.id, track.interp);
                return;
            case Channel::Color:
                Wire(layer->color, channel.id, track.interp);
                return;
            case Channel::TextureIndex:
                Wire(layer->currentFrame, channel.id, track.interp);
                return;
            case Channel::Weight:
                Wire(layer->rgbMultiply, channel.id, track.interp);
                return;
            default:
                return;
            }
        }
        switch (channel.target.channel) {
        case Channel::UvTranslate:
            Wire(layer->uvOffset, channel.id, track.interp);
            return;
        case Channel::UvRotate:
            Wire(layer->uvAngle, channel.id, track.interp);
            return;
        case Channel::UvScale:
            Wire(layer->uvTiling, channel.id, track.interp);
            return;
        default:
            return;
        }
    }

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

    const Document& document_;
    const Model& model_;
    u32 modelIndex_;
    const ExportContext& context_;
    m3::Model& out_;
    Diagnostics& diagnostics_;
    std::vector<std::vector<u32>> materialOrdinals_;
};

} // namespace

void Export(const Document& document, u32 model, const ExportContext& context, m3::Model& out,
            Diagnostics& diagnostics) {
    if (model >= document.models.size()) {
        return;
    }
    Exporter(document, model, context, out, diagnostics).run();
}

} // namespace m3_anim
} // namespace wem
} // namespace models
} // namespace whiteout

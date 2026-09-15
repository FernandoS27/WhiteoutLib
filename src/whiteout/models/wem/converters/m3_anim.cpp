// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_anim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include <whiteout/models/wem/anim/clip.h>

#include "../materials/m3_core.h"
#include "m3_track_sink.h"

namespace whiteout {
namespace models {
namespace wem {
namespace m3_anim {

namespace {


/// STC `animRefs[j]` packs the slot in the high half and the block index in the
/// low one — the 13 typed `AnimBlock` arrays are addressed no other way.
/// An `STS_` record's four link fields at rest: parent, next sibling and first
/// child all -1, then a -1 `i16` and a zero `u16` (§7.4). Every shipped record
/// carries exactly this.
constexpr std::array<u8, 16> kAnimationStateLinks = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
};

/// The id the slot-0 event stream joins under. It is not free: across 2,044
/// shipped models every slot-0 stream carries one of exactly two ids, and no
/// model mixes them — 1,517 use this one, 527 the older `0x65BD3215`. An id the
/// client does not know is an event stream it never finds, and `0` — what this
/// export used to write — is the format's own "not animated" sentinel.
constexpr u32 kEventStreamAnimId = 0x063F6D59u;

/// Every animation ends with this event, and the rule has no exceptions: it is
/// the last key of all 2,378 slot-0 blocks measured, and all 1,209 StarCraft II
/// and 671 Heroes models carrying sequences have one per sequence -- always
/// type 4, always bone 0xFFFF, keyed at the sequence's end frame. It is how the
/// engine learns a clip is over, so a stream without it is a clip that never
/// ends; 18 of this export's 27 had no slot-0 stream at all.
constexpr char kSequenceEndEvent[] = "Evt_SeqEnd";
constexpr u32 kSequenceEndType = 4u;
constexpr u16 kNoBone = 0xFFFFu;

/// The type a named event crosses under. `0` -- what a Warcraft III event
/// object carries, having no type of its own -- appears in none of the 2,426
/// shipped events; every one that is not the end marker is type 2.
constexpr u32 kNamedEventType = 2u;

/// The name a container crosses under. Formats with no container of their own
/// (MDX, M2, Diablo III) all name theirs `base`, so every clip would export a
/// container by the same name; retail qualifies each with its sequence
/// (`Walk_full`, `Attack_full`). A container already named for its clip — an
/// `.m3` making the round trip — keeps the name it came with.
std::string containerName(const std::string& container, const std::string& clip) {
    if (clip.empty() || container.rfind(clip, 0) == 0) {
        return container;
    }
    return container.empty() ? clip : clip + "_" + container;
}

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
            SetClipMoveSpeed(clip, sequence.moveSpeed);
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
        // Everything `Import` keeps off a SEQS record, kept here too: an
        // external sequence is a sequence, and a reader that asked what its
        // blend time was should not get a different answer for having found it
        // in the other file.
        clip.native.set("m3SeqFlags", static_cast<i64>(static_cast<u32>(sequence.flags)));
        clip.native.set("m3Frequency", static_cast<i64>(sequence.frequency));
        SetClipMoveSpeed(clip, sequence.moveSpeed);
        clip.native.set("blendTime", static_cast<i64>(sequence.blendTime));
        clip.native.set("startFrame", static_cast<i64>(sequence.startFrame));
        clip.native.set("external", static_cast<i64>(1));
        // The posed bound the file states for this sequence. Without it the
        // clip falls back on the model's own, which is the bind pose's — safe,
        // because it is the larger, and wrong by however much the motion
        // leaves it.
        clip.bounds = RebaseExtent(sequence.bounds);

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
//     nothing else does — but `interpType` has to agree with it here, because
//     the loader reads the file's value once and folds it into that same bit.
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

using m3_sink::Ticks;
using m3_sink::WarcraftWindow;

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


// ---- UV-animation conversion (M2/WC3 -> the layer's own transform) ---------

constexpr f32 kPi = 3.14159265358979323846f;

/// Whether @p channel keys a `UvAnimation` feature in a SOURCE convention: a
/// three-float translate, a quaternion rotate, a three-float scale -- M2 and
/// MDX's spelling. An `.m3`-sourced channel already keys the layer's native
/// pair/triple (F32x2 offset, F32x3 angle, F32x2 tiling) and crosses verbatim.
/// The source spellings cannot: the value would land in the wrong SD stream
/// for the layer's AnimRef, and the two conventions disagree about the SIGN of
/// the translation -- WoW and Warcraft III ADD it inside the (0.5, 0.5) pivot,
/// StarCraft II SUBTRACTS it inside the same pivot -- so the group converts
/// through `UvCompose` (`convertUvTracks`).
bool NeedsUvConversion(const AnimChannel& channel) {
    if (channel.target.kind != TrackTarget::Kind::MaterialFeature) {
        return false;
    }
    switch (channel.target.channel) {
    case Channel::UvTranslate:
    case Channel::UvScale:
        return channel.valueType == geom::AttrType::F32x3;
    case Channel::UvRotate:
        return channel.valueType == geom::AttrType::Quat;
    default:
        return false;
    }
}

/// Key @p key of @p track as @p comps floats -- the value element only. A
/// Hermite or Bezier key stores tangents beside it, and no M3 stream carries
/// tangents; `writeStream` already drops them for every channel it writes.
void UvKeyValue(const SubTrack& track, u32 comps, std::size_t key, f32* out) {
    const std::size_t stride = static_cast<std::size_t>(ValuesPerKey(track.interp)) * comps;
    std::memcpy(out, track.values.data() + key * stride * sizeof(f32),
                static_cast<std::size_t>(comps) * sizeof(f32));
}

/// One instant of the M2/WC3 texture transform, restated in the M3 layer's
/// vocabulary.
///
/// The source flattens to `uv' = ((uv + t - 0.5) . S . R) + 0.5`
/// (CM2Model::AnimateTextureTransformMT; Warcraft III's `AnimateTextureMap`
/// takes the same shape), whose column-form linear block is a rotation times
/// the scale -- so the angle and the tiling read off it directly. StarCraft II
/// composes `uv' = tiling . R . (uv - 0.5 - offset) + 0.5` (`sub_102ABBDE0`,
/// `M3ComposeUvTransform`): the SAME centre pivot with the translation on the
/// same side of it, only negated. So the offset is the source's own
/// translation with its sign turned over -- exactly, whatever the rotation and
/// the scale, since both engines apply it in source space ahead of the linear
/// block.
///
/// Composing the pivot into the offset, which is what this did while the M3
/// transform was thought to be origin-pivoted, scrolled every crossed layer
/// backwards: the Infernal's four flipbook tracks stepped through the mirrored
/// cell of its 4x4 atlas and the Necropolis' ghosts slid the wrong way.
///
/// What does NOT cross is the ORDER -- Warcraft III scales then rotates and
/// StarCraft II rotates then scales, which agree only under a uniform tiling.
/// `convertUvTracks` reports the rest.
void UvCompose(const f32 t[3], const f32 q[4], const f32 s[3], f32& angle, Vector2f& tiling,
               Vector2f& offset) {
    const f32 xx = q[0] * q[0];
    const f32 yy = q[1] * q[1];
    const f32 zz = q[2] * q[2];
    const f32 xy = q[0] * q[1];
    const f32 wz = q[3] * q[2];
    const f32 m00 = s[0] * (1.0f - 2.0f * (yy + zz));
    const f32 m01 = s[1] * 2.0f * (xy - wz);
    const f32 m10 = s[0] * 2.0f * (xy + wz);
    const f32 m11 = s[1] * (1.0f - 2.0f * (xx + zz));
    angle = std::atan2(m10, m00);
    const f32 c = std::cos(angle);
    const f32 sn = std::sin(angle);
    tiling = Vector2f{c * m00 + sn * m10, c * m11 - sn * m01};
    offset = Vector2f{-t[0], -t[1]};
}

class Exporter {
public:
    Exporter(const Document& document, u32 modelIndex, const ExportContext& context, m3::Model& out,
             Diagnostics& diagnostics, M3ExportMap* map)
        : document_(document), model_(document.models[modelIndex]), modelIndex_(modelIndex),
          context_(context), out_(out), diagnostics_(diagnostics), map_(map) {}

    void run() {
        buildMaterialOrdinals();
        if (map_ != nullptr) {
            map_->clipSequence.assign(document_.clips.size(), kInvalidIndex);
            map_->sequenceStc.clear();
        }
        for (std::size_t c = 0; c < document_.clips.size(); ++c) {
            if (document_.clips[c].model == modelIndex_) {
                if (map_ != nullptr) {
                    map_->clipSequence[c] = static_cast<u32>(out_.sequences.size());
                }
                buildClip(document_.clips[c]);
            }
        }
    }

private:
    using Stream = m3_sink::Stream;

    /// The slot a channel's values belong in.
    ///
    /// The one judgement here is `F32`: a **flag** goes back to SDFG (slot 11,
    /// where every keyed visibility in shipped content lives) and a real scalar
    /// to SDR3. Which it is, is a property of the channel — `Visibility` is the
    /// only F32 the import read out of SDFG.
    Stream StreamFor(const AnimChannel& channel) const {
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
            if (channel.target.channel == Channel::Visibility) {
                return Stream::Sdfg;
            }
            // A channel a gate bone reads is a visibility in alpha's
            // clothing, whatever its target kind — a section fade or a
            // material element alpha alike; the flag stream is the slot its
            // AnimRef joins.
            if (context_.sectionGateBones.count(channel.id) != 0) {
                return Stream::Sdfg;
            }
            return Stream::Sdr3;
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

    /// A sub-track re-emitted in the layer's own vocabulary, under the
    /// channel copy it is written and wired as.
    struct ConvertedTrack {
        AnimChannel channel;
        SubTrack track;
    };

    /// The container's section colour tracks a tint carrier reads
    /// (`sectionColorLayers`), widened to the RGBA its SDCC stream holds: the
    /// colour at each key, opaque, and linear where the source was smooth.
    std::vector<ConvertedTrack> convertTintTracks(const SubTrackContainer& source,
                                                  std::set<u32>& consumed) {
        std::vector<ConvertedTrack> out;
        for (const SubTrack& track : source.subTracks) {
            const AnimChannel* channel = model_.animChannels.find(track.channel);
            if (channel == nullptr || channel->target.kind != TrackTarget::Kind::Section ||
                channel->target.channel != Channel::Color ||
                channel->valueType != geom::AttrType::F32x3 ||
                context_.sectionColorLayers.count(channel->id) == 0 ||
                !track.wellSized(channel->valueType)) {
                continue;
            }
            consumed.insert(channel->id);
            ConvertedTrack converted;
            converted.channel = *channel;
            converted.channel.valueType = geom::AttrType::F32x4;
            converted.channel.initValue.clear();
            converted.track.channel = channel->id;
            converted.track.interp =
                track.interp == Interpolation::Step ? Interpolation::Step : Interpolation::Linear;
            const std::size_t stride = ValuesPerKey(track.interp) * 3 * sizeof(f32);
            for (std::size_t k = 0; k < track.times.size(); ++k) {
                f32 rgb[3] = {1.0f, 1.0f, 1.0f};
                std::memcpy(rgb, track.values.data() + k * stride, sizeof(rgb));
                converted.track.times.push_back(track.times[k]);
                const f32 rgba[4] = {rgb[0], rgb[1], rgb[2], 1.0f};
                const u8* bytes = reinterpret_cast<const u8*>(rgba);
                converted.track.values.insert(converted.track.values.end(), bytes,
                                              bytes + sizeof(rgba));
            }
            out.push_back(std::move(converted));
        }
        return out;
    }

    /// The container's convertible UV feature channels (`NeedsUvConversion`),
    /// regrouped per feature into offset / angle / tiling tracks. Each becomes
    /// the layer field it names: the angle is the quaternion's in-plane
    /// rotation unwrapped across keys so a spin that crosses +-pi keeps
    /// turning, the tiling is the scale, and the offset is the translation
    /// negated (`UvCompose`) -- the pivot is the same on both sides, so it
    /// needs no arithmetic and a turn on its own does not move the offset.
    ///
    /// Ids the caller must not also write land in @p consumed.
    std::vector<ConvertedTrack> convertUvTracks(const SubTrackContainer& source,
                                                std::set<u32>& consumed) {
        struct Group {
            const AnimChannel* channel[3] = {nullptr, nullptr, nullptr};
            const SubTrack* track[3] = {nullptr, nullptr, nullptr};
        };
        std::map<std::tuple<u32, u32, u32>, Group> groups;
        for (const SubTrack& track : source.subTracks) {
            const AnimChannel* channel = model_.animChannels.find(track.channel);
            if (channel == nullptr || !NeedsUvConversion(*channel) ||
                channel->target.material.profile != profile() ||
                !track.wellSized(channel->valueType)) {
                continue;
            }
            const int part = channel->target.channel == Channel::UvTranslate ? 0
                             : channel->target.channel == Channel::UvRotate  ? 1
                                                                             : 2;
            Group& group = groups[std::make_tuple(channel->target.material.slot,
                                                  channel->target.material.look,
                                                  channel->target.sub)];
            group.channel[part] = channel;
            group.track[part] = &track;
            consumed.insert(channel->id);
        }

        std::vector<ConvertedTrack> out;
        const auto pushF32 = [](std::vector<u8>& values, std::initializer_list<f32> parts) {
            for (const f32 part : parts) {
                const u8* bytes = reinterpret_cast<const u8*>(&part);
                values.insert(values.end(), bytes, bytes + sizeof(f32));
            }
        };
        for (const auto& entry : groups) {
            const Group& group = entry.second;
            const SubTrack* trans = group.track[0];
            const SubTrack* rot = group.track[1];
            const SubTrack* scale = group.track[2];

            // The one part that does not cross: WoW and Warcraft III scale
            // then turn, StarCraft II turns then scales, and the two agree
            // only where the tiling is uniform. 13 of the Warcraft III
            // corpus' 213 moving texture animations pair a turn with an
            // uneven scale; the fit below takes the angle and squares the
            // tiling off to it.
            if (rot != nullptr && scale != nullptr) {
                for (std::size_t k = 0; k < scale->times.size(); ++k) {
                    f32 v[3];
                    UvKeyValue(*scale, 3, k, v);
                    if (std::fabs(v[0] - v[1]) > 1e-4f) {
                        diagnostics_.warn(
                            DiagCode::AnimTrackApproximated,
                            "a turning UV layer tiles its axes differently; StarCraft II "
                            "scales AFTER the turn and has no spelling for it",
                            ElementRef(ElementKind::Slot, std::get<0>(entry.first)), profile());
                        break;
                    }
                }
            }

            // Angle: at the rotation's own keys, unwrapped so a spin that
            // crosses +-pi keeps turning instead of snapping back around.
            if (rot != nullptr) {
                ConvertedTrack converted;
                converted.channel = *group.channel[1];
                converted.channel.valueType = geom::AttrType::F32x3;
                converted.channel.initValue.clear();
                converted.track.channel = converted.channel.id;
                converted.track.interp = rot->interp == Interpolation::Step
                                             ? Interpolation::Step
                                             : Interpolation::Linear;
                f32 previous = 0.0f;
                for (std::size_t k = 0; k < rot->times.size(); ++k) {
                    f32 q[4];
                    UvKeyValue(*rot, 4, k, q);
                    constexpr f32 kZero[3] = {0.0f, 0.0f, 0.0f};
                    constexpr f32 kUnit[3] = {1.0f, 1.0f, 1.0f};
                    f32 angle = 0.0f;
                    Vector2f tiling{};
                    Vector2f offset{};
                    UvCompose(kZero, q, kUnit, angle, tiling, offset);
                    if (k > 0) {
                        while (angle - previous > kPi) {
                            angle -= 2.0f * kPi;
                        }
                        while (previous - angle > kPi) {
                            angle += 2.0f * kPi;
                        }
                    }
                    previous = angle;
                    converted.track.times.push_back(rot->times[k]);
                    pushF32(converted.track.values, {0.0f, 0.0f, angle});
                }
                out.push_back(std::move(converted));
            }

            // Tiling: the scale, at its own keys.
            if (scale != nullptr) {
                ConvertedTrack converted;
                converted.channel = *group.channel[2];
                converted.channel.valueType = geom::AttrType::F32x2;
                converted.channel.initValue.clear();
                converted.track.channel = converted.channel.id;
                converted.track.interp = scale->interp == Interpolation::Step
                                             ? Interpolation::Step
                                             : Interpolation::Linear;
                for (std::size_t k = 0; k < scale->times.size(); ++k) {
                    f32 v[3];
                    UvKeyValue(*scale, 3, k, v);
                    converted.track.times.push_back(scale->times[k]);
                    pushF32(converted.track.values, {v[0], v[1]});
                }
                out.push_back(std::move(converted));
            }

            // Offset: the source's own translation, negated. Both engines
            // apply it in source space ahead of the linear block, so a
            // rotation or a scale key no longer moves it -- and a group with
            // no translate track leaves the layer's AnimRef resting where the
            // material import already put it.
            if (trans == nullptr) {
                continue;
            }
            ConvertedTrack converted;
            converted.channel = *group.channel[0];
            converted.channel.valueType = geom::AttrType::F32x2;
            converted.channel.initValue.clear();
            converted.track.channel = converted.channel.id;
            converted.track.interp =
                trans->interp == Interpolation::Step ? Interpolation::Step : Interpolation::Linear;
            for (std::size_t k = 0; k < trans->times.size(); ++k) {
                f32 t[3];
                UvKeyValue(*trans, 3, k, t);
                converted.track.times.push_back(trans->times[k]);
                pushF32(converted.track.values, {-t[0], -t[1]});
            }
            out.push_back(std::move(converted));
        }
        return out;
    }

    void buildClip(const Clip& clip) {
        m3::Sequence sequence;
        sequence.name = clip.name;
        // The four fields no shipped sequence varies: 3,109 of 3,109 `SEQS` v2
        // records carry id and index -1, replay 1/1 and a 100ms blend. Only a
        // source that stated its own is asked (an `.m3` keeps both in the bag);
        // a conversion that invented 0 for all of them was stating "sequence
        // zero, replay nothing, blend instantly".
        sequence.id = static_cast<i32>(clip.native.value("sequenceId", -1));
        sequence.index = -1;
        sequence.replayStart = 1;
        sequence.replayEnd = 1;
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
        const i64 rarity = static_cast<i64>(ClipRarity(clip));
        sequence.frequency =
            static_cast<u32>(clip.native.value("m3Frequency", rarity != 0 ? rarity : 100));
        sequence.moveSpeed = ClipMoveSpeed(clip);
        sequence.blendTime = static_cast<u32>(clip.native.value("blendTime", 100));
        const i32 origin = static_cast<i32>(clip.native.value("startFrame", 0));
        sequence.startFrame = static_cast<u32>(origin);
        sequence.endFrame = static_cast<u32>(origin + Ticks(clip.duration));
        sequence.bounds = UnrebaseExtent(HasExtent(clip.bounds) ? clip.bounds : model_.bounds);

        m3::AnimationGroup group;
        group.name = clip.name;
        // Every clip owes a slot-0 stream, whether or not it carries events of
        // its own, because the end marker lives in it.
        bool eventsPending = true;
        for (const SubTrackContainer& container : clip.containers) {
            const u32 index = buildContainer(container, clip, origin, eventsPending);
            if (index != kInvalidIndex) {
                group.subtrackIndices.push_back(index);
                eventsPending = false;
            }
        }

        if (map_ != nullptr) {
            map_->sequenceStc.push_back(group.subtrackIndices.empty()
                                            ? kInvalidIndex
                                            : group.subtrackIndices.front());
        }
        out_.sequences.push_back(std::move(sequence));
        out_.animationGroups.push_back(std::move(group));
    }

    u32 buildContainer(const SubTrackContainer& source, const Clip& clip, i32 origin,
                       bool takeEvents) {
        m3::SubTrackContainer stc;
        stc.name = containerName(source.name, clip.name);
        stc.animPriority = static_cast<u16>(source.priority);
        stc.runsConcurrent = source.concurrent ? 1u : 0u;
        stc.animationStateIndex = static_cast<u16>(source.native.value("animationStateIndex", 0));
        stc.animationStateIndexCopy = stc.animationStateIndex;
        stc.unknown = 0;

        // Source-convention UV feature channels become the layer's own
        // offset/angle/tiling tracks; the originals must not ALSO be
        // written, or the STC carries a quaternion no Vector2 AnimRef reads.
        std::set<u32> convertedIds;
        std::vector<ConvertedTrack> convertedUv = convertUvTracks(source, convertedIds);
        for (ConvertedTrack& tint : convertTintTracks(source, convertedIds)) {
            convertedUv.push_back(std::move(tint));
        }
        const bool warcraft = WarcraftWindow(clip);

        for (const SubTrack& track : source.subTracks) {
            const AnimChannel* channel = model_.animChannels.find(track.channel);
            if (channel == nullptr) {
                continue;
            }
            // A material channel of another profile is the source's spelling
            // of a track the derive twinned for this one: nothing wires it,
            // so writing it would only orphan a stream in the STC.
            if (IsMaterialTarget(channel->target.kind) &&
                channel->target.material.profile != profile()) {
                continue;
            }
            if (convertedIds.count(channel->id) != 0) {
                continue;
            }
            if (!track.wellSized(channel->valueType)) {
                diagnostics_.warn(DiagCode::AnimTrackDropped,
                                  "a sub-track of clip '" + clip.name +
                                      "' is not sized for its channel",
                                  ElementRef(ElementKind::Track, channel->id), profile());
                continue;
            }
            const u32 animRef =
                writeStream(stc, *channel, track, origin, clip.duration, warcraft);
            if (animRef == kInvalidIndex) {
                continue;
            }
            stc.animIds.push_back(exportId(channel->id));
            stc.animRefs.push_back(animRef);
            wireAnimRef(*channel, track);
        }

        for (const ConvertedTrack& entry : convertedUv) {
            const u32 animRef =
                writeStream(stc, entry.channel, entry.track, origin, clip.duration, warcraft);
            if (animRef == kInvalidIndex) {
                continue;
            }
            stc.animIds.push_back(exportId(entry.channel.id));
            stc.animRefs.push_back(animRef);
            wireAnimRef(entry.channel, entry.track);
        }

        // Events are the slot-0 stream, and they belong to the CLIP rather than
        // to a container — so they go into the first container that is written
        // and not into every one of them, which is how a three-layer clip would
        // otherwise fire each of its events three times.
        if (takeEvents) {
            const u32 animRef = writeEvents(stc, clip, origin);
            if (animRef != kInvalidIndex) {
                stc.animIds.push_back(kEventStreamAnimId);
                stc.animRefs.push_back(animRef);
            }
        }

        if (stc.animRefs.empty()) {
            return kInvalidIndex;
        }

        // `animIds` and `animRefs` are one lookup table in two arrays, and
        // StarCraft II searches it: every shipped container keeps it sorted by
        // id — 1,221 of 1,221 measured across 600 models. Ours came out in the
        // order the channels happened to be written.
        m3_sink::SortLookup(stc);

        // Every container names an animation state, and the index it named was
        // whatever the source said with nothing behind it: this export wrote no
        // `STS_` at all. The record is the same id list read a second way, one
        // per container (972 of 1,002 shipped records are exactly their
        // container's list), and its four link fields rest at "no parent, no
        // sibling, no child".
        m3::AnimationState state;
        state.animIds = stc.animIds;
        state.unknown = kAnimationStateLinks;
        stc.animationStateIndex = static_cast<u16>(out_.animationStates.size());
        stc.animationStateIndexCopy = stc.animationStateIndex;
        out_.animationStates.push_back(std::move(state));

        out_.subTrackCollections.push_back(std::move(stc));
        return static_cast<u32>(out_.subTrackCollections.size() - 1);
    }

    ProfileId profile() const {
        return context_.profile;
    }

    /// Fills the typed block and returns `(slot << 16) | block`. @p warcraft
    /// says the clip is a Warcraft III window (`WarcraftWindow`).
    u32 writeStream(m3::SubTrackContainer& stc, const AnimChannel& channel, const SubTrack& track,
                    i32 origin, f32 duration, bool warcraft) {
        m3_sink::StreamSpec spec;
        spec.stream = StreamFor(channel);
        spec.type = channel.valueType;
        spec.unrebaseVector = RebasesVector(channel);
        spec.unrebaseQuaternion = RebasesQuaternion(channel);
        Quaternion rest{};
        if (channel.valueType == geom::AttrType::Quat && channel.hasInitValue()) {
            std::memcpy(&rest, channel.initValue.data(), sizeof(rest));
            rest = spec.unrebaseQuaternion ? UnrebaseRotation(rest) : rest;
            spec.restQuaternion = &rest;
        }
        return m3_sink::WriteStream(stc, spec, track, origin, duration, warcraft);
    }

    u32 writeEvents(m3::SubTrackContainer& stc, const Clip& clip, i32 origin) {
        m3::AnimBlock<m3::Event> entry;
        for (const ClipEvent& event : clip.events) {
            // An `.m3` that made the round trip brings its own end marker back
            // as a clip event; the one appended below replaces it rather than
            // joining it, or the block would end with two.
            if (event.name == kSequenceEndEvent) {
                continue;
            }
            m3::Event key{};
            key.name = event.name;
            key.eventType = event.value != 0 ? event.value : kNamedEventType;
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
        m3::Event end{};
        end.name = kSequenceEndEvent;
        end.eventType = kSequenceEndType;
        end.boneIndex = kNoBone;
        entry.timestamps.push_back(origin + Ticks(clip.duration));
        entry.keys.push_back(std::move(end));

        // A block of one key is flagged, and one of several is not: 2,317 of
        // 2,317 single-key blocks carry 1 and all 61 longer ones carry 0.
        entry.flags = entry.keys.size() == 1 ? 1u : 0u;
        entry.endFrame = static_cast<u32>(entry.timestamps.back());
        const u32 block = static_cast<u32>(stc.sdev.size());
        stc.sdev.push_back(std::move(entry));
        return block; // slot 0, so the reference is the block index itself
    }

    // ---- the AnimRefs the STC's ids have to match ---------------------------

    /// The id a channel crosses into the `.m3` under. `animId == 0` means "not
    /// animated" to every consumer of the format, so a channel that arrived
    /// with id 0 -- a document whose importer numbers channels from zero, as
    /// MDX's does -- would join every DEFAULTED AnimRef in the file to its
    /// keys: all 49 of a Footman's bones posed by the one track that drew id
    /// 0, a pile of parts at the origin. Remapped here, at the boundary;
    /// every nonzero id crosses verbatim (an `.m3` round trip keeps its
    /// hashes).
    u32 exportId(u32 channelId) const {
        if (channelId != 0) {
            return channelId;
        }
        if (zeroRemap_ == 0) {
            u32 next = 1;
            for (const AnimChannel& entry : model_.animChannels.channels) {
                next = (std::max)(next, entry.id + 1);
            }
            zeroRemap_ = next;
        }
        return zeroRemap_;
    }
    mutable u32 zeroRemap_ = 0;

    /// Binding an AnimRef is two statements, and this used to make neither.
    ///
    /// `flags` says the property IS answered by a track -- `Wiring::kBound`,
    /// see SolveBoneAnimFlags for what reads it and why a zero here freezes
    /// the whole model. `interpType` is the step half: the per-chunk load
    /// fixup folds it in as `if (interpType == 0) flags |= 0x10` and then
    /// overwrites the row with a track-table slot, which is why the import
    /// must never read it back and why a bound ref never states bit 4 itself.
    ///
    /// One AnimRef serves every sequence, so the step stands only while EVERY
    /// clip's track holds -- a clip that moves clears it for good, whichever
    /// order they are wired (`m3_sink::Wiring`).
    template <class T>
    void Wire(m3::AnimRef<T>& ref, u32 animId, Interpolation interp) {
        wiring_.wire(ref, animId, interp);
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
        case TrackTarget::Kind::Section: {
            // A tint: the carrier emissive's colour (`convertTintTracks`
            // widened the stream to its RGBA).
            if (channel.target.channel == Channel::Color) {
                const auto tint = context_.sectionColorLayers.find(channel.id);
                if (tint == context_.sectionColorLayers.end()) {
                    return;
                }
                for (const auto& [matIndex, which] : tint->second) {
                    if (matIndex >= out_.standardMaterials.size()) {
                        continue;
                    }
                    m3::StandardMaterial& mat = out_.standardMaterials[matIndex];
                    std::optional<m3::TextureLayer>& slot =
                        which == 1 ? mat.emissiveLayer1 : mat.emissiveLayer2;
                    if (slot.has_value()) {
                        Wire(slot->color, exportId(channel.id), track.interp);
                    }
                }
                return;
            }
            // The stream is already in the STC under this channel's id; the
            // gate bone's AnimRef is the one reader (see ExportContext).
            // Warcraft III spells section visibility as an ALPHA track, so
            // both spellings reach the gate.
            if (channel.target.channel != Channel::Visibility &&
                channel.target.channel != Channel::Alpha) {
                return;
            }
            const auto gate = context_.sectionGateBones.find(channel.id);
            if (gate != context_.sectionGateBones.end() && gate->second < out_.bones.size()) {
                Wire(out_.bones[gate->second].visibility, exportId(channel.id), track.interp);
            }
            const auto fade = context_.sectionAlphaLayers.find(channel.id);
            if (fade != context_.sectionAlphaLayers.end()) {
                for (const auto& [matIndex, which] : fade->second) {
                    if (matIndex >= out_.standardMaterials.size()) {
                        continue;
                    }
                    m3::StandardMaterial& mat = out_.standardMaterials[matIndex];
                    std::optional<m3::TextureLayer>& slot =
                        which == 1 ? mat.alphaLayer1 : mat.alphaLayer2;
                    if (slot.has_value()) {
                        Wire(slot->mapAlpha, exportId(channel.id), track.interp);
                    }
                }
            }
            return;
        }
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
        // The Warcraft III carriers (`M3ExportSettings::effectNodeBones`): a
        // visibility goes to the bone `toM3` chose for it -- a `_Vis` leaf
        // where the node has children -- and a light's or a camera's transform
        // to the bone that carries its record.
        const u32 carrier = node < context_.nodeBone.size() ? context_.nodeBone[node] : kInvalidIndex;
        const u32 visCarrier =
            node < context_.nodeVisBone.size() ? context_.nodeVisBone[node] : kInvalidIndex;
        if (channel.target.channel == Channel::Visibility && visCarrier < out_.bones.size()) {
            Wire(out_.bones[visCarrier].visibility, exportId(channel.id), track.interp);
            return;
        }
        if (carrier < out_.bones.size() && slot.slot != ExportContext::Slot::Bone) {
            m3::Bone& bone = out_.bones[carrier];
            switch (channel.target.channel) {
            case Channel::Translation:
                Wire(bone.position, exportId(channel.id), track.interp);
                return;
            case Channel::Rotation:
                Wire(bone.rotation, exportId(channel.id), track.interp);
                return;
            case Channel::Scale:
                Wire(bone.scale, exportId(channel.id), track.interp);
                return;
            default:
                break;
            }
        }
        if (slot.slot == ExportContext::Slot::Bone && slot.index < out_.bones.size()) {
            m3::Bone& bone = out_.bones[slot.index];
            switch (channel.target.channel) {
            case Channel::Translation:
                Wire(bone.position, exportId(channel.id), track.interp);
                return;
            case Channel::Rotation:
                Wire(bone.rotation, exportId(channel.id), track.interp);
                return;
            case Channel::Scale:
                Wire(bone.scale, exportId(channel.id), track.interp);
                return;
            case Channel::Visibility:
                Wire(bone.visibility, exportId(channel.id), track.interp);
                return;
            default:
                return;
            }
        }
        if (slot.slot == ExportContext::Slot::Light && slot.index < out_.lights.size()) {
            m3::Light& light = out_.lights[slot.index];
            // `sub` 1 is a Warcraft III or World of Warcraft light's AMBIENT
            // term, which a LITE has no field for; wiring it onto the diffuse
            // made the ambient colour the light's colour.
            if (channel.target.sub != 0) {
                if (ambientDropped_.insert(channel.id).second) {
                    diagnostics_.info(DiagCode::AnimTrackDropped,
                                      "a light's ambient term has no LITE field",
                                      ElementRef(ElementKind::Channel, channel.id), profile());
                }
                return;
            }
            switch (channel.target.channel) {
            case Channel::Color:
                Wire(light.diffuseColor, exportId(channel.id), track.interp);
                return;
            case Channel::Intensity:
                Wire(light.intensityMultiplier, exportId(channel.id), track.interp);
                return;
            case Channel::AttenuationStart:
                Wire(light.attenuationStart, exportId(channel.id), track.interp);
                return;
            case Channel::AttenuationEnd:
                // The far attenuation is the AnimRef the struct calls `decay`
                // (its init is the plain float beside it on every shipped
                // light).
                Wire(light.decay, exportId(channel.id), track.interp);
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
            // The export's own report is exact; the re-import below is the
            // guess it replaces, kept for native-block materials that never
            // fill one.
            if (m < context_.materialOrdinals.size() && !context_.materialOrdinals[m].empty()) {
                materialOrdinals_[m] = context_.materialOrdinals[m];
                continue;
            }
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
        // A binary material alpha gates its batches through a gate bone
        // (`toM3` plants one exactly as it does for a section visibility --
        // WoW's element-alpha hide idiom targets the MATERIAL). The bone's
        // AnimRef still has to be wired here, because the channel arrives as
        // a MaterialLayer target, never a Section one.
        const auto gate = context_.sectionGateBones.find(channel.id);
        if (gate != context_.sectionGateBones.end() && gate->second < out_.bones.size()) {
            Wire(out_.bones[gate->second].visibility, exportId(channel.id), track.interp);
        }
        // WoW's whole-batch tint and fade (`M2Color` times the unit-0 weight
        // — the element-alpha product) target the material, not a stage. M3
        // has no whole-material alpha; the spelling Blizzard's own
        // conversions use is a Color-flag alpha layer whose `mapAlpha`
        // carries the keys — the same carrier the section fades ride.
        if (channel.target.kind == TrackTarget::Kind::MaterialLayer &&
            channel.target.sub == kWholeMaterial) {
            for (const SectionRef& section : sectionsOf(ref.slot)) {
                m3::StandardMaterial& mat = out_.standardMaterials[section.standard];
                if (channel.target.channel == Channel::Alpha) {
                    wireAlphaCarrier(mat, section.standard, channel, track);
                } else if (channel.target.channel == Channel::Color &&
                           mat.diffuseLayer.has_value()) {
                    // The rest crosses into the layer's initValue — a static
                    // `M2Color` is most of them (the Earth Spirit's near-black
                    // body is a WHITE texture under a dark constant tint), and
                    // a consumer that samples no layer tracks reads only this.
                    if (colorSeeded_.insert(channel.id).second &&
                        track.values.size() >= sizeof(f32) * 3) {
                        f32 rest[3] = {1.0f, 1.0f, 1.0f};
                        std::memcpy(rest, track.values.data(), sizeof(rest));
                        m3::ColorBGRA& c = mat.diffuseLayer->color.initValue;
                        c.r = static_cast<u8>((std::min)(rest[0], 1.0f) * 255.0f);
                        c.g = static_cast<u8>((std::min)(rest[1], 1.0f) * 255.0f);
                        c.b = static_cast<u8>((std::min)(rest[2], 1.0f) * 255.0f);
                    }
                    Wire(mat.diffuseLayer->color, exportId(channel.id), track.interp);
                }
            }
            return;
        }
        const u32 ordinal = channel.target.kind == TrackTarget::Kind::MaterialLayer
                                ? channel.target.sub
                                : featureLayer(ref, channel.target.sub);
        if (ordinal == kInvalidIndex) {
            return;
        }

        // EVERY StandardLayer that ordinal became — the inverse of the map the
        // re-import just produced, and it is one-to-many: one chain stage
        // routinely lands in two slots (a Diablo III flame is emissive1 AND
        // alpha1), and wiring only the first scrolled the colour against a
        // frozen mask — Imperius's wings slid into red sheets. A composite
        // slot's map is section-major, `StandardLayer::Count` per section.
        const std::vector<u32>& ordinals = materialOrdinals_[ref.slot];
        const std::size_t count = static_cast<std::size_t>(m3_core::StandardLayer::Count);
        const auto coverageSwitch = context_.coverageSwitches.find(ref.slot);
        for (const SectionRef& section : sectionsOf(ref.slot)) {
        m3::StandardMaterial& mat = out_.standardMaterials[section.standard];
        for (std::size_t layerSlot = 0;
             layerSlot < count && section.ordinalBase + layerSlot < ordinals.size(); ++layerSlot) {
            if (ordinals[section.ordinalBase + layerSlot] != ordinal) {
                continue;
            }
            // The element alpha: a weight on the base stage fades the whole
            // draw, and M3 never reads the diffuse layer's alpha as coverage —
            // the track rides a carrier alpha layer instead. Unless the fold
            // made it the live/dead coverage switch: then it drives the
            // coverage layer's add (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5.3 R6b).
            if (channel.target.kind == TrackTarget::Kind::MaterialLayer &&
                channel.target.channel == Channel::Alpha &&
                static_cast<m3_core::StandardLayer>(layerSlot) ==
                    m3_core::StandardLayer::Diffuse) {
                if (section.ordinalBase == 0 && coverageSwitch != context_.coverageSwitches.end() &&
                    coverageSwitch->second == ordinal && mat.alphaLayer1.has_value()) {
                    Wire(mat.alphaLayer1->rgbAdd, exportId(channel.id), track.interp);
                    continue;
                }
                wireAlphaCarrier(mat, section.standard, channel, track);
                continue;
            }
            std::optional<m3::TextureLayer>& layer =
                m3_core::MutableLayerOf(mat, static_cast<m3_core::StandardLayer>(layerSlot));
            if (!layer.has_value()) {
                continue;
            }
            // The coverage mask a fold copied from this diffuse samples its UVs,
            // so it takes the UV and frame tracks too -- never the fade, which
            // already rides a carrier.
            m3::TextureLayer* const mask =
                maskCopyOf(mat, ordinals, section.ordinalBase, layerSlot);

            if (channel.target.kind == TrackTarget::Kind::MaterialLayer) {
                switch (channel.target.channel) {
                case Channel::Alpha:
                    Wire(layer->mapAlpha, exportId(channel.id), track.interp);
                    continue;
                case Channel::Color:
                    Wire(layer->color, exportId(channel.id), track.interp);
                    continue;
                case Channel::TextureIndex:
                    Wire(layer->currentFrame, exportId(channel.id), track.interp);
                    if (mask != nullptr) {
                        Wire(mask->currentFrame, exportId(channel.id), track.interp);
                    }
                    continue;
                case Channel::Weight:
                    Wire(layer->rgbMultiply, exportId(channel.id), track.interp);
                    continue;
                default:
                    continue;
                }
            }
            switch (channel.target.channel) {
            case Channel::UvTranslate:
                Wire(layer->uvOffset, exportId(channel.id), track.interp);
                if (mask != nullptr) {
                    Wire(mask->uvOffset, exportId(channel.id), track.interp);
                }
                continue;
            case Channel::UvRotate:
                Wire(layer->uvAngle, exportId(channel.id), track.interp);
                if (mask != nullptr) {
                    Wire(mask->uvAngle, exportId(channel.id), track.interp);
                }
                continue;
            case Channel::UvScale:
                Wire(layer->uvTiling, exportId(channel.id), track.interp);
                if (mask != nullptr) {
                    Wire(mask->uvTiling, exportId(channel.id), track.interp);
                }
                continue;
            default:
                continue;
            }
        }
        }
    }

    /// The coverage mask `m3_core`'s fold copied from a section's diffuse -- that
    /// texture on its alpha channel, under no ordinal of its own -- when
    /// @p layerSlot is that diffuse. Wired to the colour alone, a scrolling
    /// Warcraft III blend slid its colour over a frozen mask and drew nothing
    /// (ChineseFairy's slashes).
    m3::TextureLayer* maskCopyOf(m3::StandardMaterial& mat, const std::vector<u32>& ordinals,
                                 std::size_t ordinalBase, std::size_t layerSlot) const {
        if (static_cast<m3_core::StandardLayer>(layerSlot) != m3_core::StandardLayer::Diffuse ||
            !mat.diffuseLayer.has_value() || !mat.alphaLayer1.has_value()) {
            return nullptr;
        }
        const std::size_t alpha1 =
            ordinalBase + static_cast<std::size_t>(m3_core::StandardLayer::Alpha1);
        if (alpha1 < ordinals.size() && ordinals[alpha1] != kInvalidIndex) {
            return nullptr; // a stage of its own, wired through its ordinal
        }
        m3::TextureLayer& mask = *mat.alphaLayer1;
        if (hasFlag(mask.flags, m3::TextureLayerFlag::Color) ||
            mask.colorType != m3::ColorChannelSelect::Alpha ||
            TrimNuls(mask.texturePath) != TrimNuls(mat.diffuseLayer->texturePath)) {
            return nullptr;
        }
        return &mask;
    }

    /// One standard material the slot draws with, and where its ordinals start
    /// in the slot's section-major map.
    struct SectionRef {
        u32 standard = 0;
        std::size_t ordinalBase = 0;
    };

    /// The standard materials behind a slot: one, or a composite's sections.
    std::vector<SectionRef> sectionsOf(u32 slot) const {
        std::vector<SectionRef> result;
        if (slot >= out_.materialMaps.size()) {
            return result;
        }
        const m3::MaterialMap& map = out_.materialMaps[slot];
        const std::size_t count = static_cast<std::size_t>(m3_core::StandardLayer::Count);
        if (map.materialType == m3::MaterialType::Standard) {
            if (map.materialIndex < out_.standardMaterials.size()) {
                result.push_back({map.materialIndex, 0});
            }
        } else if (map.materialType == m3::MaterialType::Composite &&
                   map.materialIndex < out_.compositeMaterials.size()) {
            const auto& sections = out_.compositeMaterials[map.materialIndex].sections;
            for (std::size_t k = 0; k < sections.size(); ++k) {
                const u32 mapIndex = sections[k].materialIndex;
                if (mapIndex < out_.materialMaps.size() &&
                    out_.materialMaps[mapIndex].materialType == m3::MaterialType::Standard &&
                    out_.materialMaps[mapIndex].materialIndex < out_.standardMaterials.size()) {
                    result.push_back({out_.materialMaps[mapIndex].materialIndex, k * count});
                }
            }
        }
        // The copies a differing tint or fade drew (`ExportContext::materialClones`).
        const auto clones = context_.materialClones.find(slot);
        if (clones != context_.materialClones.end()) {
            for (const u32 entry : clones->second) {
                if (entry < out_.materialMaps.size() &&
                    out_.materialMaps[entry].materialType == m3::MaterialType::Standard &&
                    out_.materialMaps[entry].materialIndex < out_.standardMaterials.size()) {
                    result.push_back({out_.materialMaps[entry].materialIndex, 0});
                }
            }
        }
        return result;
    }

    /// A fade with no stage of its own rides a Color-flag carrier in the
    /// first free alpha slot (`cFinal.a = mask1.a * mask2.a`, so two carriers
    /// multiply — which is exactly WoW's colour-times-weight product).
    ///
    /// Once per channel — `wireAnimRef` runs per subtrack per clip, and the
    /// first version of this planted one carrier per clip until the slots ran
    /// out. A channel a gate bone already reads plants nothing: the gate is
    /// the binary spelling, and its stream is flags, not scalars.
    void wireAlphaCarrier(m3::StandardMaterial& mat, u32 standardIndex, const AnimChannel& channel,
                          const SubTrack& track) {
        if (context_.sectionGateBones.count(channel.id) != 0) {
            return;
        }
        if (!alphaCarriers_.emplace(channel.id, standardIndex).second) {
            return;
        }
        // A free slot, or a Color-flag carrier the material fold planted for
        // a static alpha whose `mapAlpha` nothing drives yet: the multiply is
        // the weight, the track rides the map alpha, and the two multiply.
        const auto shareable = [](const std::optional<m3::TextureLayer>& l) {
            return l.has_value() && hasFlag(l->flags, m3::TextureLayerFlag::Color) &&
                   !l->mapAlpha.isAnimated();
        };
        // Last, a textured mask: its map alpha multiplies the sample
        // (`PSMaterialLayer.fx`, `cResult.a *= p_vMultiplyAddAlphaTrans.z`),
        // and while it rests at 1 nothing else states it.
        const auto freeMapAlpha = [](const std::optional<m3::TextureLayer>& l) {
            return l.has_value() && !l->mapAlpha.isAnimated() && l->mapAlpha.initValue == 1.0f &&
                   l->mapAlpha.nullValue == 1.0f;
        };
        std::optional<m3::TextureLayer>* slot = nullptr;
        if (!mat.alphaLayer1.has_value()) {
            slot = &mat.alphaLayer1;
        } else if (!mat.alphaLayer2.has_value()) {
            slot = &mat.alphaLayer2;
        } else if (shareable(mat.alphaLayer1)) {
            slot = &mat.alphaLayer1;
        } else if (shareable(mat.alphaLayer2)) {
            slot = &mat.alphaLayer2;
        } else if (freeMapAlpha(mat.alphaLayer1)) {
            slot = &mat.alphaLayer1;
        } else if (freeMapAlpha(mat.alphaLayer2)) {
            slot = &mat.alphaLayer2;
        }
        if (slot == nullptr) {
            diagnostics_.warn(DiagCode::AnimTrackDropped,
                              "a material alpha track found both alpha layers' map alphas taken",
                              ElementRef(ElementKind::Channel, channel.id), context_.profile);
            return;
        }
        // The rest is the first key when the channel states nothing — the
        // native WoW draw path reads exactly that (`FirstValue`), so a plane
        // shipped at weight 0.3 stays at 0.3 for a consumer that does not
        // sample layer tracks.
        f32 rest = 1.0f;
        if (channel.hasInitValue() && channel.initValue.size() >= sizeof(f32)) {
            std::memcpy(&rest, channel.initValue.data(), sizeof(f32));
        } else if (track.values.size() >= sizeof(f32)) {
            std::memcpy(&rest, track.values.data(), sizeof(f32));
        }
        if (!slot->has_value()) {
            m3::TextureLayer carrier;
            carrier.flags = m3::TextureLayerFlag::Color;
            carrier.color.initValue = m3::ColorBGRA{255, 255, 255, 255};
            carrier.rgbMultiply.initValue = 1.0f;
            *slot = std::move(carrier);
        }
        (*slot)->mapAlpha.initValue = rest;
        Wire((*slot)->mapAlpha, exportId(channel.id), track.interp);
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
    M3ExportMap* map_ = nullptr;
    std::vector<std::vector<u32>> materialOrdinals_;
    /// (channel, standard material) pairs that already have a carrier alpha
    /// layer (`wireAlphaCarrier`) -- a composite slot plants one per section.
    std::set<std::pair<u32, u32>> alphaCarriers_;
    /// Whole-material colour channels whose rest already seeded the diffuse.
    std::set<u32> colorSeeded_;
    /// Light ambient channels already reported as dropped.
    std::set<u32> ambientDropped_;
    /// Channels some clip interpolates, so no clip may leave them stepped.
    m3_sink::Wiring wiring_;
};

} // namespace

void SolveBoneAnimFlags(m3::Model& out) {
    // Bit 1 of `Wiring::kBound` alone: the solver asks only whether a track
    // answers the property, never which model's tracks it was.
    const auto bound = [](u16 flags) { return (flags & 0x2u) != 0; };
    const std::size_t count = out.bones.size();
    const auto ancestorHas = [&](std::size_t self, u32 mask) {
        std::size_t p = out.bones[self].parentIndex;
        for (std::size_t guard = 0; p != 0xFFFFu && p < count && guard < count; ++guard) {
            if ((static_cast<u32>(out.bones[p].flags) & mask) != 0) {
                return true;
            }
            p = out.bones[p].parentIndex;
        }
        return false;
    };
    for (std::size_t i = 0; i < count; ++i) {
        m3::Bone& bone = out.bones[i];
        u32 flags = static_cast<u32>(bone.flags);
        if (bound(bone.position.flags) || bound(bone.rotation.flags) ||
            bound(bone.scale.flags)) {
            flags |= 0x2200u;
        }
        if (bound(bone.visibility.flags)) {
            flags |= 0xC000u;
        }
        // Parents precede children, so their own inherited bits are already in
        // by the time a child looks up the chain.
        if (ancestorHas(i, 0x2200u)) {
            flags |= 0x2000u;
        }
        if (ancestorHas(i, 0xC000u)) {
            flags |= 0x8000u;
        }
        bone.flags = static_cast<m3::BoneFlag>(flags);
    }
}

void Export(const Document& document, u32 model, const ExportContext& context, m3::Model& out,
            Diagnostics& diagnostics, M3ExportMap* map) {
    if (model >= document.models.size()) {
        return;
    }
    Exporter(document, model, context, out, diagnostics, map).run();
    m3_sink::UnshareAnimIds(out);
    SolveBoneAnimFlags(out);
}

} // namespace m3_anim
} // namespace wem
} // namespace models
} // namespace whiteout

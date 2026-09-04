// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "d3_anim.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <whiteout/models/wem/anim/clip.h>

namespace whiteout {
namespace models {
namespace wem {
namespace d3_anim {

namespace d3n = sno::d3::native;

namespace {

bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

/// `x / 32767` per component, normalised. The fields are `u16` and the data is
/// signed; reading them unsigned mirrors every rotation past a half turn.
Quaternion DecodeQ16(const d3n::Quaternion16& source) {
    const auto component = [](u16 raw) {
        return static_cast<f32>(static_cast<i16>(raw)) * (1.0f / 32767.0f);
    };
    Quaternion out{component(source.nX), component(source.nY), component(source.nZ),
                   component(source.nW)};
    const f32 length = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
    if (length > 0.0f) {
        const f32 scale = 1.0f / length;
        out.x *= scale;
        out.y *= scale;
        out.z *= scale;
        out.w *= scale;
    }
    return out;
}

template <class T>
void AppendBytes(const T& value, std::vector<u8>& out) {
    const u8* raw = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), raw, raw + sizeof(T));
}

class Builder {
public:
    Builder(const d3n::Anim& source, Document& document, u32 modelIndex, Diagnostics& out)
        : source_(source), document_(document), model_(document.models[modelIndex]),
          modelIndex_(modelIndex), out_(out) {}

    std::vector<u32> run() {
        std::vector<u32> clips;
        clips.reserve(source_.arPermutations.size());
        for (const d3n::AnimPermutation& permutation : source_.arPermutations) {
            clips.push_back(importPermutation(permutation));
        }
        return clips;
    }

private:
    u32 importPermutation(const d3n::AnimPermutation& permutation) {
        // `Anim_InitPlaybackState`: fps is the per-permutation rate, and the
        // duration counts spans rather than frames.
        const f32 fps = permutation.flFramesPerTick * 60.0f;
        if (fps <= 0.0f) {
            out_.warn(DiagCode::AnimTrackDropped,
                      "permutation '" + permutation.szName + "' has no frame rate",
                      ElementRef(ElementKind::Document, modelIndex_));
            return kInvalidIndex;
        }

        Clip clip;
        clip.name = permutation.szName;
        clip.model = modelIndex_;
        clip.duration = permutation.dwFrameCount > 1
                            ? static_cast<f32>(permutation.dwFrameCount - 1) / fps
                            : 0.0f;
        clip.looping = true;
        clip.native.set("animSnoId", static_cast<i64>(source_.dwSnoId));
        clip.native.set("flags", static_cast<i64>(permutation.dwFlags));
        clip.native.set("selectionWeight", static_cast<i64>(permutation.dwSelectionWeight));
        clip.native.set("blendTicksFromOtherAnim",
                        static_cast<i64>(permutation.nBlendTicksFromOtherAnim));
        clip.native.set("blendTicksSamePermSwap",
                        static_cast<i64>(permutation.nBlendTicksSamePermSwap));
        clip.native.set("speedScalar", permutation.flSpeedScalar);
        clip.native.set("frameCount", static_cast<i64>(permutation.dwFrameCount));

        SubTrackContainer container;
        container.name = "base";

        for (std::size_t b = 0; b < permutation.arBoneNames.size(); ++b) {
            const u32 node = nodeNamed(permutation.arBoneNames[b].szBoneName);
            if (node == kInvalidNode) {
                continue; // One `.ani` drives several appearances; a name this
                          // one lacks is a skipped track, not an error.
            }
            addTranslation(permutation, b, node, fps, container);
            addRotation(permutation, b, node, fps, container);
            addScale(permutation, b, node, fps, container);
        }

        clip.containers.push_back(std::move(container));
        addAttachments(permutation, fps, clip);

        const u32 index = static_cast<u32>(document_.clips.size());
        document_.clips.push_back(std::move(clip));
        return index;
    }

    u32 nodeNamed(const std::string& name) {
        for (u32 n = 0; n < model_.nodes.size(); ++n) {
            const Node& node = model_.nodes.nodes[n];
            if (node.kind == NodeKind::Bone && IEquals(node.name, name)) {
                return n;
            }
        }
        return kInvalidNode;
    }

    /// The channel for this property, **reused** across permutations and across
    /// `.ani` files.
    ///
    /// A channel is a property declared once (§10.8.1), not a track: a bone's
    /// translation is one property however many animations drive it. Declaring a
    /// fresh one per permutation would give a model one channel per sub-track,
    /// which is the id indirection paying for nothing — measured at 193,584
    /// channels for 193,584 sub-tracks over 24 corpus actors before this
    /// reused.
    u32 declare(u32 node, Channel channel, geom::AttrType type) {
        for (const AnimChannel& existing : model_.animChannels.channels) {
            if (existing.target.kind == TrackTarget::Kind::Node && existing.target.node == node &&
                existing.target.channel == channel && existing.valueType == type) {
                return existing.id;
            }
        }
        AnimChannel entry;
        entry.id = model_.animChannels.nextFreeId();
        entry.target.kind = TrackTarget::Kind::Node;
        entry.target.node = node;
        entry.target.channel = channel;
        entry.valueType = type;
        model_.animChannels.add(entry);
        return entry.id;
    }

    void addTranslation(const d3n::AnimPermutation& permutation, std::size_t bone, u32 node,
                        f32 fps, SubTrackContainer& container) {
        if (bone >= permutation.arTranslationCurves.size()) {
            return;
        }
        const auto& keys = permutation.arTranslationCurves[bone].arKeys;
        if (keys.empty()) {
            return;
        }
        SubTrack track;
        track.channel = declare(node, Channel::Translation, geom::AttrType::F32x3);
        track.interp = Interpolation::Linear;
        for (const d3n::TranslationKey& key : keys) {
            track.times.push_back(static_cast<f32>(key.nFrame) / fps);
            AppendBytes(key.vPosition, track.values);
        }
        container.subTracks.push_back(std::move(track));
    }

    void addRotation(const d3n::AnimPermutation& permutation, std::size_t bone, u32 node, f32 fps,
                     SubTrackContainer& container) {
        if (bone >= permutation.arRotationCurves.size()) {
            return;
        }
        const auto& keys = permutation.arRotationCurves[bone].arKeys;
        if (keys.empty()) {
            return;
        }
        SubTrack track;
        track.channel = declare(node, Channel::Rotation, geom::AttrType::Quat);
        // The engine NLERPs with a dot-sign flip and there is no acos anywhere in
        // the subsystem; `Slerp` is what WEM calls "shortest arc between two
        // quaternion keys", and which of the two a consumer runs is its business.
        track.interp = Interpolation::Slerp;
        for (const d3n::RotationKey& key : keys) {
            track.times.push_back(static_cast<f32>(key.nFrame) / fps);
            AppendBytes(DecodeQ16(key.tRotation), track.values);
        }
        container.subTracks.push_back(std::move(track));
    }

    void addScale(const d3n::AnimPermutation& permutation, std::size_t bone, u32 node, f32 fps,
                  SubTrackContainer& container) {
        if (bone >= permutation.arScaleCurves.size()) {
            return;
        }
        const auto& keys = permutation.arScaleCurves[bone].arKeys;
        if (keys.empty()) {
            return;
        }
        SubTrack track;
        // One float, not three: D3 ships a uniform scale per bone, which is why
        // the node import sets `uniformScaleOnly`. The channel says `F32` and
        // the stream matches it — widening here would invent two components.
        track.channel = declare(node, Channel::Scale, geom::AttrType::F32);
        track.interp = Interpolation::Linear;
        for (const d3n::ScaleKey& key : keys) {
            track.times.push_back(static_cast<f32>(key.nFrame) / fps);
            AppendBytes(key.flScale, track.values);
        }
        container.subTracks.push_back(std::move(track));
    }

    /// `KeyframedAttachment` is `{frame, TriggerEvent}` — the same event an
    /// actor fires by message, keyed on the clip instead. The hardpoint is the
    /// where, so it becomes the event's node when the model carries one.
    void addAttachments(const d3n::AnimPermutation& permutation, f32 fps, Clip& clip) {
        for (const d3n::KeyframedAttachment& attachment : permutation.arAttachments) {
            ClipEvent event;
            event.time = attachment.flFrame / fps;
            // The name is the hardpoint's; the payload is a `(group, handle)`
            // pair with no name in it, so it rides `value` beside the trigger
            // type rather than being spelled into a string that would look like
            // an asset path.
            event.name = attachment.tEvent.tHardpoint0.szName;
            event.value = static_cast<u32>(attachment.tEvent.tPayload.dwNameHandle);
            event.node = attachmentNamed(attachment.tEvent.tHardpoint0.szName);
            clip.events.push_back(std::move(event));
        }
    }

    u32 attachmentNamed(const std::string& name) {
        if (name.empty()) {
            return kInvalidNode;
        }
        for (u32 n = 0; n < model_.nodes.size(); ++n) {
            const Node& node = model_.nodes.nodes[n];
            if (node.kind == NodeKind::Attachment && IEquals(node.name, name)) {
                return n;
            }
        }
        return kInvalidNode;
    }

    const d3n::Anim& source_;
    Document& document_;
    Model& model_;
    u32 modelIndex_;
    Diagnostics& out_;
};

} // namespace

std::vector<u32> ImportAnim(const d3n::Anim& source, Document& document, u32 model,
                            Diagnostics& out) {
    if (model >= document.models.size()) {
        return {};
    }
    return Builder(source, document, model, out).run();
}

// ============================================================================
// ExportAnim
// ============================================================================

namespace {

/// The inverse of `DecodeQ16`. The fields are `u16` and the data is signed, so
/// this writes the `i16` bit pattern — reading it back unsigned is what mirrors
/// every rotation past a half turn, and writing it unsigned is the same bug on
/// the other side.
d3n::Quaternion16 EncodeQ16(const Quaternion& source) {
    const auto component = [](f32 value) -> u16 {
        const f32 scaled = std::clamp(value, -1.0f, 1.0f) * 32767.0f;
        const i32 rounded = static_cast<i32>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
        return static_cast<u16>(static_cast<i16>(std::clamp(rounded, -32767, 32767)));
    };
    d3n::Quaternion16 out;
    out.nX = component(source.x);
    out.nY = component(source.y);
    out.nZ = component(source.z);
    out.nW = component(source.w);
    return out;
}

template <class T>
T ReadAt(const std::vector<u8>& values, std::size_t offset) {
    T out{};
    if (offset + sizeof(T) <= values.size()) {
        std::memcpy(&out, values.data() + offset, sizeof(T));
    }
    return out;
}

/// The frame a key at @p time lands on. `Anim_InitPlaybackState` runs the other
/// way — `time = frame / fps` — so this rounds, and a source whose times were
/// not on a frame boundary is reported by the caller rather than silently
/// snapped.
i32 FrameOf(f32 time, f32 fps, bool& snapped) {
    const f32 exact = time * fps;
    const i32 rounded = static_cast<i32>(exact >= 0.0f ? exact + 0.5f : exact - 0.5f);
    if (std::fabs(exact - static_cast<f32>(rounded)) > 1e-3f) {
        snapped = true;
    }
    return rounded;
}

/// The bone a channel drives, and where in the permutation's own bone list it
/// went. One list per permutation, in first-use order, because that is the
/// order the curve arrays are parallel to.
class PermutationWriter {
public:
    PermutationWriter(const Model& model, const Clip& clip, Diagnostics& out)
        : model_(model), clip_(clip), out_(out) {}

    d3n::AnimPermutation run() {
        d3n::AnimPermutation out;
        out.szName = clip_.name;
        out.dwFlags = static_cast<i32>(clip_.native.value("flags", 0));
        out.dwSelectionWeight = static_cast<i32>(clip_.native.value("selectionWeight", 0));
        out.nBlendTicksFromOtherAnim =
            static_cast<i32>(clip_.native.value("blendTicksFromOtherAnim", 0));
        out.nBlendTicksSamePermSwap =
            static_cast<i32>(clip_.native.value("blendTicksSamePermSwap", 0));
        out.flSpeedScalar = 1.0f;

        // The rate the import derived the times with, recovered from the frame
        // count and the duration rather than stored: `fps = flFramesPerTick*60`
        // is what turned frames into seconds, and `(frameCount - 1) / fps` is
        // the duration, so the pair inverts exactly. A clip that never came
        // from an `.ani` has no frame count, and 30 fps is then the rate the
        // times are quantised at — stated, not inferred.
        const i64 frames = clip_.native.value("frameCount", 0);
        f32 fps = kDefaultFps;
        if (frames > 1 && clip_.duration > 0.0f) {
            fps = static_cast<f32>(frames - 1) / clip_.duration;
        }
        out.flFramesPerTick = fps / 60.0f;
        out.dwFrameCount =
            frames > 0 ? static_cast<i32>(frames) : static_cast<i32>(clip_.duration * fps) + 1;

        bool snapped = false;
        for (const SubTrackContainer& container : clip_.containers) {
            for (const SubTrack& track : container.subTracks) {
                addTrack(out, track, fps, snapped);
            }
        }
        if (snapped) {
            out_.warn(DiagCode::AnimTrackApproximated,
                      "clip '" + clip_.name +
                          "' has keys off the frame grid; a D3 key is an integer frame and they "
                          "were rounded to the nearest",
                      ElementRef(), ProfileId::Diablo3);
        }

        out.dwBoneCount = static_cast<i32>(out.arBoneNames.size());
        // Every bone that got any curve gets all three arrays, because the three
        // are indexed by the same bone ordinal and a short one would silently
        // re-map every bone past it.
        out.arTranslationCurves.resize(out.arBoneNames.size());
        out.arRotationCurves.resize(out.arBoneNames.size());
        out.arScaleCurves.resize(out.arBoneNames.size());
        for (auto& [bone, curves] : pending_) {
            out.arTranslationCurves[bone] = std::move(curves.translation);
            out.arRotationCurves[bone] = std::move(curves.rotation);
            out.arScaleCurves[bone] = std::move(curves.scale);
        }
        for (std::size_t b = 0; b < out.arBoneNames.size(); ++b) {
            out.arTranslationCurves[b].dwKeyCount =
                static_cast<i32>(out.arTranslationCurves[b].arKeys.size());
            out.arRotationCurves[b].dwKeyCount =
                static_cast<i32>(out.arRotationCurves[b].arKeys.size());
            out.arScaleCurves[b].dwKeyCount = static_cast<i32>(out.arScaleCurves[b].arKeys.size());
        }

        writeAttachments(out, fps);
        return out;
    }

private:
    struct Curves {
        d3n::TranslationCurve translation;
        d3n::RotationCurve rotation;
        d3n::ScaleCurve scale;
    };

    void addTrack(d3n::AnimPermutation& out, const SubTrack& track, f32 fps, bool& snapped) {
        const AnimChannel* channel = model_.animChannels.find(track.channel);
        if (channel == nullptr || channel->target.kind != TrackTarget::Kind::Node) {
            return;
        }
        const u32 node = channel->target.node;
        if (node >= model_.nodes.size() || model_.nodes.nodes[node].kind != NodeKind::Bone) {
            // A D3 permutation drives bones and nothing else; a light's colour
            // track has no field in an `.ani` to land in.
            out_.warn(DiagCode::AnimTrackDropped,
                      "clip '" + clip_.name +
                          "' drives a non-bone node; a `.ani` curve is per "
                          "bone",
                      ElementRef(ElementKind::Track, track.channel), ProfileId::Diablo3);
            return;
        }
        const std::size_t bone = boneSlot(out, model_.nodes.nodes[node].name);
        Curves& curves = pending_[bone];

        const std::size_t stride =
            ValuesPerKey(track.interp) * geom::AttrTypeSize(channel->valueType);
        const std::size_t value = (ValuesPerKey(track.interp) - 1) / 2; // Hermite: the value slot.
        for (std::size_t k = 0; k < track.times.size(); ++k) {
            const std::size_t at = k * stride + value * geom::AttrTypeSize(channel->valueType);
            const i32 frame = FrameOf(track.times[k], fps, snapped);
            switch (channel->target.channel) {
            case Channel::Translation:
                curves.translation.arKeys.push_back(
                    d3n::TranslationKey{frame, ReadAt<Vector3f>(track.values, at)});
                break;
            case Channel::Rotation:
                curves.rotation.arKeys.push_back(
                    d3n::RotationKey{frame, EncodeQ16(ReadAt<Quaternion>(track.values, at))});
                break;
            case Channel::Scale:
                // One float, matching the channel the import declared. A
                // three-component scale from another format takes x, the same
                // flattening the bind pose reports.
                curves.scale.arKeys.push_back(
                    d3n::ScaleKey{frame, channel->valueType == geom::AttrType::F32
                                             ? ReadAt<f32>(track.values, at)
                                             : ReadAt<Vector3f>(track.values, at).x});
                break;
            default:
                out_.warn(DiagCode::AnimTrackDropped,
                          "clip '" + clip_.name + "' drives a channel a `.ani` has no curve for",
                          ElementRef(ElementKind::Track, track.channel), ProfileId::Diablo3);
                break;
            }
        }
    }

    std::size_t boneSlot(d3n::AnimPermutation& out, const std::string& name) {
        for (std::size_t b = 0; b < out.arBoneNames.size(); ++b) {
            if (out.arBoneNames[b].szBoneName == name) {
                return b;
            }
        }
        out.arBoneNames.push_back(d3n::BoneName{name});
        return out.arBoneNames.size() - 1;
    }

    void writeAttachments(d3n::AnimPermutation& out, f32 fps) {
        for (const ClipEvent& event : clip_.events) {
            d3n::KeyframedAttachment attachment;
            attachment.flFrame = event.time * fps;
            attachment.tEvent.tHardpoint0.szName = event.name;
            attachment.tEvent.tPayload.dwNameHandle = static_cast<i32>(event.value);
            out.arAttachments.push_back(std::move(attachment));
        }
    }

    const Model& model_;
    const Clip& clip_;
    Diagnostics& out_;
    std::map<std::size_t, Curves> pending_;
};

} // namespace

std::vector<d3n::Anim> ExportAnims(const Document& document, u32 model, Diagnostics& out) {
    std::vector<d3n::Anim> anims;
    if (model >= document.models.size()) {
        return anims;
    }
    const Model& source = document.models[model];

    // One `Anim` per source `.ani`, in first-use order — the clips of one
    // animation are its permutations, and the import recorded which they came
    // from. A clip with no id gets one from a synthetic range so the anim set
    // still has something to name; the range is above every shipped SNO id, so
    // it cannot collide with a real one the host has open.
    i32 nextSynthetic = kSyntheticAnimBase;
    std::vector<std::pair<i32, std::size_t>> byId;
    u32 invented = 0;

    for (std::size_t c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != model && !(clip.model == kInvalidIndex && document.models.size() == 1)) {
            continue;
        }
        i32 id = static_cast<i32>(clip.native.value("animSnoId", -1));
        if (id < 0) {
            id = nextSynthetic++;
            ++invented;
        }
        std::size_t at = anims.size();
        bool found = false;
        for (const auto& entry : byId) {
            if (entry.first == id) {
                at = entry.second;
                found = true;
                break;
            }
        }
        if (!found) {
            d3n::Anim anim;
            anim.dwSnoId = id;
            anims.push_back(std::move(anim));
            byId.emplace_back(id, at);
        }
        anims[at].arPermutations.push_back(PermutationWriter(source, clip, out).run());
    }

    for (d3n::Anim& anim : anims) {
        anim.dwPermutationCount = static_cast<i32>(anim.arPermutations.size());
    }
    if (invented != 0) {
        out.info(DiagCode::AssetUnresolved,
                 std::to_string(invented) +
                     " clips carry no source `.ani` id and were given one from the synthetic "
                     "range; a host that resolves ids against a storage will not find them",
                 ElementRef(), ProfileId::Diablo3);
    }
    return anims;
}

} // namespace d3_anim
} // namespace wem
} // namespace models
} // namespace whiteout

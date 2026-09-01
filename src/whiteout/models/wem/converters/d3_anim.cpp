// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "d3_anim.h"

#include <cmath>
#include <cstring>
#include <string>

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

} // namespace d3_anim
} // namespace wem
} // namespace models
} // namespace whiteout

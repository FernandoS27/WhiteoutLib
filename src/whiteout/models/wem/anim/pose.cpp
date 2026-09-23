// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/pose.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// The three node channels, in the order the slots are kept.
constexpr Channel kNodeChannels[3] = {Channel::Translation, Channel::Rotation, Channel::Scale};

Vector3f Apply(const Matrix44f& m, const Vector3f& v) {
    return Vector3f{v.x * m.data[0][0] + v.y * m.data[1][0] + v.z * m.data[2][0] + m.data[3][0],
                    v.x * m.data[0][1] + v.y * m.data[1][1] + v.z * m.data[2][1] + m.data[3][1],
                    v.x * m.data[0][2] + v.y * m.data[1][2] + v.z * m.data[2][2] + m.data[3][2]};
}

const AnimChannel* FindNodeChannel(const AnimChannelTable& table, u32 node, Channel channel) {
    for (const AnimChannel& entry : table.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node &&
            entry.target.channel == channel && entry.target.sub == 0) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

void SampleTrack(const SubTrack& track, geom::AttrType type, f32 time, f32* out, u32 count) {
    const u32 components = geom::AttrTypeComponents(type);
    const u32 stride = ValuesPerKey(track.interp) * components;
    const std::size_t keys = track.times.size();
    if (keys == 0 || track.values.size() < keys * stride * sizeof(f32)) {
        return;
    }
    const f32* values = reinterpret_cast<const f32*>(track.values.data());
    const u32 wanted = std::min(count, components);

    std::size_t after = 0;
    while (after < keys && track.times[after] <= time) {
        ++after;
    }
    if (after == 0) {
        for (u32 c = 0; c < wanted; ++c) {
            out[c] = values[c];
        }
        return;
    }
    const std::size_t before = after - 1;
    if (after >= keys || track.interp == Interpolation::Step) {
        for (u32 c = 0; c < wanted; ++c) {
            out[c] = values[before * stride + c];
        }
        return;
    }
    const f32 span = track.times[after] - track.times[before];
    const f32 alpha = span > 0.0f ? (time - track.times[before]) / span : 0.0f;
    const f32* a = values + before * stride;
    const f32* b = values + after * stride;
    if (type == geom::AttrType::Quat && wanted == 4) {
        // Shortest arc -- what `Slerp` means, and what a componentwise lerp of
        // two keys on opposite hemispheres would get wrong by half a turn.
        const f32 dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const f32 sign = dot < 0.0f ? -1.0f : 1.0f;
        f32 length = 0.0f;
        for (u32 c = 0; c < 4; ++c) {
            out[c] = a[c] + alpha * (sign * b[c] - a[c]);
            length += out[c] * out[c];
        }
        length = std::sqrt(length);
        if (length > 0.0f) {
            for (u32 c = 0; c < 4; ++c) {
                out[c] /= length;
            }
        }
        return;
    }
    for (u32 c = 0; c < wanted; ++c) {
        out[c] = a[c] + alpha * (b[c] - a[c]);
    }
}

Matrix44f PivotComposition(const Transform& trs, const Vector3f& pivot) {
    Matrix44f out = ToMatrix(Transform{Vector3f{0, 0, 0}, trs.rotation, trs.scale});
    const Vector3f moved = Apply(out, Vector3f{-pivot.x, -pivot.y, -pivot.z});
    out.data[3][0] = moved.x + pivot.x + trs.translation.x;
    out.data[3][1] = moved.y + pivot.y + trs.translation.y;
    out.data[3][2] = moved.z + pivot.z + trs.translation.z;
    return out;
}

ClipPose::ClipPose(const Document& document, u32 model, u32 clip) {
    if (model >= document.models.size() || clip >= document.clips.size()) {
        return;
    }
    const Model& owner = document.models[model];
    if (owner.nodes.empty() || document.clips[clip].model != model) {
        return;
    }
    tree_ = &owner.nodes;
    slots_.assign(static_cast<std::size_t>(tree_->size()) * 3, Slot{});
    for (u32 n = 0; n < tree_->size(); ++n) {
        for (u32 c = 0; c < 3; ++c) {
            const AnimChannel* channel = FindNodeChannel(owner.animChannels, n, kNodeChannels[c]);
            if (channel == nullptr) {
                continue;
            }
            Slot& slot = slots_[static_cast<std::size_t>(n) * 3 + c];
            slot.channel = channel;
            slot.type = channel->valueType;
            for (const SubTrackContainer& container : document.clips[clip].containers) {
                if (const SubTrack* track = container.find(channel->id)) {
                    slot.track = track;
                    break;
                }
            }
            if (slot.track != nullptr) {
                times_.insert(times_.end(), slot.track->times.begin(), slot.track->times.end());
            }
        }
    }
    std::sort(times_.begin(), times_.end());
    times_.erase(std::unique(times_.begin(), times_.end()), times_.end());

    order_.reserve(tree_->size());
    for (const u32 root : tree_->roots()) {
        for (const u32 node : tree_->subtree(root)) {
            order_.push_back(node);
        }
    }
    if (order_.size() < tree_->size()) {
        std::vector<u8> seen(tree_->size(), 0);
        for (const u32 node : order_) {
            seen[node] = 1;
        }
        for (u32 n = 0; n < tree_->size(); ++n) {
            if (!seen[n]) {
                order_.push_back(n);
            }
        }
    }
}

Transform ClipPose::local(u32 node, f32 seconds) const {
    if (tree_ == nullptr || node >= tree_->size()) {
        return Transform{};
    }
    // A pivot rig rests at "no track at all" -- `local` holds the pivot chain
    // there, not a rest transform, so starting from it would pose the node
    // twice. `MdxHierarchy::Evaluate` takes the same branch, by name.
    Transform out =
        tree_->rig == RigConvention::PivotRelative ? Transform{} : tree_->nodes[node].local;
    for (u32 c = 0; c < 3; ++c) {
        const Slot& slot = slots_[static_cast<std::size_t>(node) * 3 + c];
        const u32 components = geom::AttrTypeComponents(slot.type);
        f32 buffer[4] = {0, 0, 0, 1};
        if (slot.track != nullptr) {
            SampleTrack(*slot.track, slot.type, seconds, buffer, 4);
        } else if (slot.channel != nullptr && slot.channel->hasInitValue()) {
            const f32* init = reinterpret_cast<const f32*>(slot.channel->initValue.data());
            for (u32 i = 0; i < components && i < 4; ++i) {
                buffer[i] = init[i];
            }
        } else {
            continue;
        }
        switch (kNodeChannels[c]) {
        case Channel::Translation:
            out.translation = Vector3f{buffer[0], buffer[1], buffer[2]};
            break;
        case Channel::Rotation:
            out.rotation = Quaternion{buffer[0], buffer[1], buffer[2], buffer[3]};
            break;
        case Channel::Scale:
            // A source that keys one float scales uniformly -- D3 is the case.
            out.scale = components == 1 ? Vector3f{buffer[0], buffer[0], buffer[0]}
                                        : Vector3f{buffer[0], buffer[1], buffer[2]};
            break;
        default:
            break;
        }
    }
    return out;
}

Matrix44f ClipPose::frame(u32 node, f32 seconds) const {
    Matrix44f out = Matrix44f::identity();
    if (tree_ == nullptr) {
        return out;
    }
    const bool pivoted = tree_->rig == RigConvention::PivotRelative;
    for (u32 at = node; at < tree_->size();) {
        const Transform trs = local(at, seconds);
        out = out * (pivoted ? PivotComposition(trs, tree_->nodes[at].pivot) : ToMatrix(trs));
        const u32 parent = tree_->nodes[at].parent;
        at = parent < tree_->size() && parent != at ? parent : kInvalidNode;
    }
    return out;
}

Matrix44f ClipPose::skinning(u32 node, f32 seconds) const {
    if (tree_ == nullptr || node >= tree_->size()) {
        return Matrix44f::identity();
    }
    // A pivot rig composes to the identity at rest by itself, so its inverse
    // bind is not a second factor to apply here -- it is the same one.
    return tree_->rig == RigConvention::PivotRelative
               ? frame(node, seconds)
               : tree_->inverseBindMatrix(node) * frame(node, seconds);
}

void ClipPose::skinningAt(f32 seconds, std::vector<Matrix44f>& out) const {
    if (tree_ == nullptr) {
        out.clear();
        return;
    }
    const bool pivoted = tree_->rig == RigConvention::PivotRelative;
    out.assign(tree_->size(), Matrix44f::identity());
    // Parents first, so every node reads a frame that is already composed.
    for (const u32 n : order_) {
        const Transform trs = local(n, seconds);
        const Matrix44f own =
            pivoted ? PivotComposition(trs, tree_->nodes[n].pivot) : ToMatrix(trs);
        const u32 parent = tree_->nodes[n].parent;
        out[n] = parent < tree_->size() && parent != n ? own * out[parent] : own;
    }
    if (!pivoted) {
        for (u32 n = 0; n < tree_->size(); ++n) {
            out[n] = tree_->inverseBindMatrix(n) * out[n];
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout

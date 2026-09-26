// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/pose.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

namespace {

Vector3f Apply(const Matrix44f& m, const Vector3f& v) {
    return Vector3f{v.x * m.data[0][0] + v.y * m.data[1][0] + v.z * m.data[2][0] + m.data[3][0],
                    v.x * m.data[0][1] + v.y * m.data[1][1] + v.z * m.data[2][1] + m.data[3][1],
                    v.x * m.data[0][2] + v.y * m.data[1][2] + v.z * m.data[2][2] + m.data[3][2]};
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

ClipPose::ClipPose(const Document& document, u32 model, u32 clip) : animator_(document, model) {
    if (model >= document.models.size() || clip >= document.clips.size()) {
        return;
    }
    const Model& owner = document.models[model];
    if (owner.nodes.empty() || document.clips[clip].model != model) {
        return;
    }
    tree_ = &owner.nodes;
    clip_ = clip;
    for (const AnimChannel& channel : owner.animChannels.channels) {
        const Channel kind = channel.target.channel;
        if (channel.target.kind != TrackTarget::Kind::Node || channel.target.node >= tree_->size() ||
            (kind != Channel::Translation && kind != Channel::Rotation && kind != Channel::Scale)) {
            continue;
        }
        for (const SubTrackContainer& container : document.clips[clip].containers) {
            if (const SubTrack* track = container.find(channel.id)) {
                times_.insert(times_.end(), track->times.begin(), track->times.end());
            }
        }
    }
    std::sort(times_.begin(), times_.end());
    times_.erase(std::unique(times_.begin(), times_.end()), times_.end());
}

void ClipPose::poseAt(f32 seconds, Pose& out) const {
    // The clip alone, holding its last frame past its end.
    Mix mix;
    mix.plays.push_back(Play{clip_, seconds, 1.0f, false});
    mix.globals = false;
    animator_.evaluate(mix, out);
}

Transform ClipPose::local(u32 node, f32 seconds) const {
    if (tree_ == nullptr || node >= tree_->size()) {
        return Transform{};
    }
    Pose pose;
    poseAt(seconds, pose);
    return pose.local[node];
}

Matrix44f ClipPose::frame(u32 node, f32 seconds) const {
    if (tree_ == nullptr || node >= tree_->size()) {
        return Matrix44f::identity();
    }
    Pose pose;
    poseAt(seconds, pose);
    return pose.frame[node];
}

Matrix44f ClipPose::skinning(u32 node, f32 seconds) const {
    if (tree_ == nullptr || node >= tree_->size()) {
        return Matrix44f::identity();
    }
    Pose pose;
    poseAt(seconds, pose);
    return pose.skinning[node];
}

void ClipPose::skinningAt(f32 seconds, std::vector<Matrix44f>& out) const {
    if (tree_ == nullptr) {
        out.clear();
        return;
    }
    Pose pose;
    poseAt(seconds, pose);
    out = std::move(pose.skinning);
}

} // namespace wem
} // namespace models
} // namespace whiteout

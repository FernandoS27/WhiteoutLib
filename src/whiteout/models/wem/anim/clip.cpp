// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <algorithm>
#include <cmath>
#include <string>

#include <whiteout/models/wem/anim/clip.h>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Thousandths of a unit per second; see @ref ClipMoveSpeed.
const std::string kMoveSpeedMilli = "moveSpeedMilli";

} // namespace

const char* ToString(Interpolation interp) {
    switch (interp) {
    case Interpolation::Step:
        return "step";
    case Interpolation::Linear:
        return "linear";
    case Interpolation::Hermite:
        return "hermite";
    case Interpolation::Bezier:
        return "bezier";
    case Interpolation::Slerp:
        return "slerp";
    case Interpolation::Count:
        break;
    }
    return "invalid";
}

const SubTrack* SubTrackContainer::find(u32 channel) const {
    for (const SubTrack& track : subTracks) {
        if (track.channel == channel) {
            return &track;
        }
    }
    return nullptr;
}

f32 ClipMoveSpeed(const Clip& clip) {
    if (const NativeBag::Entry* milli = clip.native.find(kMoveSpeedMilli); milli != nullptr) {
        return static_cast<f32>(milli->value) / 1000.0f;
    }
    // Whole units, which is how a document written before this key holds it.
    return static_cast<f32>(clip.native.value("m3MoveSpeed", clip.native.value("moveSpeed", 0)));
}

void SetClipMoveSpeed(Clip& clip, f32 speed) {
    // A float off a file, so neither finite nor small: `llround` of either is
    // undefined.
    const f64 milli = std::isfinite(speed) ? static_cast<f64>(speed) * 1000.0 : 0.0;
    clip.native.set(kMoveSpeedMilli,
                    static_cast<i64>(std::llround(std::clamp(milli, -1e15, 1e15))));
}

u32 AnimSet::find(u32 tagId) const {
    for (const AnimTag& entry : byTag) {
        if (entry.tagId == tagId) {
            return entry.clip;
        }
    }
    return kInvalidIndex;
}

} // namespace wem
} // namespace models
} // namespace whiteout

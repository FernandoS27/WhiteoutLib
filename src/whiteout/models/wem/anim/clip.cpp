// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <string>

#include <whiteout/models/wem/anim/clip.h>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Thousandths of a unit per second; see @ref ClipMoveSpeed.
const std::string kMoveSpeedMilli = "moveSpeedMilli";
/// Thousandths; see @ref ClipRarity.
const std::string kRarityMilli = "rarityMilli";

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
    // Whole units, which is how a document written before this key holds it.
    const i64 whole = clip.native.value("m3MoveSpeed", clip.native.value("moveSpeed", 0));
    return Milli(clip.native, kMoveSpeedMilli, static_cast<f32>(whole));
}

void SetClipMoveSpeed(Clip& clip, f32 speed) {
    SetMilli(clip.native, kMoveSpeedMilli, speed);
}

f32 ClipRarity(const Clip& clip) {
    return Milli(clip.native, kRarityMilli, static_cast<f32>(clip.native.value("rarity", 0)));
}

void SetClipRarity(Clip& clip, f32 rarity) {
    SetMilli(clip.native, kRarityMilli, rarity);
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

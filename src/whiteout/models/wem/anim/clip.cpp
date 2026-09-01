// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/anim/clip.h>

namespace whiteout {
namespace models {
namespace wem {

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

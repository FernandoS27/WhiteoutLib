// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/anim/transition.h>

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {

ClipTransition ClipTransitionOf(Game game) {
    ClipTransition out;
    switch (game) {
    case Game::Warcraft:
        out.curve = ClipTransition::Curve::Snapshot;
        break;
    case Game::Wow:
        out.curve = ClipTransition::Curve::Smoothstep;
        break;
    default:
        out.curve = ClipTransition::Curve::Linear;
        break;
    }
    return out;
}

ClipTransition ClipTransitionInto(const Document& document, u32 clip) {
    const Game game = GameOf(document.defaultProfile);
    ClipTransition out = ClipTransitionOf(game);
    if (game == Game::Wow && clip < document.clips.size()) {
        const i64 ms = document.clips[clip].native.value("blendTimeIn", -1);
        if (ms >= 0) {
            out.seconds = static_cast<f32>(ms) / 1000.0f;
        }
    }
    return out;
}

f32 TransitionWeight(const ClipTransition& transition, f32 seconds) {
    if (transition.seconds <= 0.0f) {
        return 1.0f;
    }
    const f32 t = std::clamp(seconds / transition.seconds, 0.0f, 1.0f);
    return transition.curve == ClipTransition::Curve::Smoothstep ? t * t * (3.0f - 2.0f * t) : t;
}

} // namespace wem
} // namespace models
} // namespace whiteout

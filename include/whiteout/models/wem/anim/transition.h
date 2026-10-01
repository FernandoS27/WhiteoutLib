// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file transition.h
 * @brief How each game blends a switch from one clip into another: the one
 *        table the renderer's playlist and the physics bake's lead-ins read
 *        (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §7.2).
 *
 * Warcraft III freezes the outgoing pose and ramps the new clip over it
 * linearly, over the model's `BlendTime`; that is not imported, so over the
 * 150 ms nine models in ten carry.
 * World of Warcraft keeps the old clip playing and blends by a smoothstep over
 * the new sequence's `blendTimeIn`. StarCraft II cross-fades over 150 ms.
 */

#include <whiteout/common_types.h>

#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

struct ClipTransition {
    enum class Curve : u8 {
        Snapshot,   ///< The outgoing pose frozen where the switch found it, the new one ramped in linearly.
        Linear,     ///< Both clips play, cross-faded linearly.
        Smoothstep, ///< Both clips play, cross-faded by `t²(3 - 2t)`.
    };
    f32 seconds = 0.15f;
    Curve curve = Curve::Linear;
};

/// @p game's default switch.
ClipTransition ClipTransitionOf(Game game);

/// The switch into @p clip, as the document's main profile's game plays it:
/// its default, with World of Warcraft's time from the clip's own
/// `blendTimeIn`.
ClipTransition ClipTransitionInto(const Document& document, u32 clip);

/// The incoming clip's weight @p seconds into @p transition, 0 to 1.
f32 TransitionWeight(const ClipTransition& transition, f32 seconds);

} // namespace wem
} // namespace models
} // namespace whiteout

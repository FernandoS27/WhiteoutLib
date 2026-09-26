// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file m3_math.h
 * @brief StarCraft II's track read and layer blend arithmetic.
 *
 * Moved here from the viewer's `io/m3/m3_animation.h`, which keeps the parts
 * tied to M3's own structures and forwards these, so the M3 adapter and the
 * `Animator` share one copy (WEM_ANIMATION_RUNTIME_DESIGN.md §3.1, §3.3).
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include <cstddef>
#include <span>

namespace whiteout {
namespace models {
namespace wem {

/// @brief Where a time lands in a keyframe block.
struct M3KeySpan {
    /// @brief Indices of the bracketing keys; equal when the time is pinned to
    ///        a single key.
    std::size_t i0 = 0;
    std::size_t i1 = 0;
    f32 frac = 0.0f;
    bool valid = false;
};

/// @brief Bracket @p timeMs inside @p times.
///
/// @param loop wrap on the block's own last timestamp: a looping track wraps
///        on its own duration, not the sequence's.
/// @param interpolate false ⇒ hold the left key (a step track).
M3KeySpan M3LocateKey(std::span<const i32> times, i32 timeMs, bool loop, bool interpolate);

/// @brief Componentwise quaternion lerp — deliberately not a slerp.
///
/// Reproduces `M3Anim_EvalTrackQuat`, which does a plain SSE lerp with no
/// normalisation and no hemisphere correction. Real slerping happens one level
/// up, when several layers are combined.
Quaternion M3LerpQuatRaw(const Quaternion& a, const Quaternion& b, f32 t);

/// @brief Slerp used to combine two layers' rotations, with the shortest-arc
///        sign fix the track-level lerp omits.
Quaternion M3SlerpQuat(const Quaternion& a, const Quaternion& b, f32 t);

/// @brief The blend factor one contribution applies, given the weight already
///        accumulated below it.
///
/// `t = w / (acc + w)`, shaped by `t²(3 − 2t)`. Constants verified in the
/// binary (`-2.0f`, `3.0f`); a plain normalised mean is visibly different on
/// any three-way blend.
inline f32 M3SmoothstepFactor(f32 accumulated, f32 w) {
    const f32 denom = accumulated + w;
    if (denom <= 0.0f)
        return 1.0f;
    const f32 t = w / denom;
    return t * t * (3.0f - 2.0f * t);
}

/// @brief Budget the weight walk starts with, and the epsilon it stops at.
inline constexpr f32 kM3StartBudget = 1.0f;
inline constexpr f32 kM3BudgetEpsilon = 1e-5f;
/// @brief Above this remaining budget the first contribution is returned as-is.
inline constexpr f32 kM3SettledBudget = 0.99999f;

} // namespace wem
} // namespace models
} // namespace whiteout

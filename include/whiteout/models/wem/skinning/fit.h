// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file fit.h
 * @brief A few bones for dense motion (BONE_REDUCTION_RESEARCH.md §7.1).
 *
 * Rigid bones and sparse convex weights fitted to where every point is in
 * every frame, the squared distance summed over all of them made as small as
 * the bone budget allows. Weights and bones are solved in turn, each exactly
 * while the other holds: a point's weights by least squares on the simplex,
 * a bone's frames by Horn's closed form on what the others leave it.
 */

#include <functional>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../geometry/skin.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/// Horn's closed form: the rotation and translation carrying points about the
/// weighted centre @p from onto points about @p to, given their weighted
/// cross-covariance @p cross, `Σ w (p - from)(q - to)ᵀ`. False when it is all
/// zero.
bool SolveRigid(const f64 (&from)[3], const f64 (&to)[3], const f64 (&cross)[3][3], Quaternion& rotation,
                Vector3f& translation);

/// The motion to reproduce: rest positions and, frame by frame, where each
/// point is.
struct FitMotion {
    std::span<const Vector3f> rest;   ///< Per point.
    std::span<const Vector3f> frames; ///< `frameCount` × points, frame-major.
    u32 frameCount = 0;
};

/// Bones whose motion is given and kept, which points may lean on too: the
/// skeleton a cloth hangs from, the kept bones of a rig.
struct FitFixedBones {
    std::span<const Matrix44f> skinning; ///< `count` per frame, frame-major: rest to frame, `v · M`.
    u32 count = 0;
    /// Per point, the fixed bones it may lean on; empty: none for any.
    std::span<const std::vector<u32>> allowed;
};

/// How the file holds a point's weights: the fit rounds them so before its
/// bones are fitted once more, to take up what the rounding moved.
enum class FitWeights : u8 {
    Exact,       ///< As solved.
    Bytes,       ///< Four bytes summing to 255 (`QuantizeWeights`).
    EqualShares, ///< A classic group: the heaviest few at one share each (`QuantizeClassic`).
};

struct FitOptions {
    u32 maxBones = 32;    ///< The most new bones.
    f32 rmsTarget = 0.0f; ///< Fewer bones once the RMS distance is this low; 0: the budget.
    u32 maxInfluences = 4;
    FitWeights weights = FitWeights::Exact;
    u32 candidates = 8;   ///< The nearest new bones a point's weights are solved over.
    u32 iterations = 24;  ///< Rounds of weights and bones at most.
    u32 threads = 0;      ///< 0: one per hardware thread.
    /// Asked between rounds: true stops with what the last round reached.
    std::function<bool()> cancelled;
};

struct FitResult {
    u32 bones = 0;                   ///< New bones.
    std::vector<Matrix44f> skinning; ///< `bones` per frame, frame-major: rest to frame, rigid.
    std::vector<Vector3f> pivots;    ///< Each new bone's weighted centre at rest.
    /// Per point, heaviest first, as `FitOptions::weights` holds them: a bone
    /// under `bones` is a new one, `bones + i` fixed bone `i`.
    std::vector<std::vector<geom::Influence>> weights;
    f32 rms = 0.0f;   ///< Over every point of every frame.
    f32 worst = 0.0f; ///< The farthest any point is in any frame.
    bool cancelled = false;
};

/// The bones and weights reproducing @p motion. The same input gives the same
/// result on any number of threads.
FitResult FitBones(const FitMotion& motion, const FitFixedBones& fixed, const FitOptions& options);

/// One point's weights over bones whose motion is given.
struct PointFit {
    std::vector<f32> weights; ///< By candidate; at most `maxInfluences` above zero, summing to 1.
    f64 error = 0.0;          ///< The squared distance left, summed over the frames.
    f32 worst = 0.0f;         ///< The farthest it is in any frame.
};

/// Where the point at @p rest goes under @p truth's influences, reproduced by
/// @p candidates alone: least squares on the simplex, the heaviest
/// @p maxInfluences solved again. @p palette is `frames` × `nodes` skinning
/// matrices, frame-major (`v · M`); influences and candidates name nodes.
PointFit FitPointWeights(const Vector3f& rest, std::span<const geom::Influence> truth,
                         std::span<const Matrix44f> palette, u32 nodes, u32 frames,
                         std::span<const u32> candidates, u32 maxInfluences);

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

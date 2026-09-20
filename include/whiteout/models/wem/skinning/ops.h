// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file ops.h
 * @brief The weight algebra (EDIT_MODE_SKIN_DESIGN.md §6): every command, every
 *        brush dab and every generator's output goes through one of these.
 *
 * Device-free, over a `Mesh`, its `PointTable` and the `NodeTree` the locks and
 * the fold read. One rule is therefore behind every button, and the gates aim at
 * one place.
 *
 * **Normalisation is how an operation ends, not a mode.** Warcraft III cannot
 * hold anything else: `SKIN` sums to 255 and a matrix group averages. For a
 * point with locked bones `K`, a target `t` and a requested value `r`:
 *
 * ```
 * L = sum of w over K                the locked share never moves
 * T = clamp(r, 0, 1 - L)             the target gets what is left, at most
 * R = 1 - L - T                      what the other unlocked bones share
 * O = the unlocked bones but t with w > 0,  S = sum over O
 * if S > 0:  w_o <- w_o * R / S      the others keep their proportions
 * else if R > 0:                     t was the only unlocked influence, lowered
 *     R goes to t's nearest ancestor that is a Bone, unlocked and not t;
 *     with none, T <- 1 - L, and the point is refused and counted
 * ```
 *
 * Then weights under `1e-4` go and the point is sorted. Every member of a point
 * (§3.2) is written the same, so a stroke across a seam cannot open it.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../geometry/mesh.h"
#include "../nodes/tree.h"
#include "points.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/// The tools' influence limit, in every Warcraft III session (§6.3): the GPU's
/// four lanes and the Reforged file's four influences. The Classic profile holds
/// eight so that imported groups survive a round trip; the tools do not make new
/// ones.
inline constexpr u32 kToolInfluenceLimit = 4;

/// Below this a weight is not worth a lane, and is dropped as an operation ends.
inline constexpr f32 kMinWeight = 1e-4f;

/// What an operation may write: point ids from a `PointTable`, and how strongly
/// each is written -- a brush's falloff, in `[0, 1]`. An empty `strength` means
/// fully, which is what every command outside Paint asks for.
struct SkinScope {
    std::span<const u32> points;
    std::span<const f32> strength;
    /**
     * @brief Bones held as though locked, for this operation alone (§8.1).
     *
     * A generator writes a *bone set*, and a point's weight on a bone outside
     * that set is kept: "regenerate the arm without touching the chest" is a
     * scope and three bones. That is exactly what a lock already means, so it
     * is the lock that answers it rather than a second rule -- `L` in §6.1
     * gains these bones, and every operation keeps its one normalisation.
     *
     * Unlike `Node::skin.locked` it is not saved and not shown: it lives for
     * the length of one call.
     */
    std::span<const u32> heldBones;

    f32 strengthOf(std::size_t index) const {
        return index < strength.size() ? strength[index] : 1.0f;
    }
};

/// What an operation did. `refused` is §6.1's last case: the point's only
/// unlocked influence was being lowered and its bone has no unlocked Bone
/// ancestor to take the weight.
struct SkinResult {
    u32 changed = 0;
    u32 locked = 0;
    u32 refused = 0;

    SkinResult& operator+=(const SkinResult& other) {
        changed += other.changed;
        locked += other.locked;
        refused += other.refused;
        return *this;
    }
};

/// `{bone: 1 - L}` plus the locked share; every other unlocked influence goes.
SkinResult Rigid(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, u32 bone);

/// §6.1 with `r = value`.
SkinResult Set(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
               const SkinScope& scope, u32 bone, f32 value);

/// §6.1 with `r = w + delta`. Subtract is a negative delta.
SkinResult Add(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
               const SkinScope& scope, u32 bone, f32 delta);

/// §6.1 with `r = factor * w`.
SkinResult Scale(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, u32 bone, f32 factor);

/// `Set(bone, 0)`.
SkinResult Remove(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                  const SkinScope& scope, u32 bone);

/// The unlocked share scaled to `1 - L`, the locks kept.
SkinResult Normalize(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                     const SkinScope& scope);

/// Drops unlocked influences under @p epsilon, then normalises.
SkinResult Prune(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, f32 epsilon);

/// Keeps the @p width heaviest, locked first, and folds the rest into the
/// nearest joints (`geom::FoldInfluences`), then normalises.
SkinResult Limit(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope, u32 width = kToolInfluenceLimit);

/// `from`'s weight is added to `to`; `from` goes. The `ReassignToParent` merge,
/// generalised to any target.
SkinResult Replace(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                   const SkinScope& scope, u32 from, u32 to);

/// @p passes of `w <- (1 - s) w + s * mean(one-ring)` per bone, over the welded
/// ring, then normalise and limit. Reads its neighbours outside the scope and
/// writes only inside it, so a selection's edge blends into what surrounds it.
SkinResult Smooth(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                  const SkinScope& scope, f32 strength, u32 passes = 1);

/// `w <- w + s (w - mean(one-ring))`, clamped at 0, then normalise. Pushes each
/// point toward its dominant bone.
SkinResult Sharpen(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                   const SkinScope& scope, f32 strength);

/// A point's members get their mean: what fixes a co-located set that disagrees
/// (§3.2) without changing anything else.
SkinResult Unify(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                 const SkinScope& scope);

/**
 * @brief A flexible connection across the band in @p scope (§9.4).
 *
 * Each point takes `t = smoothstep(dA / (dA + dB))`, where `dA` and `dB` are its
 * geodesic distances over the welded ring to the nearest point that goes wholly
 * to @p boneA and to @p boneB, measured within the scope plus one ring. It then
 * writes `Set(A, 1 - t)` and `Set(B, t)`. The width is the scope: a wider band
 * is a softer joint.
 */
SkinResult Soften(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                  const SkinScope& scope, u32 boneA, u32 boneB);

// ---------------------------------------------------------------------------
// The locks (§6.2)
// ---------------------------------------------------------------------------

/// True when every member of @p point carries the mesh's `skinLocked` flag.
bool PointLocked(const Mesh& mesh, const PointTable& points, u32 point);

/// Sets or clears `skinLocked` on every member of every point in @p scope,
/// creating the layer when it is needed. Returns the points changed.
u32 SetPointsLocked(Mesh& mesh, const PointTable& points, std::span<const u32> scope, bool locked);

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file rebind.h
 * @brief Moving a rig's rest pose and everything that rests on it
 *        (EDIT_MODE_TPOSE_DESIGN.md §5).
 *
 * The chokepoint. One function moves vertices, normals, pivots, keys, bind
 * frames and collision shapes **together**, because a re-bind that moved any of
 * them without the others would be a model that disagrees with itself. "Bake
 * the current pose" and "bake a clip frame" are later callers of this, not
 * copies of it.
 *
 * It is the non-uniform, per-node sibling of `RescaleDocument` (retarget.h),
 * and it enumerates exactly what that enumerates: every length a document
 * holds, plus the rotation tracks, the normals and the tangents.
 *
 * ### What it is, in one line
 *
 * Every node moves by a rigid `B(b)` in the model's space, and the animations
 * must come out looking the same. Writing `A(b,t)` for the matrix the runtime
 * skins with — for a pivot rig `T(-p) * S * R * T(p + t)` composed up the chain,
 * the identity at rest — a vertex `v` bound to `b` goes to `v * B(b)`, so
 * keeping `v' * A'(b,t) = v * A(b,t)` asks for `A'(b,t) = B(b)^-1 * A(b,t)`.
 * Working that through the composition gives the whole rewrite:
 *
 *  - the linear part `S' * R' = L(b)^-1 * (S * R) * L(parent)`, where `L` is
 *    `B`'s rotation — so the **scale is untouched** and, in row vectors,
 *    `q' = q_parent * q * conj(q_b)`;
 *  - the translation `t' = t * L(parent)`, and **nothing else**: the pivot
 *    terms cancel identically, so `t = 0` stays `t = 0` and no translation
 *    track ever gains a key;
 *  - the pivots `p'(b) = p(b) * B(parent)`, which is what makes that
 *    cancellation happen and is therefore not a separate choice.
 *
 * Because every one of those is a **constant** map applied to each key at its
 * own unchanged time, and every interpolation WEM has is linear in its control
 * points, the curve **between** keys is rewritten exactly too — tangents
 * included. `SubTrack::tcb` is left alone for the same reason: TCB derives a
 * key's tangents linearly from its neighbours' values, so the same three
 * numbers produce exactly the rewritten tangents.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../diagnostics.h"
#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

/// A node a re-bind moved that carries something it cannot move cleanly
/// (§5.3). Reported, never worked around: the rig is the user's.
enum class RebindCase : u8 {
    NonUniformScale,  ///< The node's scale is not the same on every axis.
    DontInherit,      ///< It refuses part of its parent's frame.
    Billboard,        ///< The runtime faces it at the camera, rest or no rest.
    CollisionShape,   ///< Model-space geometry under a node that turned.
    GlobalSequence,   ///< A rotation track on a clock of its own.
    Count
};

const char* ToString(RebindCase which);

struct RebindStructural {
    RebindCase which = RebindCase::NonUniformScale;
    u32 node = kInvalidNode;
};

struct RebindOptions {
    /// §5.5: solve each vertex for the rest that minimises the animated
    /// difference, instead of skinning it once. Off — it drifts the rest mesh.
    bool favourAnimations = false;
    /// How many frames of EACH clip it fits against. Six over each of six
    /// clips is the sampling §5.5's table was measured at.
    u32 sampleFrames = 6;
    /// And it stops there, however many clips the model has, so that pressing
    /// a button on a model with three hundred of them is still a button press.
    u32 maxSampleFrames = 512;
};

struct RebindResult {
    bool ok = false;
    u32 nodesMoved = 0;
    u32 verticesMoved = 0;
    u32 keysRewritten = 0;
    u32 keysAdded = 0;
    u32 shapesMoved = 0;
    /// The four cases of §5.3, one row per node that has one.
    std::vector<RebindStructural> cases;
    /// The worst vertex's move as a share of the model's size — what §5.3
    /// measures a re-bind's cost in. It is the spread between what a blended
    /// vertex's bones each wanted, so it says what the BLEND cost, and
    /// favouring the animations does not change it.
    f32 worstVertexFraction = 0;
    /// How far favouring the animations moved the rest mesh itself away from
    /// the skinned one, as a share of the model's size — §5.5's last column,
    /// and the reason a clean skin is the default. Zero when it is off.
    f32 restDrift = 0;
    Diagnostics diagnostics;
};

/**
 * @brief Moves @p model's rest to @p rest and rewrites everything that rested
 *        on it.
 *
 * @p rest is one transform per node, in the model's space: where the node
 * should stand once this returns. Its **translation** is the node's new pivot
 * and its **rotation** is the turn the node takes; the scale is ignored,
 * because a re-bind turns a rig and never stretches it.
 *
 * The caller builds it — `TPoseRest` (rigging/tpose.h) for a recovered T-pose,
 * a sampled clip frame for "bake this pose". A span of the wrong length is
 * refused rather than half-applied.
 *
 * Extents are **not** recomputed here: they are the union over the clips, which
 * the editor already computes its own way (`RecomputeExtents`), and doing it
 * twice in two places is how the two come to disagree.
 */
RebindResult Rebind(Document& document, u32 model, std::span<const Transform> rest,
                    const RebindOptions& options = {});

/**
 * @brief Where @p rest would leave @p model's mesh, without moving anything.
 *
 * The posed rest bounds. Q3's "put the feet on the grid" is one translation on
 * every rest transform, and this is what it measures that translation from —
 * asked here rather than worked out by the caller, so the skinning that answers
 * it is the skinning that will run.
 *
 * An empty extent when @p rest is the wrong length or the model has no mesh.
 */
Extent RestExtent(const Model& model, std::span<const Transform> rest);

} // namespace wem
} // namespace models
} // namespace whiteout

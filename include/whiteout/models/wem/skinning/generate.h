// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file generate.h
 * @brief Weights made rather than painted (EDIT_MODE_SKIN_DESIGN.md §8).
 *
 * What every generator here shares (§8.1):
 *
 * - the **scope** is a set of points, or every point when none is given;
 * - the **bone set** is the bones it may write. Empty means every unlocked
 *   `Bone`. A point's weight on a bone outside the set is kept as locked share,
 *   so "regenerate the arm without touching the chest" is a scope and three
 *   bones;
 * - the output is limited to four, pruned at 0.01 and normalised, through
 *   `ops.h` rather than by hand;
 * - a point no bone reaches goes Rigid to the nearest bone by segment distance
 *   and is counted, never left empty.
 *
 * Only Rigid per Island is here so far (§8.3). Envelopes, Heat and the voxel
 * geodesic attach to the same two structs.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../geometry/mesh.h"
#include "../nodes/tree.h"
#include "ops.h"
#include "points.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/**
 * @brief Where each bone is, and what it carries (§8.2).
 *
 * A Warcraft III bone rotates about its pivot and carries the mesh between
 * itself and its children, so the segment of bone B runs from `joint(B)` to the
 * joint of each of its child bones — one segment per child.
 *
 * **A bone with no child bone gets a sphere at its joint**, of radius half the
 * distance to its parent's joint. That radius is not decoration. A parent's
 * segment *ends* at the child's joint, so with a bare point a leaf could never
 * be strictly nearer than its own parent anywhere: a hand bone would take no
 * weight at all, and the mechanical one-click would leave every extremity on
 * the body. The sphere gives the leaf the outer half of the last segment,
 * which is what §8.2 asks for and what an artist means by "the hand".
 *
 * A segment is therefore measured as a signed distance to its geometry —
 * `distance - radius`, negative inside — with the radius 0 for a real segment
 * and the sphere's for a leaf, so the two are one query and not two cases.
 *
 * The joint is `NodeTree::worldBind`'s translation rather than `Node::pivot`, so
 * a rig that binds with matrices rather than pivots answers the same question.
 */
struct BoneSegments {
    /// The bones, in the order the set was built: node indices.
    std::vector<u32> bones;
    /// Per segment: the bone it belongs to, its two ends in model space, and
    /// the radius of the volume around it (0 for all but a leaf's sphere).
    struct Segment {
        u32 bone = 0;
        Vector3f start{0, 0, 0};
        Vector3f end{0, 0, 0};
        f32 radius = 0.0f;
    };
    std::vector<Segment> segments;

    bool empty() const {
        return segments.empty();
    }
};

/// The segments of @p bones, or of every `Bone` node when @p bones is empty.
/// A locked bone is left out: a generator may not write one (§6.2).
BoneSegments BuildBoneSegments(const NodeTree& nodes, std::span<const u32> bones = {});

/// The distance from @p position to the nearest point of the segment.
f32 DistanceToSegment(const Vector3f& position, const Vector3f& start, const Vector3f& end);

/// The bone of @p segments nearest @p position — `distance - radius`, so a
/// point inside a leaf's sphere is nearer it than its parent's segment end.
/// `kInvalidNode` for an empty set. Ties go to the bone nearer the root, which
/// is the lower index in a tree whose parents precede their children.
u32 NearestBone(const BoneSegments& segments, const Vector3f& position);

/// How a generator was asked to run.
struct GenerateOptions {
    /// The bones it may write; empty is every unlocked `Bone` (§8.1).
    std::vector<u32> bones;
    /// Drop the "a whole island goes to one bone" rule: each point then goes to
    /// its own nearest segment (§8.3).
    bool splitIslands = false;
};

/// What a generator did, beside `SkinResult`'s counts.
struct GenerateResult {
    SkinResult weights;
    u32 islands = 0;  ///< Islands it decided.
    u32 unreached = 0;///< Points no bone reached, which went to the nearest.
};

/**
 * @brief Rigid per Island (§8.3): the mechanical one-click.
 *
 * Each island in @p scope goes wholly to one bone — the bone whose segment most
 * of the island's points are nearest to, a vote with ties to the bone nearer the
 * root. `splitIslands` sends each point to its own nearest bone instead.
 *
 * @p scope is a set of point ids, or empty for every point of the mesh.
 */
GenerateResult RigidPerIsland(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                              std::span<const u32> scope, const GenerateOptions& options = {});

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

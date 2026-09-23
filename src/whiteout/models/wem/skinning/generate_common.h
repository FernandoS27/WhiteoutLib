// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file generate_common.h
 * @brief What the generators of `generate.h` share (EDIT_MODE_SKIN_DESIGN.md
 *        §8.1). Private to `skinning/generate*.cpp`: nothing outside them
 *        includes it.
 */

#include <functional>
#include <span>
#include <vector>

#include <whiteout/models/wem/skinning/generate.h>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {
namespace detail {

/// @p node's joint in model space. The bind matrix rather than `Node::pivot`,
/// so a rig that binds with matrices rather than pivots answers the same
/// question -- and it is the formula `ops.cpp`'s fold uses, so the two cannot
/// drift.
Vector3f JointOf(const NodeTree& nodes, u32 node);

/// A point's position: its first member's, which is the vertex every other
/// member is co-located with to within the weld tolerance (§3.2).
Vector3f PositionOf(std::span<const Vector3f> positions, const PointTable& points, u32 point);

/// @p scope, or every point of @p points when it is empty (§8.1).
std::vector<u32> ScopeOrEvery(const PointTable& points, std::span<const u32> scope);

/// The Bones a run over @p segments may not write although they are not
/// locked: every Bone outside the set @p asked for. Empty when nothing was
/// asked for, which is every unlocked bone (§8.1).
std::vector<u32> HeldBones(const NodeTree& nodes, const BoneSegments& segments,
                           std::span<const u32> asked);

/// A mesh's faces cut into the triangles they are drawn as
/// (`geom::TriangulateMesh`), three vertex ids each: what `bvh.h` and the voxel
/// shell both read.
std::vector<u32> TrianglesOf(const Mesh& mesh);

/// Rigid per Island's vote (§8.3) over @p members: the bone most of them are
/// nearest to, ties to the lower index. `kInvalidNode` for no bone.
u32 VoteBone(const BoneSegments& segments, std::span<const Vector3f> positions,
             const PointTable& points, std::span<const u32> members);

/**
 * @brief Runs @p job for every index below @p count, on @p control's pool and
 *        on the calling thread, and returns false when the run was stopped.
 *
 * The calling thread takes jobs like any worker and is the one that reports:
 * `done` advances by what finished, against @p total. Jobs are handed out from
 * one counter, so a stop is seen before the next job starts rather than after
 * the whole queue; a job already running finishes, and a long one checks
 * `control.stopped()` itself.
 *
 * Each job must write only its own output. The answer then does not depend on
 * how many threads ran it, or in what order.
 */
bool RunJobs(u32 count, const GenerateControl& control, u32& done, u32 total,
             const std::function<void(u32)>& job);

/// The end of a one-mesh run: nothing when it was stopped, `WriteGenerated`
/// otherwise.
GenerateResult WriteOneMesh(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                            const GeneratedRun& run, const GenerateOptions& options);

} // namespace detail
} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file points.h
 * @brief What one click means: a mesh's points, their ring and their islands
 *        (EDIT_MODE_SKIN_DESIGN.md §3.2-3.4).
 *
 * An `.mdx` geoset stores a vertex once per corner combination, so a UV seam or
 * a hard edge gives one position two or three vertices, and the faces on either
 * side do not even share an edge. A brush that painted one side would open the
 * seam the moment the bone moves, and a diffusion over the raw topology would
 * stop at every seam as though it were a cut.
 *
 * So the workspace edits **points**: the vertices of one mesh joined by
 *
 * - **seam twins** — positions equal to within `1e-5 x the bounding diagonal`;
 * - **repair twins** — the same `mergeGroup`, which `BuildRenderMesh` collapses
 *   into one GPU vertex using the FIRST twin's skin, so an edit to the second
 *   alone would be dropped on export without a word.
 *
 * Points never span meshes (§1.3): inside a geoset co-located vertices agree
 * 99.6 % of the time and are seams, while across geosets a quarter disagree and
 * are parts that touch and move differently.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../geometry/mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/// One mesh's points, their welded one-ring and their islands.
struct PointTable {
    u32 vertexCount = 0;
    u32 pointCount = 0;
    u32 islandCount = 0;
    /// `1e-5 x` the mesh's bounding diagonal: what counted as the same position.
    f32 weldTolerance = 0.0f;

    std::vector<u32> pointOf; ///< Per vertex.

    /// Per point, its vertices: `members[memberOffsets[p] .. memberOffsets[p+1])`.
    std::vector<u32> memberOffsets;
    std::vector<u32> members;

    /// Per point, the points it shares a face edge with, across seams. Sorted,
    /// with no repeats and never itself.
    std::vector<u32> ringOffsets;
    std::vector<u32> ring;

    std::vector<u32> islandOf;   ///< Per point.
    std::vector<u32> faceIsland; ///< Per face.

    std::span<const u32> membersOf(u32 point) const;
    std::span<const u32> ringOf(u32 point) const;

    /// The point @p vertex belongs to, or `kInvalidIndex` past the mesh.
    u32 pointOfVertex(u32 vertex) const;
};

/// Builds @p mesh's table (§3.2). Reads the face set, so it needs no half-edge
/// connectivity; a caller caches the result against its own mesh revision.
PointTable BuildPointTable(const Mesh& mesh);

/// The points whose members do not all hold the same influences (§3.2): 0.4 % of
/// shipped co-located sets, and what the Problems view flags. Sorted.
std::vector<u32> DisagreeingPoints(const Mesh& mesh, const PointTable& points);

/// Every point within @p rings of @p seed through the welded ring, @p seed
/// included. Sorted.
std::vector<u32> GrowPoints(const PointTable& points, std::span<const u32> seed, u32 rings = 1);

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

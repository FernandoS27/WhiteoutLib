// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file patch_map.h
 * @brief One patch laid flat: a Tutte map of its triangles onto a convex
 *        polygon (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.6, §1.8).
 *
 * The patch is cut open along its own layout edges first, so a seam inside it
 * is two runs of border and a vertex on it two border points. With the border
 * pinned on a convex polygon and every weight positive, the map is one to one
 * (Tutte 1963; Floater 2003): the layout cuts straight spokes along it, and
 * the extraction reads its grid out of it.
 */

#include <array>
#include <vector>

#include <whiteout/common_types.h>

#include "layout.h"
#include "work_mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

using P2 = std::array<f64, 2>;

class PatchMap {
public:
    /// Per map vertex, the work vertex it is. The first `borderCount` are the
    /// patch's loop, one per loop halfedge's start, in loop order.
    std::vector<u32> vertices;
    u32 borderCount = 0;
    /// Per patch triangle (in `LayoutPatch::triangles` order), its three map
    /// vertices.
    std::vector<u32> corners;
    std::vector<P2> uv;
    /// Per map triangle corner, the map triangle across its outgoing edge, or
    /// kNone where the patch's border is.
    std::vector<u32> across;
    /// The work triangle of each map triangle.
    std::vector<u32> triangles;

    u32 triangleCount() const {
        return static_cast<u32>(triangles.size());
    }

    /// The map triangle holding @p point and its weights; for a point just
    /// outside, the nearest triangle with its weights clamped. kNone for an
    /// empty map, or one whose triangles are all degenerate.
    u32 locate(const P2& point, f64 weights[3]) const;
};

/**
 * @brief Lays @p patch out with its loop pinned at @p border (one point per
 *        loop halfedge, in loop order, on a convex polygon).
 *
 * False when the patch is not a disk with one loop.
 */
bool BuildPatchMap(const WorkMesh& mesh, const LayoutPatch& patch, const std::vector<P2>& border,
                   PatchMap& map);

/// The loop laid on the unit circle by length.
std::vector<P2> CircleBorder(const WorkMesh& mesh, const LayoutPatch& patch);

/// The loop on the unit circle with the loop positions @p corners at a regular
/// polygon's vertices, each side spread by length over its own arc: every side
/// gets an equal share of the turn, however short it is.
std::vector<P2> CornerCircleBorder(const WorkMesh& mesh, const LayoutPatch& patch,
                                   const std::vector<u32>& corners);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

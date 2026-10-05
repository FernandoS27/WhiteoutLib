// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file cdt.h
 * @brief A constrained Delaunay triangulation in the plane
 *        (EDIT_MODE_FRACTURE_DESIGN.md §5.5).
 *
 * The Fracture's inside faces are a cell face's polygon triangulated with the
 * mesh's cut segments forced in, then split into the regions those segments
 * bound. `TriangulateFace` does one loop with no holes; this does any set of
 * segments.
 *
 * The predicates are exact: the points are snapped to an integer grid of 2^26
 * over their extent, so orientation is evaluated in 64-bit and the in-circle
 * test in 128-bit integers. Points within 16 grid steps are one point, and an
 * end that close to another segment splits it. Segments that cross are split
 * where they cross before anything is triangulated, which is where
 * overlapping shells meet.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

struct Cdt2d {
    /// The input's points, then each crossing point the split added.
    std::vector<Vector2<f64>> points;
    /// Three points each, counter-clockwise. A point merged into an earlier
    /// one is named by the earlier one.
    std::vector<u32> triangles;
    /// Per triangle, its region: triangles reached from each other without
    /// crossing a segment. The region reaching the hull is dropped, so only
    /// what closed segments enclose is returned.
    std::vector<u32> regions;
    u32 regionCount = 0;
    /// Segments that could not be forced in. Their regions may have leaked.
    u32 droppedSegments = 0;

    bool ok() const {
        return droppedSegments == 0;
    }
};

/// Triangulates @p points with @p segments (two point indices each) forced in.
Cdt2d ConstrainedTriangulation2d(std::span<const Vector2<f64>> points,
                                 std::span<const u32> segments);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

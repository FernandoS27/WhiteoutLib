// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file cells.h
 * @brief Convex cells and the Voronoi cells the Fracture breaks along
 *        (EDIT_MODE_FRACTURE_DESIGN.md §5.2).
 *
 * A cell is the convex polytope of the points nearer its seed than any other,
 * measured in grain space, clipped to the targets' box. Every face carries the
 * key of the plane it lies on, which is the pair of sites either side of it,
 * so the face between cells i and j has the same key, and the same plane, in
 * both. The plane is never stored per cell: `VoronoiDiagram::plane` computes it
 * from the key, so both sides get the same bits.
 *
 * A vertex carries its sites too, the seeds and box faces it is equidistant
 * from, and its position is computed from them. The cells either side of a
 * corner therefore hold it at the same bits, which is what lets their pieces
 * share inside faces without a weld.
 */

#include <array>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../../bounds.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

/// Box face k (0..5: -x, +x, -y, +y, -z, +z) is the site `kBoxSite + k`, past
/// every seed.
inline constexpr u32 kBoxSite = 0xFFFFFFF0u;
inline constexpr u32 kNoSite = 0xFFFFFFFFu;

inline bool IsBoxSite(u32 site) {
    return site >= kBoxSite && site < kBoxSite + 6;
}

/// The key of the plane between sites @p a and @p b: the pair, low first. A box
/// face's key is its site twice, whichever cell it bounds.
u64 PlaneKey(u32 a, u32 b);
u32 PlaneLow(u64 key);
u32 PlaneHigh(u64 key);

/// The points with n · x <= offset.
struct HalfSpace {
    Vector3d normal{0, 0, 0};
    f64 offset = 0.0;

    f64 distance(const Vector3d& p) const {
        return normal.x * p.x + normal.y * p.y + normal.z * p.z - offset;
    }
};

struct CellFace {
    u64 plane = 0;       ///< `PlaneKey` of the cell's site and `other`.
    u32 other = kNoSite; ///< The seed or box face across it.
    std::vector<u32> loop; ///< Vertices, counter-clockwise seen from outside.
};

struct ConvexCell {
    u32 site = kNoSite;
    std::vector<Vector3d> vertices;
    /// Per vertex, its sites in ascending order, `kNoSite` padded: the cell's
    /// own and those of the faces through it.
    std::vector<std::array<u32, 4>> sites;
    std::vector<CellFace> faces;

    bool empty() const {
        return faces.empty();
    }
};

/// The box as a cell of @p site: six faces on the box's sites.
ConvexCell BoxCell(const Vector3d& low, const Vector3d& high, u32 site);

/**
 * @brief Keeps the part of @p cell with `plane.distance(x) <= 0`.
 *
 * The cut makes a face on the plane, keyed @p key, across from @p other. A
 * vertex within @p epsilon of the plane lies on it. Leaves the cell empty when
 * nothing is kept.
 */
void ClipConvex(ConvexCell& cell, const HalfSpace& plane, u64 key, u32 other, f64 epsilon);

/// Grain axes: none, or the model's x, y or z.
enum class Grain : u8 { None, X, Y, Z };

struct VoronoiOptions {
    Grain grain = Grain::None;
    f64 stretch = 3.0; ///< The cells come out this many times longer along the grain.
    u32 threads = 0;   ///< 0 is one per hardware thread.
};

struct VoronoiDiagram {
    std::vector<Vector3d> seeds; ///< Model space.
    Vector3d low{0, 0, 0};       ///< The box.
    Vector3d high{0, 0, 0};
    Vector3d grain{1, 1, 1};     ///< Model to grain space, per axis.
    std::vector<ConvexCell> cells; ///< Per seed; empty for a seed that repeats an earlier one.

    /// The plane of @p key in model space, normalised, oriented so that its low
    /// site's side is `distance <= 0`. For a box face, its outside is positive.
    HalfSpace plane(u64 key) const;

    /// Where the sites of @p sites meet, from their planes. False when they do
    /// not fix one point.
    bool corner(const std::array<u32, 4>& sites, Vector3d& out) const;
};

/**
 * @brief The Voronoi cells of @p seeds in the box from @p low to @p high.
 *
 * Each cell starts as the box and is clipped by the bisectors of the other
 * seeds, nearest first, until the next is more than twice as far as the
 * cell's farthest corner. Deterministic: cells run in parallel and are
 * independent.
 */
VoronoiDiagram VoronoiCells(std::span<const Vector3d> seeds, const Vector3d& low,
                            const Vector3d& high, const VoronoiOptions& options);

/**
 * @brief The convex polytope of @p planes (each keeping `distance <= 0`) with
 *        every plane moved in by @p margin: its corners, or nothing when no
 *        volume is left. @p planes must bound a volume; @p low and @p high are
 *        a box that holds it.
 */
std::vector<Vector3d> InsetConvex(std::span<const HalfSpace> planes, f64 margin,
                                  const Vector3d& low, const Vector3d& high);

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

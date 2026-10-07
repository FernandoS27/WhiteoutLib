// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file cells.h
 * @brief Convex cells, and the two ways the Fracture makes the ones it breaks
 *        along: Voronoi cells (EDIT_MODE_FRACTURE_DESIGN.md §5.2) and the
 *        regions of slice planes (EDIT_MODE_FRACTURE_NEXT_DESIGN.md §3.4).
 *
 * A cell is the convex polytope of the points nearer its seed than any other,
 * measured in grain space, clipped to the targets' box. Every face carries the
 * key of the plane it lies on, which is the pair of sites either side of it,
 * so the face between cells i and j has the same key, and the same plane, in
 * both. The plane is never stored per cell: `CellComplex::plane` computes it
 * from the key, so both sides get the same bits.
 *
 * A vertex carries its sites too, the seeds and box faces it is equidistant
 * from, and its position is computed from them. The cells either side of a
 * corner therefore hold it at the same bits, which is what lets their pieces
 * share inside faces without a weld.
 *
 * A slice plane is a site of its own, as a box face is: a region has no seed,
 * the face on plane k is keyed by k alone in every region it bounds, and a
 * vertex is keyed by the planes through it.
 */

#include <array>
#include <cmath>
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

/// Slice plane k is the site `kPlaneSite + k`: past every seed, before the box's.
inline constexpr u32 kPlaneSite = 0xFFFF0000u;

inline bool IsPlaneSite(u32 site) {
    return site >= kPlaneSite && site < kBoxSite;
}

/// A site that is a plane by itself, a box face or a slice plane, and not a
/// seed: its plane's key is the site twice.
inline bool IsFixedSite(u32 site) {
    return IsPlaneSite(site) || IsBoxSite(site);
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
    u64 plane = 0;       ///< `PlaneKey` of the cell's site and `other`, or of a fixed site twice.
    u32 other = kNoSite; ///< The cell or box face across it.
    /// The cell is on the `distance <= 0` side of `CellComplex::plane(plane)`,
    /// which is the side a point on the plane goes to.
    bool low = true;
    std::vector<u32> loop; ///< Vertices, counter-clockwise seen from outside.
};

struct ConvexCell {
    u32 site = kNoSite; ///< Its index among the complex's cells.
    std::vector<Vector3d> vertices;
    /// Per vertex, its sites in ascending order, `kNoSite` padded: the cell's
    /// own seed and those of the faces through it; for a region of slice
    /// planes, every fixed site through it and no seed.
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

/// Where a slice plane cuts: a rectangle on it, `halfU` to each side of
/// `centre` along unit `u` and `halfV` along unit `v`.
struct SliceReach {
    Vector3d centre{0, 0, 0};
    Vector3d u{1, 0, 0};
    Vector3d v{0, 1, 0};
    f64 halfU = 0.0;
    f64 halfV = 0.0;

    bool holds(const Vector3d& p) const {
        const Vector3d r = p - centre;
        return std::fabs(r.x * u.x + r.y * u.y + r.z * u.z) <= halfU &&
               std::fabs(r.x * v.x + r.y * v.y + r.z * v.z) <= halfV;
    }
};

/// The cells a cut runs along, from either builder: `VoronoiCells`' or
/// `PlaneCells`'.
struct CellComplex {
    std::vector<Vector3d> seeds; ///< Voronoi: model space, one per cell. Slices: none.
    Vector3d low{0, 0, 0};       ///< The box.
    Vector3d high{0, 0, 0};
    Vector3d grain{1, 1, 1};     ///< Model to grain space, per axis.
    /// Slices: plane k, normalised, is the site `kPlaneSite + k`.
    std::vector<HalfSpace> slices;
    /// Slices: per plane, the rectangles it cuts within; none cuts everywhere.
    /// The regions are the whole planes' all the same: where a plane does not
    /// reach, the cut joins the pieces either side of it again (cut.h).
    std::vector<std::vector<SliceReach>> reach;
    bool sliced = false; ///< `PlaneCells` made it: a cell has no seed.

    /// Whether some plane cuts only part of the way.
    bool bounded() const {
        for (const std::vector<SliceReach>& list : reach) {
            if (!list.empty()) {
                return true;
            }
        }
        return false;
    }
    /// Voronoi: per seed, empty for a seed that repeats an earlier one.
    /// Slices: per region.
    std::vector<ConvexCell> cells;

    /// The plane of @p key in model space, normalised, oriented so that its low
    /// site's side is `distance <= 0`. For a box face, its outside is positive;
    /// a slice plane is as it was given.
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
CellComplex VoronoiCells(std::span<const Vector3d> seeds, const Vector3d& low,
                         const Vector3d& high, const VoronoiOptions& options);

/**
 * @brief The regions @p planes cut the box from @p low to @p high into.
 *
 * It starts from the box and splits every region a plane crosses, plane by
 * plane. Every plane crosses the whole box, so a region's face on a plane has
 * exactly one region across it, with the same corners. A point a split makes
 * is computed from its edge's ends in position order, so the regions round an
 * edge hold it at the same bits.
 *
 * A plane that misses the box, or repeats an earlier one whichever way it
 * faces, is dropped: `CellComplex::slices` holds the planes cut along, and
 * @p slots, when given, each of @p planes' place among them (`kNoSite` for one
 * that misses the box). Past @p most regions (0 is no limit) it stops and
 * returns no cell.
 */
CellComplex PlaneCells(std::span<const HalfSpace> planes, const Vector3d& low, const Vector3d& high,
                       u32 most = 0, std::vector<u32>* slots = nullptr);

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

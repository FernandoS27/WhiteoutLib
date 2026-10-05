// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file winding.h
 * @brief The generalized winding number, which tells inside from outside on a
 *        mesh that is not closed (EDIT_MODE_FRACTURE_DESIGN.md §5.3).
 *
 * w(p) = (1/4π) Σ Ω_t(p), each triangle's solid angle by Van Oosterom and
 * Strackee. A closed mesh wound outward reads 1 inside and 0 outside; a box
 * with no floor reads close to 1 inside it; a single sheet reads between -½
 * and ½, so it encloses nothing; overlapping shells add up. It needs no
 * connectivity, so the seams an `.mdx` import leaves open do not matter.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../../bounds.h"

namespace whiteout {
namespace models {
namespace wem {

class Mesh;

namespace geom {
namespace fracture {

/// A cluster of a target's triangles: seen from far enough, its solid angle is
/// its area vector's, as a dipole's (Barill et al. 2018).
struct WindingNode {
    Vector3d centre{0, 0, 0}; ///< Area-weighted.
    Vector3d normal{0, 0, 0}; ///< The sum of its triangles' area vectors.
    f64 radius = 0.0;         ///< From the centre to its farthest corner.
    u32 first = 0;            ///< A leaf's triangles, in `order`.
    u32 count = 0;            ///< 0 for an inner node, whose left child follows it.
    u32 right = 0;
};

/// One target's triangles as the winding queries read them.
struct WindingMesh {
    std::vector<Vector3d> corners; ///< Three per triangle, as drawn.
    f64 sign = 1.0;                ///< -1 for a target wound inward (§5.3).
    Extent bounds;
    /// Built for a large target: clusters far from the point are summed whole,
    /// near ones triangle by triangle. Empty: every query is the exact sum.
    std::vector<WindingNode> tree;
    std::vector<u32> order;
};

/// @p mesh's triangles, cut as they are drawn (`TriangulateMesh`).
WindingMesh WindingMeshOf(const Mesh& mesh);

/// The winding number of @p target at @p point, times its sign.
f64 WindingNumber(const WindingMesh& target, const Vector3d& point);

/**
 * @brief Reads @p target as wound inward when most of @p probes that it
 *        clearly winds round (|w| ≥ ½) read negative, and sets its sign so.
 * @return true when it flipped the sign.
 */
bool OrientWinding(WindingMesh& target, std::span<const Vector3d> probes);

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

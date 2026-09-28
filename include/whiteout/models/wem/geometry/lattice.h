// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file lattice.h
 * @brief The Control Gizmo's lattice: a free-form deformation box (Sederberg and
 *        Parry's FFD, 3ds Max's FFD Box).
 *
 * A box of control points is fitted round a set of points. Moving a control
 * point bends the space inside the box, and every point inside follows through
 * the trivariate Bernstein polynomial of the box's coordinates. With every
 * control where the fit put it, every point is exactly where it was.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// The most control points along one axis: the Bernstein degree is one less,
/// and past this a point's pull is too spread out to aim.
inline constexpr u32 kMaxLatticePoints = 16;

struct Lattice {
    Vector3f origin{0.0f, 0.0f, 0.0f}; ///< The box's (0, 0, 0) corner.
    /// The box's edges, unit and square to one another.
    Vector3f axis[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    Vector3f size{1.0f, 1.0f, 1.0f}; ///< Its extent along each axis, never 0.
    u32 points[3] = {2, 2, 2};       ///< Control points per axis, 2 to kMaxLatticePoints.
    /// Where each control point stands now, x fastest, then y, then z.
    std::vector<Vector3f> controls;

    u32 count() const {
        return points[0] * points[1] * points[2];
    }
    u32 index(u32 i, u32 j, u32 k) const {
        return i + points[0] * (j + points[1] * k);
    }
    /// Where control @p index stands undeformed.
    Vector3f rest(u32 index) const;
};

/**
 * @brief The box round @p positions on @p axes, with @p points control points
 *        along each (clamped to 2..kMaxLatticePoints), every one at rest.
 *
 * @p axes must be unit and square to one another. An axis along which the
 * positions have no extent -- a flat patch -- gets a tenth of the widest, and
 * a single point a box of 1, so the box never collapses.
 */
Lattice FitLattice(std::span<const Vector3f> positions, const Vector3f (&axes)[3], const u32 (&points)[3]);

/// Where @p p lies in @p lattice: 0 on the origin's face and 1 on the far one,
/// per axis.
Vector3f LatticeCoordinates(const Lattice& lattice, const Vector3f& p);

/**
 * @brief Each of @p base moved by what @p lattice's controls moved:
 *        `base + sum(B_i(s) B_j(t) B_k(u) (control - rest))`, with (s, t, u)
 *        its @p coordinates.
 *
 * The displacement form, so a point whose controls are all at rest comes back
 * as its base bit for bit. All three spans are parallel; @p out may alias
 * nothing else.
 */
void DeformByLattice(const Lattice& lattice, std::span<const Vector3f> base,
                     std::span<const Vector3f> coordinates, std::span<Vector3f> out);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

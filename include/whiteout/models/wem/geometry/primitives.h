// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file primitives.h
 * @brief The shapes Create makes (EDIT_MODE_MODELLING_DESIGN.md §3.16): a box,
 *        a plane, a cylinder, a cone and a sphere.
 *
 * Each comes back **born finished**: carrying the `modelled` marker, canonical,
 * with its rows materialised and its bounds computed, so nothing has to prepare
 * it before a tool touches it. Each carries its own standard unwrap in UV set
 * 0, and a box's edges are `sharp` while the round ones shade smooth.
 *
 * Where it goes and what it is bound to are the viewer's: a primitive is built
 * around the origin and the caller places it.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// What a primitive is cut from (§3.16), all of it re-plannable.
struct PrimitiveParams {
    /// Half-extents for a box and a plane; x the radius and z the height for
    /// the round ones.
    Vector3f size{1.0f, 1.0f, 1.0f};
    u32 sides = 12;   ///< Around: a cylinder, a cone, a sphere's longitude.
    u32 segments = 1; ///< A plane's cells per side, a sphere's latitude bands.
};

/// A box of `2 * size`, six quads, every edge `sharp`.
Mesh MakeBox(const PrimitiveParams& params);

/// A plane in XY of `2 * size`, `segments` x `segments` quads.
Mesh MakePlane(const PrimitiveParams& params);

/// A cylinder of radius `size.x` and height `size.z`, `sides` side quads and an
/// n-gon at each end.
Mesh MakeCylinder(const PrimitiveParams& params);

/// A cone of radius `size.x` and height `size.z`: `sides` triangles and one
/// n-gon under them.
Mesh MakeCone(const PrimitiveParams& params);

/// A sphere of radius `size.x`: `sides` round, `segments` bands of latitude
/// (at least two), quads between them and a fan of triangles at each pole.
Mesh MakeSphere(const PrimitiveParams& params);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file hull.h
 * @brief A convex hull kept within a point budget (EDIT_MODE_PHYSICS_REDESIGN.md §17).
 *
 * StarCraft II's art tools make a physics hull of at most *Verts Count* points
 * (8 by default); `m3::CookHull` only merges faces to fit its tables and has no
 * budget. This picks the points: the extreme tetrahedron first, then, one at a
 * time, the point farthest outside the hull so far, until the budget is spent
 * or no point is outside. The cooker's own hull decides what "outside" is.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// At most @p maxPoints of @p points (at least 4), whose hull holds the rest
/// as nearly as that many can. Every point, when its hull has no more
/// vertices than that. Points that span no volume come back as they are, up
/// to the budget.
std::vector<Vector3f> HullWithin(std::span<const Vector3f> points, u32 maxPoints);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

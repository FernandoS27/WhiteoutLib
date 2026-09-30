// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file fit.h
 * @brief A box fitted to a cloud of skin points (EDIT_MODE_PHYSICS_REDESIGN.md §8.8).
 *
 * One axis is given: the bone. The other two are the cloud's principal axes
 * across it, so a chest's box is a slab rather than a cube. Each face stands
 * where a `share` of the points that face it lie within it: a skin cloud is a
 * surface, most of its points on the faces, and a few far ones -- a hand
 * beside a hip, a pauldron -- do not push a face out.
 */

#include <array>
#include <span>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

struct OrientedBox {
    Vector3f centre{0, 0, 0};
    /// Unit and square to each other: the major axis across the given one,
    /// the minor across it, and the given one last.
    std::array<Vector3f, 3> axes{Vector3f{1, 0, 0}, Vector3f{0, 1, 0}, Vector3f{0, 0, 1}};
    Vector3f halfExtents{0, 0, 0}; ///< Along each of `axes`.
};

/// The box over @p points with one axis along @p axis (the cloud's principal
/// axis when it has no length), each face at the @p share quantile of the
/// points assigned to it (0.75 is the capsule fit's tightness).
OrientedBox FitOrientedBox(std::span<const Vector3f> points, const Vector3f& axis, f32 share);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

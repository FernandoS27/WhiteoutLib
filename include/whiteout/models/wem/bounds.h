// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file bounds.h
 * @brief The one bounding volume WEM uses.
 *
 * MDX, M2, M3 and D3 each define their own; WEM keeps a single unified
 * definition so a mesh, a section, a model and an actor all speak the same
 * shape. Bounds are **derived** data everywhere they appear — recomputed, not
 * authored — with one exception: an actor's three bounds (§9) are what the
 * source file said and are carried, not recomputed.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {

struct Extent {
    // Zero-initialised, not merely declared: `Vector3f` is an aggregate over an
    // anonymous union with no initialisers, so a default-constructed `Extent`
    // whose bounds nobody set used to serialise indeterminate stack bytes.
    Vector3f minimum{};   ///< AABB min corner.
    Vector3f maximum{};   ///< AABB max corner.
    f32 sphereRadius = 0; ///< Bounding sphere radius.

    bool valid() const {
        return minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z;
    }
};

/// Resets to the empty extent that `grow` starts from — min at +inf, max at -inf,
/// so the first point sets both.
void ResetExtent(Extent& extent);

/// Grows @p extent to contain @p point. Does not touch `sphereRadius`.
void GrowExtent(Extent& extent, const Vector3f& point);

/// Sets `sphereRadius` to the distance from the AABB centre to its corner.
void FinishExtent(Extent& extent);

} // namespace wem
} // namespace models
} // namespace whiteout

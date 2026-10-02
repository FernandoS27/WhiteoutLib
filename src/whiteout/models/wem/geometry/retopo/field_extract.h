// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file field_extract.h
 * @brief From a position field to quads on the source surface
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md §6.3).
 *
 * Every work edge whose ends share a lattice point is collapsed, each collapse
 * checked by the link condition, so what is left keeps the piece's topology:
 * its vertices are the lattice points, and two triangles across a diagonal
 * step make a quad. Every output vertex is homed on the source.
 */

#include <whiteout/common_types.h>

#include "extract.h"
#include "position_field.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

struct FieldExtractStats {
    u32 collapses = 0;
    u32 refused = 0;   ///< Zero shifts the link condition kept apart.
    u32 quads = 0;
    u32 triangles = 0; ///< Left over: steps that do not close round them.
    u32 polygons = 0;  ///< Cells of five corners or six.
    u32 crushed = 0;   ///< Faces the lattice put on a point or a line: at their corners' own places.
    u32 welded = 0;    ///< Corners on one point made one.
    u32 absorbed = 0;  ///< Faces with no area taken into a neighbour.
    u32 flipped = 0;   ///< Faces that turn from the surface where placed.
};

/**
 * @brief The quads of @p field on @p mesh (collapsed in place: pass a copy),
 *        each edge's steps read from @p steps.
 *
 * With @p split, every face of n corners is then split into n quads through
 * its edge midpoints and centroid (§6.3's pure quads).
 */
QuadMesh ExtractFieldQuads(WorkMesh& mesh, const Surface& surface, const PositionField& field, LatticeSteps steps,
                           bool split, FieldExtractStats& stats);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

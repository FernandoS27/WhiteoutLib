// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file position_field.h
 * @brief The field method's frames and lattice on the work mesh's vertices
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md §6.1, §6.2).
 *
 * Each vertex holds a normal, an axis in its tangent plane and a lattice
 * origin: the points origin + scale (a axis + b axis'), a and b integers, are
 * where output vertices may go (Jakob, Tarini, Panozzo & Sorkine-Hornung,
 * "Instant Field-Aligned Meshes", 2015). Neighbours' lattices are smoothed
 * toward agreement, so wherever they agree the output is a grid.
 */

#include <cstdlib>
#include <vector>

#include <whiteout/common_types.h>

#include "cross_field.h"
#include "work_mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

/// What a vertex's lattice is held to.
enum class LatticePin : u8 {
    None,
    Line,  ///< A lattice line through the vertex along its axis: a feature curve.
    Point, ///< A lattice point at the vertex: a corner.
};

struct PositionField {
    f64 scale = 1.0;          ///< The lattice step: the quad edge.
    std::vector<V3> normals;  ///< Per work vertex, unit.
    std::vector<V3> axes;     ///< Per work vertex, unit, in the tangent plane.
    std::vector<V3> origins;  ///< Per work vertex, its lattice point nearest it.
    std::vector<LatticePin> pins;

    V3 across(u32 v) const {
        return Cross(normals[v], axes[v]);
    }
};

struct PositionOptions {
    f64 scale = 1.0;
    /// Smoothing sweeps on each level of the hierarchy, and on the finest.
    u32 sweeps = 8;
    u32 finestSweeps = 16;
};

/// The quarter turns (0..3) about @p normalB that bring @p axisB nearest
/// @p axisA: of the two crosses' arms, the pair nearest parallel decides, so
/// two frames across a crease, whose planes meet at a steep angle, still
/// match by the arm they share.
u32 MatchQuarter(const V3& normalA, const V3& axisA, const V3& normalB, const V3& axisB);

/// @p axis turned @p quarters quarter turns about @p normal.
V3 TurnQuarter(const V3& normal, const V3& axis, u32 quarters);

/// The lattice steps from one vertex's lattice point to another's, in the
/// first's frame (§6.3).
struct LatticeShift {
    i32 x = 0;
    i32 y = 0;
    /// How far apart the two lattices' matched points are, in lattice steps:
    /// near zero where the lattices agree.
    f64 gap = 0.0;

    bool zero() const {
        return x == 0 && y == 0;
    }
    bool unit() const {
        return std::abs(x) + std::abs(y) == 1;
    }
    bool diagonal() const {
        return std::abs(x) == 1 && std::abs(y) == 1;
    }
};

/// One end of a shift: where the vertex is and its lattice.
struct LatticeFrame {
    V3 position;
    V3 normal;
    V3 axis;
    V3 origin;
};

LatticeShift Shift(const LatticeFrame& a, const LatticeFrame& b, f64 scale);

/// The lattice steps along every work edge: per halfedge, from its start's
/// lattice point to its end's, in its start's frame.
struct LatticeSteps {
    std::vector<i32> x;
    std::vector<i32> y;
};

/// The steps of @p field as it stands, each edge's two directions agreeing.
LatticeSteps MeasureSteps(const WorkMesh& mesh, const PositionField& field);

/// Turns lattice steps (@p x, @p y) from one frame into another, where the
/// first's axis turned @p quarters quarter turns is the second's.
void TurnSteps(i32& x, i32& y, u32 quarters);

/// The quarter turns that take @p from's frame to @p to's (`MatchQuarter`).
u32 FrameQuarter(const PositionField& field, u32 to, u32 from);

/// The frames of §6.1, read from @p cross: each vertex's normal and axis, and
/// its pin. Origins are left at the vertices.
PositionField VertexFrames(const WorkMesh& mesh, const Surface& surface, const CrossField& cross);

/// Smooths @p field's origins at `options.scale` (§6.2), coarse to fine.
void SolvePositions(const WorkMesh& mesh, PositionField& field, const PositionOptions& options);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

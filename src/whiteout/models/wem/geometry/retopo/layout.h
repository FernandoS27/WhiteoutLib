// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file layout.h
 * @brief The patch layout: separatrices traced in the cross field until every
 *        patch is a rectangle, and the T-mesh of its sides
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.6).
 *
 * Traces are the motorcycle graph of Myles, Pietroni & Zorin (2014), run on the
 * work mesh's edges: each streamline is snapped as it goes to the nearer end
 * of every edge it crosses, so a layout path is a chain of edges and a patch a
 * set of whole triangles. Validity is then read off the result rather than
 * assumed from the traces: every layout vertex's sectors are measured in field
 * quarter turns, and a patch that is not a disk with four corners is repaired.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "cross_field.h"
#include "work_mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

struct LayoutOptions {
    /// How far along a path its direction is read, for the corner measure.
    f64 chord = 1.0;
    u32 repairRounds = 48;
    /// Every repair decision goes to `Layout::notes`, for a test to read.
    bool verbose = false;
};

/// A side's run along the border of a patch: an arc, and whether the patch
/// walks it the arc's own way.
struct SideArc {
    u32 arc = kNone;
    bool forward = true;
};

struct LayoutPatch {
    std::vector<u32> triangles;
    /// The border, as halfedges inside the patch, the patch on their left.
    std::vector<u32> loop;
    /// Four positions in `loop` where a side begins (a corner's outgoing
    /// halfedge), in loop order; empty for a patch that is no rectangle.
    std::vector<u32> corners;
    std::vector<SideArc> sides[4];
    bool rectangle = false;
    u32 loops = 0;  ///< Border loops; a disk has one.
    i32 euler = 0;  ///< Of the patch cut open along its layout edges; a disk's is one.
};

/// A path of layout edges between two nodes.
struct LayoutArc {
    std::vector<u32> vertices; ///< Work vertices, node to node.
    /// The patch on the arc's left and on its right, walking it node to node
    /// (kNone on a border), and which side of each the arc is on.
    u32 left = kNone;
    u32 right = kNone;
    u8 leftSide = 0;
    u8 rightSide = 0;
    f64 length = 0.0;
    u8 features = 0; ///< The source feature bits along it, OR'ed.
};

struct LayoutStats {
    u32 separatrices = 0;
    u32 repairTraces = 0;
    u32 centreSplits = 0;
    u32 forcedCorners = 0;
    u32 rounds = 0;
    u32 unfilled = 0; ///< Patches left no rectangle.
};

class Layout {
public:
    std::vector<u32> patchOf; ///< Per work triangle.
    std::vector<LayoutPatch> patches;
    std::vector<LayoutArc> arcs;
    std::vector<u32> nodeOf;        ///< Per work vertex: its node, or kNone.
    std::vector<u32> nodeVertex;    ///< Per node.
    /// Per halfedge leaving a layout vertex along a layout edge: the quarter
    /// turns of the sector counter-clockwise from it to the next layout edge.
    std::vector<u8> sectorTurns;
    /// Per work vertex: its total quarter turns when the layout forced it (a
    /// centre split's centre, a forced corner); 0 otherwise.
    std::vector<i32> turnsOverride;
    LayoutStats stats;
    std::vector<std::string> notes;
};

/**
 * @brief Traces and repairs the layout on @p mesh in @p field. Both are
 *        changed: edges are split where a trace must start, and each new
 *        triangle inherits its parent's cross.
 */
Layout BuildLayout(WorkMesh& mesh, CrossField& field, const Surface& surface,
                   const LayoutOptions& options);

/// Splits @p h's edge at @p point (re-homed on @p surface), keeping the cross
/// of the triangles it cuts. Returns the new vertex.
u32 SplitWithField(WorkMesh& mesh, CrossField& field, const Surface& surface, u32 h, const V3& point);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

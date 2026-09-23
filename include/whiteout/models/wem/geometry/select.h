// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file select.h
 * @brief The selection walkers: Loop, Ring, Grow, Shrink, Linked, the border
 *        loop and Convert (EDIT_MODE_MODELLING_DESIGN.md §2.5).
 *
 * Topology algorithms, device-free, over **points** (the caller's
 * `skinning::PointTable`, which it already caches): a walk entering a vertex
 * continues from every member of its point, so a loop crossing a binding crease
 * the weld kept apart continues on the far twin's edges, matched by position.
 * An edge is one pair of points; every edge realising that pair is taken with
 * it, as every member of a point is. All of them need the mesh's connectivity.
 */

#include <vector>

#include <whiteout/common_types.h>

#include "../skinning/points.h"
#include "ids.h"
#include "mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

using skinning::PointTable;

/// Elements of one mesh, each list sorted and without repeats. A Border-level
/// selection is its border edges, in `edges`.
struct ElementSet {
    std::vector<u32> vertices;
    std::vector<u32> edges;
    std::vector<u32> faces;

    bool empty() const {
        return vertices.empty() && edges.empty() && faces.empty();
    }

    /// Sorts each list and drops repeats.
    void normalise();
};

/// The level a selection is at (the viewer's Vertex, Edge, Border and Polygon).
enum class ElementLevel : u8 {
    Vertex,
    Edge,
    Border,
    Face,
};

/// Where Linked stops, as flags. `Angle` is 3ds Max's By Angle: it stops where
/// two faces meet at more than `LinkedParams::angle`.
enum class LinkDelimit : u8 {
    None = 0,
    Seam = 1,    ///< An edge flagged `seam`.
    Sharp = 2,   ///< An edge flagged `sharp`.
    Section = 4, ///< An edge between two sections.
    Angle = 8,   ///< Faces meeting at more than the angle asked for.
};

constexpr LinkDelimit operator|(LinkDelimit a, LinkDelimit b) {
    return static_cast<LinkDelimit>(static_cast<u8>(a) | static_cast<u8>(b));
}

constexpr bool HasDelimit(LinkDelimit value, LinkDelimit flag) {
    return (static_cast<u8>(value) & static_cast<u8>(flag)) != 0;
}

/// 3ds Max's Convert rule.
enum class ConvertRule : u8 {
    Touching, ///< Every element that touches the selection.
    Enclosed, ///< Only those all of whose vertices are selected.
};

/// The edge loop through @p edge: straight on through every point of four
/// edges (R§3.1), stopping at a pole or a border. A border edge walks its
/// border loop, as 3ds Max and Blender do.
ElementSet EdgeLoop(const Mesh& mesh, const PointTable& points, EdgeId edge);

/// The edge ring through @p edge: across each quad beside it to the opposite
/// edge, both ways, stopping at a face that is not a quad.
ElementSet EdgeRing(const Mesh& mesh, const PointTable& points, EdgeId edge);

/// The quads `EdgeRing` crosses from @p edge: a face loop.
ElementSet FaceLoop(const Mesh& mesh, const PointTable& points, EdgeId edge);

/// Every edge of the border loop @p edge lies on; empty for an interior edge.
ElementSet BorderLoop(const Mesh& mesh, const PointTable& points, EdgeId edge);

/// One ring more, per list: the points beside each selected point, the edges
/// touching a selected edge's points, the faces touching a selected face's.
ElementSet Grow(const Mesh& mesh, const PointTable& points, const ElementSet& selection);

/// One ring less, per list: a point, edge or face stays only while everything
/// around its points is selected too.
ElementSet Shrink(const Mesh& mesh, const PointTable& points, const ElementSet& selection);

/// Every element connected to the selection through faces sharing an edge, not
/// crossing an edge @p delimit names. Each non-empty list grows to its kind of
/// element over the connected faces.
ElementSet Linked(const Mesh& mesh, const PointTable& points, const ElementSet& selection,
                  LinkDelimit delimit);

namespace detail {
/// Linked's one body: `angle` is read only when `delimit` holds `Angle`.
ElementSet LinkedWithin(const Mesh& mesh, const PointTable& points, const ElementSet& selection,
                        LinkDelimit delimit, f32 angle);
} // namespace detail

/// The same, stopping also where two faces meet at more than @p angle radians
/// when @p delimit holds `Angle` (§3.17).
ElementSet LinkedByAngle(const Mesh& mesh, const PointTable& points, const ElementSet& selection,
                         LinkDelimit delimit, f32 angle);

/// Select Sharp (§3.17): every edge whose two faces meet at more than @p angle
/// radians — what Hard takes as its input.
ElementSet SharpEdges(const Mesh& mesh, f32 angle);

/// The selection at level @p from, as elements of level @p to (§4.3):
/// - to Vertex: every vertex of the elements;
/// - to Edge: touching, every edge with a selected end or on a selected face;
///   enclosed, every edge with both ends selected (from faces: every edge all
///   of whose faces are selected);
/// - to Border: the border loops touching the selection;
/// - to Face: touching, every face with a selected element; enclosed, every
///   face all of whose vertices (from edges: edges) are selected.
ElementSet Convert(const Mesh& mesh, const PointTable& points, const ElementSet& selection,
                   ElementLevel from, ElementLevel to, ConvertRule rule);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

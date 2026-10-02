// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file work_mesh.h
 * @brief The triangle mesh the retopology refines, traces and fills
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.4).
 *
 * A corner table of its own rather than the WEM kernel: it is split, collapsed
 * and flipped thousands of times and carries none of WEM's layers. Every vertex
 * has a home on the source (`SurfacePoint`); a feature vertex also its point on
 * a feature curve, a corner its source vertex. Halfedge `3t + i` runs from
 * corner i of triangle t to corner i + 1.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "surface.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

class WorkMesh {
public:
    // --- vertices ---
    std::vector<V3> positions;
    std::vector<SurfacePoint> homes;
    std::vector<FeaturePoint> onCurve; ///< Valid for `Feature` vertices.
    std::vector<u32> curve;            ///< A `Feature` vertex's curve; kNone otherwise.
    std::vector<VertexKind> kinds;
    std::vector<u32> sourceVertex;     ///< A `Corner`'s source vertex.
    std::vector<u32> out;              ///< One outgoing halfedge; kNone for a dead vertex.

    // --- triangles ---
    std::vector<u32> corners;  ///< Three per triangle.
    std::vector<u32> twins;    ///< Per halfedge, or kNone on a border.
    std::vector<u8> features;  ///< Per halfedge, the source's `FeatureBit`s.
    std::vector<u32> curves;   ///< Per halfedge, the source curve, or kNone.
    std::vector<u8> dead;      ///< Per triangle.

    static u32 Next(u32 h) {
        return h - h % 3 + (h % 3 + 1) % 3;
    }
    static u32 Prev(u32 h) {
        return h - h % 3 + (h % 3 + 2) % 3;
    }
    u32 from(u32 h) const {
        return corners[h];
    }
    u32 to(u32 h) const {
        return corners[Next(h)];
    }
    u32 triangleCount() const {
        return static_cast<u32>(corners.size() / 3);
    }
    u32 vertexCount() const {
        return static_cast<u32>(positions.size());
    }
    /// A source feature runs along @p h's edge.
    bool isFeature(u32 h) const {
        return (features[h] & kSourceFeatures) != 0;
    }
    /// A feature or a layout trace does: the edge bounds a patch.
    bool isLayout(u32 h) const {
        return features[h] != 0;
    }
    bool alive(u32 vertex) const {
        return out[vertex] != kNone;
    }

    /// Outgoing halfedges round @p vertex, counter-clockwise, starting after a
    /// border when there is one. Returns whether the fan is closed.
    bool ring(u32 vertex, std::vector<u32>& outgoing) const;
    bool isBorder(u32 vertex) const;
    u32 valence(u32 vertex) const;
    /// The halfedge from @p a to @p b, or kNone.
    u32 find(u32 a, u32 b) const;
    V3 triangleNormal(u32 t) const;

    u32 addVertex(const V3& position, const SurfacePoint& home, VertexKind kind);

    /// Splits @p h's edge with vertex @p vertex (made by `addVertex`, no
    /// triangles yet): two triangles become four, or one two on a border.
    void split(u32 h, u32 vertex);
    /// Merges `from(h)` into `to(h)`. The caller has checked `canCollapse`.
    void collapse(u32 h);
    /// Turns @p h's edge to join the two opposite corners. Interior edges only.
    void flip(u32 h);
    /// The link condition, and no interior edge between two border vertices
    /// (it would pinch the surface); a border vertex merged inward is
    /// `CollapseDirection`'s to refuse.
    bool canCollapse(u32 h) const;

    /// Drops dead triangles and vertices; returns the vertex remap.
    std::vector<u32> compact();
};

/// The triangles of @p component of @p surface, every vertex at its own home.
WorkMesh SeedWorkMesh(const Surface& surface, u32 component);

/// A feature point at work vertex @p vertex on @p curve: its own when it is a
/// feature vertex of that curve, else (a corner) the end of the curve's source
/// edge at its source vertex that heads toward @p toward -- a loop with one
/// corner has the corner at both ends, and a walk never passes a corner.
FeaturePoint CurveStart(const WorkMesh& mesh, const Surface& surface, u32 vertex, u32 curve, const V3& toward);

struct RemeshOptions {
    f64 targetLength = 1.0;
    u32 iterations = 8;
    f64 relaxation = 0.5;
};

struct RemeshStats {
    u32 splits = 0;
    u32 collapses = 0;
    u32 flips = 0;
};

/// The remesh's four stages, each usable alone: split every edge longer than
/// @p high (feature edges along their curve), collapse those under @p low
/// shortest first, flip toward valence six, and relax tangentially.
u32 SplitLong(WorkMesh& mesh, const Surface& surface, f64 high);
u32 CollapseShort(WorkMesh& mesh, f64 low, f64 high);
u32 FlipToValence(WorkMesh& mesh);
void Relax(WorkMesh& mesh, const Surface& surface, f64 relaxation);

/// What is wrong with the connectivity, or empty: twins agree, every live
/// vertex's `out` leaves it from a live triangle, no triangle repeats a vertex.
std::string Validate(const WorkMesh& mesh);

/// Botsch & Kobbelt's isotropic remesh, every vertex re-homed on @p surface
/// after every move, features held (§1.4).
RemeshStats Remesh(WorkMesh& mesh, const Surface& surface, const RemeshOptions& options);

/// Splits at its centroid every triangle with two or more feature edges, so each
/// triangle's field constraint is a single direction (§1.4). Returns how many.
u32 SplitFeatureCorners(WorkMesh& mesh, const Surface& surface);

/// Every vertex's position equals its home on @p surface within @p tolerance,
/// and every feature vertex's home is on its curve.
bool HomesConsistent(const WorkMesh& mesh, const Surface& surface, f64 tolerance);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

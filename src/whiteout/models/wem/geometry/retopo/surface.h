// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file surface.h
 * @brief The source surface every retopology vertex lives on
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.1, §1.3).
 *
 * The welded mesh as the triangles it is drawn as, with each triangle's WEM
 * face and each corner's WEM halfedge, the feature edges and the curves they
 * form, and the regions they cut the surface into. A point on it is a
 * `SurfacePoint`; the only way to find one is `locate`, a walk that never leaves
 * the connected surface near the point it starts from.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/geometry/mesh.h>
#include <whiteout/models/wem/geometry/retopo/retopology.h>

#include "vec.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

/// A point on the source: a triangle, and the weights of its second and third
/// corners (the first's is what is left).
struct SurfacePoint {
    u32 triangle = kNone;
    f64 u = 0.0;
    f64 v = 0.0;

    bool valid() const {
        return triangle != kNone;
    }
    f64 weight(u32 corner) const {
        return corner == 0 ? 1.0 - u - v : (corner == 1 ? u : v);
    }
};

/// A point on a feature edge: source halfedge `halfedge` at `t` from its start.
struct FeaturePoint {
    u32 halfedge = kNone;
    f64 t = 0.0;

    bool valid() const {
        return halfedge != kNone;
    }
};

/// What makes a source edge a feature (§1.3); more than one may hold.
enum FeatureBit : u8 {
    kFeatureBorder = 1,
    kFeatureSection = 2,
    kFeatureSharp = 4,
    kFeatureUvSeam = 8,
    kFeatureAngle = 16,
    kFeatureCrease = 32,
    /// Not a source feature: a work edge a layout trace runs along (§1.6).
    kLayoutTrace = 128,
};

/// The bits that come from the source, as opposed to the layout's traces.
inline constexpr u8 kSourceFeatures = 0x3F;

enum class VertexKind : u8 {
    Free,    ///< Moves anywhere on its region.
    Feature, ///< Moves along its feature curve.
    Corner,  ///< Never moves.
};

class Surface {
public:
    // --- the triangles -------------------------------------------------------
    std::vector<V3> positions;          ///< Per source vertex.
    std::vector<u32> corners;           ///< Three source vertices per triangle.
    std::vector<u32> twins;             ///< Per halfedge `3t + i` (corner i to i + 1), or kNone.
    std::vector<u32> faces;             ///< Per triangle, its WEM face slot.
    std::vector<u32> cornerHalfedges;   ///< Per triangle corner, the WEM corner (halfedge) it is.
    std::vector<V3> triangleNormals;
    std::vector<f64> triangleAreas;
    std::vector<V3> cornerNormals;      ///< Authored where the mesh has them, else the triangle's.
    bool authoredNormals = false;

    // --- features ------------------------------------------------------------
    std::vector<u8> features;           ///< Per halfedge, `FeatureBit`s; equal on twins.
    std::vector<u32> curves;            ///< Per halfedge, its feature curve, or kNone.
    /// Per feature halfedge, the curve's direction there (from `from` to `to`)
    /// over a quad edge of it, so a jagged curve reads as its course; empty
    /// until `ScaleFeatures`, and zero off features.
    std::vector<V3> tangents;
    std::vector<VertexKind> kinds;      ///< Per source vertex.
    std::vector<u32> vertexTriangles;   ///< Per source vertex, one triangle holding it.
    u32 curveCount = 0;

    // --- partitions ------------------------------------------------------------
    std::vector<u32> regions;           ///< Per triangle: no feature crosses a region.
    std::vector<u32> components;        ///< Per triangle: connected pieces.
    u32 regionCount = 0;
    u32 componentCount = 0;
    f64 diagonal = 0.0;

    u32 triangleCount() const {
        return static_cast<u32>(corners.size() / 3);
    }
    u32 vertexCount() const {
        return static_cast<u32>(positions.size());
    }
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
    bool isFeature(u32 h) const {
        return features[h] != 0;
    }

    // --- points ---------------------------------------------------------------
    V3 position(const SurfacePoint& point) const;
    /// The authored normal blended at @p point, unit.
    V3 normal(const SurfacePoint& point) const;
    V3 position(const FeaturePoint& point) const;
    SurfacePoint toSurface(const FeaturePoint& point) const;
    SurfacePoint atVertex(u32 vertex) const;
    /// @p triangle's point at weights @p w0, @p w1, @p w2.
    static SurfacePoint Make(u32 triangle, f64 w0, f64 w1, f64 w2);

    /**
     * @brief The point of the surface nearest @p point among the triangles
     *        connected to @p start through triangles that come within @p radius
     *        of it. The start triangles are always tried, near or not.
     */
    SurfacePoint locate(std::span<const u32> start, const V3& point, f64 radius) const;

    /// The same walk over the feature edges of @p start's curve.
    FeaturePoint locateOnCurve(const FeaturePoint& start, const V3& point, f64 radius) const;

    /// The triangles around source vertex @p vertex, in fan order.
    void fan(u32 vertex, std::vector<u32>& triangles) const;

    /// The feature halfedges at source vertex @p vertex, one per edge.
    const std::vector<u32>& featureEdges(u32 vertex) const {
        return vertexFeatureEdges_[vertex];
    }

private:
    mutable std::vector<u32> stamp_;
    mutable u32 stampValue_ = 0;
    std::vector<std::vector<u32>> vertexFeatureEdges_; ///< Per vertex, the feature halfedges leaving or entering it.

    u32 nextStamp() const;

    friend Surface BuildSurface(const Mesh& welded, const FeatureOptions& options);
    friend void ScaleFeatures(Surface& surface, f64 scale, f32 cornerAngle);
    friend void BuildCurves(Surface& surface);
};

/// @p welded must have connectivity: `PrepareForModelling` has run on it.
Surface BuildSurface(const Mesh& welded, const FeatureOptions& options);

/**
 * @brief Reads the feature curves at the quad edge @p scale.
 *
 * A curve point is a corner only where the curve turns by @p cornerAngle both
 * at the point and over half a quad edge each way, and where it bends hardest
 * within that reach: a jagged border or seam is one side of quads, not a
 * corner at every tooth. Points where curves meet stay corners. The curves
 * are rebuilt and `tangents` set.
 */
void ScaleFeatures(Surface& surface, f64 scale, f32 cornerAngle);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file physics_cook.h
 * @brief StarCraft II's cooked PHSH tables, built from authoring input
 *
 * A convex hull ships as a Domino polytope — vertices, face planes, twin-paired
 * half-edges, each face's first half-edge, the volume centroid, the volume and
 * the surface area — and the client uses those tables as they are. A mesh
 * ships as vertices and triangles plus the four values the client copies over
 * the tree it rebuilds at load: centre, extent, quantization tolerance and
 * tree height. Both are rebuilt here from points and triangles, with no
 * physics engine: `UpgradePhysics` cooks what an old file left to the client,
 * and the WEM export cooks every hull and mesh it writes.
 *
 * `ComputeMeshTree` reproduces `dmMeshBuilder_Build` (SC2 5.0, 0x103377f00) to
 * the float, because the client overwrites its own tree's values with the
 * file's and a mismatch misplaces every triangle.
 */

#include <span>

#include "structures.h"

namespace whiteout {
namespace m3 {

/// How a hull's points become a polytope.
enum class HullCook : u8 {
    /// The points' own convex hull, coplanar triangles merged into polygons.
    /// Points already on a hull keep their exact values, so re-cooking a
    /// shipped hull from its vertices gives it back.
    Exact,
    /// What the client does to a PHSH's source points at load
    /// (`dmPolytope_BuildFromHull`): faces whose normals are within 20° merged
    /// into one plane and the polytope rebuilt from the merged planes.
    ClientLoad,
};

/// What a hull cook did.
struct HullCookReport {
    bool ok = false;         ///< A polytope with volume came out; false leaves the tables empty
    bool simplified = false; ///< Faces were merged to fit the `u8` table limits
};

/// The `u8` limits of a cooked hull's tables.
inline constexpr u32 kMaxHullVertices = 255;
inline constexpr u32 kMaxHullFaces = 255;
inline constexpr u32 kMaxHullHalfEdges = 256;

/**
 * @brief Replaces @p shape's hull tables with a cook of @p points.
 *
 * The points are in the shape's own frame: the matrix is not applied. Every
 * other field of @p shape is left alone. Fewer than four points, or points
 * with no volume, give empty tables and `ok == false`.
 */
HullCookReport CookHull(PhysicsShape& shape, std::span<const Vector3f> points,
                        HullCook mode = HullCook::Exact);

/// The four tree values the client copies from a PHSH over its own mesh tree.
struct MeshTree {
    Vector3f center{};
    Vector3f extent{};
    Vector3f tolerance{};
    u32 height = 0;
    u32 vertexCount = 0; ///< One past the highest vertex a kept triangle uses
};

/// `dmMeshBuilder_Build`'s values for these triangles (three indices each),
/// degenerate ones dropped as the builder drops them.
MeshTree ComputeMeshTree(std::span<const Vector3f> vertices, std::span<const u32> triangles);

/**
 * @brief Replaces @p shape's mesh tables with a cook of the triangles.
 *
 * @p vertices are in the shape's frame. They are written about the tree
 * centre, as VEC4 (w = 0): the client builds its tree from the stored
 * vertices and then takes the centre from the record, which places them back.
 * MT32 triangles carry flags 0 and each edge's neighbouring vertex; no DMMN is
 * written (the client never reads it). Triangles keep their order, degenerate
 * ones included: the client drops those itself.
 */
void CookMesh(PhysicsShape& shape, std::span<const Vector3f> vertices, std::span<const u32> triangles);

} // namespace m3
} // namespace whiteout

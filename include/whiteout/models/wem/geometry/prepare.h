// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file prepare.h
 * @brief What makes vertices one point, and the weld that prepares a mesh for
 *        modelling (EDIT_MODE_MODELLING_DESIGN.md §2.2): what a merge of a
 *        modelled and an unmodelled mesh runs, so it is not the modelling
 *        tools' (`modelling.h`) alone.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "ids.h"
#include "mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// The distance under which two vertices are one point: 1e-5 of the bounding
/// diagonal of @p positions, so it scales with the mesh (the user's U5). Zero
/// for an empty or single-point set. `skinning::BuildPointTable` and the weld
/// both read it.
f32 CoincidenceTolerance(std::span<const Vector3f> positions);

/// `CoincidenceTolerance` over @p mesh's positions.
f32 CoincidenceTolerance(const Mesh& mesh);

/// Two influence weights of one bone agree when they differ by no more than
/// this: the members of a point hold the same skin to it.
inline constexpr f32 kSkinWeightTolerance = 1e-5f;

// ============================================================================
// The weld (§2.2)
// ============================================================================

/// What `PrepareForModelling` did.
struct PrepareReport {
    f32 tolerance = 0.0f;        ///< What counted as one position: CoincidenceTolerance.
    u32 verticesWelded = 0;      ///< Vertices merged away.
    u32 keptNonManifold = 0;     ///< Coincident vertices left apart: merging would not stay manifold.
    u32 keptBinding = 0;         ///< Coincident vertices left apart: their bindings differ.
    u32 keptFaceCorners = 0;     ///< Left apart: they are two corners of one face.
    u32 seamsMarked = 0;         ///< Edges the weld closed where an authored corner value differs.
    u32 sharpMarked = 0;         ///< Edges whose two sides' normals break, closed by the weld or not.
    std::vector<u32> vertexOf;   ///< Old vertex -> new. Identity when nothing welded.
    std::vector<u32> faceOf;     ///< Old face -> new. The weld drops no face (rules 2 and 3).
    std::vector<u32> cornerOf;   ///< Old corner -> new, flattened in face-set order.
};

/// Whether @p mesh carries the `modelled` marker: a prepare already ran on it.
bool IsModelled(const Mesh& mesh);

/**
 * @brief Makes @p mesh one connected surface wherever the file only split it for
 *        its corners (§2.2). Deterministic and idempotent; identity on a mesh
 *        carrying the `modelled` marker, which it sets.
 *
 * Coincident vertices (a point of `skinning::BuildPointTable`) merge, except:
 * 1. vertices whose effective bindings differ (skin within
 *    `kSkinWeightTolerance`, a rigid section's node, the `classicBones` pin);
 * 2. two corners of one face;
 * 3. vertices whose merge would not stay 2-manifold or would drop a face,
 *    found by trial `Repair` runs and refined along the repair's own partition.
 * Every member of a point is snapped to the position of its lowest-numbered
 * vertex, merged or not, so the point table still sees one point; a corner
 * kept apart because a face would lose its area keeps its own position. Each
 * edge the weld closes is marked `seam` where an authored corner value differs
 * across it (`SeamBetween`) and `sharp` where its normals differ by more than
 * 1.1 degrees.
 */
PrepareReport PrepareForModelling(Mesh& mesh);

/// Whether an authored corner value differs across @p h's edge, at either end:
/// every Halfedge layer but `normal`, `binormal` and the tangent's xyz, and the
/// sign of the tangent's `w` (a mirrored-UV line is a seam). False on a border.
bool SeamBetween(const Mesh& mesh, HalfedgeId h);

/// Whether the corner normals across @p h's edge differ by more than
/// @p angle radians at either end. False on a border.
bool NormalsBreakAcross(const Mesh& mesh, HalfedgeId h, f32 angle);

/// The prepare's normal break: 1.1 degrees.
inline constexpr f32 kWeldSharpAngle = 0.0191986218f;

namespace uv {

/// Rewrites `seam` over every interior edge of @p mesh from `SeamBetween`: set
/// where the two faces' corners differ in anything the surface holds, cleared
/// where they no longer do. A border edge is left alone (it is a border, not a
/// seam) and `sharp` is never touched, because that is the normals' answer and
/// not the corners'. Returns how many interior edges carry `seam` afterwards.
///
/// This is what a UV commit re-derives through: moving a corner's UV can make a
/// delimit and undoing it can take one away, and a modelling walk that stopped
/// where the old layout happened to split would be stopping at history.
u32 MarkDelimitSeams(Mesh& mesh);

/// The same rule over @p edges alone, and set-only: what the prepare marks the
/// edges a weld closed by, an edge that was already interior keeping the flags
/// it had. Returns how many of @p edges it marked.
u32 MarkDelimitSeams(Mesh& mesh, std::span<const u32> edges);

} // namespace uv

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

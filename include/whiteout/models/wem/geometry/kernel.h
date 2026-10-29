// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file kernel.h
 * @brief What the half-edge kernel keeps true of a mesh, and the data it
 *        derives: canonical numbering, compaction, normals, tangents and
 *        sharp edges. What anything holding a document needs; the edit
 *        operations over it are `ops.h`.
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

/// What `Canonicalize` moved: `GarbageCollect`'s vertex and face tables composed
/// with the halfedge and edge renumbering, so a caller holding ids can follow
/// them. `table[old]` is the new id, or `kInvalidId` for one that is gone; an
/// empty table is the identity.
struct CanonicalRemap {
    std::vector<u32> vertices;
    std::vector<u32> faces;
    std::vector<u32> halfedges;
    std::vector<u32> edges;

    bool identity() const {
        return vertices.empty() && faces.empty() && halfedges.empty() && edges.empty();
    }
};

/**
 * @brief Compacts the mesh and numbers its halfedges, edges and each vertex's
 *        outgoing halfedge as a fresh build of its face set would
 *        (EDIT_MODE_MODELLING_DESIGN.md §2.1).
 *
 * Every Halfedge layer is carried by corner correspondence and every Edge layer
 * with its edge, so no corner or edge value moves. The identity (an empty
 * remap) on a mesh no edit has renumbered, and on one without connectivity.
 * Implemented as a fresh build, never through `invalidateConnectivity`, which
 * calls this.
 */
CanonicalRemap Canonicalize(Mesh& mesh);

/// True when the numbering, `vertexOutgoing` included, is what a build of the
/// face set gives, and nothing is lazily deleted. A mesh without connectivity
/// is. For tests and `Validate`: it builds a second topology to compare.
bool IsCanonical(const Mesh& mesh);

/// True when an edit may have renumbered @p mesh since its connectivity was
/// last built: what `Canonicalize` would not skip. Cheap.
bool NumberingDirty(const Mesh& mesh);

/**
 * @brief Compacts the mesh: `Topology::garbageCollect` plus the attribute and
 *        skin remaps it implies.
 *
 * The kernel's own `garbageCollect` returns remap tables and applies none of
 * them, which is right for a kernel and wrong for anything holding attributes
 * alongside — the counts drift and the §5.7 structural invariant "every layer's
 * element count matches its domain" breaks. Deleting is lazy (§5.2), so *this*
 * is the call that makes a deletion real. The stored triangulation is remapped
 * with the rest; a row naming a deleted vertex is dropped.
 *
 * @return the topology's remap tables, so a caller holding indices can fix them.
 */
Topology::Remap GarbageCollect(Mesh& mesh);

/**
 * @brief Writes a rigid section's binding into the skin: every vertex of
 *        @p section gets `{rigidNode, 1}`, and `rigidNode` is cleared.
 *
 * @return false, mesh untouched, when the section is not rigid, or a vertex of
 *         it is shared with another section's faces — one vertex cannot bind
 *         both ways.
 */
bool BakeRigidNode(Mesh& mesh, u32 section);

// ============================================================================
// Derived data — connectivity untouched
// ============================================================================

/**
 * @brief Rewrites the `normal` Halfedge layer from the geometry.
 *
 * Corners around a vertex are averaged together while the angle between their
 * face normals stays under @p angleThreshold (radians) and neither the shared
 * edge's `sharp` flag nor a `smoothGroup` disagreement separates them — so a hard
 * edge produces two normals at one vertex, which is what the Halfedge domain is
 * for.
 */
void RecomputeNormals(Mesh& mesh, f32 angleThreshold = 1.047197551f);

/// Rewrites the `tangent` Halfedge layer from @p uvSet: the face-set form below
/// over every face, so the two never disagree. Does nothing when the mesh has
/// no such UV layer.
void RecomputeTangents(Mesh& mesh, u32 uvSet = 0);

// ---- The face-set re-shade (EDIT_MODE_MODELLING_DESIGN.md §2.7.10) ----------

/// The default normal-smoothing angle: 60 degrees.
inline constexpr f32 kDefaultShadingAngle = 1.047197551f;

/// The angle normals smooth across on @p mesh: pi on a `modelled` mesh, where
/// only `sharp` splits them (the weld turned the file's hard edges into it, and
/// 60 degrees would harden creases a low-poly model draws smooth), and
/// `kDefaultShadingAngle` elsewhere. The one place the choice is made.
f32 ShadingAngle(const Mesh& mesh);

/// `RecomputeNormals` over the corners at the vertices of @p faces only.
void RecomputeNormals(Mesh& mesh, std::span<const FaceId> faces, f32 angleThreshold);
/// The same over the corners at @p vertices. Both read the normals of the
/// faces round those vertices and of no other, so a patch costs its size.
void RecomputeNormalsAt(Mesh& mesh, std::span<const VertexId> vertices, f32 angleThreshold);

/**
 * @brief States a smoothing angle as `sharp` flags: an edge between two faces
 *        goes hard where they meet at more than @p angle (radians), and soft
 *        elsewhere unless @p keepHard.
 *
 * How an angle is applied to a `modelled` mesh, whose shading reads the flags
 * alone (`ShadingAngle`): shaded by an angle with no flag written, the crease
 * would be forgotten at the next re-shade. Two faces exactly at the angle are
 * soft. A border edge is left as it is.
 *
 * @return the number of flags that changed.
 */
u32 MarkSharpByAngle(Mesh& mesh, std::span<const EdgeId> edges, f32 angle, bool keepHard = false);

/// `MarkSharpByAngle` over every edge.
u32 MarkSharpByAngle(Mesh& mesh, f32 angle, bool keepHard = false);

/**
 * @brief Rewrites the tangents of the corners at the vertices of @p faces.
 *
 * MikkTSpace's rule, which Warcraft III's files follow and the bake tools use.
 * Each triangle a face is drawn as gives its corners its unit dP/du, laid into
 * the corner's normal plane and weighted by the corner's angle. The corners at
 * a vertex that share a normal, a UV and a UV winding share the sum, so a UV
 * seam or a mirror line keeps two tangents at one vertex and a crease smoothed
 * into one normal gets one. It reads the `normal` layer: re-shade first.
 *
 * `w` is the side `names::kTangent` states: +1 where the UVs wind against the
 * face, so `w * cross(normal, tangent)` runs toward decreasing v.
 *
 * A triangle whose UV map has no area contributes nothing; corners left with
 * nothing take a tangent along their face's first edge, orthogonal to their
 * normal, and `w` +1.
 */
void RecomputeTangents(Mesh& mesh, std::span<const FaceId> faces, u32 uvSet);
/// The same over the corners at @p vertices.
void RecomputeTangentsAt(Mesh& mesh, std::span<const VertexId> vertices, u32 uvSet);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file ops.h
 * @brief The edit-operation surface over the half-edge kernel (WEM v3, §5.7).
 *
 * Every operation takes a whole `Mesh`, never a bare `Topology`, because an edit
 * that adds a vertex has to add a vertex *attribute element* and a skin entry in
 * the same breath. Splitting that responsibility across two calls is how an
 * attribute set drifts out of step with its domain — the one §5.7 structural
 * invariant a caller would otherwise have to remember.
 *
 * ### Two classes of operation
 *
 * **Maintaining** ops rewrite the half-edge arrays in place, so every handle a
 * caller holds outside the immediate neighbourhood stays valid.
 *
 * **Rebuilding** ops go out through the indexed face set and come back through a
 * fresh connectivity build. All handles are invalidated; halfedge attributes are
 * carried across by corner correspondence, so *data* survives even though
 * *handles* do not.
 *
 * `UnifyWinding` is in the second class for a reason worth stating, because it
 * is not a shortcut: reversing a face's winding means that face now wants the
 * halfedge `b->a` where it had `a->b` — and `b->a` is `h^1`, already owned by the
 * neighbour. With opposites implicit (§5.2) an in-place flip is not a local
 * operation, which is also exactly why the §5.3 repair *splits* inconsistent
 * windings rather than flipping them.
 *
 * `CollapseEdge` is maintaining, and is gated: `IsCollapseLegal` refuses both the
 * collapses that would produce a non-manifold link (the design's requirement) and
 * the loop configurations this kernel does not splice — a boundary loop that
 * would be left with two halfedges. A refused collapse changes nothing.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "../diagnostics.h"
#include "ids.h"
#include "mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

// ============================================================================
// Maintaining — connectivity rewritten in place
// ============================================================================

/**
 * @brief Inserts a vertex at `lerp(from, to, t)` along @p edge.
 *
 * Both incident loops gain one corner; no face is added, so this is valid for
 * n-gons as well as triangles. Each new corner blends the two corners the edge
 * spans *in its loop* through `BlendCorners` (interpolate.h), both loops from
 * the same ordered pair, so a split of a continuous edge stays bit-equal across
 * it. The new vertex's skin and Vertex layers blend its ends' the same way
 * (`BlendVertex`), and it is a point of its own: a fresh `mergeGroup`.
 *
 * @return the new vertex, or an invalid id if @p edge is not usable.
 */
VertexId SplitEdge(Mesh& mesh, EdgeId edge, f32 t = 0.5f);

/**
 * @brief Cuts @p face along the diagonal between two of its corners.
 *
 * The half holding @p cornerA keeps the original face (and therefore its section
 * and every other Face attribute); the other half becomes a new face with those
 * values copied.
 *
 * @return the new face, or an invalid id when the corners are equal, adjacent,
 *         out of range, or already joined by an edge elsewhere (a second edge
 *         between one pair of vertices, which no rebuild can hold).
 */
FaceId SplitFace(Mesh& mesh, FaceId face, u32 cornerA, u32 cornerB);

/// `SplitFace` addressed by halfedges rather than corner ordinals — what
/// `Triangulate` walks with.
FaceId SplitFaceAt(Mesh& mesh, HalfedgeId a, HalfedgeId b);

/**
 * @brief True when collapsing `from(h)` into `to(h)` leaves a valid 2-manifold.
 *
 * The link condition for polygons (EDIT_MODE_MODELLING_DESIGN.md §2.7.9): the
 * vertices the two one-rings share are exactly the apexes of the *triangular*
 * side faces, and no face other than the two side faces holds both ends. A quad
 * or an n-gon beside the edge has no apex, and loses a corner.
 */
bool IsCollapseLegal(const Mesh& mesh, HalfedgeId h);

/**
 * @brief Collapses `from(h)` into `to(h)`, removing the vertex and the edge.
 *
 * An incident triangle collapses to a single edge: its two remaining edges merge,
 * one is deleted, and the survivor is spliced into the deleted one's loop —
 * carrying that corner's halfedge attributes with it. Deletion is lazy (§5.2), so
 * `garbageCollect()` still has to run to compact.
 *
 * @return false, changing nothing, when `IsCollapseLegal` says no.
 */
bool CollapseEdge(Mesh& mesh, HalfedgeId h);

/**
 * @brief Rotates @p edge inside the quad its two triangles form.
 *
 * Refuses a boundary edge, a non-triangular incident face, and a rotation whose
 * new diagonal already exists — that last one would make the edge non-manifold.
 */
bool FlipEdge(Mesh& mesh, EdgeId edge);

/// Cuts @p face along the diagonals it is drawn with (`TriangulateFace`: its
/// stored row when valid, else the automatic rule), so it becomes the triangles
/// it showed. @return the number of faces added.
u32 Triangulate(Mesh& mesh, FaceId face);

/**
 * @brief Merges the two faces sharing @p edge into one and deletes the edge.
 *
 * @p survivor is the face that stays, and must be one of the two; invalid keeps
 * the face on the edge's even side. A join keeps the lower-numbered face.
 *
 * @return false, changing nothing, for a boundary edge; an edge whose two sides
 *         are one face; two faces of different sections (one would take the
 *         other's material); a merged loop that would visit a vertex twice; and
 *         a merged face with the corners of a face already there.
 */
bool DissolveEdge(Mesh& mesh, EdgeId edge, FaceId survivor = FaceId());

/**
 * @brief Removes a valence-2 vertex, merging its two edges into one.
 * @return false, changing nothing, when the valence is not 2, when a face
 *         beside it would drop below three corners, or when its two neighbours
 *         already share an edge.
 */
bool DissolveVertex(Mesh& mesh, VertexId vertex);

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

// ============================================================================
// Rebuilding — handles invalidated, attribute data carried across
// ============================================================================

/// `Triangulate` on every face with more than three corners. @return faces added.
u32 TriangulateAll(Mesh& mesh);

struct WeldResult {
    u32 verticesMerged = 0;
    u32 facesDropped = 0; ///< Faces a weld made degenerate.
};

/**
 * @brief Merges vertices closer than @p epsilon — the F2 fix, and what makes
 *        cross-format round trips converge.
 *
 * With @p respectMergeGroups (the default) two vertices only merge when they
 * already share a merge group — so the weld can re-merge what the §5.3 repair
 * split apart, but never fuse two distinct source vertices. The render view was
 * already grouping those copies into one GPU vertex, so the §5.8 identity
 * property is untouched by that form.
 *
 * Clearing the flag welds by position alone. *That* is the F2 fix, it is what
 * makes cross-format round trips converge, and it is also what breaks the
 * identity property — which is why it is an explicit user action and not
 * something import does.
 */
WeldResult WeldVertices(Mesh& mesh, f32 epsilon, bool respectMergeGroups = true);

/**
 * @brief Splits every vertex whose corners disagree on any of @p layers — the
 *        inverse of a weld, and what an exporter runs before writing a format
 *        with no per-corner attributes.
 *
 * `mergeGroup` is preserved across the split, so the render view regroups them
 * and the identity property is unaffected.
 *
 * Separating a vertex's fans can leave two faces meeting at a single vertex, and
 * the rebuild's repair then splits that bowtie too — so the result can hold more
 * vertices than there were distinct attribute tuples.
 *
 * @return the number of vertices added, counting that.
 */
u32 SplitVertexByHalfedgeAttr(Mesh& mesh, std::span<const std::string> layers);

/// @return the number of faces whose winding was reversed.
u32 UnifyWinding(Mesh& mesh);

/// Concatenates @p meshes into one. Sections, attribute layers, skin bindings,
/// merge groups and stored triangulations are all renumbered into the combined
/// space.
Mesh MergeMeshes(std::span<const Mesh> meshes);

/// One mesh per section, in section order. A section with no faces still yields a
/// mesh, so the result indexes like `Mesh::sections`.
std::vector<Mesh> SplitMesh(const Mesh& mesh);

/**
 * @brief Relabels every face of @p sections to @p keep and removes the others,
 *        renumbering the rest — the face layer and the repair log alike.
 *
 * What `MergeMeshes` leaves undone when the goal is one draw section: it keeps
 * every input's section, and an exporter writes one draw per section. The kept
 * section's metadata is the result's.
 *
 * @return the section remap (`remap[old]` = new index, `kInvalidId` for a
 *         removed section, whose faces are now @p keep's). Empty, mesh
 *         untouched, when the sections disagree on `rigidNode` or on the
 *         visibility gate (`kSectionVisibilityNode`), or an index is out of
 *         range.
 */
std::vector<u32> MergeSections(Mesh& mesh, std::span<const u32> sections, u32 keep);

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

/// Rewrites the `tangent` Halfedge layer (F32x4, w = handedness) from @p uvSet.
/// `w` is -1 where the UV-derived bitangent opposes `cross(normal, tangent)` —
/// a mirrored island — so `bitangent = w * cross(normal, tangent)` everywhere.
/// A polygon's face tangent is the UV-area-weighted mean over the triangles it
/// is drawn as. Does nothing when the mesh has no such UV layer.
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

/**
 * @brief Rewrites the tangents of the corners at the vertices of @p faces.
 *
 * Corners are grouped inside the normal groups (`ShadingAngle`), then split
 * where their UVs or their face's handedness differ, so a UV seam or a mirror
 * line keeps two tangents at one vertex and a crease smoothed into one normal
 * gets one tangent. A face whose UV map has no area contributes nothing; a group
 * left with nothing takes a tangent along its face's first edge, orthogonal to
 * its normal.
 */
void RecomputeTangents(Mesh& mesh, std::span<const FaceId> faces, u32 uvSet);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

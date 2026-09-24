// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file modelling.h
 * @brief Preparing a mesh for modelling, and the tools' contract
 *        (EDIT_MODE_MODELLING_DESIGN.md §2.2, §3.1).
 *
 * It starts with the two numbers every workspace that joins vertices into
 * points shares, so that a point is one thing wherever it is asked (D13).
 */

#include <cfloat>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "interpolate.h"
#include "mesh.h"
#include "ops.h"
#include "select.h"

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
    u32 sharpMarked = 0;         ///< ... where the corner normals differ.
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

// ============================================================================
// The contract every tool keeps (§3.1)
// ============================================================================

/// Why a tool refused, one per sentence the viewer shows (§5.6); `None` when it
/// did not. A refused plan leaves the mesh untouched.
enum class ModelRefusal : u8 {
    None,
    EmptySelection,
    DifferentMeshes,
    WouldPinch,
    CrackNeedsWeld,
    NotAdjacent,
    BorderEdgeRemove,
    BorderEdgeChamfer,
    WouldRepeatVertex,
    WouldDuplicateFace,
    WouldFold,
    CollapseIllegal,
    TargetCollapseIllegal,
    AllFaces,
    ZeroAmount,
    ConcaveTurn,
    DiagonalOutside,
    AdjacentCorners,
    NotOneFace,
    DiagonalExists,
    CapTooShort,
    CrossesSection,
    DissolveVertexIllegal,
    NothingToSlice,
    NoSourceSide,
    WindingDisagrees,
    LoopCountsDiffer,
    SameLoop,
    NotBuiltYet,
};

const char* ToString(ModelRefusal refusal);

/// A vertex a tool placed, as a function of its amount: base + amount * direction.
struct VertexMotion {
    u32 vertex = kInvalidId;
    Vector3f base{0.0f, 0.0f, 0.0f};
    Vector3f direction{0.0f, 0.0f, 0.0f};
};

/// One class of corners (§2.4) whose values depend on the amount: re-sampled
/// from @p source at the class's vertex's new position, and written alike to
/// every corner of the class.
struct Resample {
    u32 targetVertex = kInvalidId;         ///< Skin re-blended, when valid.
    std::vector<HalfedgeId> targetCorners; ///< One class: all get the same bytes.
    SourcePolygon source;                  ///< §2.4, captured at Plan.
};

/// What a tool made: its topology, fixed; how its amount moves it; what it
/// selects; and why it refused, when it did (then `mesh` is untouched).
struct ModelPlan {
    ModelRefusal refusal = ModelRefusal::None;
    std::vector<VertexMotion> motions;
    std::vector<Resample> resamples;
    f32 amountMin = -FLT_MAX; ///< `AmountRange`'s; 0 lies inside.
    f32 amountMax = FLT_MAX;
    ElementSet selection;           ///< The tool's output.
    std::vector<u32> touchedEdges;  ///< Edges made or re-shaped: §3.12's derivations.
    std::vector<u32> changedFaces;  ///< Faces whose rows `FinishTool` materialises.
    /// The edges a dissolve took AWAY -- To Quads' joins and To Polygons'
    /// dissolves -- numbered as the mesh Plan was handed was numbered.
    ///
    /// `FinishTool` does not remap it, because there is nothing left to remap
    /// to; it is meaningful only against the mesh as it came in, which is
    /// exactly what a preview planning on a copy holds. That is what it is for:
    /// a count says how many edges go, and this says which.
    std::vector<u32> dissolvedEdges;
    bool renumbers = true;          ///< False for the row-only tools: no element moved id.
    bool fellBack = false;          ///< Extrude's Group fell back to Local Normal (§3.8).
    u32 kept = 0;                   ///< What the tool left as it was, for the result line.
    u32 changed = 0;                ///< What it did, for the result line.

    bool refused() const {
        return refusal != ModelRefusal::None;
    }
};

/**
 * @brief §2.4's one value per class, for @p vertex moving to @p to: the corners
 *        at @p vertex grouped by their bytes (bit-equal corners are one class),
 *        in fan order, and one `Resample` per class.
 *
 * Each class samples the first of its faces, in fan order, whose polygon (as
 * captured now) holds @p to, else its first face; so only corners that already
 * differed at the vertex (a seam through it) sample their own face. The class
 * holding the fan's own source face carries the vertex's skin.
 */
std::vector<Resample> ResamplesAt(const Mesh& mesh, VertexId vertex, const Vector3f& to);

/// D14: the nearest amounts either side of 0 at which any drawn triangle of any
/// face the plan moves reaches zero signed area on its Plan-time normal. Every
/// point is linear in the amount, so each area is a quadratic with exact roots.
void AmountRange(const Mesh& mesh, ModelPlan& plan);

/// Positions at @p amount (clamped into the plan's range), resamples re-blended,
/// the faces around them re-shaded (§2.7.10). Positions and attributes only:
/// the topology is Plan's.
void ApplyAmount(Mesh& mesh, const ModelPlan& plan, f32 amount);

/// The end of every tool, in order: §3.12's crease rule (`sharp` on every edge
/// in `touchedEdges` whose dihedral exceeds @p creaseAngle) and `seam`
/// derivation on `touchedEdges`; the re-shade around them; `Canonicalize`
/// (§2.1), renumbering `selection` through its remap; `MaterialiseRows` over
/// `changedFaces` (§2.3); `recomputeBounds`. Nothing on a refused plan.
void FinishTool(Mesh& mesh, ModelPlan& plan, f32 creaseAngle = kDefaultShadingAngle);

// ============================================================================
// The first tools (§3.2-§3.4, §3.6)
// ============================================================================

/// To Quads (§3.2): Blender's Triangles to Quads.
struct JoinTrianglesParams {
    f32 faceAngle = 0.698131701f;  ///< 40 degrees: the two normals differ by at most this.
    f32 shapeAngle = 0.698131701f; ///< 40 degrees: every quad corner within this of 90.
    bool delimitFlagged = true;    ///< Never across a `seam` or `sharp` edge.
    f32 topologyInfluence = 0.0f;  ///< Blender 4.4's grid propagation; a later phase builds it.
    bool deselectJoined = false;   ///< Output the triangles left over instead.
};

/// Joins pairs of triangles among @p faces into quads, cheapest first, never
/// across a section, smoothing group or corner-value difference (§3.2). Each
/// quad keeps both triangles as its row, so what the file gets is unchanged.
ModelPlan PlanJoinTriangles(Mesh& mesh, const PointTable& points, const ElementSet& faces,
                            const JoinTrianglesParams& params = {});

/// To Polygons (§3.3).
struct LimitedDissolveParams {
    f32 angle = 0.0872664626f; ///< 5 degrees.
    bool delimitFlagged = true;
};

/// Dissolves interior edges between faces of @p faces, flattest first, keeping
/// every vertex; refuses each dissolve whose union row is not a valid cut or
/// strays more than `angle` from the merged face's normal.
ModelPlan PlanLimitedDissolve(Mesh& mesh, const PointTable& points, const ElementSet& faces,
                              const LimitedDissolveParams& params = {});

/// Turn (§3.4): the drawn diagonal @p a - @p b of @p face swapped for the other
/// diagonal of its two triangles. Rows only.
ModelPlan PlanTurn(Mesh& mesh, FaceId face, VertexId a, VertexId b);

/// Edit Triangulation (§3.4): the diagonal @p a - @p b added to @p face's cut;
/// the diagonals it crosses are re-cut by the automatic rule on each side.
ModelPlan PlanEditTriangulation(Mesh& mesh, FaceId face, VertexId a, VertexId b);

/// Retriangulate (§3.4): @p faces re-cut by the automatic rule and stored.
ModelPlan PlanRetriangulate(Mesh& mesh, const ElementSet& faces);

/// Triangulate (§3.4): @p faces split into their drawn triangles. A face whose
/// cut uses a diagonal that already is an edge stays a polygon, counted in `kept`.
ModelPlan PlanTriangulate(Mesh& mesh, const ElementSet& faces);

/// Target Weld (§3.6): the point of @p source onto the point of @p target.
ModelPlan PlanTargetWeld(Mesh& mesh, const PointTable& points, VertexId source, VertexId target);

/// Target Weld at the Border level: border edge @p source onto border edge
/// @p target, `from` onto `to` and `to` onto `from`.
ModelPlan PlanTargetWeldEdges(Mesh& mesh, const PointTable& points, EdgeId source, EdgeId target);

/// Weld's default threshold: `CoincidenceTolerance` x 100.
f32 DefaultWeldThreshold(const Mesh& mesh);

/// Weld (§3.6): every selected vertex within @p threshold of another merges, the
/// members of one point included, all at once; each cluster keeps its most
/// central vertex at the cluster's centroid.
ModelPlan PlanWeld(Mesh& mesh, const PointTable& points, const ElementSet& selection, f32 threshold);

/// Weld's `onto` form (Weld on Snap): each vertex of @p sources merges onto one
/// vertex of @p targets within @p threshold, one to one, preferring the one bound
/// alike, then the nearest; never two sources together, so the twins of a crease
/// moved as one point stay apart. Both lists are taken as given, whole points
/// named by the caller: after a Snap the point table would make them one.
ModelPlan PlanWeld(Mesh& mesh, const PointTable& points, const ElementSet& sources,
                   const ElementSet& targets, f32 threshold);

// ============================================================================
// Collapse, Remove, Delete, Cap, Flip, Hard and Soft (§3.7, §3.10)
// ============================================================================

/**
 * @brief Collapse (§3.7): each connected part of @p selection, over points,
 *        becomes one vertex at its centroid.
 *
 * The survivor's skin is `BlendInfluences` over the part's vertices at equal
 * weight, and per UV island its corners take the centre of that island's UV
 * bounding box from before the collapse, which keeps the seams where they were.
 * Atomic: planned on a copy, and any part the kernel would have to keep apart
 * refuses the whole (`CollapseIllegal`).
 */
ModelPlan PlanCollapse(Mesh& mesh, const PointTable& points, const ElementSet& selection);

/// Remove's one parameter (§3.7): 3ds Max's Ctrl+Backspace, which takes the
/// valence-2 vertices a dissolve leaves with it.
struct RemoveParams {
    bool cleanVertices = false;
};

/**
 * @brief Remove (§3.7): the selected edges dissolved, and the selected vertices
 *        replaced by one face over their ring, so the surface stays closed.
 *
 * A border edge is refused (`BorderEdgeRemove`): Remove closes a surface, and a
 * hole's edge has nothing to close. A vertex's ring is refused when it would
 * repeat a vertex, when the faces around it differ in section, and when the
 * ring's own projection folds. Atomic, as Collapse is.
 */
ModelPlan PlanRemove(Mesh& mesh, const PointTable& points, const ElementSet& selection,
                     const RemoveParams& params = {});

/**
 * @brief Delete (§3.7): the face set without @p faces, through the rebuild.
 *
 * The repair splits every vertex the deletion pinched, one per fan, and the
 * twins keep the original's `mergeGroup`, so the file still writes one vertex
 * wherever their corners agree. Isolated vertices go. Deleting every face is
 * refused (`AllFaces`).
 */
ModelPlan PlanDelete(Mesh& mesh, const ElementSet& faces);

/**
 * @brief Cap (§3.10): the border loop @p edge lies on closed with one polygon.
 *
 * Each corner copies the corner of the face beside the border edge leaving that
 * vertex, so the cap is continuous with its rim, and the cap takes the section
 * and smoothing group of the lowest-numbered rim face. Refused for a loop of
 * fewer than three edges (`CapTooShort`), one whose projection folds
 * (`WouldFold`), and one whose vertices are already a face's
 * (`WouldDuplicateFace`).
 */
ModelPlan PlanCap(Mesh& mesh, const PointTable& points, EdgeId edge);

/**
 * @brief Flip (§3.10): @p faces reversed, detaching cleanly along their border.
 *
 * Every border vertex of the region is duplicated once per fan of flipped
 * faces, with a fresh `mergeGroup`, before the region is reversed: the kernel
 * cannot hold two faces that disagree on an edge's direction, and leaving it to
 * the repair would shatter the region instead. Each flipped corner's normal is
 * negated and its tangent's `w` with it, so the bitangent is kept, and each
 * stored triangle is reversed. The file does not see any of it: the vertices
 * are coincident and keep their skin.
 */
ModelPlan PlanFlip(Mesh& mesh, const ElementSet& faces);

/// Hard and Soft (§3.10): `sharp` set or cleared on @p edges, whose faces the
/// finish re-shades. Nothing else moves, and nothing renumbers.
ModelPlan PlanSharpen(Mesh& mesh, const ElementSet& edges, bool hard);

// ============================================================================
// Extrude, Border Extrude and Inset (§3.8)
// ============================================================================

/// Extrude's three ways of aiming what it moves (§3.8), 3ds Max's.
enum class ExtrudeType : u8 {
    Group,       ///< One direction per region: its area-weighted mean normal.
    LocalNormal, ///< Per vertex: the mean of its selected faces' normals.
    ByPolygon,   ///< Each face on its own, along its own normal.
};

/**
 * @brief Extrude (§3.8): @p faces lifted, with a wall on every boundary edge.
 *
 * A region is the selected faces connected across edges *and across twin edges*
 * (§2.2), so a crease the weld kept apart extrudes as one: one direction, no
 * pair of walls back to back inside the solid, and the twins' copies stay
 * twins. A vertex inside a region moves with it; a boundary vertex is copied
 * once per fan of selected faces around it, which keeps a region that touches
 * itself manifold; and every boundary edge grows one wall quad wound to match
 * its region face, a boundary edge on a hole included.
 *
 * `Group` falls back to `LocalNormal` for a region whose mean normal is shorter
 * than 0.1 (a band, or a closed region, where the normals cancel) or one of
 * whose boundary edges lies within a degree of parallel to it (the wall would
 * have zero area at every height); `fellBack` then says so.
 *
 * The amount is the height. Output selection: the extruded faces.
 */
ModelPlan PlanExtrudeFaces(Mesh& mesh, const PointTable& points, const ElementSet& faces, ExtrudeType type);

/**
 * @brief Border Extrude (§3.8): a strip grown from the border runs in @p edges.
 *
 * Every border vertex of each selected loop or open run is copied, with one
 * quad per border edge, and aimed along the mean of those edges' OUTWARD
 * directions: per edge, away from the centre of the face behind it, in that
 * face's plane and square to the edge -- so the answer does not depend on how
 * the face is triangulated. The amount is the distance.
 *
 * So the strip continues the surface rather than standing up off it: a plane
 * extruded at its border gets wider, a tube's rim runs on along the tube, and a
 * hole's border closes inward. (Until 2026-09-24 the aim was the face NORMAL,
 * which grew a collar instead.) The offset is not mitred at a corner, so a
 * vertex shared by two edges meeting at 2θ travels its full amount along the
 * bisector and clears the surface by `amount·cos θ`.
 *
 * Output selection: the new border, ready for the gizmo after Apply.
 */
ModelPlan PlanExtrudeBorder(Mesh& mesh, const PointTable& points, const ElementSet& edges);

/// Inset's two ways (§3.8): one ring per region, or one per face.
enum class InsetType : u8 { Group, ByPolygon };

/**
 * @brief Inset (§3.8): extrude's topology at height 0, the inner vertices moved
 *        in their tangent plane.
 *
 * Each inner vertex slides along the bisector of its two boundary edges within
 * its own fan, scaled by 1 / sin(half the corner angle), so every inner edge
 * ends up exactly the amount from its outer twin: Blender's even offset.
 * Boundary is on, as Blender has it, so a region edge on a hole gets its ring
 * quad too.
 *
 * `amountMax` is `AmountRange`'s (D14), which covers an inner edge collapsing,
 * a face inverting, and a reflex inner vertex crossing an inner edge that is
 * not its own. The inner vertices' corners and skin are `Resample`'s (§2.4), so
 * the texture and the binding stay where they were on the surface.
 *
 * Output selection: the inner faces.
 */
ModelPlan PlanInset(Mesh& mesh, const PointTable& points, const ElementSet& faces, InsetType type);

// ============================================================================
// Chamfer (§3.9)
// ============================================================================

/// Chamfer's parameters (§3.9). The first phase cuts one segment with a flat
/// profile, so the other two are refused until they are built.
struct ChamferParams {
    u32 segments = 1; ///< Past 1: `NotBuiltYet`.
    bool open = false; ///< True: `NotBuiltYet`.
};

/**
 * @brief Chamfer (§3.9): each selected edge replaced by a strip, and each
 *        vertex the selection touches by a ring of points.
 *
 * At every touched vertex each of its edges gets a point: an unselected edge
 * gets one on itself, `amount / sin θ` along it where it shares a face with a
 * selected edge at an angle θ inside [1°, 179°] — so the offset line in that
 * face stays exactly `amount` from the selected edge — and `amount` otherwise,
 * the nearer of two such offsets winning. A face whose corner there lies
 * outside that band (a straight corner, a reflex one) gives a face point
 * instead, and a face whose two edges at the vertex are both selected gives the
 * miter point where its two offset lines meet. The points round the vertex are
 * its ring: three or more close into a patch, two leave the strips meeting
 * along one edge. Members of one point are chamfered as one vertex.
 *
 * `amountMax` is `AmountRange`'s, which covers the edge events *and* the miter
 * events an edge-only rule misses (a cube with all twelve edges chamfered stops
 * at 0.5, where its faces' inner squares run out).
 *
 * Edge and miter points carry `Resample`s, since their distance moves with the
 * amount; a strip's corners come from its own side faces, and across a `seam`
 * from one of them, so the seam stays crisp on the strip's far edge.
 *
 * A selected border edge is refused (`BorderEdgeChamfer`) while `open` is not
 * built: the rules assume a face on each side.
 *
 * Output selection: the strips.
 */
ModelPlan PlanChamferEdges(Mesh& mesh, const PointTable& points, const ElementSet& edges,
                           const ChamferParams& params = {});

// ============================================================================
// Adding topology by hand (§3.14)
// ============================================================================

/// Insert Vertex (§3.14): @p edge split at @p t, `SplitEdge`'s vertex. Its
/// corners are lerped from one ordered pair, its skin blended, and it is a
/// point of its own. Nothing moves afterwards, so the plan has no amount.
ModelPlan PlanInsertVertex(Mesh& mesh, EdgeId edge, f32 t);

/// Divide (§3.14): every edge in @p edges cut into @p segments equal parts.
ModelPlan PlanDivide(Mesh& mesh, const ElementSet& edges, u32 segments = 2);

/// Connect's parameters (§3.14), re-planned when either moves (§5.4 step 3).
struct ConnectParams {
    u32 segments = 1; ///< How many new vertices each selected edge takes.
    f32 pinch = 0.0f; ///< Past 0: `NotBuiltYet` until the phase that spreads them.
};

/**
 * @brief Connect at the Vertex level (§3.14): each face holding two or more
 *        selected vertices split between them.
 *
 * With more than two in one face, each pair of consecutive selected corners in
 * loop order that do not already share an edge is joined — 3ds Max's rule,
 * which turns a hexagon's corners 0, 2 and 4 into an inner triangle. Refused
 * with `NotOneFace` when no face holds two, and with `DiagonalExists` when
 * every pair is already joined.
 *
 * Output selection: the new edges. Nothing moves, so there is no amount.
 */
ModelPlan PlanConnectVertices(Mesh& mesh, const PointTable& points, const ElementSet& vertices);

/**
 * @brief Connect at the Edge level (§3.14): `segments` vertices on every
 *        selected edge, joined across every face that holds exactly two.
 *
 * A face with three or more selected edges gets its edge vertices only, and
 * `kept` counts it. The amount is Slide, in [-1, 1] of each new vertex's free
 * length along its own edge; 0 is the centre, not a cancel (§3.1).
 *
 * Output selection: the new edges.
 */
ModelPlan PlanConnectEdges(Mesh& mesh, const PointTable& points, const ElementSet& edges,
                           const ConnectParams& params = {});

// ============================================================================
// Symmetry and moving well (§3.15, §3.17)
// ============================================================================

/// Symmetrize's options (§3.15): which plane, and which side is the source.
/// The axis is Skin's own `MirrorAxis` as a number: 0 X, 1 Y, 2 Z.
struct SymmetrizeParams {
    u32 axis = 1;
    bool fromPositive = true; ///< + to -, or the other way.
    /// The plane the two halves meet on, when it is not an axis plane through
    /// the model's origin: a point on it and its direction.
    ///
    /// A zero-length `normal` -- the default -- means `axis`'s plane through
    /// the origin, which is what a mirrored model means and what Skin's own
    /// mirror axis says. A model whose symmetry does not run through the origin
    /// names its own plane here, and `origin` is then read too.
    ///
    /// `axis` still chooses the bone map either way: a bone's mirror is named
    /// (`Left`/`Right` in the skeleton), not measured, so a free plane cannot
    /// tell which bone mirrors which and the nearest axis is what does.
    Vector3f origin{0.0f, 0.0f, 0.0f};
    Vector3f normal{0.0f, 0.0f, 0.0f};
};

/**
 * @brief Symmetrize (§3.15): @p mesh made symmetric from one side of a plane.
 *
 * The far side is sliced away, and the near one is copied reflected: the copy's
 * winding is reversed, since a reflection flips orientation, its normals are
 * reflected and each tangent's `w` is negated, because the copy is a mirrored
 * island. A vertex on the plane is not copied at all — the two halves share it,
 * which is what welds the seam exactly rather than to a tolerance.
 *
 * UVs are copied as they are, so the mirrored half samples the same texture,
 * which is how Warcraft III's mirrored textures work.
 *
 * With @p boneMirror (per bone, its mirror, or `kInvalidId`) the copy's skin is
 * mapped through it; a bone with no mirror keeps itself and `kept` counts the
 * vertices that happened to.
 *
 * Refused with `NoSourceSide` when the source side holds no face.
 */
ModelPlan PlanSymmetrize(Mesh& mesh, const PointTable& points, const SymmetrizeParams& params,
                         std::span<const u32> boneMirror = {});

/**
 * @brief Make Planar (§3.17): @p vertices moved onto the plane that fits them
 *        best, or onto the axis plane through their centre.
 *
 * With `axis` 0, 1 or 2 the plane is the one square to that axis through the
 * selection's centre; with 3 it is the least-squares fit of the points
 * themselves, which is what flattening a dented face wants.
 *
 * The amount takes each point from where it is (0) to the plane (1).
 */
ModelPlan PlanMakePlanar(Mesh& mesh, const PointTable& points, const ElementSet& selection, u32 axis);

/**
 * @brief The least-squares plane through @p points -- §3.17's *Best fit*.
 *
 * @p origin comes back as their centre and @p normal as the smallest
 * eigenvector of their scatter, found by the power method on its adjugate.
 * Both are always written, so a caller may use them without checking.
 *
 * @return false when the scatter has no smallest direction -- fewer than three
 *         points, all of them in one place, or all of them along one line. The
 *         normal is then `{0, 0, 1}`, which is a plane but not a fitted one, and
 *         a caller drawing the fit should say nothing rather than draw that.
 */
bool FitPlane(std::span<const Vector3f> points, Vector3f* origin, Vector3f* normal);

/**
 * @brief Make Planar onto the plane the caller names (§3.17): @p origin a point
 *        on it, @p normal its direction.
 *
 * What the axis form does once it has resolved its axis into a plane, and what
 * a plane a hand has placed calls instead. The amount takes each point from
 * where it is (0) to that plane (1), as it does there.
 *
 * Refused with `ZeroAmount` for a zero-length @p normal, which names no plane.
 */
ModelPlan PlanMakePlanar(Mesh& mesh, const PointTable& points, const ElementSet& selection,
                         const Vector3f& origin, const Vector3f& normal);

// ============================================================================
// Parts (§3.16)
// ============================================================================

/**
 * @brief Duplicate (§3.16): the selected faces copied inside their own mesh, as
 *        a shell of their own.
 *
 * Every vertex the copy needs is copied with it, with a fresh `mergeGroup`, so
 * nothing welds the two together. The copies sit exactly on the originals until
 * something moves them — the viewer's Shift+Move does it in the same entry.
 *
 * Output selection: the copies.
 */
ModelPlan PlanDuplicate(Mesh& mesh, const ElementSet& faces);

/**
 * @brief Detach to a new mesh (§3.16): @p faces lifted out of @p mesh into
 *        @p made, which the caller appends to the model.
 *
 * @p made comes back as @p mesh with every other face deleted, so it carries
 * the source's props, its vertices' skin and every layer they had; a vertex the
 * faces left behind also use is copied by that deletion. With @p keep the
 * source is left as it was — Detach as a clone.
 *
 * Refused for every face of the mesh (`AllFaces`: the Mesh level is where a
 * whole mesh is moved) and for none (`EmptySelection`).
 */
ModelPlan PlanDetach(Mesh& mesh, const ElementSet& faces, Mesh& made, bool keep);

/**
 * @brief Detach to element (§3.16): the selected faces split from the rest
 *        along their border, staying in the same mesh as their own shell.
 *
 * This is Flip's first step without the reversal: every border vertex of the
 * region is copied once per fan of selected faces, with a fresh `mergeGroup`.
 *
 * Refused for every face of the mesh (`AllFaces`) and for none
 * (`EmptySelection`).
 */
ModelPlan PlanDetachToElement(Mesh& mesh, const ElementSet& faces);

/**
 * @brief Create Polygon (§3.16): one new face on @p loop, which must be border
 *        vertices in their border order.
 *
 * Each edge of the new face is either new or an existing border edge used
 * against its direction, so it gains its second face wound consistently. Its
 * corners copy the face beside each vertex, as Cap's do, and its section is the
 * lowest-numbered of those.
 *
 * Refused with `WouldPinch` for an interior edge (a third face), with
 * `WouldRepeatVertex` for a repeated vertex, `WouldFold` for a fold,
 * `CapTooShort` under three vertices, and `WindingDisagrees` when a border edge
 * it uses runs the same way it does.
 */
ModelPlan PlanCreatePolygon(Mesh& mesh, const PointTable& points, const std::vector<u32>& loop);

/// Bridge's parameters (§3.16). `segments` is refused past 1 until the phase
/// that builds it; `twist` offsets which pair the loops start from.
struct BridgeParams {
    u32 segments = 1;
    i32 twist = 0;
};

/**
 * @brief Bridge (§3.16): two border loops of one mesh joined by a band of
 *        quads, one per edge pair.
 *
 * The loops are matched in opposite directions, as a consistent bridge needs,
 * and the starting pair is the one whose connecting edges are shortest in sum;
 * `twist` steps it round. Each quad copies its rim faces' corners at the two
 * loops, as Extrude's walls do.
 *
 * Refused with `LoopCountsDiffer` for loops of different lengths, `SameLoop`
 * for one loop twice, `EmptySelection` when either is not a border loop, and
 * `NotBuiltYet` past one segment.
 */
ModelPlan PlanBridge(Mesh& mesh, const PointTable& points, EdgeId first, EdgeId second,
                     const BridgeParams& params = {});

/**
 * @brief Bridge at the Polygon level (§3.16): two faces of one mesh replaced by
 *        a band of quads between their rims.
 *
 * §3.16's second form -- "two selected faces, which are deleted first, leaving
 * their loops" -- done in one rebuild rather than as a delete and then a
 * bridge, so it is one plan, one refusal and one journal entry. The loops are
 * the two faces' own, taken the way the borders their removal leaves would run,
 * matched in opposite directions and started from the pair whose connecting
 * edges are shortest in sum; `twist` steps it round, as it does there.
 *
 * Each quad copies its corners from the face it grew out of, at both ends: the
 * band wears the two faces' own values, which is what makes a bridge through a
 * textured surface keep the surface's look.
 *
 * Refused with `SameLoop` for one face twice or two faces that share a vertex,
 * `LoopCountsDiffer` for faces of different valence, `EmptySelection` for a
 * face that is not there, and `NotBuiltYet` past one segment. Two faces that
 * are the whole mesh are not refused: what is left is the band, which is a
 * mesh -- bridging the two ends of nothing is how a tube is made.
 */
ModelPlan PlanBridgeFaces(Mesh& mesh, const PointTable& points, FaceId first, FaceId second,
                          const BridgeParams& params = {});

/// Where one of Cut's clicks landed (§3.14): on a vertex, on a point along an
/// edge, or on a point inside a face. The viewer's pick says which.
struct CutPoint {
    u32 vertex = kInvalidId; ///< Set: the click took this vertex.
    u32 edge = kInvalidId;   ///< Set: a point along this edge, at `t` from its first end.
    u32 face = kInvalidId;   ///< Set: a point inside this face, at `at`.
    f32 t = 0.5f;
    Vector3f at{0.0f, 0.0f, 0.0f};
};

/**
 * @brief Cut (§3.14): the surface cut from @p from to @p to, under the
 *        screen-space line between them.
 *
 * @p plane is the plane through the eye and the two points — in an orthographic
 * view, the one holding the view direction. The cut starts in the first point's
 * face and crosses every edge that plane crosses on the way to the second,
 * making a vertex at each crossing and an edge between consecutive ones. A
 * point on an edge is `SplitEdge`'s vertex; a point on a vertex is that vertex.
 *
 * A point inside a face is the viewer's to hold: the kernel cannot keep an edge
 * that ends inside one, so such a point is only realised once the chain leaves
 * the face again (§3.14), and this takes the pair it leaves on.
 *
 * Refused with `NothingToSlice` when the two points are the same or the walk
 * reaches no crossing, and with `WouldFold` when a cut would repeat an edge.
 *
 * Output selection: the edges of the cut.
 */
ModelPlan PlanCut(Mesh& mesh, const PointTable& points, const CutPoint& from, const CutPoint& to,
                  const Vector3f& plane);

/// What Slice does with the cut it made (§3.14).
enum class SliceMode : u8 {
    Cut,    ///< Leave both sides joined along it.
    Split,  ///< Part them: the vertices along the cut are doubled.
    Remove, ///< Drop one side, through Delete's rebuild.
};

/// Slice's plane and mode (§3.14). The plane is the gizmo's widget, or
/// QuickSlice's two clicks.
struct SliceParams {
    Vector3f origin{0.0f, 0.0f, 0.0f};
    Vector3f normal{0.0f, 0.0f, 1.0f};
    SliceMode mode = SliceMode::Cut;
    bool keepPositive = true; ///< Remove: the side the normal points at stays.
};

/**
 * @brief Slice (§3.14): @p faces cut along a plane — every face when @p faces
 *        is empty.
 *
 * Every edge the plane crosses strictly gets a vertex at the crossing; a vertex
 * within the mesh's own tolerance of the plane is snapped onto it and used as
 * the crossing instead, so no sliver is made. Each crossed face is split
 * between its crossings, paired in order along the plane's line through it — a
 * concave face can cross four times. `Split` then parts the two sides and
 * `Remove` drops one.
 *
 * Refused with `NothingToSlice` when the plane crosses no selected face.
 *
 * Output selection: the edges along the cut.
 */
ModelPlan PlanSlice(Mesh& mesh, const PointTable& points, const ElementSet& faces,
                    const SliceParams& params);

/// Turn on a real edge (§3.14): rotated to the next corners round its two
/// faces, Blender's Rotate Edge; between two triangles that is `FlipEdge`.
/// Refused with `DiagonalExists` when the new edge is already there, and
/// `WouldFold` when either new face would fold.
ModelPlan PlanTurnEdge(Mesh& mesh, EdgeId edge);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

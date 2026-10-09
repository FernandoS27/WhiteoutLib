// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
#pragma once

/**
 * @file shading.h
 * @brief How a surface shades: its hard and soft edges, the fans they make, the
 *        weighting of a fan's normal, and the normals set by hand
 *        (EDIT_MODE_NORMALS_DESIGN.md §5).
 *
 * Two kinds of edge and two kinds of normal:
 *
 * - An edge is **hard** (`sharp`) or soft. A **fan** is the corners at one point
 *   that soft edges connect, and it holds one normal.
 * - A normal is **automatic**, the weighted mean of its fan's faces, or
 *   **custom** (`normalCustom`), set by a command and left alone by every
 *   recompute until it is reset.
 *
 * `RecomputeNormals` (ops.h) is the one-mesh rule and stays inside each
 * vertex's ring. A `Surface` is the same rule over several meshes, where a
 * border that lies on another border the other way round is one edge (§5.2):
 * a material border, or a line the weld kept apart because its skin differs.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "mesh.h"
#include "select.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace shading {

// ============================================================================
// The weighting (§5.1)
// ============================================================================

/// How a fan's faces are weighted into its normal. Stored as `shading`.
enum class Weighting : u16 {
    Even = 0,      ///< Every face alike: Warcraft III's Classic models.
    Angle = 1,     ///< By the corner's angle: independent of how a polygon is cut.
    Area = 2,      ///< By the face's area.
    AreaAngle = 3, ///< By both: the closest to Warcraft III's HD models.
};
inline constexpr u32 kWeightingCount = 4;

/// @p mesh's weighting: Even when it carries no `shading` layer, which is how
/// every mesh shaded before this file.
Weighting WeightingOf(const Mesh& mesh);

/// Whether the Normals workspace has written @p mesh (§5.7): it carries `shading`.
bool IsAdopted(const Mesh& mesh);

/// Stores @p weighting on @p mesh, which adopts it. No normal moves.
void SetWeighting(Mesh& mesh, Weighting weighting);

/// A fit worse than this in the median takes By angle instead: 10 degrees.
inline constexpr f32 kWeightingFallback = 0.174532925f;

struct WeightingFit {
    Weighting weighting = Weighting::Angle;
    /// Each weighting's median error against the stored normals, radians.
    f32 medians[kWeightingCount] = {0.0f, 0.0f, 0.0f, 0.0f};
    /// False when the mesh has no normals or none fit within the fallback;
    /// `weighting` is then By angle.
    bool fitted = false;
};

/// The weighting whose fans are closest in the median to the normals @p mesh
/// holds, by its own hard edges. Even wins a tie.
WeightingFit FitWeighting(const Mesh& mesh);

// ============================================================================
// The surface (§5.2)
// ============================================================================

/// A corner of one of a surface's meshes: the halfedge leaving its vertex.
struct Corner {
    u32 mesh = 0;
    u32 halfedge = kInvalidId;

    auto operator<=>(const Corner&) const = default;
};

/// An edge of one of a surface's meshes.
struct MeshEdge {
    u32 mesh = 0;
    u32 edge = kInvalidId;

    auto operator<=>(const MeshEdge&) const = default;
};

/**
 * @brief Several meshes as one shading surface: their partners and their fans.
 *
 * It holds the meshes by pointer and builds their connectivity. Partners are
 * found once, from the positions; fans are built on first use and again after
 * `touch()`, which a caller that changed a flag calls.
 */
class Surface {
public:
    explicit Surface(std::span<Mesh* const> meshes);
    /// Over meshes that are only read: for what a view shows of a surface
    /// nobody is editing. Each must have its connectivity built, and no
    /// command may be run on the result.
    static Surface ReadOnly(std::span<const Mesh* const> meshes);

    u32 meshCount() const {
        return static_cast<u32>(meshes_.size());
    }
    Mesh& mesh(u32 m) const {
        return *meshes_[m];
    }

    /// The border edge lying on @p edge the other way round, when there is
    /// exactly one: three sheets on one line are not a surface.
    std::optional<MeshEdge> partner(MeshEdge edge) const;
    /// Whether @p edge has a face on both sides, its partner's counting.
    bool twoSided(MeshEdge edge) const;
    /// Whether @p edge shades hard: its own flag, or its partner's.
    bool hard(MeshEdge edge) const;
    /// The two corners either side of @p edge at each of its ends: {this side,
    /// the other side} at its first end, then at its second. Empty on a border
    /// with no partner.
    std::vector<std::pair<Corner, Corner>> across(MeshEdge edge) const;

    /// Flags or positions changed: the fans are built again when next asked.
    void touch() {
        built_ = false;
    }
    u32 fanCount() const;
    /// The fan @p corner is in; `kInvalidId` for a halfedge with no face.
    u32 fanOf(Corner corner) const;
    std::span<const Corner> fan(u32 fan) const;

    /// A fan's automatic normal by its corners' meshes' weightings, whether or
    /// not any of them is custom.
    Vector3f fanNormal(u32 fan) const;
    /// @p mesh's unit face normals, as of the last build.
    std::span<const Vector3f> faceNormals(u32 mesh) const;
    /// Every corner at the point @p vertex of @p mesh is one of: its own, its
    /// seam twins', and those of the other meshes' vertices that lie on it.
    std::vector<Corner> cornersAtPointOf(u32 mesh, u32 vertex) const;

private:
    void build() const;

    std::vector<Mesh*> meshes_;
    std::vector<std::vector<u32>> pointOf_;       ///< Per mesh, per vertex: its point across the surface.
    std::vector<std::vector<MeshEdge>> partners_; ///< Per mesh, per edge; invalid when none.
    mutable bool built_ = false;
    mutable std::vector<std::vector<u32>> fanOf_; ///< Per mesh, per halfedge.
    mutable std::vector<u32> offsets_;
    mutable std::vector<Corner> corners_;
    mutable std::vector<std::vector<Vector3f>> faceNormals_;
};

// ============================================================================
// The first write (§5.7)
// ============================================================================

struct AdoptReport {
    u32 meshes = 0;        ///< Adopted here; one already adopted is left as it is.
    u32 groupEdges = 0;    ///< Hard from a `smoothGroup` change, the layer then dropped.
    u32 partnersHard = 0;  ///< Partner pairs marked hard: their sides differ in the file.
    u32 partnersSoft = 0;  ///< Partner pairs whose sides agree, so they shade as one.
};

/// Adopts every mesh of @p surface that is not: folds `smoothGroup` into
/// `sharp`, marks hard the partners whose two sides' normals break (the weld's
/// test, `kWeldSharpAngle`), and fits and stores the weighting. So what the
/// file showed is what the flags say; no normal moves. Each mesh must be
/// modelled (`PrepareForModelling`) already.
AdoptReport Adopt(Surface& surface);

// ============================================================================
// Smoothing (§7)
// ============================================================================

/// What a smoothing command did: the fans to recompute are the ones these
/// corners are in once the flags are set.
struct ShadingChange {
    u32 edges = 0; ///< Edges whose state changed; a partner pair counts once.
    std::vector<Corner> corners;

    bool empty() const {
        return edges == 0;
    }
};

/// What a selection names (§7's table).
enum class Level : u8 { Vertex, Edge, Polygon, Mesh };

/// Hard or Soft on what @p picked (one set per mesh of the surface) names at
/// @p level. Edges: those edges. Vertices: every edge at them. Polygons: hard,
/// every edge of them; soft, every edge between two of them. Mesh: every edge
/// of the meshes with a non-empty set, or of all when every set is empty.
/// A partner takes its twin's state with it.
ShadingChange Sharpen(Surface& surface, std::span<const ElementSet> picked, Level level, bool hard);

/// One smooth group of the picked polygons: every edge between two of them
/// goes soft and their border goes hard. A mesh level is its meshes' faces.
ShadingChange Group(Surface& surface, std::span<const ElementSet> picked, Level level);

struct AutoSmoothOptions {
    f32 angle = 1.396263402f;   ///< 80 degrees (§2.2).
    bool keepHard = false;      ///< Only adds: an edge already hard stays.
    bool hardAtUvSeams = false; ///< A `seam` edge goes hard whatever its angle.
};

/// Every edge the selection names: hard where its faces meet at more than the
/// angle, soft elsewhere. With every set empty, every edge of every mesh.
ShadingChange AutoSmooth(Surface& surface, std::span<const ElementSet> picked, Level level,
                         const AutoSmoothOptions& options);

/// Recomputes the fans @p corners are in, clears `normalCustom` on them, and
/// rebuilds their tangents: what follows a smoothing command, and nothing
/// outside those fans moves (§5.4). Returns the corners written.
u32 FinishShading(Surface& surface, std::span<const Corner> corners);

/// Every automatic normal of the surface recomputed, custom ones left, and the
/// tangents rebuilt. Returns the corners written.
u32 ReshadeAll(Surface& surface);

// ============================================================================
// Editing normals (§8)
// ============================================================================

/// The corners a selection names (§6.4): every corner at a vertex's point, the
/// corners both sides of an edge at its two ends, a polygon's own corners, or
/// every corner of a mesh. Sorted, with no repeats.
std::vector<Corner> CornersOf(const Surface& surface, std::span<const ElementSet> picked, Level level);

/// Writes @p values to @p corners and marks them custom. The tangents follow.
void SetNormals(Surface& surface, std::span<const Corner> corners, std::span<const Vector3f> values);

/// Marks @p corners custom as they stand.
u32 KeepNormals(Surface& surface, std::span<const Corner> corners);

/// Back to automatic: the fans @p corners are in are recomputed whole.
u32 ResetNormals(Surface& surface, std::span<const Corner> corners);

/// The picked polygons shade flat and the ones round them bend to meet: at
/// each of their points, the fan's normal becomes the mean of the picked
/// faces' normals there. The corners it wrote are custom.
u32 FavorFaces(Surface& surface, std::span<const ElementSet> picked);

/// A custom normal turns with its face (§5.3): for each of @p faces, the
/// rotation that carries its frame (its normal and first drawn edge) from
/// @p before to the positions it has now is applied to its custom corners,
/// and corners that shared one normal before share the mean after.
void TurnCustomNormals(Mesh& mesh, std::span<const FaceId> faces, std::span<const Vector3f> before);

// ============================================================================
// Smooth groups (§5.5)
// ============================================================================

struct SmoothGroups {
    /// Per mesh, per face: its group's number from 1, or 0 for a polygon alone
    /// in its group, which is drawn untinted.
    std::vector<std::vector<u32>> groupOf;
    u32 groups = 0;    ///< Groups of two polygons or more.
    u32 hardEdges = 0; ///< Two-sided edges that are hard; a partner pair once.
};

SmoothGroups GroupsOf(const Surface& surface);

/// The faces in the smooth groups of @p picked's polygons, per mesh.
std::vector<ElementSet> GroupFaces(const Surface& surface, std::span<const ElementSet> picked);

// ============================================================================
// Check (§10)
// ============================================================================

struct CheckOptions {
    f32 faintAngle = 0.261799388f; ///< 15 degrees: a hard edge this soft is a note.
    u32 uvSet = 0;                 ///< The set the tangents were built from.
    /// A face whose UV area is under this share of its mesh's median face's is
    /// left out of the mirrored test: its side is rounding.
    f32 smallUvArea = 1e-4f;
};

/// One row: the elements it found, per mesh of the surface.
struct CheckRow {
    std::vector<ElementSet> found;
    u32 count = 0; ///< In the row's own unit: corners, edges or polygons.
};

struct CheckReport {
    CheckRow broken;           ///< Corners: a normal of zero length, not unit length, or not a number.
    CheckRow seams;            ///< Edges: soft, and its two sides disagree at an end.
    CheckRow insideOut;        ///< Polygons wound against the rest of their shell.
    CheckRow tangentsBroken;   ///< Corners: zero length, or not across the normal.
    CheckRow tangentsMirrored; ///< Corners: `w` against the side the UVs give.
    CheckRow faintHard;        ///< Edges: hard, and its sides differ by `faintAngle` or less.
    CheckRow backwards;        ///< Corners: a normal more than 90 degrees from its own face.
    CheckRow custom;           ///< Corners marked custom.
};

/// Corner rows name their faces in `found`, as that is what a level can select.
CheckReport CheckNormals(const Surface& surface, const CheckOptions& options = {});

} // namespace shading

namespace detail {

/// Every face's unit normal (Newell); (0, 0, 1) for a deleted one.
std::vector<Vector3f> FaceNormals(const Mesh& mesh);

/// The corners around @p v in ring order, split into fans: a break is a hard
/// edge, a `smoothGroup` change, a border, or two face normals more than the
/// angle of @p cosThreshold apart. The one ring walk every recompute shares.
std::vector<std::vector<HalfedgeId>> RingFans(const Mesh& mesh, std::span<const Vector3f> faceNormals,
                                              VertexId v, f32 cosThreshold);

/// Whether two faces with these unit normals meet at more than the angle whose
/// cosine is @p cosAngle. Exactly at it reads soft, whichever way they round.
bool HardAtAngle(const Vector3f& a, const Vector3f& b, f32 cosAngle);

/// What corner @p h's face adds to its fan's sum under @p weighting.
Vector3f WeightedFaceNormal(const Mesh& mesh, std::span<const Vector3f> faceNormals, HalfedgeId h,
                            shading::Weighting weighting);

/// `RecomputeTangents` over @p corners alone, as the face-set form computes them.
void RecomputeTangentsAt(Mesh& mesh, std::span<const HalfedgeId> corners, u32 uvSet);

} // namespace detail

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

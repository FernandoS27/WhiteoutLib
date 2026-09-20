// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file generate.h
 * @brief Weights made rather than painted (EDIT_MODE_SKIN_DESIGN.md §8).
 *
 * What every generator here shares (§8.1):
 *
 * - the **scope** is a set of points, or every point when none is given;
 * - the **bone set** is the bones it may write. Empty means every unlocked
 *   `Bone`. A point's weight on a bone outside the set is kept as locked share,
 *   so "regenerate the arm without touching the chest" is a scope and three
 *   bones;
 * - the output is limited to four, pruned at 0.01 and normalised, through
 *   `ops.h` rather than by hand;
 * - a point no bone reaches goes Rigid to the nearest bone by segment distance
 *   and is counted, never left empty.
 *
 * Only Rigid per Island is here so far (§8.3). Envelopes, Heat and the voxel
 * geodesic attach to the same two structs.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../geometry/mesh.h"
#include "../nodes/tree.h"
#include "ops.h"
#include "points.h"
#include "setup.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/**
 * @brief Where each bone is, and what it carries (§8.2).
 *
 * A Warcraft III bone rotates about its pivot and carries the mesh between
 * itself and its children, so the segment of bone B runs from `joint(B)` to the
 * joint of each of its child bones — one segment per child.
 *
 * **A bone with no child bone gets a sphere at its joint**, of radius half the
 * distance to its parent's joint. That radius is not decoration. A parent's
 * segment *ends* at the child's joint, so with a bare point a leaf could never
 * be strictly nearer than its own parent anywhere: a hand bone would take no
 * weight at all, and the mechanical one-click would leave every extremity on
 * the body. The sphere gives the leaf the outer half of the last segment,
 * which is what §8.2 asks for and what an artist means by "the hand".
 *
 * A segment is therefore measured as a signed distance to its geometry —
 * `distance - radius`, negative inside — with the radius 0 for a real segment
 * and the sphere's for a leaf, so the two are one query and not two cases.
 *
 * The joint is `NodeTree::worldBind`'s translation rather than `Node::pivot`, so
 * a rig that binds with matrices rather than pivots answers the same question.
 */
struct BoneSegments {
    /// The bones, in the order the set was built: node indices.
    std::vector<u32> bones;
    /// Per segment: the bone it belongs to, its two ends in model space, and
    /// the radius of the volume around it (0 for all but a leaf's sphere).
    struct Segment {
        u32 bone = 0;
        Vector3f start{0, 0, 0};
        Vector3f end{0, 0, 0};
        f32 radius = 0.0f;
    };
    std::vector<Segment> segments;

    bool empty() const {
        return segments.empty();
    }
};

/// The segments of @p bones, or of every `Bone` node when @p bones is empty.
/// A locked bone is left out: a generator may not write one (§6.2).
BoneSegments BuildBoneSegments(const NodeTree& nodes, std::span<const u32> bones = {});

/// The distance from @p position to the nearest point of the segment.
f32 DistanceToSegment(const Vector3f& position, const Vector3f& start, const Vector3f& end);

/// The bone of @p segments nearest @p position — `distance - radius`, so a
/// point inside a leaf's sphere is nearer it than its parent's segment end.
/// `kInvalidNode` for an empty set. Ties go to the bone nearer the root, which
/// is the lower index in a tree whose parents precede their children.
u32 NearestBone(const BoneSegments& segments, const Vector3f& position);

/// How a generator was asked to run.
struct GenerateOptions {
    /// The bones it may write; empty is every unlocked `Bone` (§8.1).
    std::vector<u32> bones;
    /// Drop the "a whole island goes to one bone" rule: each point then goes to
    /// its own nearest segment (§8.3).
    bool splitIslands = false;
    /// The default envelope of each bone of the set, in `BoneSegments::bones`
    /// order, for the bones with none saved (§8.2). Empty measures them on the
    /// mesh being written -- right for a model of one mesh and wrong for one
    /// whose bones reach several, so a caller with more than one mesh measures
    /// over all of them once and passes the answer in.
    std::vector<Envelope> defaults;
};

/// What a generator did, beside `SkinResult`'s counts.
struct GenerateResult {
    SkinResult weights;
    u32 islands = 0;  ///< Islands it decided.
    u32 unreached = 0;///< Points no bone reached, which went to the nearest.
};

/**
 * @brief Rigid per Island (§8.3): the mechanical one-click.
 *
 * Each island in @p scope goes wholly to one bone — the bone whose segment most
 * of the island's points are nearest to, a vote with ties to the bone nearer the
 * root. `splitIslands` sends each point to its own nearest bone instead.
 *
 * @p scope is a set of point ids, or empty for every point of the mesh.
 */
GenerateResult RigidPerIsland(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                              std::span<const u32> scope, const GenerateOptions& options = {});

// ---- Envelopes (§8.2) --------------------------------------------------------

/// Where a position falls on a bone: how far it is from the nearest point of
/// the bone's nearest segment, and how far along that segment the point lies --
/// `0` at the bone's own joint, `1` at the child's. The `along` is what makes
/// the two radii a capsule that can be wider at one end than the other.
///
/// The sphere radius a leaf carries for `NearestBone` is NOT taken off here:
/// the envelope's own radii say how far its influence reaches, and subtracting
/// the sphere as well would count the same half-segment twice.
struct SegmentReach {
    f32 distance = 0.0f;
    f32 along = 0.0f;
};

/// @p position's reach on @p bone, over whichever of its segments is nearest.
/// A bone with no segment in @p segments is infinitely far away.
SegmentReach ReachOfBone(const BoneSegments& segments, u32 bone, const Vector3f& position);

/// The weight @p envelope gives at @p reach: 1 within the inner radius, 0 past
/// the outer one, and the falloff between, with both radii interpolated along
/// the segment (§8.2). Gaussian is `exp(-x² / 2σ²)` with `x` the distance past
/// the inner radius and `σ` a third of the gap, cut to 0 at the outer radius.
f32 EnvelopeWeight(const Envelope& envelope, const SegmentReach& reach);

/// §8.2's three presets.
/// @bind
enum class EnvelopePreset : u8 {
    Mechanical, ///< The Voronoi boundary, Hard: every point to its nearest bone.
    Organic,    ///< 0.6x / 1.6x the median, Smooth: joints that bend.
    Hybrid,     ///< 1.0x / 1.3x, Smooth: a rigid body with a short blend.
};

/// One mesh and its table, for a measurement that spans a model. An envelope
/// is ONE capsule per bone (§8.2), so a bone that reaches three meshes has to
/// be measured over all three: measured on one, a shoulder that happens to be
/// bare in the mesh the caller picked comes back with no radius at all.
struct MeshPoints {
    const Mesh* mesh = nullptr;
    const PointTable* points = nullptr;
};

/// The same measure over several meshes at once.
std::vector<Envelope> MeasureEnvelopes(std::span<const MeshPoints> meshes,
                                       const BoneSegments& segments,
                                       EnvelopePreset preset = EnvelopePreset::Organic);

/**
 * @brief Every bone of @p segments measured under @p preset, in its order
 *        (§8.2).
 *
 * The measure is the median distance from a bone's segments to the points that
 * are nearer to it than to any other -- its Voronoi cell -- taken separately
 * over the half of the cell nearer each end, so a capsule wide at the shoulder
 * and narrow at the elbow is what a shoulder measures. A half with no points of
 * its own borrows the other's.
 *
 * `Mechanical` measures the cell's outer EDGE rather than its middle, since its
 * envelope is meant to hold the whole cell and nothing past it.
 *
 * A bone whose cell is empty gets an envelope of zero radius, which reaches
 * nothing: the honest answer for a bone with no geometry near it, and one
 * §8.1's fallback then handles.
 *
 * All of them at once because the cell is one walk over the mesh: measuring
 * sixty bones one at a time would walk it sixty times.
 */
std::vector<Envelope> MeasureEnvelopes(const Mesh& mesh, const PointTable& points,
                                       const BoneSegments& segments,
                                       EnvelopePreset preset = EnvelopePreset::Organic);

/// One bone's, for a panel asking about the primary. `MeasureEnvelopes` when
/// there is more than one to ask about.
Envelope MeasureEnvelope(std::span<const MeshPoints> meshes, const BoneSegments& segments,
                         u32 bone, EnvelopePreset preset = EnvelopePreset::Organic);

/**
 * @brief Envelope weights (§8.2): each bone writes the points its capsule
 *        reaches, and the result is normalised over the bones that reached.
 *
 * A bone uses its saved envelope (`Node::skin.envelope`) when it has one and a
 * measured `Organic` one when it has not, so a model reopened next week
 * generates what it generated before.
 *
 * **A Hard envelope takes a point whole.** Where two Hard envelopes both reach
 * a point the nearer bone gets it and the other gets nothing -- that is what
 * makes the Mechanical preset the Voronoi assignment its row promises rather
 * than an even split down every overlap. A Smooth, Linear or Gaussian envelope
 * overlapping is a blend, which is what those are for.
 *
 * A point no envelope reaches goes rigid to the nearest bone and is counted
 * (§8.1), never left empty.
 */
GenerateResult EnvelopeWeights(Mesh& mesh, const NodeTree& nodes, const PointTable& points,
                               std::span<const u32> scope, const GenerateOptions& options = {});

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

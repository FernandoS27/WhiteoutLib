// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file seams.h
 * @brief Where a UV surface is cut (EDIT_MODE_UV_DESIGN.md §3, §5).
 *
 * Two kinds of line meet here and they are not the same thing:
 *
 *  - `seam`, the mesh's own **delimit**: an edge whose two faces disagree in
 *    something a corner holds. It is derived, never authored, and the modelling
 *    walkers stop at it. `MarkDelimitSeams` is the one writer of it, so a UV
 *    commit and the prepare's weld both mark it by the same rule.
 *  - `uvSeamN`, a **mark**: what a modeller said should be cut in set N,
 *    whether or not the corners round it differ yet. A mark is the input to the
 *    islands (`CutsOf`); the seam is the output of the geometry.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../../model.h"
#include "../../skinning/points.h"
#include "../ids.h"
#include "../mesh.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
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

/// Sets or clears `uvSeamN` on @p edges. The one writer of a mark, so the
/// layer's creation and the set's numbering live in one place.
void ApplyMarks(Mesh& mesh, u32 set, std::span<const EdgeId> edges, bool mark);

// ============================================================================
// Where a cut wants to go (EDIT_MODE_UV_DESIGN.md §5)
// ============================================================================

struct SeamPathOptions {
    /// The direction a cut should hide from: towards the camera for the tool,
    /// the model's front for an automatic part. A path prefers edges facing
    /// away from it.
    Vector3f visible{0.0f, 0.0f, 1.0f};
    /// How much dearer a fully visible edge is than a fully hidden one.
    f32 hiddenWeight = 4.0f;
};

struct SeamPath {
    std::vector<EdgeId> edges;
    /// The far end could not be reached without leaving the surface: a border
    /// or another shell stood in the way, and the path is empty.
    bool stopped = false;
};

/// The cheapest run of edges from @p from to @p to, hugging the hidden side.
///
/// Dijkstra over the welded points, so a path crosses a seam the file left in
/// the geometry as though it were not there -- a modeller picking two edges of
/// what looks like one surface means one surface.
SeamPath FindSeamPath(const Mesh& mesh, const skinning::PointTable& points, EdgeId from,
                      EdgeId to, const SeamPathOptions& options = {});

struct SkeletonSeams {
    u32 parts = 0;
    u32 cuts = 0;
    /// The share of the cut length that faces away from `visible`.
    f32 hiddenShare = 0.0f;

    enum class Refusal : u8 {
        None = 0,
        NoLimbs ///< The record names no limb, so there is no shape to read.
    };
    Refusal refusal = Refusal::None;

    bool ok() const {
        return refusal == Refusal::None;
    }
};

/// Marks the seams the shape of the rig asks for (EDIT_MODE_UV_DESIGN.md §5.3).
///
/// Every face is given the part its strongest bone belongs to, folded through
/// the record so a twist bone is its forearm and a kneecap is its knee; the
/// edges between two parts are cut, and each part that is a closed tube is cut
/// once more along its hidden side so it can be laid flat. Adds to `uvSeamN`
/// and never clears: what a modeller has already said stands.
SkeletonSeams SeamsFromSkeleton(const Model& model, u32 mesh, Mesh& view, u32 set,
                                std::span<const u32> pointMirror);

/// Marks the twin of every marked edge, so a cut made on one side of a figure
/// is made on the other. Returns how many it added.
u32 MirrorMarks(Mesh& view, u32 set, std::span<const u32> pointMirror);

} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

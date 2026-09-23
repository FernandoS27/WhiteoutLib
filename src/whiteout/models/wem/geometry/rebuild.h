// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file rebuild.h
 * @brief Internal: the machinery the rebuilding ops share (ops.h), and the
 *        modelling tools after them (EDIT_MODE_MODELLING_DESIGN.md §2.7).
 *
 * A rebuild goes out through a face set and comes back through a fresh
 * connectivity build. Every handle changes; attribute data is carried across by
 * correspondence.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/geometry/mesh.h>
#include <whiteout/models/wem/geometry/repair.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace detail {

/// For each corner in `faceSet()` order, the halfedge it sits on. Corner order is
/// face-major from `halfedge(f)`, which is the order `Topology::toFaceSet()`
/// writes, so the two index alike. Empty without connectivity.
std::vector<u32> snapshotCorners(const Mesh& mesh);

/// The same for a const mesh with or without connectivity: its own topology's,
/// or a local deterministic build's, which numbers halfedges exactly as the
/// mesh's Halfedge layers are indexed when it has none.
std::vector<u32> snapshotCornersBuilt(const Mesh& mesh);

struct RebuildMapping {
    FaceSet faces;
    std::vector<u32> vertexSource; ///< new vertex -> old vertex, or kInvalidId.
    std::vector<u32> faceSource;   ///< new face   -> old face slot.
    std::vector<u32> cornerSource; ///< new corner -> old corner (into the snapshot), or kInvalidId.
    /// A tool whose amount starts at 0 (extrude, inset, §3.8) builds faces that
    /// have no area until `ApplyAmount` moves them, so the repair's zero-area
    /// rule must not drop them. Its combinatorial rules still apply.
    bool mayHaveZeroArea = false;
};

struct RebuildResult {
    bool ok = false;
    /// What the §5.3 repair did to the target face set; `changed` is false and
    /// the log empty when it was already a manifold.
    RepairResult repair;
    /// New edges whose two sides came from two different old edges: the edges
    /// a weld closed.
    std::vector<u32> closedEdges;
};

/**
 * @brief Replaces @p mesh's geometry with @p mapping, carrying attribute *data*
 *        across by correspondence even though every handle changes.
 *
 * Vertex and Face layers go by `vertexSource` and `faceSource`, Halfedge layers
 * by `cornerSource`. An Edge layer goes by halfedge correspondence: a new edge's
 * side is the old edge between its two corners' sources, whichever way round the
 * old face ran. Where a weld closed an edge, its two old sides combine by type:
 * Bool ORs, F32 takes the maximum, anything else the lower-numbered side's.
 */
RebuildResult rebuild(Mesh& mesh, RebuildMapping mapping,
                      const std::vector<u32>& oldCornerHalfedge);

/// The mesh's own current face set, identity everywhere: for an op that only
/// rewrote corner *vertices* (weld, unify, vertex split).
RebuildMapping identityMapping(const Mesh& mesh);

/// What `DissolveEdgeDeferred` did.
struct Dissolved {
    bool ok = false;
    FaceId kept;              ///< The merged face.
    FaceId gone;              ///< The face that was deleted.
    std::vector<u32> joined;  ///< The union row `DissolveEdge` would store on `kept`.
};

/// `DissolveEdge` without its row writes: the union goes to the result, for a
/// caller that builds every row in one pass (the joins, EDIT_MODE_MODELLING_DESIGN.md
/// §3.2). The caller owns `kept`'s row and `gone`'s.
Dissolved DissolveEdgeDeferred(Mesh& mesh, EdgeId edge, FaceId survivor);

} // namespace detail
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

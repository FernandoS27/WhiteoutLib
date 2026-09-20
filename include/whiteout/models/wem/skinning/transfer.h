// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file transfer.h
 * @brief Weights taken from other meshes (EDIT_MODE_SKIN_DESIGN.md §7.6).
 *
 * A point is given the influences of the closest point on the source meshes'
 * triangles, blended by that point's barycentric coordinates, and the result
 * goes through `Assign` so it is normalised and limited by the same rule every
 * other command ends with.
 *
 * The query is at **rest**. Transfer is a question about the model -- which
 * bone carries this piece of surface -- and the answer must not depend on which
 * frame of which animation the viewport happens to be showing.
 *
 * **To LODs** is the case that makes it worth a button: 2,430 corpus files ship
 * LODs, and skinning LOD 0 and then LOD 1 to 3 by hand is the same work four
 * times. That command is the host's, since it is the one that knows which
 * meshes share a material; this is the one mesh-to-meshes step it runs per LOD
 * mesh.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../geometry/mesh.h"
#include "../nodes/tree.h"
#include "ops.h"
#include "points.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/// How a transfer was asked to run.
struct TransferOptions {
    /// A point whose nearest source surface is further than this keeps what it
    /// has and is counted. `0` means no limit, which is the default: a point
    /// with no source in reach is a hole in the LOD, and the user wants to see
    /// the count rather than a silent partial result.
    f32 maxDistance = 0.0f;
    /// The bones it may write; empty is every one (§8.1). A bone outside the
    /// set is held, so its weight survives the transfer.
    std::vector<u32> bones;
};

/// What a transfer did.
struct TransferResult {
    SkinResult weights;
    u32 transferred = 0; ///< Points that found a source surface.
    u32 unreached = 0;   ///< Points that did not, and were left alone.
};

/**
 * @brief Give each point of @p scope the weights of the nearest surface of
 *        @p sources (§7.6).
 *
 * @p scope is a set of point ids, or empty for every point of @p target. A
 * source that is @p target itself is skipped: a mesh cannot be its own source,
 * and the nearest point would be the point.
 */
TransferResult Transfer(Mesh& target, const NodeTree& nodes, const PointTable& points,
                        std::span<const u32> scope, std::span<const Mesh* const> sources,
                        const TransferOptions& options = {});

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

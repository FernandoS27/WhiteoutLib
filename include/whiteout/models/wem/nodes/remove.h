// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file remove.h
 * @brief Node removal and compaction (WEM v3, design §10.6).
 *
 * "Easy at runtime" means two things concretely: no eager reindex of every
 * referencer on each removal, and no dangling indices either. Same two-phase
 * contract as the topology (§5.2), so a caller learns one lifecycle, not two —
 * `RemoveNode` marks and reparents, `CompactNodes` collects and remaps.
 *
 * A node the skin references is the one hard case, and it is a policy, not an
 * accident. **Deleting a bone out from under a skin silently is not on the
 * menu.**
 *
 * ### The referencer table (§10.6) — normative
 *
 * | Referencer | Field |
 * |---|---|
 * | `SkinBinding` | `Influence::bone` |
 * | `MeshSection` | `rigidNode` |
 * | `AnimChannel` | `target.node` |
 * | `ClipEvent` | `node` |
 *
 * P6 settled that there is no `Actor` row: an attach point's child model rides
 * the node's own payload (§10.2), so removing the node takes it along and there
 * is nothing left to invalidate.
 *
 * A new structure that stores a node index must add itself to that table, and
 * `Validate` cross-checks every listed field against the tree. The table's single
 * registration point is `NodeReferencers` below: each later phase adds one row,
 * not one mechanism.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "../anim/clip.h"
#include "../diagnostics.h"
#include "../geometry/mesh.h"
#include "tree.h"

namespace whiteout {
namespace models {
namespace wem {

enum class RemovePolicy : u8 {
    ReparentChildren, ///< Children go to the removed node's parent.
    RemoveSubtree,    ///< Children and their subtrees go with it.
};

enum class SkinPolicy : u8 {
    Refuse,           ///< Fail the call, naming the affected meshes.
    ReassignToParent, ///< Move the influences up one level, merging per vertex.
};

struct RemoveResult {
    bool removed = false;
    std::vector<u32> removedNodes; ///< Every node the call marked, in index order.
    Diagnostics diagnostics;
};

/**
 * @brief The meshes a removal has to consider.
 *
 * Passed explicitly rather than reached through a `Document`, because P1 has no
 * `Document` yet and because a caller that knows only one mesh is affected should
 * not have to hand over the whole document to say so.
 */
struct NodeReferencers {
    std::span<Mesh> meshes;

    /// This model's channel table (§10.8). A channel whose node dies is
    /// **invalidated, not dropped** — an id is never reused, and dropping the
    /// declaration would leave its sub-tracks joining on nothing with no way to
    /// tell that from a merge that has not landed yet.
    AnimChannelTable* channels = nullptr;

    /// The clips driving this model — `ClipEvent::node` is the referencer. The
    /// caller filters by `Clip::model`, because a `NodeTree` does not know its
    /// own index and a clip for another model must not be touched.
    std::span<Clip> clips;
};

/**
 * @brief Marks the node(s) deleted, reparents per @p policy and — by default —
 *        recomposes each surviving child's `local` so its **world** bind pose is
 *        unchanged.
 *
 * Model-space pose values need no fix, which is one more reason D3's A/B poses
 * are stored as shipped.
 *
 * Removal is *marking*: the node stays in `nodes` with a `Removed` marker until
 * `CompactNodes` runs, so every index a caller holds stays valid across the call.
 */
RemoveResult RemoveNode(NodeTree& tree, u32 node, RemovePolicy policy,
                        SkinPolicy skinPolicy = SkinPolicy::Refuse, bool preserveWorld = true,
                        NodeReferencers referencers = {});

/// True when @p node has been removed but not yet compacted away.
bool IsNodeRemoved(const NodeTree& tree, u32 node);

struct NodeRemaps {
    std::vector<u32> nodes; ///< `remap[old]` = new index, or `kInvalidNode`.
    u32 newCount = 0;
};

/**
 * @brief Compacts the tree and applies the remap to every referencer in the
 *        §10.6 table.
 *
 * A referencer pointing at a removed node is reported (`DanglingNodeReference`)
 * and left invalid rather than silently repointed, because "which bone did you
 * mean" is not a question this layer can answer.
 */
NodeRemaps CompactNodes(NodeTree& tree, NodeReferencers referencers, Diagnostics& out);

/// Cross-checks every §10.6 referencer against the tree without changing
/// anything — the `Validate` half of the table.
void CheckNodeReferencers(const NodeTree& tree, std::span<const Mesh> meshes, Diagnostics& out,
                          const AnimChannelTable* channels = nullptr,
                          std::span<const Clip> clips = {});

} // namespace wem
} // namespace models
} // namespace whiteout

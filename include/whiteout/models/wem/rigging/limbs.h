// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file limbs.h
 * @brief A limb read off the rig record (EDIT_MODE_AUTO_IK_DESIGN.md §3.3), and
 *        the chain above a tip.
 *
 * The record says what a node is; this says which nodes make one limb, so a
 * solve can take the whole of it. It reads `NodeTree` and `Node::rig` and
 * nothing else — no session, no pose — which is why it sits beside the record
 * rather than in the editor (EDIT_MODE_TPOSE_PLAN.md T0).
 */

#include <optional>
#include <vector>

#include <whiteout/common_types.h>

#include "../nodes/tree.h"
#include "record.h"

namespace whiteout {
namespace models {
namespace wem {

// ---- The tree, as the rig reads it ------------------------------------------------

/// @p node's parent, or `kInvalidNode` when it has none or parents itself.
u32 ParentOf(const NodeTree& tree, u32 node);

/// What the record calls @p node.
RigRole RoleOf(const NodeTree& tree, u32 node);

/// A node a rig turns: a bone or a helper.
bool IsJoint(const NodeTree& tree, u32 node);

/// A joint with no parent.
bool IsRoot(const NodeTree& tree, u32 node);

// ---- A limb -----------------------------------------------------------------------

/// A limb as the record names it (§3.3): Upper, Lower, the Hocks of a
/// three-joint leg, and the End, with the Twist bones between them riding.
struct Limb {
    u32 upper = kInvalidNode;
    u32 lower = kInvalidNode;
    /// The Hock nearest the Lower; every joint from it to the End rides it.
    u32 hock = kInvalidNode;
    u32 end = kInvalidNode;
    /// What lies between and rides: the Twist bones, and any Hock after the
    /// first. Never keyed.
    std::vector<u32> riders;
};

/// @p node's limb when it is one of a limb's joints (Upper, Lower, Hock or
/// End) and the record names the whole of it, the Twist bones skipped.
std::optional<Limb> LimbOf(const NodeTree& tree, u32 node);

/// The joints a chain of @p length above @p tip turns, top first: bones and
/// helpers, Twist bones skipped, and never a Body or a root — those move the
/// figure, they do not reach.
std::vector<u32> ChainAbove(const NodeTree& tree, u32 tip, u32 length);

} // namespace wem
} // namespace models
} // namespace whiteout

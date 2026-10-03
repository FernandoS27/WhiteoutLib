// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file presence.h
 * @brief What a node and a section are in one profile (§6): the profile masks
 *        read as a rule, so every writer and every host read them alike.
 *
 * A section a profile does not draw is not written for it. A node a profile
 * does not hold is not written either, unless a node under it is held: the
 * children still hang off its transform, so it stays as that transform alone
 * — a helper. The rule is one function so the `.mdx` export, its export map
 * and an editor's view of the same profile cannot disagree about it.
 */

#include <vector>

#include <whiteout/common_types.h>

#include "../document.h"
#include "../geometry/mesh.h"
#include "../profile.h"
#include "tree.h"

namespace whiteout {
namespace models {
namespace wem {

enum class NodePresence : u8 {
    Present,   ///< In the profile's mask: written as itself.
    Placement, ///< Outside it, with a node under it inside: written as a helper.
    Absent,    ///< Outside it, and nothing under it is in: not written.
};

/// Per node of @p tree, its presence in @p profile. A removed node is absent.
std::vector<NodePresence> NodePresenceIn(const NodeTree& tree, ProfileId profile);

/// The kind @p node is written as under @p presence: its own, a helper for a
/// placement. Meaningless for an absent node.
inline NodeKind PresentKind(const Node& node, NodePresence presence) {
    return presence == NodePresence::Placement ? NodeKind::Helper : node.kind;
}

/// Whether @p profile draws @p section.
inline bool SectionDrawnIn(const MeshSection& section, ProfileId profile) {
    return HasProfile(section.profiles, profile);
}

/// Sets @p to's bit on every node and section of @p document to @p from's:
/// what a profile derived from another holds and draws.
void CopyProfileMasks(Document& document, ProfileId from, ProfileId to);

} // namespace wem
} // namespace models
} // namespace whiteout

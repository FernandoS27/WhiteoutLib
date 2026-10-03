// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/nodes/presence.h>

namespace whiteout {
namespace models {
namespace wem {

std::vector<NodePresence> NodePresenceIn(const NodeTree& tree, ProfileId profile) {
    const u32 count = tree.size();
    std::vector<NodePresence> out(count, NodePresence::Absent);
    for (u32 n = 0; n < count; ++n) {
        const Node& node = tree.nodes[n];
        if (node.removed || !HasProfile(node.profiles, profile)) {
            continue;
        }
        // Over a Placement an earlier child's walk left, when the tree lists
        // this node after that child.
        out[n] = NodePresence::Present;
        // Up the chain to the first ancestor already standing; the walk is
        // bounded by the tree's size, so a cycle cannot hold it.
        u32 parent = node.parent;
        for (u32 steps = 0; parent < count && steps < count; ++steps) {
            if (out[parent] != NodePresence::Absent || tree.nodes[parent].removed) {
                break;
            }
            out[parent] = NodePresence::Placement;
            parent = tree.nodes[parent].parent;
        }
    }
    return out;
}

void CopyProfileMasks(Document& document, ProfileId from, ProfileId to) {
    if (from == to) {
        return;
    }
    const ProfileMask fromBit = ProfileBit(from);
    const ProfileMask toBit = ProfileBit(to);
    const auto copy = [&](ProfileMask& mask) {
        mask = (mask & fromBit) != 0 ? (mask | toBit) : (mask & ~toBit);
    };
    for (Model& model : document.models) {
        for (Node& node : model.nodes.nodes) {
            copy(node.profiles);
        }
        for (Mesh& mesh : model.meshes) {
            for (MeshSection& section : mesh.sections) {
                copy(section.profiles);
            }
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout

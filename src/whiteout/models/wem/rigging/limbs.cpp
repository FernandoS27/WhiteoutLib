// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/rigging/limbs.h"

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr u32 kNone = kInvalidNode;

bool Contains(const std::vector<u32>& list, u32 value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

u32 ParentOf(const NodeTree& tree, u32 node) {
    if (node >= tree.size())
        return kNone;
    const u32 parent = tree.nodes[node].parent;
    return parent < tree.size() && parent != node ? parent : kNone;
}

RigRole RoleOf(const NodeTree& tree, u32 node) {
    return node < tree.size() ? tree.nodes[node].rig.role : RigRole::None;
}

bool IsJoint(const NodeTree& tree, u32 node) {
    return node < tree.size() && (tree.nodes[node].kind == NodeKind::Bone ||
                                  tree.nodes[node].kind == NodeKind::Helper);
}

bool IsRoot(const NodeTree& tree, u32 node) {
    return IsJoint(tree, node) && ParentOf(tree, node) == kNone;
}

std::optional<Limb> LimbOf(const NodeTree& tree, u32 node) {
    if (!IsJoint(tree, node))
        return std::nullopt;
    const RigRole role = RoleOf(tree, node);
    u32 end = kNone;
    if (role == RigRole::End) {
        end = node;
    } else if (role == RigRole::Upper || role == RigRole::Lower || role == RigRole::Hock) {
        // Down to the End through nothing but the limb's own joints.
        for (const u32 below : tree.subtree(node)) {
            if (below == node || RoleOf(tree, below) != RigRole::End)
                continue;
            bool through = true;
            for (u32 at = ParentOf(tree, below); at != node && at != kNone && through;
                 at = ParentOf(tree, at)) {
                const RigRole r = RoleOf(tree, at);
                through = r == RigRole::Lower || r == RigRole::Hock || r == RigRole::Twist;
            }
            if (through) {
                end = below;
                break;
            }
        }
    }
    if (end == kNone)
        return std::nullopt;

    // Up from the End: Twist bones and Hocks ride, then the Lower and the Upper.
    Limb limb;
    limb.end = end;
    std::vector<u32> hocks;
    u32 at = ParentOf(tree, end);
    while (at != kNone && (RoleOf(tree, at) == RigRole::Twist || RoleOf(tree, at) == RigRole::Hock)) {
        if (RoleOf(tree, at) == RigRole::Hock)
            hocks.push_back(at);
        else
            limb.riders.push_back(at);
        at = ParentOf(tree, at);
    }
    if (RoleOf(tree, at) != RigRole::Lower)
        return std::nullopt;
    limb.lower = at;
    at = ParentOf(tree, at);
    while (at != kNone && RoleOf(tree, at) == RigRole::Twist) {
        limb.riders.push_back(at);
        at = ParentOf(tree, at);
    }
    if (RoleOf(tree, at) != RigRole::Upper)
        return std::nullopt;
    limb.upper = at;
    if (!hocks.empty()) {
        limb.hock = hocks.back(); // the one nearest the Lower
        hocks.pop_back();
        limb.riders.insert(limb.riders.end(), hocks.begin(), hocks.end());
    }
    if (node != limb.upper && node != limb.lower && node != limb.end && node != limb.hock &&
        !Contains(limb.riders, node))
        return std::nullopt;
    return limb;
}

std::vector<u32> ChainAbove(const NodeTree& tree, u32 tip, u32 length) {
    std::vector<u32> chain;
    for (u32 at = ParentOf(tree, tip); at != kNone && chain.size() < length; at = ParentOf(tree, at)) {
        // A root turned would swing everything under it: that is the Body's
        // move, or the stage's, never a reach.
        if (!IsJoint(tree, at) || RoleOf(tree, at) == RigRole::Body || IsRoot(tree, at))
            break;
        if (RoleOf(tree, at) == RigRole::Twist)
            continue;
        chain.push_back(at);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/nodes/tree.h>

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {

u32 NodeTree::add(Node node) {
    const u32 index = static_cast<u32>(nodes.size());
    nodes.push_back(std::move(node));
    invalidateHierarchy();
    return index;
}

void NodeTree::clear() {
    nodes.clear();
    poseSchema.clear();
    authoritativePose = 0;
    invalidateHierarchy();
}

bool NodeTree::parentsPrecedeChildren() const {
    for (u32 i = 0; i < nodes.size(); ++i) {
        const u32 parent = nodes[i].parent;
        if (parent != kInvalidNode && parent >= i) {
            return false;
        }
    }
    return true;
}

std::vector<u32> NodeTree::sortParentsFirst() {
    const u32 count = size();
    std::vector<u32> order;
    order.reserve(count);
    std::vector<u8> placed(count, 0);

    // Repeated sweeps in index order: a node is emitted once its parent has been.
    // Index order inside each sweep keeps the result deterministic, and the sweep
    // count is bounded by the depth, which is tiny for a skeleton.
    bool progress = true;
    while (order.size() < count && progress) {
        progress = false;
        for (u32 i = 0; i < count; ++i) {
            if (placed[i] != 0) {
                continue;
            }
            const u32 parent = nodes[i].parent;
            if (parent != kInvalidNode && (parent >= count || placed[parent] == 0)) {
                continue;
            }
            order.push_back(i);
            placed[i] = 1;
            progress = true;
        }
    }
    if (order.size() != count) {
        return {}; // A cycle. Leave the tree alone rather than half-sort it.
    }

    std::vector<u32> remap(count, kInvalidNode);
    for (u32 fresh = 0; fresh < count; ++fresh) {
        remap[order[fresh]] = fresh;
    }

    std::vector<Node> reordered;
    reordered.reserve(count);
    for (u32 fresh = 0; fresh < count; ++fresh) {
        Node moved = std::move(nodes[order[fresh]]);
        if (moved.parent != kInvalidNode && moved.parent < count) {
            moved.parent = remap[moved.parent];
        }
        reordered.push_back(std::move(moved));
    }
    nodes = std::move(reordered);
    invalidateHierarchy();
    return remap;
}

// ============================================================================
// The hierarchy view
// ============================================================================

void NodeTree::invalidateHierarchy() const {
    hierarchyValid_ = false;
    byKind_.clear();
}

void NodeTree::ensureHierarchy() const {
    if (!hierarchyValid_) {
        buildHierarchy();
    }
}

void NodeTree::buildHierarchy() const {
    const u32 count = size();
    childOffset_.assign(count + 1, 0);
    childCount_.assign(count, 0);
    childrenFlat_.clear();
    roots_.clear();
    preorder_.clear();
    preorderIndex_.assign(count, kInvalidNode);
    subtreeSize_.assign(count, 0);

    for (u32 i = 0; i < count; ++i) {
        const u32 parent = nodes[i].parent;
        if (parent == kInvalidNode || parent >= count) {
            roots_.push_back(i);
        } else {
            ++childCount_[parent];
        }
    }

    u32 running = 0;
    for (u32 i = 0; i < count; ++i) {
        childOffset_[i] = running;
        running += childCount_[i];
    }
    childOffset_[count] = running;

    childrenFlat_.assign(running, kInvalidNode);
    std::vector<u32> cursor(childOffset_.begin(), childOffset_.begin() + count);
    // Walking `nodes` in index order puts each parent's children in index order.
    for (u32 i = 0; i < count; ++i) {
        const u32 parent = nodes[i].parent;
        if (parent != kInvalidNode && parent < count) {
            childrenFlat_[cursor[parent]++] = i;
        }
    }

    // Depth-first pre-order, roots in index order. Iterative, because a deep
    // chain — D3 skeletons run to dozens of levels — should not recurse.
    preorder_.reserve(count);
    std::vector<std::pair<u32, u32>> stack; // (node, next child slot)
    for (u32 root : roots_) {
        stack.clear();
        preorderIndex_[root] = static_cast<u32>(preorder_.size());
        preorder_.push_back(root);
        stack.emplace_back(root, childOffset_[root]);
        while (!stack.empty()) {
            auto& top = stack.back();
            const u32 end = childOffset_[top.first] + childCount_[top.first];
            if (top.second >= end) {
                subtreeSize_[top.first] =
                    static_cast<u32>(preorder_.size()) - preorderIndex_[top.first];
                stack.pop_back();
                continue;
            }
            const u32 child = childrenFlat_[top.second++];
            preorderIndex_[child] = static_cast<u32>(preorder_.size());
            preorder_.push_back(child);
            stack.emplace_back(child, childOffset_[child]);
        }
    }

    // A node in a parent cycle never enters the pre-order; give it a subtree of
    // itself alone so `subtree()` stays total rather than returning garbage.
    for (u32 i = 0; i < count; ++i) {
        if (preorderIndex_[i] == kInvalidNode) {
            subtreeSize_[i] = 0;
        }
    }

    hierarchyValid_ = true;
}

NodeRange NodeTree::roots() const {
    ensureHierarchy();
    return NodeRange(roots_.data(), roots_.size());
}

NodeRange NodeTree::children(u32 node) const {
    ensureHierarchy();
    if (node >= size()) {
        return NodeRange();
    }
    return NodeRange(childrenFlat_.data() + childOffset_[node], childCount_[node]);
}

NodeRange NodeTree::subtree(u32 node) const {
    ensureHierarchy();
    if (node >= size() || preorderIndex_[node] == kInvalidNode) {
        return NodeRange();
    }
    return NodeRange(preorder_.data() + preorderIndex_[node], subtreeSize_[node]);
}

NodeRange NodeTree::ofKind(NodeKind kind) const {
    const auto slot = static_cast<std::size_t>(kind);
    if (byKind_.size() != static_cast<std::size_t>(NodeKind::Count)) {
        byKind_.assign(static_cast<std::size_t>(NodeKind::Count), {});
        for (u32 i = 0; i < size(); ++i) {
            const auto index = static_cast<std::size_t>(nodes[i].kind);
            if (index < byKind_.size()) {
                byKind_[index].push_back(i);
            }
        }
    }
    if (slot >= byKind_.size()) {
        return NodeRange();
    }
    return NodeRange(byKind_[slot].data(), byKind_[slot].size());
}

u32 NodeTree::depth(u32 node) const {
    u32 levels = 0;
    u32 walk = node;
    while (walk < size() && nodes[walk].parent != kInvalidNode) {
        const u32 parent = nodes[walk].parent;
        if (parent >= size() || parent == walk) {
            break;
        }
        walk = parent;
        ++levels;
        if (levels > size()) {
            break; // A cycle; report what we have rather than spin.
        }
    }
    return levels;
}

// ============================================================================
// Poses
// ============================================================================

Transform NodeTree::worldBind(u32 node) const {
    if (node >= size()) {
        return Transform::identity();
    }

    // Walk up first, then compose down: parents precede children in storage but a
    // caller may ask about any node, and composing on the way down is what makes
    // the inherit flags apply in the right order.
    std::vector<u32> chain;
    u32 walk = node;
    while (walk != kInvalidNode && walk < size()) {
        chain.push_back(walk);
        if (hasFlag(nodes[walk].flags, NodeFlags::ModelSpace)) {
            break; // Its local *is* its world; the walk stops here.
        }
        const u32 parent = nodes[walk].parent;
        if (parent == walk || chain.size() > size()) {
            break;
        }
        walk = parent;
    }

    Transform world = Transform::identity();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Node& current = nodes[*it];
        Transform effective = world;
        if (hasFlag(current.flags, NodeFlags::DontInheritTranslation)) {
            effective.translation = Vector3f{0, 0, 0};
        }
        if (hasFlag(current.flags, NodeFlags::DontInheritRotation)) {
            effective.rotation = Quaternion{0, 0, 0, 1};
        }
        if (hasFlag(current.flags, NodeFlags::DontInheritScale)) {
            effective.scale = Vector3f{1, 1, 1};
        }
        world = Compose(effective, current.local);
    }
    return world;
}

Transform NodeTree::poseOf(u32 node, u32 pose) const {
    if (node >= size()) {
        return Transform::identity();
    }
    const Node& current = nodes[node];
    if (pose < current.poses.size()) {
        return current.poses[pose];
    }
    // No stored value: derive one from the schema's space, which is what a format
    // with an implicit identity bind (MDX, M2) wants.
    const PoseSpace space = pose < poseSchema.size() ? poseSchema[pose].space : PoseSpace::Model;
    const bool wantInverse = pose < poseSchema.size() && poseSchema[pose].inverse;
    const Transform base = space == PoseSpace::Model ? worldBind(node) : current.local;
    return wantInverse ? Inverse(base) : base;
}

void NodeTree::conformPoses() {
    const std::size_t wanted = poseSchema.size();
    for (u32 i = 0; i < size(); ++i) {
        Node& current = nodes[i];
        if (current.kind != NodeKind::Bone) {
            current.poses.clear();
            continue;
        }
        const std::size_t had = current.poses.size();
        if (had == wanted) {
            continue;
        }
        // Derive into a copy: growing `poses` first would make `poseOf` read the
        // freshly default-constructed entry instead of deriving one.
        std::vector<Transform> filled = current.poses;
        filled.resize(wanted);
        for (std::size_t p = had; p < wanted; ++p) {
            const bool wantInverse = poseSchema[p].inverse;
            const Transform base =
                poseSchema[p].space == PoseSpace::Model ? worldBind(i) : current.local;
            filled[p] = wantInverse ? Inverse(base) : base;
        }
        current.poses = std::move(filled);
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout

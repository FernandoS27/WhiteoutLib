// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/nodes/visitor.h>

namespace whiteout {
namespace models {
namespace wem {

void AcceptNode(NodeTree& tree, u32 node, NodeVisitor& visitor) {
    if (node >= tree.size()) {
        return;
    }
    Node& target = tree.nodes[node];
    // Dispatch on the payload alternative, not on `kind`: the two agree on any
    // document that passes validation, and where they disagree the payload is the
    // one that says what is actually stored.
    if (auto* payload = std::get_if<BonePayload>(&target.payload)) {
        visitor.OnBone(node, target, *payload);
    } else if (auto* attachment = std::get_if<AttachmentPayload>(&target.payload)) {
        visitor.OnAttachment(node, target, *attachment);
    } else if (auto* light = std::get_if<LightPayload>(&target.payload)) {
        visitor.OnLight(node, target, *light);
    } else if (auto* camera = std::get_if<CameraPayload>(&target.payload)) {
        visitor.OnCamera(node, target, *camera);
    } else if (auto* particle = std::get_if<ParticlePayload>(&target.payload)) {
        visitor.OnParticleEmitter(node, target, *particle);
    } else if (auto* ribbon = std::get_if<RibbonPayload>(&target.payload)) {
        visitor.OnRibbonEmitter(node, target, *ribbon);
    } else if (auto* event = std::get_if<EventPayload>(&target.payload)) {
        visitor.OnEvent(node, target, *event);
    } else if (auto* collision = std::get_if<CollisionPayload>(&target.payload)) {
        visitor.OnCollisionShape(node, target, *collision);
    } else {
        visitor.OnHelper(node, target);
    }
}

void Accept(NodeTree& tree, NodeVisitor& visitor) {
    for (u32 i = 0; i < tree.size(); ++i) {
        AcceptNode(tree, i, visitor);
    }
}

void AcceptDepthFirst(NodeTree& tree, NodeVisitor& visitor) {
    tree.ensureHierarchy();
    // Copy the root list: `AcceptNode` hands the visitor a mutable node, and a
    // visitor that restructures the tree would invalidate the cached ranges
    // underneath the loop.
    const NodeRange rootRange = tree.roots();
    const std::vector<u32> rootList(rootRange.begin(), rootRange.end());
    for (u32 root : rootList) {
        const NodeRange range = tree.subtree(root);
        const std::vector<u32> order(range.begin(), range.end());
        for (u32 node : order) {
            AcceptNode(tree, node, visitor);
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout

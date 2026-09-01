// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file visitor.h
 * @brief Per-kind node visitation (WEM v3, design §10.4).
 *
 * Two consumers with different needs, so two mechanisms:
 *
 * - **Compile-time**: an overload set over the payload variant. No virtuals, it
 *   inlines — the native-code default.
 * - **Runtime**: an interface with one defaulted no-op per kind, which is what
 *   the wasm / C / Python bindings expose, because an overload set does not cross
 *   an FFI.
 *
 * And a third that is neither: `tree.ofKind(NodeKind::Light)` is typed iteration
 * with no dispatch at all, which is the right answer whenever the caller already
 * knows the kind.
 */

#include <variant>

#include <whiteout/common_types.h>

#include "node.h"
#include "tree.h"

namespace whiteout {
namespace models {
namespace wem {

/// The usual overload-set helper, so a caller can write a lambda per kind and a
/// catch-all `[](auto&, auto&){}` for the rest.
template <class... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};
template <class... Fs>
overloaded(Fs...) -> overloaded<Fs...>;

// ============================================================================
// Compile-time visitation
// ============================================================================

/// Calls @p visitor with `(Node&, PayloadAlternative&)` for one node.
template <class Visitor>
void Visit(NodeTree& tree, u32 node, Visitor&& visitor) {
    if (node >= tree.size()) {
        return;
    }
    Node& target = tree.nodes[node];
    std::visit([&](auto& payload) { visitor(target, payload); }, target.payload);
}

template <class Visitor>
void Visit(const NodeTree& tree, u32 node, Visitor&& visitor) {
    if (node >= tree.size()) {
        return;
    }
    const Node& target = tree.nodes[node];
    std::visit([&](const auto& payload) { visitor(target, payload); }, target.payload);
}

/// Array order — the cheapest traversal, and the one a converter wants.
template <class Visitor>
void VisitAll(NodeTree& tree, Visitor&& visitor) {
    for (u32 i = 0; i < tree.size(); ++i) {
        Visit(tree, i, visitor);
    }
}

template <class Visitor>
void VisitAll(const NodeTree& tree, Visitor&& visitor) {
    for (u32 i = 0; i < tree.size(); ++i) {
        Visit(tree, i, visitor);
    }
}

/// Hierarchy order, depth-first pre-order over every root.
template <class Visitor>
void VisitDepthFirst(NodeTree& tree, Visitor&& visitor) {
    tree.ensureHierarchy();
    for (u32 root : tree.roots()) {
        for (u32 node : tree.subtree(root)) {
            Visit(tree, node, visitor);
        }
    }
}

template <class Visitor>
void VisitDepthFirst(const NodeTree& tree, Visitor&& visitor) {
    tree.ensureHierarchy();
    for (u32 root : tree.roots()) {
        for (u32 node : tree.subtree(root)) {
            Visit(tree, node, visitor);
        }
    }
}

/// Depth-first over one subtree.
template <class Visitor>
void VisitSubtree(NodeTree& tree, u32 root, Visitor&& visitor) {
    for (u32 node : tree.subtree(root)) {
        Visit(tree, node, visitor);
    }
}

// ============================================================================
// Runtime visitation
// ============================================================================

/**
 * @brief One defaulted no-op per kind — what crosses an FFI.
 *
 * A binding subclasses this and overrides the kinds it cares about. Adding a
 * `NodeKind` adds a method here; existing subclasses keep compiling, which is the
 * point of defaulting them.
 */
struct NodeVisitor {
    virtual ~NodeVisitor() = default;

    virtual void OnHelper(u32 id, Node& node) {
        (void)id;
        (void)node;
    }
    virtual void OnBone(u32 id, Node& node, BonePayload& payload) {
        (void)id;
        (void)node;
        (void)payload;
    }
    virtual void OnAttachment(u32 id, Node& node, AttachmentPayload& payload) {
        (void)id;
        (void)node;
        (void)payload;
    }
    virtual void OnLight(u32 id, Node& node, LightPayload& payload) {
        (void)id;
        (void)node;
        (void)payload;
    }
    virtual void OnCamera(u32 id, Node& node, CameraPayload& payload) {
        (void)id;
        (void)node;
        (void)payload;
    }
    virtual void OnParticleEmitter(u32 id, Node& node, ParticlePayload& payload) {
        (void)id;
        (void)node;
        (void)payload;
    }
    virtual void OnRibbonEmitter(u32 id, Node& node, RibbonPayload& payload) {
        (void)id;
        (void)node;
        (void)payload;
    }
    virtual void OnEvent(u32 id, Node& node, EventPayload& payload) {
        (void)id;
        (void)node;
        (void)payload;
    }
    virtual void OnCollisionShape(u32 id, Node& node, CollisionPayload& payload) {
        (void)id;
        (void)node;
        (void)payload;
    }
};

/// Array order.
void Accept(NodeTree& tree, NodeVisitor& visitor);
/// Hierarchy order.
void AcceptDepthFirst(NodeTree& tree, NodeVisitor& visitor);
/// One node.
void AcceptNode(NodeTree& tree, u32 node, NodeVisitor& visitor);

} // namespace wem
} // namespace models
} // namespace whiteout

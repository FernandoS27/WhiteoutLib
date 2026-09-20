// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file setup.h
 * @brief How a model is skinned, as the document keeps it
 *        (EDIT_MODE_SKIN_DESIGN.md §13.4).
 *
 * WEM holds how a model is worked on as well as the model, so a model reopened
 * next week is set up as it was left. None of it reaches a file: every exporter
 * ignores `Node::skin` and `Model::testPoses`, and the one thing that does
 * change a file is a classic pin, which lives in a vertex layer and is read by
 * `MdxConverter::writtenSkin` alone.
 *
 * The rest of the setup is per vertex and lives in the mesh's own attribute
 * layers, where a merge and a split carry it: `skinLocked`, `classicBones` and
 * the `selection.<name>` family (`geometry/attributes.h`).
 */

#include <optional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "../profile.h"

namespace whiteout {
namespace models {
namespace wem {

/// How an envelope's weight falls from its inner radius to its outer one (§8.2).
/// @bind
enum class EnvelopeFalloff : u8 {
    Hard,     ///< 1 inside, 0 outside.
    Linear,   ///< A straight ramp.
    Smooth,   ///< Smoothstep.
    Gaussian, ///< A bell, cut at the outer radius.
};

/// A bone's capsule of influence, at its two ends (§8.2). The radii are in the
/// model's units, and the inner one is where the weight is still 1.
struct Envelope {
    f32 innerStart = 0.0f;
    f32 innerEnd = 0.0f;
    f32 outerStart = 0.0f;
    f32 outerEnd = 0.0f;
    EnvelopeFalloff falloff = EnvelopeFalloff::Smooth;

    bool operator==(const Envelope&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("innerStart", innerStart);
        v.field("innerEnd", innerEnd);
        v.field("outerStart", outerStart);
        v.field("outerEnd", outerEnd);
        v.field("falloff", falloff);
    }
};

/// How a bone is meant to move (§9.3). An MDX bone stores a pivot and nothing
/// else, so this is the document's own record: Flex moves a bone through it, and
/// the stress test moves it only through it, between its limits.
/// @bind
enum class JointMotion : u8 {
    Bend,  ///< A hinge about the axis, between `low` and `high` degrees.
    Twist, ///< A turn about the bone's own length.
    Spin,  ///< A full turn; the limits are unused.
    Slide, ///< A travel along the axis, `low` to `high` in the model's units.
};

/// @bind
enum class JointAxis : u8 {
    X,
    Y,
    Z,
    Along, ///< The pivot to the first child's pivot.
};

struct Joint {
    JointMotion motion = JointMotion::Bend;
    JointAxis axis = JointAxis::X;
    /// X, Y and Z in the parent's space instead of the model's.
    bool parentSpace = false;
    f32 low = -45.0f;
    f32 high = 45.0f;

    bool operator==(const Joint&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("motion", motion);
        v.field("axis", axis);
        v.field("parentSpace", parentSpace);
        v.field("low", low);
        v.field("high", high);
    }
};

/// A saved test pose (§11.5): a base, and each node's departure from it in
/// `NodeSkinSetup::poseDeltas`.
struct TestPose {
    std::string name;
    /// The base: rest when `kInvalidIndex`, else `Document::clips[clip]` at
    /// `timeMs`. `RemoveClip` remaps it.
    u32 clip = kInvalidIndex;
    f32 timeMs = 0.0f;

    bool operator==(const TestPose&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("clip", clip);
        v.field("timeMs", timeMs);
    }
};

/// One node's departure from a test pose's base, parallel to `Model::testPoses`.
struct PoseDelta {
    Quaternion rotation{0, 0, 0, 1}; ///< About the node's pivot, in the parent's space.
    Vector3f translation{0, 0, 0};   ///< In the parent's space.

    bool operator==(const PoseDelta&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("rotation", rotation);
        v.field("translation", translation);
    }
};

/**
 * @brief The skin setup of one node: `Node::skin` (§13.4).
 *
 * Everything here is about this node, so a removal takes it along and a reorder
 * moves it, with no side table to keep in step — the rule `Node::poses` follows.
 * The one field that names another node, `mirror`, is a row in the §10.6
 * referencer table and is walked by `ForEachNodeLink`.
 */
struct NodeSkinSetup {
    /// No operation changes this bone's weight on any point (§6.2).
    bool locked = false;
    std::optional<Envelope> envelope;
    std::optional<Joint> joint;
    /// Overrides §7.5's mirror map; `kInvalidNode` for none.
    u32 mirror = kInvalidNode;
    /// Empty, or exactly as long as `Model::testPoses`. Empty means the identity
    /// in every pose, which is what an import and a fresh node both hold.
    std::vector<PoseDelta> poseDeltas;

    /// True when nothing is set: what an import makes, and what a file written
    /// before `NODE` v6 reads back as.
    bool empty() const {
        return !locked && !envelope.has_value() && !joint.has_value() &&
               mirror == kInvalidNode && poseDeltas.empty();
    }

    bool operator==(const NodeSkinSetup&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("locked", locked);
        v.optional("envelope", envelope);
        v.optional("joint", joint);
        v.field("mirror", mirror);
        v.field("poseDeltas", poseDeltas);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout

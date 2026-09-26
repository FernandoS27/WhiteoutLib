// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file pose_stage.h
 * @brief A pose stage: a constraint, an IK solve or a physics step that runs
 *        on the pose after the layers blend (WEM_ANIMATION_RUNTIME_DESIGN.md
 *        §5.1).
 *
 * A list on the model, in the order the stages run, and not a node kind: an
 * unknown kind would be written as a helper by `toMdx`, and a stage's inputs
 * are all nodes the pose already has, which is what makes it bakeable. Its
 * animated values are channels on its driven node, so the curve editor lists
 * them under that node and the key edits reach them as they reach any node
 * channel: its weight (`Channel::StageWeight`) and each source's weight
 * (`Channel::StageSourceWeight`) or, on a Link, its Enabled
 * (`Channel::StageSourceEnabled`), told apart by `StageSub`.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../nodes/node.h"

namespace whiteout {
namespace models {
namespace wem {

enum class StageKind : u8 {
    Position,    ///< The driven node stands at its sources' weighted position.
    Orientation, ///< The driven node takes its sources' weighted rotation.
    LookAt,      ///< The driven node's `axis` points at its sources' weighted position.
    Link,        ///< The driven node rides its first enabled source, or its own parent.
    LimbIk,      ///< Upper, lower and end reach the goal (`SolveLimb`).
    ChainIk,     ///< A chain of any length reaches the goal (`SolveChain`).
    Spring,      ///< A damped jiggle: each driven node lags where it is carried.
    Ragdoll,     ///< Rigid bodies, stepped by the host (`StageHooks`).
    Cloth,       ///< A cloth sheet, stepped by the host (`StageHooks`).
    Count
};

const char* ToString(StageKind kind);

/// Whether @p kind is a constraint: one driven node, read from its sources.
constexpr bool IsConstraint(StageKind kind) {
    return kind == StageKind::Position || kind == StageKind::Orientation || kind == StageKind::LookAt ||
           kind == StageKind::Link;
}

/// Whether @p kind is a physics stage: stepped in fixed 1/60 s steps with state
/// carried between frames, and run after every constraint and IK stage.
constexpr bool IsPhysicsStage(StageKind kind) {
    return kind == StageKind::Spring || kind == StageKind::Ragdoll || kind == StageKind::Cloth;
}

/// A stage channel's `TrackTarget::sub`: the stage's id, and above it the
/// source's, 0 for the stage's own weight.
constexpr u32 StageSub(u32 stage, u32 source = 0) {
    return stage | (source << 16);
}
constexpr u32 StageOfSub(u32 sub) {
    return sub & 0xFFFFu;
}
constexpr u32 SourceOfSub(u32 sub) {
    return sub >> 16;
}

/// A constraint's source: a node it reads, weighed against the others.
struct StageSource {
    /// Stable within its stage, from 1, and never reused while a channel names
    /// it (`StageSub`).
    u32 id = 0;
    /// The node read. On a Link, `kInvalidNode` is the world.
    u32 node = kInvalidNode;
    /// Where nothing keys its channel: its share of the pull, 1 for all of it
    /// — on a Link, whether it is enabled (above 0.5).
    f32 weight = 1.0f;

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("node", node);
        v.field("weight", weight);
    }
};

struct PoseStage {
    /// Stable for the model's life and never reused: its channels' `sub`.
    u32 id = 0;
    std::string name;
    StageKind kind = StageKind::Position;
    bool enabled = true;
    /// The nodes it writes, root first: one for a constraint, upper-lower-end
    /// for a limb, a chain root to tip, the bones a spring carries.
    std::vector<u32> driven;
    /// IK: the goal, and a pole.
    std::vector<u32> targets;
    /// A constraint's sources: a Position, Orientation or Look At blends them
    /// by weight; a Link rides one at a time.
    std::vector<StageSource> sources;

    /// Position, Orientation: where the driven node stood relative to the
    /// sources when `offset` was taken is kept, instead of snapping onto them.
    bool keepOffset = false;
    /// Look At: the driven node's own axis that points at the sources, and the
    /// one kept toward `upNode` (world up when it is none).
    Vector3f axis{1, 0, 0};
    Vector3f up{0, 0, 1};
    u32 upNode = kInvalidNode;
    /// Position, Orientation with `keepOffset`: the driven node relative to its
    /// blended sources at rest.
    Transform offset;
    /// Spring: how hard the lag pulls back to the carried pose (0..1 a step),
    /// how much of the speed each step loses (0..1), and the pull downward, in
    /// document units per second squared — a ragdoll's and a cloth's too.
    f32 stiffness = 0.2f;
    f32 damping = 0.6f;
    f32 gravity = 0.0f;

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("name", name);
        v.field("kind", kind);
        v.field("enabled", enabled);
        v.field("driven", driven);
        v.field("targets", targets);
        v.field("sources", sources);
        v.field("keepOffset", keepOffset);
        v.field("axis", axis);
        v.field("up", up);
        v.field("upNode", upNode);
        v.field("offset", offset);
        v.field("stiffness", stiffness);
        v.field("damping", damping);
        v.field("gravity", gravity);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout

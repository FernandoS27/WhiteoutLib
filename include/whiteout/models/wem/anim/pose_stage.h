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

#include <optional>
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
    Link,        ///< The driven node rides its sources, blended by share, and its own parent what they leave.
    LimbIk,      ///< Upper, lower and end reach the goal (`SolveLimb`).
    ChainIk,     ///< A chain of any length reaches the goal (`SolveChain`).
    Spring,      ///< A damped jiggle: each driven node lags where it is carried.
    Ragdoll,     ///< Rigid bodies, stepped by the host (`StageHooks`).
    Cloth,       ///< A cloth sheet, stepped by the host (`StageHooks`).
    Sc2LegIk,    ///< StarCraft II's leg adaptation (`IKJT`), which its runtime runs.
    Sc2Turret,   ///< StarCraft II's turret (`PATU`), which its runtime runs.
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

/// Whether @p kind is one only a game's own runtime runs
/// (EDIT_MODE_SC2_PROFILE_PLAN.md P1): the editor keeps it, the runner leaves it
/// alone, an export for a game that runs it writes its records, and one for any
/// other game drops it.
constexpr bool IsGameStage(StageKind kind) {
    return kind == StageKind::Sc2LegIk || kind == StageKind::Sc2Turret;
}

/// A StarCraft II leg's adaptation to the ground (`IKJT`). The chain is the
/// stage's `driven`, root to end: `boneIndex1` to `boneIndex2`.
struct Sc2LegIkParams {
    f32 probeUp = 1.5f;    ///< How far above the foot the ground is sought.
    f32 probeDown = -4.0f; ///< How far below, signed: the window is [z + down, z + up].
    f32 maxSpeed = 0.2f;   ///< How fast the leg may move to its goal.
    f32 goalThreshold = 0.05f;
    std::vector<u16> dependents; ///< As read.

    template <class V>
    void reflect(V& v) {
        v.field("probeUp", probeUp);
        v.field("probeDown", probeDown);
        v.field("maxSpeed", maxSpeed);
        v.field("goalThreshold", goalThreshold);
        v.field("dependents", dependents);
    }
};

/// A StarCraft II turret (`PATU`) on the stage's one driven node, and its place
/// in the named group (`TRGD`) the game aims it with. The frame and the two
/// rotations are in StarCraft II's basis, as read.
struct Sc2TurretParams {
    Matrix44f frame = Matrix44f::identity();
    Vector4f rotationA{0, 0, 0, 1};
    Vector4f rotationB{0, 0, 0, 1};
    bool mainTurret = false;
    u8 groupFlag = 1; ///< `turretGroupId`: 0 or 1 in every shipped record.
    u32 yawLimited = 2; ///< 2 limits yaw to [yawMin, yawMax]; 0 leaves it free.
    f32 yawMin = 0.0f;
    f32 yawMax = 0.0f;
    f32 yawWeight = 1.0f;
    u32 pitchLimited = 2;
    f32 pitchMin = 0.0f;
    f32 pitchMax = 0.0f;
    f32 pitchWeight = 1.0f;
    f32 unknown3 = 1.0f; ///< As read.
    f32 unknown4 = 1.0f; ///< As read.
    Vector3f mainBoneOffset{0, 0, 0};
    std::string group;       ///< The `TRGD` name; empty for none.
    u32 groupOrder = 0xFFFFFFFFu; ///< Where the group stood among the `TRGD`s.
    /// Where this turret stood in the group's list: more than once where a
    /// shipped list names it again.
    std::vector<u32> groupSlots;
    i32 version = 4;              ///< The `PATU` version read.

    template <class V>
    void reflect(V& v) {
        v.field("frame", frame);
        v.field("rotationA", rotationA);
        v.field("rotationB", rotationB);
        v.field("mainTurret", mainTurret);
        v.field("groupFlag", groupFlag);
        v.field("yawLimited", yawLimited);
        v.field("yawMin", yawMin);
        v.field("yawMax", yawMax);
        v.field("yawWeight", yawWeight);
        v.field("pitchLimited", pitchLimited);
        v.field("pitchMin", pitchMin);
        v.field("pitchMax", pitchMax);
        v.field("pitchWeight", pitchWeight);
        v.field("unknown3", unknown3);
        v.field("unknown4", unknown4);
        v.field("mainBoneOffset", mainBoneOffset);
        v.field("group", group);
        v.field("groupOrder", groupOrder);
        v.field("groupSlots", groupSlots);
        v.field("version", version);
    }
};

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
    /// — on a Link, of the carry (`LinkShares`).
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
    /// by weight; a Link by share, which it clamps to sum to at most 1.
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
    /// Ragdoll: the `PhysicsRig` it previews and bakes, 0 for the capsule chain
    /// over `driven`. Cloth: the `Cloth`. A target that runs either itself
    /// exports it natively instead (WEM_PHYSICS_DESIGN.md §7).
    u32 rig = 0;
    u32 cloth = 0;
    /// `Sc2LegIk` and `Sc2Turret`: what the game reads. PSTG v3; none on an
    /// older chunk, which holds neither kind.
    std::optional<Sc2LegIkParams> legIk;
    std::optional<Sc2TurretParams> turret;

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
        v.since(2).field("rig", rig);
        v.since(2).field("cloth", cloth);
        v.since(3).optional("legIk", legIk);
        v.since(3).optional("turret", turret);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout

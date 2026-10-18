// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file stages.h
 * @brief Running the pose stages, and baking them — with a clip's layers —
 *        into keys before an export stages the document
 *        (WEM_ANIMATION_RUNTIME_DESIGN.md §5.2, §6.1, §6.2).
 *
 * Constraints and IK are pure functions of the pose. Physics keeps state and
 * advances in fixed 1/60 s steps with the remainder carried, so a preview at
 * any frame rate and a bake agree. The bake runs on a copy, never the edited
 * document, and never inside a converter, because `toMdx` also draws the
 * viewport.
 */

#include <functional>
#include <map>
#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../diagnostics.h"
#include "../document.h"
#include "animator.h"
#include "pose_stage.h"

namespace whiteout {
namespace models {
namespace wem {

/// What a host lends the stages that the library does not have: a physics
/// engine for ragdoll and cloth.
struct StageHooks {
    /// Advances @p stage by @p dt on @p pose, writing its driven nodes'
    /// `local`s; false when the host cannot.
    std::function<bool(const Document&, u32 model, const PoseStage& stage, Pose& pose, f32 dt)> step;
    /// Forgets what the host simulates for @p model's @p stage.
    std::function<void(u32 model, const PoseStage& stage)> reset;
    /// Whether `step` can run a stage of @p kind; unset, every kind. A bake
    /// reports a stage its host cannot step rather than key it unmoved.
    std::function<bool(StageKind kind)> steps;
};

/// The game's own gravity in its own units, what a new physics stage falls
/// by: 9.8 m/s² for Warcraft III, Diablo III and World of Warcraft, and
/// StarCraft II's 8.8 (`PhysicsHostFor`). A force's *g*; never a length.
f32 DefaultGravity(Game game);

/// How many of @p game's units make a metre: Warcraft III about 55, Diablo
/// III about 3.5, StarCraft II and World of Warcraft one. What a tolerance or
/// a size in metres is scaled by.
f32 UnitsPerMetre(Game game);

/// The weight channel of @p stage, or null: `Channel::StageWeight` on its
/// first driven node, `sub` its id.
const AnimChannel* StageWeightChannel(const Model& model, const PoseStage& stage);

/// @p stage's weight in @p pose: its channel's value, 1 where it has none.
f32 StageWeight(const Model& model, const PoseStage& stage, const Pose& pose);

/// @p source's share of @p stage in @p pose: its channel's value, its own
/// `weight` where it has none.
f32 SourceWeight(const Model& model, const PoseStage& stage, const StageSource& source, const Pose& pose);

/// Each of a Link's sources' share of the node in @p pose, in `sources`' order:
/// its value, clamped so the shares, the first sources first, sum to at most 1.
/// What they leave is the node's own parent's.
std::vector<f32> LinkShares(const Model& model, const PoseStage& stage, const Pose& pose);

/// @p node's local transform that stands it at model-space @p frame over its
/// parent's frame in @p pose, by the rig's composition: what a host writes a
/// simulated body back as.
Transform StageLocalFor(const NodeTree& tree, const Pose& pose, u32 node, const Matrix44f& frame);

/// Sets @p stage's `offset` to where its driven node stands relative to its
/// blended sources at rest, so a Position or Orientation that keeps it starts
/// where the node already is. Any other kind's is the identity.
void CaptureStageOffset(const Document& document, u32 model, PoseStage& stage);

/// A node something other than the stages stands: a simulation's body, where
/// its step left it, in model space.
struct StageHold {
    u32 node = kInvalidNode;
    Matrix44f frame = Matrix44f::identity();
};

/// What is held on a pose the constraints run on: at the pose's own moment,
/// and at an earlier one of the same play, where a Link reads its switches.
struct StageHolds {
    std::span<const StageHold> now;
    /// The nodes held @p secondsAgo, where they stood then. Unset: none, and
    /// an earlier switch reads the animation alone.
    std::function<std::span<const StageHold>(f32 secondsAgo)> earlier;
};

/// Where a timeline's switches have left its Links, for a host to keep from
/// one pose to the next while the timeline and the document stand as they
/// were: a Link then replays only the switches it has not met.
struct StageCarries {
    /// By a Link's place in the stages and a switch's time in microseconds:
    /// the offset under each of its carriers once that switch is replayed.
    std::map<std::pair<u32, i64>, std::vector<Matrix44f>> offsets;
};

/// A timeline a host composes itself, layers of its own laid over the clips,
/// which no mix names: what the stages read of its other moments.
struct StageTimeline {
    /// The pose at @p seconds, as `Animator::sample` leaves one: the stage
    /// channels' values and the nodes' locals.
    std::function<void(f32 seconds, Pose& out)> sample;
    /// The times up to @p seconds, ascending, where the timeline keys any of
    /// @p channels, by id: where a Link whose sources' Enabled they are may
    /// switch.
    std::function<std::vector<f32>(std::span<const u32> channels, f32 seconds)> keys;
    /// What the host keeps of it. Null: each pose replays every switch behind
    /// it, once.
    StageCarries* carries = nullptr;
};

/**
 * @brief Runs one model's stages on a pose, in the list's order.
 *
 * One instance per played mix: a physics stage keeps its state between calls.
 * A snapshot of that state is kept every 30 steps, so going back in time
 * restores the nearest one at or before it and simulates forward from there,
 * which lands exactly where a straight run would.
 */
class StageRunner {
public:
    StageRunner(const Document& document, u32 model);

    /// Whether any enabled stage runs.
    bool empty() const;

    /// Runs the stages on @p pose, which @p animator sampled and composed, at
    /// @p seconds of a timeline @p loop seconds long (0 when it does not loop).
    /// Physics steps to @p seconds; a looping timeline starts warmed up, one
    /// cycle simulated before its first frame.
    void run(const Animator& animator, const Mix& mix, Pose& pose, f32 seconds, f32 loop,
             const StageHooks* hooks);

    /// The constraint and IK stages alone, which keep no state: what a pose
    /// read anywhere in the editor gets. A Link reads its switches from @p mix.
    void constrain(const Animator& animator, const Mix& mix, Pose& pose) const;

    /// The same with @p held's nodes standing at their frames: every stage
    /// reads them there, and one whose driven nodes are all held is left to
    /// what holds them.
    void constrain(const Animator& animator, const Mix& mix, Pose& pose, const StageHolds& held) const;

    /// The same on a host's own timeline, @p pose its pose at @p seconds,
    /// composed: a Link replays its switches along @p timeline from its start,
    /// as along a clip.
    void constrain(const Animator& animator, const StageTimeline& timeline, f32 seconds, Pose& pose) const;

    /// Drops every physics state and snapshot: a clip change, a loop restart,
    /// an undo.
    void reset(const StageHooks* hooks = nullptr);

private:
    struct Body {
        std::vector<Vector3f> position;
        std::vector<Vector3f> velocity;
        bool primed = false;
    };
    struct State {
        std::map<u32, Body> bodies; ///< By stage id.
    };

    /// The offset a Link's switches left under each of its carriers: the
    /// node's own parent, then its sources in order.
    struct LinkCarry {
        std::vector<Matrix44f> offsets;
    };

    /// What one pass over the stages has worked out of its Links.
    struct LinkPass;
    /// A Link's carriers and their shares in a pose.
    struct LinkReading {
        std::vector<Matrix44f> carriers;
        std::vector<f32> shares;
    };

    /// `constrain`, a Link's other moments read from @p timeline where it is
    /// given, each named by a mix's world clock.
    void constrainOn(const Animator& animator, const Mix& mix, Pose& pose, const StageHolds& held,
                     const StageTimeline* timeline) const;
    void constrainOne(const Animator& animator, const Mix& mix, Pose& pose, const PoseStage& stage,
                      LinkPass& pass) const;
    /// `constrainOne` on a pose where @p held's nodes, parents first, are held.
    void constrainHeld(const Animator& animator, const Mix& mix, Pose& pose, const PoseStage& stage, LinkPass& pass,
                       std::span<const StageHold> held) const;
    /// @p stage, a Link, read in the pose the constraint and IK stages before
    /// it leave at @p mix, @p ago seconds before the pass's own; at rest where
    /// it is null.
    LinkReading linkReading(const Animator& animator, const Mix* mix, const PoseStage& stage, LinkPass& pass,
                            f32 ago = 0.0f) const;
    /// Where @p mix's switches have left a Link: every key of its sources'
    /// Enabled replayed from the clip's start, the offsets rebased wherever the
    /// shares change, so the node keeps its place.
    LinkCarry linkCarry(const Animator& animator, const Mix& mix, const PoseStage& stage, LinkPass& pass) const;
    void step(const Animator& animator, const Mix& mix, f32 at, f32 loop, const StageHooks* hooks);
    void simulate(const Animator& animator, Pose& pose, const PoseStage& stage, f32 dt, const StageHooks* hooks);

    const Document* document_ = nullptr;
    const Model* model_ = nullptr;
    u32 modelIndex_ = kInvalidIndex;
    State state_;
    /// Steps taken since time 0 (a warmed loop starts at minus one cycle).
    i64 steps_ = 0;
    bool started_ = false;
    std::map<i64, State> snapshots_;
};

/// What `SimulatedStages` keeps of a run from step to step. A run that
/// outlives an edit holds this and makes the stages again.
struct SimulatedSteps {
    /// What each step wrote, by its time: where a Link's earlier switch reads.
    std::vector<f32> times;
    std::vector<std::vector<StageHold>> held;
    /// The mix's own keys at the step `before` saw, the pose a pass reads the
    /// simulation on, and whether `before` moved a node by it.
    Pose tracks;
    Pose read;
    bool led = false;

    void clear() {
        times.clear();
        held.clear();
        led = false;
    }
};

/**
 * @brief The constraint and IK stages around a simulation's steps (a bake's
 *        run, the Physics Test): they read the nodes it moves where it left
 *        them, never where the animation has them.
 *
 * `before` gives the pose a step runs on: the animation's, as `constrain`
 * leaves it, with each constrained node the simulation does not move standing
 * where the last step's bodies put what it reads. `after` gives the pose the
 * step leaves: its nodes where it wrote them, and every constraint over the
 * others run again on that, which is what a bake's keys play.
 */
class SimulatedStages {
public:
    SimulatedStages(const Document& document, u32 model, SimulatedSteps& steps);

    /// No constraint or IK stage is enabled: both calls leave the pose alone.
    bool empty() const {
        return driven_.empty();
    }

    /// @p pose is @p mix's, sampled and composed, at @p seconds of the run.
    /// @p simulated says, by node, whether the step about to run still moves
    /// it; a node being handed back to its animation is not one.
    void before(const Animator& animator, const Mix& mix, f32 seconds, Pose& pose, std::span<const u8> simulated);

    /// @p pose is `before`'s once the step wrote its nodes, @p written by node.
    void after(const Animator& animator, const Mix& mix, f32 seconds, Pose& pose, std::span<const u8> written);

private:
    StageHolds holds(f32 seconds, std::span<const StageHold> now) const;

    StageRunner stages_;
    const Model* model_ = nullptr;
    SimulatedSteps& steps_;
    /// The nodes a constraint or IK stage drives, parents first, and the force
    /// fields, which ride their animation whatever the bodies do.
    std::vector<u32> driven_;
    std::vector<u32> fields_;
    /// How long a step is kept: as far back as a Link can read.
    f32 keep_ = 0.0f;
};

/// Bakes @p document for an export to @p target: every clip's stages written
/// as keys on the nodes they drive, at 60 keys a second, each clip played
/// alone with the global loops under it; the stages and their weight channels
/// then removed. And every clip with several layers flattened unless
/// @p target's game plays layers. A ragdoll or cloth stage needs @p hooks, and
/// without them is reported (`AnimStageNotBaked`) and left out. Idempotent: a
/// document with nothing to bake comes back unchanged. Returns the clips
/// rewritten.
u32 BakeStages(Document& document, ProfileId target, const StageHooks* hooks,
               Diagnostics& diagnostics);

} // namespace wem
} // namespace models
} // namespace whiteout

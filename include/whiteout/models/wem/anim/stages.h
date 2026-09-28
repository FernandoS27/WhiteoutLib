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
};

/// 9.8 m/s² downward in @p game's own units, what a new physics stage falls
/// by: Warcraft III has about 55 units a metre, Diablo III about 3.5, and
/// StarCraft II and World of Warcraft one.
f32 DefaultGravity(Game game);

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

    void constrainOne(const Animator& animator, const Mix* mix, Pose& pose, const PoseStage& stage) const;
    /// The constraint and IK stages that run before @p stage.
    void constrainBefore(const Animator& animator, const Mix* mix, Pose& pose, const PoseStage& stage) const;
    /// Where @p mix's switches have left a Link: every key of its sources'
    /// Enabled replayed from the clip's start, the offsets rebased wherever the
    /// shares change, so the node keeps its place.
    LinkCarry linkCarry(const Animator& animator, const Mix* mix, const PoseStage& stage) const;
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

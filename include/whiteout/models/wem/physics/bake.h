// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file bake.h
 * @brief A clip's physics simulated once and baked into its keys
 *        (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §6-§8).
 *
 * The library runs the clip: its lead-in, the switch into it, the pose each
 * step and which bodies the simulation owns (`switches.h`). The physics
 * itself is the host's, lent through `BakeHooks` as the pose stages' is
 * through `StageHooks`, so the library stays engine-free.
 */

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../anim/animator.h"
#include "../diagnostics.h"
#include "../document.h"
#include "switches.h"

namespace whiteout {
namespace models {
namespace wem {

/// What the host needs for one step of a run.
struct BakeStep {
    /// Every body's state, in body order.
    std::span<const BodySwitch> switches;
    /// The winds and blasts of the clip being run (§4.7), at @ref time.
    std::span<const BakeWorldForce> world;
    f32 time = 0.0f; ///< Seconds into the clip being run.
    BakeFloor floor = BakeFloor::Grid;
    /// `FollowsRoot`: the floor's top, model space, where the clip puts the
    /// root (§4.8).
    f32 floorHeight = 0.0f;
    /// How far travel has carried the rig, model space: the run adds it
    /// where it places a body and takes it off where it writes one back.
    Vector3f travel{0, 0, 0};
    f32 dt = 1.0f / 60.0f; ///< One step: 1 / the step rate.
    /// Round the bodied nodes where the run started, model space: what a
    /// blast is placed and sized by.
    Vector3f lower{0, 0, 0};
    Vector3f upper{0, 0, 0};
};

/// One cloth particle where a step left it, model space: 28 bytes
/// (EDIT_MODE_PHYSICS_CLOTH_DESIGN.md §9.5). Its rotation turns its rest
/// frame onto its live one.
struct ClothParticleFrame {
    Vector3f position{0, 0, 0};
    Quaternion rotation{0, 0, 0, 1};
};

/// What the host's cloths did over one step: each by id, and every one's
/// particles, cloth after cloth.
struct BakeClothStep {
    std::vector<u32> cloths;
    std::vector<u32> particles; ///< Per cloth, how many.
    std::vector<u32> vertices;  ///< Per particle, its cage vertex.
    std::vector<ClothParticleFrame> frames;
};

/// The host's physics for a bake, as `StageHooks` is for the pose stages.
struct BakeHooks {
    /// Forget what the host simulates for @p model; the next step builds it
    /// where its pose stands, once for every clip: @p movable is every body
    /// that can move in any clip, by body index (§6).
    std::function<void(const Document& document, u32 model, std::span<const u8> movable)> reset;
    /// One step on @p pose, which the run sampled and composed: the bodies
    /// the step's switches hand the simulation are written back into its
    /// `local`s, T and R, and its frames composed again.
    std::function<void(const Document& document, u32 model, const BakeStep& step, Pose& pose)> step;
    /// The cloths stepped on @p pose, after `step` wrote the bodies back, so a
    /// cape follows its ragdoll; built where the first pose stands after a
    /// `reset`. Optional: without it, no cloth is recorded.
    std::function<void(const Document& document, u32 model, const BakeStep& step, const Pose& pose,
                       BakeClothStep& out)>
        cloth;
};

/// A clip's run, recorded (§6): every step of its lead-in, the clip and any
/// post-roll, at one step rate. What Simulate plays and a bake writes.
struct BakeRun {
    u32 clip = kInvalidIndex;
    f32 dt = 1.0f / 60.0f; ///< The clip's length over a whole number of steps.
    u32 first = 0;         ///< The step at the clip's frame 0.
    u32 steps = 0;         ///< Steps over the clip: its frame 0 to its end, `steps + 1` recorded.
    /// The nodes recorded, ascending: every node a body that can move sits on.
    std::vector<u32> nodes;
    /// Per recorded step, each recorded node's local T and R (its scale the
    /// animation's), and each body's state.
    std::vector<Transform> locals;
    std::vector<BodySwitch> switches;
    u32 bodies = 0;
    /// Per recorded step, the time in the clip: negative in the lead-in, past
    /// the end in a post-roll; and the time in the clip that step played,
    /// which in the lead-in is another clip's.
    std::vector<f32> times;
    std::vector<f32> localTimes;
    /// The lead-in's pieces, for the ruler: each one's clip and first step.
    struct Piece {
        u32 clip = kInvalidIndex;
        u32 step = 0;
        bool held = false; ///< The clip's first frame, held.
    };
    std::vector<Piece> pieces;
    /// After each preheat loop, the largest turn any recorded node made from
    /// the loop before at the same time, degrees (§7.5).
    std::vector<f32> settling;
    /// The cloths recorded, by id, and how many particles each has; per
    /// recorded step, every particle's frame, cloth after cloth.
    std::vector<u32> cloths;
    std::vector<u32> clothParticles;
    std::vector<u32> clothVertices; ///< Per particle, its cage vertex.
    u32 particles = 0;
    std::vector<ClothParticleFrame> clothFrames;
    /// Per recorded step, whether each recorded cloth draws (its *Active*).
    std::vector<u8> clothActive;
    /// Per recorded cloth that bakes into its bones (cloth design §10.2): the
    /// bones its fit moves, which `nodes` records; the largest distance the fit
    /// left between a drawn point and where the cloth put it, over the clip's
    /// own steps where it is active; and its drawn faces' size. No bones and 0
    /// for one that does not.
    std::vector<std::vector<u32>> clothBones;
    std::vector<f32> clothFit;
    std::vector<f32> clothSize;
    /// What went in (`BakeInputs`); whether the run reached its end.
    u64 inputs = 0;
    bool complete = false;
    /// Why it could not run, when it could not.
    std::string error;

    u32 recorded() const {
        return static_cast<u32>(times.size());
    }
    const Transform* localsAt(u32 step) const {
        return step < recorded() ? locals.data() + static_cast<std::size_t>(step) * nodes.size() : nullptr;
    }
    const BodySwitch* switchesAt(u32 step) const {
        return step < recorded() ? switches.data() + static_cast<std::size_t>(step) * bodies : nullptr;
    }
    bool clothActiveAt(u32 step, u32 cloth) const {
        const std::size_t at = static_cast<std::size_t>(step) * cloths.size() + cloth;
        return cloth < cloths.size() && at < clothActive.size() && clothActive[at] != 0;
    }
    const ClothParticleFrame* clothFramesAt(u32 step) const {
        return step < recorded() && particles != 0 ? clothFrames.data() + static_cast<std::size_t>(step) * particles
                                                   : nullptr;
    }
    /// The clip @p step played: the clip's own, or a lead-in's.
    u32 clipAt(u32 step) const {
        u32 at = clip;
        for (const Piece& piece : pieces) {
            if (piece.step <= step) {
                at = piece.clip;
            }
        }
        return at;
    }
    /// The recorded step nearest @p seconds of the clip.
    u32 stepAt(f32 seconds) const;
};

/// A hash of everything a bake of @p clip reads: its tracks and every lead-in
/// clip's, each from its bake's source; the model's nodes, physics and the
/// channels the sources key; the settings; and a once-played clip's *Goes to*
/// as it stands. A bake whose record holds another is out of date.
u64 BakeInputs(const Document& document, u32 clip);

/**
 * @brief Runs @p clip's physics once, from before its start (§6, §7.2).
 *
 * Every clip of the run is simulated from its bake's source, with its own
 * switches, winds, blasts and floor: a loop comes from itself, preheated; a
 * once-played clip from its *Comes from* chain, the game's switch between
 * each; a first frame is held a second per preheat loop. A loop matched at
 * its start runs on past its end for the match. @p cancel, when set, stops
 * the run between steps; the record then says it is not complete.
 */
/// How far a run has got, for a worker's progress: steps taken of all it
/// will take.
struct BakeProgress {
    std::atomic<u32> step{0};
    std::atomic<u32> steps{0};
};

BakeRun SimulateClip(const Document& document, u32 clip, const BakeHooks& hooks,
                     const std::atomic<bool>* cancel = nullptr, BakeProgress* progress = nullptr);

/// @p run's @p step onto @p pose, which holds the animation at that step's
/// time: the recorded nodes' T and R, and every frame composed again.
void RecordedPose(const Document& document, const BakeRun& run, u32 step, Pose& pose);

/// What a bake did (§8.3).
struct BakeReport {
    bool ok = false;
    std::string error;
    u32 nodes = 0;
    u32 keys = 0;
    u32 restated = 0; ///< TCB tracks restated as Hermite.
    f32 maxError = 0.0f;
    f32 seamBefore = 0.0f;
    f32 seamAfter = 0.0f;
    /// Nodes a global loop keys, which the bake left as they were (§8.2).
    std::vector<u32> globalLoops;
    /// A loop with no preheat cannot cross-fade its end: it was offset.
    bool offsetInstead = false;
    /// Per cloth baked into its bones (cloth design §10.2): its id, how many
    /// bones, and the fit's error against its size.
    struct ClothLine {
        u32 cloth = 0;
        u32 bones = 0;
        f32 fit = 0.0f;
        f32 size = 0.0f;
    };
    std::vector<ClothLine> cloths;
    Diagnostics diagnostics;
};

/**
 * @brief Writes @p run into its clip as keys (§8.2-§8.4): matched, spliced
 *        inside each node's active spans, reduced, and recorded so it can be
 *        undone.
 *
 * A clip baked before is restored to its source first. A clip on its MDX
 * window is detached. @p goesTo, the run of a once-played clip's *Goes to*,
 * is what its end is cross-faded into; without it, the target's keys as they
 * stand.
 */
BakeReport BakeClip(Document& document, const BakeRun& run, const BakeRun* goesTo = nullptr);

/// Puts back what @p clip's bake replaced, and drops the channels it declared
/// that nothing keys any more (§8.4). Nothing for a clip not baked. An editor
/// that journals the clips alone keeps them (@p dropChannels false): an
/// unkeyed channel plays nothing.
void UnbakeClip(Document& document, u32 clip, bool dropChannels = true);

/// @p document with every clip's bake source put back, for a run that reads
/// the sources: what `SimulateClip` runs on.
void RestoreBakeSources(Document& document);

/// @p clips in the order *Bake all* bakes them (§8.5): each after the clips
/// its *Comes from* chain and its *Goes to* name, so an Attack is matched into
/// a Stand already baked.
std::vector<u32> BakeOrder(const Document& document, std::vector<u32> clips);

/// The clips set up for the bake that are not baked: what an export's *Bake
/// physics* bakes (§8.6).
std::vector<u32> UnbakedSetUpClips(const Document& document);
/// The baked clips whose inputs no longer hold (§8.4).
std::vector<u32> OutOfDateBakes(const Document& document);
/// Whether @p clip's baked keys were edited since its bake (§8.4): a Rebake
/// would replace them.
bool BakedKeysEdited(const Document& document, u32 clip);

/**
 * @brief The export's bake (§8.6): every set-up clip not baked yet, in
 *        `BakeOrder`, run and baked on @p document, the export's own copy.
 *
 * A recording in @p runs of a clip whose inputs still hold is written rather
 * than run again, so an export writes what the editor showed. Returns how
 * many clips it baked; one it could not is reported and left as it was.
 */
u32 BakeForExport(Document& document, const BakeHooks& hooks,
                  std::span<const std::shared_ptr<const BakeRun>> runs, Diagnostics& out);

} // namespace wem
} // namespace models
} // namespace whiteout

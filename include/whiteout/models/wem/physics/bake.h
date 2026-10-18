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

struct SoftModesCache;

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
    /// One step on @p pose, which the run sampled, constrained and composed:
    /// the bodies the step's switches hand the simulation are written back
    /// into its `local`s, T and R, and its frames composed again. The run then
    /// has every constraint read them there (`SimulatedStages`).
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
    /// The nodes recorded, ascending: every node a body that can move sits
    /// on, and every node a constraint or IK stage drives.
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
/// as it stands. A bake whose record holds another is out of date. No bound is
/// in it: every one is measured from what the document holds, a bake's own
/// keys among it, and a save measures them again.
u64 BakeInputs(const Document& document, u32 clip);
/// Whether @p clip is baked and its record's key is the one `BakeInputs` gives
/// now. A record made while the bounds were in the key holds for as long as
/// they stand. This, and no comparison of the two, is what "out of date" asks.
bool BakeInDate(const Document& document, u32 clip);

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
    /// Per soft body the bake carried (soft design §11.2): its id, the lumps
    /// that got keys and the keys, how far its points are from where a key
    /// every step had them, and *follows within* over the clip: the mean, and
    /// the worst point's.
    struct SoftLine {
        u32 body = 0;
        u32 lumps = 0;
        u32 keys = 0;
        f32 within = 0.0f;
        f32 follows = 0.0f;
        f32 worst = 0.0f;
        f32 push = 0.0f;  ///< A Jiggle's push over its *Limit*, before the ceiling.
        f32 atEnd = 0.0f; ///< A clip played once: the swing it ends with, over the *Limit*.
    };
    std::vector<SoftLine> soft;
    Diagnostics diagnostics;
};

/**
 * @brief Writes @p run into its clip as keys (§8.2-§8.4): matched, spliced
 *        inside each node's active spans, reduced, and recorded so it can be
 *        undone.
 *
 * The model's soft bodies come last (soft design §11): stepped on the clip as
 * those keys now play it, their copies' and surface lumps' departures written
 * on the lumps' own channels and their bones used as they are keyed in place,
 * scale included, each only where it departs.
 *
 * A clip baked before is restored to its source first. A clip on its MDX
 * window is detached. A constraint or IK stage over a node it writes has its
 * weight keyed off there, so the clip plays the recording and not the stage
 * run over it again; its other nodes are written with it. A constrained node
 * the run does not move is left to its stage, which reads the baked keys of
 * what it follows. @p goesTo, the run of a once-played clip's *Goes to*,
 * is what its end is cross-faded into; without it, the target's keys as they
 * stand.
 */
BakeReport BakeClip(Document& document, const BakeRun& run, const BakeRun* goesTo = nullptr,
                    SoftModesCache* softModes = nullptr);

/// The steps a run of @p clip takes over the clip itself, and each one's
/// length: what a preview that is to show what a bake writes steps by.
u32 BakeStepsOf(const Document& document, u32 clip, f32& dt);

/// Puts back what @p clip's bake replaced, and drops the channels it declared
/// that nothing keys any more (§8.4). A key set or changed since outside the
/// stretches the bake keyed is the clip's own and stays, here and in every
/// read of the source. Nothing for a clip not baked. An editor
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
/// physics* bakes (§8.6). A clip a soft body acts in (`SoftActsIn`) is one,
/// set up or not: its lumps move only in a baked clip's keys.
std::vector<u32> UnbakedSetUpClips(const Document& document);
/// Those of them a soft body acts in: what is baked even for a game that runs
/// its own physics, since none runs a soft body.
std::vector<u32> UnbakedSoftClips(const Document& document);
/// The clips an export to a game without physics bakes (§8.6): the set-up ones
/// not baked, and every other clip the rule hands a body with a shape to the
/// simulation in. A World of Warcraft or Diablo III rig runs from the model's
/// creation, so no clip of such a model needs setting up to be one.
std::vector<u32> UnbakedPhysicsClips(const Document& document);
/// The baked clips whose inputs no longer hold (§8.4).
std::vector<u32> OutOfDateBakes(const Document& document);
/// Whether @p clip's baked keys were edited since its bake (§8.4): a Rebake
/// would replace them.
bool BakedKeysEdited(const Document& document, u32 clip);
/// What @p clip's bake record holds of the keys it wrote, hashed as they now
/// stand: what `PhysicsBake::written` is compared with. 0 for a clip not
/// baked. For an edit that changes what a bake wrote and stands by it.
u64 BakeWrittenNow(const Document& document, u32 clip);
/// @p clip's record stamped with what its bake wrote as it now stands
/// (`PhysicsBake::written` and `stretchInside`): for an edit that changes
/// what a bake wrote and stands by it.
void StampBakeWritten(Document& document, u32 clip);
/// Gives each bake record made before the stretches were kept
/// (`PhysicsBake::stretchChannels`) those of its nodes' channels, read off the
/// keys, so a key set beside them afterwards stays (§8.4). In a record whose
/// keys were edited before: a channel left with no key has no stretch, and
/// what the bake replaced on it is not put back; a key edited on any other
/// reads as the bake's. What a read of a `.wem` runs.
void MarkBakeStretches(Document& document);

/**
 * @brief The export's bake (§8.6): every clip `UnbakedPhysicsClips` lists, in
 *        `BakeOrder`, run and baked on @p document, the export's own copy.
 *
 * A recording in @p runs of a clip whose inputs still hold is written rather
 * than run again, so an export writes what the editor showed. Returns how
 * many clips it baked; one it could not is reported and left as it was.
 * With @p only, those clips and no other.
 */
u32 BakeForExport(Document& document, const BakeHooks& hooks,
                  std::span<const std::shared_ptr<const BakeRun>> runs, Diagnostics& out,
                  const std::vector<u32>* only = nullptr);

/// What an export's bake says as it goes, and asks.
struct BakeExportWatch {
    /// The clips baked so far of @p total; then, @p reducing, those whose keys
    /// the end reduces together. One call at a time, from any of its threads.
    std::function<void(bool reducing, u32 done, u32 total)> progress;
    /// Asked before each clip runs and each reduction: true stops, and what
    /// is not yet baked keeps its animation alone. From any of its threads.
    std::function<bool()> cancelled;
};

/// As above, the runs made side by side on @p threads threads (0: half the
/// hardware's count), each on hooks of its own from @p hooksFor, and the keys
/// of every clip no other bake reads reduced together at the end: the same
/// keys as one clip at a time, sooner.
u32 BakeForExport(Document& document, const std::function<BakeHooks()>& hooksFor,
                  std::span<const std::shared_ptr<const BakeRun>> runs, Diagnostics& out,
                  const std::vector<u32>* only = nullptr, u32 threads = 0,
                  const BakeExportWatch* watch = nullptr);

} // namespace wem
} // namespace models
} // namespace whiteout

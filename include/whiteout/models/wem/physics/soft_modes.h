// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file soft_modes.h
 * @brief Jiggle: a soft body's own ways of swinging, and its swing over a clip
 *        (EDIT_MODE_SOFTBODY_DESIGN.md §8.3).
 *
 * **A linear model in the lumps' own coordinates.** Each lump that swings has
 * up to nine numbers, in its carrier's frame and measured from its goal: a
 * move, a small turn and the logarithms of its three scales. A point moves by
 * what those do to it through its lanes. Mass is the points' areas gathered
 * onto the lumps; stiffness is three terms, **one mix for the whole body**:
 * each point pulled back to its goal by each of its lumps, neighbouring points
 * (found through space) moving alike, and the one direction that changes the
 * volume stiffened. Mass and stiffness give the modes, found once.
 *
 * **A step is exact.** Each mode is a damped oscillator; over a step in which
 * the push changes along a straight line its motion has a closed form, so
 * there are no substeps and the answer does not depend on the step rate. A
 * mode quicker than keys can play is slowed to the quickest that are and fully
 * damped, its resting answer kept: it follows the push and does not ring.
 *
 * **A loop closes exactly.** One pass from rest gives what a loop adds; the
 * state the loop maps onto itself is then two numbers a mode, solved; the pass
 * from it ends where it began.
 *
 * Nothing here is a constraint solver, a mesh of the body's volume, or a
 * stiffness set for one lump or bone.
 */

#include <limits>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../anim/clip.h"
#include "../nodes/node.h"
#include "soft.h"
#include "soft_contact.h"
#include "soft_fields.h"
#include "soft_fit.h"

namespace whiteout {
namespace models {
namespace wem {

/// A soft body's modes, for one mix of *Hold shape*, *Keep volume* and *Keep
/// in place*. *Looseness* and *Bounce* are not in them: they scale and damp.
struct SoftModes {
    /// One coordinate: a lump, and which of its nine (0-2 its move, 3-5 its
    /// turn, 6-8 the logarithms of its scales).
    struct Coordinate {
        u32 lump = 0;
        u8 kind = 0;
    };
    std::vector<Coordinate> coordinates;
    /// Per mode, its stiffness over the slowest's, ascending from 1: it swings
    /// at *Looseness* times the root.
    std::vector<f64> stiffness;
    /// The modes' shapes, a row a mode over the coordinates, each of mass 1.
    std::vector<f64> shapes;
    /// Per mode, the farthest one unit of it moves any point of the body.
    std::vector<f64> span;
    f32 size = 0.0f; ///< The region's.

    u32 count() const {
        return static_cast<u32>(stiffness.size());
    }
    bool empty() const {
        return stiffness.empty();
    }
    /// Mode @p mode's rate at *Looseness* @p looseness, swings a second.
    f32 rate(u32 mode, f32 looseness) const;
};

/// @p rig's modes under @p settings' *Hold shape*, *Keep volume* and *Keep in
/// place*. Empty for a body none of whose lumps swings.
SoftModes MakeSoftModes(const SoftRig& rig, const SoftVolume& volume, const SoftJiggle& settings);

/// A number that moves when anything @p rig's modes are made from does: its
/// points, their lanes, and its lumps' pivots and what each may do.
u64 SoftRigPrint(const SoftRig& rig);

/// How the push is kept from throwing the body past its *Limit* (§8.3). Both
/// leave it a function of time, so a loop still closes.
enum class SoftCeiling : u8 {
    /// Where a mode would swing past the *Limit*, the pushes that throw it there
    /// are scaled down, each as far as it is still felt there.
    Swing,
    Clip,  ///< One gain for the clip, found in the first pass.
    None,  ///< No ceiling: the output's stop is all there is.
};

/// What drives a body over a clip, a step at a time.
struct SoftJiggleDrive {
    u32 steps = 0; ///< `steps + 1` are given: the clip's frame 0 to its end.
    f32 dt = 1.0f / 60.0f;
    bool loops = false;
    /// Per step, every point where what drives the body has it, model space.
    std::vector<Vector3f> driven;
    /// Per step, per lump, its family bone's skinning matrix: the frame the
    /// lump's coordinates are said in.
    std::vector<Matrix44f> carriers;
    /// Per step, the Jiggle's level there (§9), 0 to 4: it scales what comes
    /// out. All of it where none is given.
    std::vector<f32> level;
    /// Per step, per point, the acceleration the world gives it there, model
    /// space: the clip's winds and blasts (`SoftWindAt`, `SoftBlastAt`) and
    /// the Jiggle's kicks (`SoftKickPush`). None where empty.
    std::vector<Vector3f> pushed;
    /// The colliders the body is kept out of (§8.3): per step, `colliderCount`
    /// of them as the pose has them there. None where empty.
    u32 colliderCount = 0;
    std::vector<SoftCollider> colliders;
    /// The floor's height, model space, where the body meets one.
    f32 floor = -std::numeric_limits<f32>::infinity();
};

struct SoftJiggleResult {
    /// Per step, per lump, its swing: a transform about its pivot along the
    /// model's axes at the bind, on top of its goal. At rest for a lump that
    /// does not swing.
    std::vector<Transform> swing;
    /// How far the loop's end is from its start, as the largest move of any
    /// mode over the region's size: rounding, when the loop was closed.
    f32 seam = 0.0f;
    /// The largest any mode moves a point, over the *Limit*, before the
    /// output's stop: 1 at most under a ceiling, with gravity's sag (half at
    /// most) and what the clip began with on top.
    f32 peak = 0.0f;
    /// The same before any ceiling: over 1, the push was limited.
    f32 over = 0.0f;
    f32 gain = 1.0f; ///< `Clip`: the one gain the push took.
    /// A clip played once: how much of a swing it ends with, over the *Limit*.
    f32 atEnd = 0.0f;
    /// Seconds from the last step whose push was above a hundredth of the
    /// clip's largest to the body lying within a hundredth of the *Limit* of
    /// rest; negative when it never settles within the clip.
    f32 settles = -1.0f;
};

/**
 * @brief @p rig's swing over a clip under @p drive (§8.3).
 *
 * The push is the points' own acceleration, by second differences, turned into
 * each lump's carrier's frame, and what the world gives them on top
 * (`SoftJiggleDrive::pushed`). What comes out is stopped at the *Limit*, kept
 * over the floor and out of the drive's colliders, each with no memory: no
 * point the swing moved is left under the floor, or deeper in a collider, than
 * what drives the body had it. Gravity, by *Weight*, sets where each mode
 * rests, and that is followed and not swung. A looping clip is run once from
 * rest, solved for the state it maps onto itself, and run again; a clip played
 * once starts from @p start (two numbers a mode: where and how fast), or
 * resting under its weight. @p end, when given, receives the state it ends in.
 */
SoftJiggleResult RunSoftJiggle(const SoftRig& rig, const SoftModes& modes, const SoftJiggle& settings,
                               const SoftJiggleDrive& drive, SoftCeiling ceiling = SoftCeiling::Swing,
                               std::span<const f64> start = {}, std::vector<f64>* end = nullptr);

/// How many of a body's ways of swinging a Wobble on *Random* mixes: its slowest.
constexpr u32 kSoftRandomModes = 6;

/// The farthest any point is moved by one of them, in Amounts.
constexpr f32 kSoftRandomFar = 3.0f;

/**
 * @brief *Random* (§8.4): a body's own ways of swinging, mixed by @p seed, as a
 *        swing a lump a step like `RunSoftJiggle`'s.
 *
 * The slowest of @p modes, each a sine: the slowest at @p speed swings a
 * second and every other quicker by its own rate over that (four times at
 * most), from a phase @p seed gives it. In a loop each makes a whole number
 * of cycles, so the loop closes; the phases are the same in every clip.
 * @p amount holds, per step, how far the wobble moves the body there, model
 * units: each mode is given the share of it that moves the body's points so
 * far over the whole of its skin (the root of their mean square, by area),
 * and no more than moves its farthest point `kSoftRandomFar` times that; over
 * the root of how many are mixed. A body's slowest ways of swinging are its
 * thin ends', a fin or a finger: said by the farthest point alone, an Amount
 * that sways the whole of a jelly would barely stir this one. Nothing here
 * has a memory.
 */
std::vector<Transform> SoftRandomWobble(const SoftRig& rig, const SoftModes& modes, f32 speed, u32 seed,
                                        std::span<const f32> amount, u32 steps, f32 dt, f32 duration, bool loops);

/// How long a blast is taken to push for, seconds: what makes its strength,
/// in g, a change of speed. The Physics tools' own number.
constexpr f32 kSoftBlastSeconds = 0.1f;

/// A clip's winds and blasts (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §4.7) as a soft
/// body feels them. Its g is the Jiggle's own, which goes by its size.
struct SoftWorld {
    std::span<const BakeWorldForce> forces;
    f32 size = 1.0f;     ///< The body's.
    f32 duration = 0.0f; ///< The clip's.
    bool looping = false; ///< A wind's gusts then come in whole cycles a loop.
    /// The box round the body at the bind: what a blast is placed and sized by.
    Vector3f lower{0, 0, 0};
    Vector3f upper{0, 0, 0};
    /// Per force, where a blast that names a centre node goes off: that node
    /// as the pose has it. Read for no other force.
    std::vector<Vector3f> centres;
};

/// The acceleration @p world's winds give every point at @p seconds, model
/// space: each by its strength, its ramps and its gusts, down its heading.
Vector3f SoftWindAt(const SoftWorld& world, f32 seconds);

/// The acceleration @p world's blasts give a point at @p at over the step of
/// @p dt that starts at @p seconds, model space. A blast kicks once, on the
/// step whose number holds its moment (a loop's last frame is its first): its
/// whole change of speed, away from its centre and fading to nothing at its
/// radius, spread over that step.
Vector3f SoftBlastAt(const SoftWorld& world, f32 seconds, f32 dt, const Vector3f& at);

/// How far round its place a kick is felt, of the region's size.
constexpr f32 kSoftKickReach = 0.5f;

/// The share of a kick a point @p distance from its place takes, on a body of
/// @p size: all of it at the place, none `kSoftKickReach` away.
f32 SoftKickShare(f32 distance, f32 size);

/// The acceleration a kick of @p strength g gives the point at its place over
/// the step of @p dt it lands in (§8.3): its whole change of speed, taken as a
/// blast's is (`kSoftBlastSeconds`), spread over that step.
f32 SoftKickPush(f32 strength, f32 size, f32 dt);

/// *Bounce* as a damping ratio for the slowest mode: 1 (no overshoot) at 0,
/// about a dozen swings to settle at 1.
f32 SoftDampingOf(f32 bounce);

} // namespace wem
} // namespace models
} // namespace whiteout

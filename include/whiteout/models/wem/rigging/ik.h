// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file ik.h
 * @brief The IK solves (EDIT_MODE_AUTO_IK_DESIGN.md §5): a limb of two joints,
 *        a leg of three, and a chain of any length.
 *
 * Positions in, world-space turns out. No WEM, no pose, no session — a limb is
 * its pivots and a goal, and the caller writes the turns as ordinary rotation
 * keys through the path every other drag uses. It sits here rather than in the
 * editor because the T-pose solve is a library operation
 * (EDIT_MODE_TPOSE_PLAN.md T0).
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

// ---- Auto IK (EDIT_MODE_AUTO_IK_DESIGN.md §5) -----------------------------------
//
// The goal is data (§9): a position and an orientation, handed in by the drag
// today and by a control rig's keyed targets later. Every solve starts again
// from the press, so a drag taken back to where it began gives back the press.
// World turns out, applied in order, each about where the turns before it left
// its joint — which is how `KeyTurn` writes them.

/// Past `L − kSoftReach·L` a limb eases toward full reach instead of snapping
/// straight (§5). A constant, not a setting.
constexpr f32 kSoftReach = 0.05f;

/// Where the End is asked to go.
struct IkGoal {
    Vector3f position{0, 0, 0};
    /// The End's world rotation to keep, so a foot stays flat.
    Quaternion orientation{0, 0, 0, 1};
    bool holdOrientation = true;
};

/// A limb at the press, in world space: the pivots of the Upper (thigh, upper
/// arm), the Lower (shin, forearm) and the End (foot, hand). A three-joint leg
/// has its Hock between the Lower and the End, and it rides unkeyed: the Lower
/// to the End is then one rigid segment.
struct LimbPress {
    Vector3f upper{0, 0, 0};
    Vector3f lower{0, 0, 0};
    Vector3f end{0, 0, 0};
    std::optional<Vector3f> hock;
    Quaternion endWorld{0, 0, 0, 1};
};

/// How the Lower bends. @ref hinge is a world unit axis, taken once at the press
/// (`JointHingeAxis`, or `FallbackHinge`) and never again during the drag: a
/// pole recomputed each frame is what flipped the old solve (§1.3). A straight
/// limb bends positively about it.
struct LimbSetup {
    Vector3f hinge{0, 0, 0};
    /// A Bend joint's `low` and `high`, degrees from the bind, as Flex measures.
    std::optional<std::pair<f32, f32>> bendLimitsDeg;
    /// The Lower's bend about @ref hinge at the press, measured the same way
    /// (`JointBendDeg`). The limits clamp this plus the solve's turn — and never
    /// pull a bend the clip already has back inside them.
    f32 bendDeg = 0.0f;
};

/// Three world turns: the Upper about its pivot, then the Lower about where that
/// left it, then the End about where both left it.
struct LimbSolve {
    Quaternion upperDelta{0, 0, 0, 1};
    Quaternion lowerDelta{0, 0, 0, 1};
    Quaternion endDelta{0, 0, 0, 1};
    /// Where the joints land once the turns are applied; a hock only for a
    /// three-joint leg.
    Vector3f lower{0, 0, 0};
    std::optional<Vector3f> hock;
    Vector3f end{0, 0, 0};
    bool ok = false;
    /// The End landed on the goal (to 1e-4 of the limb's length). Soft reach,
    /// a limit or a target past the limb leaves it short, pointing there.
    bool reached = false;
};

/// Hinge first (§5; Holden's two-joint IK): the Lower turns about the hinge
/// until the End is as far from the Upper as the (soft) target, the Upper then
/// aims it with the shortest arc, and a last turn about the Upper-to-target
/// line puts the hinge back as near its press direction as it can go, which is
/// what keeps the swivel. Lengths never change. Not ok when a bone has no length
/// or there is no hinge.
LimbSolve SolveLimb(const LimbPress& press, const LimbSetup& setup, const IkGoal& goal);

/// The whole limb turned by @p angleRad about the Upper-to-End line: the knee
/// swings and the End stays exactly where it is, its rotation held.
LimbSolve SwivelLimb(const LimbPress& press, f32 angleRad);

/// The hinge when no joint names one: the plane the press bends in; when the
/// limb is straight there, the plane @p rest bends in; when that is straight
/// too, square to the limb and @p forward, turned so a positive bend takes the
/// Lower toward @p forward when @p bendsForward (a knee) and away from it
/// otherwise (an elbow). Zero when the limb has no length.
Vector3f FallbackHinge(const LimbPress& press, const LimbPress& rest, const Vector3f& forward,
                       bool bendsForward);

/// A chain of any length (§5: spine, tail, neck): FABRIK from the press, then
/// one shortest-arc turn per joint, root down, each measured from where the
/// turns above left its segment, so no joint gains twist.
struct ChainSolve {
    /// One per joint, `press.size() - 1` of them.
    std::vector<Quaternion> deltas;
    Quaternion endDelta{0, 0, 0, 1};
    /// Where each point lands, the tip last.
    std::vector<Vector3f> points;
    bool ok = false;
    bool reached = false;
};

/// FABRIK's iterations and tolerance (the plan's §3 step 8).
constexpr u32 kChainIterations = 32;
constexpr f32 kChainTolerance = 1e-4f;

/// @p press is the chain's pivots root first, the tip last; @p endWorld is the
/// tip's rotation at the press. A target past the chain's length straightens
/// it toward the target. Not ok with fewer than two points or no length.
ChainSolve SolveChain(std::span<const Vector3f> press, const Quaternion& endWorld,
                      const IkGoal& goal);

/// @p v turned by @p q, as the rest of the solve turns things.
Vector3f Rotate(const Quaternion& q, const Vector3f& v);

} // namespace wem
} // namespace models
} // namespace whiteout

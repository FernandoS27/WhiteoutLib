// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file tpose.h
 * @brief Recovering a rig's T-pose (EDIT_MODE_TPOSE_DESIGN.md).
 *
 * No Warcraft III model is T-posed: the exporter wrote the scene as it stood,
 * so a rest arm sits a median 54 degrees off the figure's side with the elbow
 * bent. The pose is not in the file, so it is solved — from a donor, a saved
 * record, the clips, `BPOS` or the user — and this is where the solving lives.
 *
 * Two pieces, in the order they run:
 *
 * 1. @ref HingeOf, the axis a joint bends about. Elbows and knees really are
 *    hinges in the clips (the top eigenvalue takes >= 0.95 of the scatter on
 *    75% of HD elbows and 85% of knees), so the axis is measured rather than
 *    guessed — but the document's own `Joint` record outranks the measurement,
 *    and a joint with no keys falls back to the plane the limb rests in.
 * 2. The solve itself, which turns the joints and hands back a list of turns;
 *    applying them to a document is `Rebind`'s (rebind.h).
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../document.h"
#include "limbs.h"

namespace whiteout {
namespace models {
namespace wem {

/// Which rung of the ladder answered (§3.4).
enum class HingeSource : u8 {
    None,     ///< Nothing did: no length, no record and no key.
    Joint,    ///< `NodeSkinSetup::joint`, a `Bend` — the document's own answer.
    Clips,    ///< The scatter of the node's own rotation keys.
    Fallback, ///< The plane the limb rests in (`FallbackHinge`).
};

const char* ToString(HingeSource source);

/// The axis a joint bends about, and how much the answer is worth.
struct HingeFit {
    /// Unit, in the model's space. Zero when @ref source is `None`.
    Vector3f axis{0, 0, 0};
    /// The top eigenvalue's share of the scatter — 1.0 for a pure hinge, 1/3
    /// for a joint that turns every way. Only `Clips` fills it.
    f32 share = 0;
    /// The largest bend the clips actually reach, in degrees: how far the
    /// measurement may be trusted to extrapolate.
    f32 visitedDeg = 0;
    /// Rotation keys the measurement read.
    u32 keys = 0;
    HingeSource source = HingeSource::None;
};

/**
 * @brief The hinge @p node's own rotation keys describe (§3.4).
 *
 * Every `Channel::Rotation` sub-track that names @p node, in every clip and
 * every container: each key's turn contributes `angle * axis`, and the answer
 * is the principal direction of those vectors — so a key's weight is the
 * square of its angle, and a hundred small turns cannot outvote the one frame
 * that actually bends the elbow. A key under three degrees points its axis
 * anywhere and is dropped; fewer than @ref kHingeKeys of them left is no
 * measurement, and `source` stays `None`.
 *
 * Global-sequence tracks count like any other: they are keys. Hermite and
 * Bezier streams contribute their **values** and not their tangents — a tangent
 * is not a pose.
 *
 * Reads keys, not poses, so it needs no sampler and no playhead.
 */
HingeFit HingeFromClips(const Document& document, u32 model, u32 node);

/**
 * @brief @p limb's bending axis, from the best rung that answers (§3.4).
 *
 * `Joint` when the Lower carries a `Bend` record, else `Clips` when the
 * measurement is a hinge worth the name (`share >= 0.9`, at least four keys),
 * else `Fallback` — the plane the limb rests in, turned so a positive bend
 * takes the Lower the way a knee or an elbow goes.
 *
 * At rest, which is where the T-pose is solved: no pose, no playhead.
 */
HingeFit HingeOf(const Document& document, u32 model, const Limb& limb);

/// `HingeFromClips` is believed from here up (§3.4's measured shares).
inline constexpr f32 kHingeShare = 0.9f;
/// Turning keys below which there is no measurement at all — §3.4's floor.
inline constexpr u32 kHingeKeys = 6;


// ============================================================================
// The solve (§3.2, §4)
// ============================================================================

/// What the solve is asked for (§3.2). Every row of the canonical table is a
/// switch, because "T-pose" is not one thing.
struct TPoseRules {
    bool arms = true;
    bool legs = true;
    /// Q1: off on a fresh model — a spine is posed, not rested.
    bool spineAndHead = false;
    /// Aim a hand along its arm and a foot along +X, levelled.
    bool handsAndFeet = true;
    bool symmetry = true;
    /// Q2: 0 is a T, 45 an A. Degrees the arms come down from level.
    f32 armDownDeg = 0;
};

/// Why a limb was not solved. Reported, never worked around (§5.3, §12).
enum class TPoseRefusal : u8 {
    None,
    NoChain,     ///< The record does not name the whole limb.
    ZeroLength,  ///< A bone with no length: there is nothing to aim.
    NoHinge,     ///< Nothing said which way it bends.
    JointLimits, ///< It cannot straighten inside its own `Bend` limits.
    Unreachable, ///< The solve did not land the End on the goal.
    Overlaps,    ///< Another limb already turned one of its joints.
};

const char* ToString(TPoseRefusal refusal);
const char* ToString(TPoseSource source);

/// One node's turn: a rotation in the model's space about where the turns above
/// it have left its rest point — the shape every Auto IK solve hands back, and
/// what `KeyTurn` and @ref TPoseRest both read.
struct TPoseJoint {
    u32 node = kInvalidNode;
    Quaternion turn{0, 0, 0, 1};
    /// How far that is, in degrees: the number the panel prints.
    f32 turnDeg = 0;
    TPoseSource source = TPoseSource::None;
};

/// One limb, as the panel lists it (§6).
struct TPoseLimb {
    u32 end = kInvalidNode;
    RigSide side = RigSide::Centre;
    RigLimb limb = RigLimb::Other;
    HingeFit hinge;
    TPoseRefusal refusal = TPoseRefusal::None;
    /// The largest turn any of its joints takes.
    f32 turnDeg = 0;
    /// How far the Upper came out from the rule, in degrees, as the solver
    /// landed it: 0 for a limb that straightened, and the reason the row is
    /// refused when it did not.
    f32 aimDeg = 0;
};

/// Where the solve may start from, beyond the model itself.
struct TPoseInputs {
    /// A saved pose whose `You` joints must survive a re-solve: an index into
    /// `Model::testPoses`, normally `Model::tPose`.
    u32 keep = kInvalidIndex;
};

struct TPoseResult {
    std::vector<TPoseJoint> joints;
    std::vector<TPoseLimb> limbs;

    bool empty() const {
        return joints.empty();
    }
};

/**
 * @brief The turns that put @p model's rig in the canonical pose (§3.2, §4).
 *
 * No new solver: each limb the record names goes through `SolveLimb` with a
 * canonical goal — the Upper's rest point plus the canonical direction times
 * the chain's own reach — and a spine through `SolveChain`. Soft reach is what
 * makes "straight" reachable without stretching, and a limb that cannot
 * straighten inside its `Bend` limits is **reported, not forced**.
 *
 * Runs on the rest pose: no playhead, no plant, no blend.
 *
 * A model with no nameable limb comes back empty — there is nothing wrong with
 * it and nothing to offer.
 */
TPoseResult SolveTPose(const Document& document, u32 model, const TPoseRules& rules = {},
                       const TPoseInputs& inputs = {});

/**
 * @brief @p result as the rest transform every node should stand in — one per
 *        node, in the model's space. What `Rebind` takes.
 *
 * A node the solve did not touch keeps the rest it has, so the span is always
 * `model.nodes.size()` long and a caller never has to ask which nodes moved.
 */
std::vector<Transform> TPoseRest(const Model& model, const TPoseResult& result);

} // namespace wem
} // namespace models
} // namespace whiteout

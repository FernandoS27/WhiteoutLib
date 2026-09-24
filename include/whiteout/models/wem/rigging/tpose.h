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

#include <span>
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
// The canon, and the directions a rig is read in (EDIT_MODE_TPOSE_REDESIGN.md §1, §2)
// ============================================================================

/**
 * @brief The canonical pose, in degrees.
 *
 * What Reforged ships on every HD rig, measured over the 416 HD units shaped
 * like a person (R§1.1): a relaxed A, not a textbook T. It is what a rig with
 * no twin is aimed at, and what an HD rig is measured against to show the tool
 * leaves it alone. The defaults are pinned to the HD medians by
 * `tpose_canon_test`, so a number here cannot drift from the measurement it
 * came from. @ref StrictT is the T — arms level, joints locked, feet straight —
 * one control away for a retarget, never the default.
 */
struct TPoseCanon {
    f32 armDownDeg = 45.4f; ///< The upper arm below level.
    f32 armBackDeg = 10.9f; ///< The upper arm behind the figure's Y axis (toward -X).
    f32 elbowDeg = 21.0f;   ///< The elbow's bend. The forearm goes forward: the elbow points back.
    f32 legOutDeg = 7.4f;     ///< The upper leg off vertical, outward (R§1.1's 14.2 is the total, creatures in).
    f32 legForwardDeg = 6.3f; ///< The upper leg off vertical, forward (+X): the knee's share of the stance.
    f32 kneeDeg = 14.0f;    ///< The knee's bend, the humanoid median. The shin goes back: the knee points forward.
    f32 footOutDeg = 11.0f; ///< A foot's heading off +X, toe-out.
    /// How far a limb may sit from the table's direction and be left as it is.
    /// The table is a median, and a correct rig sits up to this far from it
    /// (`tpose_canon_test` prints HD's spread), so a solve aimed at the table
    /// would turn a correct rig toward the middle for nothing. A twin is exact
    /// and gets no band. Zero for none.
    f32 spreadDeg = 20.0f;

    static TPoseCanon StrictT();

    /// Where @p role's bone aims under this canon: unit, in the model's space,
    /// +Y being the figure's left. A foot's is its heading, level; a hand's is
    /// its forearm's.
    Vector3f Direction(RigLimb limb, RigSide side, RigRole role) const;
};

/// One limb's bones at a rest, as unit directions in the model's space. The
/// terms the canon and a twin are stated in: scale-free, which is what makes a
/// twin of another size usable (R§2 — heights differ by 1.09 at the median).
struct LimbDirections {
    u32 end = kInvalidNode;
    u32 lower = kInvalidNode;
    u32 upper = kInvalidNode;
    RigLimb limb = RigLimb::Other;
    RigSide side = RigSide::Centre;
    Vector3f upperDir{0, 0, 0}; ///< Upper -> Lower.
    Vector3f lowerDir{0, 0, 0}; ///< Lower -> End.
    Vector3f bone{0, 0, 0};     ///< The End's own bone, End -> its first joint child; zero for a leaf.
    Vector3f up{0, 0, 0};       ///< The End's +Z, as the rest turns it.
    f32 endHeight = 0;          ///< The End's z.
};

/// What a rig looks like at a rest, role by role: every limb `LimbOf` names,
/// the head's bone and the hip axis. A twin's, read at its bind, is the donor
/// the solve takes (R§3.0).
struct RigDirections {
    std::vector<LimbDirections> limbs;
    u32 head = kInvalidNode;
    Vector3f headBone{0, 0, 0}; ///< The head's parent -> the head.
    Vector3f headUp{0, 0, 0};   ///< The head's +Z, as the rest turns it.
    Vector3f hips{0, 0, 0};     ///< The right Upper leg -> the left one; zero without both.
    f32 height = 0;             ///< The joints' extent along +Z.

    /// The first limb of @p limb on @p side, or null.
    const LimbDirections* Find(RigLimb limb, RigSide side) const;
};

/// @p model at @p rest — one `Transform` per node in the model's space, as
/// @ref TPoseRest hands back — or at its bind.
RigDirections DirectionsOf(const Model& model, std::span<const Transform> rest);
RigDirections DirectionsAtBind(const Model& model);

// The angles the canon is stated in, from a unit direction. Degrees.

/// Below level; negative above it.
f32 DroopDeg(const Vector3f& direction);
/// Above level; negative below it.
f32 PitchDeg(const Vector3f& direction);
/// Forward (+X) of @p side's own Y axis; negative behind it.
f32 SweepDeg(const Vector3f& direction, RigSide side);
/// Off +X in the ground plane, toward +Y positive: a left foot's toe-out.
f32 HeadingDeg(const Vector3f& direction);
/// Between two directions; 0 when either has no length.
f32 AngleDeg(const Vector3f& a, const Vector3f& b);
/// Between two lines: the angle or its supplement, whichever is smaller.
f32 LineAngleDeg(const Vector3f& a, const Vector3f& b);

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
    /// Where the rules aim (R§2): HD’s relaxed A by default, `TPoseCanon::StrictT()`
    /// for a T. A donor outranks it role by role (`TPoseInputs::donor`).
    TPoseCanon canon;
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
    /// Left as it stands: already within `TPoseCanon::spreadDeg` of the table's
    /// direction (R§2). Never set for a limb a twin aimed.
    bool inCanon = false;
};

/// Where the solve may start from, beyond the model itself.
struct TPoseInputs {
    /// A saved pose whose `You` joints must survive a re-solve: an index into
    /// `Model::testPoses`, normally `Model::tPose`.
    u32 keep = kInvalidIndex;
    /// A rig already in the pose — the HD twin of a DE unit, read at its bind
    /// (`DirectionsAtBind`) — whose directions are the targets for every role
    /// it has (R§3.0). Null for none. Directions, never positions: the
    /// twin is another size.
    const RigDirections* donor = nullptr;
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


// ============================================================================
// The measurement (R§4.5, R§5)
// ============================================================================

/// What one row of a score measures.
enum class TPoseScoreKind : u8 {
    Aim,         ///< A limb joint's direction off its target, or a hand's bone off its.
    Heading,     ///< A foot's heading off its target.
    Level,       ///< An End's up off +Z.
    Mirror,      ///< A mirrored pair's disagreement: Uppers across the figure's plane, feet headings summed.
    AnkleHeight, ///< The two ankles' height gap, in units, not degrees.
    Head,        ///< The head's bone off its target.
    Hips,        ///< The hip axis off +Y.
};

const char* ToString(TPoseScoreKind kind);

struct TPoseScoreRow {
    u32 node = kInvalidNode;
    RigLimb limb = RigLimb::Other;
    RigSide side = RigSide::Centre;
    RigRole role = RigRole::None;
    TPoseScoreKind kind = TPoseScoreKind::Aim;
    f32 value = 0; ///< Degrees; units for `AnkleHeight`.
};

/// A pose scored against its target: the twin where there is one, the canon
/// where there is not. What the gates assert and what the panel lists as still
/// off, from one function, so the two cannot disagree.
struct TPoseScore {
    std::vector<TPoseScoreRow> rows;
    /// The largest degree row.
    f32 worstDeg = 0;
};

/**
 * @brief @p model at @p rest, measured against @p donor's directions where it
 *        has them and @p canon everywhere else (R§4.5, R§5).
 *
 * A limb joint's target is the donor's same role on the same side, else the
 * canon's direction for it; a foot's is a heading, never a pitch (the slope
 * of a foot bone is the bind's, R§1.6); a hand's is the donor's hand bone,
 * else its own forearm. The mirror rows, the ankle gap, the head and the hips
 * are measured against the figure itself.
 */
TPoseScore MeasureTPose(const Model& model, std::span<const Transform> rest,
                        const TPoseCanon& canon, const RigDirections* donor = nullptr);

} // namespace wem
} // namespace models
} // namespace whiteout

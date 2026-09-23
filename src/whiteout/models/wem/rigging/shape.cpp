// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// The shape source (EDIT_MODE_AUTO_IK_DESIGN.md §3.2, revision 1's rules): the
// last guess, and the only one for rigs whose names say nothing.
//
// - A passenger is a node with no length of its own, or one its limb runs
//   straight on through: it does not bend, so it is a Twist and not a joint.
// - A joint is a hub when another of its branches goes two bending joints deep
//   (a chest carrying the other arm and the head): a limb does not reach past
//   one.
// - Above a known End, the first two joints that are neither are its Lower and
//   Upper.
// - A pad is a leaf hanging off a limb joint onto a point further down the same
//   limb, within a tenth of the limb's length: the kneecap on the knee, the
//   armpad at the wrist.

#include "detect_parts.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace rig_detect {

namespace {

f32 Distance(const Vector3f& a, const Vector3f& b) {
    return (a - b).length();
}

/// The angle between two segments, in degrees; 0 when either has no length,
/// because a segment that is a point runs nowhere of its own.
f32 AngleDeg(const Vector3f& u, const Vector3f& v, f32 tiny) {
    const f32 lu = u.length();
    const f32 lv = v.length();
    if (lu < tiny || lv < tiny) {
        return 0.0f;
    }
    const f32 c = std::clamp(u.dot(v) / (lu * lv), -1.0f, 1.0f);
    return std::acos(c) * 57.29577951308232f;
}

struct Shape {
    Work& work;
    f32 tiny;
    std::vector<i32> depth;

    explicit Shape(Work& w) : work(w), tiny(w.Tiny()), depth(w.tree.size(), -1) {}

    f32 Segment(u32 node) const {
        const u32 parent = work.Parent(node);
        return parent == kInvalidNode ? 0.0f : Distance(work.point[node], work.point[parent]);
    }

    /// How many bending joints deep the limb below @p node goes.
    i32 Depth(u32 node) {
        if (depth[node] >= 0) {
            return depth[node];
        }
        depth[node] = 0; // A cycle reads as no depth rather than recursing.
        i32 best = 0;
        for (const u32 child : work.joints[node]) {
            best = std::max(best, (Segment(child) < tiny ? 0 : 1) + Depth(child));
        }
        depth[node] = best;
        return best;
    }

    bool Passenger(u32 node, u32 toward) const {
        const u32 parent = work.Parent(node);
        if (parent == kInvalidNode) {
            return false;
        }
        if (Segment(node) < tiny) {
            return true;
        }
        if (toward == kInvalidNode) {
            return false;
        }
        return AngleDeg(work.point[node] - work.point[parent], work.point[toward] - work.point[node],
                        tiny) < 1.0f;
    }

    bool Hub(u32 node, u32 toward) {
        for (const u32 child : work.joints[node]) {
            if (child != toward && (Segment(child) < tiny ? 0 : 1) + Depth(child) >= 2) {
                return true;
            }
        }
        return false;
    }
};

bool IsChainRole(RigRole role) {
    return role == RigRole::Upper || role == RigRole::Lower || role == RigRole::Hock;
}

/// Up from @p end: the joints a limb would take, passengers collected apart.
/// Stops at a hub, at a role no limb has, or once it holds @p want joints.
///
/// One walk, because the chain below and the check above it have to agree on
/// what counts as a joint: two enumerations of that would drift.
struct Above {
    std::vector<u32> joints;
    std::vector<u32> passengers;
};

Above WalkAbove(Work& work, Shape& shape, u32 end, std::size_t want) {
    Above out;
    u32 prev = end;
    for (u32 o = work.Parent(end); o != kInvalidNode && out.joints.size() < want;
         o = work.Parent(o)) {
        const NodeRig& rig = work.rig[o];
        if (!work.Free(o) && rig.role != RigRole::Twist) {
            if (!IsChainRole(rig.role)) {
                break;
            }
            out.joints.push_back(o);
        } else if (rig.role == RigRole::Twist || shape.Passenger(o, prev)) {
            out.passengers.push_back(o);
        } else if (shape.Hub(o, prev)) {
            break;
        } else {
            out.joints.push_back(o);
        }
        prev = o;
    }
    return out;
}

/// A limb's two bones are comparable — a thigh and a shin, an upper arm and a
/// forearm. An End whose OWN bone is this small a share of the one above it is
/// not where the limb ends: it is a foot standing on the ankle.
///
/// Warcraft III's Deluxe Edition rigs are the case that needs it. They name the
/// BALL of the foot `foot_L0_0_jnt`, which the name tier reads as the End,
/// while the leg chain itself ends at `leg_L0_end_jnt` above it — so the chain
/// comes out two joints low and the T-pose straightens the shin and the foot
/// instead of the thigh and the shin. Measured on the DE footman the share is
/// 0.18 (4.0 against 22.6); the HD footman's real knee is 1.22 and the DE
/// elbow 0.70, so the bar sits well clear of both.
constexpr f32 kStubBoneShare = 0.25f;

/// A foot and its ball, never a walk up the whole leg.
constexpr u32 kReanchorLimit = 2;

/// Another End already hangs below @p at, beside @p but and not under it.
///
/// Then @p but is a leaf next to where the limb ends, not the limb's own tip,
/// and moving it up would put two Ends on one line. The HD harpy is the case:
/// `R_hand_feather_03_bind_jnt` is a FEATHER off the forearm that the name tier
/// reads as a hand, on the forearm's own point, while the real
/// `R_hand_bind_jnt` hangs two twist bones further down.
bool HoldsAnotherEnd(Work& work, u32 at, u32 but) {
    std::vector<u32> stack{at};
    for (u32 steps = work.tree.size(); !stack.empty() && steps > 0; --steps) {
        const u32 node = stack.back();
        stack.pop_back();
        for (const u32 child : work.joints[node]) {
            if (child == but) {
                continue; // Past the End is the End's own business.
            }
            if (work.rig[child].role == RigRole::End) {
                return true;
            }
            stack.push_back(child);
        }
    }
    return false;
}

/// Moves an End that rides below its limb up to where the bones say the limb
/// ends. The joint it leaves keeps the limb and the side and becomes a Toe:
/// still the leg's, no longer what the solve aims.
///
/// Never touches an End the FILE named or a person set: shape is the last
/// guess (§3.2), and it corrects only the guesses above it.
void ReanchorEnds(Work& work, Shape& shape) {
    const u32 count = work.tree.size();
    for (u32 n = 0; n < count; ++n) {
        if (!work.IsJoint(n) || work.rig[n].role != RigRole::End) {
            continue;
        }
        const RigSource source = work.rig[n].source;
        if (source == RigSource::File || source == RigSource::You) {
            continue;
        }
        u32 end = n;
        for (u32 step = 0; step < kReanchorLimit; ++step) {
            const Above above = WalkAbove(work, shape, end, 2);
            if (above.joints.size() < 2) {
                break;
            }
            const f32 own = Distance(work.point[end], work.point[above.joints[0]]);
            const f32 next = Distance(work.point[above.joints[0]], work.point[above.joints[1]]);
            if (next < shape.tiny || own >= kStubBoneShare * next) {
                break;
            }
            const u32 to = above.joints[0];
            const RigSource held = work.rig[to].source;
            if (held == RigSource::File || held == RigSource::You ||
                HoldsAnotherEnd(work, to, end)) {
                break;
            }
            const RigSide side = work.rig[end].side;
            const RigLimb limb = work.rig[end].limb;
            // Written rather than `Set`: that claims a joint nothing has named
            // yet, and both of these are already named — this is the one place
            // that CORRECTS a guess instead of filling a gap.
            NodeRig& was = work.rig[end];
            was.role = limb == RigLimb::Leg ? RigRole::Toe : RigRole::None;
            was.source = RigSource::Shape;
            NodeRig& now = work.rig[to];
            now.role = RigRole::End;
            now.side = side;
            now.limb = limb;
            now.source = RigSource::Shape;
            end = to;
        }
    }
}

} // namespace

void ShapeTier(Work& work) {
    Shape shape(work);
    const u32 count = work.tree.size();
    // An End the guesses above put below its limb is moved up first, so the
    // chain below is walked from where the limb really ends.
    ReanchorEnds(work, shape);
    for (u32 end = 0; end < count; ++end) {
        if (!work.IsJoint(end) || work.rig[end].role != RigRole::End) {
            continue;
        }
        const NodeRig& at = work.rig[end];
        // Up from the End: passengers are skipped, a joint another source
        // named counts as it stands, and a hub or any other role ends the limb.
        const Above above = WalkAbove(work, shape, end, 2);
        const std::vector<u32>& joints = above.joints;
        const std::vector<u32>& passengers = above.passengers;
        if (joints.size() < 2) {
            continue;
        }
        // Under a Lower another source named, a joint that bends is between
        // the knee and the End: the Hock, never a second knee (the Makrura's
        // back legs: `back_L_leg_02`, then an unnamed `bone_leg_back_left`).
        const bool named = !work.Free(joints[1]) && work.rig[joints[1]].role == RigRole::Lower;
        work.Set(joints[0], named ? RigRole::Hock : RigRole::Lower, at.side, at.limb, RigSource::Shape);
        work.Set(joints[1], RigRole::Upper, at.side, at.limb, RigSource::Shape);
        for (const u32 passenger : passengers) {
            work.Set(passenger, RigRole::Twist, at.side, at.limb, RigSource::Shape);
        }
    }

    // Pads: a leaf off a limb joint (or the clavicle above it) that sits on a
    // point further down the same limb.
    for (u32 end = 0; end < count; ++end) {
        if (!work.IsJoint(end) || work.rig[end].role != RigRole::End) {
            continue;
        }
        std::vector<u32> chain{end};
        f32 length = 0.0f;
        u32 o = end;
        while (true) {
            const u32 up = work.Parent(o);
            if (up == kInvalidNode) {
                break;
            }
            const RigRole role = work.rig[up].role;
            if (!IsChainRole(role) && role != RigRole::Twist && role != RigRole::Clavicle) {
                break;
            }
            length += Distance(work.point[o], work.point[up]);
            chain.push_back(up);
            o = up;
            if (role == RigRole::Clavicle) {
                break;
            }
        }
        if (chain.size() < 2 || length < shape.tiny) {
            continue;
        }
        const NodeRig& at = work.rig[end];
        for (std::size_t j = 1; j < chain.size(); ++j) {
            const u32 joint = chain[j];
            for (const u32 child : work.joints[joint]) {
                if (!work.Free(child) || !work.joints[child].empty() ||
                    std::find(chain.begin(), chain.end(), child) != chain.end()) {
                    continue;
                }
                // Onto a point below the joint it hangs from: the chain up to
                // but not including it.
                for (std::size_t k = 0; k < j; ++k) {
                    if (Distance(work.point[child], work.point[chain[k]]) <= 0.1f * length) {
                        work.Set(child, RigRole::Pad, at.side, at.limb, RigSource::Shape);
                        break;
                    }
                }
            }
        }
    }
}

} // namespace rig_detect
} // namespace wem
} // namespace models
} // namespace whiteout

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

} // namespace

void ShapeTier(Work& work) {
    Shape shape(work);
    const u32 count = work.tree.size();
    for (u32 end = 0; end < count; ++end) {
        if (!work.IsJoint(end) || work.rig[end].role != RigRole::End) {
            continue;
        }
        const NodeRig& at = work.rig[end];
        // Up from the End: passengers are skipped, a joint another source
        // named counts as it stands, and a hub or any other role ends the limb.
        std::vector<u32> joints;
        std::vector<u32> passengers;
        u32 prev = end;
        for (u32 o = work.Parent(end); o != kInvalidNode && joints.size() < 2; o = work.Parent(o)) {
            const NodeRig& rig = work.rig[o];
            if (!work.Free(o) && rig.role != RigRole::Twist) {
                if (!IsChainRole(rig.role)) {
                    break;
                }
                joints.push_back(o);
            } else if (rig.role == RigRole::Twist || shape.Passenger(o, prev)) {
                passengers.push_back(o);
            } else if (shape.Hub(o, prev)) {
                break;
            } else {
                joints.push_back(o);
            }
            prev = o;
        }
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

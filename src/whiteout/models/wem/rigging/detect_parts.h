// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

// The pieces `DetectRig` is made of (rigging/detect.h), shared by detect.cpp,
// names.cpp and shape.cpp and by nothing else.

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/nodes/tree.h>
#include <whiteout/models/wem/rigging/detect.h>
#include <whiteout/models/wem/rigging/record.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace rig_detect {

/// What a node's name says (§3.2's second source).
struct NameRead {
    RigRole role = RigRole::None;
    RigSide side = RigSide::Centre;
    RigLimb limb = RigLimb::Other;
    /// `mount_`: the mount's figure, kept apart from the rider's.
    bool mount = false;
    /// `hd_anim`: a face rig's root.
    bool face = false;
    /// A body word (pelvis, turret, root, a Biped's COM): Body on a figure's
    /// top and Spine anywhere else, because only a top may move the figure.
    bool body = false;
    /// The word `root` and nothing else: the model's own root, which may be
    /// only the stage the figures stand on (HD's `root_bind_jnt`).
    bool stage = false;
    /// `leg_03`, `arm_02`: the joint's number along its limb, 0 for none. From
    /// 3 on it is a Hock above the End and a Toe below it.
    u32 index = 0;
    /// `leg_L0_end_jnt`: a chain terminator, the End by construction. The
    /// shape tier leaves it where it is, however short its own bone.
    bool terminator = false;
};

NameRead ReadName(const std::string& name);

/// One detection's working state.
struct Work {
    const NodeTree& tree;
    std::vector<NodeRig>& rig;
    /// Each node's bind point, in the model's space.
    std::vector<Vector3f> point;
    /// Each node's children that are joints (a bone or a helper).
    std::vector<std::vector<u32>> joints;
    std::vector<NameRead> names;
    /// The rig's extent, for tolerances that scale with the model.
    f32 size = 1.0f;
    /// The rig's height (its extent along +Z), for §3.4's ride test.
    f32 height = 1.0f;
    /// Per node: a root that is only the stage (`BodyTier`) — named `root`, and
    /// every child of it that carries a hand or a foot is a body or sits on
    /// one. Its children are the figures' tops; it is none of them.
    std::vector<u8> stage;

    Work(const NodeTree& tree, std::vector<NodeRig>& rig);

    bool IsJoint(u32 node) const;
    u32 Parent(u32 node) const;
    bool IsRoot(u32 node) const {
        return IsJoint(node) && Parent(node) == kInvalidNode;
    }
    /// A figure's top: a root that is not a stage, or a child of one. What may
    /// move the figure, ride another, or be ridden as a whole.
    bool IsTop(u32 node) const {
        if (!IsJoint(node) || stage[node]) {
            return false;
        }
        const u32 parent = Parent(node);
        return parent == kInvalidNode || stage[parent] != 0;
    }
    /// The top @p node hangs from (itself when it is one).
    u32 TopOf(u32 node) const;
    /// Role, side and limb unset: a later source may fill them.
    bool Free(u32 node) const {
        return node < rig.size() && rig[node].source == RigSource::None;
    }
    /// Sets role, side and limb, if @p node is still free.
    void Set(u32 node, RigRole role, RigSide side, RigLimb limb, RigSource source);
    /// Whether @p node is @p ancestor or under it.
    bool Under(u32 node, u32 ancestor) const;
    /// A segment shorter than this is a point: `max(0.01, 1e-4 · size)`.
    f32 Tiny() const;
};

void FileTier(Work& work);
void NameTier(Work& work);
void AttachmentTier(Work& work);
/// The numbered joints from three on, once the attachments have said where
/// the limbs end: a Hock above the End, a Toe past it, the End itself when it
/// is the last of its number and nothing else ends the limb.
void NumberedTier(Work& work);
/// One End and one Lower to a limb's line, whatever the words said: an End
/// below another is past it (a Toe), and of two Lowers in a line the one nearer
/// the End is the Hock. The HD centaurs' front legs read `upr_arm`, `lwr_arm`,
/// `lwr_knee`, `hand`, `hand_01`. What the file or you set is left alone.
void LineTier(Work& work);
void ShapeTier(Work& work);

} // namespace rig_detect
} // namespace wem
} // namespace models
} // namespace whiteout

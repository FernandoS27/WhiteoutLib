// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file record.h
 * @brief What a node is to its rig (EDIT_MODE_AUTO_IK_DESIGN.md §3.3), as the
 *        document keeps it.
 *
 * Authoring state, like the skin setup: it says which nodes are a limb's joints,
 * which roots move as one body and what rides what, so a drag can move a figure
 * the way the rig was built to move. It is filled once when a model enters WEM
 * (`DetectRig`), a correction made in the editor is saved with the `.wem`, and
 * every exporter ignores it.
 *
 * It does not repeat what WEM already has: a knee's hinge and its bend limits
 * are `NodeSkinSetup::joint`, and a node's left/right partner is the mirror map
 * with its `NodeSkinSetup::mirror` override.
 *
 * It describes the rig, not the drag, so a later control rig can read it as it
 * stands (§9).
 */

#include <whiteout/common_types.h>

#include "../profile.h"

namespace whiteout {
namespace models {
namespace wem {

/// What a node is to its rig (§3.3).
/// @bind
enum class RigRole : u8 {
    None,
    Body,     ///< A root the whole figure moves with: a pelvis, a turret, a COM.
    Spine,
    Neck,
    Head,
    Clavicle, ///< The shoulder above an arm.
    Upper,    ///< A thigh, an upper arm.
    Lower,    ///< A shin, a forearm.
    Hock,     ///< A joint between the Lower and the End of a three-joint leg.
    End,      ///< A hand, a foot, a paw, a hoof.
    Toe,
    Twist,    ///< A passenger that only shares a turn: a forearm's twist bones.
    Pad,      ///< A kneecap, an armpad, a pauldron: hangs off a joint.
    Prop,     ///< A weapon, a shield, a book.
};

/// Which side a node is on. +Y is the figure's left in the Blizzard space.
/// @bind
enum class RigSide : u8 {
    Centre,
    Left,
    Right,
};

/// Which kind of limb a joint belongs to (EDIT_MODE_AUTO_IK_PLAN.md P1): what
/// decides whether an End plants by default. A quadruped's front legs are Legs.
/// @bind
enum class RigLimb : u8 {
    Other,
    Arm,
    Leg,
};

/// When an End holds its place while something above it moves (§6).
/// @bind
enum class RigPlant : u8 {
    ByRole,   ///< A leg's End is On the ground, anything else is Free.
    OnGround, ///< Holds while it is near the ground at the playhead.
    Always,
    Free,
};

/// Who set a field (§3.2): the tiers, best first, and the user last. "Detect
/// again" redoes every field whose source is not You.
/// @bind
enum class RigSource : u8 {
    None,
    File,       ///< The file's own labels: WoW's key-bone and attachment ids.
    Name,       ///< A naming scheme's table.
    Attachment, ///< An attachment point naming where a limb ends.
    Shape,      ///< The rig's shape alone.
    You,        ///< A correction made in the editor.
};

/**
 * @brief One node's rig record (§3.3).
 *
 * `source` covers `role`, `side` and `limb`, which one tier always sets
 * together; `ridesSource` covers `ridesWith`, which is found separately.
 * `plant` and `twistShare` are never detected: each holds its default or what
 * the user set, so detecting again never touches them (the plan's P2).
 */
struct NodeRig {
    RigRole role = RigRole::None;
    RigSide side = RigSide::Centre;
    RigLimb limb = RigLimb::Other;
    /// For a node not parented to what it moves with: the node it rides. A
    /// face-rig root rides the head, a rider's Body the mount's spine, a Prop a
    /// hand. `kInvalidNode` for none. A row in the referencer table, so a
    /// removal remaps it and clears it with no code of its own.
    u32 ridesWith = kInvalidNode;
    /// Meaningful on an End.
    RigPlant plant = RigPlant::ByRole;
    /// On a Twist bone, its share of the End's twist (§9); 0 until followers
    /// read one from the clips.
    f32 twistShare = 0.0f;
    RigSource source = RigSource::None;
    RigSource ridesSource = RigSource::None;

    /// True when nothing is set: what a `NODE` written before v10 reads back
    /// as, and what a model nobody has detected holds.
    bool empty() const {
        return *this == NodeRig{};
    }

    bool operator==(const NodeRig&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("role", role);
        v.field("side", side);
        v.field("limb", limb);
        v.field("ridesWith", ridesWith);
        v.field("plant", plant);
        v.field("twistShare", twistShare);
        v.field("source", source);
        v.field("ridesSource", ridesSource);
    }
};

const char* ToString(RigRole role);
const char* ToString(RigSide side);
const char* ToString(RigLimb limb);
const char* ToString(RigPlant plant);
const char* ToString(RigSource source);

/// @p side across the figure's mirror: Left and Right swap, Centre stays.
constexpr RigSide MirroredSide(RigSide side) {
    return side == RigSide::Left ? RigSide::Right
                                 : side == RigSide::Right ? RigSide::Left : RigSide::Centre;
}

} // namespace wem
} // namespace models
} // namespace whiteout

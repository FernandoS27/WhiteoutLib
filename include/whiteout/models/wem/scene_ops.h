// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file scene_ops.h
 * @brief What an editor does to a model as a scene: rescale all of it, move a
 *        part of it, and bring another model into it.
 *
 * All three are pivot-rig operations, and they share one problem
 * `inline_models.h` met first: a pivot rig has no rest rotation or scale, so a
 * part that turns or grows is BAKED — points move, every key of its nodes is
 * conjugated — which is exact for a uniform scale. A conjugated frame stays on
 * the model's axes, though, and a leaf whose own axes say where something goes
 * (an emitter's direction, a box's sides) has to take the turn onto its frame
 * instead, by a key where it had none. A PopcornFX emitter takes the scale the
 * same way: the game runs its effect at the node's world scale, which a bake
 * that moves only lengths leaves at 1.
 */

#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "diagnostics.h"
#include "document.h"
#include "retarget.h"

namespace whiteout {
namespace models {
namespace wem {

/// A similarity in a model's space: row vectors, scale first, then the
/// rotation, then the translation — `ToMatrix`'s order. One scale for all three
/// axes, which is what keeps a placement exact through a pivot rig's keys.
struct Placement {
    Quaternion rotation{0, 0, 0, 1};
    f32 scale = 1.0f;
    Vector3f translation{0, 0, 0};

    Vector3f vector(const Vector3f& v) const {
        return rotation.rotate_vector(v * scale);
    }
    Vector3f point(const Vector3f& p) const {
        return vector(p) + translation;
    }
    /// Turning, scaling or moving anything at all.
    bool moves() const;
};

/// The global loop a turned frame's one key goes on when no clip keys it; the
/// name `inline_models.cpp` gives its own, so the two share one.
inline constexpr const char* kFrameLoopName = "frames";

/// What `RescaleScene` did: `RescaleDocument`'s report, and the PopcornFX
/// emitters whose own frame took the factor.
struct SceneRescaleResult {
    RescaleResult rescale;
    u32 framesScaled = 0;
    u32 keysAdded = 0;
};

/// `RescaleDocument`, and every PopcornFX emitter of a pivot rig scaled on its
/// own frame by @p factor, so its effect grows with the model.
SceneRescaleResult RescaleScene(Document& document, f32 factor);

/// What `TurnFrame` wrote.
struct FrameCounts {
    u32 keysRewritten = 0;
    u32 keysAdded = 0;
};

/**
 * Lays @p turn and @p scale onto @p node's own frame: each key of its rotation
 * becomes `turn * key`, each of its scale `scale * key`, and its rest value the
 * same. A channel a global loop keys is then right everywhere. One no clip keys
 * gets a one-key track on the frame loop; one only some animations key gets one
 * in each animation that leaves it at rest, holding the turned rest.
 */
void TurnFrame(Document& document, u32 model, u32 node, const Quaternion& turn, f32 scale, FrameCounts& counts);

/// Every node in the subtrees of @p roots, once each, ascending.
std::vector<u32> SubtreeNodes(const NodeTree& tree, std::span<const u32> roots);

/// Per vertex of @p mesh, the node it moves with: the section's rigid node,
/// else its heaviest influence, else `kInvalidNode`.
std::vector<u32> VertexOwners(const Mesh& mesh);

struct PlaceResult {
    bool ok = false;
    u32 nodesPlaced = 0;
    u32 verticesPlaced = 0;
    u32 keysRewritten = 0;
    /// Leaves whose own frame took the turn, or a PopcornFX emitter's the scale.
    u32 framesTurned = 0;
    /// One-key tracks those frames needed.
    u32 keysAdded = 0;
    /// Values a pivot rig cannot carry through the turn exactly: a non-uniform
    /// scale key, a collision box on a node that is not a leaf.
    u32 approximated = 0;
    Diagnostics diagnostics;
};

/**
 * @brief Moves the subtrees of @p roots, in model @p model, by @p placement —
 *        their pivots, rests and keys, the payload lengths and points they
 *        hold, and every vertex that rides one of them.
 *
 * A vertex rides the node `VertexOwners` names. Every clip's keys are
 * rewritten, a sequence's as a global loop's. A node outside the subtrees is
 * not touched, and neither is the model's extent, which the editor measures
 * again before a write.
 */
PlaceResult PlaceNodes(Document& document, u32 model, std::span<const u32> roots, const Placement& placement);

/// What a merge does with one of the donor's nodes.
enum class MergeNodeAction : u8 {
    Add,  ///< A node of ours of its own, under the group.
    Into, ///< Dissolved into a node of ours: what named it names ours.
    Skip, ///< Left out: its children and its skin go to its nearest kept parent.
};

struct MergeOptions {
    /// The helper every merged root hangs from, and the prefix of every name
    /// merged; made unique in the model.
    std::string name = "Merged";
    /// Restate the merged model in @ref profile's units first
    /// (`RescaleFactorBetween` of the donor's default profile).
    bool matchUnits = true;
    ProfileId profile = ProfileId::Wc3Reforged;

    // The choices (EDIT_MODE_SCENE_MERGER_DESIGN.md §10.1), each per donor
    // element by its index in the donor. An empty one keeps the behaviour above.

    /// Per donor node.
    std::vector<MergeNodeAction> nodes;
    /// Per donor node: the node of ours an `Into` node becomes.
    std::vector<u32> into;
    /// Per donor node: the node of ours it hangs from instead, or `kInvalidNode`
    /// to keep its own parent (the group, for a root).
    std::vector<u32> parent;
    /// Per donor node: keeps its own name, without the group's in front — an
    /// attachment the game finds by name, an event whose name is its code.
    std::vector<u8> keepName;
    /// Per `Into` node: its channels land on our node's (a pair). Otherwise
    /// they stay behind and our node keeps its own motion.
    std::vector<u8> pair;
    /// Per donor node: made a helper. Once given, a camera is made one only
    /// where this says so; a kind no profile of ours carries always is.
    std::vector<u8> asHelper;
    /// Per donor mesh: brought.
    std::vector<u8> meshes;
    /// Per donor material slot: the slot of ours its sections draw with, or
    /// `kInvalidIndex` to bring its own.
    std::vector<u32> slots;
    /// Per donor clip (document index): brought. Empty brings its global loops
    /// and none of its animations.
    std::vector<u8> clips;
    /// Per donor clip: the name an animation takes in ours; empty keeps its
    /// own. A loop is always named after the group.
    std::vector<std::string> clipNames;
    /// The node of ours the group's helper hangs from; `kInvalidNode` for a root.
    u32 groupParent = kInvalidNode;
};

struct MergeResult {
    bool ok = false;
    /// The group's helper, and the first merged node after it.
    u32 group = kInvalidNode;
    u32 nodes = 0;
    u32 meshes = 0;
    u32 slots = 0;
    u32 textures = 0;
    u32 loops = 0;
    /// The donor's animations brought (`MergeOptions::clips`).
    u32 clips = 0;
    /// The donor's animations left behind.
    u32 clipsLeft = 0;
    /// Nodes of a kind no profile of the model carries, and cameras, made helpers.
    u32 nodesDemoted = 0;
    /// The profile sets that had to be derived for the model's profiles.
    u32 setsDerived = 0;
    /// What the merged model was restated at (`MergeOptions::matchUnits`).
    f32 unitScale = 1.0f;
    /// Per donor node, mesh and clip (document index): where it landed in
    /// ours, or `kInvalidNode` / `kInvalidIndex` for one not brought. An `Into`
    /// node maps to the node of ours it became.
    std::vector<u32> nodeOf;
    std::vector<u32> meshOf;
    std::vector<u32> clipOf;
    Diagnostics diagnostics;
};

/**
 * @brief Copies model @p from of @p donor into model @p model of @p into, under
 *        a new helper at the origin that names the group.
 *
 * Its global loops come too, as loops of the model; its animations do not.
 * Its material sets are the ones the model's profiles need, derived where the
 * donor has none; its slots, nodes and meshes take the group's name in front of
 * theirs, so an attachment lookup by name never finds one of its points. A
 * node a profile of the model does not run, a camera, its physics and pose
 * stages stay out (a node as a helper). A mesh bound to nothing rides the
 * group's helper.
 *
 * @p options' choices say otherwise per element. Once any is given, an
 * animation brought comes as a new clip of the model off the timeline, its
 * events on the nodes they became; a slot, a material and a channel only what
 * is left behind used stay behind; and an event's global loop, numbered by the
 * donor, is dropped.
 *
 * Both models must be pivot rigs.
 */
MergeResult MergeModel(Document& into, u32 model, Document donor, u32 from, const MergeOptions& options = {});

} // namespace wem
} // namespace models
} // namespace whiteout

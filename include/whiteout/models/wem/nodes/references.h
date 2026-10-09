// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file references.h
 * @brief The §10.6 referencer table as one walk: every place a node index is
 *        stored outside the tree's own parent links.
 *
 * Renumbering a tree and asking what names a node are the same walk, and they
 * have to agree. A place the renumbering does not visit is left dangling; a
 * place the question does not visit is one whose node the optimize pass feels
 * free to remove, and the renumbering then clears it without a word. So both
 * go through here (`RemapNodeReferencers`, `RemapPhysicsNodes`, the pass's
 * `FactsOf`), and a structure that stores a node index adds a row here and
 * nowhere else.
 *
 * Each walk calls `f(NodeReference row, u32& node, u32 owner, u32 sub)`:
 * @p node is the stored index itself (const when the walked object is), or a
 * copy written back where the index is not stored as one. `kInvalidNode` is
 * visited like any value: several rows use it to say "none", and what a
 * renumbering does about one is the caller's answer, not the walk's.
 */

#include <span>
#include <type_traits>

#include <whiteout/common_types.h>

#include "../anim/channel.h"
#include "../anim/clip.h"
#include "../anim/pose_stage.h"
#include "../geometry/mesh.h"
#include "../physics/physics.h"
#include "node.h"
#include "tree.h"

namespace whiteout {
namespace models {
namespace wem {

/// Which row of the table an index was found in. `owner` and `sub` are what
/// the comment on each says, and `kInvalidIndex` where it names none.
enum class NodeReference : u8 {
    Influence,      ///< `SkinBinding::Influence::bone`. Owner: the mesh.
    RigidNode,      ///< `MeshSection::rigidNode`. Owner: the mesh; sub: the section.
    VisibilityGate, ///< A section's `kSectionVisibilityNode`. Owner: the mesh; sub: the section.
    Channel,        ///< A node channel's `target.node`. Owner: the channel's id.
    StageDriven,    ///< `PoseStage::driven`. Owner: the stage's place in the list.
    StageTarget,    ///< `PoseStage::targets`. Owner: as above.
    StageSource,    ///< `StageSource::node`; none for a Link's world source. Owner: as above.
    StageUp,        ///< `PoseStage::upNode`; none for world up. Owner: as above.
    PhysicsBody,    ///< `PhysicsBody::node`. Owner: the record's id, as for every physics row.
    ClothCollider,  ///< `ClothCollider::node`; none for the root.
    ClothBone,      ///< `ClothRecipe::bones`.
    FractureHelper, ///< `FractureRecipe::helper`.
    FractureField,  ///< `FractureRecipe::field`.
    FractureWholeGate,  ///< `FractureRecipe::wholeGate`.
    FracturePiecesGate, ///< `FractureRecipe::piecesGate`.
    FractureBone,       ///< `FractureRecipe::bones`.
    FractureSkinNode,   ///< `FractureRecipe::skinNodes`: its layers name them by place.
    NodeLink,       ///< A node's own links (`ForEachNodeLink`). Owner: the node.
    ClipEvent,      ///< `ClipEvent::node`. Owner: the clip, as the caller numbers it.
    ClipWorldCentre ///< `BakeWorldForce::centreNode`; none for the model's middle. Owner: the clip.
};

/// @p mesh's rows: its skin, and each section's rigid node and visibility gate.
template <class MeshT, class F>
void ForEachMeshNodeReference(MeshT& mesh, u32 index, F&& f) {
    for (auto& influence : mesh.skin.influences) {
        f(NodeReference::Influence, influence.bone, index, kInvalidIndex);
    }
    for (u32 s = 0; s < mesh.sections.size(); ++s) {
        auto& section = mesh.sections[s];
        // The gate is a number in the section's bag, where "always drawn" and
        // "none" are numbers too: only a node is a row.
        const i64 gate = section.native.value(kSectionVisibilityNode, -1);
        if (gate >= 0 && gate != kSectionAlwaysDrawn) {
            u32 node = static_cast<u32>(gate);
            f(NodeReference::VisibilityGate, node, index, s);
            if constexpr (!std::is_const_v<MeshT>) {
                if (node != static_cast<u32>(gate)) {
                    section.native.set(kSectionVisibilityNode,
                                       node == kInvalidNode ? kSectionAlwaysDrawn
                                                            : static_cast<i64>(node));
                }
            }
        }
        if (section.rigidNode.has_value()) {
            f(NodeReference::RigidNode, *section.rigidNode, index, s);
            if constexpr (!std::is_const_v<MeshT>) {
                if (*section.rigidNode == kInvalidNode) {
                    section.rigidNode.reset();
                }
            }
        }
    }
}

/// The node channels of @p channels.
template <class ChannelsT, class F>
void ForEachChannelNodeReference(ChannelsT& channels, F&& f) {
    for (auto& channel : channels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node) {
            f(NodeReference::Channel, channel.target.node, channel.id, kInvalidIndex);
        }
    }
}

/// Each pose stage's driven nodes, targets, sources and up node.
template <class StagesT, class F>
void ForEachStageNodeReference(StagesT& stages, F&& f) {
    for (u32 s = 0; s < stages.size(); ++s) {
        auto& stage = stages[s];
        for (auto& node : stage.driven) {
            f(NodeReference::StageDriven, node, s, kInvalidIndex);
        }
        for (auto& node : stage.targets) {
            f(NodeReference::StageTarget, node, s, kInvalidIndex);
        }
        for (auto& source : stage.sources) {
            f(NodeReference::StageSource, source.node, s, kInvalidIndex);
        }
        f(NodeReference::StageUp, stage.upNode, s, kInvalidIndex);
    }
}

/// @p physics's rows: what its bodies and colliders ride, a cloth's bones,
/// and everything a fracture names.
template <class PhysicsT, class F>
void ForEachPhysicsNodeReference(PhysicsT& physics, F&& f) {
    for (auto& body : physics.bodies) {
        f(NodeReference::PhysicsBody, body.node, body.id, kInvalidIndex);
    }
    for (auto& collider : physics.colliders) {
        f(NodeReference::ClothCollider, collider.node, collider.id, kInvalidIndex);
    }
    for (auto& cloth : physics.cloths) {
        if (!cloth.recipe) {
            continue;
        }
        for (auto& bone : cloth.recipe->bones) {
            f(NodeReference::ClothBone, bone, cloth.id, kInvalidIndex);
        }
    }
    for (auto& rig : physics.rigs) {
        if (!rig.fracture) {
            continue;
        }
        auto& fracture = *rig.fracture;
        f(NodeReference::FractureHelper, fracture.helper, rig.id, kInvalidIndex);
        f(NodeReference::FractureField, fracture.field, rig.id, kInvalidIndex);
        f(NodeReference::FractureWholeGate, fracture.wholeGate, rig.id, kInvalidIndex);
        f(NodeReference::FracturePiecesGate, fracture.piecesGate, rig.id, kInvalidIndex);
        for (auto& node : fracture.bones) {
            f(NodeReference::FractureBone, node, rig.id, kInvalidIndex);
        }
        for (auto& node : fracture.skinNodes) {
            f(NodeReference::FractureSkinNode, node, rig.id, kInvalidIndex);
        }
    }
}

/// The links each node of @p tree holds to another: an emitter's, the skin
/// setup's mirror, the rig record's rider.
template <class TreeT, class F>
void ForEachLinkNodeReference(TreeT& tree, F&& f) {
    for (u32 n = 0; n < tree.nodes.size(); ++n) {
        ForEachNodeLink(tree.nodes[n], [&](auto& link, EmitterLink) {
            f(NodeReference::NodeLink, link, n, kInvalidIndex);
        });
    }
}

/// @p clip's rows: where its events fire, and its winds' and blasts' centres.
template <class ClipT, class F>
void ForEachClipNodeReference(ClipT& clip, u32 index, F&& f) {
    for (auto& event : clip.events) {
        f(NodeReference::ClipEvent, event.node, index, kInvalidIndex);
    }
    if (clip.physics.has_value()) {
        for (auto& force : clip.physics->world) {
            f(NodeReference::ClipWorldCentre, force.centreNode, index, kInvalidIndex);
        }
    }
}

/**
 * @brief The whole table for one model: its meshes, channels, stages, physics,
 *        node links and the clips that drive it.
 *
 * Any part may be absent (an empty span, a null pointer). @p clips is the
 * caller's to filter by `Clip::model`: a tree does not know its own index.
 */
template <class TreeT, class MeshT, class ChannelsT, class StagesT, class PhysicsT, class ClipT, class F>
void ForEachNodeReference(TreeT& tree, std::span<MeshT> meshes, ChannelsT* channels,
                          StagesT* stages, PhysicsT* physics, std::span<ClipT> clips, F&& f) {
    for (u32 m = 0; m < meshes.size(); ++m) {
        ForEachMeshNodeReference(meshes[m], m, f);
    }
    if (channels != nullptr) {
        ForEachChannelNodeReference(*channels, f);
    }
    if (stages != nullptr) {
        ForEachStageNodeReference(*stages, f);
    }
    if (physics != nullptr) {
        ForEachPhysicsNodeReference(*physics, f);
    }
    ForEachLinkNodeReference(tree, f);
    for (u32 c = 0; c < clips.size(); ++c) {
        ForEachClipNodeReference(clips[c], c, f);
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout

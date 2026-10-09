// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file optimize.h
 * @brief Making a converted document smaller without changing what it draws
 *        (EDIT_MODE_OPTIMIZE_DESIGN.md).
 *
 * Meshes that draw alike become one, nodes that move nothing of their own are
 * replaced by their parent, and duplicate materials and textures become one.
 * The contract is that the result draws and animates exactly as the input did:
 * where a rule cannot be proved for a mesh or a node, it is left alone.
 *
 * Everything goes through the removal operations and their referencer tables
 * (`nodes/remove.h`, `meshes/remove.h`, `materials/ops.h`); nothing here
 * renumbers an index by hand.
 */

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "diagnostics.h"
#include "document.h"

namespace whiteout {
namespace models {
namespace wem {

struct OptimizeOptions {
    /// Merge meshes with the same material, flags, vertex layout and geoset
    /// animation (§3), and drop meshes with no face that nothing gates.
    bool mergeMeshes = true;
    /// Also merge meshes whose material blends. A translucent geoset sorts by
    /// its own centroid, so two merged ones sort as one (§3.3).
    bool mergeBlended = false;
    /// Replace unkeyed bones and helpers by their parents, and drop the ones
    /// that hold nothing (§4). A helper parent that takes a bone's weights
    /// becomes a bone.
    bool reduceNodes = true;
    /// Reduce nodes whatever game the document is. Off: only a Warcraft III
    /// document, since the other three bind animation from outside the model
    /// (§4.4).
    bool reduceNodesOfEveryGame = false;
    /// Nodes the node pass leaves whatever its rules say, by index, asked once
    /// per model before its nodes are reduced: what the caller still reads the
    /// document by. Unset keeps none.
    std::function<std::vector<u32>(const Document& document, u32 model)> keepNodes;
    /// The bones an animation outside the file binds by name, spelled as the
    /// game looks them up, for a model that names one (`FaceFxActorPath`).
    /// Asked once per such model. The pass then keeps each of them, the node
    /// above it, and the weights of anything under it where they are (§4.4).
    /// Unset, or no answer, keeps every node of that model: the actor's bones
    /// are not known, and losing one turns the whole actor off.
    std::function<std::optional<std::vector<std::string>>(const Document& document, u32 model)>
        boundBones;
    /// Merge duplicate textures and material slots, and drop unused ones.
    bool mergeMaterials = true;
    /// Remove the keys no frame can tell apart (`ReduceKeysExactly`), first,
    /// so the nodes and meshes they leave unkeyed or alike can go too.
    bool reduceKeys = true;
};

/// What happened to one model's indices.
struct ModelOptimizeReport {
    /// Per final mesh, the input meshes whose vertices it holds, in vertex
    /// order. Empty when no mesh changed.
    std::vector<std::vector<u32>> meshOrigins;
    /// Input node -> final node, `kInvalidNode` for one that went. Empty when
    /// no node changed.
    std::vector<u32> nodeRemap;
    /// The model names an actor (§4.4) and `boundBones` did not say what it
    /// binds, so the node pass left it alone.
    bool nodesKeptForActor = false;
    /// The nodes an actor binds, by `boundBones`' answer: kept.
    u32 boundNodes = 0;
};

struct OptimizeReport {
    u32 meshesMerged = 0;     ///< Meshes absorbed into another.
    u32 meshesRemoved = 0;    ///< Meshes with no face.
    u32 nodesRemoved = 0;     ///< Pass-throughs and dead leaves.
    /// Helpers that took a removed bone's weights and became bones.
    u32 helpersPromoted = 0;
    u32 slotsMerged = 0;      ///< Slots folded onto an identical one.
    u32 slotsRemoved = 0;     ///< Slots nothing used.
    u32 texturesMerged = 0;   ///< Textures folded onto an identical one.
    u32 texturesRemoved = 0;  ///< Textures nothing used.
    u32 keysRemoved = 0;      ///< Keys no frame can tell apart.
    u32 subTracksDropped = 0; ///< Sub-tracks that read nothing or played the rest.
    u32 tracksCollapsed = 0;  ///< Constant sub-tracks cut to one key.
    u32 channelsDropped = 0;  ///< Channel declarations nothing keys, and that state no rest.
    std::vector<ModelOptimizeReport> models; ///< Parallel to `Document::models`.
    /// An error means a step went wrong and was stopped; the document still
    /// validates.
    Diagnostics diagnostics;

    bool changed() const {
        return meshesMerged + meshesRemoved + nodesRemoved + slotsMerged + slotsRemoved +
                   texturesMerged + texturesRemoved + keysRemoved + channelsDropped !=
               0;
    }
};

/**
 * @brief The whole pass, in the design's order: keys, textures, material
 *        slots, nodes, empty meshes, mesh merges.
 */
OptimizeReport OptimizeDocument(Document& document, const OptimizeOptions& options = {});

/// Whether Warcraft III resolves @p name as one of its bones (`CBoneTokenizer`:
/// `bone_head`, `bone_chest`, `bone_foot`, `bone_hand`, `bone_turret`, as a
/// token of the name). Such a node is never reduced.
bool IsWarcraftEngineBoneName(const std::string& name);

} // namespace wem
} // namespace models
} // namespace whiteout

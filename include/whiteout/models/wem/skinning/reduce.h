// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file reduce.h
 * @brief Fewer bones for a skinned model (BONE_REDUCTION_RESEARCH.md §7.3).
 *
 * Leaf bones go one at a time, the cheapest first: each time, the weights of
 * every vertex the bone moved are solved again over the bones left, against
 * where every vertex went before anything went, in every clip. The bones kept
 * keep their animation; a parent becomes a leaf once its children have gone.
 */

#include <functional>
#include <utility>
#include <vector>

#include <whiteout/common_types.h>

#include "../document.h"
#include "../geometry/skin.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

struct BoneReduceOptions {
    u32 keep = 0;          ///< Stop with this many bones left; 0: the errors alone.
    f32 rmsShare = 0.0f;   ///< Of the model's height: no drop takes the RMS distance past it; 0: none.
    f32 worstShare = 0.0f; ///< Of the height: no vertex strays further in any frame; 0: none.
    u32 maxInfluences = 4;
    f32 rate = 30.0f;          ///< Samples a second over each clip.
    std::vector<u32> locked;   ///< Nodes that stay, beside those the model needs.
    u32 threads = 0;           ///< 0: one per hardware thread.
    std::function<bool()> cancelled;
};

struct BoneReducePlan {
    std::vector<u32> dropped; ///< Nodes, in the order they go.
    /// Per mesh: the vertices whose weights change, with their new ones.
    std::vector<std::vector<std::pair<u32, std::vector<geom::Influence>>>> weights;
    u32 bonesBefore = 0;
    u32 bonesAfter = 0;
    f32 rms = 0.0f;   ///< Model units, over every skinned vertex in every sampled frame.
    f32 worst = 0.0f; ///< The farthest any vertex strays.
    bool cancelled = false;
};

/// Per node of @p model: a bone the reduction may drop. Nothing but its own
/// weights and keys may name it, neither Warcraft III nor StarCraft II may
/// find it by name, and it carries no billboard or shadow box.
std::vector<u8> ReducibleBones(const Document& document, u32 model);

/// Which of @p model's bones go and how its vertices are weighted after, for
/// the caller to apply in one step. @p document is not changed.
BoneReducePlan PlanBoneReduction(const Document& document, u32 model, const BoneReduceOptions& options);

/// The tiers (BONE_REDUCTION_RESEARCH.md §7.4), @p tier 0 Low to 3 Ultra: the
/// vertices' RMS and farthest stray as shares of the model's height, stopping
/// at @p keep bones as well unless 0.
BoneReduceOptions BoneReduceTier(u32 tier, u32 keep = 0);

/// @p plan written into @p model of @p document: its vertices weighted anew,
/// then its bones taken out with their channels and the clip events and
/// forces on them renumbered. False when it drops none.
bool ApplyBoneReduction(Document& document, u32 model, const BoneReducePlan& plan);

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

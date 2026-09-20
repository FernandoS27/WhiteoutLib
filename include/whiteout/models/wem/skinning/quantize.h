// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file quantize.h
 * @brief What a file can hold of a vertex's float weights
 *        (EDIT_MODE_SKIN_DESIGN.md §12.1-12.2).
 *
 * Two formats, two answers, one rule behind both: **a format conversion is not a
 * modelling operation**. Each function changes a vertex only as far as its
 * format forces it to.
 *
 * - **Bytes that sum to 255** (`QuantizeWeights`): Reforged's `SKIN`, `.m2`,
 *   `.m3`. Largest remainder, so the sum is exact and the proportions are the
 *   nearest the bytes allow.
 * - **Equal sets** (`QuantizeClassic`): Warcraft III classic's matrix groups,
 *   which carry no weights at all. The game averages a group's bones equally, so
 *   a group of `k` bones can hold exactly one weight vector, `1/k` each. The
 *   rule is WhiteoutDex's (`MDLXExporter/src/assembly/mdx_skin_quantizer.cpp`),
 *   so a model exported from 3ds Max and from this library get the same groups.
 */

#include <array>
#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../geometry/skin.h"

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

/**
 * @brief @p count weights as the bytes a file stores: in proportion, and
 *        summing to exactly 255.
 *
 * Each weight takes `floor(w / total · 255)`, and what rounding down left goes
 * to the largest remainders, the earlier slot on a tie. The floor carries a
 * 1e-3 epsilon: a weight read from a byte, `b / 255`, can come back a hair under
 * `b` once multiplied again, and a plain floor would lose that unit to another
 * slot's remainder. All zero, or @p count 0, gives all zero.
 */
std::array<u8, 4> QuantizeWeights(const std::array<f32, 4>& weights, std::size_t count);

/// The classic format's limits (§12.2).
struct ClassicLimits {
    u32 maxBones = 8;    ///< The largest group written. Blizzard's SD art reaches 8.
    f32 prune = 0.02f;   ///< A share under this is dropped before the snap.
    u32 maxGroups = 256; ///< `GNDX` is a byte.
};

/// One geoset's classic skin: its groups and each vertex's.
struct ClassicSkin {
    /// Each a sorted set of the nodes it averages; never a repeat (Q2).
    std::vector<std::vector<u32>> groups;
    /// Per vertex of the geoset, its group. Empty when no vertex binds a node.
    std::vector<u32> groupOf;
    /// Vertices whose group differs from their weights by more than 2/255 on a
    /// bone.
    u32 snapped = 0;
    /// Vertices moved to another group by the 256 limit.
    u32 merged = 0;
    /// Which ones, ascending: an editor's Classic section selects them, and a
    /// count alone cannot be pointed at (EDIT_MODE_SKIN_DESIGN.md §12.6).
    std::vector<u32> mergedVertices;
    /// Groups merged away by the 256 limit.
    u32 mergedGroups = 0;
    /// Vertices whose shares outnumber `maxBones` once the bleed is pruned:
    /// the group keeps the heaviest, and the rest is the format's loss.
    u32 wide = 0;
    /// The pins alone need more than `maxGroups` groups; the geoset cannot be
    /// written as they ask.
    bool pinsOverflow = false;
};

/**
 * @brief The Skin Quantizer: the best equal-set approximation of one geoset's
 *        float weights (§12.2).
 *
 * @p vertices holds each geoset vertex's influences on the nodes the file
 * writes, unfolded, in any order. @p pins is per vertex, or empty for none: 0
 * for the automatic choice, else the group size `k` (§12.6).
 *
 * 1. **Gather.** Merge duplicate bones, normalise, drop each share under
 *    `prune`, normalise again, sort heaviest first (ties by node). A vertex left
 *    with nothing joins the geoset's first bone.
 * 2. **Snap.** Keeping the `k` heaviest at `1/k` costs `Σw² + (1 − 2·S_k)/k`
 *    in squared error, where `S_k` is their sum, so the snap takes the `k` in
 *    `1..min(n, maxBones)` minimising `(1 − 2·S_k)/k`; the smaller `k` within
 *    1e-6. Idempotent: an equal split over `m` bones snaps to those `m`. A
 *    pinned vertex keeps its `k` heaviest, or every bone it has if fewer.
 * 3. **At most `maxGroups` groups.** While more remain, the group with the
 *    fewest vertices (earliest on a tie, never one a pinned vertex uses) goes,
 *    and each of its vertices moves on its own to the surviving group nearest
 *    its own weights. The survivors are numbered once, at the end, in
 *    first-use order.
 */
ClassicSkin QuantizeClassic(std::span<const std::vector<geom::Influence>> vertices,
                            std::span<const u16> pins, const ClassicLimits& limits = {});

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

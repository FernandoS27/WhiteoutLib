// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file key_exact.h
 * @brief Tier 1 of the key reduction (EDIT_MODE_KEY_OPTIMIZE_DESIGN.md §3):
 *        the keys no frame can tell apart, which a conversion removes. Tier 2,
 *        within a tolerance, is `key_reduce.h`.
 */

#include <vector>

#include <whiteout/common_types.h>

#include "../document.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Tier 1: exact
// ============================================================================

struct ExactKeyReport {
    u32 keysRemoved = 0;      ///< Every key that went, dropped sub-tracks' included.
    u32 subTracksDropped = 0; ///< Sub-tracks that read nothing or played the rest.
    u32 tracksCollapsed = 0;  ///< Constant sub-tracks cut to their one key.
    u32 channelsDropped = 0;  ///< Declarations no clip keys and that state no rest.
};

/**
 * @brief Tier 1 (§3): per clip of @p model, in this order —
 *        1. keys no rule reads: outside a kept MDX window, or a key another
 *           takes the millisecond from;
 *        2. sub-tracks of an opaque layer that play, every millisecond, the
 *           rest the clip shows without them (Warcraft III and StarCraft II
 *           storage);
 *        3. constant sub-tracks, down to one key;
 *        4. keys the game's own curve reproduces between their neighbours,
 *           on a channel whose clips agree on the interpolation;
 *        5. and, of the model, the channels left with no sub-track in any clip
 *           and no declared rest: a target and a type, with nothing to play.
 *
 * Never touched (§3.2): trigger channels (a squirt), stage channels, sub-tracks
 * with TCB parameters, a sub-track's presence in a transparent layer, an empty
 * `Sc2` sub-track, the first and last key a rule reads, and the sub-track of
 * the clip whose clock a written track runs on (`ClockOwner`).
 */
ExactKeyReport ReduceKeysExactly(Document& document, u32 model);

/**
 * @brief Step 2 of `ReduceKeysExactly` alone: the sub-tracks that play, every
 *        millisecond, the rest their clip shows without them.
 *
 * What a file export runs on its own copy: a track that only restates its
 * record's static value is what deleting a clip, or the keys that moved, leaves
 * behind. Every other key is written as it stands.
 */
ExactKeyReport DropRestTracks(Document& document, u32 model);

/**
 * @brief The same, by the rules of @p storage, the game whose file the copy is
 *        written as: an `.m3` source staged for `.mdx` still says StarCraft II,
 *        and the two games play an unkeyed clip differently.
 */
ExactKeyReport DropRestTracks(Document& document, u32 model, Game storage);

/// The model's height, the unit the tolerance is given in: the vertical extent
/// of its meshes, or of its nodes when it has no mesh; at least 1.
f32 ModelHeight(const Model& model);

/// Keys in @p model's clips (@p clips; empty: all), every sub-track counted.
u64 CountKeys(const Document& document, u32 model, const std::vector<u32>& clips = {});

} // namespace wem
} // namespace models
} // namespace whiteout

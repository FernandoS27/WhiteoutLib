// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file key_reduce.h
 * @brief Fewer keys for the same animation (EDIT_MODE_KEY_OPTIMIZE_DESIGN.md).
 *
 * Every removal is judged by the question the design starts from: if this key
 * goes, how far does anything the model draws move, in any frame the game can
 * show? A track is read by its clip's rule (`track_read.h`) at whole
 * milliseconds, as the writers will write it.
 *
 * Two tiers. `ReduceKeysExactly` removes what no frame can tell apart, and
 * runs on conversion as part of `OptimizeDocument`. `ReduceKeys` removes more,
 * within a tolerance measured in model space, and only when asked.
 */

#include <functional>
#include <vector>

#include <whiteout/common_types.h>

#include "../diagnostics.h"
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
 *           on a channel whose clips agree on the interpolation.
 *
 * Never touched (§3.2): trigger channels (a squirt), stage channels, sub-tracks
 * with TCB parameters, a sub-track's presence in a transparent layer, an empty
 * `Sc2` sub-track, the first and last key a rule reads, and the sub-track of
 * the clip whose clock a written track runs on (`ClockOwner`).
 */
ExactKeyReport ReduceKeysExactly(Document& document, u32 model);

// ============================================================================
// Tier 2: within a tolerance
// ============================================================================

/// The model's height, the unit the tolerance is given in: the vertical extent
/// of its meshes, or of its nodes when it has no mesh; at least 1.
f32 ModelHeight(const Model& model);

/// What channels that move nothing in space may drift by (§4.1).
struct KeyCompanionTolerances {
    f32 colour = 0.5f / 255.0f; ///< Alpha and colour, absolute; 0 and 1 stay exact.
    f32 uv = 1.0f / 1024.0f;    ///< UV translation and scale, in UV units.
    f32 uvTurnDegrees = 0.1f;   ///< UV rotation.
    f32 relative = 0.005f;      ///< Emitter, light and other scalars: of the clip's peak.
};

struct KeyReduceOptions {
    /// ε in model units: nothing the model draws moves further. 0 means
    /// @ref heightShare of `ModelHeight`.
    f32 tolerance = 0.0f;
    /// ε as a share of the model's height, when @ref tolerance is 0: what an
    /// export asks for, since it measures the model after restating its units.
    f32 heightShare = 0.001f;
    /// The reach given a node that carries what the file does not describe —
    /// an attachment, an emitter, a camera, a light — as a share of the height.
    f32 floorShare = 0.10f;
    /// The share of ε a node Warcraft III names (`bone_head`, …) or an
    /// attachment point may use.
    f32 importantShare = 0.5f;

    enum class Search : u8 {
        Balanced, ///< A lazy global priority queue: the cheapest error first.
        Maximum,  ///< The fewest keys per track, by dynamic programming.
    };
    Search search = Search::Balanced;

    enum class Measure : u8 {
        Skeleton, ///< A bound on everything a node carries (§4.2).
        Mesh,     ///< The skinned extreme vertices of each node (§4.3).
    };
    Measure measure = Measure::Skeleton;

    /// Remove a clip time from every node channel at once first, so the keys
    /// that stay line up as poses (§4.5).
    bool alignPoses = false;
    /// Refit the tangents facing a removed span on a Hermite or Bezier track
    /// of vectors, rather than keep them as they stand.
    bool refitTangents = true;

    std::vector<u32> clips;    ///< Document clip indices; empty: every clip of the model.
    std::vector<u32> channels; ///< `AnimChannel::id`s; empty: every channel.
    KeyCompanionTolerances companions;
    /// Threads to spread the clips over; 0 picks the hardware's count.
    u32 threads = 0;
    /// Asked before each clip: true stops, and the clips not yet begun stay
    /// as they were.
    std::function<bool()> cancelled;
    /// Clips done of the clips in scope, one call at a time whichever thread
    /// finished one.
    std::function<void(u32 done, u32 total)> progress;
};

/// The error of one reduction, by the skinned mesh (§4.3).
struct KeyErrorRow {
    u32 index = kInvalidIndex; ///< A clip or a node.
    u64 keysBefore = 0;
    u64 keysAfter = 0;
    f32 maxError = 0.0f; ///< Model units.
    f32 rmsError = 0.0f; ///< Weighted by each vertex's share of face area.
};

struct KeyReduceReport {
    f32 tolerance = 0.0f; ///< The ε used, in model units.
    u64 keysBefore = 0;
    u64 keysAfter = 0;
    f32 maxError = 0.0f;
    f32 rmsError = 0.0f;
    std::vector<KeyErrorRow> clips; ///< Per clip in scope.
    std::vector<KeyErrorRow> nodes; ///< Per node that lost keys or moved.
    /// Clips the final check found over ε, re-reduced at ε/2 (§4.3).
    std::vector<u32> repaired;
    /// Clips left as they were: a repair that still failed, or a run
    /// cancelled before them.
    std::vector<u32> kept;
    /// Per mesh, per vertex: the farthest it moved in any clip measured.
    std::vector<std::vector<f32>> vertexError;
    Diagnostics diagnostics;
};

/**
 * @brief Tier 2 (§4): removes the keys whose absence moves nothing of @p model
 *        by more than ε in any frame, and says how far everything moved.
 *
 * Keys that no rule reads go first, as tier 1's step 1 does. The rules of
 * `ReduceKeysExactly`'s "never touched" list hold here too. Checks the result
 * against the skinned mesh at 120 Hz, and re-reduces a clip at ε/2 that the
 * check finds over ε.
 */
KeyReduceReport ReduceKeys(Document& document, u32 model, const KeyReduceOptions& options = {});

/**
 * @brief How far @p after's clips move @p model's vertices from @p before's,
 *        at 120 Hz (every millisecond for a clip under a second): the maximum
 *        and the area-weighted RMS, per clip and per node (a vertex counts for
 *        the bone that weighs most on it). The two documents must share the
 *        model's nodes, meshes and clips.
 */
KeyReduceReport MeasureKeyError(const Document& before, const Document& after, u32 model,
                                const std::vector<u32>& clips = {});

/// Keys in @p model's clips (@p clips; empty: all), every sub-track counted.
u64 CountKeys(const Document& document, u32 model, const std::vector<u32>& clips = {});

} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file crossing.h
 * @brief A clip moved to another game's read rule and storage, keys and all
 *        (WEM_ANIMATION_RUNTIME_DESIGN.md §3.7, §6.2, §6.3).
 *
 * One compare serves the three callers. A track is restated for the other
 * rule by reading it by its own at every key and edge, then comparing the two
 * reads at the quarter points of each span, on whole milliseconds, and adding
 * a key wherever they part by more than the tolerance — so a span the two
 * rules already agree on (a vector lerp, a step) costs nothing, and a wide
 * rotation, a wrap one rule plays and the other does not, or a smooth curve is
 * filled in until it plays the same. `ConvertClips` does that to the edited
 * document when its main profile changes game; `ResampleForTarget` does it to
 * an export's staged copy; `FlattenContainers` compares against the blend of a
 * clip's layers instead of one track.
 */

#include <functional>
#include <map>
#include <set>
#include <span>

#include <whiteout/common_types.h>

#include "../diagnostics.h"
#include "../document.h"
#include "animator.h"
#include "clip.h"

namespace whiteout {
namespace models {
namespace wem {

/// How far two reads may part and still count as the same motion.
struct CrossingTolerance {
    f32 rotation = 0.1f * 3.14159265f / 180.0f; ///< Radians of turn.
    /// A quaternion's length: StarCraft II builds a bone's basis from the raw
    /// lerp unnormalised, so a short quaternion shrinks the bone.
    f32 length = 1e-3f;
    f32 translation = 1e-3f; ///< Absolute; `TolerancesOf` scales it by the model.
    f32 relative = 1e-3f;    ///< Any other float, relative to its size (at least 1).
};

/// The design's tolerances for @p model: 0.1° of turn, 0.1% of the model's
/// height in translation, 0.1% of scale.
CrossingTolerance TolerancesOf(const Model& model);

/// @p track of @p clip restated so a clip of rule @p to reads what @p clip
/// reads, where @p rest is what either shows where it reads nothing. Its keys
/// sit on whole milliseconds inside the clip. @p resampled says whether keys
/// beyond the ones read at the source's keys and edges were needed.
SubTrack RestateTrack(const Clip& clip, const SubTrack& track, geom::AttrType type,
                      const TrackTarget& target, ReadRule to, const CrossingTolerance& tolerance,
                      std::span<const u8> rest, bool* resampled = nullptr);

/**
 * @brief Converts @p clips to rule @p to, stored as @p toStorage's format
 *        stores them, from what they showed under @p fromStorage's.
 *
 * Per clip: every track restated (`RestateTrack`; a smooth curve becomes the
 * lines that play it, an M3 track shorter than its clip is repeated across
 * it); `intervalStart`, `intervalEnd` and `globalSequenceId` dropped when
 * leaving Warcraft III, the clip flags kept; a global loop of no length keeps
 * one key a track. Then, over the whole batch because under MDX one clip's
 * keys decide another's rest, every channel a clip does not key and whose rest
 * differs between the two storages is keyed with what it showed, until
 * nothing changes. Entering Warcraft III, a channel whose clips disagree about
 * the controller is made linear, a step span as two keys a millisecond apart.
 * Layers and clocks are left as they are. Returns the clips converted;
 * reports `ClipRuleConverted` and `AnimTrackResampled`.
 */
u32 ConvertClips(Document& document, std::span<const u32> clips, ReadRule to, Game fromStorage,
                 Game toStorage, Diagnostics& diagnostics);

/// The same, from the main profile's storage to that of @p to's game.
u32 ConvertClips(Document& document, std::span<const u32> clips, ReadRule to,
                 Diagnostics& diagnostics);

/// Every clip of @p staged whose rule is not @p target's game's, converted to
/// it, and every rest keyed that differs between @p previewStorage (what the
/// editor showed) and @p target's storage: an export's crossing (§6.3). An
/// untouched document with no crossing comes back unchanged.
u32 ResampleForTarget(Document& staged, ProfileId target, Game previewStorage,
                      Diagnostics& diagnostics);

/// Every clip with more than one container baked into one, holding the
/// `Animator`'s pose of the clip alone at the union of its containers' keys,
/// resampled where the one track reads apart from the blend: for a format that
/// plays one layer (§6.2). Track sets over container 0 are left as they are,
/// since container 0 is already the whole pose. Returns the clips flattened;
/// reports `AnimLayersFlattened`.
u32 FlattenContainers(Document& document, Diagnostics& diagnostics);

/// Keys that play @p poseAt over @p view: every channel of @p seeds keyed at
/// each of its seed times and at both ends, then refined wherever the one
/// track, read by @p view's rule, parts from the pose at a span's quarter
/// points. @p poseAt gives every channel of @p model's table at a whole
/// millisecond, each one element of its value type, and is called once a
/// time. A discrete channel is keyed as a step. What `FlattenContainers` and
/// `BakeMix` bake with, for a host that composes its poses itself.
SubTrackContainer BakeSampled(const Model& model, const Clip& view, std::map<u32, std::set<i32>> seeds,
                              const std::function<std::vector<std::vector<u8>>(i32)>& poseAt);

/// *Bake mix as clip* (§6.2, §7.4): what @p mix shows over @p duration
/// seconds, as one clip named @p name of rule @p rule on @p model — every
/// channel a play keys, at each time a play reaches one of its keys, resampled
/// where the one track reads apart from the mix. Each play's `seconds` is its
/// time at the clip's start. The model's global loops are left out: they play
/// under the baked clip in game. Reports `AnimLayersFlattened`.
Clip BakeMix(const Document& document, u32 model, const Mix& mix, f32 duration, ReadRule rule,
             const std::string& name, Diagnostics& diagnostics);

} // namespace wem
} // namespace models
} // namespace whiteout

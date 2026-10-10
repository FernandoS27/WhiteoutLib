// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file track_read.h
 * @brief One track read by its clip's rule: bracket, interpolate, wrap
 *        (WEM_ANIMATION_RUNTIME_DESIGN.md §3.1).
 *
 * The `Wc3` read is the one the editor's sampler was held to the renderer by
 * (`anim_sample_test [a1]`): the keys `toMdx` writes of the clip, read through
 * `FindBracket` in the clip's window. `Sc2` is `M3LocateKey` and the raw lerp,
 * as `M3ModelAdapter` reads a block; `Wow` holds its ends and nlerps.
 */

#include <optional>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "clip.h"

namespace whiteout {
namespace models {
namespace wem {

/// @p seconds as whole milliseconds, rounded; 0 for anything not after 0.
u32 Milliseconds(f32 seconds);

/// @p clip's length in milliseconds.
u32 ClipMs(const Clip& clip);

/// Whether @p clip is a loop the model runs itself, on the world clock.
bool IsGlobalLoop(const Clip& clip);

/// The first of @p clip's containers' sub-tracks keying @p channelId, or the
/// first empty one when none keys it.
const SubTrack* FindSubTrack(const Clip& clip, u32 channelId);

/// Where a sub-track is read, in milliseconds on the clip's own timeline.
/// `Wc3` reads @ref timeMs in the window `[startMs, endMs]`; `Sc2` reads it
/// unwrapped and wraps a track at its own last key when @ref loop; `Wow` holds
/// at either end.
struct SampleWindow {
    i32 timeMs = 0;
    i32 startMs = 0;
    i32 endMs = 0;
    bool loop = true;
};

/// The window @p clip is played in: `[0, duration]` at @p ms for an animation;
/// for a global loop, `[0, duration]` at @p globalMs modulo the duration (the
/// clock unwrapped for `Sc2`, which wraps per track), and an unbounded window
/// at 0 for a loop of no length — `MdxHierarchy`'s `effTime`. A negative
/// @p globalMs reads a global loop at @p ms.
///
/// With @p holdEnd, a global loop the clock stands at the end of (a whole
/// number of its lengths past 0) is read at its end, where the modulo reads
/// its start: an editor's still clock on a loop's last frame. A `Sc2` loop
/// wraps per track and is read as before.
SampleWindow ClipWindow(const Clip& clip, u32 ms, i32 globalMs, bool holdEnd = false);

/// One element of @p valueType: @p track read by @p clip's rule at @p window,
/// or nothing where the rule reads no key — under `Wc3` no key inside the
/// window, and an empty track under any rule. With @p held, every span holds
/// its "from" key whatever the controller says. An integer is always held.
std::optional<std::vector<u8>> ReadTrack(const Clip& clip, const SubTrack& track,
                                         geom::AttrType valueType, const SampleWindow& window,
                                         bool held = false);

/// `ReadTrack`, or @p fallback where it reads nothing.
std::vector<u8> SampleSubTrack(const Clip& clip, const SubTrack& track, geom::AttrType valueType,
                               const SampleWindow& window, std::span<const u8> fallback,
                               bool held = false);

/// @p track's value at each of @p timesMs, read as `SampleSubTrack` reads one
/// in the window `ClipWindow` gives each time with no global clock — a global
/// loop at the times themselves — and the keys prepared once for the batch.
/// One element of @p valueType per time, back to back. @p holdEnd is
/// `ClipWindow`'s: a global loop's own timeline, its last frame included.
std::vector<u8> SampleSubTrackBatch(const Clip& clip, const SubTrack& track,
                                    geom::AttrType valueType, std::span<const i32> timesMs,
                                    std::span<const u8> fallback, bool held = false,
                                    bool holdEnd = false);

Vector3f SampleVec3(const Clip& clip, const SubTrack& track, const SampleWindow& window,
                    const Vector3f& fallback);
Quaternion SampleQuat(const Clip& clip, const SubTrack& track, const SampleWindow& window,
                      const Quaternion& fallback);

} // namespace wem
} // namespace models
} // namespace whiteout

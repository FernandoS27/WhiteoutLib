// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_track_slicer.h
 * @brief One `.mdx` track cut into the clips that play it.
 *
 * MDX has no per-sequence tracks: every track spans one global timeline and a
 * sequence is a window onto it (`mdx_anim.h`). The import cuts each track into
 * its windows; the effect crossing (`cross/mdx_m3_effects`) cuts an emitter's
 * property tracks -- which WEM does not hold -- into the SAME windows, so a
 * PAR_'s emission rate and the bone it rides agree key for key. One cut, two
 * callers (WC3_TO_SC2_COMPLETION_PLAN.md §2.1).
 */

#include <string>
#include <vector>

#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/mdx/types.h>
#include <whiteout/models/wem/anim/clip.h>
#include <whiteout/models/wem/document.h>

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_slice {

constexpr u32 kNoGlobalSequence = mdx::Track<f32>::kNoGlobalSequence;

/// MDX counts in milliseconds; WEM counts in seconds. A division, not a
/// multiply by 1e-3: `1500 * 0.001f` is 1.5000001 and `1500 / 1000.0f` is
/// exactly 1.5, and a key at a round frame should land on a round second.
constexpr f32 Seconds(f32 milliseconds) {
    return milliseconds / 1000.0f;
}

template <class T>
struct ValueTrait;
template <>
struct ValueTrait<f32> {
    static constexpr geom::AttrType kType = geom::AttrType::F32;
};
template <>
struct ValueTrait<u32> {
    static constexpr geom::AttrType kType = geom::AttrType::U32;
};
template <>
struct ValueTrait<Vector3f> {
    static constexpr geom::AttrType kType = geom::AttrType::F32x3;
};
template <>
struct ValueTrait<Quaternion> {
    static constexpr geom::AttrType kType = geom::AttrType::Quat;
};

/// MDX's `Linear` over a quaternion **is** a shortest-arc slerp in the engine,
/// so the two are the same statement and WEM says the more specific one.
inline Interpolation InterpOf(mdx::InterpolationType type, geom::AttrType valueType) {
    switch (type) {
    case mdx::InterpolationType::None:
        return Interpolation::Step;
    case mdx::InterpolationType::Linear:
        return valueType == geom::AttrType::Quat ? Interpolation::Slerp : Interpolation::Linear;
    case mdx::InterpolationType::Hermite:
        return Interpolation::Hermite;
    case mdx::InterpolationType::Bezier:
        return Interpolation::Bezier;
    }
    return Interpolation::Linear;
}

/// One sequence's window on the global timeline, and the clip it became.
struct Window {
    f32 start = 0; ///< Milliseconds.
    f32 end = 0;
    u32 clip = kInvalidIndex;
};

/// Whether @p track holds a value (and its tangents) for every key it times.
template <class T>
bool WellFormed(const mdx::Track<T>& track) {
    const u32 perKey = ValuesPerKey(InterpOf(track.interpolationType, ValueTrait<T>::kType));
    return track.keys_data.size() >= track.timestamps.size() * perKey;
}

template <class T>
void AppendKey(const mdx::Track<T>& track, std::size_t key, u32 perKey, std::vector<u8>& values) {
    const std::size_t base = key * perKey;
    for (u32 v = 0; v < perKey; ++v) {
        const T& value = track.keys_data[base + v];
        const u8* bytes = reinterpret_cast<const u8*>(&value);
        values.insert(values.end(), bytes, bytes + sizeof(T));
    }
}

/// The window's keys plus the two that bracket it, rebased to the window's
/// start: WC3 interpolates across a window's edges, and a slice that dropped
/// the neighbours would flatten the first and last spans to a hold. False when
/// no key falls in or around the window.
template <class T>
bool SliceWindow(const mdx::Track<T>& track, u32 channel, const Window& window, SubTrack& out) {
    const Interpolation interp = InterpOf(track.interpolationType, ValueTrait<T>::kType);
    const u32 perKey = ValuesPerKey(interp);
    const std::vector<u32>& times = track.timestamps;

    std::size_t first = 0;
    while (first < times.size() && static_cast<f32>(times[first]) < window.start) {
        ++first;
    }
    std::size_t last = first;
    while (last < times.size() && static_cast<f32>(times[last]) <= window.end) {
        ++last;
    }
    // One before and one after, so the spans at both edges interpolate the
    // way the global track does.
    const std::size_t lo = first > 0 ? first - 1 : first;
    const std::size_t hi = last < times.size() ? last + 1 : last;
    if (lo >= hi) {
        return false;
    }

    out = SubTrack{};
    out.channel = channel;
    out.interp = interp;
    for (std::size_t k = lo; k < hi; ++k) {
        out.times.push_back(Seconds(static_cast<f32>(times[k]) - window.start));
        AppendKey(track, k, perKey, out.values);
    }
    return true;
}

/// A global-sequence track whole: it runs on its own clock, so every key is
/// the auto-play clip's.
template <class T>
SubTrack WholeTrack(const mdx::Track<T>& track, u32 channel) {
    const Interpolation interp = InterpOf(track.interpolationType, ValueTrait<T>::kType);
    const u32 perKey = ValuesPerKey(interp);
    SubTrack out;
    out.channel = channel;
    out.interp = interp;
    for (std::size_t k = 0; k < track.timestamps.size(); ++k) {
        out.times.push_back(Seconds(static_cast<f32>(track.timestamps[k])));
        AppendKey(track, k, perKey, out.values);
    }
    return out;
}

/// One clip's cut of a track.
struct ClipCut {
    u32 clip = kInvalidIndex;
    SubTrack track;
};

/// @p track cut into the clips of @p document that model @p model plays, read
/// off the windows the import stamped on them: `intervalStart`/`intervalEnd`
/// for a sequence, `globalSequenceId` for a global sequence's auto-play clip.
/// A clip that came from anywhere else plays nothing of an `.mdx` track.
/// Empty for an unused or malformed track, and for a global sequence no clip
/// carries.
template <class T>
std::vector<ClipCut> CutForClips(const mdx::Track<T>& track, const Document& document, u32 model,
                                 u32 channel = 0) {
    std::vector<ClipCut> cuts;
    if (!track.isUsed || track.timestamps.empty() || !WellFormed(track)) {
        return cuts;
    }
    for (std::size_t c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != model) {
            continue;
        }
        if (track.globalSequenceId != kNoGlobalSequence) {
            if (clip.native.value("globalSequenceId", -1) ==
                static_cast<i64>(track.globalSequenceId)) {
                cuts.push_back({static_cast<u32>(c), WholeTrack(track, channel)});
            }
            continue;
        }
        const i64 start = clip.native.value("intervalStart", -1);
        const i64 end = clip.native.value("intervalEnd", -1);
        if (start < 0 || end < start) {
            continue;
        }
        Window window;
        window.start = static_cast<f32>(start);
        window.end = static_cast<f32>(end);
        window.clip = static_cast<u32>(c);
        ClipCut cut;
        cut.clip = window.clip;
        if (SliceWindow(track, channel, window, cut.track)) {
            cuts.push_back(std::move(cut));
        }
    }
    return cuts;
}

} // namespace mdx_slice
} // namespace wem
} // namespace models
} // namespace whiteout

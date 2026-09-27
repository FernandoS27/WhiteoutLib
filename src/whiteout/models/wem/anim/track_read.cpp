// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/track_read.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

#include "prepared_track.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using prepared::Bytes;
using prepared::Evaluate;
using prepared::Load;
using prepared::Prepare;
using prepared::PreparedTrack;

template <class T>
std::optional<std::vector<u8>> ReadTyped(const Clip& clip, const SubTrack& track, bool quat,
                                         const SampleWindow& window, bool held) {
    const std::optional<T> value = Evaluate(Prepare<T>(clip, track, quat), window, held);
    if (!value)
        return std::nullopt;
    return Bytes(*value);
}

template <class T>
std::vector<u8> ReadBatch(const Clip& clip, const SubTrack& track, bool quat,
                          std::span<const i32> timesMs, const T& fallback, bool held) {
    const PreparedTrack<T> prepared = Prepare<T>(clip, track, quat);
    std::vector<u8> out(timesMs.size() * sizeof(T));
    for (std::size_t i = 0; i < timesMs.size(); ++i) {
        const T value =
            Evaluate(prepared, ClipWindow(clip, static_cast<u32>(std::max(timesMs[i], 0)), -1),
                     held)
                .value_or(fallback);
        std::memcpy(out.data() + i * sizeof(T), &value, sizeof(T));
    }
    return out;
}

} // namespace

u32 Milliseconds(f32 seconds) {
    const f32 ms = seconds * 1000.0f;
    return ms <= 0.0f ? 0u : static_cast<u32>(ms + 0.5f);
}

u32 ClipMs(const Clip& clip) {
    return Milliseconds(clip.duration);
}

bool IsGlobalLoop(const Clip& clip) {
    return hasFlag(clip.flags, ClipFlags::AutoPlay) && hasFlag(clip.flags, ClipFlags::WorldClocked);
}

const SubTrack* FindSubTrack(const Clip& clip, u32 channelId) {
    // An M3 import keeps a bound block with no keys, and one in the base must
    // not hide another container's keys.
    const SubTrack* empty = nullptr;
    for (const SubTrackContainer& container : clip.containers) {
        if (const SubTrack* track = container.find(channelId)) {
            if (!track->times.empty())
                return track;
            if (empty == nullptr)
                empty = track;
        }
    }
    return empty;
}

SampleWindow ClipWindow(const Clip& clip, u32 ms, i32 globalMs) {
    SampleWindow window;
    window.loop = clip.looping;
    if (IsGlobalLoop(clip)) {
        const u32 duration = ClipMs(clip);
        window.loop = true;
        if (duration == 0) {
            window.endMs = 0x3FFFFFFF;
            return window;
        }
        const i32 clock = globalMs >= 0 ? globalMs : static_cast<i32>(ms);
        window.endMs = static_cast<i32>(duration);
        // M3 wraps each track at its own last key, so the clock stays whole.
        window.timeMs = clip.readRule == ReadRule::Sc2
                            ? clock
                            : static_cast<i32>(std::fmod(static_cast<f32>(clock),
                                                         static_cast<f32>(duration)));
        return window;
    }
    // The sequence's window: the stored one, or the one the export gives it.
    const i64 start = clip.native.value("intervalStart", -1);
    const i64 end = clip.native.value("intervalEnd", -1);
    window.endMs = start >= 0 && end >= start ? static_cast<i32>(end - start)
                                              : static_cast<i32>(ClipMs(clip));
    window.timeMs = static_cast<i32>(ms);
    return window;
}

std::optional<std::vector<u8>> ReadTrack(const Clip& clip, const SubTrack& track,
                                         geom::AttrType valueType, const SampleWindow& window,
                                         bool held) {
    switch (valueType) {
    case geom::AttrType::F32:
        return ReadTyped<f32>(clip, track, false, window, held);
    case geom::AttrType::F32x2:
        return ReadTyped<Vector2f>(clip, track, false, window, held);
    case geom::AttrType::F32x3:
        return ReadTyped<Vector3f>(clip, track, false, window, held);
    case geom::AttrType::F32x4:
        return ReadTyped<Vector4f>(clip, track, false, window, held);
    case geom::AttrType::Quat:
        return ReadTyped<Quaternion>(clip, track, true, window, held);
    case geom::AttrType::U32:
        return ReadTyped<u32>(clip, track, false, window, held);
    default:
        return std::nullopt;
    }
}

std::vector<u8> SampleSubTrack(const Clip& clip, const SubTrack& track, geom::AttrType valueType,
                               const SampleWindow& window, std::span<const u8> fallback,
                               bool held) {
    const std::vector<u8> otherwise(fallback.begin(), fallback.end());
    if (fallback.size() != geom::AttrTypeSize(valueType))
        return otherwise;
    std::optional<std::vector<u8>> value = ReadTrack(clip, track, valueType, window, held);
    return value ? std::move(*value) : otherwise;
}

std::vector<u8> SampleSubTrackBatch(const Clip& clip, const SubTrack& track,
                                    geom::AttrType valueType, std::span<const i32> timesMs,
                                    std::span<const u8> fallback, bool held) {
    if (fallback.size() != geom::AttrTypeSize(valueType))
        return {};
    switch (valueType) {
    case geom::AttrType::F32:
        return ReadBatch<f32>(clip, track, false, timesMs, Load<f32>(fallback.data()), held);
    case geom::AttrType::F32x2:
        return ReadBatch<Vector2f>(clip, track, false, timesMs, Load<Vector2f>(fallback.data()),
                                   held);
    case geom::AttrType::F32x3:
        return ReadBatch<Vector3f>(clip, track, false, timesMs, Load<Vector3f>(fallback.data()),
                                   held);
    case geom::AttrType::F32x4:
        return ReadBatch<Vector4f>(clip, track, false, timesMs, Load<Vector4f>(fallback.data()),
                                   held);
    case geom::AttrType::Quat:
        return ReadBatch<Quaternion>(clip, track, true, timesMs, Load<Quaternion>(fallback.data()),
                                     held);
    case geom::AttrType::U32:
        return ReadBatch<u32>(clip, track, false, timesMs, Load<u32>(fallback.data()), held);
    default: {
        std::vector<u8> out;
        for (std::size_t i = 0; i < timesMs.size(); ++i)
            out.insert(out.end(), fallback.begin(), fallback.end());
        return out;
    }
    }
}

Vector3f SampleVec3(const Clip& clip, const SubTrack& track, const SampleWindow& window,
                    const Vector3f& fallback) {
    return Evaluate(Prepare<Vector3f>(clip, track, false), window, false).value_or(fallback);
}

Quaternion SampleQuat(const Clip& clip, const SubTrack& track, const SampleWindow& window,
                      const Quaternion& fallback) {
    return Evaluate(Prepare<Quaternion>(clip, track, true), window, false).value_or(fallback);
}

} // namespace wem
} // namespace models
} // namespace whiteout

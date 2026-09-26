// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/track_read.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

#include "whiteout/models/wem/anim/curves.h"
#include "whiteout/models/wem/anim/m3_math.h"
#include "whiteout/models/wem/anim/mdx_math.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

template <class T>
T Load(const u8* at) {
    T value{};
    std::memcpy(&value, at, sizeof(T));
    return value;
}

template <class T>
std::vector<u8> Bytes(const T& value) {
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

// ---- The curves between two keys, per value type, as `EvaluateTrack*` has them ----

f32 Linear(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}
Vector2f Linear(const Vector2f& a, const Vector2f& b, f32 t) {
    return Vector2f::lerp(a, b, t);
}
Vector3f Linear(const Vector3f& a, const Vector3f& b, f32 t) {
    return Vector3f::lerp(a, b, t);
}
Vector4f Linear(const Vector4f& a, const Vector4f& b, f32 t) {
    return Vector4f::lerp(a, b, t);
}
Quaternion Linear(const Quaternion& a, const Quaternion& b, f32 t) {
    return Wc3Slerp(a, b, t);
}

f32 Curve(Interpolation interp, f32 a, f32 outA, f32 inB, f32 b, f32 t) {
    return interp == Interpolation::Hermite ? HermiteInterp(a, outA, inB, b, t)
                                            : BezierInterp(a, outA, inB, b, t);
}
Vector2f Curve(Interpolation interp, const Vector2f& a, const Vector2f& outA, const Vector2f& inB,
               const Vector2f& b, f32 t) {
    return {Curve(interp, a.x, outA.x, inB.x, b.x, t), Curve(interp, a.y, outA.y, inB.y, b.y, t)};
}
Vector3f Curve(Interpolation interp, const Vector3f& a, const Vector3f& outA, const Vector3f& inB,
               const Vector3f& b, f32 t) {
    return {Curve(interp, a.x, outA.x, inB.x, b.x, t), Curve(interp, a.y, outA.y, inB.y, b.y, t),
            Curve(interp, a.z, outA.z, inB.z, b.z, t)};
}
Vector4f Curve(Interpolation interp, const Vector4f& a, const Vector4f& outA, const Vector4f& inB,
               const Vector4f& b, f32 t) {
    return {Curve(interp, a.x, outA.x, inB.x, b.x, t), Curve(interp, a.y, outA.y, inB.y, b.y, t),
            Curve(interp, a.z, outA.z, inB.z, b.z, t), Curve(interp, a.w, outA.w, inB.w, b.w, t)};
}
/// Hermite and Bezier alike: the engine plays both as squad.
Quaternion Curve(Interpolation, const Quaternion& a, const Quaternion& outA, const Quaternion& inB,
                 const Quaternion& b, f32 t) {
    return Wc3Squad(a, outA, inB, b, t);
}

// ---- What `toMdx` writes for one sub-track -------------------------------------

template <class T>
struct Key {
    i32 ms = 0;
    T value{};
    T in{};
    T out{};
};

/// A clip's value at @p time (seconds) the way the export reads a source that
/// is not an `.mdx` — its `SampleValue` (mdx_anim.cpp): the first key held
/// before it and the last past it, stepped or lerped between, a quaternion the
/// short way round and normalised. On f32 components, as the export computes.
template <class T>
T EdgeValue(const SubTrack& track, const std::vector<T>& values, bool quat, f32 time) {
    std::size_t after = 0;
    while (after < track.times.size() && track.times[after] <= time)
        ++after;
    if (after == 0)
        return values.front();
    const std::size_t before = after - 1;
    // An integer is held between keys whatever it says, as the export holds it.
    if (after >= track.times.size() || track.interp == Interpolation::Step ||
        std::is_same_v<T, u32>)
        return values[before];
    const f32 span = track.times[after] - track.times[before];
    const f32 alpha = span > 0.0f ? (time - track.times[before]) / span : 0.0f;
    constexpr std::size_t kCount = std::max<std::size_t>(sizeof(T) / sizeof(f32), 1);
    f32 a[kCount], b[kCount], v[kCount];
    std::memcpy(a, &values[before], sizeof(T));
    std::memcpy(b, &values[after], sizeof(T));
    if (!quat) {
        for (std::size_t i = 0; i < kCount; ++i)
            v[i] = a[i] + (b[i] - a[i]) * alpha;
    } else {
        f32 dot = 0.0f;
        for (std::size_t i = 0; i < kCount; ++i)
            dot += a[i] * b[i];
        const f32 sign = dot < 0.0f ? -1.0f : 1.0f;
        f32 length = 0.0f;
        for (std::size_t i = 0; i < kCount; ++i) {
            v[i] = a[i] + (sign * b[i] - a[i]) * alpha;
            length += v[i] * v[i];
        }
        length = std::sqrt(length);
        for (std::size_t i = 0; i < kCount; ++i)
            v[i] = length > 0.0f ? v[i] / length : v[i];
    }
    T out{};
    std::memcpy(&out, v, sizeof(T));
    return out;
}

/// The keys the export writes for @p track in @p clip, on the clip's own
/// timeline in milliseconds (mdx_anim.cpp's `gather`, for one part).
///
/// A clip that keeps its window has its keys written as they stand, bracket
/// keys and all. Any other gets the keys inside `[0, endMs]` plus a made-up key
/// at each edge it does not key, holding what it shows there; on a tangent
/// stream that key is flat, and so is the side of its neighbour facing it.
template <class T>
std::vector<Key<T>> ExportedKeys(const Clip& clip, const SubTrack& track, bool quat, i32 endMs) {
    const u32 perKey = ValuesPerKey(track.interp);
    const std::size_t stride = perKey * sizeof(T);
    std::vector<T> values(track.times.size());
    std::vector<Key<T>> raw(track.times.size());
    for (std::size_t k = 0; k < track.times.size(); ++k) {
        const u8* at = track.values.data() + k * stride;
        raw[k].value = Load<T>(at);
        raw[k].in = perKey == 3 ? Load<T>(at + sizeof(T)) : raw[k].value;
        raw[k].out = perKey == 3 ? Load<T>(at + 2 * sizeof(T)) : raw[k].value;
        values[k] = raw[k].value;
    }

    std::vector<Key<T>> keys;
    if (KeepsWindow(clip)) {
        const i64 stored = clip.native.value("intervalStart", -1);
        const i64 start = IsGlobalLoop(clip) || stored < 0 ? 0 : stored;
        for (std::size_t k = 0; k < raw.size(); ++k) {
            const f32 absolute = track.times[k] * 1000.0f + static_cast<f32>(start);
            const i64 ms =
                absolute <= 0.0f ? 0 : static_cast<i64>(static_cast<u32>(absolute + 0.5f));
            Key<T> key = raw[k];
            key.ms = static_cast<i32>(ms - start);
            keys.push_back(key);
        }
    } else {
        const u32 end = static_cast<u32>(std::max(endMs, 0));
        bool keyedStart = false;
        bool keyedEnd = false;
        std::vector<std::size_t> inside;
        std::vector<i32> insideMs;
        for (std::size_t k = 0; k < raw.size(); ++k) {
            const f32 t = track.times[k];
            if (t < -1e-4f || t > clip.duration + 1e-4f)
                continue;
            const u32 time =
                std::clamp(static_cast<u32>(std::max(t, 0.0f) * 1000.0f + 0.5f), 0u, end);
            keyedStart = keyedStart || time == 0;
            keyedEnd = keyedEnd || time == end;
            inside.push_back(k);
            insideMs.push_back(static_cast<i32>(time));
        }
        const bool smooth = perKey == 3;
        const bool derivative = track.interp == Interpolation::Hermite && !quat;
        const auto flat = [&](const T& value) { return derivative ? T{} : value; };
        for (std::size_t i = 0; i < inside.size(); ++i) {
            Key<T> key = raw[inside[i]];
            key.ms = insideMs[i];
            if (smooth && !keyedStart && i == 0)
                key.in = flat(key.value);
            if (smooth && !keyedEnd && i + 1 == inside.size())
                key.out = flat(key.value);
            keys.push_back(key);
        }
        const auto edge = [&](i32 ms, f32 t) {
            Key<T> key;
            key.ms = ms;
            key.value = EdgeValue(track, values, quat, t);
            key.in = key.out = smooth ? flat(key.value) : key.value;
            keys.push_back(key);
        };
        if (!keyedStart && !values.empty())
            edge(0, 0.0f);
        if (!keyedEnd && !values.empty())
            edge(static_cast<i32>(end), clip.duration);
    }
    // One key per millisecond, the first claim winning, as the export merges.
    std::stable_sort(keys.begin(), keys.end(),
                     [](const Key<T>& a, const Key<T>& b) { return a.ms < b.ms; });
    keys.erase(std::unique(keys.begin(), keys.end(),
                           [](const Key<T>& a, const Key<T>& b) { return a.ms == b.ms; }),
               keys.end());
    return keys;
}

/// The keys of one sub-track as its rule reads them, built once and read at as
/// many times as a caller likes: the exported keys for `Wc3`, the stored ones
/// (M3's stamps for `Sc2`) otherwise.
template <class T>
struct PreparedTrack {
    std::vector<Key<T>> keys;
    std::vector<i32> times;
    Interpolation interp = Interpolation::Linear;
    ReadRule rule = ReadRule::Wc3;
    i32 origin = 0; ///< `Sc2`: the sequence's start frame, which the stamps carry.
    bool valid = false;
};

template <class T>
PreparedTrack<T> Prepare(const Clip& clip, const SubTrack& track, bool quat) {
    PreparedTrack<T> out;
    out.interp = track.interp;
    out.rule = clip.readRule;
    const u32 perKey = ValuesPerKey(track.interp);
    if (track.times.empty() || track.values.size() != track.times.size() * perKey * sizeof(T))
        return out;
    if (clip.readRule == ReadRule::Wc3) {
        // The export places a windowless clip's edges at its own length,
        // whatever window it is later read in.
        out.keys = ExportedKeys<T>(clip, track, quat, static_cast<i32>(ClipMs(clip)));
    } else {
        // As M3 stamps a key: the sequence's start frame plus its offset.
        out.origin =
            clip.readRule == ReadRule::Sc2 ? static_cast<i32>(clip.native.value("startFrame", 0)) : 0;
        out.keys.resize(track.times.size());
        const std::size_t stride = perKey * sizeof(T);
        for (std::size_t k = 0; k < track.times.size(); ++k) {
            const u8* at = track.values.data() + k * stride;
            Key<T>& key = out.keys[k];
            key.ms = out.origin + static_cast<i32>(std::lround(track.times[k] * 1000.0f));
            key.value = Load<T>(at);
            key.in = perKey == 3 ? Load<T>(at + sizeof(T)) : key.value;
            key.out = perKey == 3 ? Load<T>(at + 2 * sizeof(T)) : key.value;
        }
    }
    out.times.resize(out.keys.size());
    for (std::size_t k = 0; k < out.keys.size(); ++k)
        out.times[k] = out.keys[k].ms;
    out.valid = true;
    return out;
}

/// `Wc3`: read as `EvaluateTrackImpl` reads a track, and an integer the way
/// `EvaluateTrackU32` holds one. With @p held, the "from" key whatever the
/// controller: the renderer's `forceNoInterp`.
template <class T>
std::optional<T> EvaluateWc3(const PreparedTrack<T>& track, const SampleWindow& window, bool held) {
    const KeyBracket br = FindBracket(track.times.data(), static_cast<i32>(track.times.size()),
                                      window.timeMs, window.startMs, window.endMs);
    if (br.lo < 0)
        return std::nullopt;
    const Key<T>& a = track.keys[static_cast<std::size_t>(br.lo)];
    const Key<T>& b = track.keys[static_cast<std::size_t>(br.hi)];
    if constexpr (std::is_same_v<T, u32>) {
        return a.value;
    } else {
        if (held)
            return a.value;
        const Interpolation interp = track.interp;
        if (interp == Interpolation::Hermite || interp == Interpolation::Bezier) {
            if (br.lo == br.hi)
                return a.value;
            return Curve(interp, a.value, a.out, b.in, b.value, br.t);
        }
        if (br.lo == br.hi || interp == Interpolation::Step)
            return a.value;
        return Linear(a.value, b.value, br.t);
    }
}

/// `Sc2`: `M3LocateKey` on the stamps, wrapping at the track's own last key,
/// and the raw componentwise lerp `M3Anim_EvalTrackQuat` does — no normalise,
/// no sign fix. A stream has no tangents, so a smooth track reads its values.
template <class T>
std::optional<T> EvaluateSc2(const PreparedTrack<T>& track, const SampleWindow& window, bool held) {
    const bool interpolate =
        !held && track.interp != Interpolation::Step && !std::is_same_v<T, u32>;
    const M3KeySpan span =
        M3LocateKey(track.times, track.origin + window.timeMs, window.loop, interpolate);
    if (!span.valid)
        return std::nullopt;
    const std::size_t last = track.keys.size() - 1;
    const T& a = track.keys[std::min(span.i0, last)].value;
    const T& b = track.keys[std::min(span.i1, last)].value;
    if (span.i0 == span.i1)
        return a;
    if constexpr (std::is_same_v<T, Quaternion>) {
        return M3LerpQuatRaw(a, b, span.frac);
    } else if constexpr (std::is_same_v<T, u32>) {
        return a;
    } else {
        return Linear(a, b, span.frac);
    }
}

/// `Wow`: the clip's own span, each end held; a quaternion nlerped without a
/// sign flip.
template <class T>
std::optional<T> EvaluateWow(const PreparedTrack<T>& track, const SampleWindow& window, bool held) {
    const std::size_t count = track.times.size();
    std::size_t after = 0;
    while (after < count && track.times[after] <= window.timeMs)
        ++after;
    if (after == 0)
        return track.keys.front().value;
    const Key<T>& a = track.keys[after - 1];
    if (after >= count || held || track.interp == Interpolation::Step || std::is_same_v<T, u32>)
        return a.value;
    const Key<T>& b = track.keys[after];
    const i32 span = track.times[after] - track.times[after - 1];
    const f32 t = span > 0 ? static_cast<f32>(window.timeMs - track.times[after - 1]) /
                                 static_cast<f32>(span)
                           : 0.0f;
    if constexpr (std::is_same_v<T, Quaternion>) {
        return Quaternion(a.value.x + (b.value.x - a.value.x) * t,
                          a.value.y + (b.value.y - a.value.y) * t,
                          a.value.z + (b.value.z - a.value.z) * t,
                          a.value.w + (b.value.w - a.value.w) * t)
            .normalized();
    } else if constexpr (std::is_same_v<T, u32>) {
        return a.value;
    } else {
        if (track.interp == Interpolation::Hermite || track.interp == Interpolation::Bezier)
            return Curve(track.interp, a.value, a.out, b.in, b.value, t);
        return Linear(a.value, b.value, t);
    }
}

template <class T>
std::optional<T> Evaluate(const PreparedTrack<T>& track, const SampleWindow& window, bool held) {
    if (!track.valid)
        return std::nullopt;
    switch (track.rule) {
    case ReadRule::Sc2:
        return EvaluateSc2(track, window, held);
    case ReadRule::Wow:
        return EvaluateWow(track, window, held);
    default:
        return EvaluateWc3(track, window, held);
    }
}

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

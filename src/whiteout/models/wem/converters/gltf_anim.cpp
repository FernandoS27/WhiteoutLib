// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file gltf_anim.cpp
 * @brief Clips → glTF animations (GLTF_DESIGN §8).
 *
 * The clock alignment is a gift — both sides key in seconds, so nothing here
 * retimes. What does need converting is the smooth modes' *shape*:
 *
 * - WEM stores `{value, inTan, outTan}` per Hermite key; glTF CUBICSPLINE
 *   wants `{inTan, value, outTan}` — a reorder.
 * - The reference evaluator (`HermiteInterp` in the host) applies tangents
 *   **without** a span multiply, so a WEM tangent is per-key-span; glTF
 *   multiplies out-tangents by `t_{k+1} − t_k` at evaluation, so its tangents
 *   are per-second. The crossing divides by the span (design R1, spiked
 *   against the evaluator).
 * - A Bezier key stores absolute control points (`BezierInterp` is the plain
 *   cubic Bernstein form); the Hermite equivalents are `3·(c_out − v)` and
 *   `3·(v − c_in)`, then the same span division.
 * - A Warcraft III quaternion curve is **squad** (`Wc3Squad` in the reference
 *   evaluator): its stored per-key "tangents" are squad control quaternions,
 *   not derivatives. glTF has no squad, so an mdx rotation curve exports its
 *   key values under LINEAR — exact at every key, only the inner easing lost.
 *   (Design R1 originally claimed componentwise `ApplyV4` here; the reference
 *   overturned it.)
 *
 * A pivot-relative rig's translation channel is an *offset* from the bind
 * local (the pivot chain); glTF replaces the whole TRS, so the bind local
 * translation is added to every key value — the derivation lands on
 * `L(t) = S·R·T(local_bind + t_anim)`, tangents untouched by the constant.
 *
 * Sub-tracks are not clamped to their clip (clip.h); the slicers below
 * enforce the window at this boundary. An mdx-windowed clip (marked by
 * `intervalStart`/`globalSequenceId` in its native bag) follows the engine's
 * own rule — in-window keys only, last→first wrap across the boundary, rest
 * when the window holds nothing (`FindBracket` in the reference viewer);
 * every other source takes a plain evaluated cut.
 */

#include "gltf_anim.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "whiteout/models/gltf/parser.h"

namespace whiteout {
namespace models {
namespace wem {
namespace gltf_anim {

namespace {

using gltf_detail::AddFloatAccessor;
using gltf_detail::PermuteToGltf;

/// One key slot's floats out of a sub-track's raw bytes.
void readKeySlot(const SubTrack& track, u32 key, u32 slot, u32 componentCount, f32* out) {
    const u32 valuesPerKey = ValuesPerKey(track.interp);
    const std::size_t offset =
        (static_cast<std::size_t>(key) * valuesPerKey + slot) * componentCount * sizeof(f32);
    std::memcpy(out, track.values.data() + offset, componentCount * sizeof(f32));
}

struct DecodedTrack {
    std::vector<f32> times;
    /// `times.size() * components` values, or three times that for a cubic
    /// output (in-tangent, value, out-tangent per key, glTF's order).
    std::vector<f32> values;
    gltf::AnimInterpolation interpolation = gltf::AnimInterpolation::Linear;
    u32 components = 0;
};

/// A sub-track unpacked for slicing: `values` holds `{value, inTan, outTan}`
/// per key (tangents zero for the non-cubic modes; Bezier control points are
/// already converted to Hermite tangents, which evaluate identically).
struct RawKeys {
    std::vector<f32> times;
    std::vector<f32> values; ///< `times.size() * 3 * comps`.
    u32 comps = 0;
    bool cubic = false;

    f32* value(std::size_t k) {
        return values.data() + (k * 3 + 0) * comps;
    }
    f32* inTan(std::size_t k) {
        return values.data() + (k * 3 + 1) * comps;
    }
    f32* outTan(std::size_t k) {
        return values.data() + (k * 3 + 2) * comps;
    }
};

/// `P(u)` and `dP/du` of one Hermite segment in the reference evaluator's
/// convention (per-span tangents, no Δt term) — the same cubic a Bezier key
/// pair describes after the control-point conversion.
void EvalHermite(const RawKeys& keys, std::size_t seg, f32 u, f32* value, f32* slope) {
    RawKeys& k = const_cast<RawKeys&>(keys);
    const f32* v0 = k.value(seg);
    const f32* t0 = k.outTan(seg);
    const f32* v1 = k.value(seg + 1);
    const f32* t1 = k.inTan(seg + 1);
    const f32 u2 = u * u;
    const f32 u3 = u2 * u;
    for (u32 c = 0; c < keys.comps; ++c) {
        value[c] = (2 * u3 - 3 * u2 + 1) * v0[c] + (u3 - 2 * u2 + u) * t0[c] +
                   (-2 * u3 + 3 * u2) * v1[c] + (u3 - u2) * t1[c];
        slope[c] = (6 * u2 - 6 * u) * v0[c] + (3 * u2 - 4 * u + 1) * t0[c] +
                   (-6 * u2 + 6 * u) * v1[c] + (3 * u2 - 2 * u) * t1[c];
    }
}

/// Value of @p keys at @p time under @p interp — hold outside the key range,
/// the segment's own curve inside. For a rotation, the linear modes lerp with
/// the shortest-arc sign fix; Hermite is componentwise, as the reference
/// evaluator has it.
void EvalAt(const RawKeys& keys, Interpolation interp, bool isRotation, f32 time, f32* value) {
    RawKeys& k = const_cast<RawKeys&>(keys);
    const std::size_t n = keys.times.size();
    if (time <= keys.times.front()) {
        std::memcpy(value, k.value(0), keys.comps * sizeof(f32));
        return;
    }
    if (time >= keys.times.back()) {
        std::memcpy(value, k.value(n - 1), keys.comps * sizeof(f32));
        return;
    }
    std::size_t seg = 0;
    while (seg + 2 < n && keys.times[seg + 1] <= time) {
        ++seg;
    }
    const f32 span = keys.times[seg + 1] - keys.times[seg];
    const f32 u = span > 1e-9f ? (time - keys.times[seg]) / span : 0.0f;
    if (interp == Interpolation::Step) {
        std::memcpy(value, k.value(seg), keys.comps * sizeof(f32));
        return;
    }
    if (keys.cubic) {
        f32 slope[4];
        EvalHermite(keys, seg, u, value, slope);
        return;
    }
    const f32* v0 = k.value(seg);
    const f32* v1 = k.value(seg + 1);
    f32 sign = 1.0f;
    if (isRotation && keys.comps == 4) {
        const f32 dot = v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2] + v0[3] * v1[3];
        sign = dot < 0.0f ? -1.0f : 1.0f;
    }
    for (u32 c = 0; c < keys.comps; ++c) {
        value[c] = v0[c] * (1.0f - u) + v1[c] * sign * u;
    }
}

/// Warcraft III's slerp, mirrored from the reference evaluator (`Wc3Slerp`):
/// shortest arc via the sign flip, nlerp under the 0.9 dot threshold.
void SlerpQuat(const f32* a, const f32* bIn, f32 t, f32* out) {
    f32 b[4] = {bIn[0], bIn[1], bIn[2], bIn[3]};
    f32 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) {
        d = -d;
        for (f32& c : b) {
            c = -c;
        }
    }
    d = std::min(d, 1.0f);
    if (d > 0.9f) {
        f32 lengthSq = 0;
        for (int c = 0; c < 4; ++c) {
            out[c] = a[c] + t * (b[c] - a[c]);
            lengthSq += out[c] * out[c];
        }
        const f32 inverse = lengthSq > 1e-12f ? 1.0f / std::sqrt(lengthSq) : 0.0f;
        for (int c = 0; c < 4; ++c) {
            out[c] *= inverse;
        }
        return;
    }
    const f32 theta0 = std::acos(d);
    const f32 theta = theta0 * t;
    const f32 sinTheta0 = std::sin(theta0);
    if (sinTheta0 < 1e-6f) {
        std::memcpy(out, a, 4 * sizeof(f32));
        return;
    }
    const f32 s0 = std::sin(theta0 - theta) / sinTheta0;
    const f32 s1 = std::sin(theta) / sinTheta0;
    for (int c = 0; c < 4; ++c) {
        out[c] = a[c] * s0 + b[c] * s1;
    }
}

/// Warcraft III's quaternion curve (`Wc3Squad`): the stored per-key "tangents"
/// are squad CONTROL QUATERNIONS, not derivatives.
void SquadQuat(const f32* start, const f32* outTan, const f32* inTan, const f32* end, f32 t,
               f32* out) {
    f32 arc[4];
    f32 inner[4];
    SlerpQuat(start, end, t, arc);
    SlerpQuat(outTan, inTan, t, inner);
    SlerpQuat(arc, inner, 2.0f * t * (1.0f - t), out);
}

/// One Hermite segment between explicit endpoints (per-span tangents, host
/// convention) — value and d/du at @p u.
void HermiteAt(const f32* v0, const f32* out0, const f32* in1, const f32* v1, f32 u, u32 comps,
               f32* value, f32* slope) {
    const f32 u2 = u * u;
    const f32 u3 = u2 * u;
    for (u32 c = 0; c < comps; ++c) {
        value[c] = (2 * u3 - 3 * u2 + 1) * v0[c] + (u3 - 2 * u2 + u) * out0[c] +
                   (-2 * u3 + 3 * u2) * v1[c] + (u3 - u2) * in1[c];
        if (slope != nullptr) {
            slope[c] = (6 * u2 - 6 * u) * v0[c] + (3 * u2 - 4 * u + 1) * out0[c] +
                       (-6 * u2 + 6 * u) * v1[c] + (3 * u2 - 2 * u) * in1[c];
        }
    }
}

/// Rebuilds @p keys the way the **Warcraft III engine** samples a sequence
/// (`FindBracket` in the reference viewer, parity-proven): only keys inside
/// the window exist; before the first and after the last the value wraps
/// last→first across the boundary as one continuous segment; and a track with
/// no in-window key contributes the node's rest value. The bracketing keys
/// the WEM sub-track carries for the mdx round trip are exactly the keys the
/// engine never reads — consuming them as curve neighbours splayed every limb
/// whose window starts between keys.
///
/// Returns false when the window holds no key at all — the caller then omits
/// the channel, which in glTF says "the node's bind TRS", the engine's
/// defaultVal.
bool SliceMdxWindow(RawKeys& keys, Interpolation interp, bool isRotation, f32 duration) {
    constexpr f32 kSnap = 1e-5f;
    const std::size_t n = keys.times.size();
    // The in-window run [first, last).
    std::size_t first = 0;
    while (first < n && keys.times[first] < -kSnap) {
        ++first;
    }
    std::size_t last = first;
    while (last < n && keys.times[last] <= duration + kSnap) {
        ++last;
    }
    if (first >= last) {
        return false;
    }

    const bool cubic = keys.cubic;
    RawKeys sliced;
    sliced.comps = keys.comps;
    sliced.cubic = cubic;
    const auto push = [&](f32 time, const f32* value, const f32* inTan, const f32* outTan) {
        sliced.times.push_back(time);
        const std::size_t base = sliced.values.size();
        sliced.values.resize(base + 3 * sliced.comps, 0.0f);
        std::memcpy(sliced.values.data() + base, value, sliced.comps * sizeof(f32));
        if (inTan != nullptr) {
            std::memcpy(sliced.values.data() + base + sliced.comps, inTan,
                        sliced.comps * sizeof(f32));
        }
        if (outTan != nullptr) {
            std::memcpy(sliced.values.data() + base + 2 * sliced.comps, outTan,
                        sliced.comps * sizeof(f32));
        }
    };

    const f32 firstT = std::max(keys.times[first], 0.0f);
    const f32 lastT = std::min(keys.times[last - 1], duration);
    const bool needsWrap =
        last - first > 1 && (firstT > kSnap || lastT < duration - kSnap) && duration > kSnap;

    // The wrap segment runs lastKey → firstKey over `(first − last) + loop`
    // milliseconds (here seconds); `uSeam` is where the clip boundary cuts it.
    f32 uSeam = 0.0f;
    f32 seamValue[4] = {0, 0, 0, 0};
    f32 seamSlope[4] = {0, 0, 0, 0};
    f32 segLen = 0.0f;
    if (needsWrap) {
        segLen = (firstT - lastT) + duration;
        uSeam = segLen > 1e-9f ? (duration - lastT) / segLen : 0.0f;
        const f32* lastValue = keys.value(last - 1);
        const f32* firstValue = keys.value(first);
        if (interp == Interpolation::Step) {
            std::memcpy(seamValue, lastValue, keys.comps * sizeof(f32));
        } else if (cubic && isRotation) {
            SquadQuat(lastValue, keys.outTan(last - 1), keys.inTan(first), firstValue, uSeam,
                      seamValue);
        } else if (cubic) {
            HermiteAt(lastValue, keys.outTan(last - 1), keys.inTan(first), firstValue, uSeam,
                      keys.comps, seamValue, seamSlope);
        } else if (isRotation && keys.comps == 4) {
            SlerpQuat(lastValue, firstValue, uSeam, seamValue);
        } else {
            for (u32 c = 0; c < keys.comps; ++c) {
                seamValue[c] = lastValue[c] * (1.0f - uSeam) + firstValue[c] * uSeam;
            }
        }
    }

    // Leading boundary: the wrapped value, entering the window mid-segment.
    if (needsWrap && firstT > kSnap) {
        f32 outTan[4] = {0, 0, 0, 0};
        if (cubic && !isRotation) {
            for (u32 c = 0; c < keys.comps; ++c) {
                outTan[c] = seamSlope[c] * (1.0f - uSeam);
            }
        }
        push(0.0f, seamValue, nullptr, outTan);
    }
    for (std::size_t k = first; k < last; ++k) {
        f32 inTan[4];
        f32 outTan[4];
        std::memcpy(inTan, keys.inTan(k), keys.comps * sizeof(f32));
        std::memcpy(outTan, keys.outTan(k), keys.comps * sizeof(f32));
        if (cubic && !isRotation && needsWrap) {
            // The cut wrap segment keeps its curve when the per-span tangents
            // facing each cut scale by the kept fraction of the segment.
            if (k == first && firstT > kSnap) {
                for (u32 c = 0; c < keys.comps; ++c) {
                    inTan[c] *= 1.0f - uSeam;
                }
            }
            if (k + 1 == last && lastT < duration - kSnap) {
                for (u32 c = 0; c < keys.comps; ++c) {
                    outTan[c] *= uSeam;
                }
            }
        }
        push(std::min(std::max(keys.times[k], 0.0f), duration), keys.value(k), inTan, outTan);
    }
    // Trailing boundary — not for Step, which holds its last key the way the
    // engine does.
    if (needsWrap && lastT < duration - kSnap && interp != Interpolation::Step) {
        f32 inTan[4] = {0, 0, 0, 0};
        if (cubic && !isRotation) {
            for (u32 c = 0; c < keys.comps; ++c) {
                inTan[c] = seamSlope[c] * uSeam;
            }
        }
        push(duration, seamValue, inTan, nullptr);
    }
    keys = std::move(sliced);
    return true;
}

/// Cuts @p keys down to the clip window `[0, duration]` (GLTF_DESIGN §8).
///
/// The generic rule for clips whose source declares no window semantics of
/// its own: a consumer plays every key it is given, so keys outside the
/// window are evaluated away — a boundary key holds the curve's exact value
/// at the cut, and a cut cubic segment keeps its shape because a Hermite
/// sub-segment is the same cubic with tangents scaled by the kept fraction of
/// the span.
void SliceToWindow(RawKeys& keys, Interpolation interp, bool isRotation, f32 duration) {
    const std::size_t n = keys.times.size();
    if (n == 0) {
        return;
    }
    constexpr f32 kSnap = 1e-5f; ///< Well under MDX's 1ms key spacing.
    if (duration <= kSnap) {
        duration = 0.0f;
    }
    const bool startsInside = keys.times.front() >= -kSnap;
    const bool endsInside = keys.times.back() <= duration + kSnap;
    if (startsInside && endsInside) {
        // The common case — every key already in the window — must stay
        // bit-identical, so it does not pass through the rebuild below.
        if (keys.times.front() < 0.0f) {
            keys.times.front() = 0.0f;
        }
        if (keys.times.back() > duration) {
            keys.times.back() = duration;
        }
        return;
    }

    RawKeys sliced;
    sliced.comps = keys.comps;
    sliced.cubic = keys.cubic;
    const auto push = [&](f32 time, const f32* value, const f32* inTan, const f32* outTan) {
        sliced.times.push_back(time);
        const std::size_t base = sliced.values.size();
        sliced.values.resize(base + 3 * sliced.comps, 0.0f);
        std::memcpy(sliced.values.data() + base, value, sliced.comps * sizeof(f32));
        if (inTan != nullptr) {
            std::memcpy(sliced.values.data() + base + sliced.comps, inTan,
                        sliced.comps * sizeof(f32));
        }
        if (outTan != nullptr) {
            std::memcpy(sliced.values.data() + base + 2 * sliced.comps, outTan,
                        sliced.comps * sizeof(f32));
        }
    };

    // The kept range: keys strictly inside (0, duration), snapping a key that
    // sits on a boundary onto it rather than synthesizing a twin beside it.
    std::size_t first = 0;
    while (first < n && keys.times[first] < -kSnap) {
        ++first;
    }
    std::size_t last = n; // One past the last kept key.
    while (last > first && keys.times[last - 1] > duration + kSnap) {
        --last;
    }

    if (first >= last && (first == 0 || first == n)) {
        // Every key on one side of the window: it sees one held value —
        // hold-first when they are all after it, hold-last when all before.
        // (Keys on *both* sides with none inside fall through instead: that
        // segment crosses the whole window and is cut at both ends.)
        f32 value[4];
        EvalAt(keys, interp, isRotation, 0.0f, value);
        push(0.0f, value, nullptr, nullptr);
        keys = std::move(sliced);
        return;
    }

    // A start cut lands in the segment [first - 1, first]; an end cut in
    // [last - 1, last]. When both land in the same segment (the whole window
    // inside one span) the kept fraction is the distance between the cuts.
    const bool cutStart = first > 0 && keys.times[first] > kSnap;
    const bool cutEnd = last < n && keys.times[last - 1] < duration - kSnap;
    const bool sameSegment = cutStart && cutEnd && last == first;

    f32 startU = 0.0f, endU = 1.0f, startSpan = 0.0f, endSpan = 0.0f;
    if (cutStart) {
        startSpan = keys.times[first] - keys.times[first - 1];
        startU = startSpan > 1e-9f ? (0.0f - keys.times[first - 1]) / startSpan : 0.0f;
    }
    if (cutEnd) {
        endSpan = keys.times[last] - keys.times[last - 1];
        endU = endSpan > 1e-9f ? (duration - keys.times[last - 1]) / endSpan : 0.0f;
    }

    if (cutStart) {
        f32 value[4];
        f32 slope[4] = {0, 0, 0, 0};
        if (keys.cubic) {
            EvalHermite(keys, first - 1, startU, value, slope);
        } else {
            EvalAt(keys, interp, isRotation, 0.0f, value);
        }
        const f32 fraction = (sameSegment ? endU : 1.0f) - startU;
        f32 outTan[4] = {0, 0, 0, 0};
        for (u32 c = 0; c < keys.comps; ++c) {
            outTan[c] = slope[c] * fraction;
        }
        push(0.0f, value, nullptr, outTan);
    }
    for (std::size_t k = first; k < last; ++k) {
        f32 inTan[4], outTan[4];
        std::memcpy(inTan, keys.inTan(k), keys.comps * sizeof(f32));
        std::memcpy(outTan, keys.outTan(k), keys.comps * sizeof(f32));
        // A truncated neighbouring segment keeps its curve when the per-span
        // tangent facing it scales by the kept fraction of that span.
        if (cutStart && k == first) {
            for (u32 c = 0; c < keys.comps; ++c) {
                inTan[c] *= 1.0f - startU;
            }
        }
        if (cutEnd && k + 1 == last) {
            for (u32 c = 0; c < keys.comps; ++c) {
                outTan[c] *= endU;
            }
        }
        const f32 time = std::min(std::max(keys.times[k], 0.0f), duration);
        push(time, keys.value(k), inTan, outTan);
    }
    if (cutEnd) {
        f32 value[4];
        f32 slope[4] = {0, 0, 0, 0};
        if (keys.cubic) {
            EvalHermite(keys, last - 1, endU, value, slope);
        } else {
            EvalAt(keys, interp, isRotation, duration, value);
        }
        const f32 fraction = endU - (sameSegment ? startU : 0.0f);
        f32 inTan[4] = {0, 0, 0, 0};
        for (u32 c = 0; c < keys.comps; ++c) {
            inTan[c] = slope[c] * fraction;
        }
        push(duration, value, inTan, nullptr);
    }
    keys = std::move(sliced);
}

enum class DecodeResult {
    Ok,
    Unsupported, ///< A value type the channel cannot mean — counted as dropped.
    Rest,        ///< An mdx-windowed clip with no key inside the window: the
                 ///< node rests at bind, said in glTF by omitting the channel.
};

/// Decodes one node sub-track into glTF-ready floats: sliced to the clip
/// window, permuted basis, offsets applied, tangents converted.
DecodeResult decodeTrack(const SubTrack& track, const AnimChannel& channel,
                         const Vector3f& bindOffset, f32 clipDuration, bool mdxWindowed,
                         DecodedTrack& out) {
    const u32 sourceComponents = geom::AttrTypeComponents(channel.valueType);
    const bool isRotation = channel.target.channel == Channel::Rotation;
    const bool scalarBroadcast =
        !isRotation && channel.valueType == geom::AttrType::F32; // D3's one-float scale.
    if (isRotation) {
        if (channel.valueType != geom::AttrType::Quat &&
            channel.valueType != geom::AttrType::F32x4) {
            return DecodeResult::Unsupported;
        }
    } else if (channel.valueType != geom::AttrType::F32x3 && !scalarBroadcast) {
        return DecodeResult::Unsupported;
    }
    const u32 components = isRotation ? 4u : 3u;
    const std::size_t keyCount = track.keyCount();
    if (keyCount == 0 || !track.wellSized(channel.valueType)) {
        return DecodeResult::Unsupported;
    }

    out.components = components;
    const bool cubic =
        track.interp == Interpolation::Hermite || track.interp == Interpolation::Bezier;

    // Unpack into the slicing shape: a NaN-stamped key is garbage and drops, a
    // non-increasing stamp keeps its first occurrence, and a Bezier key's
    // absolute control points become the Hermite tangents of the same cubic —
    // `3(v − c_in)`, `3(c_out − v)` — so the slicer speaks one smooth mode.
    RawKeys raw;
    raw.comps = sourceComponents;
    raw.cubic = cubic;
    {
        f32 value[4] = {0, 0, 0, 0};
        f32 valueIn[4] = {0, 0, 0, 0};
        f32 valueOut[4] = {0, 0, 0, 0};
        for (std::size_t k = 0; k < keyCount; ++k) {
            f32 time = track.times[k];
            if (!std::isfinite(time)) {
                continue;
            }
            if (!raw.times.empty() && time <= raw.times.back()) {
                // A duplicate stamp is a hard step and keeps both keys; a
                // stamp going backwards is garbage and rides the same nudge.
                time = std::max(raw.times.back() + 1e-4f,
                                std::nextafterf(raw.times.back(), 3.4e38f));
            }
            readKeySlot(track, static_cast<u32>(k), 0, sourceComponents, value);
            if (cubic) {
                readKeySlot(track, static_cast<u32>(k), 1, sourceComponents, valueIn);
                readKeySlot(track, static_cast<u32>(k), 2, sourceComponents, valueOut);
                // A rotation's stored "tangents" under mdx semantics are squad
                // control quaternions and stay raw for the seam evaluation.
                if (track.interp == Interpolation::Bezier &&
                    !(mdxWindowed && isRotation)) {
                    for (u32 c = 0; c < sourceComponents; ++c) {
                        valueIn[c] = 3.0f * (value[c] - valueIn[c]);
                        valueOut[c] = 3.0f * (valueOut[c] - value[c]);
                    }
                }
            }
            raw.times.push_back(time);
            const std::size_t base = raw.values.size();
            raw.values.resize(base + 3 * sourceComponents, 0.0f);
            std::memcpy(raw.values.data() + base, value, sourceComponents * sizeof(f32));
            std::memcpy(raw.values.data() + base + sourceComponents, valueIn,
                        sourceComponents * sizeof(f32));
            std::memcpy(raw.values.data() + base + 2 * sourceComponents, valueOut,
                        sourceComponents * sizeof(f32));
        }
    }
    if (raw.times.empty()) {
        return DecodeResult::Unsupported;
    }

    // Sub-tracks are not clamped to the clip (clip.h) — but a glTF consumer
    // plays every key it sees, so the window is enforced here: with the
    // engine's own wrap rule for an mdx-windowed clip, a plain cut otherwise.
    if (mdxWindowed) {
        if (!SliceMdxWindow(raw, track.interp, isRotation, clipDuration)) {
            return DecodeResult::Rest;
        }
    } else {
        SliceToWindow(raw, track.interp, isRotation, clipDuration);
    }

    out.times = raw.times;
    // glTF requires non-negative, strictly increasing input. The slice leaves
    // both true for keys it touched; duplicate stamps for hard steps still
    // nudge forward — a distortion of a tenth of a millisecond, not a drop.
    for (f32& time : out.times) {
        if (!(time > 0.0f)) { // Also catches -0.
            time = 0.0f;
        }
    }
    for (std::size_t i = 1; i < out.times.size(); ++i) {
        if (out.times[i] <= out.times[i - 1]) {
            // At the multi-week timestamps world-clocked clips reach, 1e-4
            // drowns in the float's own spacing; the next representable value
            // is the increment that always exists.
            out.times[i] = std::max(out.times[i - 1] + 1e-4f,
                                    std::nextafterf(out.times[i - 1], 3.4e38f));
        }
    }

    switch (track.interp) {
    case Interpolation::Step:
        out.interpolation = gltf::AnimInterpolation::Step;
        break;
    case Interpolation::Hermite:
    case Interpolation::Bezier:
        out.interpolation = gltf::AnimInterpolation::CubicSpline;
        break;
    default:
        out.interpolation = gltf::AnimInterpolation::Linear;
        break;
    }
    // Squad has no glTF spelling: an mdx rotation curve exports its key
    // values under LINEAR (the spec's rotation lerp is a slerp), which agrees
    // with the engine at every key and drops only the inner easing.
    if (mdxWindowed && isRotation && cubic) {
        out.interpolation = gltf::AnimInterpolation::Linear;
    }
    // A one-key cubic has no curve to speak of; STEP says the same thing and
    // stays legal (CUBICSPLINE needs two keys).
    if (cubic && raw.times.size() < 2) {
        out.interpolation = gltf::AnimInterpolation::Step;
    }

    const auto convertValue = [&](const f32* raw, f32* converted) {
        if (isRotation) {
            Quaternion q{raw[0], raw[1], raw[2], raw[3]};
            const f32 length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (!std::isfinite(length) || length < 1e-6f) {
                // Shipped `.m2` content carries NaN keys; identity is the
                // least-wrong stand-in and keeps the file legal.
                q = Quaternion{0, 0, 0, 1};
            } else if (length < 1.0f - 1e-4f || length > 1.0f + 1e-4f) {
                q = q * (1.0f / length);
            }
            const Quaternion permuted = PermuteToGltf(q);
            converted[0] = permuted.x;
            converted[1] = permuted.y;
            converted[2] = permuted.z;
            converted[3] = permuted.w;
            return;
        }
        Vector3f v = scalarBroadcast ? Vector3f{raw[0], raw[0], raw[0]}
                                     : Vector3f{raw[0], raw[1], raw[2]};
        for (f32& component : v.data) {
            if (!std::isfinite(component)) {
                component = 0.0f;
            }
        }
        if (channel.target.channel == Channel::Translation) {
            v += bindOffset;
        }
        const Vector3f permuted = channel.target.channel == Channel::Scale
                                      ? Vector3f{v.y, v.z, v.x}
                                      : PermuteToGltf(v);
        converted[0] = permuted.x;
        converted[1] = permuted.y;
        converted[2] = permuted.z;
    };
    // Tangents are directions, not positions: no bind offset, no
    // normalization — only the permutation (a linear map commutes with it)
    // and the same NaN scrub the values get.
    const auto convertTangent = [&](const f32* raw, f32 scale, f32* converted) {
        const auto finite = [](f32 value) { return std::isfinite(value) ? value : 0.0f; };
        if (isRotation) {
            const Quaternion permuted = PermuteToGltf(
                Quaternion{finite(raw[0]), finite(raw[1]), finite(raw[2]), finite(raw[3])});
            converted[0] = permuted.x * scale;
            converted[1] = permuted.y * scale;
            converted[2] = permuted.z * scale;
            converted[3] = permuted.w * scale;
            return;
        }
        const Vector3f v = scalarBroadcast
                               ? Vector3f{finite(raw[0]), finite(raw[0]), finite(raw[0])}
                               : Vector3f{finite(raw[0]), finite(raw[1]), finite(raw[2])};
        const Vector3f permuted = channel.target.channel == Channel::Scale
                                      ? Vector3f{v.y, v.z, v.x}
                                      : PermuteToGltf(v);
        converted[0] = permuted.x * scale;
        converted[1] = permuted.y * scale;
        converted[2] = permuted.z * scale;
    };

    const std::size_t emitCount = raw.times.size();
    if (out.interpolation == gltf::AnimInterpolation::CubicSpline) {
        out.values.assign(emitCount * 3 * components, 0.0f);
        for (std::size_t k = 0; k < emitCount; ++k) {
            const f32 spanIn = k > 0 ? out.times[k] - out.times[k - 1] : 0.0f;
            const f32 spanOut = k + 1 < emitCount ? out.times[k + 1] - out.times[k] : 0.0f;
            f32* base = out.values.data() + k * 3 * components;
            convertTangent(raw.inTan(k), spanIn > 1e-9f ? 1.0f / spanIn : 0.0f, base);
            convertValue(raw.value(k), base + components);
            convertTangent(raw.outTan(k), spanOut > 1e-9f ? 1.0f / spanOut : 0.0f,
                           base + 2 * components);
        }
    } else {
        out.values.assign(emitCount * components, 0.0f);
        for (std::size_t k = 0; k < emitCount; ++k) {
            convertValue(raw.value(k), out.values.data() + k * components);
        }
    }
    return DecodeResult::Ok;
}

} // namespace

void Export(const Document& document, gltf::Asset& asset, gltf_detail::BinBuilder& bin,
            std::span<const u32> modelNodeBase, Diagnostics& diagnostics) {
    for (std::size_t clipIndex = 0; clipIndex < document.clips.size(); ++clipIndex) {
        const Clip& clip = document.clips[clipIndex];
        if (clip.model >= document.models.size() || clip.model >= modelNodeBase.size()) {
            diagnostics.warn(DiagCode::ClipTargetMissing,
                             "clip '" + clip.name + "' names a model the document does not hold",
                             ElementRef(ElementKind::Clip, static_cast<u32>(clipIndex)));
            continue;
        }
        const Model& model = document.models[clip.model];
        const u32 nodeBase = modelNodeBase[clip.model];
        // An mdx clip records its timeline window (or its global sequence) in
        // the native bag; that marks its sub-tracks as engine-windowed — see
        // SliceMdxWindow. Other sources take the generic cut.
        const bool mdxWindowed = clip.native.value("intervalStart", -1) >= 0 ||
                                 clip.native.value("globalSequenceId", -1) >= 0;

        // Containers flatten by priority: for each channel, the highest
        // priority that keys it wins; a loser is counted, not blended — the
        // layering data is WEM's, the blender is a runtime's (§10.8).
        std::vector<u32> order(clip.containers.size());
        for (u32 i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
            return clip.containers[a].priority > clip.containers[b].priority;
        });
        std::vector<std::pair<u32, const SubTrack*>> chosen; // (channel id, track)
        u32 overridden = 0;
        for (const u32 containerIndex : order) {
            for (const SubTrack& track : clip.containers[containerIndex].subTracks) {
                bool taken = false;
                for (const auto& entry : chosen) {
                    taken = taken || entry.first == track.channel;
                }
                if (taken) {
                    ++overridden;
                } else {
                    chosen.emplace_back(track.channel, &track);
                }
            }
        }

        gltf::Animation animation;
        animation.name = clip.name;
        u32 dropped = 0;
        u32 duplicates = 0;
        std::vector<std::pair<u32, gltf::AnimPath>> usedTargets;

        for (const auto& [channelId, track] : chosen) {
            const AnimChannel* channel = model.animChannels.find(channelId);
            if (channel == nullptr || channel->target.kind != TrackTarget::Kind::Node ||
                channel->target.node >= model.nodes.size()) {
                ++dropped;
                continue;
            }
            gltf::AnimPath path;
            switch (channel->target.channel) {
            case Channel::Translation:
                path = gltf::AnimPath::Translation;
                break;
            case Channel::Rotation:
                path = gltf::AnimPath::Rotation;
                break;
            case Channel::Scale:
                path = gltf::AnimPath::Scale;
                break;
            default:
                ++dropped; // Visibility, colour, … — no core-glTF target.
                continue;
            }
            const u32 targetNode = nodeBase + channel->target.node;
            bool seen = false;
            for (const auto& used : usedTargets) {
                seen = seen || (used.first == targetNode && used.second == path);
            }
            if (seen) {
                // glTF forbids two channels on one (node, path); the loser was
                // a lower-priority layer or a `sub`-distinguished twin.
                ++duplicates;
                continue;
            }

            // A pivot-relative rig keys offsets from the bind local; glTF
            // replaces the node TRS, so the bind translation joins each key.
            const Vector3f bindOffset = model.nodes.rig == RigConvention::PivotRelative
                                            ? model.nodes.nodes[channel->target.node]
                                                  .local.translation
                                            : Vector3f{0, 0, 0};
            DecodedTrack decoded;
            const DecodeResult decode =
                decodeTrack(*track, *channel, bindOffset, clip.duration, mdxWindowed, decoded);
            if (decode == DecodeResult::Unsupported) {
                ++dropped;
                continue;
            }
            if (decode == DecodeResult::Rest) {
                // The engine's rest value, said in glTF by silence.
                continue;
            }

            gltf::AnimationSampler sampler;
            sampler.input = AddFloatAccessor(asset, bin, decoded.times, 1,
                                             gltf::AccessorType::Scalar, true,
                                             gltf::BufferTarget::None);
            sampler.output = AddFloatAccessor(
                asset, bin, decoded.values, decoded.components,
                decoded.components == 4 ? gltf::AccessorType::Vec4 : gltf::AccessorType::Vec3,
                false, gltf::BufferTarget::None);
            sampler.interpolation = decoded.interpolation;
            animation.samplers.push_back(sampler);

            gltf::AnimationChannel target;
            target.sampler = static_cast<u32>(animation.samplers.size() - 1);
            target.targetNode = targetNode;
            target.targetPath = path;
            animation.channels.push_back(target);
            usedTargets.emplace_back(targetNode, path);
        }

        const ElementRef where(ElementKind::Clip, static_cast<u32>(clipIndex));
        if (dropped != 0) {
            diagnostics.warn(DiagCode::AnimTrackDropped,
                             "clip '" + clip.name + "': " + std::to_string(dropped) +
                                 " track(s) target things core glTF cannot animate",
                             where);
        }
        if (overridden != 0 || duplicates != 0) {
            diagnostics.info(DiagCode::AnimTrackApproximated,
                             "clip '" + clip.name + "': " +
                                 std::to_string(overridden + duplicates) +
                                 " layered track(s) flattened by priority",
                             where);
        }
        if (!clip.events.empty()) {
            diagnostics.info(DiagCode::AnimTrackDropped,
                             "clip '" + clip.name + "': " + std::to_string(clip.events.size()) +
                                 " event(s) have no glTF form",
                             where);
        }
        if (!animation.channels.empty()) {
            asset.animations.push_back(std::move(animation));
        }
    }
}

// ============================================================================
// Import (GLTF_DESIGN §8, the other direction)
// ============================================================================

namespace {

/// `blizzard = (z, x, y)_gltf`, the inverse permutation.
inline Vector3f PermuteFromGltf(const Vector3f& v) {
    return {v.z, v.x, v.y};
}

inline Quaternion PermuteFromGltf(const Quaternion& q) {
    return {q.z, q.x, q.y, q.w};
}

/// The channel for (node, property), created on first use.
u32 channelFor(Model& model, u32 node, Channel property, geom::AttrType valueType) {
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == node &&
            channel.target.channel == property) {
            return channel.id;
        }
    }
    AnimChannel channel;
    channel.id = model.animChannels.nextFreeId();
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = node;
    channel.target.channel = property;
    channel.valueType = valueType;
    return model.animChannels.add(channel);
}

} // namespace

void Import(const gltf::Asset& source, Document& document, std::span<const u32> wemIndex,
            Diagnostics& diagnostics) {
    if (document.models.empty()) {
        return;
    }
    Model& model = document.models[0];

    for (std::size_t a = 0; a < source.animations.size(); ++a) {
        const gltf::Animation& animation = source.animations[a];
        Clip clip;
        clip.name = !animation.name.empty() ? animation.name : ("clip" + std::to_string(a));
        clip.model = 0;
        clip.looping = true;
        SubTrackContainer container;
        u32 dropped = 0;

        for (const gltf::AnimationChannel& channel : animation.channels) {
            if (channel.sampler >= animation.samplers.size() ||
                channel.targetNode == gltf::kNone || channel.targetNode >= wemIndex.size() ||
                wemIndex[channel.targetNode] == 0xFFFFFFFFu) {
                ++dropped;
                continue;
            }
            if (channel.targetPath == gltf::AnimPath::Weights ||
                channel.targetPath == gltf::AnimPath::Count) {
                ++dropped; // WEM has no morph targets (§9).
                continue;
            }
            const gltf::AnimationSampler& sampler = animation.samplers[channel.sampler];
            std::vector<f32> times;
            std::vector<f32> values;
            if (!gltf::ReadAccessorF32(source, sampler.input, times) ||
                !gltf::ReadAccessorF32(source, sampler.output, values) || times.empty()) {
                ++dropped;
                continue;
            }
            const bool isRotation = channel.targetPath == gltf::AnimPath::Rotation;
            const u32 components = isRotation ? 4u : 3u;
            const bool cubic = sampler.interpolation == gltf::AnimInterpolation::CubicSpline;
            const std::size_t expected =
                times.size() * components * (cubic ? 3 : 1);
            if (values.size() != expected) {
                ++dropped;
                continue;
            }

            const Channel property = isRotation ? Channel::Rotation
                                     : channel.targetPath == gltf::AnimPath::Translation
                                         ? Channel::Translation
                                         : Channel::Scale;
            SubTrack track;
            track.channel = channelFor(model, wemIndex[channel.targetNode], property,
                                       isRotation ? geom::AttrType::Quat
                                                  : geom::AttrType::F32x3);
            track.times = times;
            switch (sampler.interpolation) {
            case gltf::AnimInterpolation::Step:
                track.interp = Interpolation::Step;
                break;
            case gltf::AnimInterpolation::CubicSpline:
                track.interp = Interpolation::Hermite;
                break;
            case gltf::AnimInterpolation::Linear:
                track.interp = isRotation ? Interpolation::Slerp : Interpolation::Linear;
                break;
            }

            const u32 valuesPerKey = ValuesPerKey(track.interp);
            track.values.resize(times.size() * valuesPerKey * components * sizeof(f32));
            f32* out = reinterpret_cast<f32*>(track.values.data());
            const auto putVec = [&](std::size_t slot, const f32* raw, f32 scale) {
                if (isRotation) {
                    const Quaternion q = PermuteFromGltf(
                        Quaternion{raw[0], raw[1], raw[2], raw[3]});
                    out[slot * 4 + 0] = q.x * scale;
                    out[slot * 4 + 1] = q.y * scale;
                    out[slot * 4 + 2] = q.z * scale;
                    out[slot * 4 + 3] = q.w * scale;
                } else {
                    const Vector3f v{raw[0], raw[1], raw[2]};
                    const Vector3f permuted = property == Channel::Scale
                                                  ? Vector3f{v.z, v.x, v.y}
                                                  : PermuteFromGltf(v);
                    out[slot * 3 + 0] = permuted.x * scale;
                    out[slot * 3 + 1] = permuted.y * scale;
                    out[slot * 3 + 2] = permuted.z * scale;
                }
            };
            for (std::size_t k = 0; k < times.size(); ++k) {
                if (cubic) {
                    // glTF stores {inTan, value, outTan}, per second; WEM
                    // stores {value, inTan, outTan}, per span — the reorder
                    // and the multiplication invert the export exactly.
                    const f32 spanIn = k > 0 ? times[k] - times[k - 1] : 0.0f;
                    const f32 spanOut = k + 1 < times.size() ? times[k + 1] - times[k] : 0.0f;
                    const f32* key = values.data() + k * 3 * components;
                    putVec(k * 3 + 0, key + components, 1.0f);
                    putVec(k * 3 + 1, key, spanIn);
                    putVec(k * 3 + 2, key + 2 * components, spanOut);
                } else {
                    putVec(k * (valuesPerKey), values.data() + k * components, 1.0f);
                }
            }
            if (!times.empty() && times.back() > clip.duration) {
                clip.duration = times.back();
            }
            container.subTracks.push_back(std::move(track));
        }

        if (dropped != 0) {
            diagnostics.warn(DiagCode::AnimTrackDropped,
                             "animation '" + clip.name + "': " + std::to_string(dropped) +
                                 " channel(s) have no WEM target",
                             ElementRef(ElementKind::Clip, static_cast<u32>(a)));
        }
        clip.containers.push_back(std::move(container));
        document.clips.push_back(std::move(clip));
    }
}

} // namespace gltf_anim
} // namespace wem
} // namespace models
} // namespace whiteout

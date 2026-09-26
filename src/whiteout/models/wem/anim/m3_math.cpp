// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/m3_math.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

M3KeySpan M3LocateKey(std::span<const i32> times, i32 timeMs, bool loop, bool interpolate) {
    M3KeySpan span;
    if (times.empty())
        return span;
    span.valid = true;

    if (times.size() == 1) {
        span.i0 = span.i1 = 0;
        return span;
    }

    // A looping track wraps on its own last key, which is why the caller hands
    // in unwrapped time rather than the sequence-windowed value.
    if (loop) {
        const i32 period = times.back();
        if (period > 0) {
            timeMs %= period;
            if (timeMs < 0)
                timeMs += period;
        }
    }

    if (timeMs <= times.front()) {
        span.i0 = span.i1 = 0;
        return span;
    }
    if (timeMs >= times.back()) {
        span.i0 = span.i1 = times.size() - 1;
        return span;
    }

    const auto it = std::upper_bound(times.begin(), times.end(), timeMs);
    const std::size_t hi = static_cast<std::size_t>(it - times.begin());
    span.i0 = hi - 1;
    span.i1 = hi;

    if (!interpolate) {
        span.i1 = span.i0; // step: hold the left key
        return span;
    }
    const i32 t0 = times[span.i0];
    const i32 t1 = times[span.i1];
    const i32 dt = t1 - t0;
    span.frac = dt > 0 ? static_cast<f32>(timeMs - t0) / static_cast<f32>(dt)
                       : 0.0f;
    return span;
}

Quaternion M3LerpQuatRaw(const Quaternion& a,
                                     const Quaternion& b, f32 t) {
    // No normalise, no sign fix — see the header. Matching this exactly matters
    // on tracks whose adjacent keys are more than 90° apart, where a slerp and
    // this lerp disagree visibly.
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t};
}

Quaternion M3SlerpQuat(const Quaternion& a, const Quaternion& b,
                                   f32 t) {
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    Quaternion e = b;
    if (dot < 0.0f) {
        dot = -dot;
        e = {-b.x, -b.y, -b.z, -b.w};
    }
    if (dot > 0.9995f)
        return M3LerpQuatRaw(a, e, t);

    const f32 theta = std::acos(dot);
    const f32 sinTheta = std::sin(theta);
    if (sinTheta <= 1e-6f)
        return M3LerpQuatRaw(a, e, t);
    const f32 wa = std::sin((1.0f - t) * theta) / sinTheta;
    const f32 wb = std::sin(t * theta) / sinTheta;
    return {a.x * wa + e.x * wb, a.y * wa + e.y * wb, a.z * wa + e.z * wb, a.w * wa + e.w * wb};
}

} // namespace wem
} // namespace models
} // namespace whiteout

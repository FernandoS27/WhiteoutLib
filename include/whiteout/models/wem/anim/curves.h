// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file curves.h
 * @brief Warcraft III's curves between two keys, and a smooth track restated
 *        as straight lines for a format that has none.
 *
 * The same arithmetic as the viewer's `mdx_eval_math.h` (parity-gated there
 * against the 3.0 engine), so a converter that has to know what a Hermite or
 * Bezier span plays does not grow its own copy.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include <algorithm>
#include <cmath>

#include "clip.h"

namespace whiteout {
namespace models {
namespace wem {

inline f32 HermiteInterp(f32 a, f32 outTanA, f32 inTanB, f32 b, f32 t) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * a + (-2 * t3 + 3 * t2) * b + (t3 - 2 * t2 + t) * outTanA +
           (t3 - t2) * inTanB;
}

/// Bezier handles are positions, not offsets: @p outTanA and @p inTanB are the
/// two inner control points.
inline f32 BezierInterp(f32 a, f32 outTanA, f32 inTanB, f32 b, f32 t) {
    const f32 it = 1.0f - t;
    return it * it * it * a + 3 * it * it * t * outTanA + 3 * it * t * t * inTanB + t * t * t * b;
}

/// The engine's slerp (`NTempest::C4Quaternion::Slerp`, 3.0): a normalised lerp
/// once the RAW dot reaches 0.9, and the short arc below that. A pair in
/// opposite hemispheres never takes the lerp.
inline Quaternion Wc3Slerp(const Quaternion& a, const Quaternion& b, f32 t) {
    f32 d = a.dot(b);
    if (d >= 0.9f) {
        return Quaternion(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z),
                          a.w + t * (b.w - a.w))
            .normalized();
    }
    d = std::clamp(d, -1.0f, 1.0f);
    Quaternion end = b;
    if (d < 0.0f) {
        d = -d;
        end = Quaternion(-b.x, -b.y, -b.z, -b.w);
    }
    const f32 theta0 = std::acos(d);
    const f32 theta = theta0 * t;
    const f32 sinTheta0 = std::sin(theta0);
    if (sinTheta0 < 1e-6f) {
        return a;
    }
    const f32 s0 = std::sin(theta0 - theta) / sinTheta0;
    const f32 s1 = std::sin(theta) / sinTheta0;
    return Quaternion(a.x * s0 + end.x * s1, a.y * s0 + end.y * s1, a.z * s0 + end.z * s1,
                      a.w * s0 + end.w * s1);
}

/// What the engine plays a Hermite rotation AND a Bezier one as. The
/// "tangents" are control quaternions, not derivatives.
inline Quaternion Wc3Squad(const Quaternion& start, const Quaternion& outTan,
                           const Quaternion& inTan, const Quaternion& end, f32 t) {
    return Wc3Slerp(Wc3Slerp(start, end, t), Wc3Slerp(outTan, inTan, t), 2 * t * (1 - t));
}

/// @p track, when `Hermite` or `Bezier`, as a `Linear` sub-track that plays
/// the same curve: keys are added inside each span until straight lines (a
/// normalised lerp for a rotation) stay within 0.1 degree, or 0.1% of the
/// span's size, of what Warcraft III plays. Only spans with both keys inside
/// `[from, to]` are subdivided, so a clip window's bracketing keys, which the
/// game never reads, add nothing. Any other track is returned as it is.
SubTrack LinearisedTrack(const SubTrack& track, geom::AttrType type, f32 from, f32 to);

} // namespace wem
} // namespace models
} // namespace whiteout

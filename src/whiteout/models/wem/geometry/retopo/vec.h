// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file vec.h
 * @brief The retopology's small geometry kit, in doubles: every stage works in
 *        them, because a layout traced through thousands of triangles adds up
 *        float error that moves a snapped vertex onto the wrong side of an edge.
 */

#include <algorithm>
#include <cmath>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

using V3 = Vector3d;

inline constexpr u32 kNone = 0xFFFFFFFFu;
inline constexpr f64 kPi = 3.14159265358979323846;

inline V3 Make(f64 x, f64 y, f64 z) {
    return V3{x, y, z};
}
inline V3 ToV3(const Vector3f& v) {
    return V3{v.x, v.y, v.z};
}
inline Vector3f ToV3f(const V3& v) {
    return Vector3f{static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}
inline f64 Dot(const V3& a, const V3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 Cross(const V3& a, const V3& b) {
    return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline f64 Length(const V3& a) {
    return std::sqrt(Dot(a, a));
}
inline f64 Length2(const V3& a) {
    return Dot(a, a);
}
/// @p a over its length, or zero for a zero vector.
inline V3 Unit(const V3& a) {
    const f64 length = Length(a);
    return length > 0.0 ? a * (1.0 / length) : V3{0.0, 0.0, 0.0};
}
inline V3 Lerp(const V3& a, const V3& b, f64 t) {
    return a + (b - a) * t;
}
inline f64 Distance(const V3& a, const V3& b) {
    return Length(a - b);
}

/// The unsigned angle between two vectors, in [0, pi].
inline f64 Angle(const V3& a, const V3& b) {
    return std::atan2(Length(Cross(a, b)), Dot(a, b));
}

/// @p v with its component along unit @p n removed.
inline V3 Tangent(const V3& v, const V3& n) {
    return v - n * Dot(v, n);
}

inline V3 TriangleNormal(const V3& a, const V3& b, const V3& c) {
    return Unit(Cross(b - a, c - a));
}
inline f64 TriangleArea(const V3& a, const V3& b, const V3& c) {
    return 0.5 * Length(Cross(b - a, c - a));
}

/// The point of triangle abc nearest @p p, as barycentric weights (Ericson,
/// Real-Time Collision Detection §5.1.5), with the distance.
struct Nearest {
    f64 w0 = 1.0;
    f64 w1 = 0.0;
    f64 w2 = 0.0;
    f64 distance = 0.0;
};

inline Nearest NearestOnTriangle(const V3& p, const V3& a, const V3& b, const V3& c) {
    Nearest out;
    const V3 ab = b - a;
    const V3 ac = c - a;
    const V3 ap = p - a;
    const f64 d1 = Dot(ab, ap);
    const f64 d2 = Dot(ac, ap);
    auto finish = [&](f64 w0, f64 w1, f64 w2) {
        out.w0 = w0;
        out.w1 = w1;
        out.w2 = w2;
        out.distance = Distance(p, a * w0 + b * w1 + c * w2);
        return out;
    };
    if (d1 <= 0.0 && d2 <= 0.0) {
        return finish(1.0, 0.0, 0.0);
    }
    const V3 bp = p - b;
    const f64 d3 = Dot(ab, bp);
    const f64 d4 = Dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return finish(0.0, 1.0, 0.0);
    }
    const f64 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0 && d1 - d3 > 0.0) {
        const f64 v = d1 / (d1 - d3);
        return finish(1.0 - v, v, 0.0);
    }
    const V3 cp = p - c;
    const f64 d5 = Dot(ab, cp);
    const f64 d6 = Dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return finish(0.0, 0.0, 1.0);
    }
    const f64 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0 && d2 - d6 > 0.0) {
        const f64 w = d2 / (d2 - d6);
        return finish(1.0 - w, 0.0, w);
    }
    const f64 va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0 && (d4 - d3) + (d5 - d6) > 0.0) {
        const f64 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return finish(0.0, 1.0 - w, w);
    }
    const f64 denominator = va + vb + vc;
    if (!(denominator > 0.0)) {
        // A degenerate triangle: its nearest vertex.
        const f64 da = Distance(p, a);
        const f64 db = Distance(p, b);
        const f64 dc = Distance(p, c);
        if (da <= db && da <= dc) {
            return finish(1.0, 0.0, 0.0);
        }
        return db <= dc ? finish(0.0, 1.0, 0.0) : finish(0.0, 0.0, 1.0);
    }
    const f64 v = vb / denominator;
    const f64 w = vc / denominator;
    return finish(1.0 - v - w, v, w);
}

/// The parameter in [0, 1] of the point of segment ab nearest @p p.
inline f64 NearestOnSegment(const V3& p, const V3& a, const V3& b) {
    const V3 ab = b - a;
    const f64 l2 = Length2(ab);
    if (l2 <= 0.0) {
        return 0.0;
    }
    return std::clamp(Dot(p - a, ab) / l2, 0.0, 1.0);
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

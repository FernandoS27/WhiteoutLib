// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/curves.h"

#include <cstring>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr f32 kMinSpan = 0.002f; ///< Two of the engine's milliseconds.
constexpr u32 kMaxDepth = 6;     ///< At most 64 lines per span.
constexpr f32 kRotationTolerance = 0.1f * 3.14159265f / 180.0f;
constexpr f32 kRelativeTolerance = 0.001f;

struct Span {
    const f32* v0;
    const f32* out0;
    const f32* in1;
    const f32* v1;
};

void CurveAt(const Span& span, Interpolation interp, bool rotation, u32 comps, f32 u, f32* out) {
    if (rotation) {
        const auto q = [](const f32* p) { return Quaternion(p[0], p[1], p[2], p[3]); };
        const Quaternion r = Wc3Squad(q(span.v0), q(span.out0), q(span.in1), q(span.v1), u);
        out[0] = r.x;
        out[1] = r.y;
        out[2] = r.z;
        out[3] = r.w;
        return;
    }
    for (u32 c = 0; c < comps; ++c) {
        out[c] = interp == Interpolation::Bezier
                     ? BezierInterp(span.v0[c], span.out0[c], span.in1[c], span.v1[c], u)
                     : HermiteInterp(span.v0[c], span.out0[c], span.in1[c], span.v1[c], u);
    }
}

/// Whether the straight line from @p a to @p b, at @p u, stays within
/// tolerance of @p curve.
bool Close(const f32* a, const f32* b, const f32* curve, bool rotation, u32 comps, f32 u,
           f32 size) {
    if (rotation) {
        f32 dotAb = 0;
        for (u32 c = 0; c < 4; ++c) {
            dotAb += a[c] * b[c];
        }
        const f32 sign = dotAb < 0 ? -1.0f : 1.0f;
        f32 line[4];
        f32 length = 0;
        for (u32 c = 0; c < 4; ++c) {
            line[c] = a[c] + (sign * b[c] - a[c]) * u;
            length += line[c] * line[c];
        }
        length = std::sqrt(length);
        f32 dot = 0;
        for (u32 c = 0; c < 4; ++c) {
            dot += line[c] / (length > 0 ? length : 1.0f) * curve[c];
        }
        return 2.0f * std::acos(std::min(std::fabs(dot), 1.0f)) <= kRotationTolerance;
    }
    for (u32 c = 0; c < comps; ++c) {
        if (std::fabs(a[c] + (b[c] - a[c]) * u - curve[c]) > kRelativeTolerance * size) {
            return false;
        }
    }
    return true;
}

} // namespace

SubTrack LinearisedTrack(const SubTrack& track, geom::AttrType type, f32 from, f32 to) {
    const bool smooth =
        track.interp == Interpolation::Hermite || track.interp == Interpolation::Bezier;
    const bool rotation = type == geom::AttrType::Quat;
    const bool floats = rotation || type == geom::AttrType::F32 || type == geom::AttrType::F32x2 ||
                        type == geom::AttrType::F32x3 || type == geom::AttrType::F32x4;
    if (!smooth || !floats || !track.wellSized(type)) {
        return track;
    }
    const u32 comps = geom::AttrTypeComponents(type);
    const f32* values = reinterpret_cast<const f32*>(track.values.data());
    const auto slot = [&](std::size_t key, u32 part) { return values + (key * 3 + part) * comps; };

    SubTrack out;
    out.channel = track.channel;
    out.interp = Interpolation::Linear;
    const auto push = [&](f32 time, const f32* value) {
        out.times.push_back(time);
        const u8* bytes = reinterpret_cast<const u8*>(value);
        out.values.insert(out.values.end(), bytes, bytes + comps * sizeof(f32));
    };

    for (std::size_t k = 0; k < track.times.size(); ++k) {
        push(track.times[k], slot(k, 0));
        if (k + 1 == track.times.size()) {
            break;
        }
        const f32 t0 = track.times[k];
        const f32 t1 = track.times[k + 1];
        if (t0 < from - 1e-4f || t1 > to + 1e-4f || t1 - t0 < kMinSpan) {
            continue;
        }
        const Span span{slot(k, 0), slot(k, 2), slot(k + 1, 1), slot(k + 1, 0)};
        f32 size = 0;
        for (u32 c = 0; c < comps; ++c) {
            size = std::max({size, std::fabs(span.v0[c]), std::fabs(span.v1[c]),
                             std::fabs(span.v1[c] - span.v0[c])});
        }
        size = std::max(size, 1e-3f);

        // Bisect until each piece's line matches the curve at its quarter
        // points; the pieces' inner ends become keys, in time order.
        struct Piece {
            f32 u0, u1;
            u32 depth;
        };
        std::vector<Piece> stack{{0.0f, 1.0f, 0}};
        while (!stack.empty()) {
            const Piece piece = stack.back();
            stack.pop_back();
            f32 a[4], b[4];
            CurveAt(span, track.interp, rotation, comps, piece.u0, a);
            CurveAt(span, track.interp, rotation, comps, piece.u1, b);
            bool close = true;
            for (const f32 q : {0.25f, 0.5f, 0.75f}) {
                f32 curve[4];
                CurveAt(span, track.interp, rotation, comps,
                        piece.u0 + (piece.u1 - piece.u0) * q, curve);
                close = close && Close(a, b, curve, rotation, comps, q, size);
            }
            const f32 mid = 0.5f * (piece.u0 + piece.u1);
            const bool atFloor = (piece.u1 - piece.u0) * (t1 - t0) < 2.0f * kMinSpan;
            if (close || piece.depth >= kMaxDepth || atFloor) {
                if (piece.u1 < 1.0f) {
                    push(t0 + (t1 - t0) * piece.u1, b);
                }
                continue;
            }
            // The later half goes on first so the earlier one is taken next.
            stack.push_back({mid, piece.u1, piece.depth + 1});
            stack.push_back({piece.u0, mid, piece.depth + 1});
        }
    }
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout

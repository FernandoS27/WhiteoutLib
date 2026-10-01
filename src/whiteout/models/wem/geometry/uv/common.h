// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// Shared inside the UV module and nowhere else: the triangles a face is drawn
/// as, and the two areas every one of these files measures.

#include <whiteout/models/wem/geometry/uv/islands.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace uv {
namespace detail {

/// One triangle of a face, by the corners it is drawn from: a corner carries
/// the UV, so a triangle of vertices alone would not do.
struct Tri {
    HalfedgeId corner[3];
};

/// The triangles @p face is drawn and written as -- the stored triangulation
/// where the mesh has one, a fan where it has not. Never a fresh cut: the file
/// and the viewport must agree on what the polygon is.
inline std::vector<Tri> TrianglesOf(const Mesh& mesh, FaceId face) {
    std::vector<Tri> out;
    const Topology& topology = std::as_const(mesh).topology();
    HalfedgeId loop[32];
    VertexId loopVertex[32];
    u32 valence = 0;
    for (const HalfedgeId h : topology.fh(face)) {
        if (valence < 32) {
            loop[valence] = h;
            loopVertex[valence] = topology.from(h);
        }
        ++valence;
    }
    if (valence < 3) {
        return out;
    }
    valence = std::min<u32>(valence, 32);

    const std::span<const u32> row = mesh.triangulation.row(face.value());
    if (!row.empty()) {
        out.reserve(row.size() / 3);
        for (std::size_t t = 0; t + 2 < row.size(); t += 3) {
            Tri tri;
            bool ok = true;
            for (u32 k = 0; k < 3; ++k) {
                ok = false;
                for (u32 i = 0; i < valence; ++i) {
                    if (loopVertex[i].value() == row[t + k]) {
                        tri.corner[k] = loop[i];
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    break;
                }
            }
            if (ok) {
                out.push_back(tri);
            }
        }
        if (!out.empty()) {
            return out;
        }
    }
    out.reserve(valence - 2);
    for (u32 i = 1; i + 1 < valence; ++i) {
        out.push_back(Tri{{loop[0], loop[i], loop[i + 1]}});
    }
    return out;
}

inline f32 TriArea3d(const Vector3f& a, const Vector3f& b, const Vector3f& c) {
    return cross(b - a, c - a).length() * 0.5f;
}

/// Signed, so a flip is a sign and not a size.
inline f32 TriAreaUv(const Vector2f& a, const Vector2f& b, const Vector2f& c) {
    return ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5f;
}

/// A triangle laid in its own plane, in doubles: the first corner at the
/// origin, the first edge along x, the third corner at positive y -- the
/// frame LSCM and SLIM both measure a triangle in. False for one with no area.
inline bool LocalTriangle(const Vector3f& a, const Vector3f& b, const Vector3f& c, f64 (&out)[3][2]) {
    const f64 e1[3] = {static_cast<f64>(b.x) - a.x, static_cast<f64>(b.y) - a.y, static_cast<f64>(b.z) - a.z};
    const f64 e2[3] = {static_cast<f64>(c.x) - a.x, static_cast<f64>(c.y) - a.y, static_cast<f64>(c.z) - a.z};
    const f64 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const f64 twiceArea = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const f64 length = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    if (!(twiceArea > 0.0) || !(length > 0.0)) {
        return false;
    }
    const f64 x = (e2[0] * e1[0] + e2[1] * e1[1] + e2[2] * e1[2]) / length;
    const f64 y = twiceArea / length;
    out[0][0] = 0.0;
    out[0][1] = 0.0;
    out[1][0] = length;
    out[1][1] = 0.0;
    out[2][0] = x;
    out[2][1] = y;
    return length * y != 0.0;
}

/// The texels of a @p width x @p height grid that a triangle (in texel units)
/// covers, each handed to @p fill(x, y): those whose centre is inside, and with
/// @p ring the ring round them as well -- the packer's conservative cover, so
/// no island is packed closer than it looks, where a check wants only what the
/// texture would show. The one rasteriser of the module.
template <class Fill>
void RasterTriangle(u32 width, u32 height, const Vector2f& a, const Vector2f& b, const Vector2f& c,
                    bool ring, Fill&& fill) {
    const f32 lowX = std::min(a.x, std::min(b.x, c.x));
    const f32 highX = std::max(a.x, std::max(b.x, c.x));
    const f32 lowY = std::min(a.y, std::min(b.y, c.y));
    const f32 highY = std::max(a.y, std::max(b.y, c.y));
    const i32 x0 = std::max(0, static_cast<i32>(std::floor(lowX)) - 1);
    const i32 x1 = std::min(static_cast<i32>(width) - 1, static_cast<i32>(std::ceil(highX)));
    const i32 y0 = std::max(0, static_cast<i32>(std::floor(lowY)) - 1);
    const i32 y1 = std::min(static_cast<i32>(height) - 1, static_cast<i32>(std::ceil(highY)));
    const f32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.0f) {
        return;
    }
    const f32 inverse = 1.0f / area;
    const f32 edge = ring ? -0.5f : 0.0f;
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const f32 px = static_cast<f32>(x) + 0.5f;
            const f32 py = static_cast<f32>(y) + 0.5f;
            const f32 w0 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inverse;
            const f32 w1 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inverse;
            const f32 w2 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * inverse;
            if (w0 >= edge && w1 >= edge && w2 >= edge) {
                fill(static_cast<u32>(x), static_cast<u32>(y));
            }
        }
    }
}

/// One island's two areas: what every density and every stretch is a ratio of.
struct IslandArea {
    f32 world = 0.0f;
    f32 uv = 0.0f;     ///< Absolute, so a flipped triangle still counts as area.
    f32 signedUv = 0.0f;
};

inline IslandArea AreasOf(const Mesh& mesh, const UvIslands& islands, u32 island,
                          std::span<const Vector3f> positions, std::span<const Vector2f> uvs) {
    IslandArea out;
    const Topology& topology = std::as_const(mesh).topology();
    for (const u32 face : islands.facesOf(island)) {
        for (const Tri& tri : TrianglesOf(mesh, FaceId(face))) {
            const Vector3f p[3] = {positions[topology.from(tri.corner[0]).index()],
                                   positions[topology.from(tri.corner[1]).index()],
                                   positions[topology.from(tri.corner[2]).index()]};
            out.world += TriArea3d(p[0], p[1], p[2]);
            if (uvs.empty()) {
                continue;
            }
            const f32 area = TriAreaUv(uvs[tri.corner[0].index()], uvs[tri.corner[1].index()],
                                       uvs[tri.corner[2].index()]);
            out.uv += std::abs(area);
            out.signedUv += area;
        }
    }
    return out;
}

} // namespace detail
} // namespace uv
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

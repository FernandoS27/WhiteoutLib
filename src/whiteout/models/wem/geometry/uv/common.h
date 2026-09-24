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

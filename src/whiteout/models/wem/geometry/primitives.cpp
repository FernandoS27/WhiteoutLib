// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/primitives.h>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

namespace {

constexpr f32 kTau = 6.28318530718f;

/// One shape under construction: its corners, its faces and the UV each corner
/// carries, so every primitive says the same three things.
struct Build {
    std::vector<Vector3f> places;
    std::vector<std::vector<u32>> faces;
    std::vector<std::vector<Vector2f>> uvs; ///< Per face, per corner.

    u32 place(const Vector3f& at) {
        places.push_back(at);
        return static_cast<u32>(places.size() - 1);
    }
    void face(std::vector<u32> loop, std::vector<Vector2f> map) {
        faces.push_back(std::move(loop));
        uvs.push_back(std::move(map));
    }
};

/// What every primitive comes back as (§3.16): built, marked `modelled`,
/// canonical, its rows materialised and its bounds computed.
Mesh finish(const Build& build, bool sharp) {
    FaceSet set;
    set.vertexCount = static_cast<u32>(build.places.size());
    for (const std::vector<u32>& loop : build.faces) {
        set.addFace(loop);
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> positions =
        mesh.attributes.getOrCreate<Vector3f>(names::kPosition, Domain::Vertex, AttrType::F32x3);
    std::copy(build.places.begin(), build.places.end(), positions.begin());
    mesh.attributes.getOrCreate<u32>(names::kSection, Domain::Face, AttrType::U32);
    mesh.sections.emplace_back();
    if (!mesh.ensureConnectivity().ok()) {
        return mesh;
    }
    // The UVs go on by corner, in the order the faces were given.
    {
        const std::span<Vector2f> uv =
            mesh.attributes.getOrCreate<Vector2f>(names::uv(0), Domain::Halfedge, AttrType::F32x2);
        const Topology& topology = std::as_const(mesh).topology();
        for (u32 f = 0; f < build.faces.size() && f < topology.faceCount(); ++f) {
            u32 corner = 0;
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                if (corner < build.uvs[f].size() && h.index() < uv.size()) {
                    uv[h.index()] = build.uvs[f][corner];
                }
                ++corner;
            }
        }
    }
    if (sharp) {
        // A box's edges are its shape; the round ones shade smooth (§3.16).
        const std::span<u8> flags =
            mesh.attributes.getOrCreate<u8>(names::kSharp, Domain::Edge, AttrType::Bool);
        std::fill(flags.begin(), flags.end(), static_cast<u8>(1));
    }
    mesh.attributes.getOrCreate<u8>(names::kModelled, Domain::Mesh, AttrType::Bool)[0] = 1;
    RecomputeNormals(mesh);
    Canonicalize(mesh);
    MaterialiseRows(mesh);
    mesh.recomputeBounds();
    return mesh;
}

} // namespace

Mesh MakeBox(const PrimitiveParams& params) {
    const Vector3f s = params.size;
    Build build;
    const u32 v[8] = {
        build.place({-s.x, -s.y, -s.z}), build.place({s.x, -s.y, -s.z}),
        build.place({s.x, s.y, -s.z}),   build.place({-s.x, s.y, -s.z}),
        build.place({-s.x, -s.y, s.z}),  build.place({s.x, -s.y, s.z}),
        build.place({s.x, s.y, s.z}),    build.place({-s.x, s.y, s.z}),
    };
    // Each side gets its own 0-1 square, which is the unwrap a modeller expects
    // to paint on.
    const std::vector<Vector2f> square = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    build.face({v[0], v[3], v[2], v[1]}, square); // -Z
    build.face({v[4], v[5], v[6], v[7]}, square); // +Z
    build.face({v[0], v[1], v[5], v[4]}, square); // -Y
    build.face({v[1], v[2], v[6], v[5]}, square); // +X
    build.face({v[2], v[3], v[7], v[6]}, square); // +Y
    build.face({v[3], v[0], v[4], v[7]}, square); // -X
    return finish(build, /*sharp=*/true);
}

Mesh MakePlane(const PrimitiveParams& params) {
    const u32 cells = std::max(1u, params.segments);
    Build build;
    std::vector<std::vector<u32>> grid(cells + 1);
    for (u32 y = 0; y <= cells; ++y) {
        for (u32 x = 0; x <= cells; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(cells);
            const f32 t = static_cast<f32>(y) / static_cast<f32>(cells);
            grid[y].push_back(build.place({params.size.x * (u * 2.0f - 1.0f),
                                           params.size.y * (t * 2.0f - 1.0f), 0.0f}));
        }
    }
    for (u32 y = 0; y < cells; ++y) {
        for (u32 x = 0; x < cells; ++x) {
            const f32 u0 = static_cast<f32>(x) / static_cast<f32>(cells);
            const f32 u1 = static_cast<f32>(x + 1) / static_cast<f32>(cells);
            const f32 t0 = static_cast<f32>(y) / static_cast<f32>(cells);
            const f32 t1 = static_cast<f32>(y + 1) / static_cast<f32>(cells);
            build.face({grid[y][x], grid[y][x + 1], grid[y + 1][x + 1], grid[y + 1][x]},
                       {{u0, t0}, {u1, t0}, {u1, t1}, {u0, t1}});
        }
    }
    return finish(build, /*sharp=*/false);
}

Mesh MakeCylinder(const PrimitiveParams& params) {
    const u32 sides = std::max(3u, params.sides);
    const f32 radius = params.size.x;
    const f32 height = params.size.z;
    Build build;
    std::vector<u32> bottom;
    std::vector<u32> top;
    for (u32 i = 0; i < sides; ++i) {
        const f32 angle = kTau * static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 x = std::cos(angle) * radius;
        const f32 y = std::sin(angle) * radius;
        bottom.push_back(build.place({x, y, -height}));
        top.push_back(build.place({x, y, height}));
    }
    // The side is one wrapped strip; the caps are their own discs.
    for (u32 i = 0; i < sides; ++i) {
        const u32 next = (i + 1) % sides;
        const f32 u0 = static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 u1 = static_cast<f32>(i + 1) / static_cast<f32>(sides);
        build.face({bottom[i], bottom[next], top[next], top[i]},
                   {{u0, 0.0f}, {u1, 0.0f}, {u1, 1.0f}, {u0, 1.0f}});
    }
    std::vector<u32> lower;
    std::vector<Vector2f> lowerUv;
    std::vector<u32> upper;
    std::vector<Vector2f> upperUv;
    for (u32 i = 0; i < sides; ++i) {
        const u32 back = sides - 1 - i;
        const f32 angle = kTau * static_cast<f32>(back) / static_cast<f32>(sides);
        lower.push_back(bottom[back]);
        lowerUv.push_back({0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::sin(angle)});
        const f32 up = kTau * static_cast<f32>(i) / static_cast<f32>(sides);
        upper.push_back(top[i]);
        upperUv.push_back({0.5f + 0.5f * std::cos(up), 0.5f + 0.5f * std::sin(up)});
    }
    build.face(lower, lowerUv);
    build.face(upper, upperUv);
    return finish(build, /*sharp=*/false);
}

Mesh MakeCone(const PrimitiveParams& params) {
    const u32 sides = std::max(3u, params.sides);
    const f32 radius = params.size.x;
    const f32 height = params.size.z;
    Build build;
    std::vector<u32> ring;
    for (u32 i = 0; i < sides; ++i) {
        const f32 angle = kTau * static_cast<f32>(i) / static_cast<f32>(sides);
        ring.push_back(build.place({std::cos(angle) * radius, std::sin(angle) * radius, -height}));
    }
    const u32 apex = build.place({0.0f, 0.0f, height});
    for (u32 i = 0; i < sides; ++i) {
        const u32 next = (i + 1) % sides;
        const f32 u0 = static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 u1 = static_cast<f32>(i + 1) / static_cast<f32>(sides);
        build.face({ring[i], ring[next], apex}, {{u0, 0.0f}, {u1, 0.0f}, {(u0 + u1) * 0.5f, 1.0f}});
    }
    std::vector<u32> base;
    std::vector<Vector2f> baseUv;
    for (u32 i = 0; i < sides; ++i) {
        const u32 back = sides - 1 - i;
        const f32 angle = kTau * static_cast<f32>(back) / static_cast<f32>(sides);
        base.push_back(ring[back]);
        baseUv.push_back({0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::sin(angle)});
    }
    build.face(base, baseUv);
    return finish(build, /*sharp=*/false);
}

Mesh MakeSphere(const PrimitiveParams& params) {
    const u32 sides = std::max(3u, params.sides);
    const u32 bands = std::max(2u, params.segments + 1);
    const f32 radius = params.size.x;
    Build build;
    // Latitude and longitude: quads between the bands, and a fan of triangles
    // at each pole where the quads would be degenerate.
    const u32 south = build.place({0.0f, 0.0f, -radius});
    std::vector<std::vector<u32>> rings;
    for (u32 b = 1; b < bands; ++b) {
        const f32 phi = 3.14159265f * static_cast<f32>(b) / static_cast<f32>(bands);
        std::vector<u32> ring;
        for (u32 i = 0; i < sides; ++i) {
            const f32 angle = kTau * static_cast<f32>(i) / static_cast<f32>(sides);
            ring.push_back(build.place({std::sin(phi) * std::cos(angle) * radius,
                                        std::sin(phi) * std::sin(angle) * radius,
                                        -std::cos(phi) * radius}));
        }
        rings.push_back(std::move(ring));
    }
    const u32 north = build.place({0.0f, 0.0f, radius});
    const auto u = [&](u32 i) { return static_cast<f32>(i) / static_cast<f32>(sides); };
    const auto v = [&](u32 b) { return static_cast<f32>(b) / static_cast<f32>(bands); };
    for (u32 i = 0; i < sides; ++i) {
        const u32 next = (i + 1) % sides;
        // Each fan runs against the band beside it, which is what makes the
        // shared edge carry one halfedge each way.
        build.face({south, rings.front()[next], rings.front()[i]},
                   {{(u(i) + u(i + 1)) * 0.5f, 0.0f}, {u(i + 1), v(1)}, {u(i), v(1)}});
        build.face({north, rings.back()[i], rings.back()[next]},
                   {{(u(i) + u(i + 1)) * 0.5f, 1.0f}, {u(i), v(bands - 1)}, {u(i + 1), v(bands - 1)}});
    }
    for (u32 b = 0; b + 1 < rings.size(); ++b) {
        for (u32 i = 0; i < sides; ++i) {
            const u32 next = (i + 1) % sides;
            build.face({rings[b][i], rings[b][next], rings[b + 1][next], rings[b + 1][i]},
                       {{u(i), v(b + 1)}, {u(i + 1), v(b + 1)}, {u(i + 1), v(b + 2)}, {u(i), v(b + 2)}});
        }
    }
    return finish(build, /*sharp=*/false);
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// The Mesh workspace audit's fixtures and checks (EDIT_MODE_MESH_AUDIT_PLAN.md
/// §2.2-§2.3 in WhiteoutFlakes). Every builder returns a mesh with connectivity,
/// one section and fresh normals; the checks return "" when they hold, else a
/// short reason, so a sweep can write them into a CSV row.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/geometry/kernel.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/reflect_bytes.h>
#include <whiteout/models/wem/skinning/points.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace audit {

using namespace whiteout;
using namespace whiteout::models::wem;
using geom::Domain;
using geom::EdgeId;
using geom::ElementSet;
using geom::FaceId;
using geom::HalfedgeId;
using geom::ModelPlan;
using geom::ModelRefusal;
using geom::Topology;
using geom::VertexId;
using geom::kInvalidId;
using skinning::PointTable;

// ============================================================================
// Building
// ============================================================================

inline std::span<Vector3f> Positions(Mesh& mesh) {
    return mesh.attributes.getOrCreate<Vector3f>(geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
}
inline std::span<const Vector3f> Positions(const Mesh& mesh) {
    return mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
}

/// Normals from the geometry at the default angle, as an import leaves them.
inline void Shade(Mesh& mesh) {
    geom::RecomputeNormals(mesh, geom::ShadingAngle(mesh));
}

/// A mesh of @p corners over @p positions, one section, every face in section
/// @p sectionOf (or 0), connectivity built and normals fresh.
inline Mesh FromFaces(const std::vector<std::vector<u32>>& corners, const std::vector<Vector3f>& positions,
                      const std::vector<u32>& sectionOf = {}, u32 sections = 1) {
    geom::FaceSet faces;
    faces.vertexCount = static_cast<u32>(positions.size());
    for (const std::vector<u32>& face : corners) {
        faces.addFace(face);
    }
    Mesh mesh;
    mesh.name = "audit";
    mesh.setFaceSet(faces);
    std::span<Vector3f> written = Positions(mesh);
    std::copy(positions.begin(), positions.end(), written.begin());
    std::span<u32> section =
        mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    for (u32 f = 0; f < section.size(); ++f) {
        section[f] = f < sectionOf.size() ? sectionOf[f] : 0u;
    }
    for (u32 s = 0; s < sections; ++s) {
        MeshSection made;
        made.name = "section" + std::to_string(s);
        made.materialSlot = s;
        mesh.sections.push_back(made);
    }
    REQUIRE(mesh.ensureConnectivity().ok());
    Shade(mesh);
    return mesh;
}

/// F1: a @p w x @p h grid of unit quads in z = 0, (0,0) to (w,h). Vertex
/// (x, y) is `y * (w + 1) + x`; face (x, y) is `y * w + x`.
inline Mesh QuadGrid(u32 w, u32 h) {
    std::vector<Vector3f> positions;
    for (u32 y = 0; y <= h; ++y) {
        for (u32 x = 0; x <= w; ++x) {
            positions.push_back(Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
        }
    }
    const auto at = [&](u32 x, u32 y) { return y * (w + 1) + x; };
    std::vector<std::vector<u32>> faces;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            faces.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    return FromFaces(faces, positions);
}

/// F2: the same grid as triangles, ((x,y), (x+1,y), (x+1,y+1)) then
/// ((x,y), (x+1,y+1), (x,y+1)) per cell.
inline Mesh TriGrid(u32 w, u32 h) {
    std::vector<Vector3f> positions;
    for (u32 y = 0; y <= h; ++y) {
        for (u32 x = 0; x <= w; ++x) {
            positions.push_back(Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
        }
    }
    const auto at = [&](u32 x, u32 y) { return y * (w + 1) + x; };
    std::vector<std::vector<u32>> faces;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            faces.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1)});
            faces.push_back({at(x, y), at(x + 1, y + 1), at(x, y + 1)});
        }
    }
    return FromFaces(faces, positions);
}

/// The corner lists of @p set, one per face.
inline std::vector<std::vector<u32>> CornerLists(const geom::FaceSet& set) {
    std::vector<std::vector<u32>> out;
    u32 at = 0;
    for (const u32 valence : set.faceValence) {
        out.emplace_back(set.cornerVertex.begin() + at, set.cornerVertex.begin() + at + valence);
        at += valence;
    }
    return out;
}

/// F3: a flat patch of n-gons: a hexagon (column 0, rows 0..2), five quads and
/// a pentagon (15 on its top edge). `LShape` is the concave one.
inline Mesh MixedGrid() {
    // Vertices on a 4 x 2 lattice plus one extra on the pentagon's top edge.
    std::vector<Vector3f> p = {
        {0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0},     // 0..4
        {0, 1, 0}, {1, 1, 0}, {2, 1, 0}, {3, 1, 0}, {4, 1, 0},     // 5..9
        {0, 2, 0}, {1, 2, 0}, {2, 2, 0}, {3, 2, 0}, {4, 2, 0},     // 10..14
        {3.5f, 2, 0},                                              // 15
    };
    std::vector<std::vector<u32>> faces = {
        {0, 1, 6, 11, 10, 5}, // hexagon
        {1, 2, 7, 6},         // quad
        {2, 3, 8, 7},         // quad
        {3, 4, 9, 8},         // quad
        {6, 7, 12, 11},       // quad
        {7, 8, 13, 12},       // quad
        {8, 9, 14, 15, 13},   // pentagon (15 on its top edge)
    };
    return FromFaces(faces, p);
}

/// A concave L hexagon in z = 0: (0,0) (2,0) (2,1) (1,1) (1,2) (0,2); the
/// reflex corner is vertex 3.
inline Mesh LShape() {
    return FromFaces({{0, 1, 2, 3, 4, 5}}, {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}, {1, 2, 0}, {0, 2, 0}});
}

/// F4: the closed unit cube of six outward quads, corners at 0 and 1.
inline Mesh Box() {
    std::vector<Vector3f> p = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                               {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    std::vector<std::vector<u32>> faces = {
        {0, 3, 2, 1}, // z = 0, facing -z
        {4, 5, 6, 7}, // z = 1, facing +z
        {0, 1, 5, 4}, // y = 0
        {1, 2, 6, 5}, // x = 1
        {2, 3, 7, 6}, // y = 1
        {3, 0, 4, 7}, // x = 0
    };
    return FromFaces(faces, p);
}

enum class Caps { None, Ngon, Fan };

/// F5 / F6: a cylinder of @p sides around +z, radius 1, @p rings bands from
/// z = 0 to z = rings. Ring r, side s is vertex `r * sides + s`. Caps: none
/// (F6, an open tube), one n-gon per end, or a triangle fan round a pole.
inline Mesh Cylinder(u32 sides, u32 rings, Caps caps) {
    std::vector<Vector3f> p;
    for (u32 r = 0; r <= rings; ++r) {
        for (u32 s = 0; s < sides; ++s) {
            const f32 a = 6.28318530718f * static_cast<f32>(s) / static_cast<f32>(sides);
            p.push_back(Vector3f{std::cos(a), std::sin(a), static_cast<f32>(r)});
        }
    }
    const auto at = [&](u32 r, u32 s) { return r * sides + (s % sides); };
    std::vector<std::vector<u32>> faces;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < sides; ++s) {
            faces.push_back({at(r, s), at(r, s + 1), at(r + 1, s + 1), at(r + 1, s)});
        }
    }
    if (caps == Caps::Ngon) {
        std::vector<u32> bottom, top;
        for (u32 s = 0; s < sides; ++s) {
            bottom.push_back(at(0, sides - s));
            top.push_back(at(rings, s));
        }
        faces.push_back(bottom);
        faces.push_back(top);
    } else if (caps == Caps::Fan) {
        const u32 low = static_cast<u32>(p.size());
        p.push_back(Vector3f{0, 0, 0});
        const u32 high = static_cast<u32>(p.size());
        p.push_back(Vector3f{0, 0, static_cast<f32>(rings)});
        for (u32 s = 0; s < sides; ++s) {
            faces.push_back({low, at(0, s + 1), at(0, s)});
            faces.push_back({high, at(rings, s), at(rings, s + 1)});
        }
    }
    return FromFaces(faces, p);
}

/// F7: a UV sphere of radius 1, @p segments round and @p rings bands, with a
/// triangle fan at each pole.
inline Mesh UvSphere(u32 segments, u32 rings) {
    std::vector<Vector3f> p;
    p.push_back(Vector3f{0, 0, -1});
    for (u32 r = 1; r < rings; ++r) {
        const f32 phi = 3.14159265359f * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s < segments; ++s) {
            const f32 a = 6.28318530718f * static_cast<f32>(s) / static_cast<f32>(segments);
            p.push_back(Vector3f{std::sin(phi) * std::cos(a), std::sin(phi) * std::sin(a), -std::cos(phi)});
        }
    }
    const u32 top = static_cast<u32>(p.size());
    p.push_back(Vector3f{0, 0, 1});
    const auto at = [&](u32 r, u32 s) { return 1 + (r - 1) * segments + (s % segments); };
    std::vector<std::vector<u32>> faces;
    for (u32 s = 0; s < segments; ++s) {
        faces.push_back({0, at(1, s + 1), at(1, s)});
    }
    for (u32 r = 1; r + 1 < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            faces.push_back({at(r, s), at(r, s + 1), at(r + 1, s + 1), at(r + 1, s)});
        }
    }
    for (u32 s = 0; s < segments; ++s) {
        faces.push_back({at(rings - 1, s), at(rings - 1, s + 1), top});
    }
    return FromFaces(faces, p);
}

/// F8: a @p w x @p h quad grid cut along x = @p column: the faces right of
/// the column use their own copies of its vertices, coincident with the left
/// ones and sharing no edge -- the way an MDX import keeps bone-split twins.
/// Left vertex (x, y) is `y * (w + 1) + x`; the twin of (column, y) is
/// `(w + 1) * (h + 1) + y`.
inline Mesh Crack(u32 w, u32 h, u32 column) {
    std::vector<Vector3f> p;
    for (u32 y = 0; y <= h; ++y) {
        for (u32 x = 0; x <= w; ++x) {
            p.push_back(Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
        }
    }
    const u32 twins = static_cast<u32>(p.size());
    for (u32 y = 0; y <= h; ++y) {
        p.push_back(Vector3f{static_cast<f32>(column), static_cast<f32>(y), 0.0f});
    }
    const auto at = [&](u32 x, u32 y, bool right) {
        return (x == column && right) ? twins + y : y * (w + 1) + x;
    };
    std::vector<std::vector<u32>> faces;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const bool right = x >= column;
            faces.push_back({at(x, y, right), at(x + 1, y, right), at(x + 1, y + 1, right), at(x, y + 1, right)});
        }
    }
    return FromFaces(faces, p);
}

/// F10: a quad grid whose faces at y >= @p row are in section 1.
inline Mesh TwoSections(u32 w, u32 h, u32 row) {
    Mesh grid = QuadGrid(w, h);
    const std::vector<std::vector<u32>> faces = CornerLists(grid.faceSet());
    std::vector<u32> sectionOf;
    for (u32 f = 0; f < faces.size(); ++f) {
        sectionOf.push_back(f / w >= row ? 1u : 0u);
    }
    std::vector<Vector3f> p(Positions(grid).begin(), Positions(grid).end());
    return FromFaces(faces, p, sectionOf, 2);
}

/// F11: a double-sided card -- the unit quad twice, opposite windings, on four
/// vertices each: two shells whose points coincide.
inline Mesh DoubleCard() {
    return FromFaces({{0, 1, 2, 3}, {4, 7, 6, 5}},
                     {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}});
}

/// F12: two unit quads touching only at (1,1): two shells sharing a point.
inline Mesh TouchingQuads() {
    return FromFaces({{0, 1, 2, 3}, {4, 5, 6, 7}},
                     {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}});
}

/// F13: a closed smooth prism of @p sides (5: 72 degrees between sides) from
/// z = 0 to 2 with n-gon caps; every side edge marked soft (sharp = 0), the
/// cap rims sharp.
inline Mesh SmoothPrism(u32 sides = 5) {
    Mesh mesh = Cylinder(sides, 2, Caps::Ngon);
    std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(geom::names::kSharp, Domain::Edge, geom::AttrType::Bool);
    const Topology& t = std::as_const(mesh).topology();
    const auto p = Positions(std::as_const(mesh));
    for (u32 e = 0; e < t.edgeCount(); ++e) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const bool vertical = p[t.from(h).value()].z != p[t.to(h).value()].z;
        const bool rim = !vertical && (p[t.from(h).value()].z == 0.0f || p[t.from(h).value()].z == 2.0f);
        sharp[e] = rim ? 1 : 0;
    }
    geom::RecomputeNormals(mesh, 3.14159265f);
    return mesh;
}

/// F14: two unit quads side by side whose shared edge carries an extra vertex
/// at (1, 0.5): an interior valence-2 vertex (vertex 6).
inline Mesh ValenceTwo() {
    return FromFaces({{0, 1, 6, 4, 3}, {1, 2, 5, 4, 6}},
                     {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0}, {1, 0.5f, 0}});
}

/// F15: a 3 x 3 quad grid without its centre face: a 4-edge hole.
inline Mesh QuadHole() {
    Mesh grid = QuadGrid(3, 3);
    std::vector<std::vector<u32>> faces = CornerLists(grid.faceSet());
    faces.erase(faces.begin() + 4);
    std::vector<Vector3f> p(Positions(grid).begin(), Positions(grid).end());
    return FromFaces(faces, p);
}

/// F15: a triangle grid 3 x 3 without one interior triangle (cell (1,1)'s
/// first): a 3-edge hole.
inline Mesh TriangleHole() {
    Mesh grid = TriGrid(3, 3);
    std::vector<std::vector<u32>> faces = CornerLists(grid.faceSet());
    faces.erase(faces.begin() + 8);
    std::vector<Vector3f> p(Positions(grid).begin(), Positions(grid).end());
    return FromFaces(faces, p);
}

/// F16: a border vertex 0 at the origin with an open fan of three triangles
/// (faces 0, 1, 2 in fan order) over rim vertices 1..4 on a half circle.
inline Mesh OpenFan() {
    std::vector<Vector3f> p = {{0, 0, 0}};
    for (u32 k = 0; k < 4; ++k) {
        const f32 a = 3.14159265f * static_cast<f32>(k) / 3.0f;
        p.push_back(Vector3f{std::cos(a), std::sin(a), 0.0f});
    }
    return FromFaces({{0, 1, 2}, {0, 2, 3}, {0, 3, 4}}, p);
}

/// F21: slivers -- a needle triangle, a near-zero quad and collinear corners
/// on one strip.
inline Mesh Slivers() {
    return FromFaces({{0, 1, 2}, {1, 3, 4, 2}, {3, 5, 6, 4}},
                     {{0, 0, 0}, {1, 0, 0}, {1, 1e-4f, 0}, {1.00001f, 0, 0}, {1.00001f, 1e-4f, 0},
                      {2, 0, 0}, {2, 1e-4f, 0}});
}

// ============================================================================
// Fields (F17, F18): the P6 oracle
// ============================================================================

inline Vector2f UvField(const Vector3f& p) {
    return Vector2f{0.25f * p.x + 0.1f * p.z, 0.25f * p.y};
}
inline Vector2f Uv1Field(const Vector3f& p) {
    return Vector2f{0.5f * p.y, 0.5f * p.x};
}
inline std::array<u8, 4> ColorField(const Vector3f& p) {
    const auto byte = [](f32 v) { return static_cast<u8>(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
    return {byte(40.0f * p.x), byte(40.0f * p.y), byte(128.0f + 40.0f * p.z), 255};
}
/// Bone 1's weight at @p p: x over @p width, clamped to [0, 1]; bone 0 has the rest.
inline f32 SkinField(const Vector3f& p, f32 width) {
    return std::clamp(p.x / width, 0.0f, 1.0f);
}

/// Writes `uv0`, `uv1` and `color0` on every live corner from its vertex's
/// position (the shifted faces' `uv0` + (10, 0): a second island), and a two-
/// bone skin whose weight follows x across @p width.
inline void Fields(Mesh& mesh, f32 width, const std::vector<u32>& shiftedFaces = {}) {
    std::span<Vector2f> uv0 =
        mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    std::span<Vector2f> uv1 =
        mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(1), Domain::Halfedge, geom::AttrType::F32x2);
    std::span<std::array<u8, 4>> color = mesh.attributes.getOrCreate<std::array<u8, 4>>(
        geom::names::color(0), Domain::Halfedge, geom::AttrType::U8x4);
    const auto p = Positions(std::as_const(mesh));
    const Topology& t = std::as_const(mesh).topology();
    for (u32 f = 0; f < t.faceCount(); ++f) {
        if (t.isDeleted(FaceId(f))) {
            continue;
        }
        const bool shifted = std::find(shiftedFaces.begin(), shiftedFaces.end(), f) != shiftedFaces.end();
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            const Vector3f& at = p[t.from(h).value()];
            uv0[h.index()] = UvField(at) + Vector2f{shifted ? 10.0f : 0.0f, 0.0f};
            uv1[h.index()] = Uv1Field(at);
            color[h.index()] = ColorField(at);
        }
    }
    mesh.skin.reset(mesh.vertexCount());
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        const f32 w1 = SkinField(p[v], width);
        std::vector<geom::Influence> in;
        if (w1 < 1.0f) {
            in.push_back(geom::Influence{0, 1.0f - w1});
        }
        if (w1 > 0.0f) {
            in.push_back(geom::Influence{1, w1});
        }
        std::sort(in.begin(), in.end(), [](const auto& a, const auto& b) { return a.weight > b.weight; });
        mesh.skin.assignVertex(v, in);
    }
}

/// F9: `uv0` = the field, with the faces at x >= @p column on their own
/// island (+10): a UV seam along x = column with the vertices shared.
inline void SeamUvs(Mesh& mesh, f32 column) {
    std::vector<u32> right;
    const auto p = Positions(std::as_const(mesh));
    const Topology& t = std::as_const(mesh).topology();
    for (u32 f = 0; f < t.faceCount(); ++f) {
        if (t.isDeleted(FaceId(f))) {
            continue;
        }
        Vector3f c{0, 0, 0};
        u32 n = 0;
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            c = c + p[t.from(h).value()];
            ++n;
        }
        if (c.x / static_cast<f32>(n) > column) {
            right.push_back(f);
        }
    }
    Fields(mesh, 1.0f, right);
}

// ============================================================================
// Selecting
// ============================================================================

/// The live vertex at exactly (x, y, z), the first in id order; kInvalidId when none.
inline u32 VertexAt(const Mesh& mesh, f32 x, f32 y, f32 z = 0.0f, u32 skip = 0) {
    const auto p = Positions(mesh);
    for (u32 v = 0; v < p.size(); ++v) {
        if (!mesh.topology().isDeleted(VertexId(v)) && p[v].x == x && p[v].y == y && p[v].z == z) {
            if (skip == 0) {
                return v;
            }
            --skip;
        }
    }
    return kInvalidId;
}

/// The edge from vertex @p a to @p b; kInvalidId when there is none.
inline u32 EdgeOf(const Mesh& mesh, u32 a, u32 b) {
    const HalfedgeId h = mesh.topology().findHalfedge(VertexId(a), VertexId(b));
    return h.valid() ? Topology::edge(h).value() : kInvalidId;
}

/// The edge between the vertices at (ax, ay) and (bx, by) in z = 0.
inline u32 EdgeAt(const Mesh& mesh, f32 ax, f32 ay, f32 bx, f32 by) {
    return EdgeOf(mesh, VertexAt(mesh, ax, ay), VertexAt(mesh, bx, by));
}

inline ElementSet Vertices(std::vector<u32> ids) {
    ElementSet out;
    out.vertices = std::move(ids);
    out.normalise();
    return out;
}
inline ElementSet Edges(std::vector<u32> ids) {
    ElementSet out;
    out.edges = std::move(ids);
    out.normalise();
    return out;
}
inline ElementSet Faces(std::vector<u32> ids) {
    ElementSet out;
    out.faces = std::move(ids);
    out.normalise();
    return out;
}

inline ElementSet AllFaces(const Mesh& mesh) {
    ElementSet out;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        if (!mesh.topology().isDeleted(FaceId(f))) {
            out.faces.push_back(f);
        }
    }
    return out;
}
inline ElementSet AllEdges(const Mesh& mesh) {
    ElementSet out;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (!mesh.topology().isDeleted(EdgeId(e))) {
            out.edges.push_back(e);
        }
    }
    return out;
}
inline ElementSet AllVertices(const Mesh& mesh) {
    ElementSet out;
    for (u32 v = 0; v < mesh.topology().vertexCount(); ++v) {
        if (!mesh.topology().isDeleted(VertexId(v))) {
            out.vertices.push_back(v);
        }
    }
    return out;
}
/// Every boundary edge.
inline ElementSet BorderEdges(const Mesh& mesh) {
    ElementSet out;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (!mesh.topology().isDeleted(EdgeId(e)) && mesh.topology().isBoundary(EdgeId(e))) {
            out.edges.push_back(e);
        }
    }
    return out;
}

inline PointTable PointsOf(const Mesh& mesh) {
    return skinning::BuildPointTable(mesh);
}

inline u32 LiveFaces(const Mesh& mesh) {
    return static_cast<u32>(AllFaces(mesh).faces.size());
}
inline u32 LiveVertices(const Mesh& mesh) {
    return static_cast<u32>(AllVertices(mesh).vertices.size());
}

// ============================================================================
// The checks (§2.2). Each returns "" when it holds.
// ============================================================================

/// P1: structural and manifold errors, as a histogram.
inline std::string CheckStructure(const Mesh& mesh) {
    Diagnostics diagnostics;
    geom::CheckStructural(mesh, 0, diagnostics);
    geom::CheckManifold(mesh, 0, diagnostics);
    return diagnostics.hasErrors() ? diagnostics.formatHistogram() : std::string();
}

/// P2: canonical numbering and current triangulation rows.
inline std::string CheckCanonical(const Mesh& mesh) {
    if (!geom::IsCanonical(mesh)) {
        return "not canonical";
    }
    Diagnostics stale;
    geom::CheckStructural(mesh, 0, stale);
    if (stale.countOf(DiagCode::StaleTriangulation) != 0) {
        return "stale triangulation";
    }
    return {};
}

inline bool Finite(const Vector3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// P3: positions, UVs and normals finite; normals of unit length.
inline std::string CheckFinite(const Mesh& mesh) {
    for (const Vector3f& p : Positions(mesh)) {
        if (!Finite(p)) {
            return "position not finite";
        }
    }
    for (u32 set = 0; set < 8; ++set) {
        const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(set), Domain::Halfedge);
        for (const Vector2f& value : uv) {
            if (!std::isfinite(value.x) || !std::isfinite(value.y)) {
                return "uv not finite";
            }
        }
    }
    const auto normals = mesh.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    const Topology& t = mesh.topology();
    for (u32 h = 0; h < normals.size() && h < t.halfedgeCount(); ++h) {
        if (t.isDeleted(HalfedgeId(h)) || t.isBoundary(HalfedgeId(h))) {
            continue;
        }
        const Vector3f& n = normals[h];
        if (!Finite(n)) {
            return "normal not finite";
        }
        const f32 length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (std::abs(length - 1.0f) > 1e-3f) {
            return "normal not unit";
        }
    }
    return {};
}

inline Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline f32 Length(const Vector3f& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// The area of face @p f (Newell's vector's half length).
inline f32 FaceArea(const Mesh& mesh, u32 f) {
    const Topology& t = mesh.topology();
    const auto p = Positions(mesh);
    Vector3f sum{0, 0, 0};
    for (const HalfedgeId h : t.fh(FaceId(f))) {
        sum = sum + Cross(p[t.from(h).value()], p[t.to(h).value()]);
    }
    return 0.5f * Length(sum);
}

/// P4: faces of zero area and edges of zero length, counted relative to the
/// mesh's size (@p eps of its bounding diagonal).
inline std::string CheckDegenerate(const Mesh& mesh, f32 eps = 1e-6f) {
    const auto p = Positions(mesh);
    Vector3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Vector3f& v : p) {
        lo = Vector3f{std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = Vector3f{std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
    const f32 diagonal = std::max(Length(hi - lo), 1e-12f);
    const Topology& t = mesh.topology();
    u32 faces = 0, edges = 0;
    for (u32 f = 0; f < t.faceCount(); ++f) {
        if (!t.isDeleted(FaceId(f)) && FaceArea(mesh, f) <= eps * diagonal * diagonal) {
            ++faces;
        }
    }
    for (u32 e = 0; e < t.edgeCount(); ++e) {
        if (t.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        if (Length(p[t.from(h).value()] - p[t.to(h).value()]) <= eps * diagonal) {
            ++edges;
        }
    }
    if (faces == 0 && edges == 0) {
        return {};
    }
    return std::to_string(faces) + " zero-area faces, " + std::to_string(edges) + " zero-length edges";
}

/// The shells (edge-connected face sets) of @p mesh, per vertex: the shell of
/// any face round it; kInvalidId for a vertex with no face.
inline std::vector<u32> ShellOfVertex(const Mesh& mesh) {
    const Topology& t = mesh.topology();
    std::vector<u32> parent(t.faceCount());
    std::iota(parent.begin(), parent.end(), 0u);
    std::function<u32(u32)> find = [&](u32 x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    for (u32 e = 0; e < t.edgeCount(); ++e) {
        if (t.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId a = Topology::halfedge(EdgeId(e), 0), b = Topology::halfedge(EdgeId(e), 1);
        if (!t.isBoundary(a) && !t.isBoundary(b)) {
            parent[find(t.face(a).value())] = find(t.face(b).value());
        }
    }
    std::vector<u32> out(t.vertexCount(), kInvalidId);
    for (u32 f = 0; f < t.faceCount(); ++f) {
        if (t.isDeleted(FaceId(f))) {
            continue;
        }
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            out[t.from(h).value()] = find(f);
        }
    }
    return out;
}

/// P5's measure: how many points join vertices of two or more shells.
inline u32 PointsAcrossShells(const Mesh& mesh) {
    const PointTable points = PointsOf(mesh);
    const std::vector<u32> shell = ShellOfVertex(mesh);
    u32 out = 0;
    for (u32 point = 0; point < points.pointCount; ++point) {
        std::set<u32> shells;
        for (const u32 v : points.membersOf(point)) {
            if (v < shell.size() && shell[v] != kInvalidId) {
                shells.insert(shell[v]);
            }
        }
        out += shells.size() > 1 ? 1u : 0u;
    }
    return out;
}

/// P6: the largest distance of any live corner's `uv0` from the field (on the
/// island the corner's face sits on: +10 when the face's other corners are).
inline f32 UvFieldError(const Mesh& mesh) {
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    if (uv.empty()) {
        return 0.0f;
    }
    const auto p = Positions(mesh);
    const Topology& t = mesh.topology();
    f32 worst = 0.0f;
    for (u32 f = 0; f < t.faceCount(); ++f) {
        if (t.isDeleted(FaceId(f))) {
            continue;
        }
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            const Vector2f want = UvField(p[t.from(h).value()]);
            Vector2f got = uv[h.index()];
            if (got.x > 5.0f) {
                got.x -= 10.0f;
            }
            worst = std::max(worst, std::max(std::abs(got.x - want.x), std::abs(got.y - want.y)));
        }
    }
    return worst;
}

/// P6 for `color0`: the largest channel difference from the field.
inline u32 ColorFieldError(const Mesh& mesh) {
    const auto color = mesh.attributes.get<const std::array<u8, 4>>(geom::names::color(0), Domain::Halfedge);
    if (color.empty()) {
        return 0;
    }
    const auto p = Positions(mesh);
    const Topology& t = mesh.topology();
    u32 worst = 0;
    for (u32 f = 0; f < t.faceCount(); ++f) {
        if (t.isDeleted(FaceId(f))) {
            continue;
        }
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            const std::array<u8, 4> want = ColorField(p[t.from(h).value()]);
            for (u32 k = 0; k < 4; ++k) {
                worst = std::max(worst, static_cast<u32>(std::abs(int(color[h.index()][k]) - int(want[k]))));
            }
        }
    }
    return worst;
}

/// P6 for the skin: the largest difference of bone 1's weight from the field,
/// and whether every vertex's weights sum to 1.
inline f32 SkinFieldError(const Mesh& mesh, f32 width) {
    if (mesh.skin.empty()) {
        return 0.0f;
    }
    const auto p = Positions(mesh);
    f32 worst = 0.0f;
    for (u32 v = 0; v < mesh.skin.vertexCount() && v < p.size(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        f32 sum = 0.0f, w1 = 0.0f;
        for (const geom::Influence& in : mesh.skin.forVertex(v)) {
            sum += in.weight;
            w1 += in.bone == 1 ? in.weight : 0.0f;
        }
        worst = std::max(worst, std::abs(sum - 1.0f));
        worst = std::max(worst, std::abs(w1 - SkinField(p[v], width)));
    }
    return worst;
}

/// P7: the largest angle (degrees) between a live corner's normal and what a
/// fresh `RecomputeNormals` at the mesh's own angle gives, with the same flags.
inline f32 NormalStaleness(const Mesh& mesh) {
    const auto have = mesh.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    if (have.empty()) {
        return 0.0f;
    }
    Mesh fresh = mesh;
    geom::RecomputeNormals(fresh, geom::ShadingAngle(fresh));
    const auto want = fresh.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    const Topology& t = mesh.topology();
    f32 worst = 0.0f;
    for (u32 h = 0; h < have.size() && h < want.size(); ++h) {
        if (t.isDeleted(HalfedgeId(h)) || t.isBoundary(HalfedgeId(h))) {
            continue;
        }
        const f32 d = std::clamp(have[h].x * want[h].x + have[h].y * want[h].y + have[h].z * want[h].z, -1.0f, 1.0f);
        worst = std::max(worst, std::acos(d) * 57.2957795f);
    }
    return worst;
}

/// How many live edges carry `sharp`.
inline u32 SharpCount(const Mesh& mesh) {
    const auto sharp = mesh.attributes.get<const u8>(geom::names::kSharp, Domain::Edge);
    u32 out = 0;
    for (u32 e = 0; e < sharp.size(); ++e) {
        out += !mesh.topology().isDeleted(EdgeId(e)) && sharp[e] != 0 ? 1u : 0u;
    }
    return out;
}

/// Every drawn triangle as sorted positions, the whole list sorted: equal for
/// two meshes that draw the same.
using Tri = std::array<std::array<f32, 3>, 3>;
inline std::vector<Tri> Drawn(const Mesh& mesh) {
    std::vector<u32> triangles;
    geom::TriangulateMesh(mesh, triangles);
    const auto p = Positions(mesh);
    std::vector<Tri> out;
    for (std::size_t k = 0; k + 2 < triangles.size(); k += 3) {
        Tri tri;
        for (u32 c = 0; c < 3; ++c) {
            const Vector3f& v = p[triangles[k + c]];
            tri[c] = {v.x, v.y, v.z};
        }
        std::sort(tri.begin(), tri.end());
        out.push_back(tri);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// What a check run found: one reason per failed check, joined by "; ".
struct Verdict {
    bool refused = false;
    ModelRefusal refusal = ModelRefusal::None;
    std::string failed;
    void add(const char* check, const std::string& why) {
        if (!why.empty()) {
            failed += (failed.empty() ? "" : "; ") + std::string(check) + ": " + why;
        }
    }
};

/// What a tool is allowed to do, beyond §2.2's defaults.
struct Contract {
    bool degenerateOk = false;  ///< P4 off (Collapse's own targets, amount 0).
    bool joinsShellsOk = false; ///< P5 off (Weld).
    bool normalsOk = false;     ///< P7 off.
    f32 skinWidth = 0.0f;       ///< > 0: P6 on the skin field too.
};

/// Plans with @p plan on a copy of @p mesh, finishes it at @p crease, and runs
/// P1-P7 and P10 on the result. On a refusal the copy must be byte-equal to
/// @p mesh. @p out receives the finished mesh.
inline Verdict Run(const Mesh& mesh, const std::function<ModelPlan(Mesh&, const PointTable&)>& plan,
                   const Contract& contract = {}, Mesh* out = nullptr,
                   f32 crease = geom::kDefaultShadingAngle, std::optional<f32> amount = std::nullopt) {
    Verdict verdict;
    Mesh work = mesh;
    const PointTable points = PointsOf(work);
    const u32 shellsBefore = PointsAcrossShells(mesh);
    const f32 uvBefore = UvFieldError(mesh);
    ModelPlan made = plan(work, points);
    verdict.refused = made.refused();
    verdict.refusal = made.refusal;
    if (made.refused()) {
        if (!SameReflected(work, mesh)) {
            verdict.add("P10", "refused but changed the mesh");
        }
        if (out) {
            *out = std::move(work);
        }
        return verdict;
    }
    if (amount) {
        geom::AmountRange(work, made);
        geom::ApplyAmount(work, made, *amount);
    }
    geom::FinishTool(work, made, crease);
    // FinishTool refuses too (Chamfer at 0 puts the mesh back: Q3).
    if (made.refused()) {
        verdict.refused = true;
        verdict.refusal = made.refusal;
        if (!SameReflected(work, mesh)) {
            verdict.add("P10", "refused at the finish but changed the mesh");
        }
        if (out) {
            *out = std::move(work);
        }
        return verdict;
    }
    verdict.add("P1", CheckStructure(work));
    verdict.add("P2", CheckCanonical(work));
    verdict.add("P3", CheckFinite(work));
    if (!contract.degenerateOk) {
        verdict.add("P4", CheckDegenerate(work));
    }
    if (!contract.joinsShellsOk && PointsAcrossShells(work) > shellsBefore) {
        verdict.add("P5", std::to_string(PointsAcrossShells(work) - shellsBefore) + " points join shells");
    }
    const f32 uvAfter = UvFieldError(work);
    if (uvAfter > std::max(uvBefore, 1e-4f) * 1.01f + 1e-4f) {
        verdict.add("P6", "uv off the field by " + std::to_string(uvAfter));
    }
    if (contract.skinWidth > 0.0f) {
        const f32 skin = SkinFieldError(work, contract.skinWidth);
        if (skin > 1e-3f) {
            verdict.add("P6", "skin off the field by " + std::to_string(skin));
        }
    }
    if (!contract.normalsOk) {
        const f32 stale = NormalStaleness(work);
        if (stale > 1.0f) {
            verdict.add("P7", "normals " + std::to_string(stale) + " deg from fresh");
        }
    }
    for (const u32 v : made.selection.vertices) {
        if (v >= work.topology().vertexCount() || work.topology().isDeleted(VertexId(v))) {
            verdict.add("P8", "selected vertex not live");
            break;
        }
    }
    for (const u32 e : made.selection.edges) {
        if (e >= work.topology().edgeCount() || work.topology().isDeleted(EdgeId(e))) {
            verdict.add("P8", "selected edge not live");
            break;
        }
    }
    for (const u32 f : made.selection.faces) {
        if (f >= work.topology().faceCount() || work.topology().isDeleted(FaceId(f))) {
            verdict.add("P8", "selected face not live");
            break;
        }
    }
    if (out) {
        *out = std::move(work);
    }
    return verdict;
}

} // namespace audit

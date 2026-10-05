// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The Fracture's geometry (EDIT_MODE_FRACTURE_DESIGN.md §5; _PLAN.md F0):
/// the constrained triangulation, the cells, the winding number, and G-F1
/// and G-F2 on `CutPieces` at mesh level.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/bvh.h>
#include <whiteout/models/wem/geometry/cdt.h>
#include <whiteout/models/wem/geometry/fracture/cells.h>
#include <whiteout/models/wem/geometry/fracture/cut.h>
#include <whiteout/models/wem/geometry/fracture/fracture.h>
#include <whiteout/models/wem/geometry/fracture/seeds.h>
#include <whiteout/models/wem/geometry/fracture/winding.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/validate.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::models::wem;
namespace fr = whiteout::models::wem::geom::fracture;

namespace {

using geom::Domain;

f64 Dot(const Vector3d& a, const Vector3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3d D(const Vector3f& v) {
    return Vector3d(v.x, v.y, v.z);
}

// ----------------------------------------------------------------------------
// Meshes
// ----------------------------------------------------------------------------

/// Adds a box to @p b: six quads wound outward, with a UV per corner. With
/// @p split each face has corners of its own, as an `.mdx` import has.
void AddBox(geom::MeshBuilder& b, Vector3f lo, Vector3f hi, bool split, bool floor = true) {
    const u32 loops[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
                             {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    const auto corner = [&](u32 v) {
        return Vector3f((v & 1) ? hi.x : lo.x, (v & 2) ? hi.y : lo.y, (v & 4) ? hi.z : lo.z);
    };
    std::vector<geom::VertexId> shared;
    if (!split) {
        for (u32 v = 0; v < 8; ++v) {
            shared.push_back(b.addVertex(corner(v)));
        }
    }
    const Vector2f uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (u32 k = 0; k < 6; ++k) {
        if (!floor && k == 4) {
            continue;
        }
        std::array<geom::VertexId, 4> ids;
        for (u32 i = 0; i < 4; ++i) {
            ids[i] = split ? b.addVertex(corner(loops[k][i])) : shared[loops[k][i]];
        }
        const geom::FaceId f = b.addFace(ids, 0);
        for (u32 i = 0; i < 4; ++i) {
            b.setCornerAttr(f, i, "uv0", uvs[i]);
        }
    }
}

Mesh BoxMesh(Vector3f lo, Vector3f hi, bool split = false, bool floor = true) {
    geom::MeshBuilder b;
    b.addSection(MeshSection{});
    AddBox(b, lo, hi, split, floor);
    return b.build().mesh;
}

/// A closed box whose faces are split into quads at @p breaks, per axis.
void AddGridBox(geom::MeshBuilder& b, Vector3f lo, Vector3f hi,
                const std::array<std::vector<f32>, 3>& breaks) {
    const f32 low[3] = {lo.x, lo.y, lo.z};
    const f32 high[3] = {hi.x, hi.y, hi.z};
    std::map<std::array<f32, 3>, geom::VertexId> shared;
    const auto vertex = [&](std::array<f32, 3> p) {
        const auto found = shared.find(p);
        if (found != shared.end()) {
            return found->second;
        }
        const geom::VertexId id = b.addVertex(Vector3f(p[0], p[1], p[2]));
        shared.emplace(p, id);
        return id;
    };
    const auto steps = [&](u32 axis) {
        std::vector<f32> out = {low[axis]};
        for (f32 x : breaks[axis]) {
            if (x > low[axis] && x < high[axis]) {
                out.push_back(x);
            }
        }
        out.push_back(high[axis]);
        return out;
    };
    // u x v is the face's outward normal.
    const u32 axes[6][3] = {{0, 2, 1}, {0, 1, 2}, {1, 0, 2}, {1, 2, 0}, {2, 1, 0}, {2, 0, 1}};
    for (u32 k = 0; k < 6; ++k) {
        const u32 a = axes[k][0];
        const u32 u = axes[k][1];
        const u32 v = axes[k][2];
        const f32 at = (k & 1) ? high[a] : low[a];
        const std::vector<f32> us = steps(u);
        const std::vector<f32> vs = steps(v);
        for (std::size_t i = 0; i + 1 < us.size(); ++i) {
            for (std::size_t j = 0; j + 1 < vs.size(); ++j) {
                const auto point = [&](f32 pu, f32 pv) {
                    std::array<f32, 3> p{};
                    p[a] = at;
                    p[u] = pu;
                    p[v] = pv;
                    return vertex(p);
                };
                const std::array<geom::VertexId, 4> ids = {
                    point(us[i], vs[j]), point(us[i + 1], vs[j]), point(us[i + 1], vs[j + 1]),
                    point(us[i], vs[j + 1])};
                b.addFace(ids, 0);
            }
        }
    }
}

/// Two overlapping boxes in one mesh, each split where it meets the other, so
/// every face lies wholly inside or outside the other box.
Mesh TwoBoxes() {
    geom::MeshBuilder b;
    b.addSection(MeshSection{});
    const std::array<std::vector<f32>, 3> breaks = {std::vector<f32>{-0.5f, 0.5f},
                                                    std::vector<f32>{-0.6f, 0.6f},
                                                    std::vector<f32>{-0.7f, 0.7f}};
    AddGridBox(b, Vector3f(-1, -1, -1), Vector3f(0.5f, 1, 1), breaks);
    AddGridBox(b, Vector3f(-0.5f, -0.6f, -0.7f), Vector3f(1, 0.6f, 0.7f), breaks);
    return b.build().mesh;
}

Mesh SphereMesh() {
    geom::PrimitiveParams p;
    p.size = Vector3f(1, 1, 1);
    p.sides = 32;
    p.segments = 32;
    return geom::MakeSphere(p);
}

Mesh TorusMesh() {
    geom::LatheParams p;
    for (u32 i = 0; i < 12; ++i) {
        const f32 a = -6.28318530718f * static_cast<f32>(i) / 12.0f;
        p.profile.push_back(Vector2f(1.5f + 0.5f * std::cos(a), 0.5f * std::sin(a)));
    }
    p.profile.push_back(p.profile.front());
    p.sides = 24;
    p.caps = false;
    return geom::MakeLathe(p);
}

Mesh SheetMesh() {
    geom::PrimitiveParams p;
    p.size = Vector3f(1, 1, 1);
    p.segments = 4;
    return geom::MakePlane(p);
}

Mesh HemisphereMesh() {
    geom::LatheParams p;
    for (u32 i = 0; i <= 8; ++i) {
        const f32 a = 1.57079632679f * static_cast<f32>(i) / 8.0f;
        p.profile.push_back(Vector2f(std::cos(a), std::sin(a)));
    }
    p.sides = 24;
    p.caps = false;
    return geom::MakeLathe(p);
}

// ----------------------------------------------------------------------------
// Measures
// ----------------------------------------------------------------------------

struct Triangles {
    std::vector<Vector3d> corners;
    std::vector<u32> face;
};

Triangles TrianglesOf(const Mesh& mesh) {
    Triangles out;
    const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
    std::vector<u32> tri;
    geom::TriangulateMesh(mesh, tri, &out.face);
    for (u32 v : tri) {
        out.corners.push_back(D(positions[v]));
    }
    return out;
}

f64 AreaOf(const Vector3d& a, const Vector3d& b, const Vector3d& c) {
    const Vector3d n = cross(b - a, c - a);
    return 0.5 * std::sqrt(Dot(n, n));
}

f64 SignedVolume(const Vector3d& a, const Vector3d& b, const Vector3d& c) {
    return Dot(a, cross(b, c)) / 6.0;
}

f64 VolumeOf(const Mesh& mesh) {
    const Triangles t = TrianglesOf(mesh);
    f64 v = 0.0;
    for (std::size_t i = 0; i + 2 < t.corners.size(); i += 3) {
        v += SignedVolume(t.corners[i], t.corners[i + 1], t.corners[i + 2]);
    }
    return std::fabs(v);
}

f64 AreaOfMesh(const Mesh& mesh) {
    const Triangles t = TrianglesOf(mesh);
    f64 a = 0.0;
    for (std::size_t i = 0; i + 2 < t.corners.size(); i += 3) {
        a += AreaOf(t.corners[i], t.corners[i + 1], t.corners[i + 2]);
    }
    return a;
}

// ----------------------------------------------------------------------------
// The break, as BreakModel will run it
// ----------------------------------------------------------------------------

struct Broken {
    std::vector<fr::WindingMesh> winding;
    fr::VoronoiDiagram diagram;
    fr::CutResult cut;
    std::vector<Vector3d> seeds;
};

Broken Break(const std::vector<Mesh>& meshes, u32 pieces, u32 seed, f64 smallest = 0.1,
             u32 threads = 0) {
    Broken out;
    out.winding.resize(meshes.size());
    std::vector<geom::TriangleBvh> surfaces(meshes.size());
    Vector3d low(1e30, 1e30, 1e30);
    Vector3d high(-1e30, -1e30, -1e30);
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        out.winding[i] = fr::WindingMeshOf(meshes[i]);
        const Extent& box = out.winding[i].bounds;
        std::vector<Vector3d> probes;
        for (u32 k = 0; k < 64; ++k) {
            const auto unit = [&](u32 j) {
                return fr::HashUnit(fr::FractureHash(seed ^ 0xA5A5u, static_cast<u32>(i), 3 * k + j));
            };
            probes.push_back(Vector3d(box.minimum.x + unit(0) * (box.maximum.x - box.minimum.x),
                                      box.minimum.y + unit(1) * (box.maximum.y - box.minimum.y),
                                      box.minimum.z + unit(2) * (box.maximum.z - box.minimum.z)));
        }
        fr::OrientWinding(out.winding[i], probes);
        std::vector<u32> tri;
        geom::TriangulateMesh(meshes[i], tri);
        const auto positions =
            meshes[i].attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
        surfaces[i].build(tri, positions);
        low = Vector3d(std::min<f64>(low.x, box.minimum.x), std::min<f64>(low.y, box.minimum.y),
                       std::min<f64>(low.z, box.minimum.z));
        high = Vector3d(std::max<f64>(high.x, box.maximum.x), std::max<f64>(high.y, box.maximum.y),
                        std::max<f64>(high.z, box.maximum.z));
    }
    std::vector<fr::SeedTarget> seedTargets;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        fr::SeedTarget t;
        t.winding = &out.winding[i];
        t.surface = &surfaces[i];
        t.pieces = pieces;
        seedTargets.push_back(t);
    }
    fr::SeedOptions seedOptions;
    seedOptions.seed = seed;
    seedOptions.nearBlast = 0.0;
    seedOptions.threads = threads;
    out.seeds = fr::FractureSeeds(seedTargets, seedOptions);
    const Vector3d grow = (high - low) * 0.01;
    fr::VoronoiOptions voronoi;
    voronoi.threads = threads;
    out.diagram = fr::VoronoiCells(out.seeds, low - grow, high + grow, voronoi);
    std::vector<fr::CutTarget> targets;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        fr::CutTarget t;
        t.mesh = &meshes[i];
        t.winding = &out.winding[i];
        targets.push_back(t);
    }
    fr::CutOptions cutOptions;
    cutOptions.smallest = smallest;
    cutOptions.threads = threads;
    out.cut = fr::CutPieces(targets, out.diagram, cutOptions);
    return out;
}

// ----------------------------------------------------------------------------
// Checks
// ----------------------------------------------------------------------------

using Bits = std::array<u32, 3>;

Bits BitsOf(const Vector3f& p) {
    Bits b{};
    const f32 x = p.x + 0.0f;
    const f32 y = p.y + 0.0f;
    const f32 z = p.z + 0.0f;
    std::memcpy(&b[0], &x, 4);
    std::memcpy(&b[1], &y, 4);
    std::memcpy(&b[2], &z, 4);
    return b;
}

/// A piece's faces from every target, as position loops.
struct PieceFaces {
    std::vector<std::vector<Vector3f>> outside;
    std::vector<std::vector<Vector3f>> inside;
};

std::vector<PieceFaces> FacesByPiece(const fr::CutResult& cut) {
    std::vector<PieceFaces> out(cut.pieces.size());
    for (std::size_t ti = 0; ti < cut.meshes.size(); ++ti) {
        const Mesh& mesh = cut.meshes[ti];
        if (mesh.faceCount() == 0) {
            continue;
        }
        const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
        const auto made = mesh.attributes.get<u8>(geom::names::kFractureMade, Domain::Face);
        const geom::FaceSet& faces = mesh.faceSet();
        u32 corner = 0;
        for (u32 f = 0; f < faces.faceCount(); ++f) {
            std::vector<Vector3f> loop;
            for (u32 k = 0; k < faces.faceValence[f]; ++k) {
                loop.push_back(positions[faces.cornerVertex[corner + k]]);
            }
            corner += faces.faceValence[f];
            PieceFaces& piece = out[cut.facePiece[ti][f]];
            (made[f] != 0 ? piece.inside : piece.outside).push_back(std::move(loop));
        }
    }
    return out;
}

/// Directed edges, welded by position, that no face runs the other way, and
/// edges run more than once in one direction.
struct Openness {
    u32 open = 0;
    u32 repeated = 0;
    std::vector<std::pair<Vector3f, Vector3f>> openEdges;
};

Openness OpennessOf(const PieceFaces& piece) {
    std::map<std::pair<Bits, Bits>, u32> count;
    std::map<std::pair<Bits, Bits>, std::pair<Vector3f, Vector3f>> where;
    for (const auto* list : {&piece.outside, &piece.inside}) {
        for (const auto& loop : *list) {
            for (std::size_t k = 0; k < loop.size(); ++k) {
                const Bits a = BitsOf(loop[k]);
                const Bits b = BitsOf(loop[(k + 1) % loop.size()]);
                if (a == b) {
                    continue;
                }
                ++count[{a, b}];
                where[{a, b}] = {loop[k], loop[(k + 1) % loop.size()]};
            }
        }
    }
    Openness out;
    for (const auto& [edge, n] : count) {
        const auto back = count.find({edge.second, edge.first});
        const u32 m = back == count.end() ? 0 : back->second;
        if (m != n) {
            ++out.open;
            out.openEdges.push_back(where[edge]);
        }
        if (n > 1) {
            ++out.repeated;
        }
    }
    return out;
}

f64 LoopArea(const std::vector<Vector3f>& loop) {
    f64 a = 0.0;
    for (std::size_t k = 1; k + 1 < loop.size(); ++k) {
        a += AreaOf(D(loop[0]), D(loop[k]), D(loop[k + 1]));
    }
    return a;
}

f64 LoopVolume(const std::vector<Vector3f>& loop) {
    f64 v = 0.0;
    for (std::size_t k = 1; k + 1 < loop.size(); ++k) {
        v += SignedVolume(D(loop[0]), D(loop[k]), D(loop[k + 1]));
    }
    return v;
}

bool SameMesh(const Mesh& a, const Mesh& b) {
    const geom::FaceSet& fa = a.faceSet();
    const geom::FaceSet& fb = b.faceSet();
    if (fa.vertexCount != fb.vertexCount || fa.faceValence != fb.faceValence ||
        fa.cornerVertex != fb.cornerVertex) {
        return false;
    }
    const auto la = a.attributes.layers();
    const auto lb = b.attributes.layers();
    if (la.size() != lb.size()) {
        return false;
    }
    for (std::size_t i = 0; i < la.size(); ++i) {
        if (la[i].name != lb[i].name || la[i].data != lb[i].data) {
            return false;
        }
    }
    return a.skin.offsets == b.skin.offsets && a.skin.influences.size() == b.skin.influences.size();
}

struct Solid {
    std::string name;
    std::vector<Mesh> meshes;
    f64 volume = 0.0; ///< The union's.
    bool overlapping = false;
};

void CheckSolid(const Solid& solid, u32 pieces, u32 seed) {
    INFO(solid.name << " at " << pieces << " pieces, seed " << seed);
    const Broken broken = Break(solid.meshes, pieces, seed);
    const fr::CutResult& cut = broken.cut;
    REQUIRE(cut.failedFaces == 0);
    REQUIRE(!cut.pieces.empty());
    const std::vector<PieceFaces> faces = FacesByPiece(cut);

    // Outside area, and every source face accounted for.
    f64 sourceArea = 0.0;
    for (const Mesh& m : solid.meshes) {
        sourceArea += AreaOfMesh(m);
    }
    f64 outsideArea = 0.0;
    for (const PieceFaces& piece : faces) {
        for (const auto& loop : piece.outside) {
            outsideArea += LoopArea(loop);
        }
    }
    CHECK(std::fabs(outsideArea - sourceArea) <= 1e-5 * sourceArea);
    for (std::size_t ti = 0; ti < solid.meshes.size(); ++ti) {
        const auto source =
            cut.meshes[ti].attributes.get<u32>(geom::names::kFractureSource, Domain::Face);
        std::set<u32> seen(source.begin(), source.end());
        for (u32 f = 0; f < solid.meshes[ti].faceCount(); ++f) {
            CHECK(seen.count(f + 1) == 1);
        }
    }

    // Volumes: the union's, counting only the outside on the union's surface.
    f64 volume = 0.0;
    for (const PieceFaces& piece : faces) {
        for (const auto& loop : piece.inside) {
            volume += LoopVolume(loop);
        }
        for (const auto& loop : piece.outside) {
            if (solid.overlapping) {
                // Just outside the face: inside the union there, the face is
                // inside the other shell. (On the surface w is undefined.)
                Vector3d middle(0, 0, 0);
                Vector3d normal(0, 0, 0);
                for (std::size_t k = 0; k < loop.size(); ++k) {
                    const Vector3d p = D(loop[k]);
                    const Vector3d q = D(loop[(k + 1) % loop.size()]);
                    middle += p;
                    normal += Vector3d((p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x),
                                       (p.x - q.x) * (p.y + q.y));
                }
                const f64 length = std::sqrt(Dot(normal, normal));
                if (length > 0.0) {
                    middle = middle * (1.0 / static_cast<f64>(loop.size())) +
                             normal * (1e-4 / length);
                    if (fr::WindingNumber(broken.winding[0], middle) >= 0.5) {
                        continue;
                    }
                }
            }
            volume += LoopVolume(loop);
        }
    }
    CHECK(std::fabs(std::fabs(volume) - solid.volume) <= 1e-4 * solid.volume);

    // Each piece closed and two-manifold after a weld by position.
    if (!solid.overlapping) {
        for (std::size_t p = 0; p < faces.size(); ++p) {
            const Openness o = OpennessOf(faces[p]);
            INFO("piece " << p);
            CHECK(o.open == 0);
            CHECK(o.repeated == 0);
        }
    }

    // Inside faces either side of a crack are equal and opposite.
    std::map<std::array<Bits, 3>, std::vector<u32>> insideFaces;
    const auto canonical = [](std::array<Bits, 3> t) {
        while (!(t[0] <= t[1] && t[0] <= t[2])) {
            std::rotate(t.begin(), t.begin() + 1, t.end());
        }
        return t;
    };
    for (u32 p = 0; p < faces.size(); ++p) {
        for (const auto& loop : faces[p].inside) {
            REQUIRE(loop.size() == 3);
            insideFaces[canonical({BitsOf(loop[0]), BitsOf(loop[1]), BitsOf(loop[2])})].push_back(p);
        }
    }
    u32 unmatched = 0;
    for (const auto& [tri, owners] : insideFaces) {
        const auto back = insideFaces.find(canonical({tri[0], tri[2], tri[1]}));
        if (back == insideFaces.end() || back->second.size() != owners.size()) {
            ++unmatched;
            continue;
        }
        for (u32 p : owners) {
            CHECK(std::find(back->second.begin(), back->second.end(), p) == back->second.end());
        }
    }
    CHECK(unmatched == 0);

    // No piece under Smallest.
    f64 total = 0.0;
    for (const fr::CutPiece& piece : cut.pieces) {
        total += piece.measure;
    }
    const f64 threshold = 0.1 * total / static_cast<f64>(cut.pieces.size() + cut.merged);
    for (const fr::CutPiece& piece : cut.pieces) {
        CHECK(piece.measure >= threshold * 0.999);
    }

    // Byte-identical, whatever the threads.
    const Broken again = Break(solid.meshes, pieces, seed, 0.1, 1);
    REQUIRE(again.cut.meshes.size() == cut.meshes.size());
    for (std::size_t ti = 0; ti < cut.meshes.size(); ++ti) {
        CHECK(SameMesh(cut.meshes[ti], again.cut.meshes[ti]));
        CHECK(cut.facePiece[ti] == again.cut.facePiece[ti]);
    }
}

} // namespace

// ============================================================================
// The constrained triangulation
// ============================================================================

TEST_CASE("Fracture CDT: a square with a square hole", "[fracture][cdt]") {
    const std::vector<Vector2<f64>> points = {{0, 0}, {4, 0}, {4, 4}, {0, 4},
                                              {1, 1}, {3, 1}, {3, 3}, {1, 3}};
    const std::vector<u32> segments = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4};
    const geom::Cdt2d cdt = geom::ConstrainedTriangulation2d(points, segments);
    REQUIRE(cdt.ok());
    CHECK(cdt.regionCount == 2);
    std::vector<f64> area(cdt.regionCount, 0.0);
    for (std::size_t t = 0; t < cdt.regions.size(); ++t) {
        const auto& a = cdt.points[cdt.triangles[3 * t]];
        const auto& b = cdt.points[cdt.triangles[3 * t + 1]];
        const auto& c = cdt.points[cdt.triangles[3 * t + 2]];
        const f64 signedArea = 0.5 * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        CHECK(signedArea > 0.0);
        area[cdt.regions[t]] += signedArea;
    }
    std::sort(area.begin(), area.end());
    CHECK(std::fabs(area[0] - 4.0) < 1e-9);
    CHECK(std::fabs(area[1] - 12.0) < 1e-9);
}

TEST_CASE("Fracture CDT: crossing segments are split where they cross", "[fracture][cdt]") {
    const std::vector<Vector2<f64>> points = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    const std::vector<u32> segments = {0, 1, 1, 2, 2, 3, 3, 0, 0, 2, 1, 3};
    const geom::Cdt2d cdt = geom::ConstrainedTriangulation2d(points, segments);
    REQUIRE(cdt.ok());
    CHECK(cdt.points.size() == 5);
    CHECK(std::fabs(cdt.points[4].x - 1.0) < 1e-12);
    CHECK(std::fabs(cdt.points[4].y - 1.0) < 1e-12);
    CHECK(cdt.regionCount == 4);
}

TEST_CASE("Fracture CDT: collinear runs along the boundary", "[fracture][cdt]") {
    std::vector<Vector2<f64>> points;
    std::vector<u32> segments;
    // A square whose sides each carry five points.
    for (u32 side = 0; side < 4; ++side) {
        for (u32 i = 0; i < 5; ++i) {
            const f64 t = static_cast<f64>(i) / 5.0;
            switch (side) {
            case 0: points.push_back({t, 0}); break;
            case 1: points.push_back({1, t}); break;
            case 2: points.push_back({1 - t, 1}); break;
            default: points.push_back({0, 1 - t}); break;
            }
        }
    }
    const u32 n = static_cast<u32>(points.size());
    for (u32 i = 0; i < n; ++i) {
        segments.push_back(i);
        segments.push_back((i + 1) % n);
    }
    // And a segment across, through two of them.
    segments.push_back(2);
    segments.push_back(12);
    const geom::Cdt2d cdt = geom::ConstrainedTriangulation2d(points, segments);
    REQUIRE(cdt.ok());
    CHECK(cdt.regionCount == 2);
    f64 total = 0.0;
    for (std::size_t t = 0; t < cdt.regions.size(); ++t) {
        const auto& a = cdt.points[cdt.triangles[3 * t]];
        const auto& b = cdt.points[cdt.triangles[3 * t + 1]];
        const auto& c = cdt.points[cdt.triangles[3 * t + 2]];
        total += 0.5 * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    }
    CHECK(std::fabs(total - 1.0) < 1e-12);
}

// ============================================================================
// The cells
// ============================================================================

TEST_CASE("Fracture cells partition the box", "[fracture][cells]") {
    std::vector<Vector3d> seeds;
    for (u32 i = 0; i < 40; ++i) {
        seeds.push_back(Vector3d(fr::HashUnit(fr::FractureHash(7, 0, 3 * i)) * 2 - 1,
                                 fr::HashUnit(fr::FractureHash(7, 0, 3 * i + 1)) * 2 - 1,
                                 fr::HashUnit(fr::FractureHash(7, 0, 3 * i + 2)) * 2 - 1));
    }
    for (fr::Grain grain : {fr::Grain::None, fr::Grain::Z}) {
        fr::VoronoiOptions options;
        options.grain = grain;
        const fr::VoronoiDiagram d =
            fr::VoronoiCells(seeds, Vector3d(-1, -1, -1), Vector3d(1, 1, 1), options);
        f64 volume = 0.0;
        for (const fr::ConvexCell& cell : d.cells) {
            REQUIRE(!cell.empty());
            for (const fr::CellFace& face : cell.faces) {
                for (std::size_t k = 1; k + 1 < face.loop.size(); ++k) {
                    volume += SignedVolume(cell.vertices[face.loop[0]], cell.vertices[face.loop[k]],
                                           cell.vertices[face.loop[k + 1]]);
                }
                if (fr::IsBoxSite(face.other)) {
                    continue;
                }
                // The cell across has the same face, its corners at the same bits.
                const fr::ConvexCell& other = d.cells[face.other];
                const auto match =
                    std::find_if(other.faces.begin(), other.faces.end(),
                                 [&](const fr::CellFace& f) { return f.plane == face.plane; });
                REQUIRE(match != other.faces.end());
                CHECK(match->loop.size() == face.loop.size());
                for (u32 v : face.loop) {
                    const Vector3d& p = cell.vertices[v];
                    const bool found = std::any_of(match->loop.begin(), match->loop.end(), [&](u32 w) {
                        const Vector3d& q = other.vertices[w];
                        return p.x == q.x && p.y == q.y && p.z == q.z;
                    });
                    CHECK(found);
                }
            }
        }
        CHECK(std::fabs(volume - 8.0) < 1e-9);
    }
}

TEST_CASE("Fracture InsetConvex moves every plane in", "[fracture][cells]") {
    std::vector<fr::HalfSpace> planes;
    for (u32 axis = 0; axis < 3; ++axis) {
        for (f64 sign : {-1.0, 1.0}) {
            fr::HalfSpace h;
            f64 n[3] = {0, 0, 0};
            n[axis] = sign;
            h.normal = Vector3d(n[0], n[1], n[2]);
            h.offset = 1.0;
            planes.push_back(h);
        }
    }
    const std::vector<Vector3d> corners =
        fr::InsetConvex(planes, 0.25, Vector3d(-2, -2, -2), Vector3d(2, 2, 2));
    REQUIRE(corners.size() == 8);
    for (const Vector3d& c : corners) {
        CHECK(std::fabs(std::fabs(c.x) - 0.75) < 1e-12);
        CHECK(std::fabs(std::fabs(c.y) - 0.75) < 1e-12);
        CHECK(std::fabs(std::fabs(c.z) - 0.75) < 1e-12);
    }
}

// ============================================================================
// Winding (§5.3)
// ============================================================================

TEST_CASE("Fracture winding: the design's table", "[fracture][winding]") {
    const Mesh cube = BoxMesh(Vector3f(-1, -1, -1), Vector3f(1, 1, 1));
    const fr::WindingMesh closed = fr::WindingMeshOf(cube);
    CHECK(std::fabs(fr::WindingNumber(closed, Vector3d(0.2, -0.3, 0.1)) - 1.0) < 1e-9);
    CHECK(std::fabs(fr::WindingNumber(closed, Vector3d(3, 0, 0))) < 1e-9);

    const fr::WindingMesh noFloor =
        fr::WindingMeshOf(BoxMesh(Vector3f(-1, -1, -1), Vector3f(1, 1, 1), false, false));
    const f64 centre = fr::WindingNumber(noFloor, Vector3d(0, 0, 0));
    CHECK(centre > 0.8);
    CHECK(centre < 0.87);

    const fr::WindingMesh sheet = fr::WindingMeshOf(SheetMesh());
    for (const Vector3d& p : {Vector3d(0, 0, 0.1), Vector3d(0, 0, -0.1), Vector3d(0.3, 0.2, 0.01)}) {
        CHECK(std::fabs(fr::WindingNumber(sheet, p)) < 0.5);
    }

    const fr::WindingMesh two = fr::WindingMeshOf(TwoBoxes());
    CHECK(std::fabs(fr::WindingNumber(two, Vector3d(0, 0, 0)) - 2.0) < 1e-9);
    CHECK(std::fabs(fr::WindingNumber(two, Vector3d(-0.8, 0, 0)) - 1.0) < 1e-9);

    // A cube wound inward reads -1, and the probes turn it round.
    geom::MeshBuilder b;
    b.addSection(MeshSection{});
    AddBox(b, Vector3f(1, 1, 1), Vector3f(-1, -1, -1), false);
    fr::WindingMesh inward = fr::WindingMeshOf(b.build().mesh);
    CHECK(std::fabs(fr::WindingNumber(inward, Vector3d(0, 0, 0)) + 1.0) < 1e-9);
    const std::vector<Vector3d> probes = {Vector3d(0, 0, 0), Vector3d(0.5, 0.5, 0.5),
                                          Vector3d(3, 3, 3)};
    CHECK(fr::OrientWinding(inward, probes));
    CHECK(std::fabs(fr::WindingNumber(inward, Vector3d(0, 0, 0)) - 1.0) < 1e-9);
}

// ============================================================================
// G-F1: solids
// ============================================================================

TEST_CASE("Fracture G-F1: solids break into closed pieces that add up", "[fracture][g-f1]") {
    std::vector<Solid> solids;
    {
        Solid s;
        s.name = "cube";
        s.meshes.push_back(BoxMesh(Vector3f(-1, -1, -1), Vector3f(1, 1, 1)));
        s.volume = 8.0;
        solids.push_back(std::move(s));
    }
    {
        Solid s;
        s.name = "sphere";
        s.meshes.push_back(SphereMesh());
        s.volume = VolumeOf(s.meshes[0]);
        solids.push_back(std::move(s));
    }
    {
        Solid s;
        s.name = "torus";
        s.meshes.push_back(TorusMesh());
        s.volume = VolumeOf(s.meshes[0]);
        solids.push_back(std::move(s));
    }
    {
        Solid s;
        s.name = "two overlapping boxes";
        s.meshes.push_back(TwoBoxes());
        // 1.5 x 2 x 2 and 1.5 x 1.2 x 1.4, overlapping 1 x 1.2 x 1.4.
        s.volume = 6.0 + 2.52 - 1.68;
        s.overlapping = true;
        solids.push_back(std::move(s));
    }
    {
        Solid s;
        s.name = "box split at its seams";
        s.meshes.push_back(BoxMesh(Vector3f(-1, -0.5f, -0.25f), Vector3f(1, 0.5f, 0.25f), true));
        s.volume = 1.0;
        solids.push_back(std::move(s));
    }
    for (const Solid& solid : solids) {
        for (u32 pieces : {2u, 16u, 100u}) {
            for (u32 seed : {1u, 2u, 3u}) {
                CheckSolid(solid, pieces, seed);
            }
        }
    }
}

// ============================================================================
// G-F2: open meshes and sheets
// ============================================================================

TEST_CASE("Fracture G-F2: a box with no floor is open only along its hole", "[fracture][g-f2]") {
    const std::vector<Mesh> meshes = {
        BoxMesh(Vector3f(-1, -1, -1), Vector3f(1, 1, 1), false, false)};
    for (u32 seed : {1u, 2u, 3u}) {
        const Broken broken = Break(meshes, 16, seed);
        REQUIRE(broken.cut.failedFaces == 0);
        u32 inside = 0;
        for (const PieceFaces& piece : FacesByPiece(broken.cut)) {
            inside += static_cast<u32>(piece.inside.size());
            for (const auto& [a, b] : OpennessOf(piece).openEdges) {
                CHECK(std::fabs(a.z + 1.0f) < 1e-5f);
                CHECK(std::fabs(b.z + 1.0f) < 1e-5f);
            }
        }
        CHECK(inside > 0);
    }
}

TEST_CASE("Fracture G-F2: a sheet breaks with no inside faces", "[fracture][g-f2]") {
    const std::vector<Mesh> meshes = {SheetMesh()};
    const f64 area = AreaOfMesh(meshes[0]);
    const Broken broken = Break(meshes, 16, 1);
    CHECK(broken.cut.pieces.size() > 1);
    f64 outside = 0.0;
    for (const PieceFaces& piece : FacesByPiece(broken.cut)) {
        CHECK(piece.inside.empty());
        for (const auto& loop : piece.outside) {
            outside += LoopArea(loop);
        }
    }
    CHECK(std::fabs(outside - area) <= 1e-5 * area);
}

TEST_CASE("Fracture G-F2: a hemisphere shell, and Thicken closes it", "[fracture][g-f2]") {
    Mesh shell = HemisphereMesh();
    {
        const Broken broken = Break({shell}, 16, 1);
        u32 inside = 0;
        u32 open = 0;
        for (const PieceFaces& piece : FacesByPiece(broken.cut)) {
            inside += static_cast<u32>(piece.inside.size());
            open += OpennessOf(piece).open;
        }
        // Recorded, not judged (G-F2): what a shell left as it is gives.
        std::printf("[G-F2] hemisphere shell: %zu pieces, %u inside faces, %u open edges\n",
                    broken.cut.pieces.size(), inside, open);
    }
    CHECK(fr::ThickenOpen(shell, 0.05) == 1);
    const Broken broken = Break({shell}, 16, 1);
    REQUIRE(broken.cut.failedFaces == 0);
    for (const PieceFaces& piece : FacesByPiece(broken.cut)) {
        const Openness o = OpennessOf(piece);
        CHECK(o.open == 0);
        CHECK(o.repeated == 0);
    }
}

TEST_CASE("Fracture G-F2: Thicken makes a sheet a slab of closed pieces", "[fracture][g-f2]") {
    Mesh sheet = SheetMesh();
    const f64 area = AreaOfMesh(sheet);
    CHECK(fr::ThickenOpen(sheet, 0.04) == 1);
    CHECK(std::fabs(VolumeOf(sheet) - area * 0.04) < 1e-4);
    const Broken broken = Break({sheet}, 16, 2);
    REQUIRE(broken.cut.failedFaces == 0);
    CHECK(broken.cut.pieces.size() > 1);
    for (const PieceFaces& piece : FacesByPiece(broken.cut)) {
        CHECK(!piece.inside.empty());
        const Openness o = OpennessOf(piece);
        CHECK(o.open == 0);
        CHECK(o.repeated == 0);
    }
}

TEST_CASE("Fracture timing on 20k triangles", "[.fracture-timing]") {
    geom::PrimitiveParams p;
    p.size = Vector3f(1, 1, 1);
    p.sides = 128;
    p.segments = 80;
    const std::vector<Mesh> meshes = {geom::MakeSphere(p)};
    std::vector<u32> tri;
    geom::TriangulateMesh(meshes[0], tri);
    for (u32 pieces : {16u, 100u, 250u}) {
        const auto start = std::chrono::steady_clock::now();
        const Broken broken = Break(meshes, pieces, 1);
        const f64 seconds =
            std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
        std::printf("[timing] %zu triangles, %u pieces asked: %zu pieces, %.3f s, %u failed faces\n",
                    tri.size() / 3, pieces, broken.cut.pieces.size(), seconds,
                    broken.cut.failedFaces);
    }
}

// ============================================================================
// F1: BreakModel, the rejoin, the kept whole meshes
// ============================================================================

namespace {

/// A document of one model: two bones up the z axis, and @p mesh skinned
/// across them by height, or wholly to the first.
Document SkinnedDocument(Mesh mesh, bool across = true) {
    Document document;
    document.models.emplace_back();
    Model& model = document.models[0];
    Node root;
    root.name = "Root";
    root.kind = NodeKind::Bone;
    root.resetPayloadForKind();
    model.nodes.add(root);
    Node top;
    top.name = "Top";
    top.kind = NodeKind::Bone;
    top.resetPayloadForKind();
    top.parent = 0;
    top.pivot = Vector3f(0, 0, 1);
    top.local.translation = Vector3f(0, 0, 1);
    model.nodes.add(top);
    const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
    mesh.skin.reset(static_cast<u32>(positions.size()));
    mesh.recomputeBounds();
    const f32 low = mesh.bounds.minimum.z;
    const f32 span = std::max(mesh.bounds.maximum.z - low, 1e-6f);
    for (u32 v = 0; v < positions.size(); ++v) {
        const f32 t = across ? (positions[v].z - low) / span : 0.0f;
        std::vector<geom::Influence> list;
        if (t > 0.0f) {
            list.push_back(geom::Influence{1, t});
        }
        if (t < 1.0f) {
            list.push_back(geom::Influence{0, 1.0f - t});
        }
        mesh.skin.assignVertex(v, list);
    }
    model.meshes.push_back(std::move(mesh));
    model.materialSlots = {"Outside", "Inside"};
    return document;
}

/// Each face as bytes: its corners from the lowest position round, each with
/// its position, its vertex's layers and skin, its corner's layers and its
/// edge's; then the face's own layers. Sorted, so two meshes compare up to
/// numbering. Only the layers @p names lists are read.
std::vector<std::string> FaceDescriptions(const Mesh& source, const std::set<std::string>& names) {
    Mesh mesh = source;
    mesh.invalidateConnectivity();
    REQUIRE(mesh.ensureConnectivity().ok());
    const geom::Topology& topology = std::as_const(mesh).topology();
    const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
    std::vector<const geom::AttrLayer*> layers;
    for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
        if (names.count(layer.name) != 0 && layer.domain != Domain::Mesh) {
            layers.push_back(&layer);
        }
    }
    std::sort(layers.begin(), layers.end(), [](const geom::AttrLayer* a, const geom::AttrLayer* b) {
        return a->name != b->name ? a->name < b->name : a->domain < b->domain;
    });
    const auto append = [](std::string& out, const geom::AttrLayer& layer, u32 element) {
        const u32 stride = geom::AttrTypeSize(layer.type);
        out.append(reinterpret_cast<const char*>(layer.data.data()) + static_cast<std::size_t>(element) * stride,
                   stride);
    };
    std::vector<std::string> out;
    for (u32 f = 0; f < mesh.faceCount(); ++f) {
        std::vector<geom::HalfedgeId> loop;
        const geom::HalfedgeId first = topology.halfedge(geom::FaceId(f));
        geom::HalfedgeId h = first;
        do {
            loop.push_back(h);
            h = topology.next(h);
        } while (h != first);
        std::size_t start = 0;
        for (std::size_t k = 1; k < loop.size(); ++k) {
            if (BitsOf(positions[topology.from(loop[k]).value()]) <
                BitsOf(positions[topology.from(loop[start]).value()])) {
                start = k;
            }
        }
        std::string text;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const geom::HalfedgeId c = loop[(start + i) % loop.size()];
            const u32 v = topology.from(c).value();
            for (const geom::AttrLayer* layer : layers) {
                if (layer->domain == Domain::Vertex) {
                    append(text, *layer, v);
                } else if (layer->domain == Domain::Halfedge) {
                    append(text, *layer, c.value());
                } else if (layer->domain == Domain::Edge) {
                    append(text, *layer, geom::Topology::edge(c).value());
                }
            }
            if (!mesh.skin.empty()) {
                std::vector<geom::Influence> list(mesh.skin.forVertex(v).begin(),
                                                  mesh.skin.forVertex(v).end());
                std::sort(list.begin(), list.end(),
                          [](const geom::Influence& a, const geom::Influence& b) { return a.bone < b.bone; });
                for (const geom::Influence& influence : list) {
                    text.append(reinterpret_cast<const char*>(&influence.bone), 4);
                    text.append(reinterpret_cast<const char*>(&influence.weight), 4);
                }
            }
            text.push_back('|');
        }
        for (const geom::AttrLayer* layer : layers) {
            if (layer->domain == Domain::Face) {
                append(text, *layer, f);
            }
        }
        out.push_back(std::move(text));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::set<std::string> LayerNames(const Mesh& mesh) {
    std::set<std::string> out;
    for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.name.rfind("fracture.", 0) != 0) {
            out.insert(layer.name);
        }
    }
    return out;
}

/// How many faces of @p a have no equal in @p b, or the other way round.
u32 FacesApart(const Mesh& a, const Mesh& b, const std::set<std::string>& names) {
    const std::vector<std::string> x = FaceDescriptions(a, names);
    const std::vector<std::string> y = FaceDescriptions(b, names);
    std::vector<std::string> only;
    std::set_symmetric_difference(x.begin(), x.end(), y.begin(), y.end(), std::back_inserter(only));
    return static_cast<u32>(only.size());
}

} // namespace

TEST_CASE("Fracture G-F1 at document level: pieces, bones, and the rejoin", "[fracture][g-f1]") {
    struct Case {
        std::string name;
        Mesh mesh;
    };
    std::vector<Case> cases;
    cases.push_back({"cube", BoxMesh(Vector3f(-1, -1, -1), Vector3f(1, 1, 1))});
    cases.push_back({"sphere", SphereMesh()});
    cases.push_back({"box split at its seams", BoxMesh(Vector3f(-1, -0.5f, -0.25f), Vector3f(1, 0.5f, 0.25f), true)});
    cases.push_back({"torus", TorusMesh()});
    for (const Case& c : cases) {
        for (u32 pieces : {2u, 16u, 100u}) {
            for (u32 seed : {1u, 2u, 3u}) {
                INFO(c.name << " at " << pieces << " pieces, seed " << seed);
                Document document = SkinnedDocument(c.mesh);
                const Mesh source = document.models[0].meshes[0];
                const u32 nodesBefore = document.models[0].nodes.size();
                geom::fracture::FractureSpec spec;
                spec.targets.push_back({0, static_cast<u16>(pieces)});
                spec.seed = seed;
                spec.keepWhole = false;
                const geom::fracture::FractureResult result =
                    geom::fracture::BreakModel(document, 0, spec);
                REQUIRE(result.ok());
                CHECK(result.failedFaces == 0);
                const Model& model = document.models[0];
                REQUIRE(model.meshes.size() == 1);
                CHECK(result.made[0] == 0);
                CHECK(result.made[1] == kInvalidIndex);
                CHECK(result.sources[0] == kInvalidIndex);
                // One bone per piece, under the helper when they share a parent,
                // else under the bone each rode most.
                const u32 helpers = result.helper != kInvalidNode ? 1 : 0;
                CHECK(model.nodes.size() == nodesBefore + helpers + result.pieces.size());
                for (const geom::fracture::FracturePiece& piece : result.pieces) {
                    REQUIRE(piece.node < model.nodes.size());
                    CHECK(model.nodes.nodes[piece.node].kind == NodeKind::Bone);
                    if (helpers != 0) {
                        CHECK(model.nodes.nodes[piece.node].parent == result.helper);
                    } else {
                        CHECK(model.nodes.nodes[piece.node].parent < nodesBefore);
                    }
                }
                // Every vertex rides one piece wholly; its source skin is in the layers.
                const Mesh& made = model.meshes[0];
                REQUIRE(made.skin.vertexCount() == made.vertexCount());
                for (u32 v = 0; v < made.vertexCount(); ++v) {
                    const auto list = made.skin.forVertex(v);
                    REQUIRE(list.size() == 1);
                    CHECK(list[0].weight == 1.0f);
                    CHECK(model.nodes.nodes[list[0].bone].name.find("_Piece") != std::string::npos);
                }
                for (const char* layer : {geom::names::kFractureSource, geom::names::kFractureMade}) {
                    CHECK(made.attributes.has(layer, Domain::Face));
                }
                for (const char* layer : {geom::names::kFractureCut, geom::names::kFractureWeld,
                                          geom::names::kFractureSkinNode,
                                          geom::names::kFractureSkinWeight}) {
                    CHECK(made.attributes.has(layer, Domain::Vertex));
                }
                CHECK(!Validate(document, ValidateLevel::Manifold).hasErrors());

                // Rejoined, it is the source up to numbering.
                Mesh rejoined = made;
                const geom::fracture::RejoinReport report =
                    geom::fracture::RejoinPieces(rejoined, result.skinNodes);
                CHECK(report.fragments == 0);
                CHECK(report.cutPoints == 0);
                // A source with no merge groups has every vertex its own point;
                // the rejoin writes that out as a layer.
                std::set<std::string> names = LayerNames(source);
                std::set<std::string> back = LayerNames(rejoined);
                if (names.count(geom::names::kMergeGroup) == 0) {
                    back.erase(geom::names::kMergeGroup);
                }
                CHECK(back == names);
                CHECK(rejoined.faceCount() == source.faceCount());
                CHECK(rejoined.vertexCount() == source.vertexCount());
                CHECK(FacesApart(rejoined, source, names) == 0);
            }
        }
    }
}

TEST_CASE("Fracture keeps the whole meshes, and gives them back", "[fracture][g-f1]") {
    Document document = SkinnedDocument(BoxMesh(Vector3f(-1, -1, -1), Vector3f(1, 1, 1)), false);
    document.models[0].meshes[0].name = "Crate";
    document.models[0].meshes[0].sections[0].profiles = 0x5;
    const Mesh source = document.models[0].meshes[0];
    geom::fracture::FractureSpec spec;
    spec.targets.push_back({0, 8});
    const auto result = geom::fracture::BreakModel(document, 0, spec);
    REQUIRE(result.ok());
    Model& model = document.models[0];
    // Every piece rode the root, so a helper named for the fracture stands between.
    REQUIRE(result.helper != kInvalidNode);
    CHECK(model.nodes.nodes[result.helper].name == "Crate");
    CHECK(model.nodes.nodes[result.helper].parent == 0);
    CHECK(model.nodes.nodes[result.pieces[0].node].name == "Crate_Piece01");
    REQUIRE(result.sources[0] == 1);
    Mesh& whole = model.meshes[1];
    CHECK(whole.name == "Crate (whole)");
    CHECK(whole.sections[0].profiles == 0);
    CHECK(model.meshes[0].sections[0].profiles == 0x5);
    geom::fracture::RestoreSource(whole, model.meshes[0]);
    CHECK(whole.name == "Crate");
    CHECK(FacesApart(whole, source, LayerNames(source)) == 0);
    CHECK(whole.sections[0].profiles == 0x5);
}

TEST_CASE("Fracture puts another material's inside faces in a mesh of their own", "[fracture]") {
    Document document = SkinnedDocument(SphereMesh());
    document.models[0].meshes[0].name = "Ball";
    geom::fracture::FractureSpec spec;
    spec.targets.push_back({0, 12});
    spec.inside = 1;
    spec.keepWhole = false;
    const auto result = geom::fracture::BreakModel(document, 0, spec);
    REQUIRE(result.ok());
    const Model& model = document.models[0];
    REQUIRE(result.made[1] == 1);
    const Mesh& outside = model.meshes[0];
    const Mesh& inside = model.meshes[1];
    CHECK(inside.name == "Ball Inside");
    for (const MeshSection& section : inside.sections) {
        CHECK(section.materialSlot == 1);
    }
    const auto madeOut = outside.attributes.get<u8>(geom::names::kFractureMade, Domain::Face);
    const auto madeIn = inside.attributes.get<u8>(geom::names::kFractureMade, Domain::Face);
    CHECK(std::all_of(madeOut.begin(), madeOut.end(), [](u8 m) { return m == 0; }));
    REQUIRE(!madeIn.empty());
    CHECK(std::all_of(madeIn.begin(), madeIn.end(), [](u8 m) { return m != 0; }));
    // Its UVs are laid flat at the outside's density, not left at zero.
    const auto uv = inside.attributes.get<Vector2f>(geom::names::uv(0), Domain::Halfedge);
    CHECK(std::any_of(uv.begin(), uv.end(), [](const Vector2f& p) { return p.x != 0.0f || p.y != 0.0f; }));
    CHECK(!Validate(document, ValidateLevel::Manifold).hasErrors());
}

TEST_CASE("Fracture refuses what it cannot break", "[fracture]") {
    Document document = SkinnedDocument(SheetMesh());
    document.models[0].meshes[0].sections[0].flags = SectionFlags::Billboard;
    geom::fracture::FractureSpec spec;
    spec.targets.push_back({0, 4});
    const auto billboard = geom::fracture::BreakModel(document, 0, spec);
    CHECK(billboard.refusal == geom::fracture::Refusal::Billboard);
    CHECK(document.models[0].meshes.size() == 1);
    spec.targets[0].mesh = 7;
    CHECK(geom::fracture::BreakModel(document, 0, spec).refusal == geom::fracture::Refusal::Missing);
}

// Corpus models broken at 16 and 100 pieces, one path a line from
// FRACTURE_CORPUS: what the app's census does, without the install.
TEST_CASE("Fracture over corpus models", "[.fracture-corpus]") {
    const char* list = std::getenv("FRACTURE_CORPUS");
    REQUIRE(list != nullptr);
    std::ifstream in(list);
    std::string path;
    while (std::getline(in, path)) {
        if (!path.empty() && path.back() == '\r') {
            path.pop_back();
        }
        if (path.empty() || path[0] == '#') {
            continue;
        }
        mdx::Parser parser;
        Result<Document> converted = MdxConverter().fromMdx(parser.parse(path));
        if (!converted.ok() || converted->models.empty()) {
            std::printf("%s unreadable\n", path.c_str());
            continue;
        }
        const Model& model = converted->models[0];
        u32 target = kInvalidIndex;
        u32 most = 0;
        for (u32 m = 0; m < model.meshes.size(); ++m) {
            u32 n = 0;
            for (u32 valence : model.meshes[m].faceSet().faceValence) {
                n += valence - 2;
            }
            if (n > most && geom::fracture::RefuseTarget(model, m) == geom::fracture::Refusal::None) {
                most = n;
                target = m;
            }
        }
        for (u16 pieces : {u16(16), u16(100)}) {
            Document document = *converted;
            geom::fracture::FractureSpec spec;
            spec.targets.push_back({target, pieces});
            const auto start = std::chrono::steady_clock::now();
            const auto result = geom::fracture::BreakModel(document, 0, spec);
            const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
            std::printf("%-60s %6u tris %3u asked %3zu pieces %.3f s %u failed\n", path.c_str(), most, pieces,
                        result.pieces.size(), seconds, result.failedFaces);
        }
    }
}

TEST_CASE("Fracture CDT from a dump", "[.cdt-dump]") {
    const char* file = std::getenv("WDX_CDT_DUMP");
    REQUIRE(file != nullptr);
    std::ifstream in(file);
    std::size_t n = 0, m = 0;
    u32 dropped = 0;
    in >> n >> m >> dropped;
    std::vector<Vector2<f64>> points(n);
    for (auto& p : points) {
        in >> p.x >> p.y;
    }
    std::vector<u32> segments(2 * m);
    for (auto& s : segments) {
        in >> s;
    }
    const geom::Cdt2d cdt = geom::ConstrainedTriangulation2d(points, segments);
    std::printf("dropped %u, %zu triangles, %u regions\n", cdt.droppedSegments, cdt.regions.size(),
                cdt.regionCount);
}

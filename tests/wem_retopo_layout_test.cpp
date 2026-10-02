// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// R3 (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.6): the traced layout. Whatever the
/// shape, every patch must come out a disk with four corners, and every arc
/// must border a patch on at least one side.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/retopo/quantize.h>

#include "whiteout/models/wem/geometry/retopo/cross_field.h"
#include "whiteout/models/wem/geometry/retopo/extract.h"
#include "whiteout/models/wem/geometry/retopo/layout.h"
#include "whiteout/models/wem/geometry/retopo/patch_map.h"
#include "whiteout/models/wem/geometry/retopo/surface.h"
#include "whiteout/models/wem/geometry/retopo/work_mesh.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace whiteout::models::wem::geom::retopo;

namespace {

Mesh Prepared(Mesh mesh) {
    geom::PrepareForModelling(mesh);
    mesh.ensureConnectivity();
    return mesh;
}

Mesh MakeTorus(f32 major, f32 minor, u32 around, u32 tube) {
    geom::MeshBuilder builder;
    std::vector<geom::VertexId> v;
    for (u32 i = 0; i < around; ++i) {
        const f32 a = 2.0f * 3.14159265f * i / around;
        for (u32 j = 0; j < tube; ++j) {
            const f32 b = 2.0f * 3.14159265f * j / tube;
            const f32 r = major + minor * std::cos(b);
            v.push_back(builder.addVertex({r * std::cos(a), r * std::sin(a), minor * std::sin(b)}));
        }
    }
    for (u32 i = 0; i < around; ++i) {
        for (u32 j = 0; j < tube; ++j) {
            const u32 i1 = (i + 1) % around;
            const u32 j1 = (j + 1) % tube;
            builder.addTriangle(v[i * tube + j], v[i1 * tube + j], v[i1 * tube + j1]);
            builder.addTriangle(v[i * tube + j], v[i1 * tube + j1], v[i * tube + j1]);
        }
    }
    Mesh mesh = builder.build().mesh;
    mesh.ensureConnectivity();
    return mesh;
}

/// An L of three unit squares, flat, in quads cut in two.
Mesh MakeL() {
    geom::MeshBuilder builder;
    std::vector<geom::VertexId> v;
    const u32 n = 9; // 0..2 in steps of 0.25
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            v.push_back(builder.addVertex({i * 0.25f, j * 0.25f, 0.0f}));
        }
    }
    for (u32 j = 0; j + 1 < n; ++j) {
        for (u32 i = 0; i + 1 < n; ++i) {
            if (i >= 4 && j >= 4) {
                continue; // the missing square
            }
            builder.addTriangle(v[j * n + i], v[j * n + i + 1], v[(j + 1) * n + i + 1]);
            builder.addTriangle(v[j * n + i], v[(j + 1) * n + i + 1], v[(j + 1) * n + i]);
        }
    }
    Mesh mesh = builder.build().mesh;
    mesh.ensureConnectivity();
    return mesh;
}

struct Run {
    Surface surface;
    WorkMesh mesh;
    CrossField field;
    Layout layout;
};

Run Lay(const Mesh& source, f64 length, FeatureOptions features = {}) {
    Run r;
    r.surface = BuildSurface(source, features);
    r.mesh = SeedWorkMesh(r.surface, 0);
    Remesh(r.mesh, r.surface, RemeshOptions{length, 6, 0.5});
    SplitFeatureCorners(r.mesh, r.surface);
    r.mesh.compact();
    FieldOptions field;
    field.scale = 3.0 * length;
    r.field = SolveCrossField(r.mesh, r.surface, field);
    LayoutOptions options;
    options.chord = 2.0 * length;
    options.verbose = std::getenv("RETOPO_VERBOSE") != nullptr;
    r.layout = BuildLayout(r.mesh, r.field, r.surface, options);
    return r;
}

/// With RETOPO_DUMP set, writes the layout as text for a look:
/// `v x y z`, `t a b c patch`, `e a b bits`, `n vertex`, `s vertex index`.
void Dump(const Run& r, const char* name) {
    const char* directory = std::getenv("RETOPO_DUMP");
    if (directory == nullptr) {
        return;
    }
    std::FILE* file = std::fopen((std::string(directory) + "/" + name + ".txt").c_str(), "w");
    if (file == nullptr) {
        return;
    }
    const WorkMesh& m = r.mesh;
    for (u32 v = 0; v < m.vertexCount(); ++v) {
        std::fprintf(file, "v %.9g %.9g %.9g\n", m.positions[v].x, m.positions[v].y, m.positions[v].z);
    }
    for (u32 t = 0; t < m.triangleCount(); ++t) {
        if (!m.dead[t]) {
            std::fprintf(file, "t %u %u %u %u\n", m.corners[3 * t], m.corners[3 * t + 1], m.corners[3 * t + 2],
                         t < r.layout.patchOf.size() ? r.layout.patchOf[t] : kNone);
        }
    }
    for (u32 h = 0; h < m.corners.size(); ++h) {
        if (!m.dead[h / 3] && m.isLayout(h) && (m.twins[h] == kNone || h < m.twins[h])) {
            std::fprintf(file, "e %u %u %u\n", m.from(h), m.to(h), m.features[h]);
        }
    }
    for (u32 v : r.layout.nodeVertex) {
        std::fprintf(file, "n %u\n", v);
    }
    for (u32 v = 0; v < r.field.index.size(); ++v) {
        if (r.field.index[v] != 0) {
            std::fprintf(file, "s %u %d\n", v, r.field.index[v]);
        }
    }
    std::fclose(file);
}

void CheckLayout(const Run& r) {
    INFO("patches " << r.layout.patches.size() << " arcs " << r.layout.arcs.size() << " separatrices "
                    << r.layout.stats.separatrices << " repairs " << r.layout.stats.repairTraces << " centres "
                    << r.layout.stats.centreSplits << " forced " << r.layout.stats.forcedCorners << " rounds "
                    << r.layout.stats.rounds << " singularities " << r.field.singularityCount());
    for (const std::string& note : r.layout.notes) {
        WARN(note);
    }
    REQUIRE(Validate(r.mesh).empty());
    CHECK(r.layout.stats.unfilled == 0);
    for (const LayoutPatch& patch : r.layout.patches) {
        if (!patch.rectangle) {
            u32 ones = 0;
            u32 big = 0;
            for (u32 h : patch.loop) {
                ones += r.layout.sectorTurns[h] == 1 ? 1 : 0;
                big += r.layout.sectorTurns[h] >= 3 ? 1 : 0;
            }
            UNSCOPED_INFO("unfilled: triangles " << patch.triangles.size() << " loops " << patch.loops << " euler "
                                                 << patch.euler << " corners " << ones << " concave " << big);
        }
        CHECK(patch.rectangle);
        if (!patch.rectangle) {
            continue;
        }
        for (const auto& side : patch.sides) {
            CHECK_FALSE(side.empty());
        }
    }
    for (const LayoutArc& arc : r.layout.arcs) {
        CHECK((arc.left != kNone || arc.right != kNone));
        CHECK(arc.vertices.size() >= 2);
    }
}

} // namespace

TEST_CASE("retopo layout debug: plane border sectors", "[.retopodebug]") {
    geom::PrimitiveParams params;
    params.segments = 2;
    Run r;
    r.surface = BuildSurface(Prepared(geom::MakePlane(params)), FeatureOptions{});
    r.mesh = SeedWorkMesh(r.surface, 0);
    Remesh(r.mesh, r.surface, RemeshOptions{0.1, 6, 0.5});
    SplitFeatureCorners(r.mesh, r.surface);
    r.mesh.compact();
    FieldOptions field;
    r.field = SolveCrossField(r.mesh, r.surface, field);
    LayoutOptions options;
    options.chord = 0.2;
    options.repairRounds = 0;
    r.layout = BuildLayout(r.mesh, r.field, r.surface, options);
    for (const LayoutPatch& patch : r.layout.patches) {
        for (u32 h : patch.loop) {
            const u8 k = r.layout.sectorTurns[h];
            if (k != 2) {
                const V3 p = r.mesh.positions[r.mesh.from(h)];
                WARN("k " << int(k) << " at " << p.x << " " << p.y);
            }
        }
    }
}

TEST_CASE("retopo layout debug: L first pass", "[.retopodebug]") {
    for (u32 rounds : {0u, 1u, 2u}) {
        Run r;
        r.surface = BuildSurface(MakeL(), FeatureOptions{});
        r.mesh = SeedWorkMesh(r.surface, 0);
        Remesh(r.mesh, r.surface, RemeshOptions{0.08, 6, 0.5});
        SplitFeatureCorners(r.mesh, r.surface);
        r.mesh.compact();
        FieldOptions field;
        r.field = SolveCrossField(r.mesh, r.surface, field);
        LayoutOptions options;
        options.chord = 0.16;
        options.repairRounds = rounds;
        r.layout = BuildLayout(r.mesh, r.field, r.surface, options);
        Dump(r, ("lshape_round" + std::to_string(rounds)).c_str());
        WARN("rounds " << rounds << " patches " << r.layout.patches.size() << " separatrices "
                       << r.layout.stats.separatrices << " repairs " << r.layout.stats.repairTraces);
        for (u32 id = 0; id < r.layout.patches.size(); ++id) {
            const LayoutPatch& patch = r.layout.patches[id];
            std::string ks;
            for (u32 h : patch.loop) {
                const u8 k = r.layout.sectorTurns[h];
                if (k != 2) {
                    const V3 p = r.mesh.positions[r.mesh.from(h)];
                    ks += " k" + std::to_string(k) + "@(" + std::to_string(p.x).substr(0, 5) + "," +
                          std::to_string(p.y).substr(0, 5) + ")";
                }
            }
            WARN("  patch " << id << " rect " << patch.rectangle << ks);
        }
    }
}

TEST_CASE("retopo layout debug: cylinder rounds", "[.retopodebug]") {
    for (u32 rounds : {0u, 1u, 2u, 3u}) {
        geom::PrimitiveParams params;
        params.sides = 24;
        params.size = Vector3f{0.5f, 0.5f, 1.0f};
        Run r;
        r.surface = BuildSurface(Prepared(geom::MakeCylinder(params)), FeatureOptions{});
        r.mesh = SeedWorkMesh(r.surface, 0);
        Remesh(r.mesh, r.surface, RemeshOptions{0.08, 6, 0.5});
        SplitFeatureCorners(r.mesh, r.surface);
        r.mesh.compact();
        FieldOptions field;
        field.scale = 0.24;
        r.field = SolveCrossField(r.mesh, r.surface, field);
        LayoutOptions options;
        options.chord = 0.16;
        options.repairRounds = rounds;
        r.layout = BuildLayout(r.mesh, r.field, r.surface, options);
        Dump(r, ("cylinder_round" + std::to_string(rounds)).c_str());
        WARN("rounds " << rounds << " patches " << r.layout.patches.size() << " separatrices "
                       << r.layout.stats.separatrices << " repairs " << r.layout.stats.repairTraces << " centres "
                       << r.layout.stats.centreSplits << " forced " << r.layout.stats.forcedCorners);
        for (u32 id = 0; id < r.layout.patches.size(); ++id) {
            const LayoutPatch& patch = r.layout.patches[id];
            if (patch.rectangle) {
                continue;
            }
            std::string ks;
            for (u32 h : patch.loop) {
                const u8 k = r.layout.sectorTurns[h];
                const u32 v = r.mesh.from(h);
                if (k != 2 || r.mesh.kinds[v] == VertexKind::Corner) {
                    const V3 p = r.mesh.positions[r.mesh.from(h)];
                    ks += " k" + std::to_string(k) + "i" + std::to_string(r.field.index[v]) + "@(" +
                          std::to_string(p.x).substr(0, 5) + "," +
                          std::to_string(p.y).substr(0, 5) + "," + std::to_string(p.z).substr(0, 5) + ")";
                }
            }
            if (patch.triangles.size() > 1000) {
                WARN("  patch " << id << " tris " << patch.triangles.size() << " loop " << patch.loop.size() << ks);
            }
        }
        if (rounds > 0) {
            break;
        }
    }
}

TEST_CASE("retopo patch map: a square lays flat, one to one",
          "[retopo][layout]") {
    geom::PrimitiveParams params;
    params.segments = 2;
    const Run r = Lay(Prepared(geom::MakePlane(params)), 0.1);
    REQUIRE(r.layout.patches.size() == 1);
    const LayoutPatch& patch = r.layout.patches[0];
    PatchMap map;
    REQUIRE(BuildPatchMap(r.mesh, patch, CircleBorder(r.mesh, patch), map));
    CHECK(map.borderCount == patch.loop.size());
    // One to one: every map triangle keeps its orientation.
    u32 flipped = 0;
    for (u32 t = 0; t < map.triangleCount(); ++t) {
        const P2& a = map.uv[map.corners[3 * t]];
        const P2& b = map.uv[map.corners[3 * t + 1]];
        const P2& c = map.uv[map.corners[3 * t + 2]];
        const f64 area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        flipped += area <= 0.0 ? 1 : 0;
    }
    CHECK(flipped == 0);
}

TEST_CASE("retopo layout: a box is six rectangles", "[retopo][layout]") {
    const Run r = Lay(Prepared(geom::MakeBox({})), 0.2);
    Dump(r, "box");
    CheckLayout(r);
    CHECK(r.layout.patches.size() == 6);
}

TEST_CASE("retopo layout: a plane is one rectangle", "[retopo][layout]") {
    geom::PrimitiveParams params;
    params.segments = 2;
    const Run r = Lay(Prepared(geom::MakePlane(params)), 0.1);
    Dump(r, "plane");
    CheckLayout(r);
    CHECK(r.layout.patches.size() == 1);
}

TEST_CASE("retopo layout: an L's concave corner is traced across", "[retopo][layout]") {
    const Run r = Lay(MakeL(), 0.08);
    Dump(r, "lshape");
    CheckLayout(r);
    CHECK(r.layout.patches.size() >= 2);
    CHECK(r.layout.patches.size() <= 3);
}

TEST_CASE("retopo layout: a sphere's separatrices make rectangles", "[retopo][layout]") {
    geom::PrimitiveParams params;
    params.sides = 24;
    params.segments = 12;
    FeatureOptions features;
    features.uvSeamSets = 0;
    const Run r = Lay(Prepared(geom::MakeSphere(params)), 0.1, features);
    Dump(r, "sphere");
    CheckLayout(r);
}

TEST_CASE("retopo layout: a torus is cut into rectangles", "[retopo][layout]") {
    const Run r = Lay(MakeTorus(2.0f, 0.7f, 32, 16), 0.15);
    Dump(r, "torus");
    CheckLayout(r);
}

TEST_CASE("retopo layout: a capped cylinder", "[retopo][layout]") {
    geom::PrimitiveParams params;
    params.sides = 24;
    params.size = Vector3f{0.5f, 0.5f, 1.0f};
    const Run r = Lay(Prepared(geom::MakeCylinder(params)), 0.08);
    Dump(r, "cylinder");
    CheckLayout(r);
}

/// With RETOPO_MODEL (an .mdx), RETOPO_MESH (its mesh index) and RETOPO_QUADS
/// set: that mesh's largest piece laid out as `Retopologize` lays it out,
/// dumped to RETOPO_DUMP with its unfilled patches (`u patch`) named.
TEST_CASE("retopo layout: a real piece", "[.retopomodel]") {
    const char* path = std::getenv("RETOPO_MODEL");
    if (path == nullptr) {
        SKIP("RETOPO_MODEL not set");
    }
    std::ifstream file(path, std::ios::binary);
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(bytes.empty());
    mdx::Parser parser;
    const mdx::Model model = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    const Result<Document> document = MdxConverter().fromMdx(model);
    REQUIRE(document.ok());
    const char* meshText = std::getenv("RETOPO_MESH");
    const u32 index = meshText != nullptr ? static_cast<u32>(std::atoi(meshText)) : 0;
    REQUIRE(index < document->models[0].meshes.size());
    const Mesh mesh = Prepared(document->models[0].meshes[index]);

    // Knobs for comparing: RETOPO_SEAMS=0 and RETOPO_SHARP=0 drop those
    // features, RETOPO_CURVATURE sets the curvature pull.
    FeatureOptions features;
    if (const char* seams = std::getenv("RETOPO_SEAMS")) {
        features.uvSeamSets = static_cast<u32>(std::atoi(seams));
    }
    if (const char* sharp = std::getenv("RETOPO_SHARP")) {
        features.sharp = std::atoi(sharp) != 0;
    }
    Run r;
    r.surface = BuildSurface(mesh, features);
    std::vector<f64> area(r.surface.componentCount, 0.0);
    for (u32 t = 0; t < r.surface.triangleCount(); ++t) {
        area[r.surface.components[t]] += r.surface.triangleAreas[t];
    }
    u32 piece = static_cast<u32>(std::max_element(area.begin(), area.end()) - area.begin());
    if (const char* chosen = std::getenv("RETOPO_PIECE")) {
        piece = std::min<u32>(static_cast<u32>(std::atoi(chosen)), static_cast<u32>(area.size() - 1));
    }
    {
        std::string sizes;
        for (u32 c = 0; c < area.size(); ++c) {
            u32 count = 0;
            for (u32 t = 0; t < r.surface.triangleCount(); ++t) {
                count += r.surface.components[t] == c ? 1 : 0;
            }
            sizes += " " + std::to_string(c) + ":" + std::to_string(count);
        }
        WARN("pieces (triangles):" << sizes);
    }
    f64 total = 0.0;
    for (f64 a : area) {
        total += a;
    }
    const char* quadsText = std::getenv("RETOPO_QUADS");
    const f64 quads = quadsText != nullptr ? std::atof(quadsText) : 0.5 * r.surface.triangleCount();
    const f64 edge = std::sqrt(total / quads);
    if (std::getenv("RETOPO_NOSCALE") == nullptr) {
        ScaleFeatures(r.surface, edge, features.cornerAngle);
    }
    const f64 perTriangle = std::sqrt(3.0) / 4.0;
    const f64 coarsest = std::sqrt(area[piece] / (perTriangle * 2500.0));
    const f64 finest = std::sqrt(area[piece] / (perTriangle * 250000.0));
    const f64 length = std::clamp(edge / 3.0, finest, std::max(finest, coarsest));
    r.mesh = SeedWorkMesh(r.surface, piece);
    Remesh(r.mesh, r.surface, RemeshOptions{length, 6, 0.5});
    SplitFeatureCorners(r.mesh, r.surface);
    r.mesh.compact();
    FieldOptions field;
    field.scale = edge;
    if (const char* curvature = std::getenv("RETOPO_CURVATURE")) {
        field.curvature = std::atof(curvature);
    }
    if (const char* cancel = std::getenv("RETOPO_CANCEL")) {
        field.cancelPairs = std::atof(cancel);
    }
    r.field = SolveCrossField(r.mesh, r.surface, field);
    LayoutOptions options;
    options.chord = 2.0 * length;
    options.verbose = std::getenv("RETOPO_VERBOSE") != nullptr;
    r.layout = BuildLayout(r.mesh, r.field, r.surface, options);
    u32 featureEdges = 0;
    for (u32 h = 0; h < r.mesh.corners.size(); ++h) {
        featureEdges += !r.mesh.dead[h / 3] && r.mesh.isFeature(h) ? 1 : 0;
    }

    const std::string name = "model" + std::to_string(index);
    Dump(r, name.c_str());
    if (const char* directory = std::getenv("RETOPO_DUMP")) {
        std::FILE* out = std::fopen((std::string(directory) + "/" + name + ".txt").c_str(), "a");
        if (out != nullptr) {
            for (u32 p = 0; p < r.layout.patches.size(); ++p) {
                if (!r.layout.patches[p].rectangle) {
                    std::fprintf(out, "u %u\n", p);
                }
            }
            std::fclose(out);
        }
    }
    // What the unfilled patches are: loops, Euler, and convex corners.
    std::map<std::string, u32> kinds;
    for (const LayoutPatch& patch : r.layout.patches) {
        if (patch.rectangle) {
            continue;
        }
        u32 ones = 0;
        u32 big = 0;
        for (u32 h : patch.loop) {
            ones += r.layout.sectorTurns[h] == 1 ? 1 : 0;
            big += r.layout.sectorTurns[h] >= 3 ? 1 : 0;
        }
        u32 singular = 0;
        for (u32 t : patch.triangles) {
            for (u32 i = 0; i < 3; ++i) {
                singular += r.field.index[r.mesh.corners[3 * t + i]] != 0 ? 1 : 0;
            }
        }
        ++kinds["loops " + std::to_string(patch.loops) + " euler " + std::to_string(patch.euler) + " corners " +
                std::to_string(ones) + " concave " + std::to_string(big) + (singular ? " singular" : "")];
    }
    WARN("triangles " << r.surface.triangleCount() << " piece " << piece << " of " << area.size() << " work "
                      << r.mesh.triangleCount() << " patches " << r.layout.patches.size() << " unfilled "
                      << r.layout.stats.unfilled << " singularities " << r.field.singularityCount() << " separatrices "
                      << r.layout.stats.separatrices << " repairs " << r.layout.stats.repairTraces << " centres "
                      << r.layout.stats.centreSplits << " forced " << r.layout.stats.forcedCorners << " rounds "
                      << r.layout.stats.rounds << " feature halfedges " << featureEdges);
    for (const auto& [kind, count] : kinds) {
        WARN(count << " x " << kind);
    }
    // Inside layout vertices whose turns add up to two or less: valence-two
    // points in the quads.
    std::map<std::string, u32> thin;
    std::vector<u32> ring;
    for (u32 v = 0; v < r.mesh.vertexCount(); ++v) {
        if (!r.mesh.alive(v) || !r.mesh.ring(v, ring)) {
            continue;
        }
        u32 edges = 0;
        u32 total = 0;
        std::string sectors;
        for (u32 o : ring) {
            if (r.mesh.isLayout(o)) {
                ++edges;
                total += r.layout.sectorTurns[o];
                sectors += std::to_string(r.layout.sectorTurns[o]);
            }
        }
        if (edges == 0 || total > 2 && !(edges == 2 && total == 4)) {
            if (edges > 0 && total != 4 - r.field.index[v] && r.layout.turnsOverride[v] == 0) {
                ++thin["mismatch: edges " + std::to_string(edges) + " sectors " + sectors + " index " +
                       std::to_string(r.field.index[v])];
            }
            continue;
        }
        if (total > 2) {
            continue;
        }
        WARN("thin vertex " << v);
        ++thin["edges " + std::to_string(edges) + " sectors " + sectors + " override " +
               std::to_string(r.layout.turnsOverride[v]) + " kind " +
               std::to_string(static_cast<u32>(r.mesh.kinds[v])) + " index " + std::to_string(r.field.index[v])];
    }
    for (const auto& [kind, count] : thin) {
        WARN("node " << count << " x " << kind);
    }
    std::map<i32, u32> indices;
    for (i32 i : r.field.index) {
        if (i != 0) {
            ++indices[i];
        }
    }
    std::string histogram;
    for (const auto& [i, count] : indices) {
        histogram += " " + std::to_string(i) + ":" + std::to_string(count);
    }
    WARN("indices" << histogram);
    {
        // How far each frame's normal leans off its facet, and how far the
        // authored normals at a triangle's corners spread.
        u32 lean[6] = {0, 0, 0, 0, 0, 0};
        u32 spread[6] = {0, 0, 0, 0, 0, 0};
        for (u32 t = 0; t < r.mesh.triangleCount(); ++t) {
            if (r.mesh.dead[t]) {
                continue;
            }
            const V3& p0 = r.mesh.positions[r.mesh.corners[3 * t]];
            const V3& p1 = r.mesh.positions[r.mesh.corners[3 * t + 1]];
            const V3& p2 = r.mesh.positions[r.mesh.corners[3 * t + 2]];
            const V3 facet = TriangleNormal(p0, p1, p2);
            const f64 degrees = std::acos(std::clamp(Dot(facet, r.field.normals[t]), -1.0, 1.0)) * 180.0 / kPi;
            ++lean[std::min<u32>(5, static_cast<u32>(degrees / 15.0))];
            f64 worst = 1.0;
            for (u32 i = 0; i < 3; ++i) {
                for (u32 j = i + 1; j < 3; ++j) {
                    worst = std::min(worst, Dot(r.surface.normal(r.mesh.homes[r.mesh.corners[3 * t + i]]),
                                                r.surface.normal(r.mesh.homes[r.mesh.corners[3 * t + j]])));
                }
            }
            const f64 apart = std::acos(std::clamp(worst, -1.0, 1.0)) * 180.0 / kPi;
            ++spread[std::min<u32>(5, static_cast<u32>(apart / 15.0))];
        }
        WARN("frame lean off facet by 15 deg bins: " << lean[0] << " " << lean[1] << " " << lean[2] << " " << lean[3]
                                                      << " " << lean[4] << " " << lean[5] << "; corner normal spread: "
                                                      << spread[0] << " " << spread[1] << " " << spread[2] << " "
                                                      << spread[3] << " " << spread[4] << " " << spread[5]);
    }

    // The no-collapse quantization: arcs it still leaves at zero are forced
    // there by the layout, named with their patches and dumped (`z a b`).
    std::vector<f64> targets;
    for (const LayoutArc& arc : r.layout.arcs) {
        targets.push_back(arc.length / edge);
    }
    QuantizeProblem problem = BuildQuantizeProblem(r.layout, targets, 0);
    for (QuantizeArc& arc : problem.arcs) {
        arc.zeroPenalty = 1000.0;
    }
    const QuantizeSolution solution = SolveQuantizeDoubleCover(problem);
    u32 forced = 0;
    std::FILE* zeros = nullptr;
    if (const char* directory = std::getenv("RETOPO_DUMP")) {
        zeros = std::fopen((std::string(directory) + "/" + name + ".txt").c_str(), "a");
    }
    for (u32 a = 0; solution.solved && a < solution.lengths.size(); ++a) {
        if (solution.lengths[a] != 0) {
            continue;
        }
        ++forced;
        const LayoutArc& arc = r.layout.arcs[a];
        if (zeros != nullptr) {
            for (std::size_t k = 1; k < arc.vertices.size(); ++k) {
                std::fprintf(zeros, "z %u %u\n", arc.vertices[k - 1], arc.vertices[k]);
            }
        }
        if (forced <= 12) {
            auto describe = [&](u32 patch, u8 side) {
                if (patch == kNone) {
                    return std::string("border");
                }
                const LayoutPatch& p = r.layout.patches[patch];
                std::string text = "patch " + std::to_string(patch) + " side " + std::to_string(side) + " [";
                for (u32 k = 0; k < 4; ++k) {
                    text += std::to_string(p.sides[k].size()) + (k < 3 ? "," : "]");
                }
                return text;
            };
            WARN("forced zero arc " << a << " (" << arc.vertices.size() << " vertices, length " << arc.length / edge
                                    << ") " << describe(arc.left, arc.leftSide) << " / "
                                    << describe(arc.right, arc.rightSide));
        }
    }
    if (zeros != nullptr) {
        std::fclose(zeros);
    }
    WARN("quantized: solved " << solution.solved << " arcs " << problem.arcs.size() << " forced zero " << forced);
    for (const std::string& note : r.layout.notes) {
        WARN(note);
    }
}

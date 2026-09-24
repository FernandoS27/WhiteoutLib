// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G21: the prepare (EDIT_MODE_MODELLING_DESIGN.md §2.2). The fixtures are read
/// from `.wem`, so they have no connectivity, exactly as a file opened in Edit
/// Mode does.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/repair.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/skinning/points.h>
#include <whiteout/models/wem/writer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

using geom::Domain;
using geom::EdgeId;
using geom::FaceId;
using geom::HalfedgeId;
using geom::Topology;
using geom::VertexId;

/// A mesh as a file splits it: positions, faces, and per-corner values.
struct Fixture {
    std::vector<Vector3f> positions;
    std::vector<std::vector<u32>> faces;
    std::vector<std::vector<Vector2f>> uvs;      ///< Per face, per corner; empty for none.
    std::vector<std::vector<Vector3f>> normals;  ///< Per face, per corner; empty for none.
    std::vector<std::vector<Vector4f>> tangents; ///< Per face, per corner; empty for none.
    std::vector<std::vector<geom::Influence>> skin; ///< Per vertex; empty for none.

    /// A face with one value for every corner of each layer it is given.
    void add(std::vector<u32> corners, std::vector<Vector2f> uv = {}, Vector3f normal = {0, 0, 1},
             Vector4f tangent = {1, 0, 0, 1}) {
        const std::size_t n = corners.size();
        faces.push_back(std::move(corners));
        if (uv.empty()) {
            uv.assign(n, Vector2f{0.0f, 0.0f});
        }
        uvs.push_back(std::move(uv));
        normals.push_back(std::vector<Vector3f>(n, normal));
        tangents.push_back(std::vector<Vector4f>(n, tangent));
    }
};

Mesh throughWem(const Mesh& mesh) {
    Document document;
    document.declare(ProfileId::Generic);
    document.defaultProfile = ProfileId::Generic;
    Model model;
    model.name = "prepare";
    model.meshes.push_back(mesh);
    document.models.push_back(std::move(model));
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    std::optional<Document> read = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(read.has_value());
    REQUIRE(read->models.size() == 1u);
    REQUIRE(read->models[0].meshes.size() == 1u);
    return std::move(read->models[0].meshes[0]);
}

/// The fixture as a mesh read from `.wem`: no connectivity.
Mesh meshOf(const Fixture& fixture, f32 scale = 1.0f) {
    geom::FaceSet set;
    set.vertexCount = static_cast<u32>(fixture.positions.size());
    for (const std::vector<u32>& face : fixture.faces) {
        set.addFace(face);
    }
    Mesh mesh;
    mesh.setFaceSet(set);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    for (std::size_t v = 0; v < fixture.positions.size(); ++v) {
        const Vector3f& p = fixture.positions[v];
        positions[v] = Vector3f{p.x * scale, p.y * scale, p.z * scale};
    }
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    REQUIRE(mesh.ensureConnectivity().ok());
    const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
        geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const std::span<Vector3f> normals = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kNormal, Domain::Halfedge, geom::AttrType::F32x3);
    const std::span<Vector4f> tangents = mesh.attributes.getOrCreate<Vector4f>(
        geom::names::kTangent, Domain::Halfedge, geom::AttrType::F32x4);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 f = 0; f < fixture.faces.size(); ++f) {
        u32 i = 0;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            uvs[h.index()] = fixture.uvs[f][i];
            normals[h.index()] = fixture.normals[f][i];
            tangents[h.index()] = fixture.tangents[f][i];
            ++i;
        }
    }
    if (!fixture.skin.empty()) {
        mesh.skin.reset(0);
        for (const std::vector<geom::Influence>& influences : fixture.skin) {
            mesh.skin.appendVertex(influences);
        }
    }
    mesh.recomputeBounds();
    return throughWem(mesh);
}

/// Two quads side by side in the xy plane, each as two triangles, the middle
/// column held by both with vertices of their own: the file's seam split.
/// Left 0..5 (0 1 2 bottom-to-top at x=0, 3 4 5 at x=1), right 6..11 (6 7 8 at
/// x=1, 9 10 11 at x=2), so 3-6, 4-7 and 5-8 are the twins.
Fixture twoColumns(f32 foldY = 0.0f) {
    Fixture f;
    for (u32 x = 0; x < 2; ++x) {
        for (u32 y = 0; y < 3; ++y) {
            f.positions.push_back(Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
        }
    }
    for (u32 x = 1; x < 3; ++x) {
        for (u32 y = 0; y < 3; ++y) {
            // A fold lifts the right column's far side, for the hard edge.
            const f32 z = x == 2 ? foldY : 0.0f;
            f.positions.push_back(Vector3f{static_cast<f32>(x) - (x == 2 && foldY != 0.0f ? 1.0f : 0.0f),
                                           static_cast<f32>(y), z});
        }
    }
    return f;
}

/// The left strip's two cells and the right strip's, as triangles, with the
/// given per-side corner values.
void addStrips(Fixture& f, Vector2f uvShiftRight, Vector3f normalLeft, Vector3f normalRight,
               f32 wLeft, f32 wRight) {
    const auto uvLeft = [](u32 v) {
        return Vector2f{static_cast<f32>(v / 3) * 0.5f, static_cast<f32>(v % 3) * 0.5f};
    };
    const auto uvRight = [&](u32 v) {
        const u32 local = v - 6;
        return Vector2f{0.5f + static_cast<f32>(local / 3) * 0.5f + uvShiftRight.x,
                        static_cast<f32>(local % 3) * 0.5f + uvShiftRight.y};
    };
    for (u32 y = 0; y < 2; ++y) {
        const u32 a = y;
        const u32 b = 3 + y;
        const u32 c = 3 + y + 1;
        const u32 d = y + 1;
        f.add({a, b, c}, {uvLeft(a), uvLeft(b), uvLeft(c)}, normalLeft, Vector4f{1, 0, 0, wLeft});
        f.add({a, c, d}, {uvLeft(a), uvLeft(c), uvLeft(d)}, normalLeft, Vector4f{1, 0, 0, wLeft});
    }
    for (u32 y = 0; y < 2; ++y) {
        const u32 a = 6 + y;
        const u32 b = 9 + y;
        const u32 c = 9 + y + 1;
        const u32 d = 6 + y + 1;
        f.add({a, b, c}, {uvRight(a), uvRight(b), uvRight(c)}, normalRight, Vector4f{0, 1, 0, wRight});
        f.add({a, c, d}, {uvRight(a), uvRight(c), uvRight(d)}, normalRight, Vector4f{0, 1, 0, wRight});
    }
}

/// Edges flagged on @p layer, as endpoint position pairs.
std::set<std::pair<std::array<f32, 3>, std::array<f32, 3>>> flagged(const Mesh& mesh, const char* layer) {
    std::set<std::pair<std::array<f32, 3>, std::array<f32, 3>>> out;
    const std::span<const u8> flags = mesh.attributes.get<const u8>(layer, Domain::Edge);
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> p =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 e = 0; e < flags.size(); ++e) {
        if (flags[e] == 0 || topology.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        std::array<f32, 3> a{p[topology.from(h).index()].x, p[topology.from(h).index()].y,
                             p[topology.from(h).index()].z};
        std::array<f32, 3> b{p[topology.to(h).index()].x, p[topology.to(h).index()].y,
                             p[topology.to(h).index()].z};
        if (b < a) {
            std::swap(a, b);
        }
        out.insert({a, b});
    }
    return out;
}

/// The structure checks, and the promise the prepare makes beyond them: a
/// repair of what it left splits nothing and drops no face, so no later build
/// quietly loses a face the weld flattened.
std::string errors(const Mesh& mesh) {
    Diagnostics diagnostics;
    geom::CheckStructural(mesh, 0, diagnostics);
    geom::CheckManifold(mesh, 0, diagnostics);
    if (diagnostics.hasErrors()) {
        return diagnostics.formatHistogram();
    }
    const std::span<const u32> sections = mesh.faceSections();
    const geom::RepairResult repaired = geom::Repair(
        mesh.faceSet(), sections,
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex));
    if (!repaired.log.droppedFaces.empty()) {
        return "a repair drops " + std::to_string(repaired.log.droppedFaces.size()) + " face(s)";
    }
    if (!repaired.log.splits.empty()) {
        return "a repair splits " + std::to_string(repaired.log.splits.size()) + " vertex(es)";
    }
    return std::string();
}

u32 interiorEdges(const Mesh& mesh) {
    const Topology& topology = mesh.topology();
    u32 count = 0;
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        count += !topology.isDeleted(EdgeId(e)) && !topology.isBoundary(EdgeId(e)) ? 1u : 0u;
    }
    return count;
}

/// Everything a second prepare could change: the face set, positions, every
/// layer's bytes, the skin.
struct State {
    std::vector<u32> corners;
    std::vector<u32> valences;
    std::map<std::string, std::vector<u8>> layers;
    std::vector<u32> offsets;

    bool operator==(const State&) const = default;
};

State stateOf(const Mesh& mesh) {
    State out;
    out.corners = mesh.faceSet().cornerVertex;
    out.valences = mesh.faceSet().faceValence;
    for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.name == geom::names::kModelled) {
            continue;
        }
        out.layers[layer.name + "@" + geom::ToString(layer.domain)] = layer.data;
    }
    out.offsets = mesh.skin.offsets;
    return out;
}

/// The prepare's promise to the point table (D13): each point of the prepared
/// mesh is exactly one cluster of the original.
void checkPointsAgree(const Mesh& original, const Mesh& prepared, const geom::PrepareReport& report) {
    const skinning::PointTable before = skinning::BuildPointTable(original);
    const skinning::PointTable after = skinning::BuildPointTable(prepared);
    for (u32 p = 0; p < before.pointCount; ++p) {
        std::vector<u32> mapped;
        for (const u32 v : before.membersOf(p)) {
            mapped.push_back(report.vertexOf[v]);
        }
        std::sort(mapped.begin(), mapped.end());
        mapped.erase(std::unique(mapped.begin(), mapped.end()), mapped.end());
        REQUIRE(!mapped.empty());
        const u32 point = after.pointOf[mapped[0]];
        std::vector<u32> members(after.membersOf(point).begin(), after.membersOf(point).end());
        std::sort(members.begin(), members.end());
        CHECK(members == mapped);
    }
}

} // namespace

TEST_CASE("wem prepare a seam closes and is marked seam", "[wem][geometry][prepare]") {
    Fixture f = twoColumns();
    addStrips(f, Vector2f{0.1f, 0.0f}, {0, 0, 1}, {0, 0, 1}, 1.0f, 1.0f);
    Mesh mesh = meshOf(f);
    REQUIRE_FALSE(mesh.hasConnectivity());
    const Mesh original = mesh;
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.verticesWelded == 3u);
    CHECK(report.seamsMarked == 2u);
    CHECK(report.sharpMarked == 0u);
    CHECK(mesh.vertexCount() == 9u);
    CHECK(geom::IsModelled(mesh));
    CHECK(flagged(mesh, geom::names::kSeam).size() == 2u);
    CHECK(flagged(mesh, geom::names::kSharp).empty());
    CHECK(errors(mesh) == "");
    CHECK(geom::IsCanonical(mesh));
    checkPointsAgree(original, mesh, report);
}

TEST_CASE("wem prepare a pin neither splits a weld nor marks a seam",
          "[wem][geometry][prepare][uv]") {
    // EDIT_MODE_UV_DESIGN.md §3: a pin says "hold this corner where it is". It
    // is a thing about one corner, never a difference in what the surface holds
    // there, so the weld must not see it at all.
    Fixture f = twoColumns();
    addStrips(f, Vector2f{0.0f, 0.0f}, {0, 0, 1}, {0, 0, 1}, 1.0f, 1.0f);
    Mesh plain = meshOf(f);
    const geom::PrepareReport without = geom::PrepareForModelling(plain);

    Mesh pinned = meshOf(f);
    REQUIRE(pinned.ensureConnectivity().ok());
    {
        // One side of every twin pinned and the other not: were the layer
        // compared, not one of the three pairs could weld.
        const std::span<u8> pins = pinned.attributes.getOrCreate<u8>(
            geom::names::uvPin(0), Domain::Halfedge, geom::AttrType::Bool);
        const Topology& topology = std::as_const(pinned).topology();
        for (u32 face = 0; face < 4 && face < topology.faceCount(); ++face) {
            for (const HalfedgeId h : topology.fh(FaceId(face))) {
                pins[h.index()] = 1;
            }
        }
    }
    pinned.invalidateConnectivity();
    const geom::PrepareReport with = geom::PrepareForModelling(pinned);

    CHECK(with.verticesWelded == without.verticesWelded);
    CHECK(with.seamsMarked == without.seamsMarked);
    CHECK(with.sharpMarked == without.sharpMarked);
    CHECK(pinned.vertexCount() == plain.vertexCount());
    CHECK(flagged(pinned, geom::names::kSeam).size() == flagged(plain, geom::names::kSeam).size());
}

TEST_CASE("wem prepare a hard edge is sharp and not seam", "[wem][geometry][prepare]") {
    // Continuous UVs across the fold; the normals and the tangents' xyz differ.
    Fixture f = twoColumns(1.0f);
    addStrips(f, Vector2f{0.0f, 0.0f}, {0, 0, 1}, {1, 0, 0}, 1.0f, 1.0f);
    // The right side's UVs continue the left's exactly at the twins.
    Mesh mesh = meshOf(f);
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.verticesWelded == 3u);
    CHECK(report.seamsMarked == 0u);
    CHECK(report.sharpMarked == 2u);
    CHECK(flagged(mesh, geom::names::kSeam).empty());
    CHECK(flagged(mesh, geom::names::kSharp).size() == 2u);
    CHECK(errors(mesh) == "");
}

TEST_CASE("wem prepare a mirrored-UV line is a seam", "[wem][geometry][prepare]") {
    // The same UVs and normals either side; only the tangent's w flips.
    Fixture f = twoColumns();
    addStrips(f, Vector2f{0.0f, 0.0f}, {0, 0, 1}, {0, 0, 1}, 1.0f, -1.0f);
    Mesh mesh = meshOf(f);
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.verticesWelded == 3u);
    CHECK(report.seamsMarked == 2u);
    CHECK(report.sharpMarked == 0u);
}

TEST_CASE("wem prepare a binding mismatch stays apart", "[wem][geometry][prepare]") {
    Fixture f = twoColumns();
    addStrips(f, Vector2f{0.1f, 0.0f}, {0, 0, 1}, {0, 0, 1}, 1.0f, 1.0f);
    f.skin.assign(12, {geom::Influence{0, 1.0f}});
    for (u32 v = 6; v < 12; ++v) {
        f.skin[v] = {geom::Influence{1, 1.0f}};
    }
    Mesh mesh = meshOf(f);
    const Mesh original = mesh;
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.verticesWelded == 0u);
    CHECK(report.keptBinding == 3u);
    // Each strip's two diagonals and the edge between its cells: nothing closed.
    CHECK(interiorEdges(mesh) == 6u);
    CHECK(errors(mesh) == "");
    checkPointsAgree(original, mesh, report);
}

TEST_CASE("wem prepare two shells touching at a point stay apart", "[wem][geometry][prepare]") {
    Fixture f;
    f.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}, {2, 0, 0}, {2, 1, 0.5f}};
    f.add({0, 1, 2});
    f.add({3, 4, 5});
    Mesh mesh = meshOf(f);
    const Mesh original = mesh;
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.verticesWelded == 0u);
    CHECK(report.keptNonManifold == 1u);
    CHECK(mesh.faceSet().faceCount() == 2u);
    CHECK(errors(mesh) == "");
    checkPointsAgree(original, mesh, report);
}

TEST_CASE("wem prepare a double-sided plane stays two-sided", "[wem][geometry][prepare]") {
    Fixture f;
    f.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    f.add({0, 1, 2});
    f.add({0, 2, 3});
    f.add({6, 5, 4}, {}, {0, 0, -1});
    f.add({7, 6, 4}, {}, {0, 0, -1});
    Mesh mesh = meshOf(f);
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.verticesWelded == 0u);
    CHECK(mesh.faceSet().faceCount() == 4u);
    CHECK(mesh.vertexCount() == 8u);
    CHECK(errors(mesh) == "");
}

TEST_CASE("wem prepare a seam a third shell touches stays closed", "[wem][geometry][prepare]") {
    // The seam's middle twins (4, 7) share their position with a third shell's
    // corner, bound the same: rule 3 refines rather than excluding the group.
    Fixture f = twoColumns();
    addStrips(f, Vector2f{0.1f, 0.0f}, {0, 0, 1}, {0, 0, 1}, 1.0f, 1.0f);
    f.positions.push_back(Vector3f{1, 1, 0});   // 12, on the seam's middle
    f.positions.push_back(Vector3f{1, 1, 1});   // 13
    f.positions.push_back(Vector3f{1.5f, 1, 1}); // 14
    f.add({12, 14, 13}, {}, {0, -1, 0});
    Mesh mesh = meshOf(f);
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.verticesWelded == 3u);
    CHECK(report.keptNonManifold == 1u);
    CHECK(report.seamsMarked == 2u);
    CHECK(mesh.vertexCount() == 12u);
    CHECK(errors(mesh) == "");
}

TEST_CASE("wem prepare a sliver on a T-junction keeps its face", "[wem][geometry][prepare]") {
    // Shell one: (P, T, R) and (T, Q, R), T on PQ. Shell two: a sliver under
    // them whose apex A sits within the tolerance of T; welded, it would lie
    // on PQ and the repair would drop it.
    Fixture f;
    f.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {1, 1, 0},      // P T Q R
                   {0, 0, 0}, {1, -1e-5f, 0}, {2, 0, 0}};              // P' A Q'
    f.add({0, 1, 3});
    f.add({1, 2, 3});
    f.add({6, 5, 4});
    Mesh mesh = meshOf(f);
    const Mesh original = mesh;
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(mesh.faceSet().faceCount() == 3u);
    for (const u32 face : report.faceOf) {
        CHECK(face != geom::kInvalidId);
    }
    CHECK(mesh.repairLog.droppedFaces.empty());
    CHECK(errors(mesh) == "");
    // The apex kept its own place, off the base line.
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    CHECK(p[5].y == -1e-5f);
    checkPointsAgree(original, mesh, report);
}

TEST_CASE("wem prepare a chain within the tolerance link by link is one point",
          "[wem][geometry][prepare]") {
    // Three triangles round one corner, each with its own copy of it: a at 0,
    // c at 0.8 tolerance, b at 1.6 tolerance, so only a-c and c-b are within
    // it. Rim twins join the triangles into one fan once welded.
    const std::vector<Vector3f> rim{{2, 0, 0}, {1.4f, 1.4f, 0}, {0, 2, 0}, {-1.4f, 1.4f, 0}};
    std::vector<Vector3f> hull = rim;
    hull.push_back(Vector3f{0, 0, 0});
    const f32 tolerance = geom::CoincidenceTolerance(hull);
    Fixture f;
    f.positions = {{0, 0, 0}, rim[0], rim[1],                 // a, r0, r1
                   {0.8f * tolerance, 0, 0}, rim[1], rim[2],  // c, r1', r2
                   {1.6f * tolerance, 0, 0}, rim[2], rim[3]}; // b, r2', r3
    f.add({0, 1, 2});
    f.add({3, 4, 5});
    f.add({6, 7, 8});
    Mesh mesh = meshOf(f);
    const Mesh original = mesh;
    const skinning::PointTable chain = skinning::BuildPointTable(original);
    REQUIRE(chain.pointOf[0] == chain.pointOf[6]);
    REQUIRE(chain.pointOf[0] == chain.pointOf[3]);
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.verticesWelded == 4u);
    CHECK(mesh.vertexCount() == 5u);
    CHECK(errors(mesh) == "");
    checkPointsAgree(original, mesh, report);
}

TEST_CASE("wem prepare is idempotent and the marker makes it identity", "[wem][geometry][prepare]") {
    Fixture f = twoColumns();
    addStrips(f, Vector2f{0.1f, 0.0f}, {0, 0, 1}, {0, 0, 1}, 1.0f, -1.0f);
    Mesh mesh = meshOf(f);
    geom::PrepareForModelling(mesh);
    const State once = stateOf(mesh);

    SECTION("the marked mesh returns identity") {
        const geom::PrepareReport again = geom::PrepareForModelling(mesh);
        CHECK(again.verticesWelded == 0u);
        CHECK(stateOf(mesh) == once);
    }
    SECTION("a second run with the marker cleared changes nothing") {
        mesh.attributes.get<u8>(geom::names::kModelled, Domain::Mesh)[0] = 0;
        const geom::PrepareReport again = geom::PrepareForModelling(mesh);
        CHECK(again.verticesWelded == 0u);
        CHECK(stateOf(mesh) == once);
    }
    SECTION("nor after a hand edit of sharp") {
        const std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(geom::names::kSharp, Domain::Edge,
                                                                    geom::AttrType::Bool);
        sharp[0] = 1;
        const State edited = stateOf(mesh);
        mesh.attributes.get<u8>(geom::names::kModelled, Domain::Mesh)[0] = 0;
        geom::PrepareForModelling(mesh);
        CHECK(stateOf(mesh) == edited);
    }
}

TEST_CASE("wem prepare welds the same at 1024 and at 1 over 1024", "[wem][geometry][prepare]") {
    const auto fixtures = [] {
        std::vector<Fixture> out;
        Fixture seam = twoColumns();
        addStrips(seam, Vector2f{0.1f, 0.0f}, {0, 0, 1}, {0, 0, 1}, 1.0f, 1.0f);
        out.push_back(seam);
        Fixture hard = twoColumns(1.0f);
        addStrips(hard, Vector2f{0.0f, 0.0f}, {0, 0, 1}, {1, 0, 0}, 1.0f, 1.0f);
        out.push_back(hard);
        Fixture sliver;
        sliver.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, -1e-5f, 0}, {2, 0, 0}};
        sliver.add({0, 1, 3});
        sliver.add({1, 2, 3});
        sliver.add({6, 5, 4});
        out.push_back(sliver);
        return out;
    }();
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        CAPTURE(i);
        Mesh unit = meshOf(fixtures[i]);
        const geom::PrepareReport base = geom::PrepareForModelling(unit);
        for (const f32 scale : {1024.0f, 1.0f / 1024.0f}) {
            CAPTURE(scale);
            Mesh scaled = meshOf(fixtures[i], scale);
            const geom::PrepareReport report = geom::PrepareForModelling(scaled);
            CHECK(report.verticesWelded == base.verticesWelded);
            CHECK(report.keptNonManifold == base.keptNonManifold);
            CHECK(report.keptBinding == base.keptBinding);
            CHECK(report.seamsMarked == base.seamsMarked);
            CHECK(report.sharpMarked == base.sharpMarked);
            CHECK(report.vertexOf == base.vertexOf);
            CHECK(scaled.faceSet().cornerVertex == unit.faceSet().cornerVertex);
        }
    }
}

TEST_CASE("wem prepare snaps a kept-apart member of a chain too", "[wem][geometry][prepare]") {
    // The chain again, but b bound to another bone: a and c merge, b stays
    // apart, and it must still land on the merged vertex's position or it
    // falls out of the point (1.6 tolerances from a).
    const std::vector<Vector3f> rim{{2, 0, 0}, {1.4f, 1.4f, 0}, {0, 2, 0}, {-1.4f, 1.4f, 0}};
    std::vector<Vector3f> hull = rim;
    hull.push_back(Vector3f{0, 0, 0});
    const f32 tolerance = geom::CoincidenceTolerance(hull);
    Fixture f;
    f.positions = {{0, 0, 0}, rim[0], rim[1],
                   {0.8f * tolerance, 0, 0}, rim[1], rim[2],
                   {1.6f * tolerance, 0, 0}, rim[2], rim[3]};
    f.add({0, 1, 2});
    f.add({3, 4, 5});
    f.add({6, 7, 8});
    f.skin.assign(9, {geom::Influence{0, 1.0f}});
    f.skin[6] = {geom::Influence{1, 1.0f}};
    Mesh mesh = meshOf(f);
    const Mesh original = mesh;
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.keptBinding == 1u);
    CHECK(errors(mesh) == "");
    checkPointsAgree(original, mesh, report);
}

TEST_CASE("wem prepare leaves an edge that was already interior alone", "[wem][geometry][prepare]") {
    // The seam fixture, with the left strip's first diagonal already shared and
    // its two sides' normals apart: no weld closed it, so it is not marked.
    Fixture f = twoColumns();
    addStrips(f, Vector2f{0.1f, 0.0f}, {0, 0, 1}, {0, 0, 1}, 1.0f, 1.0f);
    f.normals[1] = std::vector<Vector3f>(3, Vector3f{0, 1, 0});
    Mesh mesh = meshOf(f);
    const geom::PrepareReport report = geom::PrepareForModelling(mesh);
    CHECK(report.sharpMarked == 0u);
    CHECK(flagged(mesh, geom::names::kSharp).empty());
}

// ============================================================================
// The corpus arm
// ============================================================================

TEST_CASE("wem prepare over corpus meshes keeps every triangle and corner",
          "[wem][geometry][prepare][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 40);
    const MdxConverter converter;
    u64 vertices = 0;
    u64 welded = 0;
    u64 keptNonManifold = 0;
    u64 keptBinding = 0;
    u64 keptCorners = 0;
    u64 seams = 0;
    u64 sharps = 0;
    u32 meshes = 0;
    std::vector<std::string> failing;
    std::vector<std::string> slow;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Parser parser;
        const mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        const Result<Document> document = converter.fromMdx(source);
        if (!document.ok() || document->models.empty()) {
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        for (const Mesh& imported : document->models[0].meshes) {
            const Mesh original = throughWem(imported);
            Mesh mesh = original;
            const geom::PrepareReport report = geom::PrepareForModelling(mesh);
            ++meshes;
            vertices += original.vertexCount();
            welded += report.verticesWelded;
            keptNonManifold += report.keptNonManifold;
            keptBinding += report.keptBinding;
            keptCorners += report.keptFaceCorners;
            seams += report.seamsMarked;
            sharps += report.sharpMarked;
            std::string broken;
            const geom::FaceSet& before = original.faceSet();
            const geom::FaceSet& after = mesh.faceSet();
            if (!mesh.repairLog.splits.empty() || !mesh.repairLog.droppedFaces.empty() ||
                after.faceCount() != before.faceCount()) {
                broken = "the final repair split or dropped (" + std::to_string(mesh.repairLog.splits.size()) +
                         " splits, " + std::to_string(mesh.repairLog.droppedFaces.size()) + " dropped, faces " +
                         std::to_string(before.faceCount()) + " -> " + std::to_string(after.faceCount()) +
                         ", welded " + std::to_string(report.verticesWelded) + ", import log " +
                         std::to_string(original.repairLog.splits.size()) + ")";
            }
            // Each face's corners, in order: the same positions within the
            // tolerance and the same corner values.
            if (broken.empty()) {
                // A corner snaps to its point's lowest vertex, which a chain of
                // members each within the tolerance of the next can put up to
                // (members - 1) tolerances away.
                const skinning::PointTable table = skinning::BuildPointTable(original);
                Mesh left = original;
                REQUIRE(left.ensureConnectivity().ok());
                const Topology& was = std::as_const(left).topology();
                const Topology& now = std::as_const(mesh).topology();
                const auto pw = left.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
                const auto pn = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
                for (u32 fo = 0; fo < before.faceCount() && broken.empty(); ++fo) {
                    const u32 fn = report.faceOf[fo];
                    if (fn == geom::kInvalidId) {
                        broken = "a face went";
                        break;
                    }
                    std::vector<HalfedgeId> a;
                    std::vector<HalfedgeId> b;
                    for (const HalfedgeId h : was.fh(FaceId(fo))) {
                        a.push_back(h);
                    }
                    for (const HalfedgeId h : now.fh(FaceId(fn))) {
                        b.push_back(h);
                    }
                    if (a.size() != b.size()) {
                        broken = "a face changed valence";
                        break;
                    }
                    for (std::size_t k = 0; k < a.size() && broken.empty(); ++k) {
                        const Vector3f& x = pw[was.from(a[k]).index()];
                        const Vector3f& y = pn[now.from(b[k]).index()];
                        const f32 d = std::sqrt((x.x - y.x) * (x.x - y.x) + (x.y - y.y) * (x.y - y.y) +
                                                (x.z - y.z) * (x.z - y.z));
                        const u32 v = was.from(a[k]).value();
                        const f32 reach = static_cast<f32>(
                            std::max<std::size_t>(1, table.membersOf(table.pointOf[v]).size() - 1));
                        if (d > report.tolerance * reach * 1.0001f + 1e-12f) {
                            broken = "a corner moved " + std::to_string(d / report.tolerance) + " tolerances";
                        }
                        for (const geom::AttrLayer& layer : left.attributes.layers()) {
                            if (layer.domain != Domain::Halfedge) {
                                continue;
                            }
                            const geom::AttrLayer* other = mesh.attributes.layer(layer.name, Domain::Halfedge);
                            const std::size_t stride = geom::AttrTypeSize(layer.type);
                            if (other == nullptr ||
                                std::memcmp(layer.data.data() + stride * a[k].index(),
                                            other->data.data() + stride * b[k].index(), stride) != 0) {
                                broken = "a corner value changed: " + layer.name;
                            }
                        }
                    }
                }
            }
            if (broken.empty()) {
                broken = errors(mesh);
            }
            if (!broken.empty() && failing.size() < 10) {
                failing.push_back(test::pathText(files[i].filename()) + " '" + imported.name + "': " + broken);
            }
        }
        const f64 seconds =
            std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
        if (seconds > 1.0) {
            slow.push_back(test::pathText(files[i].filename()) + " " + std::to_string(seconds) + " s");
        }
    }
    for (const std::string& line : failing) {
        std::cout << "  " << line << "\n";
    }
    for (const std::string& line : slow) {
        std::cout << "  slow: " << line << "\n";
    }
    std::cout << "prepare corpus arm: " << meshes << " meshes, " << vertices << " vertices, "
              << welded << " welded, kept apart " << keptBinding << " binding / " << keptCorners
              << " face corners / " << keptNonManifold << " manifold; " << seams << " seams, "
              << sharps << " sharp\n";
    CHECK(meshes > 0u);
    CHECK(failing.empty());
}

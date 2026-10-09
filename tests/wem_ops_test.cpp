// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P2 — the §5.7 edit-operation surface.
///
/// Every case ends by re-running the C1–C9 checks. That is deliberate and it is
/// most of the value here: a half-edge surgery that produces the right counts and
/// a corrupt `next` chain looks correct to every assertion except that one.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/fbx/fbx.h>
#include <whiteout/models/fbx/scene.h>
#include <whiteout/models/gltf/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/fbx_converter.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/triangulation.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// C1–C9 over one mesh. Returns the histogram, or an empty string when clean, so
/// a failure names *which* clause broke instead of only saying "false".
std::string intact(const Mesh& mesh) {
    Diagnostics diagnostics;
    geom::CheckStructural(mesh, 0, diagnostics);
    geom::CheckManifold(mesh, 0, diagnostics);
    return diagnostics.hasErrors() ? diagnostics.formatHistogram() : std::string();
}

/// Positions spread so nothing is collinear — a zero-area face is dropped by the
/// repair, which would silently empty a fixture.
const Vector3f kSpread[8] = {
    {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.25f},
    {0.5f, 2.0f, 1.0f}, {2.0f, 0.5f, 1.5f}, {2.5f, 2.5f, 0.75f}, {1.5f, 3.0f, 2.0f},
};

/// A mesh from face-vertex lists, with a per-vertex UV written to every corner so
/// the attribute plumbing has something to move around.
Mesh buildMesh(std::size_t vertexCount, const std::vector<std::vector<u32>>& faces) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "s0";
    builder.addSection(std::move(section));
    for (std::size_t i = 0; i < vertexCount; ++i) {
        builder.addVertex(kSpread[i % 8]);
    }
    for (const std::vector<u32>& corners : faces) {
        std::vector<geom::VertexId> ids;
        for (u32 v : corners) {
            ids.push_back(geom::VertexId(v));
        }
        const geom::FaceId face = builder.addFace(ids, 0);
        for (u32 i = 0; i < corners.size(); ++i) {
            builder.setCornerAttr(face, i, geom::names::uv(0),
                                  Vector2f{static_cast<f32>(corners[i]), 0.5f});
        }
    }
    auto outcome = builder.build();
    return std::move(outcome.mesh);
}

/// `(w+1) x (h+1)` vertices, two triangles per cell, diagonals running
/// `(x,y) -> (x+1,y+1)`.
Mesh grid(u32 w, u32 h) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "grid";
    builder.addSection(std::move(section));
    for (u32 y = 0; y <= h; ++y) {
        for (u32 x = 0; x <= w; ++x) {
            builder.addVertex(Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
        }
    }
    const auto at = [&](u32 x, u32 y) { return geom::VertexId(y * (w + 1) + x); };
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            builder.addTriangle(at(x, y), at(x + 1, y), at(x + 1, y + 1), 0);
            builder.addTriangle(at(x, y), at(x + 1, y + 1), at(x, y + 1), 0);
        }
    }
    auto outcome = builder.build();
    return std::move(outcome.mesh);
}

u32 liveFaces(const Mesh& mesh) {
    const geom::Topology& topology = mesh.topology();
    u32 count = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (!topology.isDeleted(geom::FaceId(f))) {
            ++count;
        }
    }
    return count;
}

u32 liveVertices(const Mesh& mesh) {
    const geom::Topology& topology = mesh.topology();
    u32 count = 0;
    for (u32 v = 0; v < topology.vertexCount(); ++v) {
        if (!topology.isDeleted(geom::VertexId(v))) {
            ++count;
        }
    }
    return count;
}

} // namespace

// ============================================================================
// Maintaining ops
// ============================================================================

TEST_CASE("WEM SplitEdge inserts a corner into both loops", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3}, {0, 3, 2}});
    REQUIRE(mesh.hasConnectivity());
    const geom::HalfedgeId shared =
        mesh.topology().findHalfedge(geom::VertexId(0), geom::VertexId(3));
    REQUIRE(shared.valid());

    const geom::VertexId inserted =
        geom::SplitEdge(mesh, geom::Topology::edge(shared), 0.25f);
    REQUIRE(inserted.valid());
    CHECK(mesh.vertexCount() == 5);
    CHECK(liveFaces(mesh) == 2);
    CHECK(mesh.topology().valence(geom::FaceId(0)) == 4);
    CHECK(mesh.topology().valence(geom::FaceId(1)) == 4);
    CHECK(mesh.topology().valence(inserted) == 2);

    const auto positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const Vector3f& a = positions[0];
    const Vector3f& b = positions[3];
    CHECK(positions[inserted.index()].x == a.x + (b.x - a.x) * 0.25f);
    CHECK(positions[inserted.index()].y == a.y + (b.y - a.y) * 0.25f);

    // The corner UV at the new vertex is the lerp of the corners the edge spans —
    // both endpoints carry `vertexIndex` as u, so this is 0 + (3 - 0) * 0.25.
    const auto uvs = mesh.attributes.get<const Vector2f>(geom::names::uv(0),
                                                         geom::Domain::Halfedge);
    bool sawInserted = false;
    for (geom::HalfedgeId h : mesh.topology().voh(inserted)) {
        if (mesh.topology().isBoundary(h)) {
            continue;
        }
        CHECK(uvs[h.index()].x == 0.75f);
        sawInserted = true;
    }
    CHECK(sawInserted);

    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM SplitFace cuts a quad into two triangles", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3, 2}});
    REQUIRE(mesh.topology().valence(geom::FaceId(0)) == 4);
    mesh.sections[0].materialSlot = 5;

    const geom::FaceId added = geom::SplitFace(mesh, geom::FaceId(0), 0, 2);
    REQUIRE(added.valid());
    CHECK(liveFaces(mesh) == 2);
    CHECK(mesh.topology().valence(geom::FaceId(0)) == 3);
    CHECK(mesh.topology().valence(added) == 3);
    // Face attributes come across, so the new half draws in the same section.
    CHECK(mesh.faceSections()[added.index()] == mesh.faceSections()[0]);

    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM SplitFace refuses a degenerate diagonal", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3, 2}});
    CHECK_FALSE(geom::SplitFace(mesh, geom::FaceId(0), 1, 1).valid());
    CHECK_FALSE(geom::SplitFace(mesh, geom::FaceId(0), 0, 1).valid()); // adjacent
    CHECK_FALSE(geom::SplitFace(mesh, geom::FaceId(0), 0, 9).valid()); // out of range
    CHECK(liveFaces(mesh) == 1);
}

namespace {

/// Triangles as sorted vertex triples, the list sorted: two cuts compared as sets.
std::vector<std::vector<u32>> asSet(std::vector<std::vector<u32>> triangles) {
    for (std::vector<u32>& triangle : triangles) {
        std::sort(triangle.begin(), triangle.end());
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

/// Every live face of @p mesh, which must all be triangles.
std::vector<std::vector<u32>> liveTriangles(const Mesh& mesh) {
    std::vector<std::vector<u32>> out;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        if (mesh.topology().isDeleted(geom::FaceId(f))) {
            continue;
        }
        std::vector<u32>& triangle = out.emplace_back();
        for (geom::VertexId v : mesh.topology().fv(geom::FaceId(f))) {
            triangle.push_back(v.value());
        }
    }
    return asSet(std::move(out));
}

/// @p row (vertex ids, three per triangle) as triples.
std::vector<std::vector<u32>> triples(std::span<const u32> row) {
    std::vector<std::vector<u32>> out;
    for (std::size_t t = 0; t + 2 < row.size(); t += 3) {
        out.push_back({row[t], row[t + 1], row[t + 2]});
    }
    return asSet(std::move(out));
}

} // namespace

TEST_CASE("WEM Triangulate cuts the face as it is drawn", "[wem][geometry][ops]") {
    // C31: Triangulate once fanned from the first corner; it now cuts along the
    // diagonals TriangulateFace draws, so the pieces are the triangles shown.
    Mesh mesh = buildMesh(5, {{0, 1, 3, 4, 2}});
    REQUIRE(mesh.topology().valence(geom::FaceId(0)) == 5);
    const std::vector<u32> loop{0, 1, 3, 4, 2};
    const auto positions =
        std::as_const(mesh).attributes.get<const Vector3f>(geom::names::kPosition,
                                                           geom::Domain::Vertex);
    const std::vector<Vector3f> held(positions.begin(), positions.end());
    std::vector<u32> automatic;
    geom::TriangulateFace(loop, held, {}, automatic);
    for (u32& corner : automatic) {
        corner = loop[corner];
    }

    SECTION("by the automatic rule") {
        CHECK(geom::Triangulate(mesh, geom::FaceId(0)) == 2);
        CHECK(liveFaces(mesh) == 3);
        CHECK(liveTriangles(mesh) == triples(automatic));
        CHECK(mesh.triangulation.row(0).empty());
        CHECK(intact(mesh) == "");
    }

    SECTION("by its stored row") {
        // A valid fan the automatic rule does not pick.
        std::vector<u32> stored;
        for (u32 apex = 0; apex < 5 && stored.empty(); ++apex) {
            std::vector<u32> fan;
            for (u32 c = 1; c + 1 < 5; ++c) {
                fan.insert(fan.end(), {loop[apex], loop[(apex + c) % 5], loop[(apex + c + 1) % 5]});
            }
            if (geom::RowValid(loop, held, fan) && triples(fan) != triples(automatic)) {
                stored = fan;
            }
        }
        REQUIRE_FALSE(stored.empty());
        mesh.triangulation.setRow(0, stored, std::as_const(mesh).topology().faceCount());
        CHECK(geom::Triangulate(mesh, geom::FaceId(0)) == 2);
        CHECK(liveTriangles(mesh) == triples(stored));
        // Every piece is a triangle, so no row is left anywhere.
        for (u32 f = 0; f < std::as_const(mesh).topology().faceCount(); ++f) {
            CHECK(mesh.triangulation.row(f).empty());
        }
        CHECK(intact(mesh) == "");
    }
}

TEST_CASE("WEM TriangulateAll leaves triangles alone", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(8, {{0, 1, 3, 2}, {4, 5, 6}, {0, 1, 3, 7, 2}});
    CHECK(geom::TriangulateAll(mesh) == 1 + 0 + 2);
    CHECK(liveFaces(mesh) == 6);
    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM FlipEdge rotates the diagonal", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3}, {0, 3, 2}});
    const geom::HalfedgeId shared =
        mesh.topology().findHalfedge(geom::VertexId(0), geom::VertexId(3));
    REQUIRE(shared.valid());

    REQUIRE(geom::FlipEdge(mesh, geom::Topology::edge(shared)));
    CHECK_FALSE(mesh.topology().findHalfedge(geom::VertexId(0), geom::VertexId(3)).valid());
    CHECK(mesh.topology().findHalfedge(geom::VertexId(1), geom::VertexId(2)).valid());
    CHECK(liveFaces(mesh) == 2);

    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM FlipEdge refuses a boundary edge", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3}, {0, 3, 2}});
    const geom::HalfedgeId boundary =
        mesh.topology().findHalfedge(geom::VertexId(0), geom::VertexId(1));
    REQUIRE(boundary.valid());
    CHECK_FALSE(geom::FlipEdge(mesh, geom::Topology::edge(boundary)));
}

TEST_CASE("WEM DissolveEdge merges two triangles into a quad", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3}, {0, 3, 2}});
    const geom::HalfedgeId shared =
        mesh.topology().findHalfedge(geom::VertexId(0), geom::VertexId(3));
    REQUIRE(geom::DissolveEdge(mesh, geom::Topology::edge(shared)));
    CHECK(liveFaces(mesh) == 1);
    CHECK(mesh.topology().valence(geom::FaceId(0)) == 4);

    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM DissolveVertex undoes a SplitEdge", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3}, {0, 3, 2}});
    const geom::FaceSet before = mesh.faceSet();
    const geom::HalfedgeId shared =
        mesh.topology().findHalfedge(geom::VertexId(0), geom::VertexId(3));
    const geom::VertexId inserted = geom::SplitEdge(mesh, geom::Topology::edge(shared));
    REQUIRE(inserted.valid());

    REQUIRE(geom::DissolveVertex(mesh, inserted));
    CHECK(liveVertices(mesh) == 4);
    CHECK(liveFaces(mesh) == 2);
    CHECK(mesh.topology().valence(geom::FaceId(0)) == 3);
    CHECK(mesh.topology().valence(geom::FaceId(1)) == 3);

    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM DissolveVertex refuses anything but valence 2", "[wem][geometry][ops]") {
    Mesh mesh = grid(2, 2);
    CHECK_FALSE(geom::DissolveVertex(mesh, geom::VertexId(4))); // the interior vertex
    CHECK_FALSE(geom::DissolveVertex(mesh, geom::VertexId(0)));
}

TEST_CASE("WEM CollapseEdge removes a vertex and both incident triangles",
          "[wem][geometry][ops]") {
    Mesh mesh = grid(3, 3);
    const geom::VertexId a(1 * 4 + 1); // (1,1)
    const geom::VertexId b(1 * 4 + 2); // (2,1)
    const geom::HalfedgeId h = mesh.topology().findHalfedge(a, b);
    REQUIRE(h.valid());
    REQUIRE(geom::IsCollapseLegal(mesh, h));

    const u32 facesBefore = liveFaces(mesh);
    const u32 verticesBefore = liveVertices(mesh);
    REQUIRE(geom::CollapseEdge(mesh, h));
    CHECK(liveVertices(mesh) == verticesBefore - 1);
    CHECK(liveFaces(mesh) == facesBefore - 2);

    CHECK(intact(mesh) == "");

    // And it compacts cleanly, which is the other half of the lazy-deletion
    // contract (§5.2) — `GarbageCollect`, not `Topology::garbageCollect`, because
    // only the former also remaps the attributes and the skin.
    geom::GarbageCollect(mesh);
    CHECK(liveVertices(mesh) == verticesBefore - 1);
    CHECK(liveFaces(mesh) == facesBefore - 2);
    CHECK(mesh.topology().hasDeleted() == false);
    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM IsCollapseLegal refuses a boundary-to-boundary interior edge",
          "[wem][geometry][ops]") {
    // Every vertex of a 3x1 grid is on the boundary, so the vertical interior
    // edge at x=1 would pinch the boundary loop into two.
    Mesh mesh = grid(3, 1);
    const geom::HalfedgeId h =
        mesh.topology().findHalfedge(geom::VertexId(1), geom::VertexId(1 + 4));
    REQUIRE(h.valid());
    REQUIRE_FALSE(mesh.topology().isBoundary(geom::Topology::edge(h)));
    CHECK_FALSE(geom::IsCollapseLegal(mesh, h));
    CHECK_FALSE(geom::CollapseEdge(mesh, h));
    CHECK(liveVertices(mesh) == 8);
}

// ============================================================================
// Rebuilding ops
// ============================================================================

TEST_CASE("WEM WeldVertices re-merges a repair split", "[wem][geometry][ops]") {
    // A bowtie: the repair splits vertex 0 into one copy per fan, both carrying
    // merge group 0. Welding within groups puts them back — and puts the bowtie
    // back with them, which the rebuild's repair then splits again. The mesh
    // stays valid either way, which is the property under test.
    Mesh mesh = buildMesh(5, {{0, 1, 2}, {0, 3, 4}});
    REQUIRE(mesh.repairLog.stats().verticesAdded == 1);
    const auto groups =
        mesh.attributes.get<const u32>(geom::names::kMergeGroup, geom::Domain::Vertex);
    REQUIRE(groups.size() == 6);
    CHECK(groups[5] == 0);

    const geom::WeldResult welded = geom::WeldVertices(mesh, 1e-5f, true);
    CHECK(welded.verticesMerged == 1);
    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM WeldVertices respects merge groups by default", "[wem][geometry][ops]") {
    // Two triangles that share no index but do share two positions — the F2 case.
    geom::MeshBuilder builder;
    builder.addSection(MeshSection{});
    builder.addVertex(kSpread[0]);
    builder.addVertex(kSpread[1]);
    builder.addVertex(kSpread[2]);
    builder.addVertex(kSpread[0]); // coincident with 0
    builder.addVertex(kSpread[1]); // coincident with 1
    builder.addVertex(kSpread[3]);
    builder.addTriangle(geom::VertexId(0), geom::VertexId(1), geom::VertexId(2), 0);
    builder.addTriangle(geom::VertexId(4), geom::VertexId(3), geom::VertexId(5), 0);
    Mesh mesh = std::move(builder.build().mesh);
    REQUIRE(mesh.vertexCount() == 6);

    Mesh conservative = mesh;
    CHECK(geom::WeldVertices(conservative, 1e-5f, true).verticesMerged == 0);
    CHECK(conservative.vertexCount() == 6);

    // Cleared, it welds by position alone — the F2 fix, and the destructive form.
    CHECK(geom::WeldVertices(mesh, 1e-5f, false).verticesMerged == 2);
    CHECK(mesh.vertexCount() == 4);
    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM SplitVertexByHalfedgeAttr materialises a seam", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3}, {0, 3, 2}});
    // Give one corner of vertex 0 a different UV; that is the seam.
    auto uvs = mesh.attributes.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    const geom::HalfedgeId corner = mesh.topology().halfedge(geom::FaceId(1));
    REQUIRE(mesh.topology().from(corner) == geom::VertexId(0));
    uvs[corner.index()] = Vector2f{9.0f, 9.0f};

    const std::string layer = geom::names::uv(0);
    // Two, not one: the split leaves the two faces meeting only at vertex 3,
    // and the rebuild's repair splits that bowtie as well.
    CHECK(geom::SplitVertexByHalfedgeAttr(mesh, std::span<const std::string>(&layer, 1)) == 2);
    CHECK(mesh.vertexCount() == 6);

    // The copy keeps the original's merge group — and sits right next to it,
    // because the renumbering is source-vertex-major. Together those are what
    // let the render view regroup them and leave §5.8 untouched.
    const auto groups =
        mesh.attributes.get<const u32>(geom::names::kMergeGroup, geom::Domain::Vertex);
    CHECK(groups[1] == groups[0]);
    CHECK(groups[2] != groups[0]);

    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM UnifyWinding reverses the odd face out", "[wem][geometry][ops]") {
    // Built through the face set directly: the builder's repair would split the
    // winding conflict apart before `UnifyWinding` ever saw it.
    Mesh mesh;
    geom::FaceSet faces;
    faces.vertexCount = 4;
    faces.addTriangle(0, 1, 3);
    faces.addTriangle(0, 2, 3); // traverses 0->3 as well; disagrees with the first
    mesh.setFaceSet(faces);
    auto positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, geom::Domain::Vertex, geom::AttrType::F32x3);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        positions[i] = kSpread[i];
    }
    REQUIRE_FALSE(mesh.ensureConnectivity().ok());

    CHECK(geom::UnifyWinding(mesh) == 1);
    REQUIRE(mesh.hasConnectivity());
    CHECK(liveFaces(mesh) == 2);
    CHECK(intact(mesh) == "");
    CHECK(geom::UnifyWinding(mesh) == 0); // idempotent
}

TEST_CASE("WEM MergeMeshes renumbers sections and merge groups", "[wem][geometry][ops]") {
    std::vector<Mesh> parts;
    parts.push_back(buildMesh(4, {{0, 1, 3}, {0, 3, 2}}));
    parts.push_back(buildMesh(3, {{0, 1, 2}}));
    parts[1].sections[0].name = "second";
    parts[1].sections[0].materialSlot = 3;

    Mesh merged = geom::MergeMeshes(std::span<const Mesh>(parts.data(), parts.size()));
    CHECK(merged.vertexCount() == 7);
    CHECK(merged.faceCount() == 3);
    REQUIRE(merged.sections.size() == 2);
    CHECK(merged.sections[1].materialSlot == 3);
    const auto sections = merged.faceSections();
    CHECK(sections[0] == 0);
    CHECK(sections[2] == 1);

    // Merge groups must not collide across inputs, or a later weld or render view
    // would fuse two unrelated vertices.
    const auto groups =
        merged.attributes.get<const u32>(geom::names::kMergeGroup, geom::Domain::Vertex);
    REQUIRE(groups.size() == 7);
    CHECK(groups[4] != groups[0]);

    CHECK(intact(merged) == "");
}

TEST_CASE("WEM SplitMesh yields one mesh per section", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(7, {{0, 1, 3}, {0, 3, 2}, {4, 5, 6}});
    mesh.sections.push_back(MeshSection{});
    mesh.sections.back().name = "second";
    mesh.faceSections()[2] = 1;

    std::vector<Mesh> parts = geom::SplitMesh(mesh);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].faceCount() == 2);
    CHECK(parts[0].vertexCount() == 4); // only the vertices its faces use
    CHECK(parts[1].faceCount() == 1);
    CHECK(parts[1].vertexCount() == 3);
    REQUIRE(parts[1].sections.size() == 1);
    CHECK(parts[1].sections[0].name == "second");
    CHECK(parts[1].faceSections()[0] == 0);

    CHECK(intact(parts[0]) == "");
    CHECK(intact(parts[1]) == "");
}

// ============================================================================
// Derived data
// ============================================================================

TEST_CASE("WEM RecomputeNormals averages a smooth fan", "[wem][geometry][ops]") {
    Mesh mesh = grid(2, 2);
    geom::RecomputeNormals(mesh, 1.047197551f);
    const auto normals =
        mesh.attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    REQUIRE(!normals.empty());
    // A planar grid: every corner normal is the plane's.
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        for (geom::HalfedgeId h : mesh.topology().fh(geom::FaceId(f))) {
            CHECK(std::abs(std::abs(normals[h.index()].z) - 1.0f) < 1e-5f);
        }
    }
}

TEST_CASE("WEM RecomputeNormals keeps a sharp edge sharp", "[wem][geometry][ops]") {
    // Two triangles folded 90 degrees about their shared edge.
    geom::MeshBuilder builder;
    builder.addSection(MeshSection{});
    builder.addVertex(Vector3f{0.0f, 0.0f, 0.0f});
    builder.addVertex(Vector3f{1.0f, 0.0f, 0.0f});
    builder.addVertex(Vector3f{0.0f, 1.0f, 0.0f});
    builder.addVertex(Vector3f{0.0f, 0.0f, 1.0f});
    builder.addTriangle(geom::VertexId(0), geom::VertexId(1), geom::VertexId(2), 0);
    builder.addTriangle(geom::VertexId(0), geom::VertexId(2), geom::VertexId(3), 0);
    Mesh mesh = std::move(builder.build().mesh);

    geom::RecomputeNormals(mesh, 0.5f); // well under the 90-degree fold
    const auto normals =
        mesh.attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    const geom::HalfedgeId a = mesh.topology().halfedge(geom::FaceId(0));
    const geom::HalfedgeId b = mesh.topology().halfedge(geom::FaceId(1));
    const Vector3f& na = normals[a.index()];
    const Vector3f& nb = normals[b.index()];
    const f32 dot = na.x * nb.x + na.y * nb.y + na.z * nb.z;
    CHECK(std::abs(dot) < 0.5f); // two normals at one vertex, which is the point
}

// ---- Fans of three or more faces that do not lie in one plane ---------------
// What the two cases above cannot see (EDIT_MODE_NORMALS_DESIGN.md §1.1): a
// planar grid shades the same flat or smooth, and a fold's ring has two faces.

namespace {

Mesh trianglesOf(const std::vector<Vector3f>& points,
                 const std::vector<std::array<u32, 3>>& faces) {
    geom::MeshBuilder builder;
    builder.addSection(MeshSection{});
    for (const Vector3f& p : points) {
        builder.addVertex(p);
    }
    for (const std::array<u32, 3>& f : faces) {
        builder.addTriangle(geom::VertexId(f[0]), geom::VertexId(f[1]), geom::VertexId(f[2]), 0);
    }
    return std::move(builder.build().mesh);
}

/// Every vertex radial by symmetry, and its faces 41.8 degrees apart: under any
/// smoothing angle in use.
Mesh icosahedron() {
    const f32 t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    return trianglesOf(
        {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
         {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1}},
        {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
         {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
         {3, 8, 9},  {4, 9, 5},  {2, 4, 11},  {6, 2, 10}, {8, 6, 7},   {9, 8, 1}});
}

Vector3f unit(const Vector3f& v) {
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return Vector3f{v.x / length, v.y / length, v.z / length};
}

f32 apart(const Vector3f& a, const Vector3f& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

/// The normal of @p v's corner in @p face.
Vector3f cornerNormal(const Mesh& mesh, u32 face, u32 v) {
    const auto normals =
        mesh.attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    for (const geom::HalfedgeId h : mesh.topology().fh(geom::FaceId(face))) {
        if (mesh.topology().from(h).index() == v) {
            return normals[h.index()];
        }
    }
    FAIL("the face has no such corner");
    return {};
}

void markSharp(Mesh& mesh, u32 a, u32 b) {
    const geom::HalfedgeId h =
        std::as_const(mesh).topology().findHalfedge(geom::VertexId(a), geom::VertexId(b));
    REQUIRE(h.valid());
    mesh.attributes.getOrCreate<u8>(geom::names::kSharp, geom::Domain::Edge,
                                    geom::AttrType::Bool)[geom::Topology::edge(h).index()] = 1;
}

std::vector<geom::FaceId> everyFace(const Mesh& mesh) {
    std::vector<geom::FaceId> faces;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        faces.push_back(geom::FaceId(f));
    }
    return faces;
}

} // namespace

TEST_CASE("WEM RecomputeNormals shades an icosahedron smooth", "[wem][geometry][ops][normals]") {
    for (const bool faceSet : {false, true}) {
        Mesh mesh = icosahedron();
        if (faceSet) {
            const std::vector<geom::FaceId> faces = everyFace(mesh);
            geom::RecomputeNormals(mesh, faces, geom::kDefaultShadingAngle);
        } else {
            geom::RecomputeNormals(mesh);
        }
        const Mesh& made = mesh;
        const auto positions =
            made.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        const auto normals =
            made.attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
        REQUIRE(made.vertexCount() == 12);
        for (u32 v = 0; v < 12; ++v) {
            const Vector3f radial = unit(positions[v]);
            u32 corners = 0;
            for (const geom::HalfedgeId h : made.topology().voh(geom::VertexId(v))) {
                if (made.topology().isBoundary(h)) {
                    continue;
                }
                ++corners;
                CHECK(apart(normals[h.index()], radial) < 1e-5f);
            }
            CHECK(corners == 5);
        }
    }
}

TEST_CASE("WEM RecomputeNormals breaks a ring at its hard edges, and only there",
          "[wem][geometry][ops][normals]") {
    // A pyramid's four sides round its apex. A ring of two faces cannot tell the
    // edge before a corner from the edge after it; a ring of four can.
    Mesh mesh = trianglesOf({{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
                            {{0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}});
    markSharp(mesh, 4, 0);
    markSharp(mesh, 4, 2);
    geom::RecomputeNormals(mesh, 3.14159265f); // only the flags break a fan
    const f32 s = std::sqrt(0.5f);
    CHECK(apart(cornerNormal(mesh, 0, 4), Vector3f{0, s, s}) < 1e-5f);
    CHECK(apart(cornerNormal(mesh, 1, 4), Vector3f{0, s, s}) < 1e-5f);
    CHECK(apart(cornerNormal(mesh, 2, 4), Vector3f{0, -s, s}) < 1e-5f);
    CHECK(apart(cornerNormal(mesh, 3, 4), Vector3f{0, -s, s}) < 1e-5f);
}

TEST_CASE("WEM RecomputeNormals makes one fan of a border vertex's faces",
          "[wem][geometry][ops][normals]") {
    // Three faces round vertex 0, open between the last and the first: the gap
    // is where the ring wraps, and nowhere else.
    Mesh mesh = trianglesOf(
        {{0, 0, 0}, {1, 0, 0}, {0.5f, 1, 0.4f}, {-0.5f, 1, -0.3f}, {-1, 0, 0.5f}},
        {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}});
    geom::RecomputeNormals(mesh, 3.14159265f);
    const Vector3f first = cornerNormal(mesh, 0, 0);
    CHECK(apart(cornerNormal(mesh, 1, 0), first) < 1e-6f);
    CHECK(apart(cornerNormal(mesh, 2, 0), first) < 1e-6f);
    // And it is none of the three faces' own.
    CHECK(apart(first, cornerNormal(mesh, 0, 1)) > 1e-2f);
}

TEST_CASE("WEM RecomputeNormals shades a box flat when every edge is hard",
          "[wem][geometry][ops][normals]") {
    Mesh mesh = geom::MakeBox(geom::PrimitiveParams{});
    geom::RecomputeNormals(mesh, geom::ShadingAngle(mesh));
    const Mesh& made = mesh;
    const auto normals =
        made.attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    const auto positions =
        made.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    u32 corners = 0;
    for (u32 f = 0; f < made.topology().faceCount(); ++f) {
        // A box's face is planar and axis-aligned: its normal is its centre's direction.
        Vector3f centre{0, 0, 0};
        for (const geom::VertexId v : made.topology().fv(geom::FaceId(f))) {
            centre.x += positions[v.index()].x;
            centre.y += positions[v.index()].y;
            centre.z += positions[v.index()].z;
        }
        const Vector3f outward = unit(centre);
        for (const geom::HalfedgeId h : made.topology().fh(geom::FaceId(f))) {
            ++corners;
            CHECK(apart(normals[h.index()], outward) < 1e-5f);
        }
    }
    CHECK(corners == 24);
}

// ---- An angle on a modelled mesh is stated as flags -------------------------
// (EDIT_MODE_NORMALS_PLAN.md P2). A modelled mesh shades by `sharp` alone, so a
// crease that was only ever an angle is gone at the next re-shade.

namespace {

u32 hardEdges(const Mesh& mesh) {
    u32 count = 0;
    for (const u8 flag : mesh.attributes.get<const u8>(geom::names::kSharp, geom::Domain::Edge)) {
        count += flag != 0 ? 1 : 0;
    }
    return count;
}

bool hardBetween(const Mesh& mesh, u32 a, u32 b) {
    const geom::HalfedgeId h = mesh.topology().findHalfedge(geom::VertexId(a), geom::VertexId(b));
    REQUIRE(h.valid());
    const auto sharp = mesh.attributes.get<const u8>(geom::names::kSharp, geom::Domain::Edge);
    const std::size_t e = geom::Topology::edge(h).index();
    return e < sharp.size() && sharp[e] != 0;
}

/// How many different normals the corners at @p v hold.
u32 normalsAt(const Mesh& mesh, u32 v) {
    const auto normals =
        mesh.attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    std::vector<Vector3f> seen;
    for (const geom::HalfedgeId h : mesh.topology().voh(geom::VertexId(v))) {
        if (mesh.topology().isBoundary(h)) {
            continue;
        }
        const Vector3f& n = normals[h.index()];
        if (std::none_of(seen.begin(), seen.end(),
                         [&](const Vector3f& s) { return apart(s, n) < 1e-5f; })) {
            seen.push_back(n);
        }
    }
    return static_cast<u32>(seen.size());
}

} // namespace

TEST_CASE("WEM MarkSharpByAngle states an angle as flags", "[wem][geometry][ops][normals]") {
    // The pyramid's sides meet at 70.5 degrees.
    Mesh mesh = trianglesOf({{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
                            {{0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}});
    CHECK(geom::MarkSharpByAngle(mesh, 1.396263402f) == 0); // 80 degrees: none is that far
    CHECK(!mesh.attributes.has(geom::names::kSharp, geom::Domain::Edge)); // and no layer for it
    CHECK(geom::MarkSharpByAngle(mesh, 1.047197551f) == 4); // 60: the four sides' edges
    CHECK(hardEdges(mesh) == 4);
    CHECK(!hardBetween(mesh, 0, 1)); // a border has no flag to state
    CHECK(geom::MarkSharpByAngle(mesh, 1.396263402f, true) == 0); // keepHard only adds
    CHECK(hardEdges(mesh) == 4);
    const geom::EdgeId one[] = {
        geom::Topology::edge(std::as_const(mesh).topology().findHalfedge(geom::VertexId(4), geom::VertexId(0)))};
    CHECK(geom::MarkSharpByAngle(mesh, one, 1.396263402f) == 1); // just the one asked for
    CHECK(hardEdges(mesh) == 3);
    CHECK(geom::MarkSharpByAngle(mesh, 1.396263402f) == 3);
    CHECK(hardEdges(mesh) == 0);
}

TEST_CASE("WEM a round primitive carries its creases as flags", "[wem][geometry][ops][normals]") {
    geom::PrimitiveParams params;
    params.sides = 12;
    SECTION("a cylinder: its two rims, and nothing along its side") {
        Mesh mesh = geom::MakeCylinder(params);
        CHECK(hardEdges(mesh) == 24);
        const Mesh& made = mesh;
        u32 two = 0;
        for (u32 v = 0; v < made.vertexCount(); ++v) {
            two += normalsAt(made, v) == 2 ? 1 : 0; // the side's, and the cap's
        }
        CHECK(two == made.vertexCount());

        // The move a Mesh edit makes: the rim stays hard and the side smooth.
        auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        positions[0].z += 0.25f;
        const std::vector<geom::FaceId> faces = everyFace(mesh);
        geom::RecomputeNormals(mesh, faces, geom::ShadingAngle(mesh));
        for (u32 v = 0; v < made.vertexCount(); ++v) {
            CHECK(normalsAt(made, v) == 2);
        }
    }
    SECTION("six sides meet exactly at the angle, and read soft every one") {
        params.sides = 6;
        CHECK(hardEdges(geom::MakeCylinder(params)) == 12);
    }
    SECTION("a sphere has none, and one normal a vertex") {
        params.sides = 16;
        params.segments = 8;
        Mesh mesh = geom::MakeSphere(params);
        CHECK(hardEdges(mesh) == 0);
        const Mesh& made = mesh;
        for (u32 v = 0; v < made.vertexCount(); ++v) {
            CHECK(normalsAt(made, v) == 1);
        }
    }
}

TEST_CASE("WEM RecomputeTangents writes an orthogonal frame", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(4, {{0, 1, 3}, {0, 3, 2}});
    // The fixture's UVs are (vertexIndex, 0.5) — degenerate in v, so give the
    // mesh a real parameterisation first.
    auto uvs = mesh.attributes.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    const auto positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        for (geom::HalfedgeId h : mesh.topology().fh(geom::FaceId(f))) {
            const Vector3f& p = positions[mesh.topology().from(h).index()];
            uvs[h.index()] = Vector2f{p.x, p.y};
        }
    }
    geom::RecomputeNormals(mesh);
    geom::RecomputeTangents(mesh, 0);

    const auto normals =
        mesh.attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    const auto tangents =
        mesh.attributes.get<const Vector4f>(geom::names::kTangent, geom::Domain::Halfedge);
    REQUIRE(tangents.size() == normals.size());
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        for (geom::HalfedgeId h : mesh.topology().fh(geom::FaceId(f))) {
            const Vector4f& t = tangents[h.index()];
            const Vector3f& n = normals[h.index()];
            CHECK(std::abs(t.x * n.x + t.y * n.y + t.z * n.z) < 1e-4f);
            CHECK(std::abs(std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z) - 1.0f) < 1e-4f);
        }
    }
}

// ---- One tangent rule, with the files' side for w ----------------------------
// (EDIT_MODE_NORMALS_PLAN.md P3). UVs here are the files': v runs down the image.

namespace {

/// One corner of a test triangle: its vertex and the UV it has there.
struct Corner {
    u32 vertex;
    Vector2f uv;
};

/// Triangles with a UV per corner, shaded and then given tangents.
Mesh mapped(const std::vector<Vector3f>& points, const std::vector<std::array<Corner, 3>>& faces) {
    geom::MeshBuilder builder;
    builder.addSection(MeshSection{});
    for (const Vector3f& p : points) {
        builder.addVertex(p);
    }
    for (const std::array<Corner, 3>& f : faces) {
        const geom::FaceId face = builder.addTriangle(
            geom::VertexId(f[0].vertex), geom::VertexId(f[1].vertex), geom::VertexId(f[2].vertex), 0);
        for (u32 c = 0; c < 3; ++c) {
            builder.setCornerAttr(face, c, geom::names::uv(0), f[c].uv);
        }
    }
    Mesh mesh = std::move(builder.build().mesh);
    geom::RecomputeNormals(mesh, 3.14159265f);
    geom::RecomputeTangents(mesh, 0);
    return mesh;
}

Vector4f cornerTangent(const Mesh& mesh, u32 face, u32 v) {
    const auto tangents =
        mesh.attributes.get<const Vector4f>(geom::names::kTangent, geom::Domain::Halfedge);
    for (const geom::HalfedgeId h : mesh.topology().fh(geom::FaceId(face))) {
        if (mesh.topology().from(h).index() == v) {
            return tangents[h.index()];
        }
    }
    FAIL("the face has no such corner");
    return {};
}

Vector3f xyz(const Vector4f& t) {
    return Vector3f{t.x, t.y, t.z};
}

f32 degreesApart(const Vector3f& a, const Vector3f& b) {
    const f32 d = (a.x * b.x + a.y * b.y + a.z * b.z) /
                  std::sqrt((a.x * a.x + a.y * a.y + a.z * a.z) * (b.x * b.x + b.y * b.y + b.z * b.z));
    return std::acos(std::clamp(d, -1.0f, 1.0f)) * 57.29577951f;
}

const std::vector<Vector3f> kQuad = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};

std::vector<u8> fileBytes(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// How many corners of @p mesh keep the `w` they had after a rebuild, of how many.
std::pair<u32, u32> sidesKept(Mesh mesh) {
    REQUIRE(mesh.ensureConnectivity().ok());
    const auto stored =
        std::as_const(mesh).attributes.get<const Vector4f>(geom::names::kTangent, geom::Domain::Halfedge);
    const std::vector<Vector4f> before(stored.begin(), stored.end());
    geom::RecomputeTangents(mesh, 0);
    const auto after =
        std::as_const(mesh).attributes.get<const Vector4f>(geom::names::kTangent, geom::Domain::Halfedge);
    std::pair<u32, u32> kept{0, 0};
    for (u32 f = 0; f < mesh.faceCount(); ++f) {
        for (const geom::HalfedgeId h : std::as_const(mesh).topology().fh(geom::FaceId(f))) {
            if (h.index() < before.size() && h.index() < after.size()) {
                ++kept.second;
                kept.first += (before[h.index()].w < 0.0f) == (after[h.index()].w < 0.0f) ? 1 : 0;
            }
        }
    }
    return kept;
}

} // namespace

TEST_CASE("WEM RecomputeTangents runs along u, and w has the files' side",
          "[wem][geometry][ops][tangents]") {
    // The usual map: u with x, v down the image as y goes up. cross(n, t) is +y,
    // toward decreasing v, so w is +1.
    const auto uv = [](const Vector3f& p) { return Vector2f{p.x, 1.0f - p.y}; };
    const Mesh mesh = mapped(kQuad, {{{{0, uv(kQuad[0])}, {1, uv(kQuad[1])}, {2, uv(kQuad[2])}}},
                                     {{{0, uv(kQuad[0])}, {2, uv(kQuad[2])}, {3, uv(kQuad[3])}}}});
    for (u32 f = 0; f < 2; ++f) {
        for (const geom::VertexId v : mesh.topology().fv(geom::FaceId(f))) {
            const Vector4f t = cornerTangent(mesh, f, static_cast<u32>(v.index()));
            CHECK(apart(xyz(t), Vector3f{1, 0, 0}) < 1e-5f);
            CHECK(t.w == 1.0f);
        }
    }
}

TEST_CASE("WEM RecomputeTangents flips w on a mirrored map", "[wem][geometry][ops][tangents]") {
    // u against x: the tangent turns round and the side with it.
    const auto uv = [](const Vector3f& p) { return Vector2f{1.0f - p.x, 1.0f - p.y}; };
    const Mesh mesh = mapped(kQuad, {{{{0, uv(kQuad[0])}, {1, uv(kQuad[1])}, {2, uv(kQuad[2])}}},
                                     {{{0, uv(kQuad[0])}, {2, uv(kQuad[2])}, {3, uv(kQuad[3])}}}});
    const Vector4f t = cornerTangent(mesh, 0, 0);
    CHECK(apart(xyz(t), Vector3f{-1, 0, 0}) < 1e-5f);
    CHECK(t.w == -1.0f);
}

TEST_CASE("WEM RecomputeTangents keeps two tangents across a UV seam in a smooth fan",
          "[wem][geometry][ops][tangents]") {
    // One flat quad, one normal a vertex. Its first triangle maps u along x and
    // its second u along y: the diagonal is a seam, and its two vertices hold a
    // tangent for each side.
    const Mesh mesh = mapped(kQuad, {{{{0, {0, 1}}, {1, {1, 1}}, {2, {1, 0}}}},
                                     {{{0, {0, 0}}, {2, {1, -1}}, {3, {1, 0}}}}});
    CHECK(apart(xyz(cornerTangent(mesh, 0, 0)), Vector3f{1, 0, 0}) < 1e-5f);
    CHECK(apart(xyz(cornerTangent(mesh, 1, 0)), Vector3f{0, 1, 0}) < 1e-5f);
    CHECK(apart(xyz(cornerTangent(mesh, 0, 2)), Vector3f{1, 0, 0}) < 1e-5f);
    CHECK(apart(xyz(cornerTangent(mesh, 1, 2)), Vector3f{0, 1, 0}) < 1e-5f);
}

TEST_CASE("WEM RecomputeTangents weights a corner by its angle", "[wem][geometry][ops][tangents]") {
    // Two flat triangles at vertex 0 with one UV there: 90 degrees of a map
    // along x and 45 of a map along y. By angle that is (2, 1); by UV area,
    // which is equal, it would be (1, 1).
    const Mesh mesh = mapped({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 1, 0}},
                             {{{{0, {0, 0}}, {1, {1, 0}}, {2, {0, -1}}}},
                              {{{0, {0, 0}}, {2, {1, 0}}, {3, {1, -1}}}}});
    const Vector3f expected = unit(Vector3f{2, 1, 0});
    CHECK(apart(xyz(cornerTangent(mesh, 0, 0)), expected) < 1e-5f);
    CHECK(apart(xyz(cornerTangent(mesh, 1, 0)), expected) < 1e-5f);
    CHECK(cornerTangent(mesh, 0, 0).w == 1.0f);
}

TEST_CASE("WEM RecomputeTangents is one rule for a mesh and a face set of it",
          "[wem][geometry][ops][tangents]") {
    geom::PrimitiveParams params;
    params.sides = 16;
    params.segments = 8;
    Mesh whole = geom::MakeSphere(params);
    Mesh bySet = whole;
    geom::RecomputeTangents(whole, 0);
    const std::vector<geom::FaceId> faces = everyFace(bySet);
    geom::RecomputeTangents(bySet, faces, 0);
    const auto a =
        std::as_const(whole).attributes.get<const Vector4f>(geom::names::kTangent, geom::Domain::Halfedge);
    const auto b =
        std::as_const(bySet).attributes.get<const Vector4f>(geom::names::kTangent, geom::Domain::Halfedge);
    const auto normals =
        std::as_const(whole).attributes.get<const Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
    REQUIRE(!a.empty());
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
    for (u32 f = 0; f < whole.faceCount(); ++f) {
        for (const geom::HalfedgeId h : std::as_const(whole).topology().fh(geom::FaceId(f))) {
            const Vector3f t = xyz(a[h.index()]);
            const Vector3f& n = normals[h.index()];
            CHECK(std::abs(t.x * n.x + t.y * n.y + t.z * n.z) < 1e-4f);
            CHECK(std::abs(std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z) - 1.0f) < 1e-4f);
            CHECK(std::abs(a[h.index()].w) == 1.0f);
        }
    }
}

TEST_CASE("WEM RecomputeTangents answers as Blender's MikkTSpace does",
          "[wem][geometry][ops][tangents]") {
    // tests/data/wem/tangent_oracle.txt, written by scripts/oracle/tangent_oracle.py
    // in WhiteoutFlakes: three meshes, with the normals and the tangents Blender
    // gave every corner. Its UVs have v up, ours down; its sign is then our w.
    std::ifstream in("tests/data/wem/tangent_oracle.txt");
    REQUIRE(in.good());
    struct Loop {
        u32 vertex;
        Vector2f uv;
        Vector3f normal;
        Vector3f tangent;
        f32 sign;
    };
    struct Source {
        std::string name;
        std::vector<Vector3f> points;
        std::vector<Loop> loops;
    };
    std::vector<Source> sources;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        char kind = 0;
        row >> kind;
        if (kind == 'm') {
            sources.emplace_back();
            row >> sources.back().name;
        } else if (kind == 'v') {
            Vector3f p{};
            row >> p.x >> p.y >> p.z;
            sources.back().points.push_back(p);
        } else if (kind == 'c') {
            Loop c{};
            row >> c.vertex >> c.uv.x >> c.uv.y >> c.normal.x >> c.normal.y >> c.normal.z >>
                c.tangent.x >> c.tangent.y >> c.tangent.z >> c.sign;
            sources.back().loops.push_back(c);
        }
    }
    REQUIRE(sources.size() == 3);
    u32 corners = 0;
    u32 close = 0;
    u32 sameSide = 0;
    f32 worst = 0.0f;
    for (const Source& source : sources) {
        geom::MeshBuilder builder;
        builder.addSection(MeshSection{});
        for (const Vector3f& p : source.points) {
            builder.addVertex(p);
        }
        REQUIRE(source.loops.size() % 3 == 0);
        for (std::size_t t = 0; t + 2 < source.loops.size(); t += 3) {
            const geom::FaceId face = builder.addTriangle(geom::VertexId(source.loops[t].vertex),
                                                          geom::VertexId(source.loops[t + 1].vertex),
                                                          geom::VertexId(source.loops[t + 2].vertex), 0);
            for (u32 c = 0; c < 3; ++c) {
                const Loop& loop = source.loops[t + c];
                builder.setCornerAttr(face, c, geom::names::uv(0), Vector2f{loop.uv.x, 1.0f - loop.uv.y});
                builder.setCornerAttr(face, c, geom::names::kNormal, loop.normal);
            }
        }
        Mesh mesh = std::move(builder.build().mesh);
        REQUIRE(mesh.faceCount() == source.loops.size() / 3);
        geom::RecomputeTangents(mesh, 0);
        const Mesh& made = mesh;
        for (u32 f = 0; f < made.faceCount(); ++f) {
            for (u32 c = 0; c < 3; ++c) {
                const Loop& loop = source.loops[f * 3 + c];
                const Vector4f ours = cornerTangent(made, f, loop.vertex);
                const f32 off = degreesApart(xyz(ours), loop.tangent);
                ++corners;
                close += off < 0.1f ? 1 : 0;
                sameSide += ours.w == loop.sign ? 1 : 0;
                worst = std::max(worst, off);
            }
        }
    }
    INFO("corners " << corners << ", within 0.1 degrees " << close << ", same side " << sameSide
                    << ", worst " << worst << " degrees");
    CHECK(corners == 528);
    CHECK(sameSide == corners);
    CHECK(close * 1000 >= corners * 999);
}

TEST_CASE("WEM an imported file keeps the side its tangents had after a rebuild",
          "[wem][geometry][ops][tangents]") {
    // The same three meshes as Blender exports them. A format that stored the
    // other side for w would have every one of these turn over.
    SECTION("glTF") {
        const std::vector<u8> bytes = fileBytes("tests/data/wem/tangent_oracle.glb");
        REQUIRE(!bytes.empty());
        models::gltf::ParseOutcome parsed = models::gltf::Parser::FromBytes(bytes);
        REQUIRE(parsed.asset.has_value());
        const Result<Document> document = GltfConverter().fromGltf(*parsed.asset);
        REQUIRE(document.ok());
        u32 meshes = 0;
        for (const Model& model : document->models) {
            for (const Mesh& mesh : model.meshes) {
                REQUIRE(mesh.attributes.has(geom::names::kTangent, geom::Domain::Halfedge));
                const std::pair<u32, u32> kept = sidesKept(mesh);
                INFO(mesh.name << ": " << kept.first << " of " << kept.second);
                CHECK(kept.second > 0);
                CHECK(kept.first == kept.second);
                ++meshes;
            }
        }
        CHECK(meshes == 3);
    }
    SECTION("FBX") {
        const std::vector<u8> bytes = fileBytes("tests/data/wem/tangent_oracle.fbx");
        REQUIRE(!bytes.empty());
        models::fbx::ReadOutcome read = models::fbx::Read(bytes);
        REQUIRE(read.file.has_value());
        models::fbx::SceneOutcome scene = models::fbx::Scene::Build(std::move(*read.file));
        REQUIRE(scene.scene.has_value());
        const Result<FbxImport> imported = FbxConverter().fromFbx(*scene.scene);
        REQUIRE(imported.ok());
        u32 meshes = 0;
        for (const Model& model : imported->document.models) {
            for (const Mesh& mesh : model.meshes) {
                REQUIRE(mesh.attributes.has(geom::names::kTangent, geom::Domain::Halfedge));
                const std::pair<u32, u32> kept = sidesKept(mesh);
                INFO(mesh.name << ": " << kept.first << " of " << kept.second);
                CHECK(kept.second > 0);
                CHECK(kept.first == kept.second);
                ++meshes;
            }
        }
        CHECK(meshes == 3);
    }
}

// ============================================================================
// What the modelling levels need of the ops (EDIT_MODE_MODELLING_DESIGN.md §2.7)
// ============================================================================

namespace {

/// `grid` with every edge's `crease` its index plus one and `sharp` set on odd
/// edges, and every face's smoothing group 100 + its index.
Mesh markedGrid(u32 w, u32 h) {
    Mesh mesh = grid(w, h);
    REQUIRE(mesh.ensureConnectivity().ok());
    const u32 edges = std::as_const(mesh).topology().edgeCount();
    const std::span<f32> crease = mesh.attributes.getOrCreate<f32>(
        geom::names::kCrease, geom::Domain::Edge, geom::AttrType::F32);
    const std::span<u8> sharp = mesh.attributes.getOrCreate<u8>(
        geom::names::kSharp, geom::Domain::Edge, geom::AttrType::Bool);
    for (u32 e = 0; e < edges; ++e) {
        crease[e] = static_cast<f32>(e + 1);
        sharp[e] = static_cast<u8>(e % 2);
    }
    const std::span<u32> smoothing = mesh.attributes.getOrCreate<u32>(
        geom::names::kSmoothGroup, geom::Domain::Face, geom::AttrType::U32);
    for (u32 f = 0; f < smoothing.size(); ++f) {
        smoothing[f] = 100 + f;
    }
    return mesh;
}

geom::EdgeId firstInterior(const Mesh& mesh) {
    const geom::Topology& topology = mesh.topology();
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (!topology.isBoundary(geom::EdgeId(e))) {
            return geom::EdgeId(e);
        }
    }
    return geom::EdgeId();
}

/// A mesh straight from a face set, no repair: the fixture is exactly what
/// the refusal is about.
Mesh fromFaces(u32 vertexCount, const std::vector<std::vector<u32>>& corners,
               const std::vector<Vector3f>& positions) {
    geom::FaceSet faces;
    faces.vertexCount = vertexCount;
    for (const std::vector<u32>& face : corners) {
        faces.addFace(face);
    }
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> written = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, geom::Domain::Vertex, geom::AttrType::F32x3);
    for (u32 i = 0; i < vertexCount; ++i) {
        written[i] = positions[i];
    }
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, geom::Domain::Face,
                                     geom::AttrType::U32);
    mesh.sections.emplace_back();
    REQUIRE(mesh.ensureConnectivity().ok());
    return mesh;
}

geom::EdgeId edgeBetween(const Mesh& mesh, u32 a, u32 b) {
    const geom::HalfedgeId h = mesh.topology().findHalfedge(geom::VertexId(a), geom::VertexId(b));
    REQUIRE(h.valid());
    return geom::Topology::edge(h);
}

} // namespace

TEST_CASE("WEM SplitEdge keeps the edge's flags on both halves", "[wem][geometry][ops]") {
    Mesh mesh = markedGrid(2, 2);
    // An odd interior edge, so `sharp` is set on it.
    geom::EdgeId edge;
    for (u32 e = 1; e < std::as_const(mesh).topology().edgeCount(); e += 2) {
        if (!std::as_const(mesh).topology().isBoundary(geom::EdgeId(e))) {
            edge = geom::EdgeId(e);
            break;
        }
    }
    REQUIRE(edge.valid());
    const f32 crease = mesh.attributes.get<f32>(geom::names::kCrease, geom::Domain::Edge)[edge.index()];
    const geom::VertexId v = geom::SplitEdge(mesh, edge, 0.4f);
    REQUIRE(v.valid());
    const geom::Topology& topology = std::as_const(mesh).topology();
    const std::span<const f32> creases =
        std::as_const(mesh).attributes.get<const f32>(geom::names::kCrease, geom::Domain::Edge);
    const std::span<const u8> sharps =
        std::as_const(mesh).attributes.get<const u8>(geom::names::kSharp, geom::Domain::Edge);
    u32 halves = 0;
    for (const geom::HalfedgeId h : topology.voh(v)) {
        const std::size_t e = geom::Topology::edge(h).index();
        CHECK(creases[e] == crease);
        CHECK(sharps[e] == 1);
        ++halves;
    }
    CHECK(halves == 2u);
    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM SplitFace copies the face's values and leaves the diagonal clear",
          "[wem][geometry][ops]") {
    Mesh mesh = markedGrid(1, 1);
    // The grid's two triangles joined into a quad, then cut the other way.
    const geom::EdgeId joined = firstInterior(mesh);
    const geom::FaceId quad = std::as_const(mesh).topology().face(geom::Topology::halfedge(joined, 0));
    REQUIRE(geom::DissolveEdge(mesh, joined, quad));
    const u32 group =
        mesh.attributes.get<u32>(geom::names::kSmoothGroup, geom::Domain::Face)[quad.index()];
    const u32 edgesBefore = std::as_const(mesh).topology().edgeCount();
    const geom::FaceId added = geom::SplitFace(mesh, quad, 1, 3);
    REQUIRE(added.valid());
    CHECK(mesh.attributes.get<u32>(geom::names::kSmoothGroup, geom::Domain::Face)[added.index()] ==
          group);
    // The diagonal is the one edge the cut made: a cross edge, clear.
    REQUIRE(std::as_const(mesh).topology().edgeCount() == edgesBefore + 1);
    CHECK(mesh.attributes.get<f32>(geom::names::kCrease, geom::Domain::Edge)[edgesBefore] == 0.0f);
    CHECK(mesh.attributes.get<u8>(geom::names::kSharp, geom::Domain::Edge)[edgesBefore] == 0);
    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM DissolveEdge refuses across sections", "[wem][geometry][ops]") {
    Mesh mesh = grid(1, 1);
    REQUIRE(mesh.ensureConnectivity().ok());
    mesh.sections.emplace_back();
    mesh.sections.emplace_back();
    mesh.faceSections()[1] = 1;
    CHECK_FALSE(geom::DissolveEdge(mesh, firstInterior(mesh)));
    CHECK(liveFaces(mesh) == 2);
}

TEST_CASE("WEM DissolveEdge refuses a loop that would visit a vertex twice",
          "[wem][geometry][ops]") {
    // A tetrahedron. Joining (0,1,2) and (0,3,1) is legal and makes the quad
    // (0,3,1,2); that quad and (1,3,2) share 1, 2 and 3, so joining them across
    // 1-3 would walk 2 twice.
    Mesh mesh = fromFaces(4, {{0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {0, 2, 3}},
                          {kSpread[2], kSpread[3], kSpread[4], kSpread[5]});
    REQUIRE(geom::DissolveEdge(mesh, edgeBetween(mesh, 0, 1)));
    CHECK_FALSE(geom::DissolveEdge(mesh, edgeBetween(mesh, 1, 3)));
    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM DissolveEdge refuses a face that would copy another", "[wem][geometry][ops]") {
    // Two triangles (0,1,2) and (0,2,3), capped by the quad (0,3,2,1): joining
    // the triangles across 0-2 makes the cap's corners again.
    Mesh mesh = fromFaces(4, {{0, 1, 2}, {0, 2, 3}, {0, 3, 2, 1}},
                          {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{1, 1, 0.2f},
                           Vector3f{0, 1, 0}});
    CHECK_FALSE(geom::DissolveEdge(mesh, edgeBetween(mesh, 0, 2)));
    CHECK(liveFaces(mesh) == 3);
}

TEST_CASE("WEM DissolveEdge keeps the face asked for", "[wem][geometry][ops]") {
    Mesh mesh = grid(1, 1);
    REQUIRE(mesh.ensureConnectivity().ok());
    const geom::EdgeId edge = firstInterior(mesh);
    const geom::FaceId odd = std::as_const(mesh).topology().face(geom::Topology::halfedge(edge, 1));
    REQUIRE(geom::DissolveEdge(mesh, edge, odd));
    CHECK_FALSE(std::as_const(mesh).topology().isDeleted(odd));
    CHECK(intact(mesh) == "");
}

TEST_CASE("WEM DissolveVertex refuses a face it would leave two corners",
          "[wem][geometry][ops]") {
    // A lone triangle: each corner has valence 2 on the border.
    Mesh mesh = fromFaces(3, {{0, 1, 2}}, {kSpread[0], kSpread[1], kSpread[2]});
    CHECK_FALSE(geom::DissolveVertex(mesh, geom::VertexId(0)));
    CHECK(liveFaces(mesh) == 1);
}

TEST_CASE("WEM DissolveVertex refuses neighbours already joined by an edge",
          "[wem][geometry][ops]") {
    // v (1) sits between a (0) and b (2) inside two quads, (a,v,b,p) and
    // (v,a,q,b); the triangle (a,b,q) already joins a and b.
    Mesh mesh = fromFaces(5, {{0, 1, 2, 3}, {1, 0, 4, 2}, {0, 2, 4}},
                          {Vector3f{0, 0, 0}, Vector3f{1, 0.3f, 0.2f}, Vector3f{2, 0, 0},
                           Vector3f{1, 1.5f, 0}, Vector3f{1, -1, 0.5f}});
    CHECK_FALSE(geom::DissolveVertex(mesh, geom::VertexId(1)));
    CHECK(intact(mesh) == "");

    SECTION("and without the triangle it is legal") {
        Mesh open = fromFaces(5, {{0, 1, 2, 3}, {1, 0, 4, 2}},
                              {Vector3f{0, 0, 0}, Vector3f{1, 0.3f, 0.2f}, Vector3f{2, 0, 0},
                               Vector3f{1, 1.5f, 0}, Vector3f{1, -1, 0.5f}});
        CHECK(geom::DissolveVertex(open, geom::VertexId(1)));
        CHECK(intact(open) == "");
    }
}

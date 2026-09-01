// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P2 — the §5.7 edit-operation surface.
///
/// Every case ends by re-running the C1–C9 checks. That is deliberate and it is
/// most of the value here: a half-edge surgery that produces the right counts and
/// a corrupt `next` chain looks correct to every assertion except that one.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/geometry/ops.h>

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

TEST_CASE("WEM Triangulate fans from the first corner", "[wem][geometry][ops]") {
    Mesh mesh = buildMesh(5, {{0, 1, 3, 4, 2}});
    REQUIRE(mesh.topology().valence(geom::FaceId(0)) == 5);

    CHECK(geom::Triangulate(mesh, geom::FaceId(0)) == 2);
    CHECK(liveFaces(mesh) == 3);

    // A fan, not a zigzag: every triangle carries the apex.
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        CHECK(mesh.topology().valence(geom::FaceId(f)) == 3);
        bool hasApex = false;
        for (geom::VertexId v : mesh.topology().fv(geom::FaceId(f))) {
            hasApex = hasApex || v == geom::VertexId(0);
        }
        CHECK(hasApex);
    }

    CHECK(intact(mesh) == "");
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

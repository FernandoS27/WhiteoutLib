// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P1b — `MeshBuilder` and the manifold repair. Every converter goes
/// through this builder, so what is asserted here is what "no converter can
/// produce a Topology that violates the §5.10 contract" actually means:
/// `addFace` never fails, the five §5.3 cases are repaired rather than refused,
/// and `Repair` -> `Unrepair` reproduces the input face-vertex list exactly.

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/checks.h>

using namespace whiteout;
using namespace whiteout::models::wem;
using geom::FaceSet;
using geom::MeshBuilder;
using geom::VertexId;

namespace {

MeshSection namedSection(const char* name) {
    MeshSection section;
    section.name = name;
    return section;
}

/// Adds @p count throwaway sections so face section values stay in range.
void addSections(MeshBuilder& builder, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        builder.addSection(namedSection("s"));
    }
}

/// Vertices in general position — no three collinear, so nothing here is dropped
/// as zero-area and the repair case under test is the only cause of a change.
/// The first draft of this test put every vertex on a line, and the degeneracy
/// check dropped every face, which is the check doing its job.
void addSpreadVertices(MeshBuilder& builder, u32 count) {
    static const Vector3f kLayout[] = {
        {0, 0, 0},        {1, 0, 0},        {0, 1, 0},        {1, 1, 0},
        {0.5f, 0.5f, 1},  {-1, 0.5f, 0.5f}, {0.5f, -1, 0.5f}, {1.5f, 1.5f, -1},
    };
    for (u32 i = 0; i < count; ++i) {
        builder.addVertex(kLayout[i % 8]);
    }
}

Diagnostics validateMesh(const Mesh& mesh) {
    Diagnostics diagnostics;
    geom::CheckStructural(mesh, 0, diagnostics);
    geom::CheckManifold(mesh, 0, diagnostics);
    return diagnostics;
}

} // namespace

TEST_CASE("wem builder produces a valid mesh from a clean input", "[wem][builder]") {
    MeshBuilder builder;
    addSections(builder, 1);
    const VertexId a = builder.addVertex(Vector3f{0, 0, 0});
    const VertexId b = builder.addVertex(Vector3f{1, 0, 0});
    const VertexId c = builder.addVertex(Vector3f{0, 1, 0});
    const VertexId d = builder.addVertex(Vector3f{1, 1, 0});
    builder.addTriangle(a, b, c);
    builder.addTriangle(c, b, d);

    auto outcome = builder.build();
    CHECK_FALSE(outcome.refused);
    CHECK(outcome.repairStats.verticesAdded == 0);
    CHECK(outcome.repairStats.facesDropped == 0);

    Mesh& mesh = outcome.mesh;
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.faceCount() == 2);
    CHECK(mesh.hasConnectivity());
    CHECK(mesh.repairLog.empty());

    const auto diagnostics = validateMesh(mesh);
    INFO(diagnostics.formatHistogram());
    CHECK_FALSE(diagnostics.hasErrors());

    // The position layer came through, and bounds followed.
    const auto positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(positions.size() == 4);
    CHECK(positions[3].x == 1.0f);
    CHECK(mesh.bounds.maximum.x == 1.0f);
    CHECK(mesh.bounds.minimum.y == 0.0f);

    // Every vertex is its own merge group when nothing was repaired.
    const auto groups =
        mesh.attributes.get<const u32>(geom::names::kMergeGroup, geom::Domain::Vertex);
    REQUIRE(groups.size() == 4);
    for (u32 v = 0; v < 4; ++v) {
        CHECK(groups[v] == v);
    }
}

TEST_CASE("wem builder repairs a non-manifold edge", "[wem][builder][repair]") {
    // Three faces on edge (1,2). Two fans keep the original vertices; the third
    // gets its own copies.
    MeshBuilder builder;
    addSections(builder, 1);
    addSpreadVertices(builder, 5);
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addTriangle(VertexId(2), VertexId(1), VertexId(3));
    builder.addTriangle(VertexId(1), VertexId(2), VertexId(4));

    auto outcome = builder.build();
    REQUIRE_FALSE(outcome.refused);
    CHECK(outcome.repairStats.verticesAdded > 0);
    CHECK(outcome.repairStats.facesDropped == 0);
    // No face is lost: the surface is preserved, only the vertex identity splits.
    CHECK(outcome.mesh.faceCount() == 3);

    const auto diagnostics = validateMesh(outcome.mesh);
    INFO(diagnostics.formatHistogram());
    CHECK_FALSE(diagnostics.hasErrors());

    // The split vertices carry their original's position and merge group.
    const auto positions =
        outcome.mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    const auto groups =
        outcome.mesh.attributes.get<const u32>(geom::names::kMergeGroup, geom::Domain::Vertex);
    REQUIRE(positions.size() == groups.size());
    for (std::size_t v = 5; v < positions.size(); ++v) {
        INFO("created vertex " << v);
        CHECK(groups[v] < 5);
        CHECK(positions[v].x == positions[groups[v]].x);
    }
}

TEST_CASE("wem builder repairs a bowtie vertex", "[wem][builder][repair]") {
    // Two triangles meeting at exactly one vertex: one vertex per fan.
    MeshBuilder builder;
    addSections(builder, 1);
    addSpreadVertices(builder, 5);
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addTriangle(VertexId(2), VertexId(3), VertexId(4));

    auto outcome = builder.build();
    REQUIRE_FALSE(outcome.refused);
    // Exactly one duplicate: vertex 2's second fan.
    CHECK(outcome.repairStats.verticesAdded == 1);
    CHECK(outcome.mesh.vertexCount() == 6);
    CHECK(outcome.mesh.faceCount() == 2);

    const auto diagnostics = validateMesh(outcome.mesh);
    INFO(diagnostics.formatHistogram());
    CHECK_FALSE(diagnostics.hasErrors());

    const auto groups =
        outcome.mesh.attributes.get<const u32>(geom::names::kMergeGroup, geom::Domain::Vertex);
    CHECK(groups[5] == 2);
}

TEST_CASE("wem builder repairs inconsistent winding by splitting", "[wem][builder][repair]") {
    // Flipping would change the normal and the source authored it that way, so
    // the repair splits instead.
    MeshBuilder builder;
    addSections(builder, 1);
    builder.addVertex(Vector3f{0, 0, 0});
    builder.addVertex(Vector3f{1, 0, 0});
    builder.addVertex(Vector3f{0, 1, 0});
    builder.addVertex(Vector3f{1, 1, 0});
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addTriangle(VertexId(1), VertexId(2), VertexId(3)); // same direction on (1,2)

    auto outcome = builder.build();
    REQUIRE_FALSE(outcome.refused);
    CHECK(outcome.repairStats.verticesAdded > 0);
    CHECK(outcome.mesh.faceCount() == 2);

    const auto diagnostics = validateMesh(outcome.mesh);
    INFO(diagnostics.formatHistogram());
    CHECK_FALSE(diagnostics.hasErrors());
}

TEST_CASE("wem builder repairs a duplicate face", "[wem][builder][repair]") {
    MeshBuilder builder;
    addSections(builder, 1);
    builder.addVertex(Vector3f{0, 0, 0});
    builder.addVertex(Vector3f{1, 0, 0});
    builder.addVertex(Vector3f{0, 1, 0});
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addTriangle(VertexId(0), VertexId(2), VertexId(1)); // the two-sided trick

    auto outcome = builder.build();
    REQUIRE_FALSE(outcome.refused);
    // Every corner of the second face is duplicated, so the two surfaces come
    // apart into two disjoint triangles.
    CHECK(outcome.repairStats.verticesAdded == 3);
    CHECK(outcome.mesh.vertexCount() == 6);
    CHECK(outcome.mesh.faceCount() == 2);

    const auto diagnostics = validateMesh(outcome.mesh);
    INFO(diagnostics.formatHistogram());
    CHECK_FALSE(diagnostics.hasErrors());
}

TEST_CASE("wem builder drops degenerate faces", "[wem][builder][repair]") {
    MeshBuilder builder;
    addSections(builder, 1);
    builder.addVertex(Vector3f{0, 0, 0});
    builder.addVertex(Vector3f{1, 0, 0});
    builder.addVertex(Vector3f{0, 1, 0});
    builder.addVertex(Vector3f{2, 0, 0});
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(1)); // repeated corner
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(3)); // collinear: zero area

    auto outcome = builder.build();
    REQUIRE_FALSE(outcome.refused);
    CHECK(outcome.repairStats.facesDropped == 2);
    CHECK(outcome.mesh.faceCount() == 1);
    REQUIRE(outcome.mesh.repairLog.droppedFaces.size() == 2);
    // The log says where each dropped face was, which is what makes Unrepair exact.
    CHECK(outcome.mesh.repairLog.droppedFaces[0].index == 1);
    CHECK(outcome.mesh.repairLog.droppedFaces[1].index == 2);

    const auto diagnostics = validateMesh(outcome.mesh);
    CHECK_FALSE(diagnostics.hasErrors());
}

TEST_CASE("wem repair is exactly undone by unrepair", "[wem][repair]") {
    // The property the design states: "the face-vertex list that comes out is the
    // one that went in". A converter exporting back to its source profile relies
    // on this.
    auto roundTrip = [](const FaceSet& input) {
        const auto repaired = geom::Repair(input);
        const FaceSet restored = geom::Unrepair(repaired.faces, repaired.log);
        CHECK(restored.vertexCount == input.vertexCount);
        CHECK(restored.faceValence == input.faceValence);
        CHECK(restored.cornerVertex == input.cornerVertex);
    };

    SECTION("already manifold") {
        FaceSet faces;
        faces.vertexCount = 4;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(2, 1, 3);
        roundTrip(faces);
    }

    SECTION("non-manifold edge") {
        FaceSet faces;
        faces.vertexCount = 5;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(2, 1, 3);
        faces.addTriangle(1, 2, 4);
        roundTrip(faces);
    }

    SECTION("bowtie") {
        FaceSet faces;
        faces.vertexCount = 5;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(2, 3, 4);
        roundTrip(faces);
    }

    SECTION("dropped faces in the middle") {
        FaceSet faces;
        faces.vertexCount = 4;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(1, 1, 2); // dropped
        faces.addTriangle(2, 1, 3);
        roundTrip(faces);
    }

    SECTION("everything at once") {
        FaceSet faces;
        faces.vertexCount = 8;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(2, 2, 2); // degenerate
        faces.addTriangle(2, 1, 3);
        faces.addTriangle(1, 2, 4); // third fan
        faces.addTriangle(0, 1, 2); // duplicate
        faces.addTriangle(5, 6, 7);
        faces.addTriangle(7, 6, 5); // opposite-wound duplicate
        roundTrip(faces);
    }
}

TEST_CASE("wem repair is deterministic", "[wem][repair]") {
    FaceSet faces;
    faces.vertexCount = 6;
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(2, 1, 3);
    faces.addTriangle(1, 2, 4);
    faces.addTriangle(2, 4, 5);

    const auto first = geom::Repair(faces);
    const auto second = geom::Repair(faces);

    CHECK(first.faces.vertexCount == second.faces.vertexCount);
    CHECK(first.faces.cornerVertex == second.faces.cornerVertex);
    REQUIRE(first.log.splits.size() == second.log.splits.size());
    for (std::size_t i = 0; i < first.log.splits.size(); ++i) {
        CHECK(first.log.splits[i].original == second.log.splits[i].original);
        CHECK(first.log.splits[i].created == second.log.splits[i].created);
    }
}

TEST_CASE("wem builder is deterministic in handle numbering", "[wem][builder]") {
    // Same input built twice must give identical handle numbering, or a golden
    // test cannot compare handles at all.
    const auto make = []() {
        MeshBuilder builder;
        addSections(builder, 2);
        addSpreadVertices(builder, 6);
        builder.addTriangle(VertexId(0), VertexId(1), VertexId(2), 0);
        builder.addTriangle(VertexId(2), VertexId(1), VertexId(3), 1);
        builder.addTriangle(VertexId(1), VertexId(2), VertexId(4), 1); // forces a repair
        builder.addTriangle(VertexId(3), VertexId(1), VertexId(5), 0);
        return builder.build();
    };

    auto first = make();
    auto second = make();
    REQUIRE(first.mesh.vertexCount() == second.mesh.vertexCount());
    REQUIRE(first.mesh.topology().halfedgeCount() == second.mesh.topology().halfedgeCount());
    for (u32 raw = 0; raw < first.mesh.topology().halfedgeCount(); ++raw) {
        const geom::HalfedgeId h(raw);
        CHECK(first.mesh.topology().to(h) == second.mesh.topology().to(h));
        CHECK(first.mesh.topology().next(h) == second.mesh.topology().next(h));
        CHECK(first.mesh.topology().face(h) == second.mesh.topology().face(h));
    }
    CHECK(first.mesh.faceSet().cornerVertex == second.mesh.faceSet().cornerVertex);
}

TEST_CASE("wem builder carries corner attributes to their halfedge", "[wem][builder]") {
    // The seam fix: two corners into the same vertex may hold different UVs
    // without the vertex splitting.
    MeshBuilder builder;
    addSections(builder, 1);
    const VertexId a = builder.addVertex(Vector3f{0, 0, 0});
    const VertexId b = builder.addVertex(Vector3f{1, 0, 0});
    const VertexId c = builder.addVertex(Vector3f{0, 1, 0});
    const VertexId d = builder.addVertex(Vector3f{1, 1, 0});
    const auto f0 = builder.addTriangle(a, b, c);
    const auto f1 = builder.addTriangle(c, b, d);

    // Vertex b is corner 1 of face 0 and corner 1 of face 1; give the two corners
    // different UVs, as a texture seam would.
    builder.setCornerAttr(f0, 1, geom::names::uv(0), Vector2f{0.25f, 0.0f});
    builder.setCornerAttr(f1, 1, geom::names::uv(0), Vector2f{0.75f, 0.0f});

    auto outcome = builder.build();
    REQUIRE_FALSE(outcome.refused);
    const Mesh& mesh = outcome.mesh;
    const auto uvs = mesh.attributes.get<const Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    REQUIRE(uvs.size() == mesh.topology().halfedgeCount());

    // Collect the UVs on the two halfedges that *point at* b within a face — the
    // corner attribute of corner i sits on the halfedge leaving corner i, so the
    // two seam values are on halfedges leaving b.
    std::vector<f32> seen;
    for (u32 raw = 0; raw < mesh.topology().halfedgeCount(); ++raw) {
        const geom::HalfedgeId h(raw);
        if (mesh.topology().face(h).valid() && mesh.topology().from(h) == b) {
            seen.push_back(uvs[raw].x);
        }
    }
    std::sort(seen.begin(), seen.end());
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == 0.25f);
    CHECK(seen[1] == 0.75f);
}

TEST_CASE("wem builder carries skinning across a split", "[wem][builder]") {
    MeshBuilder builder;
    addSections(builder, 1);
    addSpreadVertices(builder, 5);
    // Three influences on vertex 2 — kept at three, not padded to four (§5.6).
    builder.addInfluence(VertexId(2), 7, 0.5f);
    builder.addInfluence(VertexId(2), 9, 0.3f);
    builder.addInfluence(VertexId(2), 4, 0.2f);
    builder.addInfluence(VertexId(0), 7, 1.0f);
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addTriangle(VertexId(2), VertexId(3), VertexId(4)); // bowtie at 2

    auto outcome = builder.build();
    REQUIRE_FALSE(outcome.refused);
    const geom::SkinBinding& skin = outcome.mesh.skin;
    REQUIRE(skin.vertexCount() == outcome.mesh.vertexCount());

    const auto original = skin.forVertex(2);
    REQUIRE(original.size() == 3);
    // Sorted by descending weight, as §5.6 documents.
    CHECK(original[0].bone == 7);
    CHECK(original[1].bone == 9);
    CHECK(original[2].bone == 4);
    // And weights are kept as authored, not normalised.
    CHECK(original[0].weight == 0.5f);

    // The vertex the bowtie repair created carries the same influences.
    const auto created = skin.forVertex(5);
    REQUIRE(created.size() == 3);
    CHECK(created[0].bone == 7);
    CHECK(created[2].bone == 4);

    const auto diagnostics = validateMesh(outcome.mesh);
    INFO(diagnostics.formatHistogram());
    CHECK_FALSE(diagnostics.hasErrors());
}

TEST_CASE("wem builder refuses when the policy says so", "[wem][builder]") {
    MeshBuilder builder;
    addSections(builder, 1);
    addSpreadVertices(builder, 5);
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addTriangle(VertexId(2), VertexId(3), VertexId(4));

    auto outcome = builder.build(geom::RepairPolicy::Refuse);
    CHECK(outcome.refused);
    CHECK(outcome.mesh.faceCount() == 0);
    CHECK(outcome.repairStats.verticesAdded == 1);
}

TEST_CASE("wem builder can repair without recording", "[wem][builder]") {
    MeshBuilder builder;
    addSections(builder, 1);
    addSpreadVertices(builder, 5);
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addTriangle(VertexId(2), VertexId(3), VertexId(4));

    auto outcome = builder.build(geom::RepairPolicy::RepairSilently);
    CHECK_FALSE(outcome.refused);
    CHECK(outcome.mesh.vertexCount() == 6);
    // The repair happened; only the record of it is gone, so `Unrepair` is now
    // impossible — which is the documented trade.
    CHECK(outcome.mesh.repairLog.empty());
    CHECK(outcome.repairStats.verticesAdded == 1);
}

TEST_CASE("wem mesh sections are a face attribute, not a range", "[wem][mesh]") {
    MeshBuilder builder;
    addSections(builder, 2);
    builder.addVertex(Vector3f{0, 0, 0});
    builder.addVertex(Vector3f{1, 0, 0});
    builder.addVertex(Vector3f{0, 1, 0});
    builder.addVertex(Vector3f{1, 1, 0});
    // Interleaved on purpose: sections impose no ordering.
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2), 1);
    builder.addTriangle(VertexId(2), VertexId(1), VertexId(3), 0);

    auto outcome = builder.build();
    Mesh& mesh = outcome.mesh;
    const auto sectionOf = mesh.faceSections();
    REQUIRE(sectionOf.size() == 2);
    CHECK(sectionOf[0] == 1);
    CHECK(sectionOf[1] == 0);

    CHECK(mesh.facesOfSection(0) == std::vector<u32>{1});
    CHECK(mesh.facesOfSection(1) == std::vector<u32>{0});

    // Section bounds follow the faces that name them, not a range.
    CHECK(mesh.sections[1].bounds.minimum.x == 0.0f);
    CHECK(mesh.sections[0].bounds.maximum.x == 1.0f);

    const auto diagnostics = validateMesh(mesh);
    CHECK_FALSE(diagnostics.hasErrors());
}

TEST_CASE("wem mesh connectivity is lazy and reversible", "[wem][mesh]") {
    // §5.9: the bulk conversion path pays the kernel's storage cost never.
    Mesh mesh;
    FaceSet faces;
    faces.vertexCount = 4;
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(2, 1, 3);
    mesh.setFaceSet(faces);

    CHECK_FALSE(mesh.hasConnectivity());
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.faceCount() == 2);
    CHECK(mesh.attributes.domainCount(geom::Domain::Halfedge) == 0);

    REQUIRE(mesh.ensureConnectivity());
    CHECK(mesh.hasConnectivity());
    CHECK(mesh.attributes.domainCount(geom::Domain::Halfedge) == 10);
    CHECK(mesh.attributes.domainCount(geom::Domain::Edge) == 5);

    mesh.invalidateConnectivity();
    CHECK_FALSE(mesh.hasConnectivity());
    // The face set survives the round trip through the kernel unchanged.
    CHECK(mesh.faceSet().cornerVertex == faces.cornerVertex);
}

TEST_CASE("wem mesh edits flow back into the face set", "[wem][mesh]") {
    Mesh mesh;
    FaceSet faces;
    faces.vertexCount = 4;
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(2, 1, 3);
    mesh.setFaceSet(faces);
    REQUIRE(mesh.ensureConnectivity());

    mesh.topology().deleteFace(geom::FaceId(1));
    // The face set is regenerated on demand, so a caller never sees a stale one.
    CHECK(mesh.faceSet().faceCount() == 1);
    CHECK(mesh.faceSet().cornerVertex == std::vector<u32>{0, 1, 2});
}

TEST_CASE("wem checks catch a corrupted binding", "[wem][checks]") {
    MeshBuilder builder;
    addSections(builder, 1);
    builder.addVertex(Vector3f{0, 0, 0});
    builder.addVertex(Vector3f{1, 0, 0});
    builder.addVertex(Vector3f{0, 1, 0});
    builder.addTriangle(VertexId(0), VertexId(1), VertexId(2));
    builder.addInfluence(VertexId(0), 3, 1.0f);
    auto outcome = builder.build();
    REQUIRE_FALSE(validateMesh(outcome.mesh).hasErrors());

    SECTION("a section value out of range") {
        outcome.mesh.faceSections()[0] = 7;
        const auto diagnostics = validateMesh(outcome.mesh);
        CHECK(diagnostics.hasErrors());
        CHECK(diagnostics.countOf(DiagCode::IndexOutOfRange) == 1);
    }

    SECTION("non-monotonic skin offsets") {
        outcome.mesh.skin.offsets[1] = 99;
        const auto diagnostics = validateMesh(outcome.mesh);
        CHECK(diagnostics.hasErrors());
        CHECK(diagnostics.countOf(DiagCode::SkinBindingMalformed) > 0);
    }

    SECTION("a layer whose size disagrees with its domain") {
        outcome.mesh.attributes.layer(geom::names::kPosition, geom::Domain::Vertex)->data.resize(12);
        const auto diagnostics = validateMesh(outcome.mesh);
        CHECK(diagnostics.hasErrors());
        CHECK(diagnostics.countOf(DiagCode::AttributeCountMismatch) > 0);
    }
}

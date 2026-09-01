// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P1a — the half-edge kernel. Every assertion here is one of the C1–C9
/// interop invariants (design §5.10) or one of the five repair cases the kernel
/// must *refuse* (§5.3), because a strict kernel cannot represent a non-manifold
/// configuration at all and pretending otherwise is how a mesh goes silently
/// incomplete.

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/topology.h>

using namespace whiteout;
using namespace whiteout::models::wem;
using geom::EdgeId;
using geom::FaceId;
using geom::FaceSet;
using geom::HalfedgeId;
using geom::Topology;
using geom::VertexId;

namespace {

/// One triangle: the smallest mesh with a boundary.
FaceSet triangle() {
    FaceSet faces;
    faces.vertexCount = 3;
    faces.addTriangle(0, 1, 2);
    return faces;
}

/// Two triangles sharing edge (1,2) — one interior edge, four boundary edges.
FaceSet quadPair() {
    FaceSet faces;
    faces.vertexCount = 4;
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(2, 1, 3);
    return faces;
}

/// A closed tetrahedron: no boundary halfedge anywhere.
FaceSet tetrahedron() {
    FaceSet faces;
    faces.vertexCount = 4;
    faces.addTriangle(0, 2, 1);
    faces.addTriangle(0, 1, 3);
    faces.addTriangle(0, 3, 2);
    faces.addTriangle(1, 2, 3);
    return faces;
}

/// Asserts C1–C3 and C7 on whatever was built.
void checkKernelInvariants(const Topology& mesh) {
    for (u32 raw = 0; raw < mesh.halfedgeCount(); ++raw) {
        const HalfedgeId h(raw);
        INFO("halfedge " << raw);

        // C1: pairs, and the two derived relations.
        CHECK(Topology::opposite(Topology::opposite(h)) == h);
        CHECK(Topology::edge(h).value() == raw / 2);
        CHECK(Topology::halfedge(Topology::edge(h), raw & 1u) == h);

        // C2: next/prev/to are valid and agree.
        REQUIRE(mesh.next(h).valid());
        REQUIRE(mesh.prev(h).valid());
        REQUIRE(mesh.to(h).valid());
        CHECK(mesh.prev(mesh.next(h)) == h);
        CHECK(mesh.next(mesh.prev(h)) == h);
        CHECK(mesh.from(mesh.next(h)) == mesh.to(h));
        CHECK(mesh.to(h) != mesh.from(h));

        // C2: the face agrees around the whole loop.
        CHECK(mesh.face(h) == mesh.face(mesh.next(h)));
    }

    // C7: the entry points are valid.
    for (u32 raw = 0; raw < mesh.vertexCount(); ++raw) {
        const VertexId v(raw);
        const HalfedgeId h = mesh.outgoing(v);
        if (h.valid()) {
            INFO("vertex " << raw);
            CHECK(mesh.from(h) == v);
        }
    }
    for (u32 raw = 0; raw < mesh.faceCount(); ++raw) {
        const FaceId f(raw);
        const HalfedgeId h = mesh.halfedge(f);
        if (h.valid() && !mesh.isDeleted(f)) {
            INFO("face " << raw);
            CHECK(mesh.face(h) == f);
        }
    }
}

} // namespace

TEST_CASE("wem topology builds a single triangle", "[wem][topology]") {
    Topology mesh;
    REQUIRE(mesh.build(triangle()));

    CHECK(mesh.vertexCount() == 3);
    CHECK(mesh.faceCount() == 1);
    CHECK(mesh.edgeCount() == 3);
    CHECK(mesh.halfedgeCount() == 6);
    checkKernelInvariants(mesh);

    // C3: a boundary halfedge exists with an invalid face, and the three of them
    // chain into one loop. This is what keeps opposite() total.
    u32 boundaryCount = 0;
    for (u32 raw = 0; raw < mesh.halfedgeCount(); ++raw) {
        if (mesh.isBoundary(HalfedgeId(raw))) {
            ++boundaryCount;
            CHECK_FALSE(mesh.face(HalfedgeId(raw)).valid());
        }
    }
    CHECK(boundaryCount == 3);

    // Every vertex and every edge is on the boundary.
    for (u32 v = 0; v < 3; ++v) {
        CHECK(mesh.isBoundary(VertexId(v)));
        CHECK(mesh.valence(VertexId(v)) == 2);
    }
    for (u32 e = 0; e < 3; ++e) {
        CHECK(mesh.isBoundary(EdgeId(e)));
    }
    CHECK(mesh.valence(FaceId(0)) == 3);
}

TEST_CASE("wem topology builds a closed tetrahedron", "[wem][topology]") {
    Topology mesh;
    REQUIRE(mesh.build(tetrahedron()));

    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.faceCount() == 4);
    CHECK(mesh.edgeCount() == 6); // Euler: 4 - 6 + 4 == 2
    CHECK(mesh.halfedgeCount() == 12);
    checkKernelInvariants(mesh);

    for (u32 raw = 0; raw < mesh.halfedgeCount(); ++raw) {
        INFO("halfedge " << raw);
        CHECK_FALSE(mesh.isBoundary(HalfedgeId(raw)));
    }
    for (u32 v = 0; v < 4; ++v) {
        CHECK_FALSE(mesh.isBoundary(VertexId(v)));
        CHECK(mesh.valence(VertexId(v)) == 3);
    }
}

TEST_CASE("wem topology circulators walk the expected cycles", "[wem][topology]") {
    Topology mesh;
    REQUIRE(mesh.build(quadPair()));
    checkKernelInvariants(mesh);

    // Face circulation: three halfedges, three vertices, in loop order.
    std::vector<u32> faceVertices;
    for (VertexId v : mesh.fv(FaceId(0))) {
        faceVertices.push_back(v.value());
    }
    REQUIRE(faceVertices.size() == 3);
    std::sort(faceVertices.begin(), faceVertices.end());
    CHECK(faceVertices == std::vector<u32>{0, 1, 2});

    // Vertex circulation: vertex 1 and vertex 2 are the shared edge's ends and
    // touch both faces; vertices 0 and 3 touch one each.
    const auto faceCount = [&](u32 vertex) {
        u32 count = 0;
        for (FaceId f : mesh.vf(VertexId(vertex))) {
            if (f.valid()) {
                ++count;
            }
        }
        return count;
    };
    CHECK(faceCount(1) == 2);
    CHECK(faceCount(2) == 2);
    CHECK(faceCount(0) == 1);
    CHECK(faceCount(3) == 1);

    // vv() names the neighbours.
    std::vector<u32> neighbours;
    for (VertexId v : mesh.vv(VertexId(1))) {
        neighbours.push_back(v.value());
    }
    std::sort(neighbours.begin(), neighbours.end());
    CHECK(neighbours == std::vector<u32>{0, 2, 3});

    // findHalfedge is the inverse of to()/from().
    const HalfedgeId h = mesh.findHalfedge(VertexId(1), VertexId(2));
    REQUIRE(h.valid());
    CHECK(mesh.from(h) == VertexId(1));
    CHECK(mesh.to(h) == VertexId(2));
    CHECK_FALSE(mesh.findHalfedge(VertexId(0), VertexId(3)).valid());

    // The shared edge is interior; the other four are not.
    const EdgeId shared = Topology::edge(h);
    CHECK_FALSE(mesh.isBoundary(shared));
    u32 boundaryEdges = 0;
    for (u32 e = 0; e < mesh.edgeCount(); ++e) {
        if (mesh.isBoundary(EdgeId(e))) {
            ++boundaryEdges;
        }
    }
    CHECK(boundaryEdges == 4);
}

TEST_CASE("wem topology boundary halfedges chain into a loop", "[wem][topology]") {
    Topology mesh;
    REQUIRE(mesh.build(quadPair()));

    // Find a boundary halfedge and walk its loop: it must close, and it must
    // cover exactly the four boundary halfedges. C3 in one assertion.
    HalfedgeId start;
    for (u32 raw = 0; raw < mesh.halfedgeCount(); ++raw) {
        if (mesh.isBoundary(HalfedgeId(raw))) {
            start = HalfedgeId(raw);
            break;
        }
    }
    REQUIRE(start.valid());

    u32 walked = 0;
    for (HalfedgeId h : mesh.hl(start)) {
        CHECK(mesh.isBoundary(h));
        ++walked;
        REQUIRE(walked <= mesh.halfedgeCount());
    }
    CHECK(walked == 4);
}

TEST_CASE("wem topology build is deterministic", "[wem][topology]") {
    // The whole point of §5.9's lazy build: same input, same handle numbering,
    // every time. Without it a golden test cannot compare handles at all.
    const FaceSet faces = tetrahedron();

    Topology first;
    Topology second;
    REQUIRE(first.build(faces));
    REQUIRE(second.build(faces));

    REQUIRE(first.halfedgeCount() == second.halfedgeCount());
    for (u32 raw = 0; raw < first.halfedgeCount(); ++raw) {
        const HalfedgeId h(raw);
        INFO("halfedge " << raw);
        CHECK(first.to(h) == second.to(h));
        CHECK(first.face(h) == second.face(h));
        CHECK(first.next(h) == second.next(h));
        CHECK(first.prev(h) == second.prev(h));
    }
    for (u32 raw = 0; raw < first.vertexCount(); ++raw) {
        CHECK(first.outgoing(VertexId(raw)) == second.outgoing(VertexId(raw)));
    }
}

TEST_CASE("wem topology round-trips through its face set", "[wem][topology]") {
    const FaceSet original = tetrahedron();
    Topology mesh;
    REQUIRE(mesh.build(original));

    const FaceSet recovered = mesh.toFaceSet();
    CHECK(recovered.vertexCount == original.vertexCount);
    CHECK(recovered.faceValence == original.faceValence);
    CHECK(recovered.cornerVertex == original.cornerVertex);

    // And building the recovered set gives the same kernel back.
    Topology rebuilt;
    REQUIRE(rebuilt.build(recovered));
    for (u32 raw = 0; raw < mesh.halfedgeCount(); ++raw) {
        CHECK(mesh.to(HalfedgeId(raw)) == rebuilt.to(HalfedgeId(raw)));
        CHECK(mesh.next(HalfedgeId(raw)) == rebuilt.next(HalfedgeId(raw)));
    }
}

TEST_CASE("wem topology supports n-gons", "[wem][topology]") {
    // This is a polygonal kernel, not a triangle-only one (§5.2).
    FaceSet faces;
    faces.vertexCount = 4;
    const u32 quad[] = {0, 1, 2, 3};
    faces.addFace(std::span<const u32>(quad, 4));

    Topology mesh;
    REQUIRE(mesh.build(faces));
    CHECK(mesh.faceCount() == 1);
    CHECK(mesh.edgeCount() == 4);
    CHECK(mesh.valence(FaceId(0)) == 4);
    checkKernelInvariants(mesh);
}

TEST_CASE("wem topology refuses every non-manifold configuration", "[wem][topology]") {
    SECTION("non-manifold edge: three faces on one edge") {
        FaceSet faces;
        faces.vertexCount = 5;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(2, 1, 3);
        faces.addTriangle(1, 2, 4); // third fan on edge (1,2)
        Topology mesh;
        const auto result = mesh.build(faces);
        CHECK_FALSE(result.ok());
        CHECK(result.error == geom::BuildError::NonManifoldEdge);
        CHECK(mesh.empty());
    }

    SECTION("inconsistent winding across a shared edge") {
        FaceSet faces;
        faces.vertexCount = 4;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(1, 2, 3); // traverses (1,2) the same way as face 0
        Topology mesh;
        const auto result = mesh.build(faces);
        CHECK_FALSE(result.ok());
        CHECK(result.error == geom::BuildError::InconsistentWinding);
    }

    SECTION("bowtie vertex: two fans meeting at one vertex") {
        FaceSet faces;
        faces.vertexCount = 5;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(2, 3, 4); // shares only vertex 2
        Topology mesh;
        const auto result = mesh.build(faces);
        CHECK_FALSE(result.ok());
        CHECK(result.error == geom::BuildError::NonManifoldVertex);
        CHECK(result.vertex == 2);
    }

    SECTION("degenerate face: a repeated corner") {
        FaceSet faces;
        faces.vertexCount = 3;
        faces.addTriangle(0, 1, 1);
        Topology mesh;
        const auto result = mesh.build(faces);
        CHECK_FALSE(result.ok());
        CHECK(result.error == geom::BuildError::DegenerateFace);
        CHECK(result.face == 0);
    }

    SECTION("degenerate face: fewer than three corners") {
        FaceSet faces;
        faces.vertexCount = 3;
        const u32 edgeOnly[] = {0, 1};
        faces.addFace(std::span<const u32>(edgeOnly, 2));
        Topology mesh;
        const auto result = mesh.build(faces);
        CHECK_FALSE(result.ok());
        CHECK(result.error == geom::BuildError::DegenerateFace);
    }

    SECTION("corner naming a vertex the mesh does not have") {
        FaceSet faces;
        faces.vertexCount = 2;
        faces.addTriangle(0, 1, 2);
        Topology mesh;
        const auto result = mesh.build(faces);
        CHECK_FALSE(result.ok());
        CHECK(result.error == geom::BuildError::VertexOutOfRange);
    }

    SECTION("duplicate face, same winding, is caught as inconsistent winding") {
        FaceSet faces;
        faces.vertexCount = 3;
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(0, 1, 2);
        Topology mesh;
        const auto result = mesh.build(faces);
        CHECK_FALSE(result.ok());
        CHECK(result.error == geom::BuildError::InconsistentWinding);
    }
}

TEST_CASE("wem topology accepts an opposite-wound duplicate face", "[wem][topology]") {
    // The cheap two-sided trick: (0,1,2) and (0,2,1). Every edge still gets one
    // halfedge per direction, so this is a closed 2-manifold "pillow" — V-E+F is
    // 3-3+2 == 2 — and the *kernel* is right to build it. C6 ("no two faces share
    // the same vertex set") is a §5.7 Manifold-level rule and a §5.3 repair case,
    // not a build failure, and conflating the two would make the kernel refuse a
    // surface it can represent perfectly well.
    FaceSet faces;
    faces.vertexCount = 3;
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(0, 2, 1);

    Topology mesh;
    REQUIRE(mesh.build(faces));
    CHECK(mesh.faceCount() == 2);
    CHECK(mesh.edgeCount() == 3);
    for (u32 raw = 0; raw < mesh.halfedgeCount(); ++raw) {
        CHECK_FALSE(mesh.isBoundary(HalfedgeId(raw)));
    }
    checkKernelInvariants(mesh);
}

TEST_CASE("wem topology deletion is lazy and collection remaps", "[wem][topology]") {
    Topology mesh;
    REQUIRE(mesh.build(quadPair()));
    CHECK_FALSE(mesh.hasDeleted());

    mesh.deleteFace(FaceId(1));
    CHECK(mesh.hasDeleted());
    CHECK(mesh.isDeleted(FaceId(1)));
    // C8: the slot is still there until collection.
    CHECK(mesh.faceCount() == 2);
    // The surviving face keeps its handle and its loop.
    CHECK_FALSE(mesh.isDeleted(FaceId(0)));
    CHECK(mesh.valence(FaceId(0)) == 3);

    const auto remap = mesh.garbageCollect();
    CHECK(remap.newFaceCount == 1);
    CHECK(remap.faces[0] == 0);
    CHECK(remap.faces[1] == geom::kInvalidId);
    CHECK(mesh.faceCount() == 1);
    CHECK(mesh.edgeCount() == 3);
    CHECK(mesh.halfedgeCount() == 6);
    // Vertex 3 belonged only to the deleted face.
    CHECK(remap.vertices[3] == geom::kInvalidId);
    CHECK(remap.newVertexCount == 3);
    CHECK_FALSE(mesh.hasDeleted());
    checkKernelInvariants(mesh);

    // C1 survives collection because edges move as pairs.
    for (u32 raw = 0; raw < mesh.halfedgeCount(); ++raw) {
        CHECK(Topology::opposite(Topology::opposite(HalfedgeId(raw))) == HalfedgeId(raw));
    }
}

TEST_CASE("wem attribute layers live on their domain", "[wem][attributes]") {
    geom::AttributeSet attrs;
    attrs.setDomainCount(geom::Domain::Vertex, 4);
    attrs.setDomainCount(geom::Domain::Halfedge, 12);
    attrs.setDomainCount(geom::Domain::Face, 2);

    auto positions =
        attrs.getOrCreate<Vector3f>(geom::names::kPosition, geom::Domain::Vertex, geom::AttrType::F32x3);
    REQUIRE(positions.size() == 4);
    positions[0] = Vector3f{1, 2, 3};

    auto uvs = attrs.getOrCreate<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge,
                                           geom::AttrType::F32x2);
    CHECK(uvs.size() == 12);

    auto sections =
        attrs.getOrCreate<u32>(geom::names::kSection, geom::Domain::Face, geom::AttrType::U32);
    CHECK(sections.size() == 2);

    // The same name on a different domain is a different layer.
    CHECK(attrs.has(geom::names::kPosition, geom::Domain::Vertex));
    CHECK_FALSE(attrs.has(geom::names::kPosition, geom::Domain::Halfedge));
    CHECK(attrs.layerCount() == 3);

    // Growing a domain grows its layers and zero-fills.
    attrs.setDomainCount(geom::Domain::Vertex, 6);
    positions = attrs.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(positions.size() == 6);
    CHECK(positions[0].x == 1.0f);
    CHECK(positions[5].x == 0.0f);
    CHECK(attrs.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge).size() == 12);

    // A type mismatch yields an empty span rather than a reinterpretation.
    CHECK(attrs.get<Vector2f>(geom::names::kPosition, geom::Domain::Vertex).empty());
}

TEST_CASE("wem reserved attribute names carry their documented shape", "[wem][attributes]") {
    // §5.4's table. The render view and every converter read layers through it,
    // so a name in the table cannot be created with a different type.
    CHECK(geom::LookupReserved(geom::names::kPosition).domain == geom::Domain::Vertex);
    CHECK(geom::LookupReserved(geom::names::kPosition).type == geom::AttrType::F32x3);
    CHECK(geom::LookupReserved(geom::names::kNormal).domain == geom::Domain::Halfedge);
    CHECK(geom::LookupReserved(geom::names::kTangent).type == geom::AttrType::F32x4);
    CHECK(geom::LookupReserved(geom::names::kSection).domain == geom::Domain::Face);
    CHECK(geom::LookupReserved(geom::names::kSharp).type == geom::AttrType::Bool);

    // The two families.
    CHECK(geom::LookupReserved("uv0").type == geom::AttrType::F32x2);
    CHECK(geom::LookupReserved("uv4").domain == geom::Domain::Halfedge);
    CHECK(geom::LookupReserved("color1").type == geom::AttrType::U8x4);
    // "uv" alone is not a family member, and neither is "uvfoo".
    CHECK_FALSE(geom::LookupReserved("uv").reserved());
    CHECK_FALSE(geom::LookupReserved("uvfoo").reserved());
    CHECK_FALSE(geom::LookupReserved("myCustomThing").reserved());

    geom::AttributeSet attrs;
    attrs.setDomainCount(geom::Domain::Vertex, 2);
    // Asking for the wrong type on a reserved name gets the documented one.
    attrs.create(geom::names::kPosition, geom::Domain::Vertex, geom::AttrType::F32);
    CHECK(attrs.layer(geom::names::kPosition, geom::Domain::Vertex)->type == geom::AttrType::F32x3);
}

TEST_CASE("wem attribute remap drops and moves elements together", "[wem][attributes]") {
    geom::AttributeSet attrs;
    attrs.setDomainCount(geom::Domain::Vertex, 4);
    auto values = attrs.getOrCreate<u32>("scratch", geom::Domain::Vertex, geom::AttrType::U32);
    for (u32 i = 0; i < 4; ++i) {
        values[i] = 100 + i;
    }

    // Drop element 1; the rest close up. This is the shape GarbageCollection's
    // table has, which is why the two run through the same call.
    const u32 remap[] = {0, geom::kInvalidId, 1, 2};
    attrs.remapDomain(geom::Domain::Vertex, std::span<const u32>(remap, 4), 3);

    CHECK(attrs.domainCount(geom::Domain::Vertex) == 3);
    values = attrs.get<u32>("scratch", geom::Domain::Vertex);
    REQUIRE(values.size() == 3);
    CHECK(values[0] == 100);
    CHECK(values[1] == 102);
    CHECK(values[2] == 103);
}

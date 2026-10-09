// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The shading rules under the Normals workspace (EDIT_MODE_NORMALS_DESIGN.md
/// §5, EDIT_MODE_NORMALS_PLAN.md N1): the weighting and its fit, partners and
/// the fans that cross them, custom normals, the first write, the smoothing
/// commands and what each recomputes, smooth groups, and Check's rows.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/shading.h>
#include <whiteout/models/wem/meshes/remove.h>

using namespace whiteout;
using namespace whiteout::models::wem;
using geom::Domain;
using geom::ElementSet;
using geom::FaceId;
using geom::HalfedgeId;
using geom::Topology;
using geom::VertexId;
namespace shading = geom::shading;
using shading::Corner;
using shading::Level;
using shading::MeshEdge;
using shading::Surface;
using shading::Weighting;

namespace {

Vector3f unit(const Vector3f& v) {
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return Vector3f{v.x / length, v.y / length, v.z / length};
}

f32 apart(const Vector3f& a, const Vector3f& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

Mesh trianglesOf(const std::vector<Vector3f>& points, const std::vector<std::array<u32, 3>>& faces) {
    geom::MeshBuilder builder;
    builder.addSection(MeshSection{});
    for (const Vector3f& p : points) {
        builder.addVertex(p);
    }
    for (const std::array<u32, 3>& f : faces) {
        builder.addTriangle(VertexId(f[0]), VertexId(f[1]), VertexId(f[2]), 0);
    }
    return std::move(builder.build().mesh);
}

std::vector<u8> bytesOf(const Mesh& mesh, const char* layer, Domain domain) {
    const geom::AttrLayer* found = mesh.attributes.layer(layer, domain);
    return found != nullptr ? found->data : std::vector<u8>{};
}

std::span<const Vector3f> normalsOf(const Mesh& mesh) {
    return mesh.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
}

bool customAt(const Mesh& mesh, u32 halfedge) {
    const auto custom = mesh.attributes.get<const u8>(geom::names::kNormalCustom, Domain::Halfedge);
    return halfedge < custom.size() && custom[halfedge] != 0;
}

u32 hardEdges(const Mesh& mesh) {
    u32 count = 0;
    for (const u8 flag : mesh.attributes.get<const u8>(geom::names::kSharp, Domain::Edge)) {
        count += flag != 0 ? 1 : 0;
    }
    return count;
}

/// A sphere whose faces differ in size, shape and corner angle, so the four
/// weightings give four different normals. Modelled, with no hard edge.
Mesh lumpy() {
    geom::PrimitiveParams params;
    params.sides = 16;
    params.segments = 8;
    Mesh mesh = geom::MakeSphere(params);
    for (Vector3f& p : mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex)) {
        const f32 r = 1.0f + 0.2f * std::sin(3.0f * p.x + 1.0f) * std::cos(2.0f * p.y) + 0.12f * std::sin(4.0f * p.z);
        p = Vector3f{p.x * r, p.y * r * 1.3f, p.z * r};
    }
    geom::RecomputeNormals(mesh, geom::ShadingAngle(mesh));
    geom::RecomputeTangents(mesh, 0);
    return mesh;
}

/// A sphere cut in two meshes along its equator, as a material border cuts a
/// model: each half modelled, its corners holding the whole sphere's normals.
std::array<Mesh, 2> halves() {
    geom::PrimitiveParams params;
    params.sides = 16;
    params.segments = 8;
    Mesh whole = geom::MakeSphere(params);
    geom::RecomputeTangents(whole, 0);
    whole.sections.push_back(whole.sections[0]);
    const std::span<u32> sectionOf = whole.faceSections();
    const auto positions = std::as_const(whole).attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 f = 0; f < whole.faceCount(); ++f) {
        f32 height = 0.0f;
        for (const VertexId v : std::as_const(whole).topology().fv(FaceId(f))) {
            height += positions[v.index()].z;
        }
        sectionOf[f] = height > 0.0f ? 0 : 1;
    }
    std::vector<Mesh> parts = geom::SplitMesh(whole);
    REQUIRE(parts.size() == 2);
    for (Mesh& part : parts) {
        REQUIRE(part.ensureConnectivity().ok());
        part.attributes.getOrCreate<u8>(geom::names::kModelled, Domain::Mesh, geom::AttrType::Bool)[0] = 1;
    }
    return {std::move(parts[0]), std::move(parts[1])};
}

/// Edge @p which of @p mesh's border, by index order.
u32 borderEdge(const Mesh& mesh, u32 which) {
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (mesh.topology().isBoundary(geom::EdgeId(e)) && which-- == 0) {
            return e;
        }
    }
    FAIL("no such border edge");
    return 0;
}

u32 valence(const Topology& topology, VertexId v) {
    u32 faces = 0;
    for (const HalfedgeId h : topology.voh(v)) {
        faces += topology.isBoundary(h) ? 0 : 1;
    }
    return faces;
}

/// An edge of @p mesh with a face on both sides, and four faces round each of
/// its ends, neither on the border: the ordinary case, away from a pole.
u32 innerEdge(const Mesh& mesh) {
    const Topology& topology = mesh.topology();
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        const HalfedgeId h = Topology::halfedge(geom::EdgeId(e), 0);
        if (!topology.isBoundary(geom::EdgeId(e)) && !topology.isBoundary(topology.from(h)) &&
            !topology.isBoundary(topology.to(h)) && valence(topology, topology.from(h)) == 4 &&
            valence(topology, topology.to(h)) == 4) {
            return e;
        }
    }
    FAIL("no inner edge");
    return 0;
}

/// The first edge of @p mesh with a face on both sides.
u32 sharedEdge(const Mesh& mesh) {
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (!mesh.topology().isBoundary(geom::EdgeId(e))) {
            return e;
        }
    }
    FAIL("no shared edge");
    return 0;
}

std::vector<ElementSet> pick(u32 meshes, u32 mesh, std::vector<u32> vertices, std::vector<u32> edges,
                             std::vector<u32> faces) {
    std::vector<ElementSet> out(meshes);
    out[mesh].vertices = std::move(vertices);
    out[mesh].edges = std::move(edges);
    out[mesh].faces = std::move(faces);
    return out;
}

f32 across(const Surface& surface, MeshEdge edge) {
    f32 widest = 0.0f;
    for (const auto& [here, there] : surface.across(edge)) {
        widest = std::max(widest, apart(normalsOf(surface.mesh(here.mesh))[here.halfedge],
                                        normalsOf(surface.mesh(there.mesh))[there.halfedge]));
    }
    return widest;
}

} // namespace

// ============================================================================
// The weighting
// ============================================================================

TEST_CASE("shading: a mesh shades evenly until it is told otherwise", "[wem][geometry][shading]") {
    Mesh mesh = lumpy();
    CHECK(shading::WeightingOf(mesh) == Weighting::Even);
    CHECK_FALSE(shading::IsAdopted(mesh));
    const std::vector<u8> before = bytesOf(mesh, geom::names::kNormal, Domain::Halfedge);

    // Adopted as Even, a recompute is the same to the bit: one rule.
    shading::SetWeighting(mesh, Weighting::Even);
    CHECK(shading::IsAdopted(mesh));
    geom::RecomputeNormals(mesh, geom::ShadingAngle(mesh));
    CHECK(bytesOf(mesh, geom::names::kNormal, Domain::Halfedge) == before);

    shading::SetWeighting(mesh, Weighting::AreaAngle);
    geom::RecomputeNormals(mesh, geom::ShadingAngle(mesh));
    CHECK(bytesOf(mesh, geom::names::kNormal, Domain::Halfedge) != before);
}

TEST_CASE("shading: the fit finds the rule that made a mesh's normals", "[wem][geometry][shading]") {
    for (const Weighting made : {Weighting::Even, Weighting::Angle, Weighting::Area, Weighting::AreaAngle}) {
        Mesh mesh = lumpy();
        shading::SetWeighting(mesh, made);
        geom::RecomputeNormals(mesh, geom::ShadingAngle(mesh));
        mesh.attributes.remove(geom::names::kShading, Domain::Mesh);
        const shading::WeightingFit fit = shading::FitWeighting(mesh);
        INFO("made with " << static_cast<int>(made));
        CHECK(fit.fitted);
        CHECK(fit.weighting == made);
        CHECK(fit.medians[static_cast<u32>(made)] < 1e-3f);
        // And the others are visibly worse, or the fixture tests nothing.
        for (u32 w = 0; w < shading::kWeightingCount; ++w) {
            if (w != static_cast<u32>(made)) {
                CHECK(fit.medians[w] > 2e-3f);
            }
        }
        // Adopting it stores that rule, so re-shading it whole moves nothing.
        const std::vector<u8> before = bytesOf(mesh, geom::names::kNormal, Domain::Halfedge);
        Mesh* one[] = {&mesh};
        Surface surface(one);
        shading::Adopt(surface);
        CHECK(shading::WeightingOf(mesh) == made);
        shading::ReshadeAll(surface);
        CHECK(bytesOf(mesh, geom::names::kNormal, Domain::Halfedge) == before);
    }
    SECTION("flat faces fit every rule alike, and Even wins the tie") {
        Mesh box = geom::MakeBox(geom::PrimitiveParams{});
        const shading::WeightingFit fit = shading::FitWeighting(box);
        CHECK(fit.fitted);
        CHECK(fit.weighting == Weighting::Even);
    }
    SECTION("normals no rule made take By angle") {
        Mesh mesh = lumpy();
        for (Vector3f& n : mesh.attributes.get<Vector3f>(geom::names::kNormal, Domain::Halfedge)) {
            n = Vector3f{1.0f, 0.0f, 0.0f};
        }
        const shading::WeightingFit fit = shading::FitWeighting(mesh);
        CHECK_FALSE(fit.fitted);
        CHECK(fit.weighting == Weighting::Angle);
    }
    SECTION("and so does a mesh with none") {
        Mesh mesh = lumpy();
        mesh.attributes.remove(geom::names::kNormal, Domain::Halfedge);
        const shading::WeightingFit fit = shading::FitWeighting(mesh);
        CHECK_FALSE(fit.fitted);
        CHECK(fit.weighting == Weighting::Angle);
    }
}

// ============================================================================
// Custom normals
// ============================================================================

TEST_CASE("shading: a custom normal stays through a recompute, and its face still counts",
          "[wem][geometry][shading]") {
    Mesh mesh = lumpy();
    const Topology& topology = std::as_const(mesh).topology();
    const u32 edge = innerEdge(mesh);
    const HalfedgeId corner = Topology::halfedge(geom::EdgeId(edge), 0);
    const u32 vertex = static_cast<u32>(topology.from(corner).index());
    const Vector3f automatic = normalsOf(mesh)[corner.index()];
    Mesh* one[] = {&mesh};
    Surface surface(one);

    const Corner corners[] = {Corner{0, corner.value()}};
    const Vector3f set[] = {Vector3f{3.0f, 0.0f, 0.0f}};
    shading::SetNormals(surface, corners, set);
    CHECK(apart(normalsOf(mesh)[corner.index()], Vector3f{1, 0, 0}) < 1e-6f); // unit length
    CHECK(customAt(mesh, corner.value()));

    geom::RecomputeNormals(mesh, geom::ShadingAngle(mesh));
    CHECK(apart(normalsOf(mesh)[corner.index()], Vector3f{1, 0, 0}) < 1e-6f);
    // The others at its vertex are what they were: its face is in their sum.
    u32 others = 0;
    for (const HalfedgeId h : topology.voh(VertexId(vertex))) {
        if (h != corner && !topology.isBoundary(h)) {
            ++others;
            CHECK(apart(normalsOf(mesh)[h.index()], automatic) < 1e-6f);
            CHECK_FALSE(customAt(mesh, h.value()));
        }
    }
    CHECK(others >= 2);

    // Keep marks; Reset recomputes the whole fan and clears.
    const Corner another[] = {Corner{0, topology.next(corner).value()}};
    CHECK(shading::KeepNormals(surface, another) == 1);
    CHECK(shading::KeepNormals(surface, another) == 0);
    CHECK(shading::ResetNormals(surface, corners) >= 3);
    CHECK_FALSE(customAt(mesh, corner.value()));
    CHECK(apart(normalsOf(mesh)[corner.index()], automatic) < 1e-6f);
    CHECK(customAt(mesh, another[0].halfedge)); // not in that fan: left
}

TEST_CASE("shading: a new weighting re-shades every fan and leaves a custom normal", "[wem][geometry][shading]") {
    Mesh mesh = lumpy();
    const HalfedgeId corner = Topology::halfedge(geom::EdgeId(innerEdge(mesh)), 0);
    Mesh* one[] = {&mesh};
    Surface surface(one);
    shading::Adopt(surface);
    const Corner corners[] = {Corner{0, corner.value()}};
    const Vector3f set[] = {Vector3f{1.0f, 0.0f, 0.0f}};
    shading::SetNormals(surface, corners, set);
    const std::vector<u8> before = bytesOf(mesh, geom::names::kNormal, Domain::Halfedge);

    shading::SetWeighting(mesh, shading::WeightingOf(mesh) == Weighting::AreaAngle ? Weighting::Even
                                                                                   : Weighting::AreaAngle);
    CHECK(shading::ReshadeAll(surface) > 0);
    CHECK(bytesOf(mesh, geom::names::kNormal, Domain::Halfedge) != before); // the automatic ones moved
    CHECK(apart(normalsOf(mesh)[corner.index()], Vector3f{1, 0, 0}) < 1e-6f);
    CHECK(customAt(mesh, corner.value()));
}

TEST_CASE("shading: adopting a mesh folds its smoothing groups into hard edges", "[wem][geometry][shading]") {
    Mesh mesh = lumpy();
    REQUIRE(hardEdges(mesh) == 0);
    const Topology& topology = std::as_const(mesh).topology();
    // Two groups, split by which side of x = 0 a face starts on.
    {
        const std::span<const Vector3f> positions =
            std::as_const(mesh).attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
        const std::span<u32> groups =
            mesh.attributes.getOrCreate<u32>(geom::names::kSmoothGroup, Domain::Face, geom::AttrType::U32);
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            groups[f] = positions[topology.from(topology.halfedge(geom::FaceId(f))).index()].x < 0.0f ? 1u : 2u;
        }
    }
    const std::vector<u32> groups = [&] {
        const std::span<const u32> held =
            std::as_const(mesh).attributes.get<const u32>(geom::names::kSmoothGroup, Domain::Face);
        return std::vector<u32>(held.begin(), held.end());
    }();
    Mesh* one[] = {&mesh};
    Surface surface(one);
    shading::Adopt(surface);

    // One truth from here on: the layer is gone, and an edge is hard exactly
    // where its two faces were in different groups.
    CHECK_FALSE(mesh.attributes.has(geom::names::kSmoothGroup, Domain::Face));
    const std::span<const u8> sharp = std::as_const(mesh).attributes.get<const u8>(geom::names::kSharp, Domain::Edge);
    u32 between = 0;
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        if (topology.isDeleted(geom::EdgeId(e)) || topology.isBoundary(geom::EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(geom::EdgeId(e), 0);
        const bool differ = groups[topology.face(h).index()] != groups[topology.face(Topology::opposite(h)).index()];
        between += differ ? 1 : 0;
        INFO("edge " << e);
        CHECK((e < sharp.size() && sharp[e] != 0) == differ);
    }
    CHECK(between > 8);
    CHECK(hardEdges(mesh) == between);
}

TEST_CASE("shading: a custom normal turns with its face", "[wem][geometry][shading]") {
    // A quad in the xy plane, one triangle's corners aimed up and along x.
    Mesh mesh = trianglesOf({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {{0, 1, 2}, {0, 2, 3}});
    geom::RecomputeNormals(mesh, 3.14159265f);
    Mesh* one[] = {&mesh};
    Surface surface(one);
    const std::vector<Corner> corners = shading::CornersOf(surface, pick(1, 0, {}, {}, {0}), Level::Polygon);
    REQUIRE(corners.size() == 3);
    const Vector3f aimed = unit(Vector3f{1, 0, 1});
    const std::vector<Vector3f> values(3, aimed);
    shading::SetNormals(surface, corners, values);

    // The whole quad stood up about x: y goes to z.
    const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
    const std::vector<Vector3f> before(positions.begin(), positions.end());
    for (Vector3f& p : positions) {
        p = Vector3f{p.x, -p.z, p.y};
    }
    const FaceId faces[] = {FaceId(0), FaceId(1)};
    shading::TurnCustomNormals(mesh, faces, before);
    const Vector3f turned = unit(Vector3f{1, -1, 0});
    for (const Corner& corner : corners) {
        CHECK(apart(normalsOf(mesh)[corner.halfedge], turned) < 1e-4f);
        CHECK(customAt(mesh, corner.halfedge));
    }
    // The other triangle's are automatic, and a turn is not a recompute.
    for (const HalfedgeId h : std::as_const(mesh).topology().fh(FaceId(1))) {
        CHECK(apart(normalsOf(mesh)[h.index()], Vector3f{0, 0, 1}) < 1e-6f);
    }
}

TEST_CASE("shading: a custom mark is not a value the surface holds", "[wem][geometry][shading]") {
    // Two quads with a column of vertices each where they meet, as a file
    // splits a seam: the weld joins them, marked or not, and marks no seam.
    const auto strip = [](bool marked) {
        geom::MeshBuilder builder;
        builder.addSection(MeshSection{});
        for (const f32 x : {0.0f, 1.0f, 1.0f, 2.0f}) {
            builder.addVertex(Vector3f{x, 0, 0});
            builder.addVertex(Vector3f{x, 1, 0});
        }
        const u32 quads[2][4] = {{0, 2, 3, 1}, {4, 6, 7, 5}};
        for (const auto& quad : quads) {
            const std::vector<VertexId> loop{VertexId(quad[0]), VertexId(quad[1]), VertexId(quad[2]), VertexId(quad[3])};
            const FaceId face = builder.addFace(loop, 0);
            for (u32 c = 0; c < 4; ++c) {
                builder.setCornerAttr(face, c, geom::names::kNormal, Vector3f{0, 0, 1});
            }
        }
        Mesh mesh = std::move(builder.build().mesh);
        if (marked) {
            const auto custom =
                mesh.attributes.getOrCreate<u8>(geom::names::kNormalCustom, Domain::Halfedge, geom::AttrType::Bool);
            for (const HalfedgeId h : std::as_const(mesh).topology().fh(FaceId(0))) {
                custom[h.index()] = 1;
            }
        }
        return mesh;
    };
    Mesh plain = strip(false);
    Mesh marked = strip(true);
    const geom::PrepareReport a = geom::PrepareForModelling(plain);
    const geom::PrepareReport b = geom::PrepareForModelling(marked);
    CHECK(a.verticesWelded == 2);
    CHECK(b.verticesWelded == a.verticesWelded);
    CHECK(b.seamsMarked == 0);
    CHECK(b.sharpMarked == 0);
    // And across the edge they now share, one side marked: still not a seam.
    REQUIRE(marked.ensureConnectivity().ok());
    const u32 shared = sharedEdge(marked);
    CHECK_FALSE(geom::SeamBetween(marked, Topology::halfedge(geom::EdgeId(shared), 0)));

    // Nor does it keep two meshes from merging.
    Model model;
    model.meshes.push_back(strip(false));
    model.meshes.push_back(strip(true));
    const u32 both[] = {0, 1};
    CHECK(MergeMeshesInto(model, both, 0).ok);
}

// ============================================================================
// Partners, and the fans that cross them
// ============================================================================

TEST_CASE("shading: a border on a border the other way round is one edge", "[wem][geometry][shading]") {
    // Three triangles on the edge 0-1: A and B run it opposite ways, C as A.
    Mesh a = trianglesOf({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}});
    Mesh b = trianglesOf({{0, 0, 0}, {1, 0, 0}, {0, -1, 0}}, {{1, 0, 2}});
    Mesh c = trianglesOf({{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 1, 2}});
    const auto partners = [](std::span<Mesh* const> meshes) {
        const Surface surface(meshes);
        u32 count = 0;
        for (u32 m = 0; m < surface.meshCount(); ++m) {
            for (u32 e = 0; e < surface.mesh(m).topology().edgeCount(); ++e) {
                count += surface.partner(MeshEdge{m, e}).has_value() ? 1 : 0;
            }
        }
        return count;
    };
    Mesh* opposite[] = {&a, &b};
    CHECK(partners(opposite) == 2); // each is the other's
    Mesh* same[] = {&a, &c};
    CHECK(partners(same) == 0); // two sheets folded over one another, not a surface
    Mesh* three[] = {&a, &b, &c};
    CHECK(partners(three) == 0); // three on one line
}

TEST_CASE("shading: a sphere cut in two shades as one where its halves are adopted",
          "[wem][geometry][shading]") {
    std::array<Mesh, 2> parts = halves();
    Mesh* meshes[] = {&parts[0], &parts[1]};
    const u32 edge = borderEdge(parts[0], 3);

    SECTION("unadopted, each half shades to its own border") {
        Surface surface(meshes);
        REQUIRE(surface.partner(MeshEdge{0, edge}).has_value());
        shading::ReshadeAll(surface);
        CHECK(across(surface, MeshEdge{0, edge}) > 0.1f);
    }
    SECTION("adopted, soft across is one normal and hard is two") {
        Surface surface(meshes);
        const std::vector<u8> normals[] = {bytesOf(parts[0], geom::names::kNormal, Domain::Halfedge),
                                           bytesOf(parts[1], geom::names::kNormal, Domain::Halfedge)};
        const std::vector<u8> tangents = bytesOf(parts[0], geom::names::kTangent, Domain::Halfedge);
        const shading::AdoptReport report = shading::Adopt(surface);
        CHECK(report.meshes == 2);
        CHECK(report.partnersSoft == 16); // the whole equator: the file drew it smooth
        CHECK(report.partnersHard == 0);
        // The first write moves no normal.
        CHECK(bytesOf(parts[0], geom::names::kNormal, Domain::Halfedge) == normals[0]);
        CHECK(bytesOf(parts[1], geom::names::kNormal, Domain::Halfedge) == normals[1]);
        CHECK(bytesOf(parts[0], geom::names::kTangent, Domain::Halfedge) == tangents);
        CHECK(shading::Adopt(surface).meshes == 0);

        shading::ReshadeAll(surface);
        for (u32 i = 0; i < 16; ++i) {
            CHECK(across(surface, MeshEdge{0, borderEdge(parts[0], i)}) < 1e-6f);
        }

        const shading::ShadingChange hard =
            shading::Sharpen(surface, pick(2, 0, {}, {edge}, {}), Level::Edge, true);
        CHECK(hard.edges == 1);
        const MeshEdge twin = *surface.partner(MeshEdge{0, edge});
        CHECK(hardEdges(parts[0]) == 1);
        CHECK(hardEdges(parts[1]) == 1);
        CHECK(surface.hard(twin));
        shading::FinishShading(surface, hard.corners);
        // One edge alone parts nothing, as in one mesh: the fans at its ends
        // still meet round the other side.
        CHECK(across(surface, MeshEdge{0, edge}) < 1e-6f);
        // Asked of the twin, it is the same edge: nothing more to do.
        CHECK(shading::Sharpen(surface, pick(2, twin.mesh, {}, {twin.edge}, {}), Level::Edge, true).empty());

        // The whole border hard: two normals all along it.
        std::vector<u32> border;
        for (u32 i = 0; i < 16; ++i) {
            border.push_back(borderEdge(parts[0], i));
        }
        const shading::ShadingChange all = shading::Sharpen(surface, pick(2, 0, {}, border, {}), Level::Edge, true);
        CHECK(all.edges == 15);
        shading::FinishShading(surface, all.corners);
        for (const u32 e : border) {
            CHECK(across(surface, MeshEdge{0, e}) > 0.1f);
        }
        // And soft again, asked of the other half: one.
        std::vector<u32> twins;
        for (const u32 e : border) {
            twins.push_back(surface.partner(MeshEdge{0, e})->edge);
        }
        const shading::ShadingChange soft = shading::Sharpen(surface, pick(2, 1, {}, twins, {}), Level::Edge, false);
        CHECK(soft.edges == 16);
        CHECK(hardEdges(parts[0]) == 0);
        CHECK(hardEdges(parts[1]) == 0);
        shading::FinishShading(surface, soft.corners);
        for (const u32 e : border) {
            CHECK(across(surface, MeshEdge{0, e}) < 1e-6f);
        }
    }
    SECTION("the first write marks hard the partners the file drew apart") {
        // The lower half's border corners pulled 20 degrees off the upper's.
        const Topology& topology = std::as_const(parts[1]).topology();
        const auto normals = parts[1].attributes.get<Vector3f>(geom::names::kNormal, Domain::Halfedge);
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                if (topology.isBoundary(topology.from(h))) {
                    normals[h.index()] = unit(Vector3f{normals[h.index()].x, normals[h.index()].y, -0.36f});
                }
            }
        }
        Surface surface(meshes);
        const shading::AdoptReport report = shading::Adopt(surface);
        CHECK(report.partnersHard == 16);
        CHECK(report.partnersSoft == 0);
        CHECK(hardEdges(parts[0]) == 16);
        CHECK(hardEdges(parts[1]) == 16);
    }
}

TEST_CASE("shading: a smoothing command moves nothing outside the fans it changed",
          "[wem][geometry][shading]") {
    // Normals no recompute would write (the sphere's own, exactly radial), so
    // a fan recomputed by mistake shows.
    std::array<Mesh, 2> parts = halves();
    for (Mesh& part : parts) {
        const Topology& topology = std::as_const(part).topology();
        const auto positions = std::as_const(part).attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
        const auto normals = part.attributes.get<Vector3f>(geom::names::kNormal, Domain::Halfedge);
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                const Vector3f& p = positions[topology.from(h).index()];
                normals[h.index()] = unit(Vector3f{p.x * 1.02f, p.y, p.z * 0.97f});
            }
        }
    }
    Mesh* meshes[] = {&parts[0], &parts[1]};
    Surface surface(meshes);
    shading::Adopt(surface);
    const std::vector<u8> normals = bytesOf(parts[0], geom::names::kNormal, Domain::Halfedge);
    const std::vector<u8> tangents = bytesOf(parts[0], geom::names::kTangent, Domain::Halfedge);
    const std::vector<u8> other = bytesOf(parts[1], geom::names::kNormal, Domain::Halfedge);

    const u32 edge = innerEdge(parts[0]);
    const auto outsideUnchanged = [&](const shading::ShadingChange& change) {
        std::vector<u8> inFans(std::as_const(parts[0]).topology().halfedgeCount(), 0);
        for (const Corner& corner : change.corners) {
            for (const Corner& member : surface.fan(surface.fanOf(corner))) {
                if (member.mesh == 0) {
                    inFans[member.halfedge] = 1;
                }
            }
        }
        const std::vector<u8> now = bytesOf(parts[0], geom::names::kNormal, Domain::Halfedge);
        const std::vector<u8> nowTangents = bytesOf(parts[0], geom::names::kTangent, Domain::Halfedge);
        u32 moved = 0;
        u32 movedOutside = 0;
        for (u32 h = 0; h < inFans.size(); ++h) {
            const bool same = std::memcmp(now.data() + 12 * h, normals.data() + 12 * h, 12) == 0 &&
                              std::memcmp(nowTangents.data() + 16 * h, tangents.data() + 16 * h, 16) == 0;
            moved += same ? 0 : 1;
            movedOutside += !same && inFans[h] == 0 ? 1 : 0;
        }
        CHECK(movedOutside == 0);
        CHECK(bytesOf(parts[1], geom::names::kNormal, Domain::Halfedge) == other);
        return moved;
    };

    const shading::ShadingChange hard = shading::Sharpen(surface, pick(2, 0, {}, {edge}, {}), Level::Edge, true);
    REQUIRE(hard.edges == 1);
    shading::FinishShading(surface, hard.corners);
    const u32 afterHard = outsideUnchanged(hard);
    CHECK(afterHard > 0);
    CHECK(afterHard <= 8); // the two ends' fans, four corners each

    const shading::ShadingChange soft = shading::Sharpen(surface, pick(2, 0, {}, {edge}, {}), Level::Edge, false);
    REQUIRE(soft.edges == 1);
    shading::FinishShading(surface, soft.corners);
    CHECK(outsideUnchanged(soft) <= 8);
}

TEST_CASE("shading: Hard on another edge leaves a custom normal, on its own fan clears it",
          "[wem][geometry][shading]") {
    Mesh mesh = lumpy();
    Mesh* one[] = {&mesh};
    Surface surface(one);
    shading::Adopt(surface);
    const Topology& topology = std::as_const(mesh).topology();
    const u32 edge = innerEdge(mesh);
    const HalfedgeId corner = Topology::halfedge(geom::EdgeId(edge), 0);
    const Corner corners[] = {Corner{0, corner.value()}};
    const Vector3f automatic = normalsOf(mesh)[corner.index()];
    const Vector3f inward{-automatic.x, -automatic.y, -automatic.z};
    const Vector3f set[] = {inward};
    shading::SetNormals(surface, corners, set);

    // An edge with neither end at the corner's vertex.
    u32 far = geom::kInvalidId;
    for (u32 e = 0; e < topology.edgeCount() && far == geom::kInvalidId; ++e) {
        const HalfedgeId h = Topology::halfedge(geom::EdgeId(e), 0);
        if (!topology.isBoundary(geom::EdgeId(e)) && topology.from(h) != topology.from(corner) &&
            topology.to(h) != topology.from(corner)) {
            far = e;
        }
    }
    REQUIRE(far != geom::kInvalidId);
    shading::FinishShading(surface, shading::Sharpen(surface, pick(1, 0, {}, {far}, {}), Level::Edge, true).corners);
    CHECK(customAt(mesh, corner.value()));
    CHECK(apart(normalsOf(mesh)[corner.index()], inward) < 1e-6f);

    shading::FinishShading(surface, shading::Sharpen(surface, pick(1, 0, {}, {edge}, {}), Level::Edge, true).corners);
    CHECK_FALSE(customAt(mesh, corner.value()));
    CHECK(apart(normalsOf(mesh)[corner.index()], automatic) < 1e-6f);
}

TEST_CASE("shading: two cones tip to tip are two fans at one point", "[wem][geometry][shading]") {
    // One vertex given to both cones; the builder parts it, and they stay
    // apart: no border of one lies on a border of the other.
    Mesh mesh = trianglesOf({{0, 0, 0},
                             {1, 0, 1}, {0, 1, 1}, {-1, 0, 1}, {0, -1, 1},
                             {1, 0, -1}, {0, 1, -1}, {-1, 0, -1}, {0, -1, -1}},
                            {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1}, {0, 6, 5}, {0, 7, 6}, {0, 8, 7}, {0, 5, 8}});
    mesh.attributes.getOrCreate<u8>(geom::names::kModelled, Domain::Mesh, geom::AttrType::Bool)[0] = 1;
    Mesh* one[] = {&mesh};
    Surface surface(one);
    shading::Adopt(surface);
    const std::vector<Corner> tip = surface.cornersAtPointOf(0, 0);
    REQUIRE(tip.size() == 8);
    std::vector<u32> fans;
    for (const Corner& corner : tip) {
        fans.push_back(surface.fanOf(corner));
    }
    std::sort(fans.begin(), fans.end());
    fans.erase(std::unique(fans.begin(), fans.end()), fans.end());
    CHECK(fans.size() == 2);
    CHECK(surface.fan(fans[0]).size() == 4);
}

// ============================================================================
// The commands' table (§7), groups, and what a selection names (§6.4)
// ============================================================================

TEST_CASE("shading: Hard, Soft and Group at each level", "[wem][geometry][shading]") {
    Mesh box = geom::MakeBox(geom::PrimitiveParams{});
    Mesh* one[] = {&box};
    Surface surface(one);
    shading::Adopt(surface);
    const Topology& topology = std::as_const(box).topology();
    CHECK(shading::GroupsOf(surface).groups == 0); // six polygons, each alone
    CHECK(shading::GroupsOf(surface).hardEdges == 12);

    // Face 0 and the face across its first edge.
    const HalfedgeId shared = topology.halfedge(FaceId(0));
    const u32 neighbour = static_cast<u32>(topology.face(Topology::opposite(shared)).index());
    const std::vector<ElementSet> two = pick(1, 0, {}, {}, {0, neighbour});

    // Group: soft between them; their border is hard already.
    const shading::ShadingChange grouped = shading::Group(surface, two, Level::Polygon);
    CHECK(grouped.edges == 1);
    CHECK(hardEdges(box) == 11);
    const shading::SmoothGroups groups = shading::GroupsOf(surface);
    CHECK(groups.groups == 1);
    CHECK(groups.groupOf[0][0] == 1);
    CHECK(groups.groupOf[0][neighbour] == 1);
    CHECK(shading::GroupFaces(surface, pick(1, 0, {}, {}, {0}))[0].faces ==
          std::vector<u32>{std::min(0u, neighbour), std::max(0u, neighbour)});
    // Polygons, soft: only the edges between two of them, and that one is.
    CHECK(shading::Sharpen(surface, two, Level::Polygon, false).empty());
    // Polygons, hard: every edge of them.
    CHECK(shading::Sharpen(surface, pick(1, 0, {}, {}, {0}), Level::Polygon, true).edges == 1);
    CHECK(hardEdges(box) == 12);
    // Vertices: every edge at them.
    const u32 vertex = static_cast<u32>(topology.from(shared).index());
    CHECK(shading::Sharpen(surface, pick(1, 0, {vertex}, {}, {}), Level::Vertex, false).edges == 3);
    CHECK(shading::Sharpen(surface, pick(1, 0, {vertex}, {}, {}), Level::Vertex, true).edges == 3);
    // The mesh: every edge.
    CHECK(shading::Sharpen(surface, pick(1, 0, {}, {}, {}), Level::Mesh, false).edges == 12);
    CHECK(shading::GroupsOf(surface).groups == 1);
    CHECK(shading::GroupsOf(surface).hardEdges == 0);
    // Group at the Mesh level: soft inside, and it has no border to harden.
    CHECK(shading::Group(surface, pick(1, 0, {}, {}, {}), Level::Mesh).empty());
}

TEST_CASE("shading: Auto smooth sets every edge by its angle", "[wem][geometry][shading]") {
    geom::PrimitiveParams params;
    params.sides = 12;
    Mesh can = geom::MakeCylinder(params);
    Mesh* one[] = {&can};
    Surface surface(one);
    shading::Adopt(surface);
    const std::vector<ElementSet> none(1);
    shading::AutoSmoothOptions options;
    CHECK(shading::AutoSmooth(surface, none, Level::Mesh, options).empty()); // as it was made
    CHECK(shading::Sharpen(surface, none, Level::Mesh, false).edges == 24);
    CHECK(shading::AutoSmooth(surface, none, Level::Edge, options).edges == 24); // nothing picked: everything

    // 20 degrees: the side's edges too, which meet at 30.
    options.angle = 0.34906585f;
    CHECK(shading::AutoSmooth(surface, none, Level::Mesh, options).edges == 12);
    // Back at 80 they soften, unless hard edges are kept.
    options.angle = 1.396263402f;
    options.keepHard = true;
    CHECK(shading::AutoSmooth(surface, none, Level::Mesh, options).empty());
    options.keepHard = false;
    CHECK(shading::AutoSmooth(surface, none, Level::Mesh, options).edges == 12);

    // A marked seam goes hard when asked, whatever its angle.
    const Topology& topology = std::as_const(can).topology();
    u32 side = geom::kInvalidId;
    for (u32 e = 0; e < topology.edgeCount() && side == geom::kInvalidId; ++e) {
        if (!surface.hard(MeshEdge{0, e})) {
            side = e;
        }
    }
    REQUIRE(side != geom::kInvalidId);
    can.attributes.getOrCreate<u8>(geom::names::kSeam, Domain::Edge, geom::AttrType::Bool)[side] = 1;
    options.hardAtUvSeams = true;
    CHECK(shading::AutoSmooth(surface, none, Level::Mesh, options).edges == 1);
    CHECK(surface.hard(MeshEdge{0, side}));
    // Only the selection's edges when there is one.
    options.hardAtUvSeams = false;
    CHECK(shading::AutoSmooth(surface, pick(1, 0, {}, {side}, {}), Level::Edge, options).edges == 1);
}

TEST_CASE("shading: the groups of a cylinder, and what a selection names", "[wem][geometry][shading]") {
    geom::PrimitiveParams params;
    params.sides = 12;
    Mesh can = geom::MakeCylinder(params);
    Mesh* one[] = {&can};
    Surface surface(one);
    shading::Adopt(surface);
    const shading::SmoothGroups groups = shading::GroupsOf(surface);
    CHECK(groups.groups == 1); // its side; each cap is a polygon alone
    CHECK(groups.hardEdges == 24);
    u32 sideFace = geom::kInvalidId;
    u32 inGroup = 0;
    for (u32 f = 0; f < groups.groupOf[0].size(); ++f) {
        inGroup += groups.groupOf[0][f] != 0 ? 1 : 0;
        if (groups.groupOf[0][f] != 0) {
            sideFace = f;
        }
    }
    CHECK(inGroup == 12);
    const std::vector<ElementSet> group = shading::GroupFaces(surface, pick(1, 0, {}, {}, {sideFace}));
    CHECK(group[0].faces.size() == 12);

    // §6.4: a polygon names its own corners; an edge both sides at both ends;
    // a vertex every corner at its point (the side's two and the cap's one).
    const Topology& topology = std::as_const(can).topology();
    CHECK(shading::CornersOf(surface, pick(1, 0, {}, {}, {sideFace}), Level::Polygon).size() == 4);
    const HalfedgeId h = topology.halfedge(FaceId(sideFace));
    CHECK(shading::CornersOf(surface, pick(1, 0, {}, {Topology::edge(h).value()}, {}), Level::Edge).size() == 4);
    CHECK(shading::CornersOf(surface, pick(1, 0, {static_cast<u32>(topology.from(h).index())}, {}, {}), Level::Vertex)
              .size() == 3);
    CHECK(shading::CornersOf(surface, pick(1, 0, {}, {}, {}), Level::Mesh).size() == 12 * 4 + 2 * 12);
}

TEST_CASE("shading: Favor gives a fan the picked faces' normal", "[wem][geometry][shading]") {
    // A roof: two triangles over a soft ridge.
    Mesh mesh = trianglesOf({{0, 0, 1}, {1, 0, 1}, {0.5f, 1, 0}, {0.5f, -1, 0}}, {{0, 1, 2}, {1, 0, 3}});
    mesh.attributes.getOrCreate<u8>(geom::names::kModelled, Domain::Mesh, geom::AttrType::Bool)[0] = 1;
    geom::RecomputeNormals(mesh, geom::ShadingAngle(mesh));
    Mesh* one[] = {&mesh};
    Surface surface(one);
    shading::Adopt(surface);
    const Vector3f face0 = surface.faceNormals(0)[0];
    CHECK(shading::FavorFaces(surface, pick(1, 0, {}, {}, {0})) == 5); // the ridge's two fans, and face 0's far corner
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 f = 0; f < 2; ++f) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const u32 v = static_cast<u32>(topology.from(h).index());
            if (v == 0 || v == 1 || f == 0) {
                CHECK(apart(normalsOf(mesh)[h.index()], face0) < 1e-6f);
                CHECK(customAt(mesh, h.value()));
            } else {
                CHECK_FALSE(customAt(mesh, h.value())); // face 1's own far corner
            }
        }
    }
}

// ============================================================================
// Check
// ============================================================================

TEST_CASE("shading: Check finds each kind of fault and nothing on a clean mesh", "[wem][geometry][shading]") {
    Mesh mesh = lumpy();
    Mesh* one[] = {&mesh};
    Surface surface(one);
    shading::Adopt(surface);
    shading::ReshadeAll(surface);
    {
        const shading::CheckReport clean = shading::CheckNormals(surface);
        CHECK(clean.broken.count == 0);
        CHECK(clean.seams.count == 0);
        CHECK(clean.insideOut.count == 0);
        CHECK(clean.tangentsBroken.count == 0);
        CHECK(clean.tangentsMirrored.count == 0);
        CHECK(clean.faintHard.count == 0);
        CHECK(clean.backwards.count == 0);
        CHECK(clean.custom.count == 0);
    }
    const Topology& topology = std::as_const(mesh).topology();
    const u32 edge = innerEdge(mesh);
    const HalfedgeId corner = Topology::halfedge(geom::EdgeId(edge), 0);
    const auto normals = mesh.attributes.get<Vector3f>(geom::names::kNormal, Domain::Halfedge);
    const auto tangents = mesh.attributes.get<Vector4f>(geom::names::kTangent, Domain::Halfedge);
    const Vector3f was = normals[corner.index()];
    const Vector4f wasTangent = tangents[corner.index()];

    SECTION("a normal with no length is broken") {
        normals[corner.index()] = Vector3f{0, 0, 0};
        const shading::CheckReport report = shading::CheckNormals(surface);
        CHECK(report.broken.count == 1);
        CHECK(report.broken.found[0].faces == std::vector<u32>{static_cast<u32>(topology.face(corner).index())});
    }
    SECTION("one side of a soft edge turned is a seam on the two edges at that corner") {
        normals[corner.index()] = unit(Vector3f{was.x + 0.3f, was.y, was.z});
        const shading::CheckReport report = shading::CheckNormals(surface);
        CHECK(report.seams.count == 2);
        CHECK(std::find(report.seams.found[0].edges.begin(), report.seams.found[0].edges.end(), edge) !=
              report.seams.found[0].edges.end());
        // Marked custom, it is allowed to differ.
        const Corner corners[] = {Corner{0, corner.value()}};
        shading::KeepNormals(surface, corners);
        const shading::CheckReport kept = shading::CheckNormals(surface);
        CHECK(kept.seams.count == 0);
        CHECK(kept.custom.count == 1);
    }
    SECTION("a hard edge whose sides agree is faint") {
        mesh.attributes.getOrCreate<u8>(geom::names::kSharp, Domain::Edge, geom::AttrType::Bool)[edge] = 1;
        const shading::CheckReport report = shading::CheckNormals(surface);
        CHECK(report.faintHard.count == 1);
        CHECK(report.faintHard.found[0].edges == std::vector<u32>{edge});
        CHECK(report.seams.count == 0);
    }
    SECTION("a normal through its own face faces backwards") {
        normals[corner.index()] = Vector3f{-was.x, -was.y, -was.z};
        CHECK(shading::CheckNormals(surface).backwards.count == 1);
    }
    SECTION("a tangent with no length, or along its normal, is broken") {
        tangents[corner.index()] = Vector4f{0, 0, 0, 1};
        CHECK(shading::CheckNormals(surface).tangentsBroken.count == 1);
        tangents[corner.index()] = Vector4f{was.x, was.y, was.z, 1};
        CHECK(shading::CheckNormals(surface).tangentsBroken.count == 1);
    }
    SECTION("a tangent on the wrong side is mirrored wrong") {
        tangents[corner.index()].w = -wasTangent.w;
        CHECK(shading::CheckNormals(surface).tangentsMirrored.count == 1);
    }
}

TEST_CASE("shading: a closed part wound inward is inside out, an open one is not", "[wem][geometry][shading]") {
    const std::vector<Vector3f> points{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Mesh outward = trianglesOf(points, {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}});
    Mesh inward = trianglesOf(points, {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}});
    Mesh sheet = trianglesOf(points, {{0, 1, 2}});
    for (Mesh* mesh : {&outward, &inward, &sheet}) {
        geom::RecomputeNormals(*mesh, 0.5f);
        Mesh* one[] = {mesh};
        const Surface surface(one);
        const u32 count = shading::CheckNormals(surface).insideOut.count;
        CHECK(count == (mesh == &inward ? 4u : 0u));
    }
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G24's blend arm and the face-set re-shade (EDIT_MODE_MODELLING_DESIGN.md
/// §2.4, §2.7.7, §2.7.10; _PLAN.md ML0).
///
/// Every tool that makes a vertex or a corner samples the face it came from
/// through one module, and `SplitEdge` is the first: its corners and its skin
/// blend one ordered pair, so a split of a continuous edge stays continuous.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/geometry/interpolate.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/ops.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

using geom::Domain;
using geom::EdgeId;
using geom::FaceId;
using geom::HalfedgeId;
using geom::Influence;
using geom::Topology;
using geom::VertexId;

f32 sumOf(const std::vector<Influence>& influences) {
    f32 sum = 0.0f;
    for (const Influence& i : influences) {
        sum += i.weight;
    }
    return sum;
}

f32 weightOn(const std::vector<Influence>& influences, u32 bone) {
    for (const Influence& i : influences) {
        if (i.bone == bone) {
            return i.weight;
        }
    }
    return 0.0f;
}

/// Two triangles over a unit square, (0,1,2) and (0,2,3), the diagonal 0-2
/// shared; every corner at a vertex holds that vertex's UV, so the surface is
/// continuous across the diagonal. Vertex v binds to bone v.
Mesh square() {
    geom::FaceSet faces;
    faces.vertexCount = 4;
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(0, 2, 3);
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    positions[0] = Vector3f{0, 0, 0};
    positions[1] = Vector3f{1, 0, 0};
    positions[2] = Vector3f{1, 1, 0};
    positions[3] = Vector3f{0, 1, 0};
    const std::span<u32> groups = mesh.attributes.getOrCreate<u32>(
        geom::names::kMergeGroup, Domain::Vertex, geom::AttrType::U32);
    for (u32 v = 0; v < 4; ++v) {
        groups[v] = v;
    }
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    mesh.skin.reset(0);
    for (u32 v = 0; v < 4; ++v) {
        const Influence one{v, 1.0f};
        mesh.skin.appendVertex(std::span<const Influence>(&one, 1));
    }
    REQUIRE(mesh.ensureConnectivity().ok());
    const Mesh& readable = mesh;
    const Topology& topology = readable.topology();
    const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
        geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const std::span<Vector3f> normals = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kNormal, Domain::Halfedge, geom::AttrType::F32x3);
    for (u32 h = 0; h < topology.halfedgeCount(); ++h) {
        if (topology.isBoundary(HalfedgeId(h))) {
            continue;
        }
        const Vector3f& p = positions[topology.from(HalfedgeId(h)).index()];
        uvs[h] = Vector2f{p.x * 0.3f + 0.1f, p.y * 0.7f + 0.2f};
        // Unit normals that lean different ways, so a lerp of two is shorter
        // than one and has to be renormalised.
        const f32 lean = p.x - p.y;
        normals[h] = Vector3f{0.6f * lean, 0.0f, std::sqrt(1.0f - 0.36f * lean * lean)};
    }
    return mesh;
}

EdgeId diagonalOf(const Mesh& mesh) {
    const HalfedgeId h = mesh.topology().findHalfedge(VertexId(0), VertexId(2));
    REQUIRE(h.valid());
    return Topology::edge(h);
}

} // namespace

// ============================================================================
// BlendInfluences (G24)
// ============================================================================

TEST_CASE("wem interpolate BlendInfluences clamps negative weights and renormalises",
          "[wem][geometry][interpolate]") {
    const std::vector<std::vector<Influence>> sources{{{1, 1.0f}}, {{2, 1.0f}}, {{3, 1.0f}}};
    // Mean-value weights outside a polygon: one goes negative.
    const std::array<f32, 3> weights{-0.25f, 0.75f, 0.5f};
    const std::vector<Influence> out = geom::BlendInfluences(sources, weights);
    CHECK(weightOn(out, 1) == 0.0f);
    CHECK(weightOn(out, 2) == Catch::Approx(0.6f));
    CHECK(weightOn(out, 3) == Catch::Approx(0.4f));
    CHECK(sumOf(out) == Catch::Approx(1.0f));
}

TEST_CASE("wem interpolate BlendInfluences sums bones, prunes and sorts heaviest first",
          "[wem][geometry][interpolate]") {
    const std::vector<std::vector<Influence>> sources{{{4, 0.99995f}, {7, 0.00005f}},
                                                      {{4, 0.5f}, {9, 0.5f}}};
    const std::array<f32, 2> weights{0.5f, 0.5f};
    const std::vector<Influence> out = geom::BlendInfluences(sources, weights);
    REQUIRE(out.size() == 2);
    CHECK(out[0].bone == 4);
    CHECK(out[1].bone == 9);
    CHECK(weightOn(out, 7) == 0.0f); // 0.000025 of the whole, under the prune
    CHECK(sumOf(out) == Catch::Approx(1.0f));
}

TEST_CASE("wem interpolate BlendInfluences caps at eight and renormalises after the cap",
          "[wem][geometry][interpolate]") {
    // Ten bones at 0.1: eight survive the cap, and they must still sum to 1.
    std::vector<Influence> ten;
    for (u32 b = 0; b < 10; ++b) {
        ten.push_back(Influence{b, 0.1f});
    }
    const std::vector<std::vector<Influence>> sources{ten};
    const std::array<f32, 1> weights{1.0f};
    const std::vector<Influence> out = geom::BlendInfluences(sources, weights);
    REQUIRE(out.size() == geom::kMaxBlendedInfluences);
    CHECK(sumOf(out) == Catch::Approx(1.0f));
    for (const Influence& i : out) {
        CHECK(i.weight == Catch::Approx(0.125f));
    }
}

TEST_CASE("wem interpolate BlendInfluences with no positive weight takes the nearest corner",
          "[wem][geometry][interpolate]") {
    const std::vector<std::vector<Influence>> sources{{{1, 1.0f}}, {{2, 1.0f}}};
    const std::array<f32, 2> weights{-0.1f, -0.4f};
    const std::vector<Influence> out = geom::BlendInfluences(sources, weights);
    REQUIRE(out.size() == 1);
    CHECK(out[0].bone == 1);
    CHECK(out[0].weight == 1.0f);
}

// ============================================================================
// BlendCorners
// ============================================================================

TEST_CASE("wem interpolate BlendCorners renormalises normals and keeps the heaviest tangent sign",
          "[wem][geometry][interpolate]") {
    Mesh mesh = square();
    const std::span<Vector4f> tangents = mesh.attributes.getOrCreate<Vector4f>(
        geom::names::kTangent, Domain::Halfedge, geom::AttrType::F32x4);
    const std::span<u32> ids = mesh.attributes.getOrCreate<u32>("authoringId", Domain::Halfedge,
                                                                geom::AttrType::U32);
    const Topology& topology = std::as_const(mesh).topology();
    const HalfedgeId a = topology.halfedge(FaceId(0));
    const HalfedgeId b = topology.next(a);
    tangents[a.index()] = Vector4f{1, 0, 0, -1};
    tangents[b.index()] = Vector4f{0, 1, 0, 1};
    ids[a.index()] = 11;
    ids[b.index()] = 22;
    const std::array<HalfedgeId, 2> corners{a, b};
    const geom::SourcePolygon source = geom::CaptureCorners(mesh, corners);
    const std::array<f32, 2> weights{0.3f, 0.7f};
    const HalfedgeId target = topology.next(b);
    const std::array<HalfedgeId, 1> targets{target};
    geom::BlendCorners(mesh, source, weights, targets);

    const Vector3f n = mesh.attributes.get<Vector3f>(geom::names::kNormal, Domain::Halfedge)[target.index()];
    CHECK(std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z) == Catch::Approx(1.0f));
    const Vector4f t = tangents[target.index()];
    CHECK(std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z) == Catch::Approx(1.0f));
    CHECK(t.w == 1.0f);        // the heaviest corner's sign
    CHECK(ids[target.index()] == 22u); // integers are the heaviest corner's
}

// ============================================================================
// SplitEdge through the blends (§2.7.7)
// ============================================================================

TEST_CASE("wem interpolate SplitEdge blends skin the way it places the vertex",
          "[wem][geometry][interpolate]") {
    Mesh mesh = square();
    const EdgeId diagonal = diagonalOf(mesh);
    const VertexId a = std::as_const(mesh).topology().from(Topology::halfedge(diagonal, 0));
    const VertexId b = std::as_const(mesh).topology().to(Topology::halfedge(diagonal, 0));
    const VertexId v = geom::SplitEdge(mesh, diagonal, 1.0f / 3.0f);
    REQUIRE(v.valid());
    const std::span<const Influence> skin = std::as_const(mesh).skin.forVertex(v.value());
    const std::vector<Influence> influences(skin.begin(), skin.end());
    // A third of the way along: a third of the far end's weight.
    CHECK(weightOn(influences, a.value()) == Catch::Approx(2.0f / 3.0f));
    CHECK(weightOn(influences, b.value()) == Catch::Approx(1.0f / 3.0f));
    CHECK(sumOf(influences) == Catch::Approx(1.0f));
}

TEST_CASE("wem interpolate SplitEdge gives the new vertex a merge group of its own",
          "[wem][geometry][interpolate]") {
    Mesh mesh = square();
    const VertexId v = geom::SplitEdge(mesh, diagonalOf(mesh), 0.5f);
    REQUIRE(v.valid());
    const std::span<const u32> groups =
        std::as_const(mesh).attributes.get<const u32>(geom::names::kMergeGroup, Domain::Vertex);
    for (u32 other = 0; other < groups.size(); ++other) {
        if (other != v.value()) {
            CHECK(groups[other] != groups[v.value()]);
        }
    }
}

TEST_CASE("wem interpolate SplitEdge of a continuous edge splits to bit-equal corners",
          "[wem][geometry][interpolate]") {
    // Both loops blend the same ordered pair with the same weights: the two new
    // corners at v hold the same bytes, where A + (B - A)t on one side and
    // B + (A - B)(1 - t) on the other did not.
    for (const f32 t : {0.1f, 0.37f, 0.5f, 0.81f}) {
        CAPTURE(t);
        Mesh mesh = square();
        const VertexId v = geom::SplitEdge(mesh, diagonalOf(mesh), t);
        REQUIRE(v.valid());
        const Topology& topology = std::as_const(mesh).topology();
        std::vector<HalfedgeId> corners;
        for (const HalfedgeId h : topology.voh(v)) {
            if (!topology.isBoundary(h)) {
                corners.push_back(h);
            }
        }
        REQUIRE(corners.size() == 2);
        for (const char* name : {"uv0", "normal"}) {
            CAPTURE(name);
            const geom::AttrLayer* layer = mesh.attributes.layer(name, Domain::Halfedge);
            REQUIRE(layer != nullptr);
            const std::size_t stride = geom::AttrTypeSize(layer->type);
            CHECK(std::memcmp(layer->data.data() + stride * corners[0].index(),
                              layer->data.data() + stride * corners[1].index(), stride) == 0);
        }
        const Vector3f n =
            mesh.attributes.get<Vector3f>(geom::names::kNormal, Domain::Halfedge)[corners[0].index()];
        CHECK(std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z) == Catch::Approx(1.0f));
    }
}

// ============================================================================
// The face-set re-shade (§2.7.10)
// ============================================================================

namespace {

/// Quads of a strip along x, each its own face, bent up about its shared edges
/// by @p bend (radians): quad i spans x = i..i+1. @p uvOf gives each corner's UV
/// from its face and position.
template <class UvOf>
Mesh strip(u32 quads, f32 bend, bool modelled, UvOf uvOf) {
    geom::FaceSet faces;
    faces.vertexCount = 2 * (quads + 1);
    for (u32 q = 0; q < quads; ++q) {
        const std::array<u32, 4> quad{2 * q, 2 * q + 2, 2 * q + 3, 2 * q + 1};
        faces.addFace(quad);
    }
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> positions = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    Vector3f at{0, 0, 0};
    f32 heading = 0.0f;
    for (u32 i = 0; i <= quads; ++i) {
        positions[2 * i] = Vector3f{at.x, 0.0f, at.z};
        positions[2 * i + 1] = Vector3f{at.x, 1.0f, at.z};
        at.x += std::cos(heading);
        at.z += std::sin(heading);
        heading += bend;
    }
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    if (modelled) {
        mesh.attributes.getOrCreate<u8>(geom::names::kModelled, Domain::Mesh,
                                        geom::AttrType::Bool)[0] = 1;
    }
    REQUIRE(mesh.ensureConnectivity().ok());
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<Vector2f> uvs = mesh.attributes.getOrCreate<Vector2f>(
        geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            uvs[h.index()] = uvOf(f, topology.from(h).value());
        }
    }
    return mesh;
}

std::vector<FaceId> allFaces(const Mesh& mesh) {
    std::vector<FaceId> out;
    for (u32 f = 0; f < mesh.faceCount(); ++f) {
        out.push_back(FaceId(f));
    }
    return out;
}

/// The two corners at @p vertex, one in each face beside it.
std::array<HalfedgeId, 2> cornersAt(const Mesh& mesh, u32 vertex) {
    std::vector<HalfedgeId> out;
    for (const HalfedgeId h : mesh.topology().voh(VertexId(vertex))) {
        if (!mesh.topology().isBoundary(h)) {
            out.push_back(h);
        }
    }
    REQUIRE(out.size() == 2);
    return {out[0], out[1]};
}

bool near(const Vector3f& a, const Vector3f& b) {
    return std::abs(a.x - b.x) < 1e-5f && std::abs(a.y - b.y) < 1e-5f && std::abs(a.z - b.z) < 1e-5f;
}

} // namespace

TEST_CASE("wem interpolate a 70 degree crease on a modelled mesh shades as one",
          "[wem][geometry][reshade]") {
    // Continuous UVs, u along the strip: one normal and one tangent group at
    // the crease, since only `sharp` splits a modelled mesh.
    Mesh mesh = strip(2, 1.2217305f, true, [](u32, u32 v) {
        return Vector2f{static_cast<f32>(v / 2), static_cast<f32>(v % 2)};
    });
    CHECK(geom::ShadingAngle(mesh) == Catch::Approx(3.14159265f));
    const std::vector<FaceId> faces = allFaces(mesh);
    geom::RecomputeNormals(mesh, faces, geom::ShadingAngle(mesh));
    geom::RecomputeTangents(mesh, faces, 0);
    const std::array<HalfedgeId, 2> corners = cornersAt(mesh, 2);
    const std::span<const Vector3f> normals =
        std::as_const(mesh).attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    const std::span<const Vector4f> tangents =
        std::as_const(mesh).attributes.get<const Vector4f>(geom::names::kTangent, Domain::Halfedge);
    CHECK(near(normals[corners[0].index()], normals[corners[1].index()]));
    const Vector4f t0 = tangents[corners[0].index()];
    const Vector4f t1 = tangents[corners[1].index()];
    CHECK(near(Vector3f{t0.x, t0.y, t0.z}, Vector3f{t1.x, t1.y, t1.z}));
    CHECK(t0.w == t1.w);

    SECTION("unmodelled, the 60 degree default splits it") {
        mesh.attributes.get<u8>(geom::names::kModelled, Domain::Mesh)[0] = 0;
        geom::RecomputeNormals(mesh, faces, geom::ShadingAngle(mesh));
        CHECK_FALSE(near(normals[corners[0].index()], normals[corners[1].index()]));
    }
}

TEST_CASE("wem interpolate a mirror line keeps two tangents", "[wem][geometry][reshade]") {
    // Flat, and u runs backwards on the second quad: the same UV on the shared
    // edge, the opposite handedness either side of it.
    Mesh mesh = strip(2, 0.0f, true, [](u32 face, u32 v) {
        const f32 x = static_cast<f32>(v / 2);
        return Vector2f{face == 0 ? x : 2.0f - x, static_cast<f32>(v % 2)};
    });
    const std::vector<FaceId> faces = allFaces(mesh);
    geom::RecomputeNormals(mesh, faces, geom::ShadingAngle(mesh));
    geom::RecomputeTangents(mesh, faces, 0);
    const std::array<HalfedgeId, 2> corners = cornersAt(mesh, 2);
    const std::span<const Vector4f> tangents =
        std::as_const(mesh).attributes.get<const Vector4f>(geom::names::kTangent, Domain::Halfedge);
    const Vector4f t0 = tangents[corners[0].index()];
    const Vector4f t1 = tangents[corners[1].index()];
    CHECK_FALSE(near(Vector3f{t0.x, t0.y, t0.z}, Vector3f{t1.x, t1.y, t1.z}));
}

TEST_CASE("wem interpolate a face with no UV area leaves its neighbours' tangents alone",
          "[wem][geometry][reshade]") {
    // A modelled strip bent 90 degrees at each edge: quad 1 is an Extrude wall
    // whose corners copy the rim's UVs (U6), so its UV map has no area. Its
    // neighbours run u along y, a tangent of +y the wall must not pull.
    const auto uvOf = [](u32 face, u32 v) {
        const u32 column = v / 2;
        if (face == 1) {
            // The wall: both columns hold the rim's UVs at the edge it rose
            // from, so at that edge its corners are in the rim's UV class.
            return Vector2f{static_cast<f32>(v % 2), 1.0f};
        }
        return Vector2f{static_cast<f32>(v % 2), static_cast<f32>(column)};
    };
    Mesh mesh = strip(3, 1.5707963f, true, uvOf);
    const std::vector<FaceId> faces = allFaces(mesh);
    geom::RecomputeNormals(mesh, faces, geom::ShadingAngle(mesh));
    geom::RecomputeTangents(mesh, faces, 0);
    const Topology& topology = std::as_const(mesh).topology();
    const std::span<const Vector4f> tangents =
        std::as_const(mesh).attributes.get<const Vector4f>(geom::names::kTangent, Domain::Halfedge);
    for (const HalfedgeId h : topology.fh(FaceId(0))) {
        const Vector4f t = tangents[h.index()];
        CHECK(std::abs(t.y) == Catch::Approx(1.0f).margin(1e-4f));
    }
}

// ============================================================================
// G24: sampling a face (EDIT_MODE_MODELLING_DESIGN.md §2.4)
// ============================================================================

namespace {

/// A planar quad that is no parallelogram, so no affine map fits its corners.
const std::vector<Vector3f> kQuad{{0, 0, 0}, {2, 0, 0}, {2.5f, 1.5f, 0}, {0, 1, 0}};
const std::vector<u32> kQuadCut{0, 1, 2, 0, 2, 3};

std::vector<f32> weightsAt(const Vector3f& point, bool* inside = nullptr) {
    std::vector<f32> weights(kQuad.size());
    const bool in = geom::SampleWeights(kQuad, kQuadCut, point, weights);
    if (inside != nullptr) {
        *inside = in;
    }
    return weights;
}

f32 total(const std::vector<f32>& weights) {
    f32 sum = 0.0f;
    for (const f32 w : weights) {
        sum += w;
    }
    return sum;
}

} // namespace

TEST_CASE("wem interpolate SampleWeights is a partition of unity", "[wem][geometry][interpolate]") {
    for (const Vector3f& point : {Vector3f{1, 0.5f, 0}, Vector3f{2.2f, 1, 0}, Vector3f{-1, -1, 0},
                                  Vector3f{4, 0.2f, 0}, Vector3f{1, 5, 0}, Vector3f{0.5f, 0, 0}}) {
        CAPTURE(point.x, point.y);
        CHECK(total(weightsAt(point)) == Catch::Approx(1.0f).margin(1e-5));
    }
}

TEST_CASE("wem interpolate SampleWeights is exact at corners and on edges", "[wem][geometry][interpolate]") {
    bool inside = false;
    CHECK(weightsAt(kQuad[2], &inside) == std::vector<f32>{0, 0, 1, 0});
    CHECK(inside);
    const std::vector<f32> middle = weightsAt(Vector3f{1, 0, 0}, &inside);
    CHECK(inside);
    CHECK(middle[0] == Catch::Approx(0.5f));
    CHECK(middle[1] == Catch::Approx(0.5f));
    CHECK(middle[2] == 0.0f);
    CHECK(middle[3] == 0.0f);
}

TEST_CASE("wem interpolate SampleWeights inside is barycentric in the drawn triangle",
          "[wem][geometry][interpolate]") {
    // A value at each corner no affine map fits: inside, the sample is the
    // drawn triangle's own interpolation, so the picture's field comes back.
    const std::array<f32, 4> value{0.0f, 1.0f, 5.0f, 2.0f};
    const Vector3f point{1.5f, 0.5f, 0}; // in triangle (0, 1, 2)
    bool inside = false;
    const std::vector<f32> w = weightsAt(point, &inside);
    CHECK(inside);
    CHECK(w[3] == 0.0f);
    // Barycentric in (0, 1, 2) by areas.
    const auto area = [](const Vector3f& a, const Vector3f& b, const Vector3f& c) {
        return 0.5f * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    };
    const f32 whole = area(kQuad[0], kQuad[1], kQuad[2]);
    const f32 w0 = area(point, kQuad[1], kQuad[2]) / whole;
    const f32 w1 = area(kQuad[0], point, kQuad[2]) / whole;
    const f32 w2 = area(kQuad[0], kQuad[1], point) / whole;
    CHECK(w[0] == Catch::Approx(w0).margin(1e-6));
    CHECK(w[1] == Catch::Approx(w1).margin(1e-6));
    CHECK(w[2] == Catch::Approx(w2).margin(1e-6));
    f32 sampled = 0.0f;
    for (u32 i = 0; i < 4; ++i) {
        sampled += w[i] * value[i];
    }
    CHECK(sampled == Catch::Approx(w0 * value[0] + w1 * value[1] + w2 * value[2]).margin(1e-5));
}

TEST_CASE("wem interpolate SampleWeights outside reproduces an affine field", "[wem][geometry][interpolate]") {
    // uv = (0.3 x + 0.1 y + 2, -0.2 x + 0.7 y), exactly, at extrapolated points,
    // one of them on an edge's line beyond the edge.
    const auto field = [](const Vector3f& p) {
        return std::array<f32, 2>{0.3f * p.x + 0.1f * p.y + 2.0f, -0.2f * p.x + 0.7f * p.y};
    };
    for (const Vector3f& point : {Vector3f{-1, -1, 0}, Vector3f{4, 0.2f, 0}, Vector3f{3, 0, 0},
                                  Vector3f{-2, 0, 0}, Vector3f{1, 3, 0}}) {
        CAPTURE(point.x, point.y);
        bool inside = true;
        const std::vector<f32> w = weightsAt(point, &inside);
        CHECK_FALSE(inside);
        std::array<f32, 2> sampled{0.0f, 0.0f};
        for (u32 i = 0; i < 4; ++i) {
            const std::array<f32, 2> corner = field(kQuad[i]);
            sampled[0] += w[i] * corner[0];
            sampled[1] += w[i] * corner[1];
        }
        const std::array<f32, 2> expected = field(point);
        CHECK(sampled[0] == Catch::Approx(expected[0]).margin(1e-4));
        CHECK(sampled[1] == Catch::Approx(expected[1]).margin(1e-4));
    }
}

TEST_CASE("wem interpolate one value per class at a moved vertex", "[wem][geometry][interpolate]") {
    // Two triangles on edge v-w, UVs continuous along it but mapped at
    // different scales: A = (v, a, w) with u = x, B = (v, w, b) with u = 2x.
    geom::FaceSet faces;
    faces.vertexCount = 4;
    faces.addFace(std::vector<u32>{0, 2, 1}); // v, a, w
    faces.addFace(std::vector<u32>{0, 1, 3}); // v, w, b
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> p = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    p[0] = {0, 0, 0};
    p[1] = {0, 1, 0};
    p[2] = {-1, 0.5f, 0};
    p[3] = {1, 0.5f, 0};
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    REQUIRE(mesh.ensureConnectivity().ok());
    const std::span<Vector2f> uv = mesh.attributes.getOrCreate<Vector2f>(
        geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const Topology& topology = std::as_const(mesh).topology();
    for (u32 f = 0; f < 2; ++f) {
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const Vector3f& at = p[topology.from(h).index()];
            uv[h.index()] = Vector2f{f == 0 ? at.x : 2.0f * at.x, at.y};
        }
    }
    // v moves into B. Sampled per corner, A would give u = 0.2 and B 0.4.
    const Vector3f to{0.2f, 0.1f, 0};
    geom::ModelPlan plan;
    plan.motions.push_back(geom::VertexMotion{0, p[0], Vector3f{to.x - p[0].x, to.y - p[0].y, 0}});
    plan.resamples = geom::ResamplesAt(mesh, VertexId(0), to);
    REQUIRE(plan.resamples.size() == 1u);
    geom::ApplyAmount(mesh, plan, 1.0f);
    std::vector<Vector2f> atV;
    for (const HalfedgeId h : topology.voh(VertexId(0))) {
        if (topology.face(h).valid()) {
            atV.push_back(uv[h.index()]);
        }
    }
    REQUIRE(atV.size() == 2u);
    CHECK(std::memcmp(&atV[0], &atV[1], sizeof(Vector2f)) == 0);
    CHECK(atV[0].x == Catch::Approx(0.4f).margin(1e-6));
    CHECK(atV[0].y == Catch::Approx(0.1f).margin(1e-6));
}

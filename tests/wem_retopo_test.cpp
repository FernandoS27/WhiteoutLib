// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// R6 (EDIT_MODE_RETOPOLOGY_DESIGN.md §2): the whole pipeline on WEM meshes,
/// and what it carries across -- UVs on the right side of every seam, the
/// skin, sections, kept pieces -- read from the result as a file would.

#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/retopo/retopology.h>

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace whiteout::models::wem::geom::retopo;

namespace {

Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x - b.x, a.y - b.y, a.z - b.z};
}
f32 Len(const Vector3f& a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

/// Every output face is one polygon of four corners.
u32 CountQuads(const Mesh& mesh) {
    const geom::FaceSet& faces = mesh.faceSet();
    u32 quads = 0;
    for (u32 valence : faces.faceValence) {
        quads += valence == 4 ? 1 : 0;
    }
    return quads;
}

/// A cylinder along z from 0 to `height`, open at both ends, skinned to two
/// bones: bone 0 below a quarter, bone 1 above three quarters, a linear blend
/// between -- a weight anyone can compute from z.
Mesh SkinnedTube(f32 radius, f32 height, u32 around, u32 rows) {
    geom::MeshBuilder builder;
    std::vector<geom::VertexId> v;
    auto blend = [&](f32 z) { return std::clamp((z / height - 0.25f) / 0.5f, 0.0f, 1.0f); };
    for (u32 j = 0; j <= rows; ++j) {
        const f32 z = height * j / rows;
        for (u32 i = 0; i < around; ++i) {
            const f32 a = 2.0f * 3.14159265f * i / around;
            const geom::VertexId id = builder.addVertex({radius * std::cos(a), radius * std::sin(a), z});
            v.push_back(id);
            const f32 w = blend(z);
            if (w < 1.0f) {
                builder.addInfluence(id, 0, 1.0f - w);
            }
            if (w > 0.0f) {
                builder.addInfluence(id, 1, w);
            }
        }
    }
    for (u32 j = 0; j < rows; ++j) {
        for (u32 i = 0; i < around; ++i) {
            const u32 i1 = (i + 1) % around;
            const geom::VertexId quad[4] = {v[j * around + i], v[j * around + i1], v[(j + 1) * around + i1],
                                            v[(j + 1) * around + i]};
            builder.addFace(quad);
        }
    }
    Mesh mesh = builder.build().mesh;
    mesh.ensureConnectivity();
    return mesh;
}

} // namespace

TEST_CASE("retopo: a box comes out as quads near the count asked for", "[retopo]") {
    // Both methods, which share everything round the quads.
    const RetopoMethod method = GENERATE(RetopoMethod::Field, RetopoMethod::Layout);
    INFO((method == RetopoMethod::Field ? "field" : "layout"));
    const Mesh box = geom::MakeBox({});
    RetopoOptions options;
    options.targetQuads = 150;
    options.method = method;
    const RetopoResult result = Retopologize(box, options);
    INFO(ToString(result.report.failure) << " quads " << result.report.quads << " other "
                                         << result.report.otherFaces << " patches " << result.report.patches);
    REQUIRE(result.report.ok());
    CHECK(result.report.otherFaces == 0);
    CHECK(CountQuads(result.mesh) == result.report.quads);
    CHECK(result.report.quads >= 96);
    CHECK(result.report.quads <= 216);
    CHECK(result.report.irregularVertices == 8);
    // The box's own corners are still there.
    const std::span<const Vector3f> positions =
        result.mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    u32 corners = 0;
    for (const Vector3f& p : positions) {
        corners += (std::abs(std::abs(p.x) - 1.0f) < 1e-5f && std::abs(std::abs(p.y) - 1.0f) < 1e-5f &&
                    std::abs(std::abs(p.z) - 1.0f) < 1e-5f)
                       ? 1
                       : 0;
    }
    CHECK(corners == 8);
}

TEST_CASE("retopo: an adaptive size puts more of the quads where the surface curves", "[retopo]") {
    // One mesh, two pieces: a sphere, curved all over, and a box beside it,
    // flat but for its edges. Adaptive, the sphere takes a larger share.
    geom::MeshBuilder builder;
    u32 sphereFaces = 0;
    for (const bool sphere : {true, false}) {
        geom::PrimitiveParams params;
        params.sides = 24;
        params.segments = 12;
        const Mesh piece = sphere ? geom::MakeSphere(params) : geom::MakeBox({});
        const std::span<const Vector3f> positions =
            piece.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        std::vector<geom::VertexId> ids;
        for (const Vector3f& p : positions) {
            ids.push_back(builder.addVertex({p.x + (sphere ? 0.0f : 4.0f), p.y, p.z}));
        }
        const geom::FaceSet& faces = piece.faceSet();
        std::size_t at = 0;
        for (u32 valence : faces.faceValence) {
            std::vector<geom::VertexId> corners;
            for (u32 k = 0; k < valence; ++k) {
                corners.push_back(ids[faces.cornerVertex[at + k]]);
            }
            builder.addFace(corners);
            at += valence;
            sphereFaces += sphere ? 1 : 0;
        }
    }
    Mesh both = builder.build().mesh;
    both.ensureConnectivity();
    auto sphereShare = [&](f32 adaptivity) {
        RetopoOptions options;
        options.method = RetopoMethod::Layout; // the adaptive size is the layout's
        options.targetQuads = 400;
        options.adaptivity = adaptivity;
        const RetopoResult result = Retopologize(both, options);
        REQUIRE(result.report.ok());
        const std::span<const Vector3f> positions =
            result.mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        const geom::FaceSet& faces = result.mesh.faceSet();
        u32 onSphere = 0;
        std::size_t at = 0;
        for (u32 valence : faces.faceValence) {
            onSphere += positions[faces.cornerVertex[at]].x < 2.0f ? 1 : 0;
            at += valence;
        }
        return static_cast<f64>(onSphere) / static_cast<f64>(faces.faceValence.size());
    };
    const f64 even = sphereShare(0.0f);
    const f64 adaptive = sphereShare(1.0f);
    INFO("sphere share even " << even << " adaptive " << adaptive);
    CHECK(adaptive > even + 0.05);
}

TEST_CASE("retopo: no face straddles a UV island, so the texture holds", "[retopo]") {
    // Both methods, which share everything round the quads.
    const RetopoMethod method = GENERATE(RetopoMethod::Field, RetopoMethod::Layout);
    INFO((method == RetopoMethod::Field ? "field" : "layout"));
    // The box's standard unwrap cuts it into islands along its edges: a face
    // that read corners from two islands would stretch across the atlas.
    const Mesh box = geom::MakeBox({});
    RetopoOptions options;
    options.targetQuads = 200;
    options.method = method;
    const RetopoResult result = Retopologize(box, options);
    REQUIRE(result.report.ok());
    const Mesh& mesh = result.mesh;
    const geom::Topology& topology = mesh.topology();
    const std::span<const Vector2f> uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(!uv.empty());
    // Each face's UV edges are its 3D edges scaled by one factor: one island,
    // no shear across a seam.
    f32 worst = 0.0f;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        std::vector<f32> ratios;
        for (const geom::HalfedgeId h : topology.fh(geom::FaceId(f))) {
            const geom::HalfedgeId n = topology.next(h);
            const Vector2f a = uv[h.index()];
            const Vector2f b = uv[n.index()];
            const f32 inUv = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            const f32 in3d = Len(Sub(positions[topology.to(h).index()], positions[topology.from(h).index()]));
            ratios.push_back(inUv / std::max(in3d, 1e-6f));
        }
        const auto [low, high] = std::minmax_element(ratios.begin(), ratios.end());
        worst = std::max(worst, (*high - *low) / std::max(*high, 1e-6f));
    }
    CHECK(worst < 0.02f);
}

TEST_CASE("retopo: a seam that leaves the surface whole still parts the UVs", "[retopo]") {
    // Both methods, which share everything round the quads.
    const RetopoMethod method = GENERATE(RetopoMethod::Field, RetopoMethod::Layout);
    INFO((method == RetopoMethod::Field ? "field" : "layout"));
    // The cylinder's side is one strip wrapped round, its seam running rim to
    // rim: both sides of it are one region, and a face beside it must read its
    // own side's u, not the far end of the strip.
    const Mesh cylinder = geom::MakeCylinder({});
    RetopoOptions options;
    options.targetQuads = 300;
    options.method = method;
    const RetopoResult result = Retopologize(cylinder, options);
    REQUIRE(result.report.ok());
    const Mesh& mesh = result.mesh;
    const geom::Topology& topology = mesh.topology();
    const std::span<const Vector2f> uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    REQUIRE(!uv.empty());
    u32 side = 0;
    f32 widest = 0.0f;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        f32 height = 0.0f;
        f32 low = 1.0f;
        f32 high = 0.0f;
        u32 corners = 0;
        for (const geom::HalfedgeId h : topology.fh(geom::FaceId(f))) {
            height += positions[topology.from(h).index()].z;
            low = std::min(low, uv[h.index()].x);
            high = std::max(high, uv[h.index()].x);
            ++corners;
        }
        if (std::abs(height / static_cast<f32>(corners)) > 0.9f) {
            continue; // a cap's
        }
        ++side;
        widest = std::max(widest, high - low);
    }
    CHECK(side > 50);
    CHECK(widest < 0.25f);
}

TEST_CASE("retopo: the skin comes across as the source's blend at each vertex", "[retopo]") {
    // Both methods, which share everything round the quads.
    const RetopoMethod method = GENERATE(RetopoMethod::Field, RetopoMethod::Layout);
    INFO((method == RetopoMethod::Field ? "field" : "layout"));
    const Mesh tube = SkinnedTube(0.5f, 4.0f, 24, 16);
    RetopoOptions options;
    options.targetQuads = 300;
    options.method = method;
    const RetopoResult result = Retopologize(tube, options);
    REQUIRE(result.report.ok());
    const Mesh& mesh = result.mesh;
    REQUIRE(!mesh.skin.empty());
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    f32 worst = 0.0f;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        f32 sum = 0.0f;
        f32 upper = 0.0f;
        for (const geom::Influence& influence : mesh.skin.forVertex(v)) {
            sum += influence.weight;
            upper += influence.bone == 1 ? influence.weight : 0.0f;
        }
        CHECK(std::abs(sum - 1.0f) < 1e-4f);
        // Linear in z between rows of the source, so exact on its flat faces
        // up to the facet's chord.
        const f32 expected = std::clamp((positions[v].z / 4.0f - 0.25f) / 0.5f, 0.0f, 1.0f);
        worst = std::max(worst, std::abs(upper - expected));
    }
    CHECK(worst < 0.01f);
}

TEST_CASE("retopo: no face crosses a section border", "[retopo]") {
    // Both methods, which share everything round the quads.
    const RetopoMethod method = GENERATE(RetopoMethod::Field, RetopoMethod::Layout);
    INFO((method == RetopoMethod::Field ? "field" : "layout"));
    // A plane whose left half is section 0 and right half section 1.
    geom::MeshBuilder builder;
    builder.addSection({});
    builder.addSection({});
    std::vector<geom::VertexId> v;
    const u32 n = 9;
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            v.push_back(builder.addVertex({i * 0.25f, j * 0.25f, 0.0f}));
        }
    }
    for (u32 j = 0; j + 1 < n; ++j) {
        for (u32 i = 0; i + 1 < n; ++i) {
            const geom::VertexId quad[4] = {v[j * n + i], v[j * n + i + 1], v[(j + 1) * n + i + 1], v[(j + 1) * n + i]};
            builder.addFace(quad, i < 4 ? 0 : 1);
        }
    }
    Mesh plane = builder.build().mesh;
    plane.ensureConnectivity();
    RetopoOptions options;
    options.targetQuads = 100;
    options.method = method;
    const RetopoResult result = Retopologize(plane, options);
    REQUIRE(result.report.ok());
    const Mesh& mesh = result.mesh;
    const geom::Topology& topology = mesh.topology();
    const std::span<const u32> sections = mesh.faceSections();
    const std::span<const Vector3f> positions =
        mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    u32 wrong = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        f32 x = 0.0f;
        u32 count = 0;
        for (const geom::VertexId c : topology.fv(geom::FaceId(f))) {
            x += positions[c.index()].x;
            ++count;
        }
        x /= count;
        wrong += (x < 1.0f) != (sections[f] == 0) ? 1 : 0;
    }
    CHECK(wrong == 0);
    CHECK(mesh.sections.size() == 2);
}

TEST_CASE("retopo: a piece too small for a patch is kept as it was", "[retopo]") {
    // Both methods, which share everything round the quads.
    const RetopoMethod method = GENERATE(RetopoMethod::Field, RetopoMethod::Layout);
    INFO((method == RetopoMethod::Field ? "field" : "layout"));
    // A box and, far off, a tiny one.
    Mesh big = geom::MakeBox({});
    geom::PrimitiveParams small;
    small.size = Vector3f{0.05f, 0.05f, 0.05f};
    Mesh tiny = geom::MakeBox(small);
    std::span<Vector3f> p = tiny.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    for (Vector3f& q : p) {
        q.x += 5.0f;
    }
    const Mesh meshes[2] = {big, tiny};
    Mesh both = geom::MergeMeshes(meshes);
    both.ensureConnectivity();
    RetopoOptions options;
    options.targetQuads = 100;
    options.method = method;
    const RetopoResult result = Retopologize(both, options);
    REQUIRE(result.report.ok());
    CHECK(result.report.piecesKept == 1);
    CHECK(result.report.piecesRemeshed == 1);
    // The tiny box's six quads and eight corners, exactly.
    const std::span<const Vector3f> positions =
        result.mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    u32 far = 0;
    for (const Vector3f& q : positions) {
        far += q.x > 4.0f ? 1 : 0;
    }
    CHECK(far == 8);
}

TEST_CASE("retopo: a part its layout cannot simplify is kept, never emptied or blown up", "[retopo]") {
    // A tetrahedron: four triangles whose corners each want patches of their
    // own, so its layout needs dozens of quads.
    geom::MeshBuilder builder;
    const geom::VertexId a = builder.addVertex({6, 1, 1});
    const geom::VertexId b = builder.addVertex({6, -1, -1});
    const geom::VertexId c = builder.addVertex({4, 1, -1});
    const geom::VertexId d = builder.addVertex({4, -1, 1});
    builder.addTriangle(a, b, c);
    builder.addTriangle(a, d, b);
    builder.addTriangle(a, c, d);
    builder.addTriangle(b, d, c);
    Mesh tetra = builder.build().mesh;
    tetra.ensureConnectivity();
    RetopoOptions options;
    options.method = RetopoMethod::Layout;
    options.keepBelowQuads = 0.0f;

    // Alone and asked for one quad: nothing to write, not an empty mesh.
    options.targetQuads = 1;
    const RetopoResult alone = Retopologize(tetra, options);
    CHECK(alone.report.failure == RetopoReport::Failure::Unsolved);
    CHECK(alone.report.piecesKept == 1);

    // Beside a box that remeshes: kept as its own four triangles.
    const Mesh meshes[2] = {geom::MakeBox({}), tetra};
    Mesh both = geom::MergeMeshes(meshes);
    both.ensureConnectivity();
    options.targetQuads = 24;
    const RetopoResult result = Retopologize(both, options);
    REQUIRE(result.report.ok());
    CHECK(result.report.piecesRemeshed == 1);
    CHECK(result.report.piecesKept == 1);
    const geom::Topology& topology = result.mesh.topology();
    const std::span<const Vector3f> positions =
        result.mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    u32 far = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        bool beyond = true;
        for (const geom::VertexId v : topology.fv(geom::FaceId(f))) {
            beyond = beyond && positions[v.index()].x > 3.0f;
        }
        far += beyond ? 1 : 0;
    }
    CHECK(far == 4);
}

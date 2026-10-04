// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Subdivide, Smooth and Orient Outward (refine.h): a cube subdivided once has
// 26 vertices and 24 quads, stays closed, keeps its UV seams and its weights
// summing to one; Catmull-Clark rounds a corner to 5/9 and creases hold it;
// Smooth keeps a flat grid flat with its border pinned; Orient Outward turns
// an inside-out box.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/refine.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace whiteout::models::wem::geom;

namespace {

Mesh Cube() {
    PrimitiveParams params;
    params.size = {1.0f, 1.0f, 1.0f};
    return MakeBox(params);
}

std::span<const Vector3f> Positions(const Mesh& mesh) {
    return mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
}

u32 BorderEdges(const Mesh& mesh) {
    const Topology& topology = mesh.topology();
    u32 count = 0;
    for (u32 e = 0; e < topology.edgeCount(); ++e)
        count += !topology.isDeleted(EdgeId(e)) && topology.isBoundary(EdgeId(e)) ? 1u : 0u;
    return count;
}

/// Edges whose two sides' UVs part: the seams the map has.
u32 UvSeams(const Mesh& mesh) {
    const Topology& topology = mesh.topology();
    const std::span<const Vector2f> uv = mesh.attributes.get<Vector2f>(names::uv(0), Domain::Halfedge);
    u32 seams = 0;
    for (u32 e = 0; e < topology.edgeCount(); ++e) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const HalfedgeId o = Topology::halfedge(EdgeId(e), 1);
        if (topology.isBoundary(EdgeId(e)))
            continue;
        const auto same = [](const Vector2f& a, const Vector2f& b) {
            return std::abs(a.x - b.x) < 1e-5f && std::abs(a.y - b.y) < 1e-5f;
        };
        if (!same(uv[h.index()], uv[topology.next(o).index()]) || !same(uv[topology.next(h).index()], uv[o.index()]))
            ++seams;
    }
    return seams;
}

/// Each face's normal against the way from the mesh's centre to the face.
u32 FacesLookingIn(const Mesh& mesh) {
    const Topology& topology = mesh.topology();
    const std::span<const Vector3f> p = Positions(mesh);
    Vector3f centre{0, 0, 0};
    for (const Vector3f& v : p)
        centre = centre + v;
    centre = centre * (1.0f / static_cast<f32>(p.size()));
    u32 inward = 0;
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        std::vector<Vector3f> loop;
        for (const VertexId v : topology.fv(FaceId(f)))
            loop.push_back(p[v.index()]);
        Vector3f normal{0, 0, 0};
        Vector3f middle{0, 0, 0};
        for (std::size_t i = 0; i < loop.size(); ++i) {
            normal = normal + cross(loop[i], loop[(i + 1) % loop.size()]);
            middle = middle + loop[i];
        }
        middle = middle * (1.0f / static_cast<f32>(loop.size()));
        inward += normal.dot(middle - centre) < 0.0f ? 1u : 0u;
    }
    return inward;
}

void Finish(Mesh& mesh, ModelPlan& plan) {
    REQUIRE_FALSE(plan.refused());
    FinishTool(mesh, plan);
}

} // namespace

TEST_CASE("Subdivide: a cube once is 26 vertices and 24 quads, closed, seams and weights kept", "[wem][refine]") {
    for (const SubdivideScheme scheme : {SubdivideScheme::CatmullClark, SubdivideScheme::Linear}) {
        Mesh cube = Cube();
        REQUIRE(cube.vertexCount() == 8);
        // Two bones, a different share at every corner.
        cube.skin.reset(cube.vertexCount());
        for (u32 v = 0; v < cube.vertexCount(); ++v) {
            const f32 w = 0.1f + 0.1f * static_cast<f32>(v);
            const Influence two[] = {{0, w}, {1, 1.0f - w}};
            cube.skin.assignVertex(v, two);
        }
        const u32 seams = UvSeams(cube);
        CHECK(seams == 12);
        SubdivideParams params;
        params.scheme = scheme;
        ModelPlan plan = PlanSubdivide(cube, {}, params);
        Finish(cube, plan);
        CHECK(cube.vertexCount() == 26);
        CHECK(cube.faceCount() == 24);
        for (const u32 valence : cube.faceSet().faceValence)
            CHECK(valence == 4);
        CHECK(BorderEdges(cube) == 0);
        // Each old edge is two now, and nothing inside a side parts.
        CHECK(UvSeams(cube) == 2 * seams);
        for (u32 v = 0; v < cube.vertexCount(); ++v) {
            f32 sum = 0.0f;
            for (const Influence& i : cube.skin.forVertex(v))
                sum += i.weight;
            CHECK(sum == Catch::Approx(1.0f).margin(1e-4f));
        }
        CHECK(FacesLookingIn(cube) == 0);
    }
}

TEST_CASE("Subdivide: Catmull-Clark rounds a corner to 5/9; creases hold it", "[wem][refine]") {
    Mesh round = Cube();
    SubdivideParams params;
    params.creases = false;
    ModelPlan plan = PlanSubdivide(round, {}, params);
    Finish(round, plan);
    // The corners moved in: (Q + 2R) / 3 with Q = 1/3 and R = 2/3 per axis.
    // Face points stay on their faces' centres, so only a corner tells.
    bool corner = false;
    bool kept = false;
    for (const Vector3f& p : Positions(round)) {
        corner = corner || (std::abs(p.x - 5.0f / 9.0f) < 1e-4f && std::abs(p.y - 5.0f / 9.0f) < 1e-4f &&
                            std::abs(p.z - 5.0f / 9.0f) < 1e-4f);
        kept = kept || std::min({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) > 0.99f;
    }
    CHECK(corner);
    CHECK_FALSE(kept);

    // The box's edges are sharp: with creases they hold, and so does the box.
    Mesh boxed = Cube();
    ModelPlan held = PlanSubdivide(boxed, {}, SubdivideParams{});
    Finish(boxed, held);
    for (const Vector3f& p : Positions(boxed))
        CHECK(std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) == Catch::Approx(1.0f));
}

TEST_CASE("Subdivide: one face of a cube leaves the rest of it where it was", "[wem][refine]") {
    Mesh cube = Cube();
    ElementSet top;
    top.faces = {1}; // MakeBox's +Z side
    SubdivideParams params;
    params.creases = false;
    ModelPlan plan = PlanSubdivide(cube, top, params);
    Finish(cube, plan);
    CHECK(cube.faceCount() == 9);
    CHECK(BorderEdges(cube) == 0);
    for (const Vector3f& p : Positions(cube))
        if (p.z < 0.99f)
            CHECK(std::max(std::abs(p.x), std::abs(p.y)) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("Smooth keeps a flat grid flat and its border where it was", "[wem][refine]") {
    PrimitiveParams params;
    params.size = {4.0f, 4.0f, 1.0f};
    params.segments = 4;
    Mesh grid = MakePlane(params);
    const std::vector<Vector3f> before(Positions(grid).begin(), Positions(grid).end());
    // One interior point knocked off the lattice, in the plane.
    const std::span<Vector3f> p = grid.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    u32 knocked = kInvalidId;
    for (u32 v = 0; v < p.size(); ++v)
        if (!grid.topology().isBoundary(VertexId(v)) && knocked == kInvalidId) {
            knocked = v;
            p[v] = p[v] + Vector3f{0.7f, -0.4f, 0.0f};
        }
    REQUIRE(knocked != kInvalidId);
    for (const SmoothScheme scheme : {SmoothScheme::Laplacian, SmoothScheme::Taubin}) {
        Mesh copy = grid;
        SmoothParams smooth;
        smooth.iterations = 10;
        smooth.scheme = scheme;
        ModelPlan plan = PlanSmooth(copy, {}, smooth);
        Finish(copy, plan);
        const std::span<const Vector3f> after = Positions(copy);
        REQUIRE(after.size() == before.size());
        for (u32 v = 0; v < after.size(); ++v) {
            CHECK(after[v].z == Catch::Approx(0.0f).margin(1e-6f));
            if (copy.topology().isBoundary(VertexId(v))) {
                CHECK(after[v].x == Catch::Approx(before[v].x));
                CHECK(after[v].y == Catch::Approx(before[v].y));
            }
        }
        // And the knocked point went most of the way back.
        CHECK((after[knocked] - before[knocked]).length() < 0.4f);
    }
}

TEST_CASE("Orient Outward turns an inside-out box", "[wem][refine]") {
    Mesh cube = Cube();
    REQUIRE(FacesLookingIn(cube) == 0);
    // Every face reversed: wound one way still, but in.
    FaceSet reversed = cube.faceSet();
    u32 base = 0;
    for (const u32 valence : reversed.faceValence) {
        std::reverse(reversed.cornerVertex.begin() + base, reversed.cornerVertex.begin() + base + valence);
        base += valence;
    }
    Mesh inside;
    inside.setFaceSet(reversed);
    const std::span<const Vector3f> from = Positions(cube);
    const std::span<Vector3f> to =
        inside.attributes.getOrCreate<Vector3f>(names::kPosition, Domain::Vertex, AttrType::F32x3);
    std::copy(from.begin(), from.end(), to.begin());
    inside.attributes.getOrCreate<u32>(names::kSection, Domain::Face, AttrType::U32);
    inside.sections.emplace_back();
    REQUIRE(inside.ensureConnectivity().ok());
    CHECK(FacesLookingIn(inside) == 6);
    ModelPlan plan = PlanOrientOutward(inside, {});
    CHECK(plan.changed == 6);
    Finish(inside, plan);
    CHECK(FacesLookingIn(inside) == 0);
    // Again: nothing left to turn.
    const ModelPlan again = PlanOrientOutward(inside, {});
    CHECK(again.changed == 0);
}

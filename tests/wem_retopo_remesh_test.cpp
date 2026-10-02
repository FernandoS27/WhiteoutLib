// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// R1 (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.1-§1.4): the source surface, its
/// features and regions, and the refinement that keeps every vertex on it.

#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>

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

u32 CountKind(const Surface& surface, VertexKind kind) {
    return static_cast<u32>(std::count(surface.kinds.begin(), surface.kinds.end(), kind));
}

/// Every live triangle faces the way the source does where its corners sit.
u32 FlippedTriangles(const WorkMesh& mesh, const Surface& surface) {
    u32 flipped = 0;
    for (u32 t = 0; t < mesh.triangleCount(); ++t) {
        if (mesh.dead[t]) {
            continue;
        }
        const V3 n = mesh.triangleNormal(t);
        V3 reference{0.0, 0.0, 0.0};
        for (u32 i = 0; i < 3; ++i) {
            reference = reference + surface.triangleNormals[mesh.homes[mesh.corners[3 * t + i]].triangle];
        }
        if (Dot(n, reference) <= 0.0) {
            ++flipped;
        }
    }
    return flipped;
}

/// The share of live edges within [L/2, 2L].
f64 InBand(const WorkMesh& mesh, f64 length) {
    u32 inside = 0;
    u32 total = 0;
    for (u32 h = 0; h < mesh.corners.size(); ++h) {
        if (mesh.dead[h / 3] || (mesh.twins[h] != kNone && mesh.twins[h] < h)) {
            continue;
        }
        const f64 l = Distance(mesh.positions[mesh.from(h)], mesh.positions[mesh.to(h)]);
        inside += (l >= 0.5 * length && l <= 2.0 * length) ? 1 : 0;
        ++total;
    }
    return total == 0 ? 0.0 : static_cast<f64>(inside) / total;
}

/// Every work feature edge runs along a source feature edge: its midpoint is
/// on one, to within @p tolerance.
bool FeatureEdgesOnSource(const WorkMesh& mesh, const Surface& surface, f64 tolerance) {
    for (u32 h = 0; h < mesh.corners.size(); ++h) {
        if (mesh.dead[h / 3] || !mesh.isFeature(h)) {
            continue;
        }
        const V3 middle = Lerp(mesh.positions[mesh.from(h)], mesh.positions[mesh.to(h)], 0.5);
        f64 best = 1e300;
        for (u32 s = 0; s < surface.corners.size(); ++s) {
            if (!surface.isFeature(s) || surface.curves[s] != mesh.curves[h]) {
                continue;
            }
            const V3 a = surface.positions[surface.from(s)];
            const V3 b = surface.positions[surface.to(s)];
            best = std::min(best, Distance(middle, Lerp(a, b, NearestOnSegment(middle, a, b))));
        }
        if (best > tolerance) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("retopo surface: a box has eight corners, twelve curves, six regions",
          "[retopo][surface]") {
    const Mesh box = Prepared(geom::MakeBox({}));
    const Surface surface = BuildSurface(box, FeatureOptions{});
    CHECK(surface.triangleCount() == 12);
    CHECK(surface.componentCount == 1);
    CHECK(surface.regionCount == 6);
    CHECK(CountKind(surface, VertexKind::Corner) == 8);
    CHECK(surface.curveCount == 12);
    // The diagonal of each side is no feature.
    u32 featureHalfedges = 0;
    for (u32 h = 0; h < surface.corners.size(); ++h) {
        featureHalfedges += surface.isFeature(h) ? 1 : 0;
    }
    CHECK(featureHalfedges == 24);
}

TEST_CASE("retopo surface: a sharp edge is a feature only where the shading breaks hard",
          "[retopo][surface]") {
    // Two flat-shaded panels on a hinge marked sharp, folded by `fold`.
    auto hinged = [](f32 fold) {
        geom::MeshBuilder builder;
        const geom::VertexId a = builder.addVertex({0, 0, 0});
        const geom::VertexId b = builder.addVertex({0, 1, 0});
        const geom::VertexId p = builder.addVertex({-1, 0, 0});
        const geom::VertexId q = builder.addVertex({-1, 1, 0});
        const geom::VertexId r = builder.addVertex({std::cos(fold), 0, std::sin(fold)});
        const geom::VertexId t = builder.addVertex({std::cos(fold), 1, std::sin(fold)});
        const geom::VertexId left[4] = {p, a, b, q};
        const geom::VertexId right[4] = {a, r, t, b};
        builder.addFace(left);
        builder.addFace(right);
        Mesh mesh = builder.build().mesh;
        mesh.ensureConnectivity();
        const geom::Topology& topology = mesh.topology();
        const std::span<const Vector3f> positions =
            mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        const std::span<Vector3f> normals =
            mesh.attributes.getOrCreate<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge, geom::AttrType::F32x3);
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            std::vector<V3> corners;
            for (const geom::HalfedgeId h : topology.fh(geom::FaceId(f))) {
                corners.push_back(ToV3(positions[topology.from(h).value()]));
            }
            const V3 n = Unit(Cross(corners[1] - corners[0], corners[2] - corners[1]));
            for (const geom::HalfedgeId h : topology.fh(geom::FaceId(f))) {
                normals[h.index()] = Vector3f{static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z)};
            }
        }
        const geom::HalfedgeId hinge = topology.findHalfedge(a, b);
        mesh.attributes.getOrCreate<u8>(geom::names::kSharp, geom::Domain::Edge, geom::AttrType::Bool)
            [geom::Topology::edge(hinge).value()] = 1;
        return mesh;
    };
    auto sharpHalfedges = [](const Surface& surface) {
        u32 count = 0;
        for (const u8 bits : surface.features) {
            count += (bits & kFeatureSharp) != 0 ? 1 : 0;
        }
        return count;
    };
    const f32 kDegree = 0.0174532925f;
    // A soft fold, as a low-poly cloth's: no feature, unless every break is asked for.
    const Mesh soft = hinged(30.0f * kDegree);
    CHECK(sharpHalfedges(BuildSurface(soft, FeatureOptions{})) == 0);
    FeatureOptions every;
    every.sharpAngle = 0.0f;
    CHECK(sharpHalfedges(BuildSurface(soft, every)) == 2);
    // A hard edge, as a box's.
    CHECK(sharpHalfedges(BuildSurface(hinged(90.0f * kDegree), FeatureOptions{})) == 2);
}

TEST_CASE("retopo surface: locate stays on the connected surface it starts from",
          "[retopo][surface]") {
    // Two parallel plates a hair apart: a global nearest point would cross.
    geom::MeshBuilder builder;
    const f32 gap = 0.01f;
    std::vector<geom::VertexId> v;
    for (f32 z : {0.0f, gap}) {
        v.push_back(builder.addVertex({0, 0, z}));
        v.push_back(builder.addVertex({1, 0, z}));
        v.push_back(builder.addVertex({1, 1, z}));
        v.push_back(builder.addVertex({0, 1, z}));
    }
    builder.addTriangle(v[0], v[1], v[2]);
    builder.addTriangle(v[0], v[2], v[3]);
    builder.addTriangle(v[4], v[6], v[5]);
    builder.addTriangle(v[4], v[7], v[6]);
    Mesh mesh = builder.build().mesh;
    mesh.ensureConnectivity();
    const Surface surface = BuildSurface(mesh, FeatureOptions{});
    REQUIRE(surface.componentCount == 2);
    u32 lowerTriangle = 0;
    while (surface.components[lowerTriangle] != surface.components[0]) {
        ++lowerTriangle;
    }
    const u32 start[1] = {lowerTriangle};
    // A point just above the upper plate, searched from the lower one.
    const SurfacePoint found = surface.locate(start, Make(0.5, 0.5, gap * 2.0), 0.5);
    REQUIRE(found.valid());
    CHECK(surface.components[found.triangle] == surface.components[lowerTriangle]);
    CHECK(std::abs(surface.position(found).z) < 1e-12);
}

TEST_CASE("retopo remesh: each stage keeps the connectivity valid", "[retopo][remesh]") {
    const Mesh box = Prepared(geom::MakeBox({}));
    const Surface surface = BuildSurface(box, FeatureOptions{});
    WorkMesh mesh = SeedWorkMesh(surface, 0);
    REQUIRE(Validate(mesh).empty());
    const f64 length = 0.25;
    for (u32 iteration = 0; iteration < 4; ++iteration) {
        INFO("iteration " << iteration);
        SplitLong(mesh, surface, length * 4.0 / 3.0);
        REQUIRE(Validate(mesh) == "");
        CollapseShort(mesh, length * 0.8, length * 4.0 / 3.0);
        REQUIRE(Validate(mesh) == "");
        FlipToValence(mesh);
        REQUIRE(Validate(mesh) == "");
        Relax(mesh, surface, 0.5);
        REQUIRE(Validate(mesh) == "");
        CHECK(FlippedTriangles(mesh, surface) == 0);
        mesh.compact();
        REQUIRE(Validate(mesh) == "");
    }
}

TEST_CASE("retopo remesh: a box refines with every vertex on it and its edges kept",
          "[retopo][remesh]") {
    const Mesh box = Prepared(geom::MakeBox({}));
    const Surface surface = BuildSurface(box, FeatureOptions{});
    WorkMesh mesh = SeedWorkMesh(surface, 0);
    const f64 length = 0.25;
    const RemeshStats stats = Remesh(mesh, surface, RemeshOptions{length, 8, 0.5});
    CHECK(stats.splits > 0);
    CHECK(HomesConsistent(mesh, surface, 1e-9));
    CHECK(FlippedTriangles(mesh, surface) == 0);
    CHECK(FeatureEdgesOnSource(mesh, surface, 1e-9));
    u32 corners = 0;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.kinds[v] == VertexKind::Corner) {
            ++corners;
            CHECK(std::abs(std::abs(mesh.positions[v].x) - 1.0) < 1e-12);
        }
    }
    CHECK(corners == 8);
    // Six faces of area four: about 24 / (L^2 sqrt(3)/4) triangles.
    const f64 ideal = 24.0 / (length * length * std::sqrt(3.0) / 4.0);
    CHECK(mesh.triangleCount() > ideal * 0.5);
    CHECK(mesh.triangleCount() < ideal * 2.0);
    CHECK(InBand(mesh, length) > 0.97);
}

TEST_CASE("retopo remesh: a sphere refines onto itself", "[retopo][remesh]") {
    geom::PrimitiveParams params;
    params.sides = 16;
    params.segments = 8;
    const Mesh sphere = Prepared(geom::MakeSphere(params));
    FeatureOptions options;
    options.uvSeamSets = 0; // no seams: one region
    const Surface surface = BuildSurface(sphere, options);
    CHECK(surface.regionCount == 1);
    CHECK(CountKind(surface, VertexKind::Corner) == 0);
    WorkMesh mesh = SeedWorkMesh(surface, 0);
    Remesh(mesh, surface, RemeshOptions{0.15, 8, 0.5});
    CHECK(HomesConsistent(mesh, surface, 1e-9));
    CHECK(FlippedTriangles(mesh, surface) == 0);
    CHECK(InBand(mesh, 0.15) > 0.97);
    // Closed: no border vertex.
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        CHECK_FALSE(mesh.isBorder(v));
    }
}

TEST_CASE("retopo remesh: a plane's border stays its border", "[retopo][remesh]") {
    geom::PrimitiveParams params;
    params.segments = 2;
    const Mesh plane = Prepared(geom::MakePlane(params));
    const Surface surface = BuildSurface(plane, FeatureOptions{});
    WorkMesh mesh = SeedWorkMesh(surface, 0);
    Remesh(mesh, surface, RemeshOptions{0.1, 8, 0.5});
    CHECK(HomesConsistent(mesh, surface, 1e-9));
    CHECK(FlippedTriangles(mesh, surface) == 0);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.isBorder(v)) {
            const V3 p = mesh.positions[v];
            const bool onEdge = std::abs(std::abs(p.x) - 1.0) < 1e-9 || std::abs(std::abs(p.y) - 1.0) < 1e-9;
            CHECK(onEdge);
            CHECK(mesh.kinds[v] != VertexKind::Free);
        }
    }
    SplitFeatureCorners(mesh, surface);
    REQUIRE(Validate(mesh).empty());
    for (u32 t = 0; t < mesh.triangleCount(); ++t) {
        u32 features = 0;
        for (u32 i = 0; i < 3; ++i) {
            features += mesh.isFeature(3 * t + i) ? 1 : 0;
        }
        CHECK(features <= 1);
    }
}

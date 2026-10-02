// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// R2 (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.5): the cross field and its
/// singularities, against Poincaré-Hopf on closed shapes and the features it
/// must follow.

#include <cmath>
#include <numeric>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>

#include "whiteout/models/wem/geometry/retopo/cross_field.h"
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

/// A torus of @p major and @p minor radius, quads cut in two.
Mesh MakeTorus(f32 major, f32 minor, u32 around, u32 tube) {
    geom::MeshBuilder builder;
    std::vector<geom::VertexId> v;
    for (u32 i = 0; i < around; ++i) {
        const f32 a = 2.0f * 3.14159265f * i / around;
        for (u32 j = 0; j < tube; ++j) {
            const f32 b = 2.0f * 3.14159265f * j / tube;
            const f32 r = major + minor * std::cos(b);
            v.push_back(builder.addVertex({r * std::cos(a), r * std::sin(a), minor * std::sin(b)}));
        }
    }
    for (u32 i = 0; i < around; ++i) {
        for (u32 j = 0; j < tube; ++j) {
            const u32 i1 = (i + 1) % around;
            const u32 j1 = (j + 1) % tube;
            builder.addTriangle(v[i * tube + j], v[i1 * tube + j], v[i1 * tube + j1]);
            builder.addTriangle(v[i * tube + j], v[i1 * tube + j1], v[i * tube + j1]);
        }
    }
    Mesh mesh = builder.build().mesh;
    mesh.ensureConnectivity();
    return mesh;
}

struct Solved {
    Surface surface;
    WorkMesh mesh;
    CrossField field;
};

Solved Solve(const Mesh& source, const FeatureOptions& features, f64 length, f64 curvature = 0.3) {
    Solved out;
    out.surface = BuildSurface(source, features);
    out.mesh = SeedWorkMesh(out.surface, 0);
    Remesh(out.mesh, out.surface, RemeshOptions{length, 6, 0.5});
    SplitFeatureCorners(out.mesh, out.surface);
    out.mesh.compact();
    FieldOptions options;
    options.curvature = curvature;
    options.scale = length * 3.0;
    out.field = SolveCrossField(out.mesh, out.surface, options);
    return out;
}

i32 TotalIndex(const CrossField& field) {
    return std::accumulate(field.index.begin(), field.index.end(), 0);
}

} // namespace

TEST_CASE("retopo field: a box has a valence-3 singularity at each corner and nowhere else",
          "[retopo][field]") {
    const Solved s = Solve(Prepared(geom::MakeBox({})), FeatureOptions{}, 0.25);
    u32 atCorners = 0;
    for (u32 v = 0; v < s.mesh.vertexCount(); ++v) {
        if (s.field.index[v] != 0) {
            CHECK(s.mesh.kinds[v] == VertexKind::Corner);
            CHECK(s.field.index[v] == 1);
            ++atCorners;
        }
    }
    CHECK(atCorners == 8);
    CHECK(TotalIndex(s.field) == 8);
}

TEST_CASE("retopo field: a sphere's indices sum to its Euler characteristic", "[retopo][field]") {
    geom::PrimitiveParams params;
    params.sides = 24;
    params.segments = 12;
    FeatureOptions features;
    features.uvSeamSets = 0;
    const Mesh sphere = Prepared(geom::MakeSphere(params));
    // The smoothest field has eight valence-3 points, not a pole of index two.
    const Solved smooth = Solve(sphere, features, 0.12, 0.0);
    CHECK(TotalIndex(smooth.field) == 8);
    for (i32 i : smooth.field.index) {
        CHECK(i <= 1);
    }
    CHECK(smooth.field.singularityCount() >= 8);
    CHECK(smooth.field.singularityCount() <= 12);
    // With the curvature pull the count still adds up.
    const Solved pulled = Solve(sphere, features, 0.12);
    CHECK(TotalIndex(pulled.field) == 8);
}

TEST_CASE("retopo field: a cylinder's field follows its axis", "[retopo][field]") {
    geom::PrimitiveParams params;
    params.sides = 32;
    params.size = Vector3f{0.5f, 0.5f, 2.0f};
    const Solved s = Solve(Prepared(geom::MakeCylinder(params)), FeatureOptions{}, 0.08);
    // Side triangles away from the caps: one arm along z.
    // A flat triangle on a round side need not contain the axis itself, so a
    // few degrees off it is the facet, not the field.
    u32 checked = 0;
    f64 total = 0.0;
    for (u32 t = 0; t < s.mesh.triangleCount(); ++t) {
        const V3& n = s.field.normals[t];
        const V3 p = s.mesh.positions[s.mesh.corners[3 * t]];
        if (std::abs(n.z) > 0.1 || std::abs(p.z) > 1.4) {
            continue;
        }
        const V3 d = s.field.direction(t, 0);
        const f64 off = std::min(std::abs(d.z), std::sqrt(d.x * d.x + d.y * d.y));
        CHECK(off < 0.12);
        total += off;
        ++checked;
    }
    CHECK(checked > 100);
    CHECK(total / checked < 0.03);
}

TEST_CASE("retopo field: a torus needs no singularity", "[retopo][field]") {
    const Solved s = Solve(MakeTorus(2.0f, 0.7f, 32, 16), FeatureOptions{}, 0.2);
    CHECK(TotalIndex(s.field) == 0);
    CHECK(s.field.singularityCount() <= 4);
}

TEST_CASE("retopo field: on a plane it follows the border and stays regular", "[retopo][field]") {
    geom::PrimitiveParams params;
    params.segments = 3;
    const Solved s = Solve(Prepared(geom::MakePlane(params)), FeatureOptions{}, 0.1);
    CHECK(s.field.singularityCount() == 0);
    // Every triangle's cross is axis-aligned (the square's own directions).
    for (u32 t = 0; t < s.mesh.triangleCount(); ++t) {
        const V3 d = s.field.direction(t, 0);
        const f64 offAxis = std::min(std::abs(d.x), std::abs(d.y));
        CHECK(offAxis < 0.02);
    }
}

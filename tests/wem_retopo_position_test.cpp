// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// F1/F2 (EDIT_MODE_RETOPOLOGY_DESIGN.md §6): the field method's lattice, and
/// the quads it extracts, on shapes whose answers are known.

#include <cmath>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/retopo/retopology.h>

#include "whiteout/models/wem/geometry/retopo/cross_field.h"
#include "whiteout/models/wem/geometry/retopo/position_field.h"
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

/// A shift from @p a's frame as whole steps in @p ref's.
void InFrame(const PositionField& field, u32 a, u32 ref, const LatticeShift& s, i32& x, i32& y) {
    const V3 d = field.axes[a] * s.x + field.across(a) * s.y;
    x = static_cast<i32>(std::lround(Dot(d, field.axes[ref])));
    y = static_cast<i32>(std::lround(Dot(d, field.across(ref))));
}

struct Counts {
    u32 quads = 0;
    u32 other = 0;
    i64 euler = 0;
};

Counts Count(const Mesh& mesh) {
    Counts c;
    const geom::Topology& topology = mesh.topology();
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        (topology.valence(geom::FaceId(f)) == 4 ? c.quads : c.other) += 1;
    }
    c.euler = static_cast<i64>(topology.vertexCount()) - topology.edgeCount() + topology.faceCount();
    return c;
}

RetopoResult Field(const Mesh& mesh, u32 quads, bool pure = true) {
    RetopoOptions options;
    options.method = RetopoMethod::Field;
    options.targetQuads = quads;
    options.pureQuads = pure;
    return Retopologize(mesh, options);
}

} // namespace

TEST_CASE("retopo position field: on a flat square the lattice closes round every triangle", "[retopo]") {
    const Mesh plane = Prepared(geom::MakePlane(geom::PrimitiveParams{{1.0f, 1.0f, 1.0f}, 12, 6}));
    const f64 h = 0.25;
    Surface surface = BuildSurface(plane, FeatureOptions{});
    ScaleFeatures(surface, h, FeatureOptions{}.cornerAngle);
    WorkMesh mesh = SeedWorkMesh(surface, 0);
    Remesh(mesh, surface, RemeshOptions{h / 3.0, 6, 0.5});
    SplitFeatureCorners(mesh, surface);
    mesh.compact();
    FieldOptions options;
    options.scale = h;
    const CrossField cross = SolveCrossField(mesh, surface, options);
    PositionField field = VertexFrames(mesh, surface, cross);
    SolvePositions(mesh, field, PositionOptions{h});

    auto frame = [&](u32 v) { return LatticeFrame{mesh.positions[v], field.normals[v], field.axes[v], field.origins[v]}; };
    u32 open = 0;
    for (u32 t = 0; t < mesh.triangleCount(); ++t) {
        i32 sx = 0;
        i32 sy = 0;
        const u32 ref = mesh.corners[3 * t];
        for (u32 i = 0; i < 3; ++i) {
            const u32 a = mesh.corners[3 * t + i];
            const u32 b = mesh.corners[3 * t + (i + 1) % 3];
            i32 x = 0;
            i32 y = 0;
            InFrame(field, a, ref, Shift(frame(a), frame(b), h), x, y);
            sx += x;
            sy += y;
        }
        open += sx != 0 || sy != 0 ? 1 : 0;
    }
    CHECK(open == 0);

    // The border is on lattice lines: each border vertex's offset across its
    // line is whole steps.
    u32 off = 0;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (field.pins[v] == LatticePin::None) {
            continue;
        }
        const f64 across = Dot(mesh.positions[v] - field.origins[v], field.across(v)) / h;
        off += std::abs(across - std::round(across)) > 1e-6 ? 1 : 0;
    }
    CHECK(off == 0);
}

TEST_CASE("retopo field: a flat square comes out as its grid", "[retopo]") {
    const Mesh plane = geom::MakePlane(geom::PrimitiveParams{{1.0f, 1.0f, 1.0f}, 12, 6});
    for (const bool pure : {false, true}) {
        const RetopoResult result = Field(plane, 64, pure);
        INFO("pure " << pure << ": " << ToString(result.report.failure) << " quads " << result.report.quads
                     << " other " << result.report.otherFaces << " irregular " << result.report.irregularVertices);
        REQUIRE(result.report.ok());
        CHECK(result.report.quads == 64);
        CHECK(result.report.otherFaces == 0);
        CHECK(result.report.irregularVertices == 0);
    }
}

TEST_CASE("retopo field: closed shapes come out as quads, their topology kept", "[retopo]") {
    struct Shape {
        const char* name;
        Mesh mesh;
        u32 quads;
        i64 euler;
        u32 low;
        u32 high;
    };
    geom::PrimitiveParams sphere;
    sphere.sides = 24;
    sphere.segments = 12;
    const Shape shapes[] = {
        {"sphere", geom::MakeSphere(sphere), 300, 2, 240, 360},
        // A face of the box holds whole cells: 2 x 2 or 3 x 3 split in four.
        {"box", geom::MakeBox({}), 150, 2, 96, 216},
        {"torus", MakeTorus(1.0f, 0.35f, 48, 16), 400, 0, 320, 480},
    };
    for (const Shape& shape : shapes) {
        const RetopoResult result = Field(shape.mesh, shape.quads);
        const Counts counts = Count(result.mesh);
        INFO(shape.name << ": " << ToString(result.report.failure) << " quads " << result.report.quads << " other "
                        << result.report.otherFaces << " irregular " << result.report.irregularVertices << " euler "
                        << counts.euler);
        std::string notes;
        for (const std::string& note : result.report.notes) {
            notes += note + "; ";
        }
        INFO(notes);
        REQUIRE(result.report.ok());
        CHECK(result.report.otherFaces == 0);
        CHECK(counts.euler == shape.euler);
        CHECK(result.report.quads >= shape.low);
        CHECK(result.report.quads <= shape.high);
    }
}

TEST_CASE("retopo field: a box keeps its corners", "[retopo]") {
    const RetopoResult result = Field(geom::MakeBox({}), 150);
    REQUIRE(result.report.ok());
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
    CHECK(result.report.irregularVertices == 8);
}

TEST_CASE("retopo field: pieces of a few cells come out whole", "[retopo]") {
    // Closed pieces only a few lattice cells round -- a game model's eyes and
    // buttons -- at every count from one cell to a few dozen.
    geom::MeshBuilder builder;
    const geom::VertexId a = builder.addVertex({1, 1, 1});
    const geom::VertexId b = builder.addVertex({1, -1, -1});
    const geom::VertexId c = builder.addVertex({-1, 1, -1});
    const geom::VertexId d = builder.addVertex({-1, -1, 1});
    builder.addTriangle(a, b, c);
    builder.addTriangle(a, d, b);
    builder.addTriangle(a, c, d);
    builder.addTriangle(b, d, c);
    Mesh tetra = builder.build().mesh;
    tetra.ensureConnectivity();
    geom::PrimitiveParams ball;
    ball.sides = 8;
    ball.segments = 4;
    const Mesh shapes[] = {tetra, geom::MakeSphere(ball), geom::MakeBox({})};
    for (u32 k = 0; k < 3; ++k) {
        const Mesh& shape = shapes[k];
        for (const u32 quads : {1u, 2u, 4u, 6u, 9u, 14u, 24u, 40u}) {
            for (const bool pure : {true, false}) {
                RetopoOptions options;
                options.method = RetopoMethod::Field;
                options.targetQuads = quads;
                options.pureQuads = pure;
                options.keepBelowQuads = 0.0f;
                const RetopoResult result = Retopologize(shape, options);
                std::string notes;
                for (const std::string& note : result.report.notes) {
                    notes += note + "; ";
                }
                INFO("shape " << k << " quads " << quads << " pure " << pure << ": "
                              << ToString(result.report.failure) << " kept " << result.report.piecesKept << " "
                              << notes);
                CHECK((result.report.ok() || result.report.failure == RetopoReport::Failure::Unsolved));
                if (result.report.ok()) {
                    CHECK(Count(result.mesh).euler == 2);
                }
            }
        }
    }
}


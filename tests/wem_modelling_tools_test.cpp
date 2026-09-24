// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G25: the tools (EDIT_MODE_MODELLING_DESIGN.md §3). Every result is re-checked
/// Structural- and Manifold-clean and canonical, and every refusal is produced
/// by a fixture built to need exactly it. ML2's eight: To Quads, To Polygons,
/// Turn, Edit Triangulation, Retriangulate, Triangulate, Target Weld, Weld.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>

#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/geometry/modelling.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/triangulation.h>
#include <whiteout/models/wem/skinning/points.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

using geom::Domain;
using geom::EdgeId;
using geom::ElementSet;
using geom::FaceId;
using geom::HalfedgeId;
using geom::ModelPlan;
using geom::ModelRefusal;
using geom::Topology;
using geom::VertexId;
using geom::kInvalidId;

Mesh fromFaces(const std::vector<std::vector<u32>>& corners, const std::vector<Vector3f>& positions) {
    geom::FaceSet faces;
    faces.vertexCount = static_cast<u32>(positions.size());
    for (const std::vector<u32>& face : corners) {
        faces.addFace(face);
    }
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> written = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, Domain::Vertex, geom::AttrType::F32x3);
    std::copy(positions.begin(), positions.end(), written.begin());
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    REQUIRE(mesh.ensureConnectivity().ok());
    return mesh;
}

/// A @p w x @p h grid of unit cells in the xy plane, each two triangles
/// ((x,y), (x+1,y), (x+1,y+1)) and ((x,y), (x+1,y+1), (x,y+1)), or one quad.
Mesh grid(u32 w, u32 h, bool quads = false) {
    std::vector<Vector3f> positions;
    for (u32 y = 0; y <= h; ++y) {
        for (u32 x = 0; x <= w; ++x) {
            positions.push_back(Vector3f{static_cast<f32>(x), static_cast<f32>(y), 0.0f});
        }
    }
    const auto at = [&](u32 x, u32 y) { return y * (w + 1) + x; };
    std::vector<std::vector<u32>> faces;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            if (quads) {
                faces.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
            } else {
                faces.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1)});
                faces.push_back({at(x, y), at(x + 1, y + 1), at(x, y + 1)});
            }
        }
    }
    return fromFaces(faces, positions);
}

std::string errors(const Mesh& mesh) {
    Diagnostics diagnostics;
    geom::CheckStructural(mesh, 0, diagnostics);
    geom::CheckManifold(mesh, 0, diagnostics);
    return diagnostics.hasErrors() ? diagnostics.formatHistogram() : std::string();
}

/// FinishTool, then the promise every tool keeps.
void finish(Mesh& mesh, ModelPlan& plan) {
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::FinishTool(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(geom::IsCanonical(mesh));
    Diagnostics stale;
    geom::CheckStructural(mesh, 0, stale);
    CHECK(stale.countOf(DiagCode::StaleTriangulation) == 0u);
}

ElementSet allFaces(const Mesh& mesh) {
    ElementSet out;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        if (!mesh.topology().isDeleted(FaceId(f))) {
            out.faces.push_back(f);
        }
    }
    return out;
}

/// Every live edge of @p mesh.
ElementSet allEdges(const Mesh& mesh) {
    ElementSet out;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (!mesh.topology().isDeleted(EdgeId(e))) {
            out.edges.push_back(e);
        }
    }
    return out;
}

u32 liveFaces(const Mesh& mesh) {
    return static_cast<u32>(allFaces(mesh).faces.size());
}

using Point = std::array<f32, 3>;
using Triangle = std::array<Point, 3>;

/// Every drawn triangle as ordered positions (first corner first), sorted.
std::vector<Triangle> drawn(const Mesh& mesh) {
    std::vector<u32> triangles;
    geom::TriangulateMesh(mesh, triangles);
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    std::vector<Triangle> out;
    for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
        Triangle triangle;
        for (u32 k = 0; k < 3; ++k) {
            const Vector3f& v = p[triangles[t + k]];
            triangle[k] = {v.x, v.y, v.z};
        }
        out.push_back(triangle);
    }
    std::sort(out.begin(), out.end());
    return out;
}

VertexId nearest(const Mesh& mesh, f32 x, f32 y) {
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    u32 best = 0;
    f32 bestDistance = 1e30f;
    for (u32 v = 0; v < p.size(); ++v) {
        const f32 d = (p[v].x - x) * (p[v].x - x) + (p[v].y - y) * (p[v].y - y);
        if (!mesh.topology().isDeleted(VertexId(v)) && d < bestDistance) {
            best = v;
            bestDistance = d;
        }
    }
    return VertexId(best);
}

VertexId vertexAt(const Mesh& mesh, f32 x, f32 y, f32 z = 0.0f) {
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < p.size(); ++v) {
        if (!mesh.topology().isDeleted(VertexId(v)) && p[v].x == x && p[v].y == y && p[v].z == z) {
            return VertexId(v);
        }
    }
    FAIL("no vertex there");
    return VertexId();
}

EdgeId edgeBetween(const Mesh& mesh, VertexId a, VertexId b) {
    const HalfedgeId h = mesh.topology().findHalfedge(a, b);
    REQUIRE(h.valid());
    return Topology::edge(h);
}

skinning::PointTable pointsOf(const Mesh& mesh) {
    return skinning::BuildPointTable(mesh);
}

} // namespace

// ============================================================================
// To Quads
// ============================================================================

TEST_CASE("wem tools to quads joins pairs and keeps the file's triangles", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 2);
    const std::vector<Triangle> before = drawn(mesh);
    ModelPlan plan = geom::PlanJoinTriangles(mesh, pointsOf(mesh), allFaces(mesh));
    CHECK(plan.changed == 4u);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 4u);
    CHECK(drawn(mesh) == before);
    REQUIRE(plan.selection.faces.size() == 4u);
    for (const u32 f : plan.selection.faces) {
        CHECK(mesh.topology().valence(FaceId(f)) == 4u);
    }
}

TEST_CASE("wem tools to quads never crosses a delimit", "[wem][geometry][tools]") {
    const auto joinsOf = [](Mesh mesh, bool flagged = true) {
        geom::JoinTrianglesParams params;
        params.delimitFlagged = flagged;
        ModelPlan plan = geom::PlanJoinTriangles(mesh, pointsOf(mesh), allFaces(mesh), params);
        geom::FinishTool(mesh, plan);
        return plan.changed;
    };
    SECTION("a section") {
        Mesh mesh = grid(1, 1);
        mesh.sections.emplace_back();
        mesh.faceSections()[1] = 1;
        CHECK(joinsOf(mesh) == 0u);
    }
    SECTION("a smoothing group") {
        Mesh mesh = grid(1, 1);
        mesh.attributes.getOrCreate<u32>(geom::names::kSmoothGroup, Domain::Face, geom::AttrType::U32)[1] = 7;
        CHECK(joinsOf(mesh) == 0u);
    }
    SECTION("a corner value at one end") {
        Mesh mesh = grid(1, 1);
        const std::span<Vector2f> uv = mesh.attributes.getOrCreate<Vector2f>(
            geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        // The corner of face 1 at vertex 0 differs from face 0's.
        for (const HalfedgeId h : std::as_const(mesh).topology().fh(FaceId(1))) {
            if (std::as_const(mesh).topology().from(h) == VertexId(0)) {
                uv[h.index()] = Vector2f{0.5f, 0.5f};
            }
        }
        CHECK(joinsOf(mesh) == 0u);
    }
    SECTION("a flagged seam, unless flags are not asked for") {
        Mesh mesh = grid(1, 1);
        const EdgeId diagonal = edgeBetween(mesh, VertexId(0), VertexId(3));
        mesh.attributes.getOrCreate<u8>(geom::names::kSeam, Domain::Edge, geom::AttrType::Bool)[diagonal.index()] = 1;
        CHECK(joinsOf(mesh) == 0u);
        CHECK(joinsOf(mesh, false) == 1u);
    }
    SECTION("none, and it joins") {
        CHECK(joinsOf(grid(1, 1)) == 1u);
    }
}

TEST_CASE("wem tools to quads skips a pair that would copy a face", "[wem][geometry][tools]") {
    // Two triangles capped by the quad they would make.
    Mesh mesh = fromFaces({{0, 1, 2}, {0, 2, 3}, {0, 3, 2, 1}},
                          {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}});
    ModelPlan plan = geom::PlanJoinTriangles(mesh, pointsOf(mesh), allFaces(mesh));
    CHECK(plan.changed == 0u);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 3u);
}

TEST_CASE("wem tools to quads selects the joined faces or the triangles left", "[wem][geometry][tools]") {
    // Two cells, and a fifth triangle a section keeps from joining.
    const auto build = [] {
        Mesh mesh = fromFaces({{0, 1, 4}, {0, 4, 3}, {1, 2, 5}, {1, 5, 4}, {3, 4, 6}},
                              {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0}, {0.5f, 2, 0}});
        mesh.sections.emplace_back();
        mesh.faceSections()[4] = 1;
        return mesh;
    };
    Mesh joined = build();
    ModelPlan plan = geom::PlanJoinTriangles(joined, pointsOf(joined), allFaces(joined));
    finish(joined, plan);
    CHECK(plan.selection.faces.size() == 2u);
    for (const u32 f : plan.selection.faces) {
        CHECK(joined.topology().valence(FaceId(f)) == 4u);
    }
    Mesh left = build();
    geom::JoinTrianglesParams params;
    params.deselectJoined = true;
    ModelPlan rest = geom::PlanJoinTriangles(left, pointsOf(left), allFaces(left), params);
    finish(left, rest);
    REQUIRE(rest.selection.faces.size() == 1u);
    CHECK(left.topology().valence(FaceId(rest.selection.faces[0])) == 3u);
}

TEST_CASE("wem tools to quads refuses an empty selection", "[wem][geometry][tools]") {
    Mesh mesh = grid(1, 1);
    CHECK(geom::PlanJoinTriangles(mesh, pointsOf(mesh), {}).refusal == ModelRefusal::EmptySelection);
}

// ============================================================================
// To Polygons
// ============================================================================

TEST_CASE("wem tools to polygons makes a flat 2x2 grid two rectangles", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 2, true);
    ModelPlan plan = geom::PlanLimitedDissolve(mesh, pointsOf(mesh), allFaces(mesh));
    finish(mesh, plan);
    REQUIRE(liveFaces(mesh) == 2u);
    // Each is a 2 x 1 rectangle: all its corners on two lines one apart.
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 f = 0; f < 2; ++f) {
        f32 lowX = 9;
        f32 highX = -9;
        f32 lowY = 9;
        f32 highY = -9;
        for (const VertexId v : mesh.topology().fv(FaceId(f))) {
            lowX = std::min(lowX, p[v.index()].x);
            highX = std::max(highX, p[v.index()].x);
            lowY = std::min(lowY, p[v.index()].y);
            highY = std::max(highY, p[v.index()].y);
        }
        CHECK((highX - lowX) * (highY - lowY) == 2.0f);
        CHECK(mesh.topology().valence(FaceId(f)) == 6u);
    }
    // The centre keeps its vertex.
    CHECK(mesh.vertexCount() == 9u);
}

TEST_CASE("wem tools to polygons stops curvature at the flatness cap", "[wem][geometry][tools]") {
    // A strip of six quads, bent three degrees at every rung: each dissolve is
    // within five degrees, but a face may not stray more than five in total.
    std::vector<Vector3f> positions;
    std::vector<std::vector<u32>> faces;
    f32 angle = 0.0f;
    Vector3f at{0, 0, 0};
    for (u32 i = 0; i <= 6; ++i) {
        positions.push_back(Vector3f{at.x, 0, at.z});
        positions.push_back(Vector3f{at.x, 1, at.z});
        at.x += std::cos(angle);
        at.z += std::sin(angle);
        angle += 3.0f * 3.14159265f / 180.0f;
    }
    for (u32 i = 0; i < 6; ++i) {
        faces.push_back({2 * i, 2 * i + 2, 2 * i + 3, 2 * i + 1});
    }
    Mesh mesh = fromFaces(faces, positions);
    ModelPlan plan = geom::PlanLimitedDissolve(mesh, pointsOf(mesh), allFaces(mesh));
    finish(mesh, plan);
    CHECK(liveFaces(mesh) > 1u);
    CHECK(liveFaces(mesh) < 6u);
}

TEST_CASE("wem tools to polygons refuses a union that is no valid cut", "[wem][geometry][tools]") {
    // A sliver under a triangle, coplanar: within the angle, but in the merged
    // face the sliver's triangle has no area worth drawing.
    Mesh mesh = fromFaces({{0, 1, 2}, {1, 0, 3}}, {{0, 0, 0}, {10, 0, 0}, {5, 5, 0}, {5, -1e-6f, 0}});
    ModelPlan plan = geom::PlanLimitedDissolve(mesh, pointsOf(mesh), allFaces(mesh));
    CHECK(plan.changed == 0u);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 2u);
}

// ============================================================================
// Turn, Edit Triangulation, Retriangulate, Triangulate
// ============================================================================

TEST_CASE("wem tools turn swaps a diagonal and renumbers nothing", "[wem][geometry][tools]") {
    Mesh mesh = fromFaces({{0, 1, 2, 3}}, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}});
    geom::MaterialiseRows(mesh);
    const std::vector<u32> corners = mesh.faceSet().cornerVertex;
    REQUIRE(std::vector<u32>(mesh.triangulation.row(0).begin(), mesh.triangulation.row(0).end()) ==
            std::vector<u32>{0, 1, 2, 0, 2, 3});
    ModelPlan plan = geom::PlanTurn(mesh, FaceId(0), VertexId(0), VertexId(2));
    CHECK_FALSE(plan.renumbers);
    finish(mesh, plan);
    const std::span<const u32> row = mesh.triangulation.row(0);
    std::set<u32> diagonal;
    for (const u32 v : row) {
        diagonal.insert(v);
    }
    CHECK(mesh.faceSet().cornerVertex == corners);
    // The new diagonal is 1-3: both triangles hold it.
    u32 holding = 0;
    for (std::size_t t = 0; t + 2 < row.size(); t += 3) {
        const std::set<u32> triangle{row[t], row[t + 1], row[t + 2]};
        holding += triangle.count(1) != 0 && triangle.count(3) != 0 ? 1u : 0u;
    }
    CHECK(holding == 2u);

    SECTION("its refusals") {
        CHECK(geom::PlanTurn(mesh, FaceId(0), VertexId(0), VertexId(1)).refusal == ModelRefusal::DiagonalOutside);
        CHECK(geom::PlanTurn(mesh, FaceId(5), VertexId(1), VertexId(3)).refusal == ModelRefusal::NotOneFace);
    }
}

TEST_CASE("wem tools turn refuses a concave quad", "[wem][geometry][tools]") {
    // A dart: its only cut is 1-3.
    Mesh mesh = fromFaces({{0, 1, 2, 3}}, {{0, 0, 0}, {2, 1, 0}, {4, 0, 0}, {2, 3, 0}});
    geom::MaterialiseRows(mesh);
    const ModelPlan plan = geom::PlanTurn(mesh, FaceId(0), VertexId(1), VertexId(3));
    CHECK(plan.refusal == ModelRefusal::ConcaveTurn);
}

TEST_CASE("wem tools edit triangulation adds a diagonal and re-cuts the crossed", "[wem][geometry][tools]") {
    // A regular hexagon cut as the fan from 0.
    std::vector<Vector3f> hexagon;
    for (u32 i = 0; i < 6; ++i) {
        const f32 a = static_cast<f32>(i) * 3.14159265f / 3.0f;
        hexagon.push_back(Vector3f{std::cos(a), std::sin(a), 0});
    }
    Mesh mesh = fromFaces({{0, 1, 2, 3, 4, 5}}, hexagon);
    mesh.triangulation.setRow(0, std::vector<u32>{0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5}, 1);
    ModelPlan plan = geom::PlanEditTriangulation(mesh, FaceId(0), VertexId(1), VertexId(4));
    finish(mesh, plan);
    const std::span<const u32> row = mesh.triangulation.row(0);
    std::set<std::pair<u32, u32>> sides;
    for (std::size_t t = 0; t + 2 < row.size(); t += 3) {
        for (u32 s = 0; s < 3; ++s) {
            const u32 a = row[t + s];
            const u32 b = row[t + (s + 1) % 3];
            sides.insert({std::min(a, b), std::max(a, b)});
        }
    }
    CHECK(sides.count({1, 4}) == 1u);
    CHECK(sides.count({0, 4}) == 1u); // 0-4 does not cross 1-4: kept
    CHECK(sides.count({0, 2}) == 0u); // 0-2 does: re-cut
    CHECK(sides.count({0, 3}) == 0u);

    SECTION("its refusals") {
        CHECK(geom::PlanEditTriangulation(mesh, FaceId(0), VertexId(0), VertexId(1)).refusal ==
              ModelRefusal::AdjacentCorners);
        CHECK(geom::PlanEditTriangulation(mesh, FaceId(0), VertexId(0), VertexId(9)).refusal ==
              ModelRefusal::NotOneFace);
    }
}

TEST_CASE("wem tools edit triangulation refuses a diagonal outside the face", "[wem][geometry][tools]") {
    // An L: the chord from the outer corner (0) across the notch to (4) leaves it.
    Mesh mesh = fromFaces({{0, 1, 2, 3, 4, 5}},
                          {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}, {1, 2, 0}, {0, 2, 0}});
    geom::MaterialiseRows(mesh);
    CHECK(geom::PlanEditTriangulation(mesh, FaceId(0), VertexId(2), VertexId(4)).refusal ==
          ModelRefusal::DiagonalOutside);
}

TEST_CASE("wem tools retriangulate stores the automatic cut", "[wem][geometry][tools]") {
    Mesh mesh = fromFaces({{0, 1, 2, 3}}, {{0, 0, 0}, {2, -0.5f, 0}, {4, 0, 0}, {2, 0.5f, 0}});
    // Stored along the long diagonal; the rule takes the short one.
    mesh.triangulation.setRow(0, std::vector<u32>{0, 1, 2, 0, 2, 3}, 1);
    ElementSet faces;
    faces.faces = {0};
    ModelPlan plan = geom::PlanRetriangulate(mesh, faces);
    CHECK_FALSE(plan.renumbers);
    finish(mesh, plan);
    CHECK(std::vector<u32>(mesh.triangulation.row(0).begin(), mesh.triangulation.row(0).end()) ==
          std::vector<u32>{1, 2, 3, 1, 3, 0});
}

TEST_CASE("wem tools triangulate splits faces into their drawn triangles", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, true);
    const std::vector<Triangle> before = drawn(mesh);
    ModelPlan plan = geom::PlanTriangulate(mesh, allFaces(mesh));
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 4u);
    CHECK(plan.selection.faces.size() == 4u);
    // The same triangles, as sets of corners: the emission order and the
    // first corners are the split's.
    const auto unordered = [](std::vector<Triangle> triangles) {
        for (Triangle& t : triangles) {
            std::sort(t.begin(), t.end());
        }
        std::sort(triangles.begin(), triangles.end());
        return triangles;
    };
    CHECK(unordered(drawn(mesh)) == unordered(before));

    SECTION("a diagonal that is already an edge keeps its face") {
        Mesh capped = fromFaces({{0, 1, 2}, {0, 2, 3}, {0, 3, 2, 1}},
                                {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}});
        ElementSet cap;
        cap.faces = {2};
        ModelPlan blocked = geom::PlanTriangulate(capped, cap);
        CHECK(blocked.kept == 1u);
        finish(capped, blocked);
        CHECK(liveFaces(capped) == 3u);
    }
}

// ============================================================================
// Target Weld and Weld
// ============================================================================

TEST_CASE("wem tools target weld along an edge collapses onto the target", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 2, true);
    const VertexId source = vertexAt(mesh, 1, 1);
    const VertexId target = vertexAt(mesh, 2, 1);
    ModelPlan plan = geom::PlanTargetWeld(mesh, pointsOf(mesh), source, target);
    finish(mesh, plan);
    CHECK(mesh.vertexCount() == 8u);
    // The target kept its position.
    REQUIRE(plan.selection.vertices.size() == 1u);
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    CHECK(p[plan.selection.vertices[0]].x == 2.0f);
    CHECK(p[plan.selection.vertices[0]].y == 1.0f);
}

TEST_CASE("wem tools target weld closes a notch", "[wem][geometry][tools]") {
    // An L of three quads: the missing cell's two border edges meet at x = (1, 1).
    Mesh mesh = fromFaces({{0, 1, 4, 3}, {1, 2, 5, 4}, {3, 4, 7, 6}},
                          {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0},
                           {0, 2, 0}, {1, 2, 0}});
    ModelPlan plan = geom::PlanTargetWeld(mesh, pointsOf(mesh), vertexAt(mesh, 2, 1), vertexAt(mesh, 1, 2));
    finish(mesh, plan);
    CHECK(mesh.vertexCount() == 7u);
    CHECK(liveFaces(mesh) == 3u);
}

TEST_CASE("wem tools target weld refuses the two-face V-notch", "[wem][geometry][tools]") {
    // (x, u, z) and (x, z, v): welding u onto v would make a pillow.
    Mesh mesh = fromFaces({{0, 1, 2}, {0, 2, 3}}, {{0, 0, 0}, {-1, 2, 0}, {0, 2, 0}, {1, 2, 0}});
    const ModelPlan plan = geom::PlanTargetWeld(mesh, pointsOf(mesh), VertexId(1), VertexId(3));
    CHECK(plan.refusal == ModelRefusal::WouldPinch);
}

TEST_CASE("wem tools target weld refuses what it cannot do in one step", "[wem][geometry][tools]") {
    SECTION("a crack's two sides") {
        // Two quads across a gap: border vertices, no edge, no shared loop.
        Mesh mesh = fromFaces({{0, 1, 2, 3}, {4, 5, 6, 7}},
                              {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                               {1.5f, 0, 0}, {2.5f, 0, 0}, {2.5f, 1, 0}, {1.5f, 1, 0}});
        CHECK(geom::PlanTargetWeld(mesh, pointsOf(mesh), VertexId(1), VertexId(4)).refusal ==
              ModelRefusal::CrackNeedsWeld);
    }
    SECTION("two interior points with no edge") {
        Mesh mesh = grid(3, 3, true);
        CHECK(geom::PlanTargetWeld(mesh, pointsOf(mesh), vertexAt(mesh, 1, 1), vertexAt(mesh, 2, 2)).refusal ==
              ModelRefusal::NotAdjacent);
    }
    SECTION("a collapse the link condition forbids") {
        // Two border corners across the square's diagonal: an interior edge
        // between border vertices pinches.
        Mesh mesh = grid(1, 1);
        CHECK(geom::PlanTargetWeld(mesh, pointsOf(mesh), VertexId(0), VertexId(3)).refusal ==
              ModelRefusal::TargetCollapseIllegal);
    }
}

TEST_CASE("wem tools target weld pairs two border edges across a crack", "[wem][geometry][tools]") {
    // Two quads whose shared column is held twice: facing across the crack,
    // the border edges run opposite ways, so from goes onto to and to onto from.
    Mesh mesh = fromFaces({{0, 1, 2, 3}, {4, 5, 6, 7}},
                          {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                           {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}});
    ModelPlan plan = geom::PlanTargetWeldEdges(mesh, pointsOf(mesh), edgeBetween(mesh, VertexId(1), VertexId(2)),
                                               edgeBetween(mesh, VertexId(4), VertexId(7)));
    finish(mesh, plan);
    CHECK(mesh.vertexCount() == 6u);
    u32 interior = 0;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        interior += mesh.topology().isBoundary(EdgeId(e)) ? 0u : 1u;
    }
    CHECK(interior == 1u);
    // Paired by position: (1, 0) with (1, 0), (1, 1) with (1, 1).
    CHECK(mesh.topology().findHalfedge(vertexAt(mesh, 1, 0), vertexAt(mesh, 1, 1)).valid());
}

TEST_CASE("wem tools target weld closes a slit's last edge", "[wem][geometry][tools]") {
    // A 2 x 2 grid slit from its middle vertex up to the top border: (1, 2)
    // is held twice, (1, 1) once. The last edge closes it.
    Mesh mesh = fromFaces({{0, 1, 4, 3}, {1, 2, 5, 4}, {3, 4, 7, 6}, {4, 5, 8, 9}},
                          {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0},
                           {0, 2, 0}, {1, 2, 0}, {2, 2, 0}, {1, 2, 0}});
    ModelPlan plan = geom::PlanTargetWeldEdges(mesh, pointsOf(mesh), edgeBetween(mesh, VertexId(4), VertexId(7)),
                                               edgeBetween(mesh, VertexId(4), VertexId(9)));
    finish(mesh, plan);
    CHECK(mesh.vertexCount() == 9u);
    u32 interior = 0;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        interior += mesh.topology().isBoundary(EdgeId(e)) ? 0u : 1u;
    }
    CHECK(interior == 4u);
}

TEST_CASE("wem tools weld keeps the most central vertex at the centroid", "[wem][geometry][tools]") {
    // Three triangles round a corner, each with its own copy of it.
    const f32 d = 1e-3f;
    Mesh mesh = fromFaces({{0, 1, 2}, {3, 4, 5}, {6, 7, 8}},
                          {{0, 0, 0}, {2, 0, 0}, {1.4f, 1.4f, 0},
                           {d, 0, 0}, {1.4f, 1.4f, 0}, {0, 2, 0},
                           {0, d, 0}, {0, 2, 0}, {-1.4f, 1.4f, 0}});
    mesh.skin.reset(0);
    for (u32 v = 0; v < 9; ++v) {
        mesh.skin.appendVertex(std::vector<geom::Influence>{geom::Influence{v, 1.0f}});
    }
    ElementSet all;
    all.vertices = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    ModelPlan plan = geom::PlanWeld(mesh, pointsOf(mesh), all, 0.01f);
    finish(mesh, plan);
    CHECK(mesh.vertexCount() == 5u);
    const VertexId centre = nearest(mesh, d / 3.0f, d / 3.0f);
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    CHECK(std::abs(p[centre.value()].x - d / 3.0f) < 1e-7f);
    CHECK(std::abs(p[centre.value()].y - d / 3.0f) < 1e-7f);
    // Vertex 0 was the nearest the centroid: its skin stays.
    REQUIRE(mesh.skin.forVertex(centre.value()).size() == 1u);
    CHECK(mesh.skin.forVertex(centre.value())[0].bone == 0u);
}

TEST_CASE("wem tools weld collapses a polygon's two corners into one", "[wem][geometry][tools]") {
    Mesh mesh = fromFaces({{0, 1, 2, 3}}, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1.001f, 0}});
    ElementSet pair;
    pair.vertices = {2, 3};
    ModelPlan plan = geom::PlanWeld(mesh, pointsOf(mesh), pair, 0.01f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 1u);
    CHECK(mesh.topology().valence(FaceId(0)) == 3u);
}

TEST_CASE("wem tools weld merges a point's members, and onto keeps a moved crease apart",
          "[wem][geometry][tools]") {
    // Two quads whose shared column is held twice, bound differently: the
    // prepare's crease.
    const auto build = [] {
        Mesh mesh = fromFaces({{0, 1, 2, 3}, {4, 5, 6, 7}},
                              {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                               {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}});
        mesh.skin.reset(0);
        for (u32 v = 0; v < 8; ++v) {
            mesh.skin.appendVertex(std::vector<geom::Influence>{geom::Influence{v < 4 ? 0u : 1u, 1.0f}});
        }
        return mesh;
    };
    SECTION("Weld is an explicit request: the members merge") {
        // Both points of the crease, each by one member: every member merges,
        // though they were bound differently, and the crease closes.
        Mesh mesh = build();
        ElementSet two;
        two.vertices = {1, 2};
        ModelPlan plan = geom::PlanWeld(mesh, pointsOf(mesh), two, 0.0f);
        finish(mesh, plan);
        CHECK(mesh.vertexCount() == 6u);
    }
    SECTION("but not into a pinch") {
        // One point alone would leave the two quads touching at a vertex.
        Mesh mesh = build();
        ElementSet one;
        one.vertices = {1};
        ModelPlan plan = geom::PlanWeld(mesh, pointsOf(mesh), one, 0.0f);
        finish(mesh, plan);
        CHECK(mesh.vertexCount() == 8u);
        CHECK(plan.kept == 1u);
    }
    SECTION("onto: the crease's twins never merge with each other") {
        // The crease (2 and 7) moved as one point onto vertex 1, snapped there:
        // one twin merges onto it, the one bound alike; the other stays apart.
        Mesh mesh = build();
        mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex)[1] = Vector3f{1, 1, 0};
        ElementSet sources;
        sources.vertices = {2, 7};
        ElementSet targets;
        targets.vertices = {1};
        ModelPlan plan = geom::PlanWeld(mesh, pointsOf(mesh), sources, targets, 0.01f);
        finish(mesh, plan);
        CHECK(plan.changed == 1u);
        CHECK(mesh.vertexCount() == 7u);
    }
}

// ============================================================================
// G22's To Quads arm, and the HD footman (the corpus)
// ============================================================================

namespace {

/// Each drawn triangle as its corners' bytes in drawing order: the position
/// and every Halfedge layer at the corner.
std::vector<std::vector<u8>> drawnCorners(const Mesh& mesh) {
    std::vector<u32> triangles;
    std::vector<u32> faceOf;
    geom::TriangulateMesh(mesh, triangles, &faceOf);
    const Topology& topology = mesh.topology();
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    std::vector<std::vector<u8>> out;
    for (std::size_t t = 0; t < faceOf.size(); ++t) {
        std::vector<u8>& key = out.emplace_back();
        for (u32 k = 0; k < 3; ++k) {
            const u32 vertex = triangles[3 * t + k];
            const u8* at = reinterpret_cast<const u8*>(&p[vertex]);
            key.insert(key.end(), at, at + sizeof(Vector3f));
            for (const HalfedgeId h : topology.fh(FaceId(faceOf[t]))) {
                if (topology.from(h).value() != vertex) {
                    continue;
                }
                for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
                    if (layer.domain == Domain::Halfedge) {
                        const std::size_t stride = geom::AttrTypeSize(layer.type);
                        key.insert(key.end(), layer.data.data() + stride * h.index(),
                                   layer.data.data() + stride * (h.index() + 1));
                    }
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Each exported triangle of @p model as its corners' position, normal and
/// first UV, in the file's order within the triangle, sorted.
std::vector<std::array<f32, 24>> exported(const mdx::Model& model) {
    std::vector<std::array<f32, 24>> out;
    for (const mdx::Geoset& geoset : model.geosets) {
        for (std::size_t t = 0; t + 2 < geoset.faces.size(); t += 3) {
            std::array<f32, 24> key{};
            for (u32 k = 0; k < 3; ++k) {
                const u32 v = geoset.faces[t + k];
                const Vector3f& p = geoset.vertexPositions[v];
                const Vector3f& n = geoset.vertexNormals[v];
                const Vector2f uv = geoset.textureCoordinateSets.empty()
                                        ? Vector2f{0, 0}
                                        : geoset.textureCoordinateSets[0][v];
                const std::array<f32, 8> corner{p.x, p.y, p.z, n.x, n.y, n.z, uv.x, uv.y};
                std::copy(corner.begin(), corner.end(), key.begin() + 8 * k);
            }
            out.push_back(key);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

enum class CorpusSet { ShippedSd, ShippedHd, Community };

/// R§1.2's three sets, as its probe drew them.
std::vector<std::filesystem::path> corpusSet(CorpusSet set) {
    std::vector<std::filesystem::path> out;
    u32 hdSeen = 0;
    for (const std::filesystem::path& file : test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL"})) {
        const std::string path = test::pathText(file);
        const bool shipped = path.find("war3.w3mod") != std::string::npos;
        const bool hd = path.find("_hd.w3mod") != std::string::npos;
        if (set == CorpusSet::ShippedSd && shipped && !hd) {
            out.push_back(file);
        } else if (set == CorpusSet::ShippedHd && hd && path.find("portrait") == std::string::npos) {
            if (hdSeen++ % 6 == 0) {
                out.push_back(file);
            }
        } else if (set == CorpusSet::Community && !shipped && path.find("Chinese_variant") == std::string::npos &&
                   !test::isKnownBad(file)) {
            out.push_back(file);
        }
    }
    return out;
}

std::optional<Document> importMdx(const std::filesystem::path& file) {
    const std::vector<u8> bytes = test::readCorpusFile(file);
    if (bytes.empty()) {
        return std::nullopt;
    }
    mdx::Parser parser;
    const mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    const MdxConverter converter;
    Result<Document> document = converter.fromMdx(source);
    if (!document.ok() || document->models.empty()) {
        return std::nullopt;
    }
    return std::move(*document.value);
}

} // namespace

TEST_CASE("wem tools to quads over the corpus sets keeps every drawn triangle",
          "[wem][geometry][tools][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    for (const auto& [set, name] : {std::pair{CorpusSet::ShippedSd, "shipped SD"},
                                    std::pair{CorpusSet::ShippedHd, "shipped HD, LOD 0"},
                                    std::pair{CorpusSet::Community, "community"}}) {
        const std::vector<std::filesystem::path> chosen = corpusSet(set);
        const std::size_t limit = test::sweepLimit(chosen.size(), 25);
        u64 triangles = 0;
        u64 joined = 0;
        u32 meshes = 0;
        std::vector<std::string> failing;
        for (std::size_t i = 0; i < limit; ++i) {
            test::trace(chosen[i]);
            std::optional<Document> document = importMdx(chosen[i]);
            if (!document) {
                continue;
            }
            for (Mesh& mesh : document->models[0].meshes) {
                geom::PrepareForModelling(mesh);
                const std::vector<std::vector<u8>> before = drawnCorners(mesh);
                ModelPlan plan = geom::PlanJoinTriangles(mesh, pointsOf(mesh), allFaces(mesh));
                if (plan.refused()) {
                    continue;
                }
                geom::FinishTool(mesh, plan);
                ++meshes;
                u64 triangleCount = 0;
                for (const u32 valence : mesh.faceSet().faceValence) {
                    triangleCount += valence - 2;
                }
                triangles += triangleCount;
                joined += 2 * plan.changed;
                std::string broken;
                if (drawnCorners(mesh) != before) {
                    broken = "the drawn triangles moved";
                } else {
                    broken = errors(mesh);
                }
                if (!broken.empty() && failing.size() < 10) {
                    failing.push_back(test::pathText(chosen[i].filename()) + " '" + mesh.name + "': " + broken);
                }
            }
        }
        for (const std::string& line : failing) {
            std::cout << "  " << line << "\n";
        }
        const f64 share = triangles == 0 ? 0.0 : 100.0 * static_cast<f64>(joined) / static_cast<f64>(triangles);
        std::cout << "to quads, " << name << ": " << limit << " files, " << meshes << " meshes, " << joined
                  << " of " << triangles << " triangles joined (" << share << " %)\n";
        CAPTURE(name);
        CHECK(failing.empty());
    }
}

TEST_CASE("wem tools to quads on the HD footman exports the file's triangles",
          "[wem][geometry][tools][corpus]") {
    std::filesystem::path footman;
    for (const std::filesystem::path& file : test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL"})) {
        if (test::pathText(file).ends_with("_hd.w3mod/units/human/footman/footman.mdx")) {
            footman = file;
            break;
        }
    }
    if (footman.empty()) {
        SKIP("the HD footman is not in the corpus");
    }
    std::optional<Document> document = importMdx(footman);
    REQUIRE(document.has_value());
    const auto started = std::chrono::steady_clock::now();
    for (Mesh& mesh : document->models[0].meshes) {
        geom::PrepareForModelling(mesh);
    }
    const f64 prepareSeconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
    const MdxConverter converter;
    const Result<mdx::Model> before = converter.toMdx(*document, ProfileId::Wc3Reforged);
    REQUIRE(before.ok());
    u64 triangles = 0;
    u64 joined = 0;
    for (Mesh& mesh : document->models[0].meshes) {
        for (const u32 valence : mesh.faceSet().faceValence) {
            triangles += valence - 2;
        }
        ModelPlan plan = geom::PlanJoinTriangles(mesh, pointsOf(mesh), allFaces(mesh));
        REQUIRE_FALSE(plan.refused());
        geom::FinishTool(mesh, plan);
        joined += 2 * plan.changed;
        CHECK(errors(mesh) == "");
    }
    const Result<mdx::Model> after = converter.toMdx(*document, ProfileId::Wc3Reforged);
    REQUIRE(after.ok());
    CHECK(exported(*after.value) == exported(*before.value));
    const f64 share = 100.0 * static_cast<f64>(joined) / static_cast<f64>(triangles);
    std::cout << "HD footman: prepare " << prepareSeconds * 1000.0 << " ms; to quads joins " << joined << " of "
              << triangles << " triangles (" << share << " %)\n";
    // R 2.2 measured 76.0 % over the HD set at LOD 0, with UV and normal
    // delimits only; the full delimit joins a little fewer.
    CHECK(share > 60.0);
}

// ============================================================================
// Collapse, Remove, Delete, Cap, Flip, Hard and Soft (§3.7, §3.10)
// ============================================================================

namespace {

/// @p mesh with a skin: every vertex bound to bone 0, except @p second's
/// vertices, bound to bone 1.
void bindMesh(Mesh& mesh, const std::vector<u32>& second) {
    mesh.skin.reset(mesh.vertexCount());
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        const bool other = std::find(second.begin(), second.end(), v) != second.end();
        const geom::Influence one{other ? 1u : 0u, 1.0f};
        mesh.skin.assignVertex(v, std::span<const geom::Influence>(&one, 1));
    }
}

/// A UV set over @p mesh: each corner takes its vertex's x,y, with @p shifted
/// corners moved by (10, 0) -- a second island.
void mapUvs(Mesh& mesh, const std::vector<u32>& shiftedFaces = {}) {
    const std::span<Vector2f> uv =
        mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const Topology& topology = mesh.topology();
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        const bool shifted = std::find(shiftedFaces.begin(), shiftedFaces.end(), f) != shiftedFaces.end();
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            const Vector3f& at = p[topology.from(h).value()];
            uv[h.index()] = Vector2f{at.x + (shifted ? 10.0f : 0.0f), at.y};
        }
    }
}

std::span<const geom::Influence> influencesAt(const Mesh& mesh, f32 x, f32 y) {
    return mesh.skin.forVertex(vertexAt(mesh, x, y).value());
}

} // namespace

TEST_CASE("wem tools collapse takes a part to its centroid with blended skin", "[wem][geometry][tools]") {
    // The middle four vertices of a 3 x 3 quad grid: one vertex at their
    // centre, bound to both bones at half each.
    Mesh mesh = grid(3, 3, /*quads=*/true);
    bindMesh(mesh, {vertexAt(mesh, 1, 1).value(), vertexAt(mesh, 2, 1).value()});
    mapUvs(mesh);
    const u32 before = mesh.vertexCount();
    ElementSet selection;
    for (const std::pair<f32, f32> at : {std::pair<f32, f32>{1, 1}, {2, 1}, {1, 2}, {2, 2}}) {
        selection.vertices.push_back(vertexAt(mesh, at.first, at.second).value());
    }
    selection.normalise();
    ModelPlan plan = geom::PlanCollapse(mesh, pointsOf(mesh), selection);
    finish(mesh, plan);
    CHECK(mesh.vertexCount() == before - 3);
    const VertexId centre = vertexAt(mesh, 1.5f, 1.5f);
    const std::span<const geom::Influence> blended = mesh.skin.forVertex(centre.value());
    REQUIRE(blended.size() == 2u);
    CHECK(blended[0].weight == 0.5f);
    CHECK(blended[1].weight == 0.5f);
    // Its corners took their island's UV centre: one island here, so one value.
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    std::vector<Vector2f> corners;
    for (const HalfedgeId h : mesh.topology().voh(centre)) {
        if (!mesh.topology().isBoundary(h)) {
            corners.push_back(uv[h.index()]);
        }
    }
    REQUIRE_FALSE(corners.empty());
    for (const Vector2f& value : corners) {
        CHECK(value == corners.front());
    }
    CHECK(corners.front().x == 1.5f);
}

TEST_CASE("wem tools collapse keeps a UV seam apart", "[wem][geometry][tools]") {
    // A 2 x 2 grid whose right column is on its own UV island: two vertices
    // of the seam collapse into one, whose corners keep two UV values.
    Mesh mesh = grid(2, 2, /*quads=*/true);
    mapUvs(mesh, {1, 3});
    ElementSet selection;
    selection.vertices = {vertexAt(mesh, 1, 0).value(), vertexAt(mesh, 1, 1).value()};
    selection.normalise();
    ModelPlan plan = geom::PlanCollapse(mesh, pointsOf(mesh), selection);
    finish(mesh, plan);
    const VertexId centre = vertexAt(mesh, 1.0f, 0.5f);
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    std::set<std::pair<f32, f32>> values;
    for (const HalfedgeId h : mesh.topology().voh(centre)) {
        if (!mesh.topology().isBoundary(h)) {
            values.insert({uv[h.index()].x, uv[h.index()].y});
        }
    }
    CHECK(values.size() == 2u); // one per island
}

TEST_CASE("wem tools remove closes the surface over a vertex", "[wem][geometry][tools]") {
    // The middle vertex of a 2 x 2 grid: the four quads around it become one.
    Mesh mesh = grid(2, 2, /*quads=*/true);
    ElementSet selection;
    selection.vertices = {vertexAt(mesh, 1, 1).value()};
    ModelPlan plan = geom::PlanRemove(mesh, pointsOf(mesh), selection);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 1u);
    CHECK(mesh.topology().valence(FaceId(0)) == 8u); // the ring, corners and all
}

TEST_CASE("wem tools remove dissolves an edge and cleans what it left", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    const VertexId top = vertexAt(mesh, 1, 1), bottom = vertexAt(mesh, 1, 0);
    ElementSet selection;
    selection.edges = {edgeBetween(mesh, bottom, top).value()};
    SECTION("the two quads become one, its corners kept") {
        ModelPlan plan = geom::PlanRemove(mesh, pointsOf(mesh), selection);
        finish(mesh, plan);
        CHECK(liveFaces(mesh) == 1u);
        CHECK(mesh.topology().valence(FaceId(0)) == 6u); // the valence-2 corners stay
    }
    SECTION("Clean vertices takes the valence-2 corners with it") {
        geom::RemoveParams params;
        params.cleanVertices = true;
        ModelPlan plan = geom::PlanRemove(mesh, pointsOf(mesh), selection, params);
        finish(mesh, plan);
        CHECK(liveFaces(mesh) == 1u);
        CHECK(mesh.topology().valence(FaceId(0)) == 4u);
        CHECK(mesh.vertexCount() == 4u);
    }
}

TEST_CASE("wem tools remove refuses a border edge and a section crossing", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    SECTION("a border edge") {
        ElementSet selection;
        selection.edges = {edgeBetween(mesh, vertexAt(mesh, 0, 0), vertexAt(mesh, 1, 0)).value()};
        const ModelPlan plan = geom::PlanRemove(mesh, pointsOf(mesh), selection);
        CHECK(plan.refusal == ModelRefusal::BorderEdgeRemove);
        CHECK(liveFaces(mesh) == 2u); // nothing written
    }
    SECTION("two sections") {
        mesh.sections.emplace_back();
        mesh.faceSections()[1] = 1;
        ElementSet selection;
        selection.edges = {edgeBetween(mesh, vertexAt(mesh, 1, 0), vertexAt(mesh, 1, 1)).value()};
        const ModelPlan plan = geom::PlanRemove(mesh, pointsOf(mesh), selection);
        CHECK(plan.refusal == ModelRefusal::CrossesSection);
        CHECK(liveFaces(mesh) == 2u);
    }
}

TEST_CASE("wem tools delete splits the vertex it pinched", "[wem][geometry][tools]") {
    // Two quads of a 2 x 2 grid, diagonally opposite: deleting the other two
    // leaves them meeting at one vertex, which the repair splits in two, and
    // the twins keep the original's merge group.
    Mesh mesh = grid(2, 2, /*quads=*/true);
    const u32 at = vertexAt(mesh, 1, 1).value();
    const std::span<u32> groups =
        mesh.attributes.getOrCreate<u32>(geom::names::kMergeGroup, Domain::Vertex, geom::AttrType::U32);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        groups[v] = v;
    }
    const u32 group = groups[at];
    ElementSet selection;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        std::vector<f32> xs;
        for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
            xs.push_back(mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex)
                             [mesh.topology().from(h).value()]
                                 .x);
        }
        // The two quads on the diagonal from (0,0) to (2,2) stay.
        const bool left = *std::min_element(xs.begin(), xs.end()) == 0.0f;
        std::vector<f32> ys;
        for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
            ys.push_back(mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex)
                             [mesh.topology().from(h).value()]
                                 .y);
        }
        const bool low = *std::min_element(ys.begin(), ys.end()) == 0.0f;
        if (left != low) {
            selection.faces.push_back(f);
        }
    }
    selection.normalise();
    REQUIRE(selection.faces.size() == 2u);
    ModelPlan plan = geom::PlanDelete(mesh, selection);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 2u);
    // Two vertices at (1,1) now, both in the original's group.
    u32 twins = 0;
    const auto places = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const auto after = mesh.attributes.get<const u32>(geom::names::kMergeGroup, Domain::Vertex);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (places[v].x == 1.0f && places[v].y == 1.0f) {
            ++twins;
            CHECK(after[v] == group);
        }
    }
    CHECK(twins == 2u);
}

TEST_CASE("wem tools delete refuses every face of a mesh", "[wem][geometry][tools]") {
    Mesh mesh = grid(1, 1, /*quads=*/true);
    const ModelPlan plan = geom::PlanDelete(mesh, allFaces(mesh));
    CHECK(plan.refusal == ModelRefusal::AllFaces);
    CHECK(liveFaces(mesh) == 1u);
}

TEST_CASE("wem tools cap closes a loop and copies its rim", "[wem][geometry][tools]") {
    // A 2 x 2 grid with its middle quad deleted: the hole is capped, and the
    // cap's corners carry the rim's UVs.
    Mesh mesh = grid(3, 3, /*quads=*/true);
    mapUvs(mesh);
    ElementSet middle;
    middle.faces = {4};
    ModelPlan hole = geom::PlanDelete(mesh, middle);
    finish(mesh, hole);
    REQUIRE(liveFaces(mesh) == 8u);
    // A border edge of the hole.
    EdgeId rim;
    const Topology& topology = mesh.topology();
    for (u32 e = 0; e < topology.edgeCount() && !rim.valid(); ++e) {
        if (topology.isDeleted(EdgeId(e)) || !topology.isBoundary(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
        const Vector3f& a = p[topology.from(h).value()];
        const Vector3f& b = p[topology.to(h).value()];
        if (a.x >= 1.0f && a.x <= 2.0f && a.y >= 1.0f && a.y <= 2.0f && b.x >= 1.0f && b.x <= 2.0f &&
            b.y >= 1.0f && b.y <= 2.0f) {
            rim = EdgeId(e);
        }
    }
    REQUIRE(rim.valid());
    ModelPlan plan = geom::PlanCap(mesh, pointsOf(mesh), rim);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 9u);
    // The cap's corners are the rim's: each UV is its vertex's own place.
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const FaceId cap(plan.selection.faces.front());
    // And the finish materialised its row: a quad the file must draw as two
    // triangles, not left to whatever reads it next (§2.3).
    CHECK(mesh.triangulation.row(cap.value()).size() == 6u);
    for (const HalfedgeId h : mesh.topology().fh(cap)) {
        const Vector3f& at = p[mesh.topology().from(h).value()];
        CHECK(uv[h.index()].x == at.x);
        CHECK(uv[h.index()].y == at.y);
    }
}

TEST_CASE("wem tools cap refuses a card and a short loop", "[wem][geometry][tools]") {
    SECTION("a lone card would become a pillow") {
        Mesh mesh = grid(1, 1, /*quads=*/true);
        const EdgeId rim = edgeBetween(mesh, vertexAt(mesh, 0, 0), vertexAt(mesh, 1, 0));
        const ModelPlan plan = geom::PlanCap(mesh, pointsOf(mesh), rim);
        CHECK(plan.refusal == ModelRefusal::WouldDuplicateFace);
        CHECK(liveFaces(mesh) == 1u);
    }
}

TEST_CASE("wem tools flip reverses a region and detaches it cleanly", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    const std::span<Vector3f> normals =
        mesh.attributes.getOrCreate<Vector3f>(geom::names::kNormal, Domain::Halfedge, geom::AttrType::F32x3);
    for (Vector3f& n : normals) {
        n = Vector3f{0, 0, 1};
    }
    const std::span<Vector4f> tangents =
        mesh.attributes.getOrCreate<Vector4f>(geom::names::kTangent, Domain::Halfedge, geom::AttrType::F32x4);
    for (Vector4f& t : tangents) {
        t = Vector4f{1, 0, 0, 1};
    }
    const std::vector<Triangle> before = drawn(mesh);
    ElementSet one;
    one.faces = {0};
    ModelPlan plan = geom::PlanFlip(mesh, one);
    finish(mesh, plan);
    // The region detached: the shared edge's two vertices are two each now.
    CHECK(mesh.vertexCount() == 8u);
    CHECK(liveFaces(mesh) == 2u);
    // Its corners face the other way, and the tangent's sign with them.
    const auto after = mesh.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    const auto tangentsAfter = mesh.attributes.get<const Vector4f>(geom::names::kTangent, Domain::Halfedge);
    const Topology& topology = mesh.topology();
    u32 reversed = 0;
    for (const HalfedgeId h : topology.fh(FaceId(0))) {
        CHECK(after[h.index()].z == -1.0f);
        CHECK(tangentsAfter[h.index()].w == -1.0f);
        ++reversed;
    }
    CHECK(reversed == 4u);
    for (const HalfedgeId h : topology.fh(FaceId(1))) {
        CHECK(after[h.index()].z == 1.0f);
        CHECK(tangentsAfter[h.index()].w == 1.0f);
    }
    // And the flipped face is drawn the other way round.
    const std::vector<Triangle> now = drawn(mesh);
    CHECK(now.size() == before.size());
    CHECK(now != before);
}

TEST_CASE("wem tools hard and soft set the flag and nothing else", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    const EdgeId middle = edgeBetween(mesh, vertexAt(mesh, 1, 0), vertexAt(mesh, 1, 1));
    const u32 vertices = mesh.vertexCount();
    ElementSet selection;
    selection.edges = {middle.value()};
    ModelPlan hard = geom::PlanSharpen(mesh, selection, true);
    finish(mesh, hard);
    CHECK(mesh.vertexCount() == vertices);
    CHECK(mesh.attributes.get<const u8>(geom::names::kSharp, Domain::Edge)[middle.value()] == 1);
    ModelPlan soft = geom::PlanSharpen(mesh, selection, false);
    finish(mesh, soft);
    CHECK(mesh.attributes.get<const u8>(geom::names::kSharp, Domain::Edge)[middle.value()] == 0);
    CHECK(mesh.vertexCount() == vertices);
}

// ============================================================================
// Extrude, Border Extrude and Inset (§3.8)
// ============================================================================

namespace {

/// The unit box: six quads, wound outward.
Mesh box() {
    const std::vector<Vector3f> places = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                          {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const std::vector<std::vector<u32>> faces = {
        {0, 3, 2, 1}, // -Z
        {4, 5, 6, 7}, // +Z
        {0, 1, 5, 4}, // -Y
        {1, 2, 6, 5}, // +X
        {2, 3, 7, 6}, // +Y
        {3, 0, 4, 7}, // -X
    };
    return fromFaces(faces, places);
}

/// The live face whose corners average to @p at.
u32 faceCentred(const Mesh& mesh, Vector3f at) {
    const Topology& topology = mesh.topology();
    const auto places = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 f = 0; f < topology.faceCount(); ++f) {
        if (topology.isDeleted(FaceId(f))) {
            continue;
        }
        Vector3f sum{0.0f, 0.0f, 0.0f};
        u32 corners = 0;
        for (const HalfedgeId h : topology.fh(FaceId(f))) {
            sum = sum + places[topology.from(h).value()];
            ++corners;
        }
        const Vector3f centre = sum * (1.0f / static_cast<f32>(corners));
        if ((centre - at).length() < 1e-4f) {
            return f;
        }
    }
    FAIL("no face there");
    return 0;
}

u8 sharpAt(const Mesh& mesh, VertexId a, VertexId b) {
    const EdgeId edge = edgeBetween(mesh, a, b);
    const auto sharp = mesh.attributes.get<const u8>(geom::names::kSharp, Domain::Edge);
    return edge.value() < sharp.size() ? sharp[edge.value()] : 0;
}

} // namespace

TEST_CASE("wem tools extrude lifts a region and walls it", "[wem][geometry][tools]") {
    // The middle quad of a 3 x 3 grid, lifted: four walls, four new vertices,
    // and the ground it left behind unmoved.
    Mesh mesh = grid(3, 3, /*quads=*/true);
    ElementSet selection;
    selection.faces = {faceCentred(mesh, {1.5f, 1.5f, 0.0f})};
    ModelPlan plan = geom::PlanExtrudeFaces(mesh, pointsOf(mesh), selection, geom::ExtrudeType::Group);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK_FALSE(plan.fellBack);
    CHECK(plan.motions.size() == 4u);
    geom::ApplyAmount(mesh, plan, 1.0f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 13u);
    CHECK(mesh.vertexCount() == 20u);
    // The lifted quad stands at z = 1 over the hole it left.
    for (const auto& [x, y] : std::vector<std::pair<f32, f32>>{{1, 1}, {2, 1}, {2, 2}, {1, 2}}) {
        CHECK(vertexAt(mesh, x, y, 1.0f).valid());
        CHECK(vertexAt(mesh, x, y, 0.0f).valid());
    }
    // §3.12's crease rule: a wall meets the ground and the lifted face at a
    // right angle, so both edges shade hard at the 60 degree default.
    CHECK(sharpAt(mesh, vertexAt(mesh, 1, 1, 0), vertexAt(mesh, 2, 1, 0)) == 1);
    CHECK(sharpAt(mesh, vertexAt(mesh, 1, 1, 1), vertexAt(mesh, 2, 1, 1)) == 1);
    // And an edge the tool left alone keeps the smooth shading a modelled mesh
    // has.
    CHECK(sharpAt(mesh, vertexAt(mesh, 0, 1, 0), vertexAt(mesh, 1, 1, 0)) == 0);
}

TEST_CASE("wem tools extrude group falls back where its normals cancel", "[wem][geometry][tools]") {
    // A band round the box: the four sides' normals cancel, so there is no
    // group direction and each vertex takes its own.
    Mesh mesh = box();
    ElementSet band;
    for (const Vector3f& at : {Vector3f{0.5f, 0.0f, 0.5f}, Vector3f{1.0f, 0.5f, 0.5f},
                               Vector3f{0.5f, 1.0f, 0.5f}, Vector3f{0.0f, 0.5f, 0.5f}}) {
        band.faces.push_back(faceCentred(mesh, at));
    }
    ModelPlan plan = geom::PlanExtrudeFaces(mesh, pointsOf(mesh), band, geom::ExtrudeType::Group);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.fellBack);
}

TEST_CASE("wem tools extrude group falls back on a flat wall", "[wem][geometry][tools]") {
    // The box's top and two opposite sides: the mean points up, and the
    // vertical edges of those sides lie along it, so a wall would have no area
    // at any height (§3.8).
    Mesh mesh = box();
    ElementSet three;
    for (const Vector3f& at : {Vector3f{0.5f, 0.5f, 1.0f}, Vector3f{1.0f, 0.5f, 0.5f},
                               Vector3f{0.0f, 0.5f, 0.5f}}) {
        three.faces.push_back(faceCentred(mesh, at));
    }
    ModelPlan plan = geom::PlanExtrudeFaces(mesh, pointsOf(mesh), three, geom::ExtrudeType::Group);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.fellBack);
}

TEST_CASE("wem tools extrude by polygon lifts each face on its own", "[wem][geometry][tools]") {
    // Two quads side by side: each is its own region, so the edge between them
    // grows a wall on both sides and the two rise apart.
    Mesh mesh = grid(2, 1, /*quads=*/true);
    ModelPlan plan =
        geom::PlanExtrudeFaces(mesh, pointsOf(mesh), allFaces(mesh), geom::ExtrudeType::ByPolygon);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.motions.size() == 8u); // four copies each
    geom::ApplyAmount(mesh, plan, 1.0f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 10u);     // two tops and eight walls
    CHECK(mesh.vertexCount() == 14u);  // six, and four copies each
}

TEST_CASE("wem tools extrude local normal averages the faces at a vertex", "[wem][geometry][tools]") {
    // A bend of two quads, both extruded: the vertices on the shared edge move
    // along the mean of the two normals, the others along their own face's.
    const std::vector<Vector3f> places = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                          {1, 0, 1}, {1, 1, 1}};
    Mesh mesh = fromFaces({{0, 1, 2, 3}, {1, 4, 5, 2}}, places);
    ModelPlan plan =
        geom::PlanExtrudeFaces(mesh, pointsOf(mesh), allFaces(mesh), geom::ExtrudeType::LocalNormal);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    // The two faces look +Z and -X, so the shared edge's vertices aim between
    // them and the rest straight up or straight out.
    u32 mixed = 0, single = 0;
    for (const geom::VertexMotion& motion : plan.motions) {
        const Vector3f& d = motion.direction;
        if (std::abs(d.x + 0.7071068f) < 1e-4f && std::abs(d.z - 0.7071068f) < 1e-4f) {
            ++mixed;
        } else if (std::abs(d.z - 1.0f) < 1e-4f || std::abs(d.x + 1.0f) < 1e-4f) {
            ++single;
        }
    }
    CHECK(mixed == 2u);  // the two on the shared edge
    CHECK(single == 4u); // two on each far side
}

TEST_CASE("wem tools hinge swings a face about its edge and keeps the hinge attached",
          "[wem][geometry][tools]") {
    // One quad hinged on its bottom edge (§3.8): the hinge's two vertices are
    // pinned, the hinge edge grows no wall, its two neighbours are triangles,
    // the top edge's wall is a quad, and a quarter turn stands the face up
    // along its normal -- the far corners at z = 1, the hinge's untouched.
    Mesh mesh = grid(1, 1, /*quads=*/true);
    const auto placed = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    u32 bottom = kInvalidId;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        if (placed[std::as_const(mesh).topology().from(h).value()].y == 0.0f &&
            placed[std::as_const(mesh).topology().to(h).value()].y == 0.0f) {
            bottom = e;
        }
    }
    REQUIRE(bottom != kInvalidId);
    // An edge with both sides selected -- or neither -- is no hinge.
    {
        Mesh two = grid(2, 1, /*quads=*/true);
        const auto at = two.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
        u32 middle = kInvalidId;
        for (u32 e = 0; e < two.topology().edgeCount(); ++e) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            if (at[std::as_const(two).topology().from(h).value()].x == 1.0f &&
                at[std::as_const(two).topology().to(h).value()].x == 1.0f) {
                middle = e;
            }
        }
        REQUIRE(middle != kInvalidId);
        const ModelPlan refused = geom::PlanHingeFaces(two, pointsOf(two), allFaces(two), middle);
        CHECK(refused.refusal == ModelRefusal::HingeNotOnBoundary);
        CHECK(liveFaces(two) == 2u);
    }
    ModelPlan plan = geom::PlanHingeFaces(mesh, pointsOf(mesh), allFaces(mesh), bottom);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    REQUIRE(plan.hinge.has_value());
    CHECK(plan.motions.size() == 2u); // the two far corners; the hinge's two are pinned
    CHECK(plan.amountMax == FLT_MAX);
    CHECK(liveFaces(mesh) == 4u);     // the face, two triangles, one quad
    CHECK(mesh.vertexCount() == 6u);
    // At 0 every base goes back bit for bit; at 90 the far corners stand up.
    geom::ApplyAmount(mesh, plan, 0.0f);
    {
        const auto now = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
        for (const geom::VertexMotion& m : plan.motions) {
            CHECK(now[m.vertex] == m.base);
        }
    }
    geom::ApplyAmount(mesh, plan, 90.0f);
    finish(mesh, plan);
    const auto now = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    u32 standing = 0;
    u32 grounded = 0;
    for (u32 v = 0; v < now.size(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        if (now[v].z == Catch::Approx(1.0f).margin(1e-5)) {
            ++standing;
            CHECK(now[v].y == Catch::Approx(0.0f).margin(1e-5)); // over the hinge line
        } else {
            CHECK(now[v].z == 0.0f);
            ++grounded;
        }
    }
    CHECK(standing == 2u);
    CHECK(grounded == 4u);
}

TEST_CASE("wem tools hinge on a border run swings about its vertex", "[wem][geometry][tools]") {
    // A 2 x 1 grid's bottom run, hinged at its left end about the normal: the
    // hinge vertex is pinned, the quad beside it is a triangle, the other a
    // quad, and a quarter turn the other way lays the run down the y axis.
    Mesh mesh = grid(2, 1, /*quads=*/true);
    const auto placed = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    ElementSet run;
    u32 corner = kInvalidId;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const u32 a = std::as_const(mesh).topology().from(h).value();
        const u32 b = std::as_const(mesh).topology().to(h).value();
        if (placed[a].y == 0.0f && placed[b].y == 0.0f) {
            run.edges.push_back(e);
            if (placed[a].x == 0.0f) {
                corner = a;
            } else if (placed[b].x == 0.0f) {
                corner = b;
            }
        }
    }
    REQUIRE(run.edges.size() == 2u);
    REQUIRE(corner != kInvalidId);
    // A vertex off the run is no hinge, and refusing leaves the mesh alone.
    u32 elsewhere = kInvalidId;
    for (u32 v = 0; v < placed.size(); ++v) {
        if (placed[v].y == 1.0f && placed[v].x == 2.0f) {
            elsewhere = v;
        }
    }
    REQUIRE(elsewhere != kInvalidId);
    CHECK(geom::PlanHingeBorder(mesh, pointsOf(mesh), run, elsewhere, Vector3f{0.0f, 0.0f, 1.0f}).refusal ==
          ModelRefusal::HingeNotOnBoundary);
    CHECK(liveFaces(mesh) == 2u);
    ModelPlan plan = geom::PlanHingeBorder(mesh, pointsOf(mesh), run, corner, Vector3f{0.0f, 0.0f, 1.0f});
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    REQUIRE(plan.hinge.has_value());
    CHECK(plan.motions.size() == 2u); // the run's other two vertices, copied
    CHECK(liveFaces(mesh) == 4u);     // two faces, a triangle and a quad
    CHECK(mesh.vertexCount() == 8u);
    // The selection is the swung run: the copy's edge to the hinge, and the
    // far edge between the two copies -- not the side at the run's other end.
    REQUIRE(plan.selection.edges.size() == 2u);
    geom::ApplyAmount(mesh, plan, -90.0f);
    finish(mesh, plan);
    const auto now = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    u32 down = 0;
    for (u32 v = 0; v < now.size(); ++v) {
        if (!mesh.topology().isDeleted(VertexId(v)) && now[v].y < -1e-5f) {
            ++down;
            CHECK(now[v].x == Catch::Approx(0.0f).margin(1e-5)); // laid along the y axis
            CHECK(now[v].z == 0.0f);
        }
    }
    CHECK(down == 2u);
}

TEST_CASE("wem tools border extrude on an open run selects the far edges and not the ends",
          "[wem][geometry][tools]") {
    // A 2 x 1 grid of quads and its bottom two edges: two quads grow, and what
    // is selected is their two FAR edges -- the copies of what was given, both
    // ends at y = -1 -- not the four border edges the strip has. The side
    // edges at the run's two ends are border too, and selecting them would
    // send a second extrude sideways off the strip instead of on along it.
    Mesh mesh = grid(2, 1, /*quads=*/true);
    const auto placed = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    ElementSet run;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const u32 a = std::as_const(mesh).topology().from(h).value();
        const u32 b = std::as_const(mesh).topology().to(h).value();
        if (placed[a].y == 0.0f && placed[b].y == 0.0f) {
            run.edges.push_back(e);
        }
    }
    REQUIRE(run.edges.size() == 2u);
    ModelPlan plan = geom::PlanExtrudeBorder(mesh, pointsOf(mesh), run);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.motions.size() == 3u); // the run's three vertices, copied
    REQUIRE(plan.selection.edges.size() == 2u);
    geom::ApplyAmount(mesh, plan, 1.0f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 4u);
    const auto now = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (const u32 e : plan.selection.edges) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        CHECK(std::as_const(mesh).topology().isBoundary(EdgeId(e)));
        CHECK(now[std::as_const(mesh).topology().from(h).value()].y == Catch::Approx(-1.0f).margin(1e-5));
        CHECK(now[std::as_const(mesh).topology().to(h).value()].y == Catch::Approx(-1.0f).margin(1e-5));
    }
}

TEST_CASE("wem tools border extrude grows a strip and selects its far edge",
          "[wem][geometry][tools]") {
    // One quad's whole border: four quads around it, and the new border is what
    // the gizmo gets after Apply. The strip grows OUTWARD in the quad's own
    // plane, not up its normal, so what this makes is a bigger, flat ring.
    Mesh mesh = grid(1, 1, /*quads=*/true);
    ElementSet border;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (!mesh.topology().isDeleted(EdgeId(e)) && mesh.topology().isBoundary(EdgeId(e))) {
            border.edges.push_back(e);
        }
    }
    REQUIRE(border.edges.size() == 4u);
    ModelPlan plan = geom::PlanExtrudeBorder(mesh, pointsOf(mesh), border);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.motions.size() == 4u);
    CHECK(plan.selection.edges.size() == 4u);
    geom::ApplyAmount(mesh, plan, 1.0f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 5u);
    CHECK(mesh.vertexCount() == 8u);
    // Every corner belongs to two border edges meeting at a right angle, so its
    // aim is their bisector and it travels the whole amount along it: the unit
    // square becomes one of side 1 + 2/sqrt(2), and stays flat. Nothing rose,
    // which is the whole of what changed on 2026-09-24.
    const auto placed = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const f32 out = 1.0f / std::sqrt(2.0f);
    f32 low = 0.0f, high = 1.0f, lifted = 0.0f;
    for (u32 v = 0; v < placed.size(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        low = std::min(low, std::min(placed[v].x, placed[v].y));
        high = std::max(high, std::max(placed[v].x, placed[v].y));
        lifted = std::max(lifted, std::abs(placed[v].z));
    }
    CHECK(lifted == Catch::Approx(0.0f).margin(1e-5));      // not up the face's normal
    CHECK(low == Catch::Approx(-out).margin(1e-5));         // outward, in the face's plane
    CHECK(high == Catch::Approx(1.0f + out).margin(1e-5));
}

TEST_CASE("wem tools inset rings a face and offsets it evenly", "[wem][geometry][tools]") {
    // One quad inset by a quarter: a ring of four quads, and the inner face a
    // quarter in on every side, which is Blender's even offset.
    Mesh mesh = grid(1, 1, /*quads=*/true);
    mapUvs(mesh);
    ModelPlan plan = geom::PlanInset(mesh, pointsOf(mesh), allFaces(mesh), geom::InsetType::Group);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    REQUIRE(plan.motions.size() == 4u);
    // A right angle: the corner runs 1 / sin(45) as fast as the offset.
    for (const geom::VertexMotion& motion : plan.motions) {
        CHECK(motion.direction.length() == Catch::Approx(1.4142136f).margin(1e-4));
    }
    geom::ApplyAmount(mesh, plan, 0.25f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 5u);
    CHECK(mesh.vertexCount() == 8u);
    for (const auto& [x, y] : std::vector<std::pair<f32, f32>>{{0.25f, 0.25f}, {0.75f, 0.25f},
                                                               {0.75f, 0.75f}, {0.25f, 0.75f}}) {
        CHECK(nearest(mesh, x, y).valid());
        const Vector3f at = mesh.attributes.get<const Vector3f>(geom::names::kPosition,
                                                                Domain::Vertex)[nearest(mesh, x, y).value()];
        CHECK(at.x == Catch::Approx(x).margin(1e-4));
        CHECK(at.y == Catch::Approx(y).margin(1e-4));
    }
    // §2.4: the inner corners were re-sampled from the face they moved inside,
    // so the texture stayed where it was on the surface.
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    const auto places = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const Topology& topology = mesh.topology();
    const u32 inner = faceCentred(mesh, {0.5f, 0.5f, 0.0f});
    for (const HalfedgeId h : topology.fh(FaceId(inner))) {
        const Vector3f& at = places[topology.from(h).value()];
        CHECK(uv[h.index()].x == Catch::Approx(at.x).margin(1e-4));
        CHECK(uv[h.index()].y == Catch::Approx(at.y).margin(1e-4));
    }
}

TEST_CASE("wem tools inset by polygon rings each face on its own", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    ModelPlan plan = geom::PlanInset(mesh, pointsOf(mesh), allFaces(mesh), geom::InsetType::ByPolygon);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(mesh, plan, 0.2f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 10u);    // two inner faces and four ring quads each
    CHECK(mesh.vertexCount() == 14u); // six, and four copies each
}

TEST_CASE("wem tools inset clamps at a notch's split event", "[wem][geometry][tools]") {
    // A notched heptagon (D14): the notch's vertex runs several times faster
    // than the rest, so the inset stops when it reaches the inner edge across
    // from it, long before the inner face's own area runs out.
    const std::vector<Vector3f> places = {
        {0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, {4.0f, 3.0f, 0.0f}, {2.2f, 3.0f, 0.0f},
        {2.0f, 0.6f, 0.0f}, {1.8f, 3.0f, 0.0f}, {0.0f, 3.0f, 0.0f},
    };
    Mesh mesh = fromFaces({{0, 1, 2, 3, 4, 5, 6}}, places);
    ModelPlan plan = geom::PlanInset(mesh, pointsOf(mesh), allFaces(mesh), geom::InsetType::Group);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    // The notch's tip is the fast one: its two edges are 9.5 degrees apart, so
    // the corner that stays clear of both runs 1 / sin(half the angle the
    // material turns through) as fast, which is 12.
    f32 fastest = 0.0f;
    for (const geom::VertexMotion& motion : plan.motions) {
        fastest = std::max(fastest, motion.direction.length());
    }
    CHECK(fastest == Catch::Approx(12.04f).margin(0.05));
    // And the clamp is where it meets the inner edge across from it: the tip
    // sits 0.6 above the bottom edge and closes on it at 12 + 1 per unit, so
    // 0.046 — a twentieth of the amount at which this face's own area would
    // run out.
    CHECK(plan.amountMax == Catch::Approx(0.6f / 13.04f).margin(2e-3));
    std::printf("inset notch: fastest %.2f x, clamp %.4f\n", static_cast<double>(fastest),
                static_cast<double>(plan.amountMax));
    // Just inside the clamp the mesh is whole; the clamp itself is the amount
    // at which a drawn triangle reaches zero area, and a polygon with one is
    // not a surface any cut can draw.
    geom::ApplyAmount(mesh, plan, plan.amountMax * 0.95f);
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
}
TEST_CASE("wem tools extrude keeps a region that touches itself manifold", "[wem][geometry][tools]") {
    // A C of quads in a 4 x 4 grid, from (1,1) round the bottom right to (2,2):
    // the two ends meet at the vertex (2,2) without sharing an edge, so the fan
    // there is two runs of the region and the vertex needs a copy for each.
    Mesh mesh = grid(4, 4, /*quads=*/true);
    const auto quad = [&](u32 x, u32 y) { return faceCentred(mesh, {x + 0.5f, y + 0.5f, 0.0f}); };
    ElementSet region;
    for (const auto& [x, y] : std::vector<std::pair<u32, u32>>{
             {1, 1}, {1, 0}, {2, 0}, {3, 0}, {3, 1}, {3, 2}, {2, 2}}) {
        region.faces.push_back(quad(x, y));
    }
    region.normalise();
    const u32 corner = vertexAt(mesh, 2, 2).value();
    ModelPlan plan = geom::PlanExtrudeFaces(mesh, pointsOf(mesh), region, geom::ExtrudeType::Group);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(mesh, plan, 0.5f);
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    // Two copies of that vertex were lifted, one per run, and the vertex itself
    // stayed below with the faces that did not move.
    u32 lifted = 0;
    const auto places = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (!mesh.topology().isDeleted(VertexId(v)) && places[v].x == 2.0f && places[v].y == 2.0f &&
            places[v].z == 0.5f) {
            ++lifted;
        }
    }
    CHECK(lifted == 2u);
    CHECK(places[corner].z == 0.0f);
}
// ============================================================================
// Chamfer (§3.9)
// ============================================================================

TEST_CASE("wem tools chamfer refuses what it has not built", "[wem][geometry][tools]") {
    Mesh mesh = box();
    ElementSet one;
    one.edges = {edgeBetween(mesh, vertexAt(mesh, 0, 0, 0), vertexAt(mesh, 1, 0, 0)).value()};
    geom::ChamferParams params;
    params.segments = 2;
    CHECK(geom::PlanChamferEdges(mesh, pointsOf(mesh), one, params).refusal ==
          ModelRefusal::NotBuiltYet);
    params.segments = 1;
    params.open = true;
    CHECK(geom::PlanChamferEdges(mesh, pointsOf(mesh), one, params).refusal ==
          ModelRefusal::NotBuiltYet);
    // A border edge has no second face for the rules to stand on.
    Mesh flat = grid(2, 2, /*quads=*/true);
    ElementSet border;
    border.edges = {edgeBetween(flat, vertexAt(flat, 0, 0), vertexAt(flat, 1, 0)).value()};
    CHECK(geom::PlanChamferEdges(flat, pointsOf(flat), border).refusal ==
          ModelRefusal::BorderEdgeChamfer);
    CHECK(geom::PlanChamferEdges(flat, pointsOf(flat), ElementSet{}).refusal ==
          ModelRefusal::EmptySelection);
}

TEST_CASE("wem tools chamfer cuts one edge of a cube", "[wem][geometry][tools]") {
    // One edge: two edge points at each end, so the faces beside it stay quads
    // and the third face at each end becomes a pentagon -- the straight cut.
    Mesh mesh = box();
    ElementSet one;
    one.edges = {edgeBetween(mesh, vertexAt(mesh, 0, 0, 0), vertexAt(mesh, 1, 0, 0)).value()};
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), one);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.motions.size() == 4u); // two points at each end
    geom::ApplyAmount(mesh, plan, 0.25f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 7u);    // six and the strip
    CHECK(mesh.vertexCount() == 10u); // eight, less the two ends, and four points
    u32 pentagons = 0, quads = 0;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        if (mesh.topology().isDeleted(FaceId(f))) {
            continue;
        }
        u32 valence = 0;
        for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
            (void)h;
            ++valence;
        }
        pentagons += valence == 5 ? 1 : 0;
        quads += valence == 4 ? 1 : 0;
    }
    CHECK(pentagons == 2u);
    CHECK(quads == 5u); // four cube faces and the strip
    // The points sit a quarter along their own edges, off the corner.
    CHECK(nearest(mesh, 0.25f, 0.0f).valid());
    CHECK(errors(mesh) == "");
}

TEST_CASE("wem tools chamfer miters two edges at a corner", "[wem][geometry][tools]") {
    // Two edges meeting at one corner: that corner's third edge takes an edge
    // point and the face between the two selected ones a miter point, so the
    // strips meet along the diagonal.
    Mesh mesh = box();
    ElementSet two;
    two.edges = {edgeBetween(mesh, vertexAt(mesh, 0, 0, 0), vertexAt(mesh, 1, 0, 0)).value(),
                 edgeBetween(mesh, vertexAt(mesh, 0, 0, 0), vertexAt(mesh, 0, 1, 0)).value()};
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), two);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(mesh, plan, 0.2f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 8u); // six and two strips
    CHECK(errors(mesh) == "");
    // The miter sits inside the bottom face, a fifth in from both edges.
    const VertexId miter = nearest(mesh, 0.2f, 0.2f);
    const auto places = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    CHECK(places[miter.value()].x == Catch::Approx(0.2f).margin(1e-4));
    CHECK(places[miter.value()].y == Catch::Approx(0.2f).margin(1e-4));
    CHECK(places[miter.value()].z == Catch::Approx(0.0f).margin(1e-4));
}

TEST_CASE("wem tools chamfer makes a corner out of three edges", "[wem][geometry][tools]") {
    // All three edges at one corner: three miter points and the triangle
    // between them.
    Mesh mesh = box();
    const VertexId corner = vertexAt(mesh, 0, 0, 0);
    ElementSet three;
    for (const auto& to : {Vector3f{1, 0, 0}, Vector3f{0, 1, 0}, Vector3f{0, 0, 1}}) {
        three.edges.push_back(edgeBetween(mesh, corner, vertexAt(mesh, to.x, to.y, to.z)).value());
    }
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), three);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(mesh, plan, 0.3f);
    finish(mesh, plan);
    // Six faces, three strips and the corner triangle.
    CHECK(liveFaces(mesh) == 10u);
    CHECK(errors(mesh) == "");
    u32 triangles = 0;
    for (u32 f = 0; f < mesh.topology().faceCount(); ++f) {
        if (mesh.topology().isDeleted(FaceId(f))) {
            continue;
        }
        u32 valence = 0;
        for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
            (void)h;
            ++valence;
        }
        triangles += valence == 3 ? 1 : 0;
    }
    CHECK(triangles == 1u);
}

TEST_CASE("wem tools chamfer of every cube edge stops at a half", "[wem][geometry][tools]") {
    // Twelve edges: no edge points at all, so an edge-only clamp would see
    // nothing. The faces' inner squares, of side 1 - 2a, are what run out (D14).
    Mesh mesh = box();
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), allEdges(mesh));
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.motions.size() == 24u); // three miters at each of eight corners
    CHECK(plan.amountMax == Catch::Approx(0.5f).margin(1e-3));
    geom::ApplyAmount(mesh, plan, 0.25f);
    finish(mesh, plan);
    CHECK(liveFaces(mesh) == 26u); // six faces, twelve strips, eight corners
    CHECK(errors(mesh) == "");
}

TEST_CASE("wem tools chamfer of a loop leaves no patch", "[wem][geometry][tools]") {
    // A straight run through valence-4 vertices: two points at each, and the
    // strips meet across the edge between them with no patch in between.
    Mesh mesh = grid(3, 3, /*quads=*/true);
    ElementSet loop;
    loop.edges = {edgeBetween(mesh, vertexAt(mesh, 1, 1), vertexAt(mesh, 2, 1)).value(),
                  edgeBetween(mesh, vertexAt(mesh, 2, 1), vertexAt(mesh, 3, 1)).value()};
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), loop);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(mesh, plan, 0.2f);
    finish(mesh, plan);
    // The run goes straight through (2,1), whose two selected edges share no
    // face: two points there and no patch. Its far end is on the grid's border
    // with two points and no patch either. Only the inner end, where one
    // selected edge meets three unselected ones, rings three points and closes.
    // Nine faces, two strips, one patch.
    CHECK(liveFaces(mesh) == 12u);
    CHECK(errors(mesh) == "");
}
TEST_CASE("wem tools chamfer gives a straight corner a face point", "[wem][geometry][tools]") {
    // A T-vertex (§3.3): the big face holds it as a straight corner. Chamfering
    // the edge that arrives there leaves that face no angle to offset along, so
    // it puts a face point square to the edge instead, and the edge running on
    // past the T keeps its own point at the plain amount.
    // The T sits a tenth of a degree off straight, as one in real data does.
    const std::vector<Vector3f> places = {{0, 0, 0}, {2, 0, 0}, {2.002f, 1, 0}, {2, 2, 0},
                                          {0, 2, 0}, {3, 0, 0}, {3, 1, 0}, {3, 2, 0}};
    Mesh mesh = fromFaces({{0, 1, 2, 3, 4}, {1, 5, 6, 2}, {2, 6, 7, 3}}, places);
    ElementSet one;
    one.edges = {edgeBetween(mesh, vertexAt(mesh, 2, 0), vertexAt(mesh, 2.002f, 1)).value()};
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), one);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(mesh, plan, 0.25f);
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    // The face point stands a quarter into the big face, square to the edge;
    // the edge going on north of the T keeps its point a quarter along itself.
    bool square = false;
    bool along = false;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        square = square || (std::abs(at[v].x - 1.752f) < 1e-2f && std::abs(at[v].y - 1.0f) < 1e-2f);
        along = along || (std::abs(at[v].x - 2.002f) < 1e-2f && std::abs(at[v].y - 1.25f) < 1e-2f);
    }
    CHECK(square);
    CHECK(along);
}

TEST_CASE("wem tools chamfer offsets a reflex corner outward", "[wem][geometry][tools]") {
    // An L of two quads: the corner where they meet is reflex in neither, but
    // the face that wraps it is. Chamfering the edge at that corner puts the
    // wrapping face's point square to the edge rather than off to infinity.
    const std::vector<Vector3f> places = {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0},
                                          {1, 2, 0}, {0, 2, 0}, {1, 0, 0}};
    Mesh mesh = fromFaces({{0, 6, 3, 5}, {6, 1, 2, 3}, {5, 3, 4}}, places);
    ElementSet one;
    one.edges = {edgeBetween(mesh, vertexAt(mesh, 1, 0), vertexAt(mesh, 1, 1)).value()};
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), one);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(mesh, plan, 0.2f);
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    // Every point the tool placed is still inside the L's bounding box: none
    // ran off along a near-parallel edge.
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        CHECK(at[v].x >= -1e-4f);
        CHECK(at[v].x <= 2.0f + 1e-4f);
        CHECK(at[v].y >= -1e-4f);
        CHECK(at[v].y <= 2.0f + 1e-4f);
    }
}

TEST_CASE("wem tools chamfer closes a border ring only when it has three",
          "[wem][geometry][tools]") {
    // A border vertex's ring is an open chain. Two points leave the strip's end
    // as a new border edge; three close into a patch with a new border edge
    // across them.
    Mesh quads = grid(2, 2, /*quads=*/true);
    ElementSet toBorder;
    toBorder.edges = {edgeBetween(quads, vertexAt(quads, 1, 0), vertexAt(quads, 1, 1)).value()};
    ModelPlan flat = geom::PlanChamferEdges(quads, pointsOf(quads), toBorder);
    INFO("refusal " << geom::ToString(flat.refusal));
    REQUIRE_FALSE(flat.refused());
    geom::ApplyAmount(quads, flat, 0.2f);
    finish(quads, flat);
    CHECK(errors(quads) == "");
    // Four faces, the strip, and one patch at the interior end. The border end
    // rings two points and closes nothing.
    CHECK(liveFaces(quads) == 6u);

    // The same edge in a triangulated grid: its border end has two interior
    // edges as well, so its ring is three and closes.
    Mesh triangles = grid(2, 2);
    const u32 faces = liveFaces(triangles); // Plan fixes the topology, so count first
    ElementSet again;
    again.edges = {edgeBetween(triangles, vertexAt(triangles, 1, 0), vertexAt(triangles, 1, 1)).value()};
    ModelPlan plan = geom::PlanChamferEdges(triangles, pointsOf(triangles), again);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(triangles, plan, 0.15f);
    finish(triangles, plan);
    CHECK(errors(triangles) == "");
    CHECK(liveFaces(triangles) == faces + 3u); // the strip and a patch at each end
}

TEST_CASE("wem tools chamfer keeps a seam on the strip's far edge", "[wem][geometry][tools]") {
    // The chamfered edge is a seam: the strip samples one side for all four of
    // its corners, so it is continuous with that side and the seam ends up on
    // its far long edge (§3.9).
    Mesh mesh = grid(2, 1, /*quads=*/true);
    mapUvs(mesh, /*shiftedFaces=*/{1});
    const EdgeId middle = edgeBetween(mesh, vertexAt(mesh, 1, 0), vertexAt(mesh, 1, 1));
    mesh.attributes.getOrCreate<u8>(geom::names::kSeam, Domain::Edge,
                                    geom::AttrType::Bool)[middle.value()] = 1;
    ElementSet one;
    one.edges = {middle.value()};
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), one);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    const u32 strip = plan.selection.faces.front();
    geom::ApplyAmount(mesh, plan, 0.25f);
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    // The strip's four corners read one map: the left face's, where u is x.
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    REQUIRE(strip < mesh.topology().faceCount());
    for (const HalfedgeId h : mesh.topology().fh(FaceId(strip))) {
        const Vector3f& place = at[mesh.topology().from(h).value()];
        CHECK(uv[h.index()].x == Catch::Approx(place.x).margin(1e-3));
    }
}
TEST_CASE("wem tools chamfer blends an edge point's skin", "[wem][geometry][tools]") {
    // A 2 x 1 grid bound to two bones, one column each. Chamfering the edge
    // between them puts each point a quarter along an edge whose ends are
    // bound differently, so its skin is the blend at that place, not a copy of
    // either end (§2.4).
    Mesh mesh = grid(2, 1, /*quads=*/true);
    bindMesh(mesh, {vertexAt(mesh, 2, 0).value(), vertexAt(mesh, 2, 1).value()});
    ElementSet one;
    one.edges = {edgeBetween(mesh, vertexAt(mesh, 1, 0), vertexAt(mesh, 1, 1)).value()};
    ModelPlan plan = geom::PlanChamferEdges(mesh, pointsOf(mesh), one);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::ApplyAmount(mesh, plan, 0.25f);
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    // The point a quarter of the way from (1,0) towards (2,0): a quarter of
    // the far bone, three quarters of the near one.
    const std::span<const geom::Influence> blended = influencesAt(mesh, 1.25f, 0.0f);
    REQUIRE(blended.size() == 2u);
    f32 far = 0.0f;
    for (const geom::Influence& influence : blended) {
        if (influence.bone == 1) {
            far = influence.weight;
        }
    }
    CHECK(far == Catch::Approx(0.25f).margin(1e-3));
    // And the point on the near side is its own bone alone.
    const std::span<const geom::Influence> near = influencesAt(mesh, 0.75f, 0.0f);
    REQUIRE(near.size() == 1u);
    CHECK(near[0].bone == 0u);
}
// ============================================================================
// Adding topology by hand (§3.14)
// ============================================================================

TEST_CASE("wem tools insert vertex splits where it was clicked", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    mapUvs(mesh);
    const EdgeId edge = edgeBetween(mesh, vertexAt(mesh, 0, 0), vertexAt(mesh, 1, 0));
    ModelPlan plan = geom::PlanInsertVertex(mesh, edge, 0.25f);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 2u); // no face added: the quad beside it gains a corner
    const VertexId made = vertexAt(mesh, 0.25f, 0);
    CHECK(made.valid());
    // Its corner is the lerp of the ones the edge spans, and its skin is blank
    // here, so the UV is what tells: a quarter along.
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    for (const HalfedgeId h : mesh.topology().voh(made)) {
        if (mesh.topology().face(h).valid()) {
            CHECK(uv[h.index()].x == Catch::Approx(0.25f).margin(1e-4));
        }
    }
}

TEST_CASE("wem tools divide cuts an edge into equal parts", "[wem][geometry][tools]") {
    Mesh mesh = grid(1, 1, /*quads=*/true);
    ElementSet one;
    one.edges = {edgeBetween(mesh, vertexAt(mesh, 0, 0), vertexAt(mesh, 1, 0)).value()};
    ModelPlan plan = geom::PlanDivide(mesh, one, 3);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(plan.changed == 2u); // three parts, two cuts
    CHECK(nearest(mesh, 1.0f / 3.0f, 0.0f).valid());
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    CHECK(at[nearest(mesh, 1.0f / 3.0f, 0.0f).value()].x == Catch::Approx(1.0f / 3.0f).margin(1e-4));
    CHECK(at[nearest(mesh, 2.0f / 3.0f, 0.0f).value()].x == Catch::Approx(2.0f / 3.0f).margin(1e-4));
    CHECK(geom::PlanDivide(mesh, one, 1).refusal == ModelRefusal::ZeroAmount);
}

TEST_CASE("wem tools connect joins the corners of a face", "[wem][geometry][tools]") {
    // Two opposite corners of a quad: one cut, two triangles.
    Mesh quad = grid(1, 1, /*quads=*/true);
    ElementSet pair;
    pair.vertices = {vertexAt(quad, 0, 0).value(), vertexAt(quad, 1, 1).value()};
    ModelPlan plan = geom::PlanConnectVertices(quad, pointsOf(quad), pair);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(quad, plan);
    CHECK(errors(quad) == "");
    CHECK(liveFaces(quad) == 2u);
    CHECK(plan.changed == 1u);
    // Two corners that already share an edge have nothing to join.
    Mesh again = grid(1, 1, /*quads=*/true);
    ElementSet along;
    along.vertices = {vertexAt(again, 0, 0).value(), vertexAt(again, 1, 0).value()};
    CHECK(geom::PlanConnectVertices(again, pointsOf(again), along).refusal ==
          ModelRefusal::DiagonalExists);

    // A hexagon's corners 0, 2 and 4: three cuts and the inner triangle they
    // make, which is 3ds Max's rule.
    const std::vector<Vector3f> ring = {{1, 0, 0},  {0.5f, 0.87f, 0},  {-0.5f, 0.87f, 0},
                                        {-1, 0, 0}, {-0.5f, -0.87f, 0}, {0.5f, -0.87f, 0}};
    Mesh hexagon = fromFaces({{0, 1, 2, 3, 4, 5}}, ring);
    ElementSet every;
    every.vertices = {0, 2, 4};
    ModelPlan inner = geom::PlanConnectVertices(hexagon, pointsOf(hexagon), every);
    INFO("refusal " << geom::ToString(inner.refusal));
    REQUIRE_FALSE(inner.refused());
    finish(hexagon, inner);
    CHECK(errors(hexagon) == "");
    CHECK(inner.changed == 3u);
    CHECK(liveFaces(hexagon) == 4u); // three corners cut off, and the triangle
}

TEST_CASE("wem tools connect bridges two edges of a face", "[wem][geometry][tools]") {
    // A quad's two opposite edges: one vertex on each and an edge between them,
    // and the slide runs it along its own edge.
    Mesh mesh = grid(1, 1, /*quads=*/true);
    ElementSet two;
    two.edges = {edgeBetween(mesh, vertexAt(mesh, 0, 0), vertexAt(mesh, 1, 0)).value(),
                 edgeBetween(mesh, vertexAt(mesh, 0, 1), vertexAt(mesh, 1, 1)).value()};
    ModelPlan plan = geom::PlanConnectEdges(mesh, pointsOf(mesh), two);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.changed == 1u);
    CHECK(plan.motions.size() == 2u);
    CHECK(plan.amountMin == -1.0f);
    CHECK(plan.amountMax == 1.0f);
    // Halfway along the slide is a quarter of the edge from the centre.
    geom::ApplyAmount(mesh, plan, 0.5f);
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 2u);
    CHECK(nearest(mesh, 0.75f, 0.0f).valid());
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    CHECK(at[nearest(mesh, 0.75f, 0.0f).value()].x == Catch::Approx(0.75f).margin(1e-4));
}

TEST_CASE("wem tools turn rotates a real edge", "[wem][geometry][tools]") {
    // Two triangles either side of a diagonal: the turn swings it onto the
    // other pair of corners.
    Mesh mesh = grid(1, 1);
    const EdgeId diagonal = edgeBetween(mesh, vertexAt(mesh, 0, 0), vertexAt(mesh, 1, 1));
    ModelPlan plan = geom::PlanTurnEdge(mesh, diagonal);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 2u);
    CHECK(mesh.topology().findHalfedge(vertexAt(mesh, 1, 0), vertexAt(mesh, 0, 1)).valid());
    CHECK_FALSE(mesh.topology().findHalfedge(vertexAt(mesh, 0, 0), vertexAt(mesh, 1, 1)).valid());
    // A border edge has only one face to turn between.
    Mesh border = grid(1, 1);
    CHECK(geom::PlanTurnEdge(border, edgeBetween(border, vertexAt(border, 0, 0),
                                                 vertexAt(border, 1, 0))).refusal ==
          ModelRefusal::EmptySelection);
    // On a tetrahedron every pair of corners is already joined, so any turn
    // would land on an edge that is there.
    const std::vector<Vector3f> corners = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Mesh tetra = fromFaces({{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}, corners);
    const ModelPlan closed =
        geom::PlanTurnEdge(tetra, edgeBetween(tetra, VertexId(0), VertexId(1)));
    INFO("refusal " << geom::ToString(closed.refusal));
    CHECK(closed.refusal == ModelRefusal::DiagonalExists);
}
TEST_CASE("wem tools slice cuts the faces its plane crosses", "[wem][geometry][tools]") {
    // A 2 x 2 grid cut down x = 0.5: the two faces of the left column are cut
    // in two, and each crossed edge takes a vertex.
    Mesh mesh = grid(2, 2, /*quads=*/true);
    geom::SliceParams params;
    params.origin = {0.5f, 0.0f, 0.0f};
    params.normal = {1.0f, 0.0f, 0.0f};
    ModelPlan plan = geom::PlanSlice(mesh, pointsOf(mesh), ElementSet{}, params);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 6u);
    CHECK(nearest(mesh, 0.5f, 1.0f).valid());
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    CHECK(at[nearest(mesh, 0.5f, 1.0f).value()].x == Catch::Approx(0.5f).margin(1e-5));
    // A plane that misses everything refuses, and leaves the mesh as it was.
    Mesh clear = grid(2, 2, /*quads=*/true);
    geom::SliceParams away = params;
    away.origin = {5.0f, 0.0f, 0.0f};
    const u32 before = clear.vertexCount();
    CHECK(geom::PlanSlice(clear, pointsOf(clear), ElementSet{}, away).refusal ==
          ModelRefusal::NothingToSlice);
    CHECK(clear.vertexCount() == before);
}

TEST_CASE("wem tools slice snaps a corner instead of cutting a sliver", "[wem][geometry][tools]") {
    // The plane runs corner to corner, a hair off both: each corner is within
    // the mesh's tolerance, so it is snapped onto the plane and used as the
    // crossing. The quad becomes two triangles with no vertex added.
    Mesh mesh = grid(1, 1, /*quads=*/true);
    const u32 before = mesh.vertexCount();
    geom::SliceParams params;
    params.origin = {1e-6f, 0.0f, 0.0f};
    params.normal = {0.7071068f, -0.7071068f, 0.0f};
    ModelPlan plan = geom::PlanSlice(mesh, pointsOf(mesh), ElementSet{}, params);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(mesh.vertexCount() == before); // nothing added: the corners were the crossings
    CHECK(liveFaces(mesh) == 2u);
}

TEST_CASE("wem tools slice parts the two sides and drops one", "[wem][geometry][tools]") {
    // Split: the vertices along the cut are doubled, so the halves come apart.
    Mesh split = grid(2, 1, /*quads=*/true);
    geom::SliceParams params;
    params.origin = {1.0f, 0.0f, 0.0f};
    params.normal = {1.0f, 0.0f, 0.0f};
    params.mode = geom::SliceMode::Split;
    const u32 before = split.vertexCount();
    ModelPlan plan = geom::PlanSlice(split, pointsOf(split), ElementSet{}, params);
    INFO("refusal " << geom::ToString(plan.refusal));
    // The plane runs along the grid's own middle edges, so there is nothing to
    // cut there; the halves are parted at x = 0.5 instead.
    params.origin = {0.5f, 0.0f, 0.0f};
    Mesh parted = grid(2, 1, /*quads=*/true);
    ModelPlan apart = geom::PlanSlice(parted, pointsOf(parted), ElementSet{}, params);
    INFO("refusal " << geom::ToString(apart.refusal));
    REQUIRE_FALSE(apart.refused());
    finish(parted, apart);
    CHECK(errors(parted) == "");
    CHECK(parted.vertexCount() == before + 2u + 2u); // two on the cut, each doubled
    CHECK(liveFaces(parted) == 3u);

    // Remove: the side the normal does not point at goes.
    Mesh dropped = grid(2, 1, /*quads=*/true);
    geom::SliceParams cut = params;
    cut.mode = geom::SliceMode::Remove;
    ModelPlan gone = geom::PlanSlice(dropped, pointsOf(dropped), ElementSet{}, cut);
    INFO("refusal " << geom::ToString(gone.refusal));
    REQUIRE_FALSE(gone.refused());
    finish(dropped, gone);
    CHECK(errors(dropped) == "");
    CHECK(liveFaces(dropped) == 2u); // the piece left of the plane went
    const auto at = dropped.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < dropped.vertexCount(); ++v) {
        if (!dropped.topology().isDeleted(VertexId(v))) {
            CHECK(at[v].x >= 0.5f - 1e-5f);
        }
    }
    (void)plan;
}

TEST_CASE("wem tools slice cuts a concave face in two places", "[wem][geometry][tools]") {
    // A C of six corners: the plane crosses it four times, so it is cut twice
    // and not once across the gap.
    const std::vector<Vector3f> places = {{0, 0, 0}, {3, 0, 0}, {3, 1, 0}, {1, 1, 0},
                                          {1, 2, 0}, {3, 2, 0}, {3, 3, 0}, {0, 3, 0}};
    Mesh mesh = fromFaces({{0, 1, 2, 3, 4, 5, 6, 7}}, places);
    geom::SliceParams params;
    params.origin = {2.0f, 0.0f, 0.0f};
    params.normal = {1.0f, 0.0f, 0.0f};
    ModelPlan plan = geom::PlanSlice(mesh, pointsOf(mesh), ElementSet{}, params);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.changed == 2u); // two cuts, not one across the notch
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 3u);
}
TEST_CASE("wem tools cut walks the surface between two clicks", "[wem][geometry][tools]") {
    // Two quads side by side, cut from the middle of the left border edge to
    // the middle of the right one: the walk crosses the edge between them and
    // leaves an edge in each face.
    Mesh mesh = grid(2, 1, /*quads=*/true);
    const u32 before = mesh.vertexCount();
    geom::CutPoint from;
    from.edge = edgeBetween(mesh, vertexAt(mesh, 0, 0), vertexAt(mesh, 0, 1)).value();
    from.t = 0.5f;
    geom::CutPoint to;
    to.edge = edgeBetween(mesh, vertexAt(mesh, 2, 0), vertexAt(mesh, 2, 1)).value();
    to.t = 0.5f;
    // Seen from +Z, the plane through both clicks and the view is y = 0.5.
    ModelPlan plan = geom::PlanCut(mesh, pointsOf(mesh), from, to, Vector3f{0.0f, 1.0f, 0.0f});
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(mesh.vertexCount() == before + 3u); // one at each click and one crossing
    CHECK(liveFaces(mesh) == 4u);             // both quads cut in two
    CHECK(plan.changed == 2u);                // two edges of cut
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const VertexId middle = nearest(mesh, 1.0f, 0.5f);
    CHECK(at[middle.value()].x == Catch::Approx(1.0f).margin(1e-5));
    CHECK(at[middle.value()].y == Catch::Approx(0.5f).margin(1e-5));
}

TEST_CASE("wem tools cut between two corners of one face", "[wem][geometry][tools]") {
    Mesh mesh = grid(1, 1, /*quads=*/true);
    const u32 before = mesh.vertexCount();
    geom::CutPoint from;
    from.vertex = vertexAt(mesh, 0, 0).value();
    geom::CutPoint to;
    to.vertex = vertexAt(mesh, 1, 1).value();
    ModelPlan plan = geom::PlanCut(mesh, pointsOf(mesh), from, to, Vector3f{0.7071068f, -0.7071068f, 0.0f});
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(mesh.vertexCount() == before); // corner to corner adds nothing
    CHECK(liveFaces(mesh) == 2u);
    // The same click twice is no cut at all.
    Mesh again = grid(1, 1, /*quads=*/true);
    geom::CutPoint one;
    one.vertex = vertexAt(again, 0, 0).value();
    CHECK(geom::PlanCut(again, pointsOf(again), one, one, Vector3f{0.0f, 1.0f, 0.0f}).refusal ==
          ModelRefusal::NothingToSlice);
}
TEST_CASE("wem tools loop cut stops at a cylinder's cap", "[wem][geometry][tools]") {
    // A six-sided cylinder with an n-gon at each end: the ring runs round the
    // side quads and stops at the caps, as `EdgeRing` does, so the loop it cuts
    // is the belt and nothing else.
    constexpr u32 kSides = 6;
    std::vector<Vector3f> places;
    for (u32 ring = 0; ring < 2; ++ring) {
        for (u32 i = 0; i < kSides; ++i) {
            const f32 angle = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(kSides);
            places.push_back(Vector3f{std::cos(angle), std::sin(angle), static_cast<f32>(ring)});
        }
    }
    std::vector<std::vector<u32>> faces;
    for (u32 i = 0; i < kSides; ++i) {
        const u32 next = (i + 1) % kSides;
        faces.push_back({i, next, next + kSides, i + kSides});
    }
    std::vector<u32> bottom;
    std::vector<u32> top;
    for (u32 i = 0; i < kSides; ++i) {
        bottom.push_back(kSides - 1 - i);
        top.push_back(kSides + i);
    }
    faces.push_back(bottom);
    faces.push_back(top);
    Mesh mesh = fromFaces(faces, places);
    const EdgeId upright = edgeBetween(mesh, VertexId(0), VertexId(kSides));
    const ElementSet ring = geom::EdgeRing(mesh, pointsOf(mesh), upright);
    CHECK(ring.edges.size() == kSides); // the six uprights, and neither cap
    ModelPlan plan = geom::PlanConnectEdges(mesh, pointsOf(mesh), ring);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.changed == kSides); // one new edge per side quad: the belt
    CHECK(plan.kept == 0u);
    geom::ApplyAmount(mesh, plan, 0.0f);
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == kSides * 2 + 2); // each side quad in two, caps as they were
}

TEST_CASE("wem tools cut takes the plane it is given", "[wem][geometry][tools]") {
    // The same two clicks under two projections: an orthographic view hands the
    // cut the plane holding the view direction, a perspective one the plane
    // through the eye. Each walks its own way across the grid.
    const auto run = [](const Vector3f& plane) {
        Mesh mesh = grid(2, 2, /*quads=*/true);
        geom::CutPoint from;
        from.vertex = vertexAt(mesh, 0, 0).value();
        geom::CutPoint to;
        to.vertex = vertexAt(mesh, 2, 2).value();
        ModelPlan plan = geom::PlanCut(mesh, pointsOf(mesh), from, to, plane);
        INFO("refusal " << geom::ToString(plan.refusal));
        REQUIRE_FALSE(plan.refused());
        finish(mesh, plan);
        CHECK(errors(mesh) == "");
        return plan.changed;
    };
    // Straight down the diagonal: the cut crosses the middle vertex and needs
    // no new one.
    CHECK(run(Vector3f{0.7071068f, -0.7071068f, 0.0f}) >= 2u);
    // A plane leaning the other way walks a different line across the same
    // grid, and still arrives.
    CHECK(run(Vector3f{0.9f, -0.4f, 0.0f}) >= 2u);
}
// ============================================================================
// Parts (§3.16)
// ============================================================================

TEST_CASE("wem tools duplicate copies a shell of its own", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    ElementSet one;
    one.faces = {faceCentred(mesh, {0.5f, 0.5f, 0.0f})};
    ModelPlan plan = geom::PlanDuplicate(mesh, one);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 3u);     // two, and the copy
    CHECK(mesh.vertexCount() == 10u); // six, and four of its own
    // The copy is its own point everywhere: each of its vertices carries a
    // merge group no other vertex has, so nothing welds it back.
    const auto groups = mesh.attributes.get<const u32>(geom::names::kMergeGroup, Domain::Vertex);
    REQUIRE(groups.size() == mesh.vertexCount());
    for (u32 v = 6; v < mesh.vertexCount(); ++v) {
        u32 shared = 0;
        for (u32 other = 0; other < mesh.vertexCount(); ++other) {
            shared += groups[other] == groups[v] ? 1u : 0u;
        }
        CHECK(shared == 1u);
    }
}

TEST_CASE("wem tools detach to element parts a shell without moving it", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    ElementSet one;
    one.faces = {faceCentred(mesh, {0.5f, 0.5f, 0.0f})};
    ModelPlan plan = geom::PlanDetachToElement(mesh, one);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 2u); // the same faces, now two shells
    // Nothing is shared between them any more.
    std::vector<u32> first;
    std::vector<u32> second;
    for (const HalfedgeId h : mesh.topology().fh(FaceId(0)))
        first.push_back(mesh.topology().from(h).value());
    for (const HalfedgeId h : mesh.topology().fh(FaceId(1)))
        second.push_back(mesh.topology().from(h).value());
    std::sort(first.begin(), first.end());
    std::sort(second.begin(), second.end());
    std::vector<u32> both;
    std::set_intersection(first.begin(), first.end(), second.begin(), second.end(),
                          std::back_inserter(both));
    CHECK(both.empty());
    // Every face of the mesh is already its own shell.
    Mesh whole = grid(1, 1, /*quads=*/true);
    CHECK(geom::PlanDetachToElement(whole, allFaces(whole)).refusal == ModelRefusal::AllFaces);
}

TEST_CASE("wem tools create polygon closes a border loop by hand", "[wem][geometry][tools]") {
    // Two quads with a square hole between them: its four border vertices, in
    // border order, make the face that fills it.
    const std::vector<Vector3f> places = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                          {2, 1, 0}, {0, 2, 0}, {1, 2, 0}, {2, 2, 0}};
    Mesh mesh = fromFaces({{0, 1, 4, 3}, {1, 2, 5, 4}, {3, 4, 7, 6}}, places);
    ModelPlan plan = geom::PlanCreatePolygon(mesh, pointsOf(mesh), {4, 5, 8, 7});
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 4u);
    // A loop that runs a border edge the way its own face does would be the
    // second face on that side, wound alike: that is what it refuses, whether
    // the loop is an existing face or the hole taken backwards.
    Mesh again = fromFaces({{0, 1, 4, 3}, {1, 2, 5, 4}, {3, 4, 7, 6}}, places);
    CHECK(geom::PlanCreatePolygon(again, pointsOf(again), {0, 1, 4, 3}).refusal ==
          ModelRefusal::WindingDisagrees);
    CHECK(geom::PlanCreatePolygon(again, pointsOf(again), {7, 8, 5, 4}).refusal ==
          ModelRefusal::WindingDisagrees);
    CHECK(geom::PlanCreatePolygon(again, pointsOf(again), {4, 5}).refusal == ModelRefusal::CapTooShort);
}

TEST_CASE("wem tools bridge joins two border loops", "[wem][geometry][tools]") {
    // Two squares, one above the other, each its own shell: bridging their
    // borders makes the four walls between them.
    const std::vector<Vector3f> places = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                          {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    Mesh mesh = fromFaces({{0, 3, 2, 1}, {4, 5, 6, 7}}, places);
    const EdgeId lower = edgeBetween(mesh, VertexId(0), VertexId(1));
    const EdgeId upper = edgeBetween(mesh, VertexId(4), VertexId(5));
    ModelPlan plan = geom::PlanBridge(mesh, pointsOf(mesh), lower, upper);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 6u); // a closed box
    CHECK(plan.changed == 4u);
    // Every edge is now between two faces: the shell is closed.
    u32 border = 0;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (!mesh.topology().isDeleted(EdgeId(e)) && mesh.topology().isBoundary(EdgeId(e))) {
            ++border;
        }
    }
    CHECK(border == 0u);
    // One loop twice, and more segments than are built.
    Mesh again = fromFaces({{0, 3, 2, 1}, {4, 5, 6, 7}}, places);
    const EdgeId one = edgeBetween(again, VertexId(0), VertexId(1));
    CHECK(geom::PlanBridge(again, pointsOf(again), one, one).refusal == ModelRefusal::SameLoop);
    geom::BridgeParams two;
    two.segments = 2;
    CHECK(geom::PlanBridge(again, pointsOf(again), one, edgeBetween(again, VertexId(4), VertexId(5)), two)
              .refusal == ModelRefusal::NotBuiltYet);
}
TEST_CASE("wem tools detach lifts faces into a mesh of their own", "[wem][geometry][tools]") {
    Mesh mesh = grid(2, 1, /*quads=*/true);
    bindMesh(mesh, {vertexAt(mesh, 2, 0).value(), vertexAt(mesh, 2, 1).value()});
    mapUvs(mesh);
    ElementSet one;
    one.faces = {faceCentred(mesh, {1.5f, 0.5f, 0.0f})};
    Mesh made;
    ModelPlan plan = geom::PlanDetach(mesh, one, made, /*keep=*/false);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(errors(made) == "");
    CHECK(liveFaces(mesh) == 1u); // the source kept the other quad
    CHECK(liveFaces(made) == 1u);
    CHECK(made.vertexCount() == 4u);
    // The new mesh brought the skin and the map with it: its right-hand
    // corners are the far bone's, and its UVs are where they were.
    const std::span<const geom::Influence> far = made.skin.forVertex(vertexAt(made, 2, 0).value());
    REQUIRE(far.size() == 1u);
    CHECK(far[0].bone == 1u);
    const auto uv = made.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    const auto at = made.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (const HalfedgeId h : made.topology().fh(FaceId(0))) {
        CHECK(uv[h.index()].x == Catch::Approx(at[made.topology().from(h).value()].x).margin(1e-4));
    }

    // As a clone the source keeps its faces, and a whole mesh is refused.
    Mesh cloned = grid(2, 1, /*quads=*/true);
    Mesh copy;
    ElementSet same;
    same.faces = {faceCentred(cloned, {1.5f, 0.5f, 0.0f})};
    ModelPlan clone = geom::PlanDetach(cloned, same, copy, /*keep=*/true);
    INFO("refusal " << geom::ToString(clone.refusal));
    REQUIRE_FALSE(clone.refused());
    CHECK(liveFaces(cloned) == 2u);
    CHECK(liveFaces(copy) == 1u);
    Mesh whole = grid(1, 1, /*quads=*/true);
    Mesh nothing;
    CHECK(geom::PlanDetach(whole, allFaces(whole), nothing, false).refusal == ModelRefusal::AllFaces);
}
TEST_CASE("wem primitives are born finished", "[wem][geometry][tools]") {
    // Each comes back modelled, canonical, with its rows and its bounds, and
    // closed: a shape a tool can take hold of without preparing it first.
    geom::PrimitiveParams params;
    params.sides = 8;
    params.segments = 2;
    struct Made {
        const char* name;
        Mesh mesh;
        u32 faces;
    };
    std::vector<Made> made;
    made.push_back({"box", geom::MakeBox(params), 6});
    made.push_back({"plane", geom::MakePlane(params), 4});
    made.push_back({"cylinder", geom::MakeCylinder(params), 10});
    made.push_back({"cone", geom::MakeCone(params), 9});
    made.push_back({"sphere", geom::MakeSphere(params), 24});
    for (Made& one : made) {
        INFO(one.name);
        CHECK(geom::IsModelled(one.mesh));
        CHECK(geom::IsCanonical(one.mesh));
        CHECK(errors(one.mesh) == "");
        CHECK(liveFaces(one.mesh) == one.faces);
        CHECK(one.mesh.bounds.minimum.x < one.mesh.bounds.maximum.x);
        // Its rows are there: every face of four corners or more draws.
        Diagnostics stale;
        geom::CheckStructural(one.mesh, 0, stale);
        CHECK(stale.countOf(DiagCode::StaleTriangulation) == 0u);
        // And it carries a map.
        CHECK_FALSE(one.mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge).empty());
    }
    // A plane is the open one; the others are closed shells.
    for (Made& one : made) {
        INFO(one.name);
        u32 border = 0;
        for (u32 e = 0; e < one.mesh.topology().edgeCount(); ++e) {
            if (!one.mesh.topology().isDeleted(EdgeId(e)) && one.mesh.topology().isBoundary(EdgeId(e))) {
                ++border;
            }
        }
        CHECK(border == (std::string(one.name) == "plane" ? 8u : 0u));
    }
    // A box's edges are its shape; the round ones shade smooth.
    const auto sharp = made[0].mesh.attributes.get<const u8>(geom::names::kSharp, Domain::Edge);
    REQUIRE(sharp.size() == made[0].mesh.topology().edgeCount());
    for (const u8 flag : sharp) {
        CHECK(flag == 1u);
    }
    CHECK(made[2].mesh.attributes.get<const u8>(geom::names::kSharp, Domain::Edge).empty());
}
// ============================================================================
// Symmetry and moving well (§3.15, §3.17)
// ============================================================================

TEST_CASE("wem tools symmetrize mirrors one side onto the other", "[wem][geometry][tools]") {
    // A grid from y = -1 to y = 2, symmetrised across Y from the + side: the
    // half below the plane goes, the half above is copied reflected, and the
    // vertices on the plane are shared rather than doubled.
    Mesh mesh = grid(2, 3, /*quads=*/true);
    {
        const std::span<Vector3f> at =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
        for (Vector3f& place : at) {
            place.y -= 1.0f; // y now runs -1 .. 2
        }
    }
    bindMesh(mesh, {});
    geom::SymmetrizeParams params;
    params.axis = 1; // Y
    params.fromPositive = true;
    ModelPlan plan = geom::PlanSymmetrize(mesh, pointsOf(mesh), params);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    // The kept half is two rows of quads; its mirror is two more.
    CHECK(liveFaces(mesh) == 8u);
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    f32 lowest = 1e9f;
    f32 highest = -1e9f;
    u32 onPlane = 0;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        lowest = std::min(lowest, at[v].y);
        highest = std::max(highest, at[v].y);
        onPlane += std::abs(at[v].y) < 1e-4f ? 1u : 0u;
    }
    CHECK(lowest == Catch::Approx(-2.0f).margin(1e-4));
    CHECK(highest == Catch::Approx(2.0f).margin(1e-4));
    CHECK(onPlane == 3u); // the seam is shared, not doubled
    // Every edge on the seam has two faces: the halves are joined, not merely
    // touching.
    u32 border = 0;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (mesh.topology().isDeleted(EdgeId(e)) || !mesh.topology().isBoundary(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = geom::Topology::halfedge(EdgeId(e), 0);
        if (std::abs(at[mesh.topology().from(h).value()].y) < 1e-4f &&
            std::abs(at[mesh.topology().to(h).value()].y) < 1e-4f) {
            ++border;
        }
    }
    CHECK(border == 0u);
}

TEST_CASE("wem tools symmetrize maps the copy's bones", "[wem][geometry][tools]") {
    Mesh mesh = grid(1, 2, /*quads=*/true);
    {
        const std::span<Vector3f> at =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
        for (Vector3f& place : at) {
            place.y -= 1.0f;
        }
    }
    // Everything above the plane is bone 1; its mirror is bone 2.
    bindMesh(mesh, {});
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        const geom::Influence one{1u, 1.0f};
        mesh.skin.assignVertex(v, std::span<const geom::Influence>(&one, 1));
    }
    const std::vector<u32> mirror = {0u, 2u, 1u};
    geom::SymmetrizeParams params;
    params.axis = 1;
    ModelPlan plan = geom::PlanSymmetrize(mesh, pointsOf(mesh), params, mirror);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        const std::span<const geom::Influence> skin = mesh.skin.forVertex(v);
        REQUIRE(skin.size() == 1u);
        // Below the plane is the mirrored bone; on it and above, the original.
        CHECK(skin[0].bone == (at[v].y < -1e-4f ? 2u : 1u));
    }
}

TEST_CASE("wem tools bridge joins two faces", "[wem][geometry][tools]") {
    // §3.16's Polygon form: two faces replaced by a band of quads between their
    // rims. Two squares facing each other and a third off on its own, so the
    // "every face but those two" half of the rebuild is exercised too.
    const std::vector<Vector3f> places = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, // a square at z = 0
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}, // one above it
        {0, 0, 5}, {1, 0, 5}, {1, 1, 5}, {0, 1, 5}, // one nothing touches
    };
    const std::vector<std::vector<u32>> three = {{0, 3, 2, 1}, {4, 5, 6, 7}, {8, 9, 10, 11}};
    Mesh mesh = fromFaces(three, places);
    ModelPlan plan = geom::PlanBridgeFaces(mesh, pointsOf(mesh), FaceId(0), FaceId(1));
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(liveFaces(mesh) == 5u); // the band of four, and the face left alone
    CHECK(plan.changed == 4u);
    u32 border = 0, edges = 0, vertices = 0;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e) {
        if (mesh.topology().isDeleted(EdgeId(e))) {
            continue;
        }
        ++edges;
        border += mesh.topology().isBoundary(EdgeId(e)) ? 1u : 0u;
    }
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        vertices += mesh.topology().isDeleted(VertexId(v)) ? 0u : 1u;
    }
    // A tube: four walls, the two rims open, every wall edge shared but those.
    CHECK(vertices == 12u);
    CHECK(edges == 12u + 4u); // the tube's 4 + 4 + 4, and the lone square's 4
    CHECK(border == 8u + 4u); // the tube's two rims, and the lone square's rim
    // The band really is a band: the vertex that was a corner of each square
    // now has two of its quads round it, not one face.
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v)) || at[v].z > 4.0f) {
            continue;
        }
        u32 round = 0;
        for (const HalfedgeId h : mesh.topology().voh(VertexId(v))) {
            round += mesh.topology().face(h).valid() ? 1u : 0u;
        }
        CHECK(round == 2u); // two of the band's quads meet at each rim corner
    }

    // One face twice; two that share an edge; two of different valence; more
    // segments than are built; a face that is not there.
    Mesh again = fromFaces(three, places);
    CHECK(geom::PlanBridgeFaces(again, pointsOf(again), FaceId(0), FaceId(0)).refusal ==
          ModelRefusal::SameLoop);
    Mesh odd = fromFaces({{0, 3, 2, 1}, {4, 5, 6}}, places);
    CHECK(geom::PlanBridgeFaces(odd, pointsOf(odd), FaceId(0), FaceId(1)).refusal ==
          ModelRefusal::LoopCountsDiffer);
    geom::BridgeParams two;
    two.segments = 2;
    CHECK(geom::PlanBridgeFaces(again, pointsOf(again), FaceId(0), FaceId(1), two).refusal ==
          ModelRefusal::NotBuiltYet);
    CHECK(geom::PlanBridgeFaces(again, pointsOf(again), FaceId(0), FaceId(99)).refusal ==
          ModelRefusal::EmptySelection);

    // Which way round the band faces is NOT settled here, and this fixture
    // cannot say: a face with no neighbour is the whole surface, so once it is
    // gone both orientations of the band are equally manifold. What settles it is
    // a neighbour, and the tunnel case below has eight of them.

    // A closed box's two opposite faces cannot be bridged: the band would be
    // the four walls that are already there, face for face, and a mesh does not
    // hold a face twice. Refused rather than repaired, which is the whole point
    // of a plan that checks its own rebuild.
    const std::vector<std::vector<u32>> cube = {
        {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    Mesh box = fromFaces(cube, places);
    CHECK(geom::PlanBridgeFaces(box, pointsOf(box), FaceId(0), FaceId(1)).refusal ==
          ModelRefusal::WouldFold);
    // And two faces of it that share an edge share vertices.
    Mesh sides = fromFaces(cube, places);
    CHECK(geom::PlanBridgeFaces(sides, pointsOf(sides), FaceId(0), FaceId(2)).refusal ==
          ModelRefusal::SameLoop);
}

TEST_CASE("wem tools bridge tunnels through one surface", "[wem][geometry][tools]") {
    // The case two loose shells cannot catch: two faces of ONE connected
    // surface. The band then has neighbours to agree with, so a band wound the
    // wrong way leaves every rim vertex with two fans and the repair splits all
    // of them -- which this refuses rather than repairs. A cylinder's opposite
    // sides, which is a tunnel straight through it.
    geom::PrimitiveParams params;
    params.size = {1.0f, 1.0f, 2.0f};
    params.sides = 8;
    Mesh mesh = geom::MakeCylinder(params);
    REQUIRE(mesh.ensureConnectivity().ok());
    const u32 before = liveFaces(mesh);
    ModelPlan plan = geom::PlanBridgeFaces(mesh, pointsOf(mesh), FaceId(0), FaceId(4));
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    CHECK(plan.changed == 4u);
    CHECK(liveFaces(mesh) == before - 2u + 4u);
    // Still closed: the tunnel took two faces away and gave four back, and every
    // edge has two faces, which is what says the band met the surface properly.
    u32 border = 0;
    for (u32 e = 0; e < mesh.topology().edgeCount(); ++e)
        if (!mesh.topology().isDeleted(EdgeId(e)) && mesh.topology().isBoundary(EdgeId(e)))
            ++border;
    CHECK(border == 0u);

    // Two faces with only one between them: the band's own connecting edges are
    // that face's, and a mesh does not hold an edge three times. Refused, not
    // repaired -- and the same is true of the two end caps, where `PlanBridge`
    // refuses the delete-then-bridge route for exactly the same reason.
    Mesh near = geom::MakeCylinder(params);
    REQUIRE(near.ensureConnectivity().ok());
    CHECK(geom::PlanBridgeFaces(near, pointsOf(near), FaceId(0), FaceId(2)).refusal ==
          ModelRefusal::WouldFold);
}

TEST_CASE("wem tools symmetrize mirrors about a plane it is given", "[wem][geometry][tools]") {
    // The same grid as the axis case, mirrored about y = 1 rather than y = 0:
    // an offset plane is what a model whose symmetry is not the origin's needs,
    // and the axis form cannot say it at all.
    Mesh mesh = grid(2, 3, /*quads=*/true);
    {
        const std::span<Vector3f> at =
            mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex);
        for (Vector3f& place : at) {
            place.y -= 1.0f; // y now runs -1 .. 2
        }
    }
    bindMesh(mesh, {});
    geom::SymmetrizeParams params;
    params.axis = 1;
    params.fromPositive = true;
    params.origin = {0.0f, 1.0f, 0.0f};
    params.normal = {0.0f, 1.0f, 0.0f};
    ModelPlan plan = geom::PlanSymmetrize(mesh, pointsOf(mesh), params);
    INFO("refusal " << geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    finish(mesh, plan);
    CHECK(errors(mesh) == "");
    // One row of quads above y = 1 is kept, and its mirror is one more.
    CHECK(liveFaces(mesh) == 4u);
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    f32 lowest = 1e9f;
    f32 highest = -1e9f;
    u32 onPlane = 0;
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.topology().isDeleted(VertexId(v))) {
            continue;
        }
        lowest = std::min(lowest, at[v].y);
        highest = std::max(highest, at[v].y);
        onPlane += std::abs(at[v].y - 1.0f) < 1e-4f ? 1u : 0u;
    }
    CHECK(lowest == Catch::Approx(0.0f).margin(1e-4));
    CHECK(highest == Catch::Approx(2.0f).margin(1e-4));
    CHECK(onPlane == 3u); // the seam is shared, not doubled

    // A plane square to no axis: the halves still meet on it, so every vertex
    // off it has a twin the same distance the other side.
    Mesh tilted = grid(2, 2, /*quads=*/true);
    bindMesh(tilted, {});
    geom::SymmetrizeParams slanted;
    slanted.axis = 0;
    slanted.origin = {1.0f, 1.0f, 0.0f};
    slanted.normal = {1.0f, 1.0f, 0.0f};
    ModelPlan across = geom::PlanSymmetrize(tilted, pointsOf(tilted), slanted);
    INFO("refusal " << geom::ToString(across.refusal));
    REQUIRE_FALSE(across.refused());
    finish(tilted, across);
    CHECK(errors(tilted) == "");
    const auto after = tilted.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const Vector3f unit = slanted.normal.normalized();
    u32 mirrored = 0;
    for (u32 v = 0; v < tilted.vertexCount(); ++v) {
        if (tilted.topology().isDeleted(VertexId(v))) {
            continue;
        }
        const f32 side = (after[v] - slanted.origin).dot(unit);
        if (std::abs(side) < 1e-4f) {
            continue;
        }
        const Vector3f reflected = after[v] - unit * (2.0f * side);
        bool twinned = false;
        for (u32 w = 0; w < tilted.vertexCount() && !twinned; ++w) {
            twinned = !tilted.topology().isDeleted(VertexId(w)) &&
                      (after[w] - reflected).length() < 1e-4f;
        }
        INFO("vertex " << v << " at " << after[v].x << ", " << after[v].y << ", " << after[v].z);
        CHECK(twinned);
        ++mirrored;
    }
    CHECK(mirrored > 0u); // the plane really does have a side
}

TEST_CASE("wem tools make planar takes a plane it is given", "[wem][geometry][tools]") {
    // The plane the caller names, rather than the axis plane through the
    // selection's own centre: what a plane widget a hand has placed asks for.
    const std::vector<Vector3f> places = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0.4f}, {0, 1, 0}};
    Mesh mesh = fromFaces({{0, 1, 2, 3}}, places);
    ElementSet every;
    every.vertices = {0, 1, 2, 3};
    ModelPlan onto = geom::PlanMakePlanar(mesh, pointsOf(mesh), every, Vector3f{0.0f, 0.0f, 0.25f},
                                          Vector3f{0.0f, 0.0f, 2.0f});
    INFO("refusal " << geom::ToString(onto.refusal));
    REQUIRE_FALSE(onto.refused());
    geom::ApplyAmount(mesh, onto, 1.0f);
    finish(mesh, onto);
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < 4; ++v) {
        // Onto z = 0.25, not onto the centre's own 0.1, and an unnormalised
        // normal names the same plane.
        CHECK(at[v].z == Catch::Approx(0.25f).margin(1e-4));
    }
    // A zero normal is no plane at all.
    Mesh flat = fromFaces({{0, 1, 2, 3}}, places);
    CHECK(geom::PlanMakePlanar(flat, pointsOf(flat), every, Vector3f{0, 0, 0}, Vector3f{0, 0, 0})
              .refusal == ModelRefusal::ZeroAmount);
}

TEST_CASE("wem tools to quads and to polygons say which edges went", "[wem][geometry][tools]") {
    // A count says how many edges a join takes; `dissolvedEdges` says which, in
    // the numbering the mesh came in with, so a preview planned on a copy can
    // light exactly the edges the press would take away (UX §7.5).
    Mesh triangles = grid(3, 3);
    ModelPlan joined = geom::PlanJoinTriangles(triangles, pointsOf(triangles), allFaces(triangles));
    INFO("refusal " << geom::ToString(joined.refusal));
    REQUIRE_FALSE(joined.refused());
    CHECK(joined.changed == 9u); // nine cells, each a pair
    CHECK(joined.dissolvedEdges.size() == joined.changed);
    for (const u32 e : joined.dissolvedEdges) {
        REQUIRE(e < triangles.topology().edgeCount());
        // Named in the input's numbering, and gone from it: Plan is what makes
        // the topology, so the edge is already deleted by the time this is read.
        CHECK(triangles.topology().isDeleted(EdgeId(e)));
    }
    std::vector<u32> sorted = joined.dissolvedEdges;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    // To Polygons the same way: one entry per dissolve, all of them gone.
    Mesh quads = grid(3, 3, /*quads=*/true);
    geom::LimitedDissolveParams loose;
    loose.angle = 1.0f; // every interior edge of a flat grid is flat enough
    ModelPlan dissolved = geom::PlanLimitedDissolve(quads, pointsOf(quads), allFaces(quads), loose);
    INFO("refusal " << geom::ToString(dissolved.refusal));
    REQUIRE_FALSE(dissolved.refused());
    CHECK(dissolved.changed > 0u);
    CHECK(dissolved.dissolvedEdges.size() == dissolved.changed);
    for (const u32 e : dissolved.dissolvedEdges) {
        REQUIRE(e < quads.topology().edgeCount());
        CHECK(quads.topology().isDeleted(EdgeId(e)));
    }
    // A plan that dissolves nothing names nothing. Two quads folded square to
    // each other, at an angle that allows none: a flat grid would dissolve at
    // any angle at all, because flat is 0 degrees and 0 <= 0.
    const std::vector<Vector3f> bent = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0},
                                        {0, 1, 0}, {1, 0, 1}, {1, 1, 1}};
    Mesh fold = fromFaces({{0, 1, 2, 3}, {1, 4, 5, 2}}, bent);
    geom::LimitedDissolveParams none;
    none.angle = 0.0f;
    const ModelPlan nothing = geom::PlanLimitedDissolve(fold, pointsOf(fold), allFaces(fold), none);
    CHECK(nothing.changed == 0u);
    CHECK(nothing.dissolvedEdges.empty());
}

TEST_CASE("wem tools make planar flattens what it is given", "[wem][geometry][tools]") {
    // A quad with one corner lifted: the fit puts all four on one plane, and
    // an axis plane puts them on the centre's own height.
    const std::vector<Vector3f> places = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0.4f}, {0, 1, 0}};
    Mesh mesh = fromFaces({{0, 1, 2, 3}}, places);
    ElementSet every;
    every.vertices = {0, 1, 2, 3};
    ModelPlan fit = geom::PlanMakePlanar(mesh, pointsOf(mesh), every, 3);
    INFO("refusal " << geom::ToString(fit.refusal));
    REQUIRE_FALSE(fit.refused());
    geom::ApplyAmount(mesh, fit, 1.0f);
    finish(mesh, fit);
    CHECK(errors(mesh) == "");
    // Flat: every corner is on one plane, so the two triangles of its own cut
    // have the same normal.
    const auto at = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    const Vector3f first = cross(at[1] - at[0], at[2] - at[0]);
    const Vector3f second = cross(at[2] - at[0], at[3] - at[0]);
    const f32 lengths = first.length() * second.length();
    REQUIRE(lengths > 1e-9f);
    CHECK(first.dot(second) / lengths == Catch::Approx(1.0f).margin(1e-4));

    // Square to Z: every corner takes the centre's height.
    Mesh flat = fromFaces({{0, 1, 2, 3}}, places);
    ModelPlan axis = geom::PlanMakePlanar(flat, pointsOf(flat), every, 2);
    REQUIRE_FALSE(axis.refused());
    geom::ApplyAmount(flat, axis, 1.0f);
    finish(flat, axis);
    const auto after = flat.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    for (u32 v = 0; v < 4; ++v) {
        CHECK(after[v].z == Catch::Approx(0.1f).margin(1e-4));
    }
}

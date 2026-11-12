// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The Mesh workspace audit, group B: the tools that add topology -- Extrude,
/// Extrude Border, Inset, Hinge, Chamfer, Insert Vertex, Divide, Connect, Cut,
/// Slice, Bridge, Create Polygon -- and the cross-cutting X2-X13
/// (EDIT_MODE_MESH_AUDIT_PLAN.md §3, §4.B in WhiteoutFlakes). Each test is named
/// after its suspect; a confirmed one is `[!shouldfail]` until its fix.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>

#include "wem_audit_fixtures.h"

using namespace audit;

namespace {

using PlanFn = std::function<ModelPlan(Mesh&, const PointTable&)>;

/// A plan run to its end on a copy, keeping the plan (Run keeps only the mesh).
struct Done {
    Mesh mesh;
    ModelPlan plan;
};
Done Apply(const Mesh& in, const PlanFn& plan, std::optional<f32> amount = std::nullopt,
           f32 crease = geom::kDefaultShadingAngle) {
    Done done{in, {}};
    const PointTable points = PointsOf(done.mesh);
    done.plan = plan(done.mesh, points);
    if (done.plan.refused()) {
        return done;
    }
    if (amount) {
        geom::AmountRange(done.mesh, done.plan);
        geom::ApplyAmount(done.mesh, done.plan, *amount);
    }
    geom::FinishTool(done.mesh, done.plan, crease);
    return done;
}

/// Points holding two or more live vertices.
u32 JoinedPoints(const Mesh& mesh) {
    const PointTable points = PointsOf(mesh);
    u32 out = 0;
    for (u32 p = 0; p < points.pointCount; ++p) {
        u32 live = 0;
        for (const u32 v : points.membersOf(p)) {
            live += mesh.topology().isDeleted(VertexId(v)) ? 0u : 1u;
        }
        out += live > 1 ? 1u : 0u;
    }
    return out;
}

/// Live vertices within 1e-5 of (x, y, z).
u32 CountAt(const Mesh& mesh, f32 x, f32 y, f32 z = 0.0f) {
    const auto p = Positions(mesh);
    u32 out = 0;
    for (u32 v = 0; v < p.size(); ++v) {
        if (!mesh.topology().isDeleted(VertexId(v)) && std::abs(p[v].x - x) < 1e-5f &&
            std::abs(p[v].y - y) < 1e-5f && std::abs(p[v].z - z) < 1e-5f) {
            ++out;
        }
    }
    return out;
}

/// Live vertices no face uses.
u32 Isolated(const Mesh& mesh) {
    const Topology& t = mesh.topology();
    u32 out = 0;
    for (u32 v = 0; v < t.vertexCount(); ++v) {
        if (t.isDeleted(VertexId(v))) {
            continue;
        }
        bool used = false;
        if (t.outgoing(VertexId(v)).valid()) {
            for (const HalfedgeId h : t.voh(VertexId(v))) {
                used = used || t.face(h).valid();
            }
        }
        out += used ? 0u : 1u;
    }
    return out;
}

/// Newell's vector of face @p f (twice its area along its normal).
Vector3f AreaVector(const Mesh& mesh, u32 f) {
    const Topology& t = mesh.topology();
    const auto p = Positions(mesh);
    Vector3f sum{0, 0, 0};
    for (const HalfedgeId h : t.fh(FaceId(f))) {
        sum = sum + Cross(p[t.from(h).value()], p[t.to(h).value()]);
    }
    return sum;
}

/// Live faces facing against @p want (flat fixtures: a fold).
u32 Inverted(const Mesh& mesh, const Vector3f& want) {
    u32 out = 0;
    for (const u32 f : AllFaces(mesh).faces) {
        const Vector3f a = AreaVector(mesh, f);
        out += a.x * want.x + a.y * want.y + a.z * want.z < -1e-9f ? 1u : 0u;
    }
    return out;
}

bool Sharp(const Mesh& mesh, u32 e) {
    const auto sharp = mesh.attributes.get<const u8>(geom::names::kSharp, Domain::Edge);
    return e < sharp.size() && sharp[e] != 0;
}

/// Live edges that run between two heights (the prisms' uprights) and are sharp.
u32 SharpUprights(const Mesh& mesh) {
    const Topology& t = mesh.topology();
    const auto p = Positions(mesh);
    u32 out = 0;
    for (u32 e = 0; e < t.edgeCount(); ++e) {
        if (t.isDeleted(EdgeId(e))) {
            continue;
        }
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        if (p[t.from(h).value()].z != p[t.to(h).value()].z && Sharp(mesh, e)) {
            ++out;
        }
    }
    return out;
}

void MarkSeam(Mesh& mesh, u32 e) {
    mesh.attributes.getOrCreate<u8>(geom::names::kSeam, Domain::Edge, geom::AttrType::Bool)[e] = 1;
}

/// Whether every corner of face @p f sits on one UV island (+10 or not).
bool OneIsland(const Mesh& mesh, u32 f) {
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    u32 shifted = 0, plain = 0;
    for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
        (uv[h.index()].x > 5.0f ? shifted : plain)++;
    }
    return shifted == 0 || plain == 0;
}

PlanFn Slice(const ElementSet& faces, Vector3f origin, Vector3f normal, geom::SliceMode mode) {
    geom::SliceParams params;
    params.origin = origin;
    params.normal = normal;
    params.mode = mode;
    return [faces, params](Mesh& m, const PointTable& p) { return geom::PlanSlice(m, p, faces, params); };
}
PlanFn Extrude(const ElementSet& faces, geom::ExtrudeType type = geom::ExtrudeType::Group) {
    return [faces, type](Mesh& m, const PointTable& p) { return geom::PlanExtrudeFaces(m, p, faces, type); };
}
PlanFn Inset(const ElementSet& faces, geom::InsetType type = geom::InsetType::Group) {
    return [faces, type](Mesh& m, const PointTable& p) { return geom::PlanInset(m, p, faces, type); };
}
PlanFn Chamfer(const ElementSet& edges, geom::ChamferParams params = {}) {
    return [edges, params](Mesh& m, const PointTable& p) { return geom::PlanChamferEdges(m, p, edges, params); };
}
PlanFn ChamferV(const ElementSet& vertices) {
    return [vertices](Mesh& m, const PointTable& p) { return geom::PlanChamferVertices(m, p, vertices); };
}
PlanFn Border(const ElementSet& edges) {
    return [edges](Mesh& m, const PointTable& p) { return geom::PlanExtrudeBorder(m, p, edges); };
}
PlanFn Connect(const ElementSet& edges) {
    return [edges](Mesh& m, const PointTable& p) { return geom::PlanConnectEdges(m, p, edges); };
}
PlanFn InsertVertex(u32 edge, f32 t) {
    return [edge, t](Mesh& m, const PointTable&) { return geom::PlanInsertVertex(m, EdgeId(edge), t); };
}

constexpr f32 kDegrees = 0.0174532925f;

} // namespace

// ============================================================================
// Slice (SLC-1 first: a suspected loss)
// ============================================================================

TEST_CASE("audit SLC-1: Slice Remove drops only selected faces on the far side", "[wem][audit][slice]") {
    // 4 x 2 grid, row 0 selected, plane x = 2.5 keeping +x. Row 1 is not
    // selected, so its faces at x < 2.5 must stay.
    const Mesh mesh = QuadGrid(4, 2);
    Mesh out;
    const Verdict verdict =
        Run(mesh, Slice(Faces({0, 1, 2, 3}), {2.5f, 0, 0}, {1, 0, 0}, geom::SliceMode::Remove), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " faces " << LiveFaces(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(VertexAt(out, 0.0f, 2.0f) != kInvalidId);
    CHECK(VertexAt(out, 1.0f, 2.0f) != kInvalidId);
    CHECK(LiveFaces(out) == 6u); // row 0: the right half of (2,0) and (3,0); row 1 whole
}

TEST_CASE("audit SLC-2: Slice Split leaves unselected faces joined", "[wem][audit][slice]") {
    // A: one quad [0,2]x[0,1], selected, cut at x = 1. B: two quads meeting
    // along x = 1 at y in [3,4], not selected: their shared edge must stay.
    const Mesh mesh = FromFaces({{0, 1, 2, 3}, {4, 5, 8, 7}, {5, 6, 9, 8}},
                                {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}, {0, 3, 0}, {1, 3, 0},
                                 {2, 3, 0}, {0, 4, 0}, {1, 4, 0}, {2, 4, 0}});
    Mesh out;
    const Verdict verdict =
        Run(mesh, Slice(Faces({0}), {1, 0, 0}, {1, 0, 0}, geom::SliceMode::Split), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(CountAt(out, 1, 3) == 1u);
    CHECK(CountAt(out, 1, 4) == 1u);
    CHECK(CountAt(out, 1, 0) == 2u); // the selected quad did part
}

TEST_CASE("audit SLC-3: Slice Split along an existing edge loop parts the mesh there", "[wem][audit][slice]") {
    // 2 x 1 grid, plane x = 1 through the middle edge: the centre loop of a
    // symmetric model. Max splits along it.
    const Mesh mesh = QuadGrid(2, 1);
    Mesh out;
    const Verdict verdict = Run(mesh, Slice({}, {1, 0, 0}, {1, 0, 0}, geom::SliceMode::Split), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(CountAt(out, 1, 0) == 2u);
    CHECK(CountAt(out, 1, 1) == 2u);
}

TEST_CASE("audit SLC-3: Slice Remove along an existing edge loop drops one half", "[wem][audit][slice]") {
    const Mesh mesh = QuadGrid(2, 1);
    Mesh out;
    const Verdict verdict = Run(mesh, Slice({}, {1, 0, 0}, {1, 0, 0}, geom::SliceMode::Remove), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 1u);
    CHECK(VertexAt(out, 0, 0) == kInvalidId);
}

TEST_CASE("audit SLC-4: Slice's snap never moves a vertex outside the selection", "[wem][audit][slice]") {
    // Row 0 of a 4 x 2 grid sliced at x = 2.5. Vertex (2, 2), used by row 1
    // only, is put a third of the tolerance off the plane.
    Mesh mesh = QuadGrid(4, 2);
    const f32 tolerance = geom::CoincidenceTolerance(mesh);
    const f32 x = 2.5f + 0.3f * tolerance;
    Positions(mesh)[12] = Vector3f{x, 2.0f, 0.0f};
    REQUIRE(x != 2.5f);
    Mesh out;
    const Verdict verdict =
        Run(mesh, Slice(Faces({0, 1, 2, 3}), {2.5f, 0, 0}, {1, 0, 0}, geom::SliceMode::Cut), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " tolerance " << tolerance);
    REQUIRE_FALSE(verdict.refused);
    CHECK(VertexAt(out, x, 2.0f) != kInvalidId);
}

TEST_CASE("audit SLC-5: Slice Split leaves no loose copy behind", "[wem][audit][slice]") {
    // A unit quad cut at x = 0.5, and a quad [0.5,1.5]x[2,3] whose left edge
    // lies on the plane with nothing on the negative side.
    const Mesh mesh = FromFaces({{0, 1, 2, 3}, {4, 5, 6, 7}}, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                                               {0.5f, 2, 0}, {1.5f, 2, 0}, {1.5f, 3, 0}, {0.5f, 3, 0}});
    Mesh out;
    const Verdict verdict = Run(mesh, Slice({}, {0.5f, 0, 0}, {1, 0, 0}, geom::SliceMode::Split), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(Isolated(out) == 0u);
    CHECK(CountAt(out, 0.5f, 2) == 1u);
}

// ============================================================================
// Chamfer UVs (CHF-1, CHF-2)
// ============================================================================

TEST_CASE("audit CHF-1: a chamfer strip's corners keep the UV field", "[wem][audit][chamfer]") {
    // F18 on a 2 x 1 grid, no seam; the middle edge at 0.25. Every strip
    // corner must read uv = f(pos), colour = g(pos), skin = the linear field.
    Mesh mesh = QuadGrid(2, 1);
    Fields(mesh, 2.0f);
    Contract contract;
    contract.skinWidth = 2.0f;
    Mesh out;
    const Verdict verdict =
        Run(mesh, Chamfer(Edges({EdgeAt(mesh, 1, 0, 1, 1)})), contract, &out, geom::kDefaultShadingAngle, 0.25f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " uv " << UvFieldError(out) << " colour "
                                         << ColorFieldError(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(UvFieldError(out) < 1e-4f);
    CHECK(ColorFieldError(out) <= 1u);
}

namespace {
/// F9 on a 2 x 1 grid with the seam marked on its middle edge; @p reversed
/// numbers the right face 0.
Mesh SeamStrip(bool reversed) {
    const Mesh grid = QuadGrid(2, 1);
    std::vector<std::vector<u32>> faces = CornerLists(grid.faceSet());
    if (reversed) {
        std::reverse(faces.begin(), faces.end());
    }
    std::vector<Vector3f> p(Positions(grid).begin(), Positions(grid).end());
    Mesh mesh = FromFaces(faces, p);
    SeamUvs(mesh, 1.0f);
    MarkSeam(mesh, EdgeAt(mesh, 1, 0, 1, 1));
    return mesh;
}
} // namespace

TEST_CASE("audit CHF-2: a seam's chamfer strip lies on one island, whichever face is numbered first",
          "[wem][audit][chamfer]") {
    for (const bool reversed : {false, true}) {
        const Mesh mesh = SeamStrip(reversed);
        const Done done = Apply(mesh, Chamfer(Edges({EdgeAt(mesh, 1, 0, 1, 1)})), 0.25f);
        INFO("reversed " << reversed << " " << geom::ToString(done.plan.refusal) << " uv "
                         << UvFieldError(done.mesh));
        REQUIRE_FALSE(done.plan.refused());
        REQUIRE(done.plan.selection.faces.size() == 1u);
        CHECK(OneIsland(done.mesh, done.plan.selection.faces.front()));
        CHECK(UvFieldError(done.mesh) < 1e-4f);
    }
}

// ============================================================================
// The crease rule (X7 kernel half, X8)
// ============================================================================

TEST_CASE("audit X7: FinishTool honours the crease angle it is given", "[wem][audit][crease]") {
    // A face of a 3 x 3 grid extruded 0.5: its walls turn 90 degrees. At a
    // crease of 100 nothing hardens; at the default 60 the rims do.
    const Mesh mesh = QuadGrid(3, 3);
    const Done wide = Apply(mesh, Extrude(Faces({4})), 0.5f, 100.0f * kDegrees);
    const Done narrow = Apply(mesh, Extrude(Faces({4})), 0.5f);
    REQUIRE_FALSE(wide.plan.refused());
    CHECK(SharpCount(wide.mesh) == 0u);
    CHECK(SharpCount(narrow.mesh) > 0u);
}

TEST_CASE("audit X8: a Loop Cut on a smooth prism leaves its soft uprights soft", "[wem][audit][crease]") {
    // F13: five sides at 72 degrees, uprights soft. A loop cut round the lower
    // band splits each upright; Q7: the crease rule never hardens a soft edge.
    const Mesh mesh = SmoothPrism(5);
    REQUIRE(SharpUprights(mesh) == 0u);
    std::vector<u32> ring;
    for (u32 s = 0; s < 5; ++s) {
        ring.push_back(EdgeOf(mesh, s, 5 + s));
    }
    const Done done = Apply(mesh, Connect(Edges(ring)), 0.0f);
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    CHECK(SharpUprights(done.mesh) == 0u);
}

// ============================================================================
// X2: coincident copies are points of their own (Q8)
// ============================================================================

TEST_CASE("audit X2: Extrude applied at 0 leaves no copy in its source's point", "[wem][audit][x2]") {
    const Mesh mesh = QuadGrid(3, 3);
    const Done done = Apply(mesh, Extrude(Faces({4})), 0.0f);
    REQUIRE_FALSE(done.plan.refused());
    CHECK(JoinedPoints(done.mesh) == 0u);
}

TEST_CASE("audit X2: Inset applied at 0 leaves no copy in its source's point", "[wem][audit][x2]") {
    const Mesh mesh = QuadGrid(3, 3);
    const Done done = Apply(mesh, Inset(Faces({4})), 0.0f);
    REQUIRE_FALSE(done.plan.refused());
    CHECK(JoinedPoints(done.mesh) == 0u);
}

TEST_CASE("audit X2: Chamfer applied at 0 leaves no two points in one place", "[wem][audit][x2]") {
    // Q3: a Chamfer at 0 changes nothing and says so, so nothing is left in
    // one place.
    const Mesh mesh = QuadGrid(3, 3);
    const Done done = Apply(mesh, Chamfer(Edges({EdgeAt(mesh, 1, 1, 2, 1)})), 0.0f);
    CHECK(done.plan.refusal == ModelRefusal::ZeroAmount);
    CHECK(JoinedPoints(done.mesh) == 0u);
}

TEST_CASE("audit X2: Connect slid to 1 leaves no vertex on its neighbour", "[wem][audit][x2]") {
    const Mesh mesh = QuadGrid(1, 1);
    const Done done = Apply(mesh, Connect(Edges({EdgeAt(mesh, 0, 0, 1, 0), EdgeAt(mesh, 0, 1, 1, 1)})), 1.0f);
    REQUIRE_FALSE(done.plan.refused());
    CHECK(JoinedPoints(done.mesh) == 0u);
}

TEST_CASE("audit X2: Insert Vertex at t = 0 or 1 leaves no vertex on its neighbour", "[wem][audit][x2]") {
    for (const f32 t : {0.0f, 1.0f}) {
        const Mesh mesh = QuadGrid(2, 1);
        const Done done = Apply(mesh, InsertVertex(EdgeAt(mesh, 1, 0, 1, 1), t));
        INFO("t " << t << " " << geom::ToString(done.plan.refusal));
        if (!done.plan.refused()) {
            CHECK(JoinedPoints(done.mesh) == 0u);
        }
    }
}

TEST_CASE("audit X2: Slice Split's copies are points of their own", "[wem][audit][x2]") {
    const Mesh mesh = QuadGrid(2, 1);
    const Done done = Apply(mesh, Slice({}, {0.5f, 0, 0}, {1, 0, 0}, geom::SliceMode::Split));
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    CHECK(CountAt(done.mesh, 0.5f, 0) == 2u);
    CHECK(JoinedPoints(done.mesh) == 0u);
    CHECK(PointsAcrossShells(done.mesh) == 0u);
}

// ============================================================================
// X3, X4, X5: the amount's ends
// ============================================================================

TEST_CASE("audit X3: an amount tool applied at 0 keeps its walls; a Chamfer changes nothing (Q3)",
          "[wem][audit][amount]") {
    // Q3 (decided 2026-11-12): Extrude, Extrude Border, Inset and Hinge at 0
    // keep their zero-height walls, as Max and Blender do ("extrude 0, then
    // scale"), and the commit is otherwise clean (no stale row, EXT-7). A
    // Chamfer at 0 changes nothing and says so, as Blender's Bevel does.
    const Mesh grid = QuadGrid(3, 3);
    const Mesh hole = QuadHole();
    std::vector<u32> rim;
    for (const u32 e : BorderEdges(hole).edges) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const auto p = Positions(hole);
        const Vector3f a = p[hole.topology().from(h).value()], b = p[hole.topology().to(h).value()];
        if (a.x >= 1 && a.x <= 2 && a.y >= 1 && a.y <= 2 && b.x >= 1 && b.x <= 2 && b.y >= 1 && b.y <= 2) {
            rim.push_back(e);
        }
    }
    REQUIRE(rim.size() == 4u);
    const PlanFn hinge = [&](Mesh& m, const PointTable& p) {
        return geom::PlanHingeFaces(m, p, Faces({4}), EdgeAt(m, 1, 1, 2, 1));
    };
    const std::vector<std::pair<const char*, std::pair<const Mesh*, PlanFn>>> walls = {
        {"extrude", {&grid, Extrude(Faces({4}))}},
        {"inset", {&grid, Inset(Faces({4}))}},
        {"border", {&hole, Border(Edges(rim))}},
        {"hinge", {&grid, hinge}},
    };
    Contract zeroWalls;
    zeroWalls.degenerateOk = true; // the walls are zero-height by design
    for (const auto& [name, tool] : walls) {
        Mesh out;
        const Verdict verdict = Run(*tool.first, tool.second, zeroWalls, &out, geom::kDefaultShadingAngle, 0.0f);
        INFO(name << ": " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        CHECK_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
        CHECK(LiveFaces(out) > LiveFaces(*tool.first)); // the walls are there
    }
    const Done chamfer = Apply(grid, Chamfer(Edges({EdgeAt(grid, 1, 1, 2, 1)})), 0.0f);
    INFO("chamfer: " << geom::ToString(chamfer.plan.refusal));
    CHECK(chamfer.plan.refusal == ModelRefusal::ZeroAmount);
    CHECK(SameReflected(chamfer.mesh, grid));
}

TEST_CASE("audit X4: Inset past its range commits no zero-area face", "[wem][audit][amount]") {
    // A unit quad inset by 0.7: the range ends at 0.5, where the inner quad is
    // four coincident vertices.
    const Mesh mesh = QuadGrid(1, 1);
    Mesh out;
    const Verdict verdict = Run(mesh, Inset(Faces({0})), {}, &out, geom::kDefaultShadingAngle, 0.7f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit X4: Chamfer past its range commits no zero-area face", "[wem][audit][amount]") {
    // Every edge of the unit cube at 0.7; the range ends at 0.5.
    const Mesh mesh = Box();
    Mesh out;
    const Verdict verdict = Run(mesh, Chamfer(AllEdges(mesh)), {}, &out, geom::kDefaultShadingAngle, 0.7f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit X4: Extrude Border past its range commits no zero-area face", "[wem][audit][amount]") {
    // F15's square hole closed inward by 1.0; the far edges meet first.
    const Mesh mesh = QuadHole();
    std::vector<u32> rim;
    const auto p = Positions(mesh);
    for (const u32 e : BorderEdges(mesh).edges) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        const Vector3f a = p[mesh.topology().from(h).value()];
        if (a.x >= 1 && a.x <= 2 && a.y >= 1 && a.y <= 2) {
            const Vector3f b = p[mesh.topology().to(h).value()];
            if (b.x >= 1 && b.x <= 2 && b.y >= 1 && b.y <= 2) {
                rim.push_back(e);
            }
        }
    }
    REQUIRE(rim.size() == 4u);
    Mesh out;
    const Verdict verdict = Run(mesh, Border(Edges(rim)), {}, &out, geom::kDefaultShadingAngle, 1.0f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit X5: Chamfer's range stops at 0 (Q4)", "[wem][audit][amount]") {
    // 2 x 1 grid's middle edge at -0.25: Max clamps at 0, so the strip never
    // folds over its own sides.
    // Clamped to 0, the chamfer changes nothing and says so (Q3).
    const Mesh mesh = QuadGrid(2, 1);
    const Done done = Apply(mesh, Chamfer(Edges({EdgeAt(mesh, 1, 0, 1, 1)})), -0.25f);
    INFO("amountMin " << done.plan.amountMin << " " << geom::ToString(done.plan.refusal));
    CHECK(done.plan.amountMin >= 0.0f);
    CHECK(done.plan.refusal == ModelRefusal::ZeroAmount);
    CHECK(Inverted(done.mesh, {0, 0, 1}) == 0u);
}

// ============================================================================
// Extrude (EXT-1..EXT-5)
// ============================================================================

namespace {

Vector3f Unit(const Vector3f& v) {
    const f32 l = Length(v);
    return l > 0.0f ? v * (1.0f / l) : v;
}

/// @p v turned by @p degrees about the unit @p k (Rodrigues).
Vector3f Turned(const Vector3f& v, const Vector3f& k, f32 degrees) {
    const f32 c = std::cos(degrees * kDegrees), s = std::sin(degrees * kDegrees);
    return v * c + Cross(k, v) * s + k * (k.dot(v) * (1.0f - c));
}

/// P15's turn: every position turned by 37 degrees about (1, 2, 3), re-shaded.
const Vector3f kTurnAxis = Unit(Vector3f{1, 2, 3});
Mesh Turned(Mesh mesh) {
    for (Vector3f& p : Positions(mesh)) {
        p = Turned(p, kTurnAxis, 37.0f);
    }
    Shade(mesh);
    return mesh;
}

/// F12 bent: quad A in z = 0 and quad B standing in y = 1 (facing -y), sharing
/// only the point (1, 1, 0).
Mesh BentTouchingQuads() {
    return FromFaces({{0, 1, 2, 3}, {4, 5, 6, 7}}, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                                    {1, 1, 0}, {2, 1, 0}, {2, 1, 1}, {1, 1, 1}});
}

/// The edge both faces @p a and @p b hold; kInvalidId when none.
u32 SharedEdge(const Mesh& mesh, u32 a, u32 b) {
    const Topology& t = mesh.topology();
    for (const HalfedgeId h : t.fh(FaceId(a))) {
        if (t.face(Topology::opposite(h)) == FaceId(b)) {
            return Topology::edge(h).value();
        }
    }
    return kInvalidId;
}

} // namespace

TEST_CASE("audit EXT-1: Extrude of a U round a border vertex's open fan gets two copies", "[wem][audit][extrude]") {
    // F16: faces 0 and 2 of the open fan, not 1. Each is its own run round
    // vertex 0, so each gets its own copy (X12).
    const Mesh mesh = OpenFan();
    Mesh out;
    const Verdict verdict = Run(mesh, Extrude(Faces({0, 2})), {}, &out, geom::kDefaultShadingAngle, 0.3f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit EXT-2: Extrude of both sides of a double-sided card makes two slabs", "[wem][audit][extrude]") {
    // F11 both faces at 0.5: Max lifts each along its own normal and walls it.
    const Mesh mesh = DoubleCard();
    Mesh out;
    const Verdict verdict = Run(mesh, Extrude(Faces({0, 1})), {}, &out, geom::kDefaultShadingAngle, 0.5f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " faces " << LiveFaces(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 10u);
    CHECK(CountAt(out, 0, 0, 0.5f) >= 1u);
    CHECK(CountAt(out, 0, 0, -0.5f) >= 1u);
}

TEST_CASE("audit EXT-3: Extrude keeps a soft edge inside the region soft", "[wem][audit][extrude]") {
    // F13's sides 0 and 1 (72 degrees apart, a soft upright between them).
    const Mesh mesh = SmoothPrism(5);
    const Done done = Apply(mesh, Extrude(Faces({0, 1})), 0.2f);
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    REQUIRE(done.plan.selection.faces.size() == 2u);
    const u32 inside = SharedEdge(done.mesh, done.plan.selection.faces[0], done.plan.selection.faces[1]);
    REQUIRE(inside != kInvalidId);
    CHECK_FALSE(Sharp(done.mesh, inside));
}

TEST_CASE("audit EXT-4: Local Normal moves each region's copy along its own faces", "[wem][audit][extrude]") {
    // Two quads sharing only (1,1,0): A faces +z, B faces -y. Max moves A's
    // corner to (1,1,0.5) and B's to (1,0.5,0), not both along their mean.
    const Mesh mesh = BentTouchingQuads();
    Mesh out;
    const Verdict verdict = Run(mesh, Extrude(Faces({0, 1}), geom::ExtrudeType::LocalNormal), {}, &out,
                                geom::kDefaultShadingAngle, 0.5f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(CountAt(out, 1, 1, 0.5f) == 1u);
    CHECK(CountAt(out, 1, 0.5f, 0) == 1u);
}

TEST_CASE("audit EXT-5: Extrude never falls back to world +Z (P15)", "[wem][audit][extrude]") {
    // F11 turned 37 degrees about (1,2,3), both faces extruded 0.5: whatever
    // the result, every vertex stays on the card's own normal line through a
    // corner -- nothing slides along world Z.
    const Mesh mesh = Turned(DoubleCard());
    const Vector3f normal = Turned(Vector3f{0, 0, 1}, kTurnAxis, 37.0f);
    const std::vector<Vector3f> corners(Positions(mesh).begin(), Positions(mesh).end());
    Mesh out;
    const Verdict verdict = Run(mesh, Extrude(Faces({0, 1})), {}, &out, geom::kDefaultShadingAngle, 0.5f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    f32 worst = 0.0f;
    for (const u32 v : AllVertices(out).vertices) {
        f32 best = 1e30f;
        for (const Vector3f& q : corners) {
            const Vector3f d = Positions(out)[v] - q;
            best = std::min(best, Length(d - normal * d.dot(normal)));
        }
        worst = std::max(worst, best);
    }
    INFO("off the normal lines by " << worst);
    CHECK(worst < 1e-4f);
}

// ============================================================================
// Inset (INS-1..INS-5), X12, X13
// ============================================================================

TEST_CASE("audit INS-1: Inset over a crack moves the twins' inner vertices too", "[wem][audit][inset]") {
    // F8 2 x 1 cut at x = 1, both faces at 0.25 (one region across the crack):
    // the inner ring runs (1, 0.25) and (1, 0.75), twice each. Those pairs are
    // the crack's twins carried on (Q5): one point across the two shells, as
    // the crack's own are, so P5 is off.
    const Mesh mesh = Crack(2, 1, 1);
    Mesh out;
    Contract crack;
    crack.joinsShellsOk = true;
    const Verdict verdict = Run(mesh, Inset(Faces({0, 1})), crack, &out, geom::kDefaultShadingAngle, 0.25f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(CountAt(out, 1, 0.25f) == 2u);
    CHECK(CountAt(out, 1, 0.75f) == 2u);
}

TEST_CASE("audit INS-2: Inset's range stops at 0 (Q4)", "[wem][audit][inset]") {
    const Mesh mesh = QuadGrid(1, 1);
    const Done done = Apply(mesh, Inset(Faces({0})), -0.2f);
    REQUIRE_FALSE(done.plan.refused());
    INFO("amountMin " << done.plan.amountMin);
    CHECK(done.plan.amountMin >= 0.0f);
    CHECK(Inverted(done.mesh, {0, 0, 1}) == 0u);
}

TEST_CASE("audit INS-3: Inset applied at its max leaves an inner face with area", "[wem][audit][inset]") {
    const Mesh mesh = QuadGrid(1, 1);
    Mesh planned = mesh;
    const ModelPlan plan = geom::PlanInset(planned, PointsOf(planned), Faces({0}), geom::InsetType::Group);
    REQUIRE_FALSE(plan.refused());
    INFO("amountMax " << plan.amountMax);
    Mesh out;
    const Verdict verdict = Run(mesh, Inset(Faces({0})), {}, &out, geom::kDefaultShadingAngle, plan.amountMax);
    INFO(verdict.failed);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit INS-4: Inset's inner corners interpolate their face, far past half the range",
          "[wem][audit][inset]") {
    // One triangle with unrelated UVs at its corners, inset at 0.9 of its
    // range: each inner corner's UV is the triangle's own interpolation there.
    Mesh mesh = FromFaces({{0, 1, 2}}, {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}});
    const std::array<Vector2f, 3> at = {Vector2f{0.1f, 0.7f}, Vector2f{0.9f, 0.2f}, Vector2f{0.3f, 0.95f}};
    {
        std::span<Vector2f> uv =
            mesh.attributes.getOrCreate<Vector2f>(geom::names::uv(0), Domain::Halfedge, geom::AttrType::F32x2);
        for (const HalfedgeId h : mesh.topology().fh(FaceId(0))) {
            uv[h.index()] = at[mesh.topology().from(h).value()];
        }
    }
    Mesh planned = mesh;
    const ModelPlan range = geom::PlanInset(planned, PointsOf(planned), Faces({0}), geom::InsetType::Group);
    REQUIRE_FALSE(range.refused());
    const Done done = Apply(mesh, Inset(Faces({0})), 0.9f * range.amountMax);
    REQUIRE(done.plan.selection.faces.size() == 1u);
    const auto uv = done.mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    f32 worst = 0.0f;
    for (const HalfedgeId h : done.mesh.topology().fh(FaceId(done.plan.selection.faces[0]))) {
        const Vector3f p = Positions(done.mesh)[done.mesh.topology().from(h).value()];
        const f32 b1 = p.x / 2.0f, b2 = p.y / 2.0f, b0 = 1.0f - b1 - b2;
        const Vector2f want = at[0] * b0 + at[1] * b1 + at[2] * b2;
        worst = std::max(worst, std::max(std::abs(uv[h.index()].x - want.x), std::abs(uv[h.index()].y - want.y)));
    }
    INFO("amountMax " << range.amountMax << " worst " << worst);
    CHECK(worst < 1e-4f);
}

TEST_CASE("audit INS-5: Inset of a whole closed mesh reports that it did nothing", "[wem][audit][inset]") {
    Mesh mesh = Box();
    const ModelPlan plan = geom::PlanInset(mesh, PointsOf(mesh), AllFaces(mesh), geom::InsetType::Group);
    INFO(geom::ToString(plan.refusal) << " changed " << plan.changed << " motions " << plan.motions.size());
    CHECK((plan.refused() || plan.changed == 0u));
}

TEST_CASE("audit X12: Inset of a U round a border vertex's open fan is not refused", "[wem][audit][inset]") {
    const Mesh mesh = OpenFan();
    Mesh out;
    const Verdict verdict = Run(mesh, Inset(Faces({0, 2})), {}, &out, geom::kDefaultShadingAngle, 0.05f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit X13: Inset of both sides of a double-sided card rings each side", "[wem][audit][inset]") {
    const Mesh mesh = DoubleCard();
    Mesh out;
    const Verdict verdict = Run(mesh, Inset(Faces({0, 1})), {}, &out, geom::kDefaultShadingAngle, 0.2f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " faces " << LiveFaces(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 10u);
}

// ============================================================================
// Connect, Loop Cut (CON-1..CON-5)
// ============================================================================

TEST_CASE("audit CON-1: Connect's slide re-samples UVs and skin", "[wem][audit][connect]") {
    // F18 on a 3 x 1 strip; every bottom and top edge, so each face is cut
    // upright; slide 0.5 runs each new vertex a quarter along its edge.
    Mesh mesh = QuadGrid(3, 1);
    Fields(mesh, 3.0f);
    std::vector<u32> edges;
    for (u32 x = 0; x < 3; ++x) {
        edges.push_back(EdgeAt(mesh, static_cast<f32>(x), 0, static_cast<f32>(x + 1), 0));
        edges.push_back(EdgeAt(mesh, static_cast<f32>(x), 1, static_cast<f32>(x + 1), 1));
    }
    Contract contract;
    contract.skinWidth = 3.0f;
    Mesh out;
    const Verdict verdict = Run(mesh, Connect(Edges(edges)), contract, &out, geom::kDefaultShadingAngle, 0.5f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " colour " << ColorFieldError(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(ColorFieldError(out) <= 1u);
}

TEST_CASE("audit CON-2: Connect slid to 1 makes no zero-length edge", "[wem][audit][connect]") {
    const Mesh mesh = QuadGrid(1, 1);
    for (const f32 slide : {1.0f, -1.0f}) {
        Mesh out;
        const Verdict verdict = Run(mesh, Connect(Edges({EdgeAt(mesh, 0, 0, 1, 0), EdgeAt(mesh, 0, 1, 1, 1)})), {},
                                    &out, geom::kDefaultShadingAngle, slide);
        INFO("slide " << slide << " " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        CHECK(verdict.failed == "");
    }
}

TEST_CASE("audit CON-3: Vertex Connect refuses a diagonal outside a concave face", "[wem][audit][connect]") {
    // The L's corners 2 (2,1) and 4 (1,2): the line between them crosses the
    // notch, outside the face.
    const Mesh mesh = LShape();
    Mesh out;
    const Verdict verdict = Run(
        mesh, [](Mesh& m, const PointTable& p) { return geom::PlanConnectVertices(m, p, Vertices({2, 4})); }, {},
        &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " inverted " << Inverted(out, {0, 0, 1}));
    CHECK((verdict.refused || Inverted(out, {0, 0, 1}) == 0u));
}

TEST_CASE("audit CON-4: Connect's slide keeps a crack's twins together", "[wem][audit][connect]") {
    // F8 2 x 1 cut at x = 1; the open ring x = 0, 1 (both sides), 2 at slide
    // 0.5: the two new vertices on the crack stay coincident.
    const Mesh mesh = Crack(2, 1, 1);
    const Done done =
        Apply(mesh, Connect(Edges({EdgeOf(mesh, 0, 3), EdgeOf(mesh, 1, 4), EdgeOf(mesh, 6, 7), EdgeOf(mesh, 2, 5)})),
              0.5f);
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    std::vector<f32> ys;
    for (const u32 v : AllVertices(done.mesh).vertices) {
        const Vector3f p = Positions(done.mesh)[v];
        if (p.x == 1.0f && p.y != 0.0f && p.y != 1.0f) {
            ys.push_back(p.y);
        }
    }
    REQUIRE(ys.size() == 2u);
    INFO("twins at y " << ys[0] << " and " << ys[1]);
    CHECK(ys[0] == Catch::Approx(ys[1]).margin(1e-5));
}

TEST_CASE("audit CON-5: Connect with one selected edge changes nothing", "[wem][audit][connect]") {
    // Max does nothing; today a midpoint goes in with changed = 0.
    Mesh mesh = QuadGrid(2, 2);
    const u32 before = LiveVertices(mesh);
    const ModelPlan plan = geom::PlanConnectEdges(mesh, PointsOf(mesh), Edges({EdgeAt(mesh, 1, 0, 1, 1)}));
    INFO(geom::ToString(plan.refusal) << " changed " << plan.changed);
    CHECK((plan.refused() || LiveVertices(mesh) == before));
}

TEST_CASE("audit CON-4: Connect's slide keeps a crack's twins together, wherever the crack runs",
          "[wem][audit][connect]") {
    // F8 w x 1 cut at every inner column, the whole ring of uprights at 0.5.
    for (u32 w = 2; w <= 4; ++w) {
        for (u32 column = 1; column < w; ++column) {
            const Mesh mesh = Crack(w, 1, column);
            std::vector<u32> ring;
            for (u32 x = 0; x <= w; ++x) {
                ring.push_back(EdgeOf(mesh, x, (w + 1) + x));
            }
            ring.push_back(EdgeOf(mesh, (w + 1) * 2, (w + 1) * 2 + 1));
            const Done done = Apply(mesh, Connect(Edges(ring)), 0.5f);
            INFO("w " << w << " column " << column << " " << geom::ToString(done.plan.refusal));
            REQUIRE_FALSE(done.plan.refused());
            std::vector<f32> ys;
            for (const u32 v : AllVertices(done.mesh).vertices) {
                const Vector3f p = Positions(done.mesh)[v];
                if (p.x == static_cast<f32>(column) && p.y != 0.0f && p.y != 1.0f) {
                    ys.push_back(p.y);
                }
            }
            REQUIRE(ys.size() == 2u);
            INFO("twins at y " << ys[0] << " and " << ys[1]);
            CHECK(ys[0] == Catch::Approx(ys[1]).margin(1e-5));
        }
    }
}

// ============================================================================
// Cut (CUT-2, CUT-3, CUT-5, CUT-7)
// ============================================================================

namespace {
PlanFn Cut(geom::CutPoint from, geom::CutPoint to, Vector3f plane) {
    return [from, to, plane](Mesh& m, const PointTable& p) { return geom::PlanCut(m, p, from, to, plane); };
}
geom::CutPoint OnEdge(u32 edge, f32 t) {
    geom::CutPoint point;
    point.edge = edge;
    point.t = t;
    return point;
}
geom::CutPoint OnVertex(u32 vertex) {
    geom::CutPoint point;
    point.vertex = vertex;
    return point;
}
Vector3f Lerp(const Vector3f& a, const Vector3f& b, f32 t) {
    return a + (b - a) * t;
}
} // namespace

TEST_CASE("audit CUT-2: two clicks on one edge put vertices where both clicks were", "[wem][audit][cut]") {
    // The bottom edge of a unit quad, clicked at 0.75 then 0.25 along it.
    const Mesh mesh = QuadGrid(1, 1);
    const u32 edge = EdgeAt(mesh, 0, 0, 1, 0);
    const HalfedgeId h = Topology::halfedge(EdgeId(edge), 0);
    const Vector3f a = Positions(mesh)[mesh.topology().from(h).value()];
    const Vector3f b = Positions(mesh)[mesh.topology().to(h).value()];
    const Vector3f first = Lerp(a, b, 0.75f), second = Lerp(a, b, 0.25f);
    const Done done = Apply(mesh, Cut(OnEdge(edge, 0.75f), OnEdge(edge, 0.25f), {0, 1, 0}));
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    CHECK(CountAt(done.mesh, first.x, first.y) == 1u);
    CHECK(CountAt(done.mesh, second.x, second.y) == 1u);
}

TEST_CASE("audit CUT-3: a cut across a hole is refused, or ends at its target", "[wem][audit][cut]") {
    // F15 square hole; from the middle of the left border to the middle of the
    // right one along y = 1.5, straight across the hole.
    const Mesh mesh = QuadHole();
    const Done done = Apply(mesh, Cut(OnEdge(EdgeAt(mesh, 0, 1, 0, 2), 0.5f), OnEdge(EdgeAt(mesh, 3, 1, 3, 2), 0.5f),
                                      {0, 1, 0}));
    INFO(geom::ToString(done.plan.refusal) << " changed " << done.plan.changed);
    if (!done.plan.refused()) {
        REQUIRE(done.plan.selection.vertices.size() == 1u);
        const Vector3f end = Positions(done.mesh)[done.plan.selection.vertices[0]];
        INFO("ended at " << end.x << ", " << end.y);
        CHECK(end.x == Catch::Approx(3.0f).margin(1e-5));
        CHECK(Isolated(done.mesh) == 0u);
    }
}

TEST_CASE("audit CUT-5: a cut along an existing soft edge leaves it soft", "[wem][audit][cut]") {
    // F13: from vertex 1 to vertex 6, the soft upright at side 1.
    const Mesh mesh = SmoothPrism(5);
    const Vector3f low = Positions(mesh)[1], high = Positions(mesh)[6];
    const Vector3f plane{-high.y, high.x, 0.0f}; // square to the side's radius: holds the upright
    const Done done = Apply(mesh, Cut(OnVertex(1), OnVertex(6), plane));
    INFO(geom::ToString(done.plan.refusal) << " changed " << done.plan.changed);
    if (!done.plan.refused()) {
        const u32 edge = EdgeOf(done.mesh, VertexAt(done.mesh, low.x, low.y, low.z),
                                VertexAt(done.mesh, high.x, high.y, high.z));
        REQUIRE(edge != kInvalidId);
        CHECK_FALSE(Sharp(done.mesh, edge));
    }
}

TEST_CASE("audit CUT-7: a cut across a cylinder's front stays on the front", "[wem][audit][cut]") {
    // 12 sides, n-gon caps; from side 11 to side 1 at half height along the
    // plane z = 0.5: the short arc through side 0, never round the back.
    const Mesh mesh = Cylinder(12, 1, Caps::Ngon);
    const Done done =
        Apply(mesh, Cut(OnEdge(EdgeOf(mesh, 11, 23), 0.5f), OnEdge(EdgeOf(mesh, 1, 13), 0.5f), {0, 0, 1}));
    INFO(geom::ToString(done.plan.refusal) << " changed " << done.plan.changed);
    REQUIRE_FALSE(done.plan.refused());
    u32 back = 0, made = 0;
    for (const u32 v : AllVertices(done.mesh).vertices) {
        const Vector3f p = Positions(done.mesh)[v];
        if (std::abs(p.z - 0.5f) < 1e-5f) {
            ++made;
            back += p.x < 0.0f ? 1u : 0u;
        }
    }
    INFO("made " << made << " behind " << back);
    CHECK(back == 0u);
    CHECK(made == 3u);
}

// ============================================================================
// Extrude Border (EXB-1..EXB-4)
// ============================================================================

TEST_CASE("audit EXB-1: Extrude Border on both sides of a crack raises nothing off the surface",
          "[wem][audit][border]") {
    // F8 2 x 1: both crack edges. Q5: the twins are one vertex, so the crack
    // edge is interior -- refused, or at least nothing rises along +Z.
    const Mesh mesh = Crack(2, 1, 1);
    Mesh out;
    const Verdict verdict =
        Run(mesh, Border(Edges({EdgeOf(mesh, 1, 4), EdgeOf(mesh, 6, 7)})), {}, &out, geom::kDefaultShadingAngle, 0.5f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    u32 off = 0;
    for (const u32 v : AllVertices(out).vertices) {
        off += Positions(out)[v].z != 0.0f ? 1u : 0u;
    }
    CHECK(off == 0u);
}

TEST_CASE("audit EXB-2: Extrude Border at a negative distance never folds the strip over its face",
          "[wem][audit][border]") {
    const Mesh mesh = QuadGrid(2, 1);
    const Done done = Apply(mesh, Border(Edges({EdgeAt(mesh, 2, 0, 2, 1)})), -0.5f);
    INFO(geom::ToString(done.plan.refusal) << " amountMin " << done.plan.amountMin);
    REQUIRE_FALSE(done.plan.refused());
    CHECK(Inverted(done.mesh, {0, 0, 1}) == 0u);
}

TEST_CASE("audit EXB-3: Extrude Border behind a zero-area face turns with the mesh (P15)",
          "[wem][audit][border]") {
    // A unit quad with a collinear triangle (1,0)-(1,0.5)-(1,1) on its right
    // edge; the triangle's border edge (1,0)-(1,0.5) extruded 0.5. Turned
    // 37 degrees, the result must be the turned result.
    const Mesh mesh = FromFaces({{0, 1, 2, 3}, {1, 4, 2}}, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {1, 0.5f, 0}});
    const Mesh turned = Turned(mesh);
    const Done plain = Apply(mesh, Border(Edges({EdgeOf(mesh, 1, 4)})), 0.5f);
    const Done moved = Apply(turned, Border(Edges({EdgeOf(turned, 1, 4)})), 0.5f);
    INFO(geom::ToString(plain.plan.refusal) << " / " << geom::ToString(moved.plan.refusal));
    REQUIRE_FALSE(plain.plan.refused());
    REQUIRE_FALSE(moved.plan.refused());
    f32 worst = 0.0f;
    for (const u32 v : AllVertices(plain.mesh).vertices) {
        const Vector3f want = Turned(Positions(plain.mesh)[v], kTurnAxis, 37.0f);
        f32 best = 1e30f;
        for (const u32 w : AllVertices(moved.mesh).vertices) {
            best = std::min(best, Length(Positions(moved.mesh)[w] - want));
        }
        worst = std::max(worst, best);
    }
    INFO("worst " << worst);
    CHECK(worst < 1e-4f);
}

TEST_CASE("audit EXB-4: Extrude Border drops interior edges and refuses when only they are given",
          "[wem][audit][border]") {
    // Locked as it is: the kernel skips an interior edge silently, the viewer
    // gates them at the Edge level.
    Mesh mixed = QuadGrid(2, 1);
    const ModelPlan plan =
        geom::PlanExtrudeBorder(mixed, PointsOf(mixed), Edges({EdgeAt(mixed, 2, 0, 2, 1), EdgeAt(mixed, 1, 0, 1, 1)}));
    INFO(geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(plan.changed == 1u);
    Mesh inner = QuadGrid(2, 1);
    CHECK(geom::PlanExtrudeBorder(inner, PointsOf(inner), Edges({EdgeAt(inner, 1, 0, 1, 1)})).refusal ==
          ModelRefusal::EmptySelection);
}

// ============================================================================
// Hinge (HNG-1, HNG-2)
// ============================================================================

namespace {
PlanFn Hinge(const ElementSet& faces, u32 edge) {
    return [faces, edge](Mesh& m, const PointTable& p) { return geom::PlanHingeFaces(m, p, faces, edge); };
}
} // namespace

TEST_CASE("audit HNG-1: a hinge on a crack edge inside the selection is refused", "[wem][audit][hinge]") {
    // F8 2 x 1, both faces; the left crack edge. Q5: the crack is interior to
    // the selection, so the hinge is not on its boundary.
    const Mesh mesh = Crack(2, 1, 1);
    const Verdict verdict = Run(mesh, Hinge(Faces({0, 1}), EdgeOf(mesh, 1, 4)), {}, nullptr,
                                geom::kDefaultShadingAngle, 30.0f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.refusal == ModelRefusal::HingeNotOnBoundary);
}

TEST_CASE("audit HNG-2: Hinge of a U round a border vertex's open fan is not refused", "[wem][audit][hinge]") {
    const Mesh mesh = OpenFan();
    Mesh out;
    const Verdict verdict =
        Run(mesh, Hinge(Faces({0, 2}), EdgeOf(mesh, 1, 2)), {}, &out, geom::kDefaultShadingAngle, 30.0f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

// ============================================================================
// Insert Vertex, Divide (IV-2..IV-4, DIV-1)
// ============================================================================

TEST_CASE("audit IV-2: Insert Vertex on a soft edge leaves both halves soft", "[wem][audit][insert]") {
    const Mesh mesh = SmoothPrism(5);
    const Done done = Apply(mesh, InsertVertex(EdgeOf(mesh, 1, 6), 0.5f));
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    CHECK(SharpUprights(done.mesh) == 0u);
}

TEST_CASE("audit IV-3: Insert Vertex at t = 0 or 1 makes no zero-length edge", "[wem][audit][insert]") {
    const Mesh mesh = QuadGrid(2, 1);
    for (const f32 t : {0.0f, 1.0f}) {
        Mesh out;
        const Verdict verdict = Run(mesh, InsertVertex(EdgeAt(mesh, 1, 0, 1, 1), t), {}, &out);
        INFO("t " << t << " " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        CHECK((verdict.refused || verdict.failed == ""));
    }
}

TEST_CASE("audit IV-4: Insert Vertex on a crack edge splits both sides", "[wem][audit][insert]") {
    const Mesh mesh = Crack(2, 1, 1);
    const Done done = Apply(mesh, InsertVertex(EdgeOf(mesh, 1, 4), 0.5f));
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    CHECK(CountAt(done.mesh, 1, 0.5f) == 2u);
}

TEST_CASE("audit DIV-1: Divide cuts every edge it is given and counts each cut", "[wem][audit][insert]") {
    // Coverage: a partial failure needs SplitEdge to fail on a live edge,
    // which no input reached.
    const Mesh mesh = QuadGrid(2, 2);
    Mesh out;
    const Verdict verdict = Run(
        mesh,
        [&](Mesh& m, const PointTable&) {
            return geom::PlanDivide(m, Edges({EdgeAt(mesh, 0, 0, 1, 0), EdgeAt(mesh, 1, 1, 1, 2)}), 3);
        },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 13u);
}

TEST_CASE("audit EXB-1: Extrude Border on both crack edges is refused or makes no strip", "[wem][audit][border]") {
    const Mesh mesh = Crack(2, 1, 1);
    Mesh out;
    const Verdict verdict =
        Run(mesh, Border(Edges({EdgeOf(mesh, 1, 4), EdgeOf(mesh, 6, 7)})), {}, &out, geom::kDefaultShadingAngle, 0.5f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " faces " << LiveFaces(out) << " inverted "
                                         << Inverted(out, {0, 0, 1}));
    CHECK((verdict.refused || LiveFaces(out) == 2u));
}

// ============================================================================
// Chamfer (CHF-3..CHF-7, X6)
// ============================================================================

namespace {
/// The strip's corner at (x, y) and the field's distance from it, un-shifted.
f32 StripError(const Done& done, f32 x, f32 y) {
    const auto uv = done.mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    for (const HalfedgeId h : done.mesh.topology().fh(FaceId(done.plan.selection.faces.front()))) {
        const Vector3f p = Positions(done.mesh)[done.mesh.topology().from(h).value()];
        if (std::abs(p.x - x) < 1e-5f && std::abs(p.y - y) < 1e-5f) {
            Vector2f got = uv[h.index()];
            got.x -= got.x > 5.0f ? 10.0f : 0.0f;
            const Vector2f want = UvField(p);
            return std::max(std::abs(got.x - want.x), std::abs(got.y - want.y));
        }
    }
    return 1e30f;
}
} // namespace

TEST_CASE("audit CHF-3: a seam chamfer's far corner samples the strip's own source face", "[wem][audit][chamfer]") {
    // The seam on x = 2 of a 3 x 1 strip (faces 1 and 2), face 0 in front: the
    // captured face id is read on the rebuilt mesh. The far corner the
    // resample is taken at, (2.25, 0), must read face 1's polygon.
    Mesh mesh = QuadGrid(3, 1);
    SeamUvs(mesh, 2.0f);
    MarkSeam(mesh, EdgeAt(mesh, 2, 0, 2, 1));
    const Done done = Apply(mesh, Chamfer(Edges({EdgeAt(mesh, 2, 0, 2, 1)})), 0.25f);
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    REQUIRE(done.plan.selection.faces.size() == 1u);
    CHECK(OneIsland(done.mesh, done.plan.selection.faces.front()));
    CHECK(StripError(done, 2.25f, 0.0f) < 1e-4f);
}

TEST_CASE("audit EXT-8: a seam chamfer strip's two far corners each read their own place", "[wem][audit][chamfer]") {
    // The same strip: its seam resample targets both far corners, (2.25, 0)
    // and (2.25, 1), yet ApplyAmount samples once, at the first one's vertex.
    Mesh mesh = QuadGrid(3, 1);
    SeamUvs(mesh, 2.0f);
    MarkSeam(mesh, EdgeAt(mesh, 2, 0, 2, 1));
    const Done done = Apply(mesh, Chamfer(Edges({EdgeAt(mesh, 2, 0, 2, 1)})), 0.25f);
    REQUIRE_FALSE(done.plan.refused());
    REQUIRE(done.plan.selection.faces.size() == 1u);
    INFO("far corners off by " << StripError(done, 2.25f, 0.0f) << " and " << StripError(done, 2.25f, 1.0f));
    CHECK(StripError(done, 2.25f, 0.0f) < 1e-4f);
    CHECK(StripError(done, 2.25f, 1.0f) < 1e-4f);
    CHECK(UvFieldError(done.mesh) < 1e-4f);
}

TEST_CASE("audit CHF-4: an edge chamfer ending on a crack cuts the twin side too", "[wem][audit][chamfer]") {
    // F8 2 x 2 cut at x = 1; the edge (0,1)-(1,1) at 0.25. The header: members
    // of one point chamfer as one, so the crack keeps a twin for every vertex.
    const Mesh mesh = Crack(2, 2, 1);
    const Done done = Apply(mesh, Chamfer(Edges({EdgeOf(mesh, 3, 4)})), 0.25f);
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    for (const f32 y : {0.0f, 0.75f, 1.25f, 2.0f}) {
        INFO("y " << y);
        CHECK(CountAt(done.mesh, 1, y) == 2u);
    }
    CHECK(CountAt(done.mesh, 1, 1) == 0u);
}

TEST_CASE("audit CHF-4: a slanted chamfer's points on a crack land together", "[wem][audit][chamfer]") {
    // The same crack with (0,1) raised to (0,1.5): the left side's points sit at
    // amount / sin of the slant, and the twin side's must sit there too.
    Mesh mesh = Crack(2, 2, 1);
    Positions(mesh)[3] = Vector3f{0.0f, 1.5f, 0.0f};
    const Done done = Apply(mesh, Chamfer(Edges({EdgeOf(mesh, 3, 4)})), 0.25f);
    INFO(geom::ToString(done.plan.refusal));
    REQUIRE_FALSE(done.plan.refused());
    const auto p = Positions(done.mesh);
    u32 onCrack = 0;
    for (u32 v = 0; v < p.size(); ++v) {
        if (!done.mesh.topology().isDeleted(VertexId(v)) && std::abs(p[v].x - 1.0f) < 1e-5f) {
            INFO("(1, " << p[v].y << ")");
            CHECK(CountAt(done.mesh, 1, p[v].y) == 2u);
            ++onCrack;
        }
    }
    CHECK(onCrack == 8u);
    CHECK(CountAt(done.mesh, 1, 1) == 0u);
}

TEST_CASE("audit CHF-5: a vertex patch on a UV seam reads one island continuously", "[wem][audit][chamfer]") {
    // 2 x 2 grid, the right column on its own island, seam marked along x = 1;
    // the centre vertex chamfered at 0.25.
    Mesh mesh = QuadGrid(2, 2);
    SeamUvs(mesh, 1.0f);
    MarkSeam(mesh, EdgeAt(mesh, 1, 0, 1, 1));
    MarkSeam(mesh, EdgeAt(mesh, 1, 1, 1, 2));
    const Done done = Apply(mesh, ChamferV(Vertices({4})), 0.25f);
    INFO(geom::ToString(done.plan.refusal) << " uv " << UvFieldError(done.mesh));
    REQUIRE_FALSE(done.plan.refused());
    REQUIRE(done.plan.selection.faces.size() == 1u);
    CHECK(OneIsland(done.mesh, done.plan.selection.faces.front()));
    CHECK(UvFieldError(done.mesh) < 1e-4f);
}

TEST_CASE("audit CHF-6: a border edge in the selection does not refuse the interior ones", "[wem][audit][chamfer]") {
    // Q2: do the rest. 2 x 2 grid: (1,0)-(1,1) interior, (0,0)-(1,0) border.
    Mesh mesh = QuadGrid(2, 2);
    const ModelPlan plan =
        geom::PlanChamferEdges(mesh, PointsOf(mesh), Edges({EdgeAt(mesh, 1, 0, 1, 1), EdgeAt(mesh, 0, 0, 1, 0)}));
    INFO(geom::ToString(plan.refusal));
    CHECK_FALSE(plan.refused());
}

TEST_CASE("audit CHF-7: the kernel builds Chamfer's segments", "[wem][audit][chamfer]") {
    // Kernel half: three segments across one cube edge at 0.2.
    const Mesh mesh = Box();
    geom::ChamferParams params;
    params.segments = 3;
    Mesh out;
    const Verdict verdict =
        Run(mesh, Chamfer(Edges({EdgeOf(mesh, 0, 1)}), params), {}, &out, geom::kDefaultShadingAngle, 0.2f);
    const Done done = Apply(mesh, Chamfer(Edges({EdgeOf(mesh, 0, 1)}), params), 0.2f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(done.plan.selection.faces.size() == 3u);
}

TEST_CASE("audit X6: a reshaped face's zero-area triangles still bound the amount", "[wem][audit][amount]") {
    // A 0.5 x 2 rectangle, vertex (0,0) chamfered: the point on the short edge
    // reaches (0.5,0) at 0.5, so the range ends there whatever the cut.
    const Mesh mesh = FromFaces({{0, 1, 2, 3}}, {{0, 0, 0}, {0.5f, 0, 0}, {0.5f, 2, 0}, {0, 2, 0}});
    Mesh planned = mesh;
    ModelPlan plan = geom::PlanChamferVertices(planned, PointsOf(planned), Vertices({0}));
    REQUIRE_FALSE(plan.refused());
    INFO("amountMax " << plan.amountMax);
    CHECK(plan.amountMax <= 0.5f + 1e-4f);
}

// ============================================================================
// X9: hand-marked seams
// ============================================================================

TEST_CASE("audit X9: a seam marked by hand survives a tool touching its edge", "[wem][audit][crease]") {
    // Continuous UVs, seam marked on (1,0)-(1,1); Insert Vertex halves it.
    Mesh mesh = QuadGrid(2, 2);
    Fields(mesh, 2.0f);
    MarkSeam(mesh, EdgeAt(mesh, 1, 0, 1, 1));
    const Done done = Apply(mesh, InsertVertex(EdgeAt(mesh, 1, 0, 1, 1), 0.5f));
    REQUIRE_FALSE(done.plan.refused());
    const auto seam = done.mesh.attributes.get<const u8>(geom::names::kSeam, Domain::Edge);
    for (const auto& [a, b] : {std::pair{0.0f, 0.5f}, std::pair{0.5f, 1.0f}}) {
        const u32 e = EdgeAt(done.mesh, 1, a, 1, b);
        REQUIRE(e != kInvalidId);
        INFO("half from y " << a);
        CHECK((e < seam.size() && seam[e] != 0));
    }
}

// ============================================================================
// Bridge (BRG-3, BRG-4, the cases)
// ============================================================================

namespace {
/// Two open tubes round +z: A of @p sidesA from z = 0 to 1, B of @p sidesB
/// from z = 2 to 3. With @p crack, A's side 0 is cut: the faces at side 5
/// use their own copies of its two vertices. A's ring r is `r * sidesA + s`;
/// B's starts at `firstB`.
struct Tubes {
    Mesh mesh;
    u32 firstB = 0;
};
Tubes TwoTubes(u32 sidesA, u32 sidesB, bool crack) {
    std::vector<Vector3f> p;
    std::vector<std::vector<u32>> faces;
    const auto ring = [&](u32 sides, f32 z) {
        for (u32 s = 0; s < sides; ++s) {
            const f32 a = 6.28318530718f * static_cast<f32>(s) / static_cast<f32>(sides);
            p.push_back(Vector3f{std::cos(a), std::sin(a), z});
        }
    };
    ring(sidesA, 0.0f);
    ring(sidesA, 1.0f);
    const u32 twins = static_cast<u32>(p.size());
    if (crack) {
        p.push_back(p[0]);
        p.push_back(p[sidesA]);
    }
    for (u32 s = 0; s < sidesA; ++s) {
        const bool last = s + 1 == sidesA;
        const u32 b0 = last ? (crack ? twins : 0u) : s + 1;
        const u32 b1 = last ? (crack ? twins + 1 : sidesA) : sidesA + s + 1;
        faces.push_back({s, b0, b1, sidesA + s});
    }
    Tubes out;
    out.firstB = static_cast<u32>(p.size());
    ring(sidesB, 2.0f);
    ring(sidesB, 3.0f);
    for (u32 s = 0; s < sidesB; ++s) {
        const u32 n = (s + 1) % sidesB;
        faces.push_back({out.firstB + s, out.firstB + n, out.firstB + sidesB + n, out.firstB + sidesB + s});
    }
    out.mesh = FromFaces(faces, p);
    return out;
}
PlanFn Bridge(u32 first, u32 second) {
    return [first, second](Mesh& m, const PointTable& p) { return geom::PlanBridge(m, p, EdgeId(first), EdgeId(second)); };
}
} // namespace

TEST_CASE("audit BRG: Bridge joins two open tubes' facing rims", "[wem][audit][bridge]") {
    const Tubes tubes = TwoTubes(6, 6, false);
    Mesh out;
    const Verdict verdict = Run(tubes.mesh, Bridge(EdgeOf(tubes.mesh, 6, 7), EdgeOf(tubes.mesh, tubes.firstB, tubes.firstB + 1)),
                                {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 18u);
}

TEST_CASE("audit BRG: Bridge of two faces tunnels through a cylinder", "[wem][audit][bridge]") {
    const Mesh mesh = Cylinder(8, 1, Caps::Ngon);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [](Mesh& m, const PointTable& p) { return geom::PlanBridgeFaces(m, p, FaceId(0), FaceId(4)); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 10u - 2u + 4u);
}

TEST_CASE("audit BRG-3: Bridge reads a rim that crosses a crack as one loop", "[wem][audit][bridge]") {
    // A's side 0 is cracked; its top rim has six points on seven vertices. Q5:
    // one loop of six, bridged to B's bottom rim by six quads.
    const Tubes tubes = TwoTubes(6, 6, true);
    Mesh out;
    const Verdict verdict = Run(tubes.mesh, Bridge(EdgeOf(tubes.mesh, 7, 8), EdgeOf(tubes.mesh, tubes.firstB, tubes.firstB + 1)),
                                {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 18u);
}

TEST_CASE("audit BRG-4: Bridge joins loops of different edge counts", "[wem][audit][bridge]") {
    // A known gap, locked: Max bridges a 6-rim to an 8-rim.
    const Tubes tubes = TwoTubes(6, 8, false);
    Mesh out;
    const Verdict verdict = Run(tubes.mesh, Bridge(EdgeOf(tubes.mesh, 6, 7), EdgeOf(tubes.mesh, tubes.firstB, tubes.firstB + 1)),
                                {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK_FALSE(verdict.refused);
}

// ============================================================================
// Create Polygon (CP-2..CP-4)
// ============================================================================

namespace {
PlanFn CreatePolygon(std::vector<u32> loop) {
    return [loop](Mesh& m, const PointTable& p) { return geom::PlanCreatePolygon(m, p, loop); };
}
} // namespace

TEST_CASE("audit CP-2: Create Polygon's corners on a new edge copy the face beside them", "[wem][audit][polygon]") {
    // F18 3 x 2 grid without cell (1,0): its bottom edge went with it, so the
    // corner at (1,0) leaves on a new edge; (1,0) still has cell (0,0) beside it.
    const Mesh grid = QuadGrid(3, 2);
    std::vector<std::vector<u32>> faces = CornerLists(grid.faceSet());
    faces.erase(faces.begin() + 1);
    std::vector<Vector3f> p(Positions(grid).begin(), Positions(grid).end());
    Mesh mesh = FromFaces(faces, p);
    Fields(mesh, 3.0f);
    Mesh out;
    const Verdict verdict = Run(mesh, CreatePolygon({1, 2, 6, 5}), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit CP-3: Create Polygon with no rim face is reached only from loose vertices", "[wem][audit][polygon]") {
    // A polygon on new edges only, through corners of three section-1 quads
    // (6, 11, 12), would give each corner a second fan: refused. So the
    // face-0 fallback is reached only by a loop of loose vertices (16-18),
    // where no neighbour has a section to give; locked as it is.
    const std::vector<std::vector<u32>> faces = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}, {12, 13, 14, 15}};
    const std::vector<Vector3f> places = {
        {10, 10, 0}, {11, 10, 0}, {11, 11, 0}, {10, 11, 0}, {0, 0, 0},     {1, 0, 0},  {1, 1, 0},
        {0, 1, 0},   {3, 0, 0},   {4, 0, 0},   {4, 1, 0},   {3, 1, 0},     {1.5f, 3, 0}, {2.5f, 3, 0},
        {2.5f, 4, 0}, {1.5f, 4, 0}, {20, 0, 0}, {21, 0, 0},  {20, 1, 0}};
    const Mesh mesh = FromFaces(faces, places, {0, 1, 1, 1}, 2);
    const Verdict corners = Run(mesh, CreatePolygon({6, 11, 12}));
    INFO(geom::ToString(corners.refusal));
    CHECK(corners.refused);
    const Done loose = Apply(mesh, CreatePolygon({16, 17, 18}));
    INFO(geom::ToString(loose.plan.refusal));
    REQUIRE_FALSE(loose.plan.refused());
    REQUIRE(loose.plan.selection.faces.size() == 1u);
}

TEST_CASE("audit CP-4: Create Polygon refuses a loop through both twins of a crack", "[wem][audit][polygon]") {
    // F8 2 x 1: 4 (1,1), 1 (1,0), 6 (1,0), 7 (1,1) -- a face of no area.
    const Mesh mesh = Crack(2, 1, 1);
    const Verdict verdict = Run(mesh, CreatePolygon({4, 1, 6, 7}));
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.refused);
}

// ============================================================================
// AMT-2
// ============================================================================

TEST_CASE("audit AMT-2: no resample reads a polygon of zero area", "[wem][audit][amount]") {
    // CHF-1's chamfer: every class must sample a face with area at Plan time.
    Mesh mesh = QuadGrid(2, 1);
    Fields(mesh, 2.0f);
    const ModelPlan plan = geom::PlanChamferEdges(mesh, PointsOf(mesh), Edges({EdgeAt(mesh, 1, 0, 1, 1)}));
    REQUIRE_FALSE(plan.refused());
    u32 flat = 0;
    for (const geom::Resample& resample : plan.resamples) {
        Vector3f sum{0, 0, 0};
        const auto& at = resample.source.positions;
        for (std::size_t i = 0; i < at.size(); ++i) {
            sum = sum + Cross(at[i], at[(i + 1) % at.size()]);
        }
        flat += Length(sum) < 1e-9f ? 1u : 0u;
    }
    INFO(flat << " of " << plan.resamples.size() << " resamples read a flat polygon");
    CHECK(flat == 0u);
}

// ============================================================================
// F18: every tool's new corners on the fields, at a mid amount
// ============================================================================

namespace {
Vector3f Mid(const Mesh& mesh, u32 e) {
    const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
    return (Positions(mesh)[mesh.topology().from(h).value()] + Positions(mesh)[mesh.topology().to(h).value()]) * 0.5f;
}
/// The live edge (interior when @p interior) whose middle is nearest @p at.
u32 EdgeNear(const Mesh& mesh, Vector3f at, bool interior) {
    u32 best = kInvalidId;
    f32 nearest = 1e30f;
    for (const u32 e : AllEdges(mesh).edges) {
        if (interior == mesh.topology().isBoundary(EdgeId(e))) {
            continue;
        }
        const f32 d = Length(Mid(mesh, e) - at);
        if (d < nearest) {
            nearest = d;
            best = e;
        }
    }
    return best;
}
u32 FaceNear(const Mesh& mesh, Vector3f at) {
    u32 best = kInvalidId;
    f32 nearest = 1e30f;
    for (const u32 f : AllFaces(mesh).faces) {
        Vector3f c{0, 0, 0};
        u32 n = 0;
        for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
            c = c + Positions(mesh)[mesh.topology().from(h).value()];
            ++n;
        }
        const f32 d = Length(c * (1.0f / static_cast<f32>(n)) - at);
        if (d < nearest) {
            nearest = d;
            best = f;
        }
    }
    return best;
}
f32 RangeMax(const Mesh& mesh, const PlanFn& plan) {
    Mesh planned = mesh;
    ModelPlan made = plan(planned, PointsOf(planned));
    geom::AmountRange(planned, made);
    return made.amountMax < 1e30f ? made.amountMax : 1.0f;
}
} // namespace

namespace {
/// Every tool whose name @p take accepts, on F1/F2/F3 under the fields, at
/// half its range (or its plain press), through Run plus the colour field.
void FieldSweep(const std::function<bool(const std::string&)>& take) {
    struct Fixture {
        const char* name;
        Mesh mesh;
        f32 width, height;
    };
    std::vector<Fixture> fixtures = {{"quads", QuadGrid(3, 3), 3, 3}, {"triangles", TriGrid(3, 3), 3, 3},
                                     {"mixed", MixedGrid(), 4, 2}};
    for (Fixture& fixture : fixtures) {
        Fields(fixture.mesh, fixture.width);
        const Mesh& mesh = fixture.mesh;
        const Vector3f centre{fixture.width * 0.5f, fixture.height * 0.5f, 0};
        const u32 edge = EdgeNear(mesh, centre, true);
        const u32 face = FaceNear(mesh, centre);
        const u32 left = EdgeNear(mesh, {0, fixture.height * 0.5f, 0}, false);
        const u32 right = EdgeNear(mesh, {fixture.width, fixture.height * 0.5f, 0}, false);
        const u32 vertex = Topology::halfedge(EdgeId(edge), 0).valid() ? mesh.topology().from(Topology::halfedge(EdgeId(edge), 0)).value() : 0u;
        geom::CutPoint from = OnEdge(left, 0.5f), to = OnEdge(right, 0.5f);
        const Vector3f a = Mid(mesh, left), b = Mid(mesh, right);
        const Vector3f plane = Unit(Cross(b - a, Vector3f{0, 0, 1}));
        const std::vector<std::pair<std::string, std::pair<PlanFn, bool>>> tools = {
            {"inset", {Inset(Faces({face})), true}},
            {"inset by polygon", {Inset(Faces({face}), geom::InsetType::ByPolygon), true}},
            {"chamfer edge", {Chamfer(Edges({edge})), true}},
            {"chamfer vertex", {ChamferV(Vertices({vertex})), true}},
            {"insert vertex", {InsertVertex(edge, 0.3f), false}},
            {"divide", {[edge](Mesh& m, const PointTable&) { return geom::PlanDivide(m, Edges({edge}), 3); }, false}},
            {"cut", {Cut(from, to, plane), false}},
            {"slice", {Slice({}, centre + Vector3f{0.3f, 0, 0}, {1, 0.2f, 0}, geom::SliceMode::Cut), false}},
        };
        for (const auto& [name, tool] : tools) {
            if (!take(name)) {
                continue;
            }
            std::optional<f32> amount;
            if (tool.second) {
                amount = 0.5f * RangeMax(mesh, tool.first);
            }
            Contract contract;
            contract.skinWidth = fixture.width;
            Mesh out;
            const Verdict verdict = Run(mesh, tool.first, contract, &out, geom::kDefaultShadingAngle, amount);
            INFO(fixture.name << " / " << name << ": " << geom::ToString(verdict.refusal) << " " << verdict.failed
                              << " colour " << ColorFieldError(out) << " amount " << amount.value_or(-1.0f));
            CHECK_FALSE(verdict.refused);
            CHECK(verdict.failed == "");
            CHECK(ColorFieldError(out) <= 1u);
        }
    }
}
} // namespace

TEST_CASE("audit F18: Inset, Chamfer Vertex, Insert Vertex, Divide, Cut and Slice keep UV, colour and skin on the "
          "fields",
          "[wem][audit][fields]") {
    FieldSweep([](const std::string& name) { return name != "chamfer edge"; });
}

TEST_CASE("audit CHF-1: an edge chamfer keeps the fields on triangles and n-gons too", "[wem][audit][fields]") {
    FieldSweep([](const std::string& name) { return name == "chamfer edge"; });
}

// ============================================================================
// §4.B's case lists, through Run (P1-P8, P10)
// ============================================================================

TEST_CASE("audit EXT cases: two quads sharing a vertex get two copies", "[wem][audit][extrude]") {
    // The input's shared point already joins the two shells, and the lifted
    // copies meet the same way: P5 is off (X2's point rule, not a new join).
    const Mesh mesh = TouchingQuads();
    Contract contract;
    contract.joinsShellsOk = true;
    Mesh out;
    const Verdict verdict = Run(mesh, Extrude(Faces({0, 1})), contract, &out, geom::kDefaultShadingAngle, 0.5f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(CountAt(out, 1, 1, 0.5f) == 2u);
}

TEST_CASE("audit EXT cases: walls take their region face's section", "[wem][audit][extrude]") {
    // F10 3 x 3, row 2 in section 1; the 2 x 2 block at x 1..3, y 1..3.
    const Mesh mesh = TwoSections(3, 3, 2);
    Mesh out;
    const Verdict verdict = Run(mesh, Extrude(Faces({4, 5, 7, 8})), {}, &out, geom::kDefaultShadingAngle, 0.5f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    const auto section = out.attributes.get<const u32>(geom::names::kSection, Domain::Face);
    u32 ones = 0;
    for (const u32 f : AllFaces(out).faces) {
        ones += section[f] == 1u ? 1u : 0u;
    }
    CHECK(ones == 3u + 4u); // row 2, and the walls of faces 7 and 8
}

TEST_CASE("audit EXT cases: a region round a hole, a closed box, a pit", "[wem][audit][extrude]") {
    {
        const Mesh mesh = QuadHole();
        Mesh out;
        const Verdict verdict = Run(mesh, Extrude(AllFaces(mesh)), {}, &out, geom::kDefaultShadingAngle, 0.3f);
        INFO("hole: " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
        CHECK(LiveFaces(out) == 8u + 12u + 4u);
    }
    {
        const Mesh mesh = Box();
        Mesh out;
        const Verdict verdict = Run(mesh, Extrude(AllFaces(mesh)), {}, &out, geom::kDefaultShadingAngle, 0.2f);
        INFO("box: " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
    }
    {
        const Mesh mesh = QuadGrid(3, 3);
        Mesh out;
        const Verdict verdict = Run(mesh, Extrude(Faces({4})), {}, &out, geom::kDefaultShadingAngle, -0.5f);
        INFO("pit: " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
        CHECK(CountAt(out, 1, 1, -0.5f) == 1u);
    }
}

TEST_CASE("audit EXB cases: an open run of three and a 90 degree corner", "[wem][audit][border]") {
    {
        const Mesh mesh = QuadGrid(3, 1);
        Mesh out;
        const Verdict verdict = Run(mesh, Border(Edges({EdgeAt(mesh, 0, 0, 1, 0), EdgeAt(mesh, 1, 0, 2, 0),
                                                        EdgeAt(mesh, 2, 0, 3, 0)})),
                                    {}, &out, geom::kDefaultShadingAngle, 0.3f);
        INFO("run: " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
        CHECK(CountAt(out, 1, -0.3f) == 1u);
    }
    {
        const Mesh mesh = QuadGrid(2, 2);
        Mesh out;
        const Verdict verdict = Run(mesh, Border(Edges({EdgeAt(mesh, 0, 0, 1, 0), EdgeAt(mesh, 0, 0, 0, 1)})), {},
                                    &out, geom::kDefaultShadingAngle, 0.3f);
        INFO("corner: " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
    }
}

TEST_CASE("audit INS cases: a whole cylinder band", "[wem][audit][inset]") {
    const Mesh mesh = Cylinder(6, 2, Caps::Ngon);
    Mesh out;
    const Verdict verdict =
        Run(mesh, Inset(Faces({0, 1, 2, 3, 4, 5})), {}, &out, geom::kDefaultShadingAngle, 0.2f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 14u + 12u);
}

TEST_CASE("audit HNG cases: two regions with the hinge on one", "[wem][audit][hinge]") {
    // 4 x 1 strip: faces 0 and 2, hinged on face 0's left edge. Both turn about
    // the one axis; face 2 is walled all round.
    const Mesh mesh = QuadGrid(4, 1);
    Mesh out;
    const Verdict verdict =
        Run(mesh, Hinge(Faces({0, 2}), EdgeAt(mesh, 0, 0, 0, 1)), {}, &out, geom::kDefaultShadingAngle, 30.0f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit CHF cases: a fan pole, a valence-2 vertex's two edges, a crack vertex's twins",
          "[wem][audit][chamfer]") {
    {
        const Mesh mesh = Cylinder(6, 1, Caps::Fan);
        const PlanFn tool = ChamferV(Vertices({12}));
        Mesh out;
        const Verdict verdict =
            Run(mesh, tool, {}, &out, geom::kDefaultShadingAngle, 0.5f * RangeMax(mesh, tool));
        INFO("pole: " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
    }
    {
        const Mesh mesh = ValenceTwo();
        const PlanFn tool = Chamfer(Edges({EdgeOf(mesh, 1, 6), EdgeOf(mesh, 6, 4)}));
        Mesh out;
        const Verdict verdict =
            Run(mesh, tool, {}, &out, geom::kDefaultShadingAngle, 0.5f * RangeMax(mesh, tool));
        INFO("valence 2: " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
    }
    {
        // F8 2 x 2 cut at x = 1: (1,1) and its twin 10 chamfered together; the
        // two cuts meet on the crack.
        const Mesh mesh = Crack(2, 2, 1);
        Contract contract;
        contract.joinsShellsOk = true; // the new twins are the crack staying shut
        Mesh out;
        const Verdict verdict =
            Run(mesh, ChamferV(Vertices({4, 10})), contract, &out, geom::kDefaultShadingAngle, 0.25f);
        INFO("crack: " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
        CHECK(CountAt(out, 1, 0.75f) == 2u);
        CHECK(CountAt(out, 1, 1.25f) == 2u);
    }
}

namespace {
/// A click on edge @p e at @p at, as `t` from the edge's first end.
geom::CutPoint OnEdgeAt(const Mesh& mesh, u32 e, Vector3f at) {
    const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
    const Vector3f a = Positions(mesh)[mesh.topology().from(h).value()];
    const Vector3f b = Positions(mesh)[mesh.topology().to(h).value()];
    return OnEdge(e, Length(at - a) / Length(b - a));
}
} // namespace

TEST_CASE("audit CUT cases: across a concave face's bar", "[wem][audit][cut]") {
    // The L from (0, 0.5) on its left edge to (2, 0.5) on its right one.
    const Mesh mesh = LShape();
    Mesh out;
    const Verdict verdict = Run(mesh,
                                Cut(OnEdgeAt(mesh, EdgeOf(mesh, 5, 0), {0, 0.5f, 0}),
                                    OnEdgeAt(mesh, EdgeOf(mesh, 1, 2), {2, 0.5f, 0}), {0, 1, 0}),
                                {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 2u);
}

TEST_CASE("audit EXT-9: a cut between two points of one concave face never folds it", "[wem][audit][cut]") {
    // The L from (2, 0.75) to (0.75, 2): the straight line crosses the notch,
    // outside the face. The walk sees the target in the start's face and splits
    // straight to it, with no inside check (as Vertex Connect, CON-3).
    const Mesh mesh = LShape();
    Mesh out;
    const Verdict verdict = Run(mesh,
                                Cut(OnEdgeAt(mesh, EdgeOf(mesh, 1, 2), {2, 0.75f, 0}),
                                    OnEdgeAt(mesh, EdgeOf(mesh, 4, 5), {0.75f, 2, 0}), {0.7071068f, 0.7071068f, 0}),
                                {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " inverted " << Inverted(out, {0, 0, 1}));
    CHECK((verdict.refused || (Inverted(out, {0, 0, 1}) == 0u && verdict.failed == "")));
}

TEST_CASE("audit SLC cases: a concave face through its reflex vertex, and across", "[wem][audit][slice]") {
    for (const auto& [origin, normal] : {std::pair{Vector3f{0, 1, 0}, Vector3f{0, 1, 0}},
                                         std::pair{Vector3f{0.5f, 0, 0}, Vector3f{1, 0, 0}}}) {
        const Mesh mesh = LShape();
        Mesh out;
        const Verdict verdict = Run(mesh, Slice({}, origin, normal, geom::SliceMode::Cut), {}, &out);
        INFO("normal " << normal.x << "," << normal.y << ": " << geom::ToString(verdict.refusal) << " "
                       << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
        CHECK(LiveFaces(out) == 2u);
    }
}

// ============================================================================
// Speed (item 9 of the fix list): whole-mesh presses on a mesh the size of HD
// Arthas's body, timed; hidden, run by hand.
// ============================================================================

TEST_CASE("audit speed: whole-mesh Inset and Chamfer on a 12k-face mesh", "[wem][audit][.perf]") {
    const Mesh sphere = UvSphere(128, 96);
    const auto timed = [&](const char* name, const PlanFn& plan, f32 amount) {
        Mesh work = sphere;
        const auto started = std::chrono::steady_clock::now();
        const PointTable points = PointsOf(work);
        ModelPlan made = plan(work, points);
        const f64 planned = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (!made.refused()) {
            geom::ApplyAmount(work, made, amount);
            geom::FinishTool(work, made, geom::kDefaultShadingAngle);
        }
        const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
        std::printf("%s: %u faces, plan %.1f ms, all %.1f ms, %s\n", name, LiveFaces(sphere), planned, ms,
                    geom::ToString(made.refusal));
        return ms;
    };
    CHECK(timed("inset by polygon", Inset(AllFaces(sphere), geom::InsetType::ByPolygon), 0.001f) < 1000.0);
    CHECK(timed("chamfer vertices", ChamferV(AllVertices(sphere)), 0.001f) < 1000.0);
    CHECK(timed("chamfer edges", Chamfer(AllEdges(sphere)), 0.001f) < 1000.0);
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The Mesh workspace audit, group A: Collapse, Weld, Target Weld, Remove,
/// Delete, Cap, Flip, To Quads / To Polygons, the triangulation tools
/// (EDIT_MODE_MESH_AUDIT_PLAN.md §0, §4.A in WhiteoutFlakes). Each test is
/// named after its suspect; a confirmed one is `[!shouldfail]` until its fix.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <iterator>
#include <tuple>

#include <whiteout/models/wem/geometry/prepare.h>

#include "wem_audit_fixtures.h"

using namespace audit;

namespace {

ModelPlan collapse(Mesh& mesh, const PointTable& points, const ElementSet& selection) {
    return geom::PlanCollapse(mesh, points, selection);
}

/// How many live vertices lie within @p eps of @p at.
u32 Near(const Mesh& mesh, const Vector3f& at, f32 eps = 1e-4f) {
    const auto p = Positions(mesh);
    u32 n = 0;
    for (u32 v = 0; v < p.size(); ++v) {
        n += !mesh.topology().isDeleted(VertexId(v)) && Length(p[v] - at) <= eps ? 1u : 0u;
    }
    return n;
}

/// The first live vertex within @p eps of @p at; kInvalidId when none.
u32 NearId(const Mesh& mesh, const Vector3f& at, f32 eps = 1e-4f) {
    const auto p = Positions(mesh);
    for (u32 v = 0; v < p.size(); ++v) {
        if (!mesh.topology().isDeleted(VertexId(v)) && Length(p[v] - at) <= eps) {
            return v;
        }
    }
    return kInvalidId;
}

/// @p mesh with vertex @p v lifted to @p z and re-shaded: a curved patch whose
/// normals a stale re-shade shows.
Mesh Tent(Mesh mesh, u32 v, f32 z) {
    Positions(mesh)[v].z = z;
    Shade(mesh);
    return mesh;
}

void Flag(Mesh& mesh, const char* name, u32 e) {
    mesh.attributes.getOrCreate<u8>(name, Domain::Edge, geom::AttrType::Bool)[e] = 1;
}
bool Flagged(const Mesh& mesh, const char* name, u32 e) {
    const auto s = mesh.attributes.get<const u8>(name, Domain::Edge);
    return e < s.size() && s[e] != 0;
}

/// The distinct `color0` values of the live corners at @p v.
std::set<std::array<u8, 4>> CornerColors(const Mesh& mesh, u32 v) {
    const auto color = mesh.attributes.get<const std::array<u8, 4>>(geom::names::color(0), Domain::Halfedge);
    std::set<std::array<u8, 4>> out;
    for (const HalfedgeId h : mesh.topology().voh(VertexId(v))) {
        if (!mesh.topology().isBoundary(h) && h.index() < color.size()) {
            out.insert(color[h.index()]);
        }
    }
    return out;
}

/// The distinct `uv0` values of the live corners at @p v.
std::vector<Vector2f> CornerUvs(const Mesh& mesh, u32 v) {
    const auto uv = mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    std::vector<Vector2f> out;
    for (const HalfedgeId h : mesh.topology().voh(VertexId(v))) {
        if (mesh.topology().isBoundary(h) || h.index() >= uv.size()) {
            continue;
        }
        const Vector2f u = uv[h.index()];
        if (std::none_of(out.begin(), out.end(), [&](const Vector2f& o) { return o == u; })) {
            out.push_back(u);
        }
    }
    return out;
}

/// Bone 1's weight at @p v.
f32 BoneOne(const Mesh& mesh, u32 v) {
    f32 w = 0.0f;
    for (const geom::Influence& in : mesh.skin.forVertex(v)) {
        w += in.bone == 1 ? in.weight : 0.0f;
    }
    return w;
}

/// Whether @p members of @p mesh contract to one vertex by legal edge collapses
/// in SOME order, any member into any other.
bool ContractsSomehow(const Mesh& mesh, std::vector<u32> members) {
    if (members.size() <= 1) {
        return true;
    }
    for (const u32 u : members) {
        for (const u32 v : members) {
            if (u == v) {
                continue;
            }
            const HalfedgeId h = mesh.topology().findHalfedge(VertexId(u), VertexId(v));
            if (!h.valid() || !geom::IsCollapseLegal(mesh, h)) {
                continue;
            }
            Mesh copy = mesh;
            const HalfedgeId hc = std::as_const(copy).topology().findHalfedge(VertexId(u), VertexId(v));
            if (!geom::CollapseEdge(copy, hc)) {
                continue;
            }
            std::vector<u32> rest;
            for (const u32 m : members) {
                if (m != u) {
                    rest.push_back(m);
                }
            }
            if (ContractsSomehow(copy, rest)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

// ============================================================================
// §0: Collapse on many edges
// ============================================================================

TEST_CASE("audit COL-1: the two opposite edges of one quad collapse to two vertices", "[wem][audit][collapse]") {
    // 3 x 3 quads; the centre quad's bottom and top edges. Max: each edge to
    // its midpoint, so the quad goes and two vertices are left.
    const Mesh mesh = QuadGrid(3, 3);
    const ElementSet edges = Edges({EdgeAt(mesh, 1, 1, 2, 1), EdgeAt(mesh, 1, 2, 2, 2)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 14u);
    CHECK(VertexAt(out, 1.5f, 1.0f) != kInvalidId);
    CHECK(VertexAt(out, 1.5f, 2.0f) != kInvalidId);
}

TEST_CASE("audit COL-1: a ring of three parallel edges collapses each to its midpoint", "[wem][audit][collapse]") {
    // The vertical edges x = 0, 1, 2 across row 1 of a 3 x 3 grid. Max: three
    // vertices at (x, 1.5); the two quads between them go.
    const Mesh mesh = QuadGrid(3, 3);
    const ElementSet edges =
        Edges({EdgeAt(mesh, 0, 1, 0, 2), EdgeAt(mesh, 1, 1, 1, 2), EdgeAt(mesh, 2, 1, 2, 2)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 13u);
    CHECK(LiveFaces(out) == 7u);
    for (const f32 x : {0.0f, 1.0f, 2.0f}) {
        CHECK(VertexAt(out, x, 1.5f) != kInvalidId);
    }
}

TEST_CASE("audit COL-2: an edge along a crack collapses on both sides", "[wem][audit][collapse]") {
    // A 2 x 2 grid cut along x = 1; the left edge (1,0)-(1,1). Max (one
    // vertex per position): both sides go to (1, 0.5) and the crack stays shut.
    const Mesh mesh = Crack(2, 2, 1);
    const ElementSet edges = Edges({EdgeOf(mesh, 1, 4)});
    REQUIRE(edges.edges.front() != kInvalidId);
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    // Both twins of (1, 0.5) exist, and nothing is left at (1, 0) or (1, 1).
    CHECK(VertexAt(out, 1.0f, 0.5f) != kInvalidId);
    CHECK(VertexAt(out, 1.0f, 0.5f, 0.0f, 1) != kInvalidId);
    CHECK(VertexAt(out, 1.0f, 0.0f) == kInvalidId);
    CHECK(VertexAt(out, 1.0f, 1.0f) == kInvalidId);
}

TEST_CASE("audit COL-5: an open tube's border loop collapses to a cone tip", "[wem][audit][collapse]") {
    // A hexagonal open tube, two bands; the bottom ring's six vertices. Max:
    // one vertex at the ring's centre, the bottom band a cone.
    const Mesh mesh = Cylinder(6, 2, Caps::None);
    const ElementSet vertices = Vertices({0, 1, 2, 3, 4, 5});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, vertices); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 13u);
    CHECK(Near(out, Vector3f{0.0f, 0.0f, 0.0f}, 1e-5f) == 1u); // a centroid of six: not bit-exact
}

TEST_CASE("audit COL-3: a strip rung between two border vertices is refused with its own sentence",
          "[wem][audit][collapse]") {
    // 3 x 1 strip, the rung x = 1. Max makes a bowtie at (1, 0.5); WEM forbids
    // a vertex with two fans, so the expected result is a refusal that says
    // why (Q6): WouldPinch, not the generic CollapseIllegal.
    const Mesh mesh = QuadGrid(3, 1);
    const ElementSet edges = Edges({EdgeAt(mesh, 1, 0, 1, 1)});
    const Verdict verdict = Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); });
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(verdict.refusal == ModelRefusal::WouldPinch);
}

TEST_CASE("audit X1 / COL: one illegal part leaves the legal parts collapsed and is counted", "[wem][audit][collapse]") {
    // 5 x 1 strip: the rung x = 1 (a bowtie, illegal) and the bottom edge
    // (3,0)-(4,0) (legal). Q2: do the legal part, count the other.
    const Mesh mesh = QuadGrid(5, 1);
    const ElementSet edges = Edges({EdgeAt(mesh, 1, 0, 1, 1), EdgeAt(mesh, 3, 0, 4, 0)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(VertexAt(out, 3.5f, 0.0f) != kInvalidId);
    Mesh work = mesh;
    const ModelPlan plan = collapse(work, PointsOf(work), edges);
    CHECK(plan.kept == 1u);
    CHECK(plan.keptBecause == ModelRefusal::WouldPinch);
}

// ============================================================================
// §4.A Collapse: the cases and COL-4..COL-10
// ============================================================================

TEST_CASE("audit COL-case: all three edges of one triangle collapse to one vertex", "[wem][audit][collapse]") {
    // F2 3 x 3; the interior triangle (1,1) (2,1) (2,2) = vertices 5, 6, 10.
    const Mesh mesh = TriGrid(3, 3);
    const ElementSet edges = Edges({EdgeOf(mesh, 5, 6), EdgeOf(mesh, 6, 10), EdgeOf(mesh, 5, 10)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 14u);
    CHECK(Near(out, Vector3f{5.0f / 3.0f, 4.0f / 3.0f, 0.0f}) == 1u);
}

TEST_CASE("audit COL-case: faces 1 and 3 of a strip collapse to two vertices", "[wem][audit][collapse]") {
    // 5 x 2 grid (a strip two rows high, so neither face makes a bowtie);
    // faces 1 and 3 of the bottom row share no vertex. Max: one vertex each.
    const Mesh mesh = QuadGrid(5, 2);
    const ElementSet faces = Faces({1, 3});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, faces); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " live " << LiveVertices(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 12u);
    CHECK(Near(out, Vector3f{1.5f, 0.5f, 0.0f}) == 1u);
    CHECK(Near(out, Vector3f{3.5f, 0.5f, 0.0f}) == 1u);
}

TEST_CASE("audit COL-case: a vertical ring round a cylinder collapses each edge to its midpoint",
          "[wem][audit][collapse]") {
    // F5 hexagon, two bands, n-gon caps; the six verticals of band 0.
    const Mesh mesh = Cylinder(6, 2, Caps::Ngon);
    std::vector<u32> ring;
    for (u32 s = 0; s < 6; ++s) {
        ring.push_back(EdgeOf(mesh, s, 6 + s));
    }
    const ElementSet edges = Edges(ring);
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " live " << LiveVertices(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveVertices(out) == 12u);
    for (u32 s = 0; s < 6; ++s) {
        const f32 a = 6.28318530718f * static_cast<f32>(s) / 6.0f;
        CHECK(Near(out, Vector3f{std::cos(a), std::sin(a), 0.5f}, 1e-3f) == 1u);
    }
}

TEST_CASE("audit COL-case: a horizontal loop round a closed cylinder is refused with its own sentence (Q6)",
          "[wem][audit][collapse]") {
    // F5 closed; the middle ring's loop. To one point it would be two cones
    // tip to tip: a vertex with two fans, which WEM forbids. Q6: WouldPinch.
    const Mesh mesh = Cylinder(6, 2, Caps::Ngon);
    std::vector<u32> loop;
    for (u32 s = 0; s < 6; ++s) {
        loop.push_back(EdgeOf(mesh, 6 + s, 6 + (s + 1) % 6));
    }
    const ElementSet edges = Edges(loop);
    const Verdict verdict = Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); });
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(verdict.refusal == ModelRefusal::WouldPinch);
}

TEST_CASE("audit COL-case: a seam edge keeps both UV islands", "[wem][audit][collapse]") {
    // F9: 2 x 2 grid, the right column on its own island; the seam edge
    // (1,0)-(1,1). The survivor's corners keep one value per island.
    Mesh mesh = QuadGrid(2, 2);
    SeamUvs(mesh, 1.0f);
    const ElementSet edges = Edges({EdgeAt(mesh, 1, 0, 1, 1)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    const u32 survivor = NearId(out, Vector3f{1.0f, 0.5f, 0.0f});
    REQUIRE(survivor != kInvalidId);
    CHECK(CornerUvs(out, survivor).size() == 2u);
}

TEST_CASE("audit COL-case: a sharp row is kept", "[wem][audit][collapse]") {
    // F1 3 x 3, the row y = 1 sharp; collapse the vertical (1,0)-(1,1).
    Mesh mesh = QuadGrid(3, 3);
    for (u32 x = 0; x < 3; ++x) {
        Flag(mesh, geom::names::kSharp, EdgeAt(mesh, static_cast<f32>(x), 1, static_cast<f32>(x + 1), 1));
    }
    const ElementSet edges = Edges({EdgeAt(mesh, 1, 0, 1, 1)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(SharpCount(out) == 3u);
}

TEST_CASE("audit COL-4: a part that contracts legally in some order is collapsed", "[wem][audit][collapse]") {
    // Sweep: every two-edge path and every face of small fixtures. Where
    // Collapse says CollapseIllegal, try every contraction order.
    std::vector<std::pair<const char*, Mesh>> fixtures;
    fixtures.emplace_back("QuadGrid", QuadGrid(3, 3));
    fixtures.emplace_back("TriGrid", TriGrid(3, 3));
    fixtures.emplace_back("Mixed", MixedGrid());
    fixtures.emplace_back("Box", Box());
    fixtures.emplace_back("CylFan", Cylinder(6, 2, Caps::Fan));
    fixtures.emplace_back("Sphere", UvSphere(6, 4));
    fixtures.emplace_back("TriHole", TriangleHole());
    fixtures.emplace_back("OpenFan", OpenFan());
    fixtures.emplace_back("Val2", ValenceTwo());
    fixtures.emplace_back("CylNgon", Cylinder(5, 2, Caps::Ngon));
    u32 illegal = 0, found = 0;
    for (const auto& [name, mesh] : fixtures) {
        const Topology& t = mesh.topology();
        std::vector<ElementSet> selections;
        for (u32 s = 0; s < t.vertexCount(); ++s) {
            std::vector<u32> around;
            for (const VertexId n : t.vv(VertexId(s))) {
                around.push_back(n.value());
            }
            for (std::size_t i = 0; i < around.size(); ++i) {
                for (std::size_t j = i + 1; j < around.size(); ++j) {
                    selections.push_back(Edges({EdgeOf(mesh, s, around[i]), EdgeOf(mesh, s, around[j])}));
                }
            }
        }
        for (u32 f = 0; f < t.faceCount(); ++f) {
            selections.push_back(Faces({f}));
        }
        for (const ElementSet& selection : selections) {
            Mesh work = mesh;
            const PointTable points = PointsOf(work);
            const ModelPlan plan = collapse(work, points, selection);
            if (plan.refusal != ModelRefusal::CollapseIllegal) {
                continue;
            }
            ++illegal;
            std::set<u32> members;
            for (const u32 e : selection.edges) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
                members.insert(t.from(h).value());
                members.insert(t.to(h).value());
            }
            for (const u32 f : selection.faces) {
                for (const HalfedgeId h : t.fh(FaceId(f))) {
                    members.insert(t.from(h).value());
                }
            }
            if (ContractsSomehow(mesh, std::vector<u32>(members.begin(), members.end()))) {
                ++found;
                std::string what;
                for (const u32 m : members) {
                    what += std::to_string(m) + " ";
                }
                UNSCOPED_INFO(name << ": members " << what << (selection.faces.empty() ? "(edges)" : "(face)"));
            }
        }
    }
    UNSCOPED_INFO("CollapseIllegal " << illegal << ", contractible in another order " << found);
    CHECK(found == 0u);
}

TEST_CASE("audit COL-6: a point of two members counts once in the centroid and the skin", "[wem][audit][collapse]") {
    // A quad beside a pentagon with a zero-length edge 1-2: vertices 1 and 2
    // coincide at (1,0), one point. Collapse the edge (0,0)-(1,0): the
    // centroid of two positions is (0.5, 0), not (0 + 1 + 1) / 3.
    Mesh mesh = FromFaces({{0, 1, 5, 4}, {1, 2, 3, 6, 5}},
                          {{0, 0, 0}, {1, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0}});
    Fields(mesh, 2.0f);
    REQUIRE(PointsOf(mesh).pointOf[1] == PointsOf(mesh).pointOf[2]);
    const ElementSet edges = Edges({EdgeOf(mesh, 0, 1)});
    Contract contract;
    contract.degenerateOk = true;
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, contract, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    UNSCOPED_INFO("vertex near (0.667, 0): " << Near(out, Vector3f{2.0f / 3.0f, 0.0f, 0.0f}, 1e-3f));
    const u32 survivor = NearId(out, Vector3f{0.5f, 0.0f, 0.0f}, 1e-3f);
    REQUIRE(survivor != kInvalidId);
    CHECK(BoneOne(out, survivor) == Catch::Approx(0.25f).margin(1e-3));
}

TEST_CASE("audit COL-7: Collapse blends the colours at the survivor", "[wem][audit][collapse]") {
    // F18 3 x 3 with fields; collapse the interior edge (1,1)-(2,1). Every
    // corner at the survivor carries one colour, the field's at (1.5, 1).
    Mesh mesh = QuadGrid(3, 3);
    Fields(mesh, 3.0f);
    const ElementSet edges = Edges({EdgeAt(mesh, 1, 1, 2, 1)});
    Contract contract;
    contract.skinWidth = 3.0f;
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, contract, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    const u32 survivor = NearId(out, Vector3f{1.5f, 1.0f, 0.0f});
    REQUIRE(survivor != kInvalidId);
    const std::set<std::array<u8, 4>> colors = CornerColors(out, survivor);
    for (const auto& c : colors) {
        UNSCOPED_INFO("colour " << int(c[0]) << " " << int(c[1]) << " " << int(c[2]));
    }
    CHECK(colors.size() == 1u);
    CHECK(ColorFieldError(out) <= 1u);
}

TEST_CASE("audit COL-8: a triangle side's merged edge keeps its sharp and seam flags", "[wem][audit][collapse]") {
    // F2 2 x 2; collapse the interior edge (1,1)-(2,1) between triangles
    // (1, 5, 4) and (4, 5, 8). Each side triangle's two other edges become one;
    // a flag on either must survive on it (as DissolveVertex's combine does).
    const Mesh base = TriGrid(2, 2);
    const Vector3f low{1, 0, 0}, high{2, 2, 0}, mid{1.5f, 1.0f, 0.0f};
    struct Side {
        u32 a, b;
        Vector3f far;
    };
    const std::array<Side, 4> sides = {Side{1, 4, low}, Side{1, 5, low}, Side{4, 8, high}, Side{5, 8, high}};
    u32 lost = 0;
    for (const char* flag : {geom::names::kSharp, geom::names::kSeam}) {
        for (const Side& side : sides) {
            Mesh mesh = base;
            Flag(mesh, flag, EdgeOf(mesh, side.a, side.b));
            const ElementSet edges = Edges({EdgeOf(mesh, 4, 5)});
            Mesh out;
            Contract contract;
            contract.normalsOk = true;
            const Verdict verdict =
                Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, contract, &out);
            REQUIRE_FALSE(verdict.refused);
            const u32 e = EdgeOf(out, NearId(out, mid), NearId(out, side.far));
            REQUIRE(e != kInvalidId);
            if (!Flagged(out, flag, e)) {
                ++lost;
                UNSCOPED_INFO(flag << " on " << side.a << "-" << side.b << " lost");
            }
        }
    }
    CHECK(lost == 0u);
}

TEST_CASE("audit COL-10: normals round the survivor are fresh", "[wem][audit][collapse]") {
    // F1 3 x 3 with (1,1) lifted to z = 0.5; collapse (1,1)-(2,1).
    const Mesh mesh = Tent(QuadGrid(3, 3), 5, 0.5f);
    const ElementSet edges = Edges({EdgeOf(mesh, 5, 6)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    UNSCOPED_INFO("staleness " << NormalStaleness(out));
    CHECK(NormalStaleness(out) <= 1.0f);
}

// ============================================================================
// §4.A Remove (REM-1..REM-5 and the cases)
// ============================================================================

namespace {

ModelPlan removeOf(Mesh& mesh, const PointTable& points, const ElementSet& selection) {
    return geom::PlanRemove(mesh, points, selection);
}

} // namespace

TEST_CASE("audit REM-1: a quad border vertex's two faces merge into one", "[wem][audit][remove]") {
    // F1 3 x 3, the border vertex (1,0): Max merges faces 0 and 1 into one.
    const Mesh mesh = QuadGrid(3, 3);
    const ElementSet vertices = Vertices({1});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, vertices); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 8u);
    CHECK(VertexAt(out, 1, 0) == kInvalidId);
}

TEST_CASE("audit REM-1: a triangle border vertex's fan merges into one face", "[wem][audit][remove]") {
    // F2 3 x 3, the border vertex (1,0) with three triangles round it.
    const Mesh mesh = TriGrid(3, 3);
    const ElementSet vertices = Vertices({1});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, vertices); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 16u);
    CHECK(VertexAt(out, 1, 0) == kInvalidId);
}

TEST_CASE("audit REM-1 / X14: a crack vertex is removed on both sides", "[wem][audit][remove]") {
    // F8 2 x 2 cut at x = 1; the left twin of (1,1). Q5: the point goes, its
    // four faces become one.
    const Mesh mesh = Crack(2, 2, 1);
    const ElementSet vertices = Vertices({4});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, vertices); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 1u);
    CHECK(VertexAt(out, 1, 1) == kInvalidId);
}

TEST_CASE("audit REM-2: removing an interior valence-2 vertex keeps the neighbours' corner values",
          "[wem][audit][remove]") {
    // F14 with fields: vertex 6 at (1, 0.5) on the shared edge. Every corner
    // left keeps the field at its own vertex.
    Mesh mesh = ValenceTwo();
    Fields(mesh, 2.0f);
    const ElementSet vertices = Vertices({6});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, vertices); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(UvFieldError(out) <= 1e-4f);
    CHECK(ColorFieldError(out) <= 1u);
}

TEST_CASE("audit REM-3: the four edges of a plus become one face", "[wem][audit][remove]") {
    // F1 2 x 2, the four edges at the centre. Max: one face, the centre gone.
    const Mesh mesh = QuadGrid(2, 2);
    const ElementSet edges =
        Edges({EdgeOf(mesh, 4, 1), EdgeOf(mesh, 4, 3), EdgeOf(mesh, 4, 5), EdgeOf(mesh, 4, 7)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 1u);
    CHECK(VertexAt(out, 1, 1) == kInvalidId);
}

TEST_CASE("audit REM-3: two of three edges at a valence-3 vertex merge its three faces", "[wem][audit][remove]") {
    // F3: vertex 6 (1,1) has edges to 1, 7 and 11. Remove 1-6 and 6-11: the
    // hexagon and both quads are one face, the edge 6-7 inside it goes too.
    const Mesh mesh = MixedGrid();
    const ElementSet edges = Edges({EdgeOf(mesh, 1, 6), EdgeOf(mesh, 6, 11)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 5u);
}

TEST_CASE("audit REM-3: a ring round a cylinder band merges the band", "[wem][audit][remove]") {
    // F5: the six verticals of band 0. The band cannot be one face (it is an
    // annulus); Max-like: as many dissolved as stay polygons, not refused.
    const Mesh mesh = Cylinder(6, 2, Caps::Ngon);
    std::vector<u32> ring;
    for (u32 s = 0; s < 6; ++s) {
        ring.push_back(EdgeOf(mesh, s, 6 + s));
    }
    const ElementSet edges = Edges(ring);
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " faces " << LiveFaces(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) < 14u);
}

TEST_CASE("audit REM-3: an edge beside a valence-2 vertex merges the two faces", "[wem][audit][remove]") {
    // F14: remove 1-6. The two pentagons become one; 6-4 has the same face
    // on both sides and goes with vertex 6.
    const Mesh mesh = ValenceTwo();
    const ElementSet edges = Edges({EdgeOf(mesh, 1, 6)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 1u);
}

TEST_CASE("audit REM-3 / X1: one border edge does not refuse the interior edge beside it", "[wem][audit][remove]") {
    // F1 3 x 3: the interior (1,1)-(1,2) and the border (0,0)-(1,0). Q2: the
    // interior one is removed and the other counted.
    const Mesh mesh = QuadGrid(3, 3);
    const ElementSet edges = Edges({EdgeAt(mesh, 1, 1, 1, 2), EdgeAt(mesh, 0, 0, 1, 0)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 8u);
}

TEST_CASE("audit REM-4: a crack edge is not refused as a hole's edge", "[wem][audit][remove]") {
    // F8 2 x 2 at x = 1; the left edge (1,0)-(1,1) is drawn interior. Q5: it
    // merges the faces either side of the crack.
    const Mesh mesh = Crack(2, 2, 1);
    const ElementSet edges = Edges({EdgeOf(mesh, 1, 4)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.refusal != ModelRefusal::BorderEdgeRemove);
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 3u);
}

TEST_CASE("audit REM-5: the ring face is shaded fresh", "[wem][audit][remove]") {
    // F1 2 x 2 with the centre lifted; remove it: one flat octagon.
    const Mesh mesh = Tent(QuadGrid(2, 2), 4, 0.5f);
    const ElementSet vertices = Vertices({4});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, vertices); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    UNSCOPED_INFO("staleness " << NormalStaleness(out));
    CHECK(NormalStaleness(out) <= 1.0f);
}

TEST_CASE("audit REM-case: a UV-seam edge is removed with both islands kept", "[wem][audit][remove]") {
    Mesh mesh = QuadGrid(2, 2);
    SeamUvs(mesh, 1.0f);
    const ElementSet edges = Edges({EdgeAt(mesh, 1, 0, 1, 1)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 3u);
}

TEST_CASE("audit REM-case: an edge between two sections is refused (design lock)", "[wem][audit][remove]") {
    const Mesh mesh = TwoSections(2, 2, 1);
    const ElementSet edges = Edges({EdgeAt(mesh, 0, 1, 1, 1)});
    const Verdict verdict = Run(mesh, [&](Mesh& m, const PointTable& p) { return removeOf(m, p, edges); });
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(verdict.refusal == ModelRefusal::CrossesSection);
}

// ============================================================================
// §4.A Delete
// ============================================================================

namespace {

ModelPlan deleteOf(Mesh& mesh, const PointTable&, const ElementSet& selection) {
    return geom::PlanDelete(mesh, selection);
}

} // namespace

TEST_CASE("audit DEL-1: a crack edge deletes the faces on both sides", "[wem][audit][delete]") {
    // F8 2 x 2 at x = 1; the left edge (1,0)-(1,1). Q5: faces (0,0) and (1,0).
    const Mesh mesh = Crack(2, 2, 1);
    const ElementSet edges = Edges({EdgeOf(mesh, 1, 4)});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return deleteOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 2u);
}

TEST_CASE("audit DEL-5: a crack vertex deletes its twin's faces too", "[wem][audit][delete]") {
    // F8 3 x 2 at x = 1; the left twin of (1,1) (vertex 5, twin 13). Q5: all
    // four faces round the point go, two are left.
    const Mesh mesh = Crack(3, 2, 1);
    const ElementSet vertices = Vertices({5});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return deleteOf(m, p, vertices); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(LiveFaces(out) == 2u);
}

TEST_CASE("audit DEL-2: the faces left round a deleted face are shaded fresh", "[wem][audit][delete]") {
    const Mesh mesh = Tent(QuadGrid(2, 2), 4, 0.5f);
    const ElementSet faces = Faces({0});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return deleteOf(m, p, faces); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    UNSCOPED_INFO("staleness " << NormalStaleness(out));
    CHECK(NormalStaleness(out) <= 1.0f);
}

TEST_CASE("audit DEL-case: a tube's border loop deletes its band", "[wem][audit][delete]") {
    const Mesh mesh = Cylinder(6, 2, Caps::None);
    std::vector<u32> loop;
    for (u32 s = 0; s < 6; ++s) {
        loop.push_back(EdgeOf(mesh, s, (s + 1) % 6));
    }
    const ElementSet edges = Edges(loop);
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return deleteOf(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 6u);
}

TEST_CASE("audit DEL-case: nothing selected, and every face, are refused untouched", "[wem][audit][delete]") {
    const Mesh mesh = QuadGrid(2, 2);
    const Verdict empty = Run(mesh, [&](Mesh& m, const PointTable& p) { return deleteOf(m, p, ElementSet{}); });
    CHECK(empty.refusal == ModelRefusal::EmptySelection);
    CHECK(empty.failed == "");
    const Verdict all = Run(mesh, [&](Mesh& m, const PointTable& p) { return deleteOf(m, p, Vertices({4})); });
    CHECK(all.refusal == ModelRefusal::AllFaces);
    CHECK(all.failed == "");
}

// ============================================================================
// §4.A Weld
// ============================================================================

namespace {

Mesh Translated(Mesh mesh, const Vector3f& by) {
    for (Vector3f& p : Positions(mesh)) {
        p = p + by;
    }
    mesh.recomputeBounds();
    return mesh;
}

Contract Welding() {
    Contract contract;
    contract.joinsShellsOk = true;
    return contract;
}

/// Per live face, its centroid and unit Newell normal, rounded and sorted:
/// equal for two meshes whose faces sit and face alike.
std::vector<std::array<i32, 6>> FaceSignature(const Mesh& mesh) {
    const Topology& t = mesh.topology();
    const auto p = Positions(mesh);
    std::vector<std::array<i32, 6>> out;
    for (u32 f = 0; f < t.faceCount(); ++f) {
        if (t.isDeleted(FaceId(f))) {
            continue;
        }
        Vector3f c{0, 0, 0}, n{0, 0, 0};
        u32 k = 0;
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            c = c + p[t.from(h).value()];
            n = n + Cross(p[t.from(h).value()], p[t.to(h).value()]);
            ++k;
        }
        c = c * (1.0f / static_cast<f32>(k));
        n = n * (1.0f / std::max(Length(n), 1e-12f));
        const auto r = [](f32 v) { return static_cast<i32>(std::lround(v * 1000.0f)); };
        out.push_back({r(c.x), r(c.y), r(c.z), r(n.x), r(n.y), r(n.z)});
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("audit WLD-1: the four corners of a tiny quad weld to one point", "[wem][audit][weld]") {
    // F1 3 x 3 with the centre quad shrunk to 1e-3 across. Max: one point.
    Mesh mesh = QuadGrid(3, 3);
    const auto p = Positions(mesh);
    p[5] = Vector3f{1.4995f, 1.4995f, 0.0f};
    p[6] = Vector3f{1.5005f, 1.4995f, 0.0f};
    p[10] = Vector3f{1.5005f, 1.5005f, 0.0f};
    p[9] = Vector3f{1.4995f, 1.5005f, 0.0f};
    Shade(mesh);
    const ElementSet vertices = Vertices({5, 6, 9, 10});
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, vertices, 0.01f); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " live " << LiveVertices(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 13u);
    CHECK(LiveFaces(out) == 8u);
    CHECK(Near(out, Vector3f{1.5f, 1.5f, 0.0f}, 1e-3f) == 1u);
}

TEST_CASE("audit WLD-3: a tiny threshold far from the origin welds only the crack's twins", "[wem][audit][weld]") {
    // floor(p / threshold) leaves i64 at |p| / threshold > 9.2e18. The weld
    // must still join exactly the coincident twins, on either side of 0.
    for (const f32 shift : {1000.0f, -1000.0f}) {
        const Mesh mesh = Translated(Crack(2, 2, 1), Vector3f{shift, shift, 0.0f});
        const ElementSet all = AllVertices(mesh);
        Mesh out;
        const Verdict verdict = Run(
            mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, all, 1e-20f); }, Welding(), &out);
        INFO(shift << " " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
        CHECK(LiveVertices(out) == 9u);
    }
}

TEST_CASE("audit WLD-3: a row spaced 0.8 x threshold welds in pairs, never through a chain (Q9)", "[wem][audit][weld]") {
    // F1 3 x 1 squeezed to 0.8 across; the bottom row at threshold 1. Q9,
    // Blender's Merge by Distance: (0,0) takes (0.8,0), and (1.6,0), out of its
    // reach, takes (2.4,0). Two points, not one chain of four.
    Mesh mesh = QuadGrid(3, 1);
    for (Vector3f& p : Positions(mesh)) {
        p.x *= 0.8f;
    }
    Shade(mesh);
    const ElementSet row = Vertices({0, 1, 2, 3});
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, row, 1.0f); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " live " << LiveVertices(out) << " of 8 " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 6u);
    CHECK(Near(out, Vector3f{0.4f, 0.0f, 0.0f}, 1e-4f) == 1u);
    CHECK(Near(out, Vector3f{2.0f, 0.0f, 0.0f}, 1e-4f) == 1u);
}

TEST_CASE("audit WLD-4: Weld onto targets matches every source it can", "[wem][audit][weld]") {
    // Two quads: the bottom one's top edge (0,0)-(0.5,0) = targets 3, 2; the
    // top one's bottom edge (0.3,0)-(0.75,0) = sources 4, 5. Threshold 0.35:
    // 4 reaches both targets, 5 only target 2. 4 -> 3 and 5 -> 2 welds both.
    const Mesh mesh = FromFaces({{0, 1, 2, 3}, {4, 5, 6, 7}},
                                {{0, -1, 0}, {0.5f, -1, 0}, {0.5f, 0, 0}, {0, 0, 0},
                                 {0.3f, 0, 0}, {0.75f, 0, 0}, {0.75f, 1, 0}, {0.3f, 1, 0}});
    Mesh out;
    const Verdict verdict = Run(
        mesh,
        [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, Vertices({4, 5}), Vertices({2, 3}), 0.35f); },
        Welding(), &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " live " << LiveVertices(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 6u);
}

TEST_CASE("audit WLD-5: Weld on a mesh with lazy deletes welds the vertices it was given", "[wem][audit][weld]") {
    // F8 2 x 2 at x = 1, then vertex 0 collapsed into 1 without a garbage
    // collect. Weld {1, 4, 9, 10} (the twins of (1,0) and (1,1), in these ids)
    // must zip that edge of the crack and leave (1,2)'s twins apart. (The
    // (1,1) twins alone cannot weld: one vertex with two fans, rule 3.)
    Mesh mesh = Crack(2, 2, 1);
    REQUIRE(geom::CollapseEdge(mesh, std::as_const(mesh).topology().findHalfedge(VertexId(0), VertexId(1))));
    REQUIRE(mesh.topology().isDeleted(VertexId(0)));
    const ElementSet twins = Vertices({1, 4, 9, 10});
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, twins, 1e-3f); }, Welding(), &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(Near(out, Vector3f{1, 0, 0}) == 1u);
    CHECK(Near(out, Vector3f{1, 1, 0}) == 1u);
    CHECK(Near(out, Vector3f{1, 2, 0}) == 2u);
}

TEST_CASE("audit WLD-8: a threshold larger than the mesh leaves a polygon or refuses", "[wem][audit][weld]") {
    const Mesh mesh = QuadGrid(2, 2);
    const ElementSet all = AllVertices(mesh);
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, all, 100.0f); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " faces " << LiveFaces(out));
    CHECK(verdict.failed == "");
    CHECK((verdict.refused || LiveFaces(out) >= 1u));
}

TEST_CASE("audit WLD-case / X14: one side of a crack welds the crack shut", "[wem][audit][weld]") {
    const Mesh mesh = Crack(2, 2, 1);
    const ElementSet left = Vertices({1, 4, 7});
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, left, geom::DefaultWeldThreshold(m)); },
        Welding(), &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 9u);
}

TEST_CASE("audit WLD-case: two corners of one triangle drop the triangle", "[wem][audit][weld]") {
    // F2 2 x 2: (1,1) and (2,1) welded at threshold 1.01.
    const Mesh mesh = TriGrid(2, 2);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, Vertices({4, 5}), 1.01f); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 6u);
    CHECK(Near(out, Vector3f{1.5f, 1.0f, 0.0f}) == 1u);
}

TEST_CASE("audit WLD-P17: Weld after Weld is a no-op", "[wem][audit][weld]") {
    const Mesh mesh = Crack(2, 2, 1);
    Mesh once;
    REQUIRE_FALSE(Run(
                      mesh,
                      [&](Mesh& m, const PointTable& pt) {
                          return geom::PlanWeld(m, pt, AllVertices(m), geom::DefaultWeldThreshold(m));
                      },
                      Welding(), &once)
                      .refused);
    Mesh twice = once;
    const PointTable points = PointsOf(twice);
    ModelPlan plan = geom::PlanWeld(twice, points, AllVertices(twice), geom::DefaultWeldThreshold(twice));
    geom::FinishTool(twice, plan);
    CHECK_FALSE(plan.refused());
    CHECK(plan.changed == 0u);
    CHECK(SameReflected(twice, once));
}

TEST_CASE("audit X10 / Weld: normals round a weld are fresh", "[wem][audit][weld]") {
    const Mesh mesh = Tent(QuadGrid(3, 3), 5, 0.5f);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanWeld(m, pt, Vertices({5, 6}), 1.2f); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    UNSCOPED_INFO("staleness " << NormalStaleness(out));
    CHECK(NormalStaleness(out) <= 1.0f);
}

// ============================================================================
// §4.A Target Weld
// ============================================================================

TEST_CASE("audit TWL-1: a crack vertex target-welds along its edge", "[wem][audit][targetweld]") {
    // F8 2 x 2 at x = 1: the point (1,1) onto (0,1). One twin has the edge;
    // Q5: the point moves as one.
    const Mesh mesh = Crack(2, 2, 1);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(4), VertexId(3)); },
        Welding(), &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(Near(out, Vector3f{1, 1, 0}) == 0u);
}

TEST_CASE("audit TWL-1: a vertex onto its own twin is not called not adjacent", "[wem][audit][targetweld]") {
    const Mesh mesh = Crack(2, 2, 1);
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(4), VertexId(10)); },
        Welding());
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.failed == "");
    CHECK(verdict.refusal != ModelRefusal::NotAdjacent);
}

TEST_CASE("audit TWL-2: border edges facing apart do not weld into a fold", "[wem][audit][targetweld]") {
    // Two unit quads, A at x 0..1 and B at x 2..3: A's left edge onto B's right
    // edge would turn A over across B.
    const Mesh mesh = FromFaces({{0, 1, 2, 3}, {4, 5, 6, 7}}, {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                                              {2, 0, 0}, {3, 0, 0}, {3, 1, 0}, {2, 1, 0}});
    const EdgeId source(EdgeOf(mesh, 0, 3));
    const EdgeId target(EdgeOf(mesh, 5, 6));
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeldEdges(m, pt, source, target); },
        Welding(), &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " sig " << FaceSignature(out).size());
    CHECK(verdict.refused);
}

TEST_CASE("audit TWL-2: an edge target weld selects the welded edge", "[wem][audit][targetweld]") {
    Mesh work = Crack(2, 2, 1);
    const PointTable points = PointsOf(work);
    ModelPlan plan =
        geom::PlanTargetWeldEdges(work, points, EdgeId(EdgeOf(work, 1, 4)), EdgeId(EdgeOf(work, 9, 10)));
    INFO(geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::FinishTool(work, plan);
    CHECK(CheckStructure(work) == "");
    CHECK_FALSE(plan.selection.edges.empty());
}

TEST_CASE("audit TWL-3: two vertices of a triangular hole close it", "[wem][audit][targetweld]") {
    // F15: the hole (1,1) (2,1) (2,2) = 5, 6, 10. Q6: closing it stays manifold.
    const Mesh mesh = TriangleHole();
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(5), VertexId(6)); },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 15u);
    CHECK(BorderEdges(out).edges.size() == 12u);
}

TEST_CASE("audit TWL-5: two border vertices with no edge join where the result holds (Q14)", "[wem][audit][targetweld]") {
    // Q14, as Max: across a square hole, (1,1) onto (2,2) closes it. Two
    // corners of the grid's outer border would pinch (two fans at one vertex),
    // which Q6 refuses with its own sentence.
    Mesh out;
    const Verdict closed = Run(
        QuadHole(), [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(5), VertexId(10)); },
        {}, &out);
    INFO(geom::ToString(closed.refusal) << " " << closed.failed);
    REQUIRE_FALSE(closed.refused);
    CHECK(closed.failed == "");
    CHECK(LiveVertices(out) == 15u);
    CHECK(BorderEdges(out).edges.size() == 12u);
    const Verdict pinch = Run(
        QuadGrid(3, 3), [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(0), VertexId(3)); });
    CHECK(pinch.refusal == ModelRefusal::WouldPinch);
    CHECK(pinch.failed == "");
}

TEST_CASE("audit TWL-case: a border vertex onto an interior vertex", "[wem][audit][targetweld]") {
    const Mesh mesh = QuadGrid(3, 3);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(1), VertexId(5)); },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 15u);
    CHECK(VertexAt(out, 1, 0) == kInvalidId);
}

TEST_CASE("audit X10 / Target Weld: normals round the target are fresh", "[wem][audit][targetweld]") {
    const Mesh mesh = Tent(QuadGrid(3, 3), 5, 0.5f);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(6), VertexId(5)); },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    UNSCOPED_INFO("staleness " << NormalStaleness(out));
    CHECK(NormalStaleness(out) <= 1.0f);
}

namespace {
/// A unit quad (0,1,2,3) folded flat at x = 1 onto a second quad (2,1,4,5)
/// back to x = @p back: the second layer's corners 4, 5 lie on the first's
/// edge y = 0 / y = 1, coincident with 0, 3 when @p back is 0.
Mesh FoldedSheet(f32 back) {
    return FromFaces({{0, 1, 2, 3}, {2, 1, 4, 5}},
                     {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {back, 0, 0}, {back, 1, 0}});
}
} // namespace

TEST_CASE("audit FUZ-13: a Target Weld landing beside a vertex of its point takes it in", "[wem][audit][targetweld]") {
    // The fold's (1,0) onto (0,0): the folded layer's (1,0) lands beside its
    // own (0,0), the first layer's twin. One vertex, no edge of no length.
    const Mesh mesh = FoldedSheet(0.0f);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(1), VertexId(0)); },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 4u);
    CHECK(Near(out, Vector3f{0.0f, 0.0f, 0.0f}) == 1u);
}

TEST_CASE("audit FUZ-13: a Target Weld landing beside a vertex kept apart is refused", "[wem][audit][targetweld]") {
    // Q8: the folded layer's corners are copies of their own (an extrude's, a
    // detach's). Joining them would weld what was kept apart; leaving them an
    // edge of no length. Refused, the mesh as it was.
    Mesh mesh = FoldedSheet(0.0f);
    const std::vector<u32> copies = {4, 5};
    geom::MarkApart(mesh, copies);
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanTargetWeld(m, pt, VertexId(1), VertexId(0)); });
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.refusal == ModelRefusal::TargetCollapseIllegal);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit FUZ-13: an edge Target Weld onto a coincident card's other edge leaves no edge of no length",
          "[wem][audit][targetweld]") {
    // F11: the front's (0,0)-(1,0) onto the back's (1,1)-(1,0) (the fuzzer's
    // e0 -> e6): the front corner landing at (1,1) beside the front's own.
    const Mesh mesh = DoubleCard();
    Mesh out;
    const Verdict verdict = Run(
        mesh,
        [&](Mesh& m, const PointTable& pt) {
            return geom::PlanTargetWeldEdges(m, pt, EdgeId(EdgeOf(m, 0, 1)), EdgeId(EdgeOf(m, 6, 5)));
        },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit FUZ-13: a Collapse whose centre lands beside a vertex there takes it in", "[wem][audit][collapse]") {
    // The fold back to x = 0.5: the first layer's bottom edge collapses to
    // (0.5, 0), where the folded layer's corner 4 already sits beside it.
    const Mesh mesh = FoldedSheet(0.5f);
    const ElementSet edges = Edges({EdgeOf(mesh, 0, 1)});
    Mesh out;
    const Verdict verdict = Run(mesh, [&](Mesh& m, const PointTable& p) { return collapse(m, p, edges); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 4u);
    CHECK(Near(out, Vector3f{0.5f, 0.0f, 0.0f}) == 1u);
}

// ============================================================================
// §4.A Cap
// ============================================================================

TEST_CASE("audit CAP-1: a tube's cap shades flat and its rim turns hard", "[wem][audit][cap]") {
    const Mesh mesh = Cylinder(8, 2, Caps::None);
    const u32 sharpBefore = SharpCount(mesh);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanCap(m, pt, EdgeId(EdgeOf(m, 0, 1))); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    UNSCOPED_INFO("staleness " << NormalStaleness(out) << " sharp " << sharpBefore << " -> " << SharpCount(out));
    CHECK(NormalStaleness(out) <= 1.0f);
    CHECK(SharpCount(out) >= sharpBefore + 8u);
}

TEST_CASE("audit CAP-2: a hole beside a crack caps with its own four edges", "[wem][audit][cap]") {
    // F8 3 x 3 cut at x = 1, without face (1,1): the hole [1,2] x [1,2] has
    // the crack on its left. Max (one vertex per point) caps that square.
    const Mesh cracked = Crack(3, 3, 1);
    std::vector<std::vector<u32>> faces = CornerLists(cracked.faceSet());
    faces.erase(faces.begin() + 4);
    const Mesh mesh = FromFaces(faces, std::vector<Vector3f>(Positions(cracked).begin(), Positions(cracked).end()));
    Mesh work = mesh;
    const PointTable points = PointsOf(work);
    ModelPlan plan = geom::PlanCap(work, points, EdgeId(EdgeAt(work, 2, 1, 2, 2)));
    INFO(geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::FinishTool(work, plan);
    REQUIRE(plan.selection.faces.size() == 1u);
    const u32 cap = plan.selection.faces.front();
    UNSCOPED_INFO("cap valence " << work.topology().valence(FaceId(cap)) << " area " << FaceArea(work, cap));
    CHECK(work.topology().valence(FaceId(cap)) == 4u);
    CHECK(FaceArea(work, cap) == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("audit CAP-case: triangle hole, square hole and a saddle rim cap cleanly", "[wem][audit][cap]") {
    Mesh saddle = QuadHole();
    {
        const auto p = Positions(saddle);
        p[5].z = 0.2f;
        p[6].z = -0.2f;
        p[10].z = 0.2f;
        p[9].z = -0.2f;
        Shade(saddle);
    }
    const std::vector<std::pair<const char*, Mesh>> holes = {
        {"triangle", TriangleHole()}, {"square", QuadHole()}, {"saddle", saddle}};
    for (const auto& [name, mesh] : holes) {
        Mesh out;
        const Verdict verdict = Run(
            mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanCap(m, pt, EdgeId(EdgeOf(m, 5, 6))); }, {},
            &out);
        INFO(name << " " << geom::ToString(verdict.refusal) << " " << verdict.failed);
        CHECK_FALSE(verdict.refused);
        CHECK(BorderEdges(out).edges.size() == 12u);
    }
}

TEST_CASE("audit CAP-case: a single-quad card is refused as a duplicate face (design lock)", "[wem][audit][cap]") {
    const Mesh mesh = QuadGrid(1, 1);
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable& pt) { return geom::PlanCap(m, pt, EdgeId(EdgeOf(m, 0, 1))); });
    CHECK(verdict.refusal == ModelRefusal::WouldDuplicateFace);
    CHECK(verdict.failed == "");
}

// ============================================================================
// §4.A Flip
// ============================================================================

TEST_CASE("audit FLP-1: the first and last faces of an open fan flip", "[wem][audit][flip]") {
    // P5 off, as FLP-2: a flipped face's copies stay on their sources' points,
    // as Max and Blender keep it attached.
    const Mesh mesh = OpenFan();
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable&) { return geom::PlanFlip(m, Faces({0, 2})); }, Welding(), &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit FLP-2: a flipped face detaches along its border (design lock)", "[wem][audit][flip]") {
    const Mesh mesh = QuadGrid(3, 3);
    Mesh out;
    const Verdict verdict =
        Run(mesh, [&](Mesh& m, const PointTable&) { return geom::PlanFlip(m, Faces({4})); }, Welding(), &out);
    // P5 off: the copies sit on their sources' points (X2), reported apart.
    UNSCOPED_INFO("points joining shells " << PointsAcrossShells(out));
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveVertices(out) == 20u);
    CHECK(BorderEdges(out).edges.size() == 20u);
}

TEST_CASE("audit FLP-P17: Flip twice gives the original geometry", "[wem][audit][flip]") {
    // Rows stored first, as an imported mesh has them (FLP-5 is the mesh
    // without them).
    Mesh mesh = Tent(QuadGrid(3, 3), 5, 0.5f);
    geom::MaterialiseRows(mesh);
    Mesh work = mesh;
    ModelPlan first = geom::PlanFlip(work, Faces({4, 5}));
    REQUIRE_FALSE(first.refused());
    geom::FinishTool(work, first);
    ModelPlan second = geom::PlanFlip(work, first.selection);
    REQUIRE_FALSE(second.refused());
    geom::FinishTool(work, second);
    CHECK(FaceSignature(work) == FaceSignature(mesh));
    const std::vector<Tri> after = Drawn(work), before = Drawn(mesh);
    std::vector<Tri> gained, lost;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(gained));
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(lost));
    const auto show = [](const Tri& t) {
        std::ostringstream o;
        for (const auto& c : t) {
            o << "(" << c[0] << "," << c[1] << "," << c[2] << ")";
        }
        return o.str();
    };
    for (const Tri& t : gained) {
        UNSCOPED_INFO("drawn after only " << show(t));
    }
    for (const Tri& t : lost) {
        UNSCOPED_INFO("drawn before only " << show(t));
    }
    CHECK(after.size() == before.size());
    CHECK((gained.empty() && lost.empty()));
}

TEST_CASE("audit FLP-5: Flip twice keeps the drawn triangles of a face with no stored row", "[wem][audit][flip]") {
    // As P17, on a mesh whose faces have no stored row yet.
    const Mesh mesh = Tent(QuadGrid(3, 3), 5, 0.5f);
    Mesh work = mesh;
    ModelPlan first = geom::PlanFlip(work, Faces({4, 5}));
    REQUIRE_FALSE(first.refused());
    geom::FinishTool(work, first);
    ModelPlan second = geom::PlanFlip(work, first.selection);
    REQUIRE_FALSE(second.refused());
    geom::FinishTool(work, second);
    CHECK(FaceSignature(work) == FaceSignature(mesh));
    const std::vector<Tri> after = Drawn(work), before = Drawn(mesh);
    std::vector<Tri> gained, lost;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(gained));
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(lost));
    const auto show = [](const Tri& t) {
        std::ostringstream o;
        for (const auto& c : t) {
            o << "(" << c[0] << "," << c[1] << "," << c[2] << ")";
        }
        return o.str();
    };
    for (const Tri& t : gained) {
        UNSCOPED_INFO("drawn after only " << show(t));
    }
    for (const Tri& t : lost) {
        UNSCOPED_INFO("drawn before only " << show(t));
    }
    CHECK(after.size() == before.size());
    CHECK((gained.empty() && lost.empty()));
}

TEST_CASE("audit FLP-case: a whole closed box flips inside out", "[wem][audit][flip]") {
    const Mesh box = Box();
    Mesh out;
    const Verdict all = Run(box, [&](Mesh& m, const PointTable&) { return geom::PlanFlip(m, AllFaces(m)); }, {}, &out);
    INFO(geom::ToString(all.refusal) << " " << all.failed);
    REQUIRE_FALSE(all.refused);
    CHECK(all.failed == "");
    CHECK(LiveVertices(out) == 8u);
    std::vector<std::array<i32, 6>> inverted = FaceSignature(box);
    for (auto& s : inverted) {
        s[3] = -s[3];
        s[4] = -s[4];
        s[5] = -s[5];
    }
    std::sort(inverted.begin(), inverted.end());
    CHECK(FaceSignature(out) == inverted);
}

TEST_CASE("audit FLP-4: two faces touching at a vertex flip", "[wem][audit][flip]") {
    // F1 2 x 2, faces 0 and 3 (diagonal): the centre gets one copy per run.
    const Verdict touching =
        Run(QuadGrid(2, 2), [&](Mesh& m, const PointTable&) { return geom::PlanFlip(m, Faces({0, 3})); }, Welding());
    INFO(geom::ToString(touching.refusal) << " " << touching.failed);
    CHECK_FALSE(touching.refused);
    CHECK(touching.failed == "");
}

TEST_CASE("audit FLP-3: a refused Flip leaves the mesh untouched", "[wem][audit][flip]") {
    // P10 on Flip's own refusal (the open fan of FLP-1): rebuild runs on the
    // mesh before the repair says no.
    const Verdict verdict = Run(OpenFan(), [&](Mesh& m, const PointTable&) { return geom::PlanFlip(m, Faces({0, 2})); });
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.failed.find("P10") == std::string::npos);
}

// ============================================================================
// §4.A Hard / Soft (HS-2's kernel half)
// ============================================================================

TEST_CASE("audit HS-2: PlanSharpen hardens an edge and re-shades its faces", "[wem][audit][hardsoft]") {
    // PlanSharpen has no caller (the viewer's Hard / Soft is RunHardSoft); the
    // kernel half: does it work. Tent 3 x 3, the soft edge (1,1)-(2,1).
    const Mesh mesh = Tent(QuadGrid(3, 3), 5, 0.5f);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable&) { return geom::PlanSharpen(m, Edges({EdgeOf(m, 5, 6)}), true); }, {},
        &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(SharpCount(out) == 1u);
    CHECK(Flagged(out, geom::names::kSharp, EdgeOf(out, 5, 6)));
}

TEST_CASE("audit HS-2 / P17: Hard then Soft restores the flags and the normals", "[wem][audit][hardsoft]") {
    const Mesh mesh = Tent(QuadGrid(3, 3), 5, 0.5f);
    Mesh work = mesh;
    ModelPlan hard = geom::PlanSharpen(work, Edges({EdgeOf(work, 5, 6)}), true);
    REQUIRE_FALSE(hard.refused());
    geom::FinishTool(work, hard);
    ModelPlan soft = geom::PlanSharpen(work, Edges({EdgeOf(work, 5, 6)}), false);
    REQUIRE_FALSE(soft.refused());
    geom::FinishTool(work, soft);
    CHECK(SharpCount(work) == 0u);
    const auto a = work.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    const auto b = mesh.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    REQUIRE(a.size() == b.size());
    f32 worst = 0.0f;
    for (u32 h = 0; h < a.size(); ++h) {
        if (!work.topology().isBoundary(HalfedgeId(h))) {
            worst = std::max(worst, Length(a[h] - b[h]));
        }
    }
    CHECK(worst <= 1e-4f);
}

// ============================================================================
// §4.A To Quads, To Polygons (JQ-2's kernel half, JP-1)
// ============================================================================

TEST_CASE("audit JQ-2: To Quads and To Polygons on a mesh with no faces is a no-op", "[wem][audit][joinquads]") {
    // The Mesh level hands every mesh its own faces; a 0-face mesh's list is
    // empty, and the kernel answers EmptySelection, which X1 makes total.
    Mesh empty;
    empty.name = "empty";
    (void)empty.ensureConnectivity();
    const PointTable points = PointsOf(empty);
    const ModelPlan quads = geom::PlanJoinTriangles(empty, points, AllFaces(empty));
    const ModelPlan polygons = geom::PlanLimitedDissolve(empty, points, AllFaces(empty));
    INFO(geom::ToString(quads.refusal) << " / " << geom::ToString(polygons.refusal));
    CHECK_FALSE(quads.refused());
    CHECK_FALSE(polygons.refused());
}

TEST_CASE("audit JQ-case: To Quads on an all-quad selection is a no-op", "[wem][audit][joinquads]") {
    const Mesh mesh = QuadGrid(2, 2);
    Mesh work = mesh;
    const PointTable points = PointsOf(work);
    ModelPlan plan = geom::PlanJoinTriangles(work, points, AllFaces(work));
    CHECK_FALSE(plan.refused());
    CHECK(plan.changed == 0u);
    geom::FinishTool(work, plan);
    // Against the mesh as any finish leaves it (rows stored, canonical).
    Mesh finished = mesh;
    ModelPlan none;
    geom::FinishTool(finished, none);
    CHECK(SameReflected(work, finished));
}

TEST_CASE("audit JP-1: To Polygons on a flat triangulated grid is near linear", "[wem][audit][joinquads][.perf]") {
    // Every cost is 0 on a flat region: one bucket re-scanned after each
    // dissolve. Timed at growing sizes; 100 x 100 only when 30 x 30 is quick.
    std::vector<std::pair<u32, f64>> times;
    for (const u32 n : {10u, 20u, 30u, 100u}) {
        if (n == 100u && times.back().second > 5000.0) {
            UNSCOPED_INFO("100 x 100 skipped: 30 x 30 took " << times.back().second << " ms");
            break;
        }
        Mesh mesh = TriGrid(n, n);
        const PointTable points = PointsOf(mesh);
        const auto start = std::chrono::steady_clock::now();
        ModelPlan plan = geom::PlanLimitedDissolve(mesh, points, AllFaces(mesh));
        const f64 ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
        times.push_back({n, ms});
        UNSCOPED_INFO(n << " x " << n << " (" << 2 * n * n << " triangles): " << ms << " ms, " << plan.changed
                        << " dissolved");
    }
    CHECK(times.back().first == 100u);
    CHECK(times.back().second < 50.0);
}

// ============================================================================
// §4.A Triangulate, Turn, Edit Triangulation
// ============================================================================

TEST_CASE("audit TRI-P17: Triangulate on triangles is a no-op", "[wem][audit][triangulate]") {
    const Mesh mesh = TriGrid(2, 2);
    Mesh work = mesh;
    ModelPlan plan = geom::PlanTriangulate(work, AllFaces(work));
    CHECK_FALSE(plan.refused());
    CHECK(plan.changed == 0u);
    geom::FinishTool(work, plan);
    // Against the mesh as any finish leaves it (rows stored, canonical).
    Mesh finished = mesh;
    ModelPlan none;
    geom::FinishTool(finished, none);
    CHECK(SameReflected(work, finished));
}

TEST_CASE("audit TRI-1: a face whose only cut is an existing edge is kept and counted", "[wem][audit][triangulate]") {
    // A concave quad 0 1 2 3 (reflex at 0: its one cut is 0-2) and a triangle
    // (0, 2, 1) glued on 0-1 and 1-2, whose border edge IS 0-2. The kernel
    // keeps the quad and counts it; that nobody is told is X11 (viewer).
    const Mesh mesh = FromFaces({{0, 1, 2, 3}, {0, 2, 1}}, {{1.5f, 1.5f, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}});
    Mesh work = mesh;
    ModelPlan plan = geom::PlanTriangulate(work, Faces({0}));
    INFO(geom::ToString(plan.refusal));
    CHECK_FALSE(plan.refused());
    CHECK(plan.kept == 1u);
    CHECK(plan.changed == 0u);
}

TEST_CASE("audit TRN-1: Turn on a real edge keeps every corner on its field", "[wem][audit][turn]") {
    Mesh mesh = TriGrid(1, 1);
    Fields(mesh, 1.0f);
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable&) { return geom::PlanTurnEdge(m, EdgeId(EdgeOf(m, 0, 3))); },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(EdgeOf(out, VertexAt(out, 1, 0), VertexAt(out, 0, 1)) != kInvalidId);
    CHECK(UvFieldError(out) <= 1e-4f);
    CHECK(ColorFieldError(out) <= 1u);
}

TEST_CASE("audit TRN-1: Turn refuses a turn that folds", "[wem][audit][turn]") {
    // Two triangles round a reflex corner 3 (inside triangle 0 1 2): turning
    // 0-3 to 1-2 folds one triangle over the other.
    const Mesh mesh = FromFaces({{0, 1, 3}, {0, 3, 2}}, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0.3f, 0.3f, 0}});
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable&) { return geom::PlanTurnEdge(m, EdgeId(EdgeOf(m, 0, 3))); });
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.refused);
}

TEST_CASE("audit TRN-1: Turn refuses an edge between two sections", "[wem][audit][turn]") {
    const Mesh mesh =
        FromFaces({{0, 1, 3}, {0, 3, 2}}, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}}, {0, 1}, 2);
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable&) { return geom::PlanTurnEdge(m, EdgeId(EdgeOf(m, 0, 3))); });
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.refusal == ModelRefusal::CrossesSection);
}

TEST_CASE("audit TRN-1: a turned edge does not keep the old edge's sharp flag", "[wem][audit][turn]") {
    // Flat: the new diagonal has nothing to crease.
    Mesh mesh = TriGrid(1, 1);
    Flag(mesh, geom::names::kSharp, EdgeOf(mesh, 0, 3));
    Mesh out;
    const Verdict verdict = Run(
        mesh, [&](Mesh& m, const PointTable&) { return geom::PlanTurnEdge(m, EdgeId(EdgeOf(m, 0, 3))); },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(SharpCount(out) == 0u);
}

TEST_CASE("audit TRN-2: Edit Triangulation takes a diagonal in the face where it is one", "[wem][audit][turn]") {
    // Kernel half: a pentagon 0..4 and a triangle (0, 2, 1) glued on 0-1 and
    // 1-2. 0-2 is an edge of the triangle and a diagonal of the pentagon: the
    // pentagon takes it, the triangle says AdjacentCorners. Which face the
    // viewer hands in is TRN-2's other half.
    const Mesh mesh = FromFaces({{0, 1, 2, 3, 4}, {0, 2, 1}},
                                {{0, 0, 0}, {1, -0.5f, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}});
    Mesh work = mesh;
    const ModelPlan pentagon = geom::PlanEditTriangulation(work, FaceId(0), VertexId(0), VertexId(2));
    INFO(geom::ToString(pentagon.refusal));
    CHECK_FALSE(pentagon.refused());
    Mesh other = mesh;
    const ModelPlan triangle = geom::PlanEditTriangulation(other, FaceId(1), VertexId(0), VertexId(2));
    CHECK(triangle.refusal == ModelRefusal::AdjacentCorners);
}

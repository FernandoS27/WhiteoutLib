// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The Mesh workspace audit, the kernel matrix (EDIT_MODE_MESH_AUDIT_PLAN.md
/// §2.6.1 in WhiteoutFlakes): every kernel tool of §1.1-§1.2 over the fixtures,
/// the selection shapes S1-S12 and S15, its amount ladder and its options,
/// through `Run` (P1-P8, P10), and at a sample through P14-P16; P17 and S17 have
/// their own cases. One CSV row per case to $WDX_MESH_AUDIT_OUT. All hidden
/// (`[.mesh-audit]`), one TEST_CASE per tool, so a crash costs one tool.
///
/// The `audit MTX-<n>` tests at the end are the minimal cases of what the
/// matrix found; a failing one is `[!shouldfail]` until its fix.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/refine.h>
#include <whiteout/models/wem/geometry/select.h>

#include "wem_audit_fixtures.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <random>

using namespace audit;

namespace {

using geom::CutPoint;

// ============================================================================
// Maths
// ============================================================================

Vector3f V3(f32 x, f32 y, f32 z) {
    return Vector3f{x, y, z};
}
Vector3f Unit(const Vector3f& v) {
    const f32 l = Length(v);
    return l > 0.0f ? v * (1.0f / l) : v;
}

/// p -> s R p + t (P15).
struct Xf {
    std::string name;
    f32 s = 1.0f;
    std::array<f32, 9> r{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Vector3f t{0, 0, 0};
    bool rotates = false;

    Vector3f turn(const Vector3f& v) const {
        return V3(r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z,
                  r[6] * v.x + r[7] * v.y + r[8] * v.z);
    }
    Vector3f unturn(const Vector3f& v) const {
        return V3(r[0] * v.x + r[3] * v.y + r[6] * v.z, r[1] * v.x + r[4] * v.y + r[7] * v.z,
                  r[2] * v.x + r[5] * v.y + r[8] * v.z);
    }
    Vector3f point(const Vector3f& p) const {
        return turn(p) * s + t;
    }
    Vector3f back(const Vector3f& p) const {
        return unturn((p - t) * (1.0f / s));
    }
};

std::vector<Xf> Transforms() {
    Xf move{"translate"};
    move.t = V3(2000.0f, 0.0f, 0.0f);
    Xf small{"scale0.01"};
    small.s = 0.01f;
    Xf big{"scale100"};
    big.s = 100.0f;
    // 37 degrees about (1, 2, 3), by Rodrigues.
    Xf turn{"turn37"};
    turn.rotates = true;
    const Vector3f k = Unit(V3(1, 2, 3));
    const f32 c = std::cos(37.0f * 0.0174532925f), s = std::sin(37.0f * 0.0174532925f), u = 1.0f - c;
    turn.r = {c + k.x * k.x * u,       k.x * k.y * u - k.z * s, k.x * k.z * u + k.y * s,
              k.y * k.x * u + k.z * s, c + k.y * k.y * u,       k.y * k.z * u - k.x * s,
              k.z * k.x * u - k.y * s, k.z * k.y * u + k.x * s, c + k.z * k.z * u};
    return {move, small, big, turn};
}

Mesh Transformed(const Mesh& mesh, const Xf& xf) {
    Mesh out = mesh;
    for (Vector3f& p : Positions(out)) {
        p = xf.point(p);
    }
    for (Vector3f& n : out.attributes.get<Vector3f>(geom::names::kNormal, Domain::Halfedge)) {
        n = Unit(xf.turn(n));
    }
    out.recomputeBounds();
    return out;
}

// ============================================================================
// Fixtures (§2.3)
// ============================================================================

struct Fixture {
    std::string name;
    Mesh mesh;
    f32 width = 0.0f;               ///< > 0: the fields are on (F18), the skin's width.
    bool plain = true;              ///< No fields: P14-P16 run on it.
    std::set<std::string> baseline; ///< Checks that fail with no tool at all.
    u32 degFaces = 0, degEdges = 0; ///< P4's counts before any tool.
    u32 colorBefore = 0;
    bool linearFields = false;      ///< x, y >= 0: the colour and skin fields are linear (no clamp).
};

/// Zero-area faces and zero-length edges, as `CheckDegenerate` counts them.
std::pair<u32, u32> Degenerates(const Mesh& mesh, f32 eps = 1e-6f) {
    const auto p = Positions(mesh);
    Vector3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Vector3f& v : p) {
        lo = V3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
        hi = V3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
    }
    const f32 diagonal = std::max(Length(hi - lo), 1e-12f);
    const Topology& t = mesh.topology();
    u32 faces = 0, edges = 0;
    for (u32 f = 0; f < t.faceCount(); ++f) {
        faces += !t.isDeleted(FaceId(f)) && FaceArea(mesh, f) <= eps * diagonal * diagonal ? 1u : 0u;
    }
    for (u32 e = 0; e < t.edgeCount(); ++e) {
        if (!t.isDeleted(EdgeId(e))) {
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            edges += Length(p[t.from(h).value()] - p[t.to(h).value()]) <= eps * diagonal ? 1u : 0u;
        }
    }
    return {faces, edges};
}

/// `Verdict::failed` split into (check, reason).
std::vector<std::pair<std::string, std::string>> SplitFailed(const std::string& failed) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t at = 0;
    while (at < failed.size()) {
        std::size_t end = failed.find("; P", at);
        while (end != std::string::npos && !(end + 3 < failed.size() && std::isdigit(failed[end + 3]))) {
            end = failed.find("; P", end + 1);
        }
        const std::string piece = failed.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const std::size_t colon = piece.find(':');
        out.push_back({piece.substr(0, colon), colon == std::string::npos ? "" : piece.substr(colon + 2)});
        at = end == std::string::npos ? failed.size() : end + 2;
    }
    return out;
}

f32 MaxX(const Mesh& mesh) {
    f32 x = 1.0f;
    for (const Vector3f& p : Positions(mesh)) {
        x = std::max(x, p.x);
    }
    return x;
}

std::vector<Fixture> BuildFixtures() {
    const std::vector<std::pair<const char*, std::function<Mesh()>>> base = {
        {"QuadGrid", [] { return QuadGrid(3, 3); }},
        {"Strip", [] { return QuadGrid(3, 1); }},
        {"TriGrid", [] { return TriGrid(3, 3); }},
        {"MixedGrid", [] { return MixedGrid(); }},
        {"LShape", [] { return LShape(); }},
        {"Box", [] { return Box(); }},
        // Eight sides: a hexagon's 60 degree dihedral ties the default shading
        // angle, so P7 would flip on float noise.
        {"CylNone", [] { return Cylinder(8, 2, Caps::None); }},
        {"CylNgon", [] { return Cylinder(8, 2, Caps::Ngon); }},
        {"CylFan", [] { return Cylinder(8, 2, Caps::Fan); }},
        {"UvSphere", [] { return UvSphere(8, 4); }},
        {"Crack", [] { return Crack(4, 2, 2); }},
        {"TwoSections", [] { return TwoSections(3, 2, 1); }},
        {"DoubleCard", [] { return DoubleCard(); }},
        {"TouchingQuads", [] { return TouchingQuads(); }},
        {"SmoothPrism", [] { return SmoothPrism(5); }},
        {"ValenceTwo", [] { return ValenceTwo(); }},
        {"QuadHole", [] { return QuadHole(); }},
        {"TriangleHole", [] { return TriangleHole(); }},
        {"OpenFan", [] { return OpenFan(); }},
        {"Slivers", [] { return Slivers(); }},
    };
    std::vector<Fixture> out;
    for (const auto& [name, make] : base) {
        Fixture plain{name, make()};
        out.push_back(std::move(plain));
        Fixture fields{std::string(name) + "+F", make()};
        fields.width = MaxX(fields.mesh);
        fields.plain = false;
        Fields(fields.mesh, fields.width);
        out.push_back(std::move(fields));
    }
    Fixture seam{"Seam", QuadGrid(4, 2)};
    SeamUvs(seam.mesh, 2.0f);
    seam.width = 1.0f;
    seam.plain = false;
    out.push_back(std::move(seam));
    for (Fixture& f : out) {
        // FromFaces stores no rows and FinishTool does: store them first, so a
        // refusal is compared with what a finished mesh holds (P10).
        geom::MaterialiseRows(f.mesh);
        const Verdict v = Run(f.mesh, [](Mesh&, const PointTable&) { return ModelPlan{}; });
        for (const auto& [check, why] : SplitFailed(v.failed)) {
            f.baseline.insert(check);
        }
        std::tie(f.degFaces, f.degEdges) = Degenerates(f.mesh);
        f.colorBefore = ColorFieldError(f.mesh);
        f.linearFields = std::all_of(Positions(std::as_const(f.mesh)).begin(), Positions(std::as_const(f.mesh)).end(),
                                     [](const Vector3f& v) { return v.x >= 0.0f && v.y >= 0.0f; });
    }
    return out;
}

// ============================================================================
// A fixture's topology, and the selection shapes (§2.4)
// ============================================================================

struct Info {
    const Mesh& mesh;
    PointTable points;
    std::span<const Vector3f> p;
    std::vector<u32> faces, edges, verts; ///< Live.
    std::vector<std::vector<u32>> fverts, fedges, vfaces;
    std::vector<std::array<u32, 2>> everts, efaces; ///< efaces: kInvalidId on a border side.
    Vector3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, centre{0, 0, 0};
    f32 diag = 1.0f;

    explicit Info(const Mesh& m) : mesh(m), points(PointsOf(m)), p(Positions(m)) {
        const Topology& t = m.topology();
        fverts.resize(t.faceCount());
        fedges.resize(t.faceCount());
        vfaces.resize(t.vertexCount());
        everts.resize(t.edgeCount(), {kInvalidId, kInvalidId});
        efaces.resize(t.edgeCount(), {kInvalidId, kInvalidId});
        for (u32 f = 0; f < t.faceCount(); ++f) {
            if (t.isDeleted(FaceId(f))) {
                continue;
            }
            faces.push_back(f);
            for (const HalfedgeId h : t.fh(FaceId(f))) {
                fverts[f].push_back(t.from(h).value());
                fedges[f].push_back(Topology::edge(h).value());
                vfaces[t.from(h).value()].push_back(f);
            }
        }
        for (u32 e = 0; e < t.edgeCount(); ++e) {
            if (t.isDeleted(EdgeId(e))) {
                continue;
            }
            edges.push_back(e);
            const HalfedgeId a = Topology::halfedge(EdgeId(e), 0), b = Topology::halfedge(EdgeId(e), 1);
            everts[e] = {t.from(a).value(), t.to(a).value()};
            efaces[e] = {t.isBoundary(a) ? kInvalidId : t.face(a).value(),
                         t.isBoundary(b) ? kInvalidId : t.face(b).value()};
        }
        for (u32 v = 0; v < t.vertexCount(); ++v) {
            if (!t.isDeleted(VertexId(v)) && !vfaces[v].empty()) {
                verts.push_back(v);
                lo = V3(std::min(lo.x, p[v].x), std::min(lo.y, p[v].y), std::min(lo.z, p[v].z));
                hi = V3(std::max(hi.x, p[v].x), std::max(hi.y, p[v].y), std::max(hi.z, p[v].z));
            }
        }
        centre = (lo + hi) * 0.5f;
        diag = std::max(Length(hi - lo), 1e-6f);
    }
    Vector3f fc(u32 f) const {
        Vector3f c{0, 0, 0};
        for (const u32 v : fverts[f]) {
            c = c + p[v];
        }
        return c * (1.0f / static_cast<f32>(fverts[f].size()));
    }
    Vector3f em(u32 e) const {
        return (p[everts[e][0]] + p[everts[e][1]]) * 0.5f;
    }
    Vector3f normal(u32 f) const {
        Vector3f sum{0, 0, 0};
        for (std::size_t k = 0; k < fverts[f].size(); ++k) {
            sum = sum + Cross(p[fverts[f][k]], p[fverts[f][(k + 1) % fverts[f].size()]]);
        }
        return Unit(sum);
    }
    u32 pt(u32 v) const {
        return points.pointOf[v];
    }
    bool border(u32 e) const {
        return efaces[e][0] == kInvalidId || efaces[e][1] == kInvalidId;
    }
    bool borderVertex(u32 v) const {
        return mesh.topology().isBoundary(VertexId(v));
    }
    bool twin(u32 v) const {
        return points.membersOf(pt(v)).size() > 1;
    }
    bool interiorFace(u32 f) const {
        return std::none_of(fedges[f].begin(), fedges[f].end(), [&](u32 e) { return border(e); });
    }
    std::vector<u32> neighbours(u32 f) const {
        std::vector<u32> out;
        for (const u32 e : fedges[f]) {
            const u32 g = efaces[e][0] == f ? efaces[e][1] : efaces[e][0];
            if (g != kInvalidId && g != f) {
                out.push_back(g);
            }
        }
        return out;
    }
    bool shareEdge(u32 f, u32 g) const {
        const std::vector<u32> n = neighbours(f);
        return std::find(n.begin(), n.end(), g) != n.end();
    }
    u32 sharedPoints(u32 f, u32 g) const {
        u32 n = 0;
        for (const u32 a : fverts[f]) {
            for (const u32 b : fverts[g]) {
                n += pt(a) == pt(b) ? 1u : 0u;
            }
        }
        return n;
    }
    u32 sharedVertices(u32 f, u32 g) const {
        u32 n = 0;
        for (const u32 a : fverts[f]) {
            n += std::count(fverts[g].begin(), fverts[g].end(), a) != 0 ? 1u : 0u;
        }
        return n;
    }
    bool edgesTouch(u32 a, u32 b) const {
        for (const u32 x : everts[a]) {
            for (const u32 y : everts[b]) {
                if (pt(x) == pt(y)) {
                    return true;
                }
            }
        }
        return false;
    }
    bool samePointPair(u32 a, u32 b) const {
        const u32 a0 = pt(everts[a][0]), a1 = pt(everts[a][1]), b0 = pt(everts[b][0]), b1 = pt(everts[b][1]);
        return (a0 == b0 && a1 == b1) || (a0 == b1 && a1 == b0);
    }
    bool shareFace(u32 u, u32 v) const {
        for (const u32 f : vfaces[u]) {
            if (std::find(vfaces[v].begin(), vfaces[v].end(), f) != vfaces[v].end()) {
                return true;
            }
        }
        return false;
    }
    /// The corner of face @p f at vertex @p v.
    u32 corner(u32 f, u32 v) const {
        for (const HalfedgeId h : mesh.topology().fh(FaceId(f))) {
            if (mesh.topology().from(h).value() == v) {
                return h.index();
            }
        }
        return kInvalidId;
    }
};

/// The id of @p ids with the lowest score below 1e8; kInvalidId when none.
template <class Score>
u32 Best(const std::vector<u32>& ids, Score score) {
    u32 best = kInvalidId;
    f32 low = 1e8f;
    for (const u32 id : ids) {
        const f32 s = score(id);
        if (s < low) {
            low = s;
            best = id;
        }
    }
    return best;
}

std::vector<u32> Unique(std::vector<u32> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}
std::vector<u32> VertsOfFaces(const Info& in, const std::vector<u32>& faces) {
    std::vector<u32> out;
    for (const u32 f : faces) {
        out.insert(out.end(), in.fverts[f].begin(), in.fverts[f].end());
    }
    return Unique(out);
}
std::vector<u32> EdgesOfFaces(const Info& in, const std::vector<u32>& faces) {
    std::vector<u32> out;
    for (const u32 f : faces) {
        out.insert(out.end(), in.fedges[f].begin(), in.fedges[f].end());
    }
    return Unique(out);
}
std::vector<u32> VertsOfEdges(const Info& in, const std::vector<u32>& edges) {
    std::vector<u32> out;
    for (const u32 e : edges) {
        out.push_back(in.everts[e][0]);
        out.push_back(in.everts[e][1]);
    }
    return Unique(out);
}

using ShapeMap = std::map<std::string, std::vector<u32>>;

/// Two faces either side of an interior edge for which @p differs holds.
template <class Differs>
std::vector<u32> PairAcross(const Info& in, Differs differs) {
    const u32 e = Best(in.edges, [&](u32 x) {
        return !in.border(x) && differs(x) ? Length(in.em(x) - in.centre) : 1e9f;
    });
    return e == kInvalidId ? std::vector<u32>{} : std::vector<u32>{in.efaces[e][0], in.efaces[e][1]};
}

ShapeMap FaceShapes(const Info& in) {
    ShapeMap s;
    const auto d = [&](u32 f) { return Length(in.fc(f) - in.centre); };
    const u32 one = Best(in.faces, [&](u32 f) { return d(f) + (in.interiorFace(f) ? 0.0f : 1e3f); });
    if (one == kInvalidId) {
        return s;
    }
    s["S1"] = {one};
    const u32 a = Best(in.faces, [&](u32 f) { return -d(f); });
    const u32 b = Best(in.faces, [&](u32 f) { return in.sharedPoints(a, f) ? 1e9f : -Length(in.fc(f) - in.fc(a)); });
    if (b != kInvalidId) {
        std::vector<u32> far{a, b};
        const u32 c = Best(in.faces, [&](u32 f) {
            return in.sharedPoints(a, f) || in.sharedPoints(b, f)
                       ? 1e9f
                       : -std::min(Length(in.fc(f) - in.fc(a)), Length(in.fc(f) - in.fc(b)));
        });
        if (c != kInvalidId) {
            far.push_back(c);
        }
        s["S2"] = Unique(far);
    }
    const u32 g = Best(in.faces, [&](u32 f) {
        if (f == one || in.sharedPoints(one, f)) {
            return 1e9f;
        }
        for (const u32 h : in.neighbours(f)) {
            if (in.shareEdge(h, one)) {
                return d(f);
            }
        }
        return 1e9f;
    });
    if (g != kInvalidId) {
        s["S3"] = Unique({one, g});
    }
    for (const u32 n1 : in.neighbours(one)) {
        const u32 n2 =
            Best(in.neighbours(n1), [&](u32 f) { return f == one || in.sharedPoints(f, one) ? 1e9f : 0.0f; });
        if (n2 != kInvalidId) {
            s["S4"] = Unique({one, n1, n2});
            break;
        }
    }
    for (const u32 e : in.edges) {
        if (in.border(e)) {
            continue;
        }
        const ElementSet loop = geom::FaceLoop(in.mesh, in.points, EdgeId(e));
        if (loop.faces.size() < 3 || loop.faces.size() >= in.faces.size()) {
            continue;
        }
        bool closed = true;
        for (const u32 f : loop.faces) {
            u32 k = 0;
            for (const u32 h : in.neighbours(f)) {
                k += std::binary_search(loop.faces.begin(), loop.faces.end(), h) ? 1u : 0u;
            }
            closed = closed && k >= 2;
        }
        if (closed) {
            s["S5"] = loop.faces;
            break;
        }
    }
    const u32 hub = Best(in.verts, [&](u32 v) {
        return in.vfaces[v].size() >= 3 ? Length(in.p[v] - in.centre) + (in.borderVertex(v) ? 1e3f : 0.0f) : 1e9f;
    });
    if (hub != kInvalidId) {
        s["S6"] = Unique(in.vfaces[hub]);
    }
    s["S7"] = in.faces;
    f32 low = 1e8f;
    for (const u32 f : in.faces) {
        for (const u32 h : in.faces) {
            if (f < h && !in.shareEdge(f, h) && in.sharedPoints(f, h) == 1 && d(f) + d(h) < low) {
                low = d(f) + d(h);
                s["S8"] = {f, h};
            }
        }
    }
    low = 1e8f;
    for (const u32 f : in.faces) {
        for (const u32 h : in.faces) {
            if (f < h && !in.shareEdge(f, h) && in.sharedPoints(f, h) >= 2 &&
                in.sharedVertices(f, h) < in.sharedPoints(f, h) && d(f) + d(h) < low) {
                low = d(f) + d(h);
                s["S9"] = {f, h};
            }
        }
    }
    const auto uv = in.mesh.attributes.get<const Vector2f>(geom::names::uv(0), Domain::Halfedge);
    if (!uv.empty()) {
        const std::vector<u32> seam = PairAcross(in, [&](u32 e) {
            const u32 v = in.everts[e][0];
            const Vector2f a = uv[in.corner(in.efaces[e][0], v)], b = uv[in.corner(in.efaces[e][1], v)];
            return std::abs(a.x - b.x) + std::abs(a.y - b.y) > 1e-4f;
        });
        if (!seam.empty()) {
            s["S10"] = Unique(seam);
        }
    }
    const auto section = in.mesh.attributes.get<const u32>(geom::names::kSection, Domain::Face);
    const std::vector<u32> sections =
        PairAcross(in, [&](u32 e) { return section[in.efaces[e][0]] != section[in.efaces[e][1]]; });
    if (!sections.empty()) {
        s["S11"] = Unique(sections);
    }
    const u32 rim = Best(in.faces, [&](u32 f) { return in.interiorFace(f) ? 1e9f : d(f); });
    if (rim != kInvalidId) {
        const u32 next = Best(in.neighbours(rim), [&](u32 h) { return in.interiorFace(h) ? 0.0f : 1.0f; });
        s["S12"] = next == kInvalidId ? std::vector<u32>{rim} : Unique({rim, next});
    }
    return s;
}

ShapeMap EdgeShapes(const Info& in, const ShapeMap& fs) {
    ShapeMap s;
    const auto d = [&](u32 e) { return Length(in.em(e) - in.centre); };
    const u32 one = Best(in.edges, [&](u32 e) { return d(e) + (in.border(e) ? 1e3f : 0.0f); });
    if (one == kInvalidId) {
        return s;
    }
    s["S1"] = {one};
    const u32 a = Best(in.edges, [&](u32 e) { return -d(e); });
    const u32 b = Best(in.edges, [&](u32 e) { return in.edgesTouch(a, e) ? 1e9f : -Length(in.em(e) - in.em(a)); });
    if (b != kInvalidId) {
        s["S2"] = Unique({a, b});
    }
    const ElementSet ring = geom::EdgeRing(in.mesh, in.points, EdgeId(one));
    if (ring.edges.size() >= 2 && ring.edges.size() < in.edges.size()) {
        s["S3ring"] = ring.edges;
    }
    for (const u32 f : {in.efaces[one][0], in.efaces[one][1]}) {
        if (f != kInvalidId && in.fedges[f].size() == 4) {
            s["S3opp"] = Unique({in.fedges[f][0], in.fedges[f][2]});
            break;
        }
    }
    const ElementSet line = geom::EdgeLoop(in.mesh, in.points, EdgeId(one));
    std::vector<u32> chain{one};
    for (bool grew = true; grew && chain.size() < 3;) {
        grew = false;
        for (const u32 e : line.edges) {
            const bool known = std::any_of(chain.begin(), chain.end(), [&](u32 c) { return in.samePointPair(c, e); });
            if (!known && std::any_of(chain.begin(), chain.end(), [&](u32 c) { return in.edgesTouch(c, e); })) {
                chain.push_back(e);
                grew = true;
                break;
            }
        }
    }
    if (chain.size() >= 2) {
        s["S4"] = Unique(chain);
    }
    // A closed loop, interior first; else a border loop.
    for (const bool wantBorder : {false, true}) {
        for (const u32 e : in.edges) {
            if (s.count("S5") || in.border(e) != wantBorder) {
                continue;
            }
            const ElementSet loop = wantBorder ? geom::BorderLoop(in.mesh, in.points, EdgeId(e))
                                               : geom::EdgeLoop(in.mesh, in.points, EdgeId(e));
            if (loop.edges.size() < 3 || loop.edges.size() >= in.edges.size()) {
                continue;
            }
            std::map<u32, u32> uses;
            std::vector<u32> unique;
            for (const u32 x : loop.edges) {
                if (std::none_of(unique.begin(), unique.end(), [&](u32 y) { return in.samePointPair(x, y); })) {
                    unique.push_back(x);
                    ++uses[in.pt(in.everts[x][0])];
                    ++uses[in.pt(in.everts[x][1])];
                }
            }
            if (std::all_of(uses.begin(), uses.end(), [](const auto& u) { return u.second == 2; })) {
                s["S5"] = loop.edges;
            }
        }
    }
    if (fs.count("S6")) {
        s["S6"] = EdgesOfFaces(in, fs.at("S6"));
    }
    s["S7"] = in.edges;
    if (fs.count("S8")) {
        s["S8"] = EdgesOfFaces(in, fs.at("S8"));
    }
    const u32 crack = Best(in.edges, [&](u32 e) {
        return in.twin(in.everts[e][0]) && in.twin(in.everts[e][1]) ? d(e) : 1e9f;
    });
    if (crack != kInvalidId) {
        s["S9a"] = {crack};
        std::vector<u32> both;
        for (const u32 e : in.edges) {
            if (in.samePointPair(e, crack)) {
                both.push_back(e);
            }
        }
        if (both.size() > 1) {
            s["S9b"] = both;
        }
    }
    if (fs.count("S10")) {
        s["S10"] = EdgesOfFaces(in, fs.at("S10"));
    }
    if (fs.count("S11")) {
        s["S11"] = EdgesOfFaces(in, fs.at("S11"));
    }
    const u32 rim = Best(in.edges, [&](u32 e) { return in.border(e) ? d(e) : 1e9f; });
    if (rim != kInvalidId) {
        const u32 inner = Best(in.edges, [&](u32 e) { return !in.border(e) && in.edgesTouch(e, rim) ? d(e) : 1e9f; });
        s["S12"] = inner == kInvalidId ? std::vector<u32>{rim} : Unique({rim, inner});
    }
    return s;
}

ShapeMap VertexShapes(const Info& in, const ShapeMap& fs, const ShapeMap& es) {
    ShapeMap s;
    const auto d = [&](u32 v) { return Length(in.p[v] - in.centre); };
    const u32 one = Best(in.verts, [&](u32 v) { return d(v) + (in.borderVertex(v) ? 1e3f : 0.0f); });
    if (one == kInvalidId) {
        return s;
    }
    s["S1"] = {one};
    const u32 a = Best(in.verts, [&](u32 v) { return -d(v); });
    const u32 b = Best(in.verts, [&](u32 v) {
        return in.shareFace(a, v) || in.pt(a) == in.pt(v) ? 1e9f : -Length(in.p[v] - in.p[a]);
    });
    if (b != kInvalidId) {
        s["S2"] = Unique({a, b});
    }
    if (fs.count("S1") && in.fverts[fs.at("S1")[0]].size() >= 4) {
        const std::vector<u32>& c = in.fverts[fs.at("S1")[0]];
        s["S3"] = Unique({c[0], c[2]});
    }
    for (const char* name : {"S4", "S5"}) {
        if (es.count(name)) {
            s[name] = VertsOfEdges(in, es.at(name));
        }
    }
    for (const char* name : {"S6", "S8", "S10", "S11"}) {
        if (fs.count(name)) {
            s[name] = VertsOfFaces(in, fs.at(name));
        }
    }
    s["S7"] = in.verts;
    const u32 twin = Best(in.verts, [&](u32 v) { return in.twin(v) ? d(v) : 1e9f; });
    if (twin != kInvalidId) {
        s["S9a"] = {twin};
        const auto members = in.points.membersOf(in.pt(twin));
        s["S9b"] = Unique(std::vector<u32>(members.begin(), members.end()));
    }
    const u32 rim = Best(in.verts, [&](u32 v) { return in.borderVertex(v) ? d(v) : 1e9f; });
    if (rim != kInvalidId) {
        u32 inner = kInvalidId;
        for (const u32 e : in.edges) {
            for (u32 k = 0; k < 2; ++k) {
                if (in.everts[e][k] == rim && !in.borderVertex(in.everts[e][1 - k])) {
                    inner = in.everts[e][1 - k];
                }
            }
        }
        s["S12"] = inner == kInvalidId ? std::vector<u32>{rim} : Unique({rim, inner});
    }
    return s;
}

ShapeMap BorderShapes(const Info& in) {
    ShapeMap s;
    std::vector<u32> rims;
    for (const u32 e : in.edges) {
        if (in.border(e)) {
            rims.push_back(e);
        }
    }
    if (rims.empty()) {
        return s;
    }
    const auto d = [&](u32 e) { return Length(in.em(e) - in.centre); };
    const u32 one = Best(rims, d);
    s["S1"] = {one};
    const u32 a = Best(rims, [&](u32 e) { return -d(e); });
    const u32 b = Best(rims, [&](u32 e) { return in.edgesTouch(a, e) ? 1e9f : -Length(in.em(e) - in.em(a)); });
    if (b != kInvalidId) {
        s["S2"] = Unique({a, b});
    }
    for (const u32 f : in.faces) {
        std::vector<u32> mine;
        for (const u32 e : in.fedges[f]) {
            if (in.border(e)) {
                mine.push_back(e);
            }
        }
        for (std::size_t i = 0; i < mine.size() && !s.count("S3"); ++i) {
            for (std::size_t j = i + 1; j < mine.size() && !s.count("S3"); ++j) {
                if (!in.edgesTouch(mine[i], mine[j])) {
                    s["S3"] = Unique({mine[i], mine[j]});
                }
            }
        }
    }
    const ElementSet loop = geom::BorderLoop(in.mesh, in.points, EdgeId(one));
    if (!loop.edges.empty()) {
        s["S5"] = loop.edges;
        std::vector<u32> chain{one};
        for (bool grew = true; grew && chain.size() < 3;) {
            grew = false;
            for (const u32 e : loop.edges) {
                if (std::find(chain.begin(), chain.end(), e) == chain.end() &&
                    std::any_of(chain.begin(), chain.end(), [&](u32 c) { return in.edgesTouch(c, e); })) {
                    chain.push_back(e);
                    grew = true;
                    break;
                }
            }
        }
        if (chain.size() == 3 && chain.size() < loop.edges.size()) {
            s["S4"] = Unique(chain);
        }
    }
    s["S7"] = rims;
    for (const u32 x : rims) {
        for (const u32 y : rims) {
            if (x < y && !s.count("S8") && in.edgesTouch(x, y) && !in.samePointPair(x, y)) {
                bool viaVertex = false;
                for (const u32 u : in.everts[x]) {
                    viaVertex = viaVertex || u == in.everts[y][0] || u == in.everts[y][1];
                }
                if (!viaVertex) {
                    s["S8"] = {x, y};
                }
            }
        }
    }
    const u32 crack = Best(rims, [&](u32 e) { return in.twin(in.everts[e][0]) && in.twin(in.everts[e][1]) ? d(e) : 1e9f; });
    if (crack != kInvalidId) {
        s["S9a"] = {crack};
        std::vector<u32> both;
        for (const u32 e : rims) {
            if (in.samePointPair(e, crack)) {
                both.push_back(e);
            }
        }
        if (both.size() > 1) {
            s["S9b"] = both;
        }
    }
    return s;
}

// ============================================================================
// Inputs and the tool table (§1.1, §1.2)
// ============================================================================

enum class Sel : u8 { V, E, B, F };

/// One selection or pick a tool is handed: a set, ordered ids, a plane, cut points.
struct Input {
    std::string shape;
    ElementSet set;
    std::vector<u32> ids;
    std::string kinds; ///< Per id: 'v', 'e' or 'f'.
    Vector3f origin{0, 0, 0}, normal{0, 0, 0};
    CutPoint from, to;
    u32 param = 0;
    bool perPart = false; ///< P16 applies.
    bool probe = false;   ///< P14 / P15 sampled here.
    bool affine = true;   ///< P15 applies.
    bool rot = true;      ///< P15's turn applies.
};

struct Bench {
    std::vector<Fixture> fixtures;
    std::vector<std::unique_ptr<Info>> infos;
    std::vector<std::array<ShapeMap, 4>> shapes; ///< Per Sel.
};

const Bench& TheBench() {
    static const Bench bench = [] {
        Bench b;
        b.fixtures = BuildFixtures();
        for (const Fixture& f : b.fixtures) {
            b.infos.push_back(std::make_unique<Info>(f.mesh));
            const Info& in = *b.infos.back();
            std::array<ShapeMap, 4> s;
            s[3] = FaceShapes(in);
            s[1] = EdgeShapes(in, s[3]);
            s[0] = VertexShapes(in, s[3], s[1]);
            s[2] = BorderShapes(in);
            b.shapes.push_back(std::move(s));
        }
        return b;
    }();
    return bench;
}

/// P14-P16 run at every shape but the empty one: the fixtures are small.
bool Sampled(const std::string& name) {
    return name != "S15";
}

std::vector<Input> SetInputs(const std::array<ShapeMap, 4>& shapes, std::vector<Sel> kinds) {
    static const char* kLetter = "VEBF";
    std::vector<Input> out;
    for (const Sel kind : kinds) {
        const u32 k = static_cast<u32>(kind);
        for (const auto& [name, ids] : shapes[k]) {
            Input x;
            x.shape = std::string(1, kLetter[k]) + ":" + name;
            x.set = kind == Sel::V ? Vertices(ids) : kind == Sel::F ? Faces(ids) : Edges(ids);
            x.perPart = true;
            x.probe = Sampled(name);
            out.push_back(std::move(x));
        }
        Input empty;
        empty.shape = std::string(1, kLetter[k]) + ":S15";
        out.push_back(std::move(empty));
    }
    return out;
}

enum class Amt : u8 { None, Ladder, Planar, Hinge, Slide };

using PlanFn = std::function<ModelPlan(Mesh&, const PointTable&, const Input&, u32 option, f32 scale, Mesh* made)>;
using InputsFn = std::function<std::vector<Input>(const Info&, const std::array<ShapeMap, 4>&)>;

struct Op {
    std::string name;
    char group = 'A';
    InputsFn inputs;
    PlanFn plan;
    std::vector<std::string> options{""};
    Amt amount = Amt::None;
    f32 nominal = 0.5f;  ///< The ladder's max when the range is unbounded.
    u32 p6Mask = 0;      ///< Bit per option: P6 holds for it (the tool keeps corners on the surface).
    bool joins = false;  ///< P5 off.
    std::vector<Sel> kinds; ///< The set kinds, for S17.
};

InputsFn Sets(std::vector<Sel> kinds) {
    return [kinds](const Info&, const std::array<ShapeMap, 4>& s) { return SetInputs(s, kinds); };
}

std::string EdgeClass(const Info& in, u32 e) {
    if (in.twin(in.everts[e][0]) && in.twin(in.everts[e][1])) {
        return "crack";
    }
    return in.border(e) ? "bord" : "int";
}

/// Each distinct border loop: its edges, and its vertices in border order.
std::vector<std::pair<std::vector<u32>, std::vector<u32>>> BorderLoops(const Info& in) {
    const Topology& t = in.mesh.topology();
    std::vector<std::pair<std::vector<u32>, std::vector<u32>>> out;
    std::set<u32> seen;
    for (u32 h = 0; h < t.halfedgeCount(); ++h) {
        if (t.isDeleted(HalfedgeId(h)) || !t.isBoundary(HalfedgeId(h)) || seen.count(h)) {
            continue;
        }
        std::vector<u32> edges, verts;
        HalfedgeId at(h);
        for (u32 guard = 0; guard < t.halfedgeCount(); ++guard) {
            seen.insert(at.index());
            edges.push_back(Topology::edge(at).value());
            verts.push_back(t.from(at).value());
            at = t.next(at);
            if (at.index() == h) {
                break;
            }
        }
        out.push_back({edges, verts});
    }
    return out;
}

/// The edge of @p edges whose middle is lowest by (x, y, z): a choice no
/// renumbering moves.
u32 LowestEdge(const Info& in, const std::vector<u32>& edges) {
    return Best(edges, [&](u32 e) {
        const Vector3f m = in.em(e);
        return m.x * 1e4f + m.y * 1e2f + m.z;
    });
}

void Pair(std::vector<Input>& out, const std::string& shape, std::vector<u32> ids, const std::string& kinds,
          bool probe) {
    Input x;
    x.shape = shape;
    x.ids = std::move(ids);
    x.kinds = kinds;
    x.probe = probe;
    out.push_back(std::move(x));
}

std::vector<Input> TargetWeldInputs(const Info& in, const std::array<ShapeMap, 4>& s) {
    std::vector<Input> out;
    std::map<std::string, u32> per;
    for (const u32 e : in.edges) {
        const u32 a = in.everts[e][0], b = in.everts[e][1];
        const std::string cls = "adj-" + EdgeClass(in, e) + (in.borderVertex(a) ? ":Sb" : ":Si") +
                                (in.borderVertex(b) ? ":Tb" : ":Ti");
        if (per[cls]++ < 4) {
            Pair(out, cls, {a, b}, "vv", per[cls] == 1);
            Pair(out, cls + ":rev", {b, a}, "vv", false);
        }
    }
    for (const u32 v : in.verts) {
        const auto m = in.points.membersOf(in.pt(v));
        if (m.size() > 1 && m[0] == v && per["twins"]++ < 3) {
            Pair(out, "twins", {m[0], m[1]}, "vv", true);
        }
    }
    for (const u32 f : in.faces) {
        if (in.fverts[f].size() >= 4 && per["diag"]++ < 3) {
            Pair(out, "diag", {in.fverts[f][0], in.fverts[f][2]}, "vv", true);
        }
    }
    if (s[0].count("S2")) {
        Pair(out, "far", {s[0].at("S2")[0], s[0].at("S2")[1]}, "vv", true);
    }
    Pair(out, "same", {in.verts[0], in.verts[0]}, "vv", false);
    return out;
}

std::vector<Input> TargetWeldEdgeInputs(const Info& in, const std::array<ShapeMap, 4>&) {
    std::vector<Input> out;
    std::vector<u32> rims;
    for (const u32 e : in.edges) {
        if (in.border(e)) {
            rims.push_back(e);
        }
    }
    std::map<std::string, u32> per;
    for (const u32 x : rims) {
        const ElementSet loop = geom::BorderLoop(in.mesh, in.points, EdgeId(x));
        for (const u32 y : rims) {
            if (x == y) {
                continue;
            }
            bool vertex = false;
            for (const u32 u : in.everts[x]) {
                vertex = vertex || u == in.everts[y][0] || u == in.everts[y][1];
            }
            const std::string cls = in.samePointPair(x, y) ? "twinEdges"
                                    : vertex               ? "shareV"
                                    : in.edgesTouch(x, y)  ? "sharePt"
                                    : std::binary_search(loop.edges.begin(), loop.edges.end(), y) ? "sameLoop"
                                                                                                   : "otherLoop";
            if (per[cls]++ < 4) {
                Pair(out, cls, {x, y}, "ee", per[cls] == 1);
            }
        }
    }
    if (!rims.empty()) {
        Pair(out, "same", {rims[0], rims[0]}, "ee", false);
    }
    return out;
}

std::vector<Input> CapInputs(const Info& in, const std::array<ShapeMap, 4>&) {
    std::vector<Input> out;
    for (const auto& [edges, verts] : BorderLoops(in)) {
        Pair(out, "loop" + std::to_string(edges.size()), {LowestEdge(in, edges)}, "e", true);
    }
    return out;
}

std::vector<Input> EdgeEachInputs(const Info& in, const std::array<ShapeMap, 4>&) {
    std::vector<Input> out;
    for (const u32 e : in.edges) {
        Pair(out, EdgeClass(in, e) + "#" + std::to_string(e), {e}, "e", e % 4 == 0);
    }
    return out;
}

std::vector<Input> InsertInputs(const Info& in, const std::array<ShapeMap, 4>& s) {
    std::vector<Input> out;
    std::map<std::string, u32> per;
    for (const u32 e : in.edges) {
        if (per[EdgeClass(in, e)]++ >= 6) {
            continue;
        }
        for (u32 k = 0; k < 4; ++k) {
            Pair(out, EdgeClass(in, e) + "#" + std::to_string(e) + ":t" + std::to_string(k), {e}, "e", k == 2);
            out.back().param = k;
        }
    }
    (void)s;
    return out;
}

bool Adjacent(std::size_t i, std::size_t j, std::size_t n) {
    return (i + 1) % n == j || (j + 1) % n == i;
}

std::vector<Input> TurnInputs(const Info& in, const std::array<ShapeMap, 4>&) {
    std::vector<Input> out;
    std::vector<u32> tris, faceOf;
    geom::TriangulateMesh(in.mesh, tris, &faceOf);
    std::set<std::array<u32, 3>> seen;
    for (std::size_t k = 0; k < faceOf.size(); ++k) {
        const u32 f = faceOf[k];
        const std::vector<u32>& c = in.fverts[f];
        if (c.size() < 4) {
            continue;
        }
        for (u32 m = 0; m < 3; ++m) {
            u32 a = tris[3 * k + m], b = tris[3 * k + (m + 1) % 3];
            const std::size_t i = std::find(c.begin(), c.end(), a) - c.begin(),
                              j = std::find(c.begin(), c.end(), b) - c.begin();
            if (std::min(a, b) != a) {
                std::swap(a, b);
            }
            if (!Adjacent(i, j, c.size()) && seen.insert({f, a, b}).second && seen.size() <= 12) {
                Pair(out, "drawn:v" + std::to_string(c.size()), {f, a, b}, "fvv", true);
            }
        }
    }
    return out;
}

std::vector<Input> EditTriInputs(const Info& in, const std::array<ShapeMap, 4>&) {
    std::vector<Input> out;
    u32 faces = 0;
    for (const u32 f : in.faces) {
        const std::vector<u32>& c = in.fverts[f];
        if (c.size() < 4 || faces++ >= 6) {
            continue;
        }
        for (std::size_t i = 0; i < c.size(); ++i) {
            for (std::size_t j = i + 2; j < c.size(); ++j) {
                if (!Adjacent(i, j, c.size())) {
                    Pair(out, "pair:v" + std::to_string(c.size()), {f, c[i], c[j]}, "fvv", i == 0);
                }
            }
        }
    }
    return out;
}

CutPoint OnEdge(u32 e, f32 t) {
    CutPoint p;
    p.edge = e;
    p.t = t;
    return p;
}
CutPoint OnVertex(u32 v) {
    CutPoint p;
    p.vertex = v;
    return p;
}

std::vector<Input> CutInputs(const Info& in, const std::array<ShapeMap, 4>&) {
    std::vector<Input> out;
    const auto place = [&](const CutPoint& c) {
        return c.vertex != kInvalidId ? in.p[c.vertex] : c.edge != kInvalidId ? in.em(c.edge) : c.at;
    };
    const auto add = [&](const std::string& shape, const CutPoint& a, const CutPoint& b, u32 f, bool probe) {
        Input x;
        x.shape = shape;
        x.from = a;
        x.to = b;
        x.normal = Unit(Cross(place(b) - place(a), in.normal(f)));
        x.probe = probe;
        out.push_back(std::move(x));
    };
    u32 faces = 0;
    for (const u32 f : in.faces) {
        if (faces++ >= 6) {
            break;
        }
        const std::vector<u32>& c = in.fverts[f];
        const std::vector<u32>& e = in.fedges[f];
        const std::size_t n = c.size();
        bool first = true;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) {
                add(Adjacent(i, j, n) ? "corner" : "faceMid", OnEdge(e[i], 0.5f), OnEdge(e[j], 0.5f), f, first);
                if (!Adjacent(i, j, n)) {
                    add("faceVert", OnVertex(c[i]), OnVertex(c[j]), f, first);
                }
                first = false;
            }
        }
        CutPoint inside;
        inside.face = f;
        inside.at = in.fc(f);
        add("inside", inside, OnEdge(e[0], 0.5f), f, false);
        add("sameEdge", OnEdge(e[0], 0.25f), OnEdge(e[0], 0.75f), f, false);
        add("alongEdge", OnVertex(in.everts[e[0]][0]), OnVertex(in.everts[e[0]][1]), f, false);
    }
    u32 across = 0;
    for (const u32 e : in.edges) {
        if (in.border(e) || across >= 4) {
            continue;
        }
        const auto far = [&](u32 f) {
            return Best(in.fedges[f], [&](u32 x) { return in.edgesTouch(x, e) ? 1e9f : 0.0f; });
        };
        const u32 a = far(in.efaces[e][0]), b = far(in.efaces[e][1]);
        if (a != kInvalidId && b != kInvalidId) {
            add("across", OnEdge(a, 0.5f), OnEdge(b, 0.5f), in.efaces[e][0], across++ == 0);
        }
    }
    return out;
}

/// A plane through the fixture: X, Y, tilted, exactly through a vertex
/// column (on existing edges), and one that misses.
std::vector<std::pair<std::string, std::pair<Vector3f, Vector3f>>> Planes(const Info& in) {
    const u32 near = Best(in.verts, [&](u32 v) { return Length(in.p[v] - in.centre); });
    return {
        {"X", {in.centre, V3(1, 0, 0)}},
        {"Y", {in.centre, V3(0, 1, 0)}},
        {"tilt", {in.centre, Unit(V3(1, 2, 3))}},
        {"onEdges", {V3(in.p[near].x, in.centre.y, in.centre.z), V3(1, 0, 0)}},
        {"miss", {in.hi + V3(1, 1, 1) * in.diag, V3(1, 0, 0)}},
    };
}

std::vector<Input> SliceInputs(const Info& in, const std::array<ShapeMap, 4>& s) {
    std::vector<Input> out;
    for (const Input& base : SetInputs(s, {Sel::F})) {
        for (const auto& [name, plane] : Planes(in)) {
            Input x = base;
            x.shape += "|" + name;
            x.origin = plane.first;
            x.normal = plane.second;
            x.perPart = false;
            x.probe = base.probe && name != "miss";
            out.push_back(std::move(x));
        }
    }
    return out;
}

std::vector<Input> BridgeInputs(const Info& in, const std::array<ShapeMap, 4>&) {
    std::vector<Input> out;
    const auto loops = BorderLoops(in);
    for (std::size_t i = 0; i < loops.size(); ++i) {
        for (std::size_t j = i + 1; j < loops.size(); ++j) {
            Pair(out,
                 "loops" + std::to_string(loops[i].first.size()) + ":" + std::to_string(loops[j].first.size()),
                 {LowestEdge(in, loops[i].first), LowestEdge(in, loops[j].first)}, "ee", true);
        }
        const std::vector<u32>& e = loops[i].first;
        Pair(out, "same", {e[0], e[0]}, "ee", false);
        if (e.size() > 2) {
            Pair(out, "sameLoop", {e[0], e[e.size() / 2]}, "ee", false);
        }
    }
    return out;
}

std::vector<Input> BridgeFaceInputs(const Info& in, const std::array<ShapeMap, 4>& s) {
    std::vector<Input> out;
    const ShapeMap& f = s[3];
    for (const char* name : {"S2", "S3", "S8", "S9"}) {
        if (f.count(name) && f.at(name).size() >= 2) {
            Pair(out, name, {f.at(name)[0], f.at(name)[1]}, "ff", true);
        }
    }
    if (f.count("S1") && !in.neighbours(f.at("S1")[0]).empty()) {
        Pair(out, "adjacent", {f.at("S1")[0], in.neighbours(f.at("S1")[0])[0]}, "ff", true);
        Pair(out, "same", {f.at("S1")[0], f.at("S1")[0]}, "ff", false);
    }
    return out;
}

std::vector<Input> PolygonInputs(const Info& in, const std::array<ShapeMap, 4>& s) {
    std::vector<Input> out;
    for (const auto& [edges, verts] : BorderLoops(in)) {
        const std::string n = std::to_string(verts.size());
        std::vector<u32> rev(verts.rbegin(), verts.rend());
        Pair(out, "loop" + n + ":fwd", verts, std::string(verts.size(), 'v'), true);
        Pair(out, "loop" + n + ":rev", rev, std::string(rev.size(), 'v'), false);
        if (verts.size() > 3) {
            Pair(out, "run3:fwd", {verts[0], verts[1], verts[2]}, "vvv", false);
            Pair(out, "run3:rev", {verts[2], verts[1], verts[0]}, "vvv", false);
        }
    }
    if (s[3].count("S1")) {
        const std::vector<u32>& c = in.fverts[s[3].at("S1")[0]];
        Pair(out, "existing", c, std::string(c.size(), 'v'), false);
    }
    return out;
}

std::vector<Input> WholeInputs(const Info& in, bool symmetrize) {
    std::vector<Input> out;
    for (u32 axis = 0; axis < 3; ++axis) {
        const std::string name = std::string(1, "XYZ"[axis]);
        Vector3f n{0, 0, 0};
        (axis == 0 ? n.x : axis == 1 ? n.y : n.z) = 1.0f;
        Input x;
        x.param = axis;
        x.probe = true;
        if (symmetrize) {
            x.shape = "ax" + name + ":origin";
            x.affine = false;
            out.push_back(x);
        }
        x.affine = true;
        x.shape = "ax" + name + ":centre";
        x.origin = in.centre;
        x.normal = n;
        out.push_back(x);
    }
    if (!symmetrize) {
        Input x;
        x.shape = "tilt:centre";
        x.origin = in.centre;
        x.normal = Unit(V3(1, 2, 3));
        x.probe = true;
        out.push_back(x);
    }
    return out;
}

std::vector<Input> PlanarInputs(const Info& in, const std::array<ShapeMap, 4>& s, bool axisForm) {
    std::vector<Input> out;
    for (const Input& base : SetInputs(s, {Sel::V, Sel::E, Sel::F})) {
        std::vector<u32> verts = base.set.vertices;
        if (!base.set.edges.empty()) {
            verts = VertsOfEdges(in, base.set.edges);
        }
        if (!base.set.faces.empty()) {
            verts = VertsOfFaces(in, base.set.faces);
        }
        Vector3f c = in.centre;
        if (!verts.empty()) {
            c = Vector3f{0, 0, 0};
            for (const u32 v : verts) {
                c = c + in.p[v];
            }
            c = c * (1.0f / static_cast<f32>(verts.size()));
        }
        for (u32 k = 0; k < (axisForm ? 4u : 2u); ++k) {
            Input x = base;
            x.perPart = false;
            x.param = k;
            if (axisForm) {
                x.shape += std::string("|ax") + "XYZF"[k];
                x.rot = k == 3;
            } else {
                x.shape += k == 0 ? "|Z" : "|tilt";
                x.origin = c;
                x.normal = k == 0 ? V3(0, 0, 1) : Unit(V3(1, 2, 3));
            }
            out.push_back(std::move(x));
        }
    }
    return out;
}

std::vector<Input> HingeFaceInputs(const Info& in, const std::array<ShapeMap, 4>& s) {
    std::vector<Input> out;
    for (Input x : SetInputs(s, {Sel::F})) {
        // The hinge: a boundary edge of the selection, an interior one first,
        // chosen by position so no renumbering moves it.
        std::vector<u32> inner, rim;
        for (const u32 e : EdgesOfFaces(in, x.set.faces)) {
            const bool a = std::binary_search(x.set.faces.begin(), x.set.faces.end(), in.efaces[e][0]);
            const bool b = std::binary_search(x.set.faces.begin(), x.set.faces.end(), in.efaces[e][1]);
            if (a != b) {
                (in.border(e) ? rim : inner).push_back(e);
            }
        }
        const u32 hinge = !inner.empty() ? LowestEdge(in, inner) : !rim.empty() ? LowestEdge(in, rim) : kInvalidId;
        x.shape += !inner.empty() ? "|int" : "|rim";
        x.ids = {hinge == kInvalidId ? 0u : hinge};
        x.kinds = "e";
        x.perPart = false;
        out.push_back(std::move(x));
    }
    return out;
}

std::vector<Input> HingeBorderInputs(const Info& in, const std::array<ShapeMap, 4>& s) {
    std::vector<Input> out;
    for (const Input& base : SetInputs(s, {Sel::B})) {
        if (base.set.edges.empty()) {
            out.push_back(base);
            continue;
        }
        const u32 e = LowestEdge(in, base.set.edges);
        const u32 v = in.p[in.everts[e][0]].x <= in.p[in.everts[e][1]].x ? in.everts[e][0] : in.everts[e][1];
        const u32 f = in.efaces[e][0] != kInvalidId ? in.efaces[e][0] : in.efaces[e][1];
        for (u32 k = 0; k < 2; ++k) {
            Input x = base;
            x.perPart = false;
            x.shape += k == 0 ? "|axisEdge" : "|axisNormal";
            x.ids = {v};
            x.kinds = "v";
            x.normal = k == 0 ? Unit(in.p[in.everts[e][1]] - in.p[in.everts[e][0]]) : in.normal(f);
            out.push_back(std::move(x));
        }
    }
    return out;
}

ElementSet AllBut(const Mesh& mesh, const ElementSet& set) {
    ElementSet out;
    for (const u32 v : AllVertices(mesh).vertices) {
        if (!std::binary_search(set.vertices.begin(), set.vertices.end(), v)) {
            out.vertices.push_back(v);
        }
    }
    return out;
}

f32 Threshold(const Mesh& mesh, u32 option, f32 scale) {
    static const f32 kValues[] = {0.0f, 0.1f, 1.5f, 1000.0f};
    return option == 0 ? geom::DefaultWeldThreshold(mesh) : kValues[option] * scale;
}

const std::vector<Op>& Ops() {
    static const std::vector<Op> ops = [] {
        using S = Sel;
        std::vector<Op> o;
        const auto add = [&](Op op) { o.push_back(std::move(op)); };
        // ---- A: removal and clean-up
        add({"Collapse", 'A', Sets({S::V, S::E, S::F}),
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) { return geom::PlanCollapse(m, p, in.set); },
             {""}, Amt::None, 0, 1, false, {S::V, S::E, S::F}});
        add({"Weld", 'A', Sets({S::V}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32 s, Mesh*) {
                 return geom::PlanWeld(m, p, in.set, Threshold(m, k, s));
             },
             {"default", "0.1", "1.5", "1000"}, Amt::None, 0, 0, true, {S::V}});
        add({"WeldOnto", 'A', Sets({S::V}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32 s, Mesh*) {
                 const ElementSet targets = AllBut(m, in.set);
                 return geom::PlanWeld(m, p, in.set, targets, Threshold(m, k, s));
             },
             {"default", "0.1", "1.5"}, Amt::None, 0, 0, true, {S::V}});
        add({"TargetWeld", 'A', TargetWeldInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) {
                 return geom::PlanTargetWeld(m, p, VertexId(in.ids[0]), VertexId(in.ids[1]));
             },
             {""}, Amt::None, 0, 0, true});
        add({"TargetWeldEdges", 'A', TargetWeldEdgeInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) {
                 return geom::PlanTargetWeldEdges(m, p, EdgeId(in.ids[0]), EdgeId(in.ids[1]));
             },
             {""}, Amt::None, 0, 0, true});
        add({"Remove", 'A', Sets({S::V, S::E}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::RemoveParams params;
                 params.cleanVertices = k == 1;
                 return geom::PlanRemove(m, p, in.set, params);
             },
             {"", "clean"}, Amt::None, 0, 3, false, {S::V, S::E}});
        add({"Delete", 'A', Sets({S::V, S::E, S::B, S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) { return geom::PlanDelete(m, in.set); },
             {""}, Amt::None, 0, 1, false, {S::V, S::E, S::B, S::F}});
        add({"Cap", 'A', CapInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) { return geom::PlanCap(m, p, EdgeId(in.ids[0])); },
             {""}, Amt::None, 0, 1});
        add({"Flip", 'A', Sets({S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) { return geom::PlanFlip(m, in.set); },
             {""}, Amt::None, 0, 1, false, {S::F}});
        add({"Sharpen", 'A', Sets({S::E}),
             [](Mesh& m, const PointTable&, const Input& in, u32 k, f32, Mesh*) { return geom::PlanSharpen(m, in.set, k == 0); },
             {"hard", "soft"}, Amt::None, 0, 3, false, {S::E}});
        add({"ToQuads", 'A', Sets({S::F}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::JoinTrianglesParams params;
                 params.delimitFlagged = k == 0;
                 return geom::PlanJoinTriangles(m, p, in.set, params);
             },
             {"", "unflagged"}, Amt::None, 0, 3, false, {S::F}});
        add({"ToPolygons", 'A', Sets({S::F}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::LimitedDissolveParams params;
                 params.angle = k == 0 ? 0.0872664626f : 0.523598776f;
                 return geom::PlanLimitedDissolve(m, p, in.set, params);
             },
             {"5deg", "30deg"}, Amt::None, 0, 3, false, {S::F}});
        add({"Retriangulate", 'A', Sets({S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) { return geom::PlanRetriangulate(m, in.set); },
             {""}, Amt::None, 0, 1, false, {S::F}});
        add({"Triangulate", 'A', Sets({S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) { return geom::PlanTriangulate(m, in.set); },
             {""}, Amt::None, 0, 1, false, {S::F}});
        add({"Turn", 'A', TurnInputs,
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) {
                 return geom::PlanTurn(m, FaceId(in.ids[0]), VertexId(in.ids[1]), VertexId(in.ids[2]));
             },
             {""}, Amt::None, 0, 1});
        add({"EditTriangulation", 'A', EditTriInputs,
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) {
                 return geom::PlanEditTriangulation(m, FaceId(in.ids[0]), VertexId(in.ids[1]), VertexId(in.ids[2]));
             },
             {""}, Amt::None, 0, 1});
        add({"TurnEdge", 'A', EdgeEachInputs,
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) { return geom::PlanTurnEdge(m, EdgeId(in.ids[0])); },
             {""}, Amt::None, 0, 1});
        // ---- B: tools that add topology
        add({"Extrude", 'B', Sets({S::F}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 return geom::PlanExtrudeFaces(m, p, in.set, static_cast<geom::ExtrudeType>(k));
             },
             {"Group", "Local", "ByPolygon"}, Amt::Ladder, 0.5f, 0, false, {S::F}});
        add({"ExtrudeBorder", 'B', Sets({S::B}),
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) { return geom::PlanExtrudeBorder(m, p, in.set); },
             {""}, Amt::Ladder, 0.5f, 0, false, {S::B}});
        add({"Inset", 'B', Sets({S::F}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 return geom::PlanInset(m, p, in.set, static_cast<geom::InsetType>(k));
             },
             {"Group", "ByPolygon"}, Amt::Ladder, 0.5f, 3, false, {S::F}});
        add({"HingeFaces", 'B', HingeFaceInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) {
                 return geom::PlanHingeFaces(m, p, in.set, in.ids.empty() ? 0u : in.ids[0]);
             },
             {""}, Amt::Hinge, 0, 0, false, {S::F}});
        add({"HingeBorder", 'B', HingeBorderInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) {
                 return geom::PlanHingeBorder(m, p, in.set, in.ids.empty() ? 0u : in.ids[0], in.normal);
             },
             {""}, Amt::Hinge, 0, 0, false, {S::B}});
        add({"ChamferEdges", 'B', Sets({S::E}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::ChamferParams params;
                 params.segments = k + 1;
                 return geom::PlanChamferEdges(m, p, in.set, params);
             },
             {"seg1", "seg2", "seg3"}, Amt::Ladder, 0.25f, 7, false, {S::E}});
        add({"ChamferVertices", 'B', Sets({S::V}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::ChamferParams params;
                 params.open = k == 1;
                 return geom::PlanChamferVertices(m, p, in.set, params);
             },
             {"", "open"}, Amt::Ladder, 0.25f, 3, false, {S::V}});
        add({"InsertVertex", 'B', InsertInputs,
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) {
                 static const f32 kT[] = {0.0f, 0.25f, 0.5f, 1.0f};
                 return geom::PlanInsertVertex(m, EdgeId(in.ids[0]), kT[in.param]);
             },
             {""}, Amt::None, 0, 1});
        add({"Divide", 'B', Sets({S::E}),
             [](Mesh& m, const PointTable&, const Input& in, u32 k, f32, Mesh*) { return geom::PlanDivide(m, in.set, k + 2); },
             {"2", "3"}, Amt::None, 0, 3, false, {S::E}});
        add({"ConnectVertices", 'B', Sets({S::V}),
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) { return geom::PlanConnectVertices(m, p, in.set); },
             {""}, Amt::None, 0, 1, false, {S::V}});
        add({"ConnectEdges", 'B', Sets({S::E}),
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::ConnectParams params;
                 params.segments = k + 1;
                 return geom::PlanConnectEdges(m, p, in.set, params);
             },
             {"seg1", "seg2"}, Amt::Slide, 0, 3, false, {S::E}});
        add({"Cut", 'B', CutInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) { return geom::PlanCut(m, p, in.from, in.to, in.normal); },
             {""}, Amt::None, 0, 1});
        add({"Slice", 'B', SliceInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::SliceParams params;
                 params.origin = in.origin;
                 params.normal = in.normal;
                 params.mode = k == 0 ? geom::SliceMode::Cut : k == 1 ? geom::SliceMode::Split : geom::SliceMode::Remove;
                 params.keepPositive = k != 3;
                 return geom::PlanSlice(m, p, in.set, params);
             },
             {"Cut", "Split", "Remove+", "Remove-"}, Amt::None, 0, 15, false, {S::F}});
        add({"Bridge", 'B', BridgeInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::BridgeParams params;
                 params.twist = static_cast<i32>(k);
                 return geom::PlanBridge(m, p, EdgeId(in.ids[0]), EdgeId(in.ids[1]), params);
             },
             {"t0", "t1"}, Amt::None, 0, 0, true});
        add({"BridgeFaces", 'B', BridgeFaceInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::BridgeParams params;
                 params.twist = static_cast<i32>(k);
                 return geom::PlanBridgeFaces(m, p, FaceId(in.ids[0]), FaceId(in.ids[1]), params);
             },
             {"t0", "t1"}, Amt::None, 0, 0, true});
        add({"CreatePolygon", 'B', PolygonInputs,
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) { return geom::PlanCreatePolygon(m, p, in.ids); },
             {""}, Amt::None, 0, 1, true});
        // ---- C: placement and whole-mesh tools
        add({"Symmetrize", 'C', [](const Info& in, const auto&) { return WholeInputs(in, true); },
             [](Mesh& m, const PointTable& p, const Input& in, u32 k, f32, Mesh*) {
                 geom::SymmetrizeParams params;
                 params.axis = in.param;
                 params.fromPositive = k == 0;
                 params.origin = in.origin;
                 params.normal = in.normal;
                 return geom::PlanSymmetrize(m, p, params);
             },
             {"+", "-"}, Amt::None, 0, 0});
        add({"Mirror", 'C', [](const Info& in, const auto&) { return WholeInputs(in, false); },
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) {
                 geom::MirrorParams params;
                 params.origin = in.origin;
                 params.normal = in.normal;
                 return geom::PlanMirror(m, params);
             },
             {""}, Amt::None, 0, 0});
        add({"MakePlanarAxis", 'C', [](const Info& in, const auto& s) { return PlanarInputs(in, s, true); },
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) { return geom::PlanMakePlanar(m, p, in.set, in.param); },
             {""}, Amt::Planar, 1.0f, 0, false, {S::V, S::E, S::F}});
        add({"MakePlanarPlane", 'C', [](const Info& in, const auto& s) { return PlanarInputs(in, s, false); },
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) {
                 return geom::PlanMakePlanar(m, p, in.set, in.origin, in.normal);
             },
             {""}, Amt::Planar, 1.0f, 0, false, {S::V, S::E, S::F}});
        add({"MakePlanarParts", 'C', Sets({S::V, S::E, S::F}),
             [](Mesh& m, const PointTable& p, const Input& in, u32, f32, Mesh*) {
                 std::vector<geom::PlanarPart> parts;
                 const auto at = Positions(std::as_const(m));
                 for (const std::vector<u32>& part : geom::SelectionParts(m, p, in.set)) {
                     geom::PlanarPart made;
                     made.vertices = part;
                     std::vector<Vector3f> places;
                     for (const u32 v : part) {
                         places.push_back(at[v]);
                     }
                     geom::FitPlane(places, &made.origin, &made.normal);
                     parts.push_back(made);
                 }
                 return geom::PlanMakePlanar(m, parts);
             },
             {""}, Amt::Planar, 1.0f, 0, false, {S::V, S::E, S::F}});
        add({"Duplicate", 'C', Sets({S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) { return geom::PlanDuplicate(m, in.set); },
             {""}, Amt::None, 0, 1, false, {S::F}});
        add({"Detach", 'C', Sets({S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32 k, f32, Mesh* made) {
                 return geom::PlanDetach(m, in.set, *made, k == 1);
             },
             {"move", "clone"}, Amt::None, 0, 3, false, {S::F}});
        add({"DetachToElement", 'C', Sets({S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) { return geom::PlanDetachToElement(m, in.set); },
             {""}, Amt::None, 0, 1, false, {S::F}});
        add({"Subdivide", 'C', Sets({S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32 k, f32, Mesh*) {
                 geom::SubdivideParams params;
                 params.scheme = k == 1 ? geom::SubdivideScheme::Linear : geom::SubdivideScheme::CatmullClark;
                 params.creases = k != 2;
                 params.levels = k == 3 ? 2 : 1;
                 return geom::PlanSubdivide(m, in.set, params);
             },
             {"CC", "Linear", "CC-nocrease", "CC-2"}, Amt::None, 0, 2, false, {S::F}});
        add({"Smooth", 'C', Sets({S::V, S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32 k, f32, Mesh*) {
                 geom::SmoothParams params;
                 params.iterations = k == 1 ? 3 : 1;
                 params.pinBorders = k != 1;
                 params.scheme = k == 2 ? geom::SmoothScheme::Taubin : geom::SmoothScheme::Laplacian;
                 return geom::PlanSmooth(m, in.set, params);
             },
             {"lap1", "lap3-free", "taubin"}, Amt::None, 0, 0, false, {S::V, S::F}});
        add({"OrientOutward", 'C', Sets({S::F}),
             [](Mesh& m, const PointTable&, const Input& in, u32, f32, Mesh*) { return geom::PlanOrientOutward(m, in.set); },
             {""}, Amt::None, 0, 1, false, {S::F}});
        return o;
    }();
    return ops;
}

const Op& OpNamed(const std::string& name) {
    for (const Op& op : Ops()) {
        if (op.name == name) {
            return op;
        }
    }
    FAIL("no op " << name);
    return Ops().front();
}

// ============================================================================
// Comparing results: polygons as position cycles, so a tied diagonal or a
// renumbering does not count (P14-P17).
// ============================================================================

using Polys = std::vector<std::vector<Vector3f>>;

Polys PolysOf(const Mesh& mesh, const Xf* back = nullptr) {
    Polys out;
    const auto p = Positions(mesh);
    const Topology& t = mesh.topology();
    for (u32 f = 0; f < t.faceCount(); ++f) {
        if (t.isDeleted(FaceId(f))) {
            continue;
        }
        std::vector<Vector3f> poly;
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            const Vector3f& at = p[t.from(h).value()];
            poly.push_back(back ? back->back(at) : at);
        }
        out.push_back(std::move(poly));
    }
    return out;
}

bool SamePolys(const Polys& a, const Polys& b, f32 tol, std::string& why) {
    if (a.size() != b.size()) {
        why = std::to_string(b.size()) + " faces, not " + std::to_string(a.size());
        return false;
    }
    using Key = std::vector<std::array<long long, 3>>;
    const auto keys = [&](const Polys& polys) {
        std::vector<std::pair<Key, u32>> out;
        for (u32 i = 0; i < polys.size(); ++i) {
            Key k;
            for (const Vector3f& v : polys[i]) {
                k.push_back({std::llround(v.x / tol), std::llround(v.y / tol), std::llround(v.z / tol)});
            }
            std::rotate(k.begin(), std::min_element(k.begin(), k.end()), k.end());
            out.push_back({k, i});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto ka = keys(a), kb = keys(b);
    std::vector<u32> la, lb;
    std::size_t i = 0, j = 0;
    while (i < ka.size() || j < kb.size()) {
        if (j == kb.size() || (i < ka.size() && ka[i].first < kb[j].first)) {
            la.push_back(ka[i++].second);
        } else if (i == ka.size() || kb[j].first < ka[i].first) {
            lb.push_back(kb[j++].second);
        } else {
            ++i;
            ++j;
        }
    }
    std::vector<bool> used(lb.size(), false);
    for (const u32 x : la) {
        bool found = false;
        for (std::size_t y = 0; y < lb.size() && !found; ++y) {
            const auto& pa = a[x];
            const auto& pb = b[lb[y]];
            if (used[y] || pa.size() != pb.size()) {
                continue;
            }
            for (std::size_t shift = 0; shift < pb.size() && !found; ++shift) {
                bool same = true;
                for (std::size_t c = 0; c < pa.size() && same; ++c) {
                    const Vector3f d = pa[c] - pb[(c + shift) % pb.size()];
                    same = std::abs(d.x) <= 2 * tol && std::abs(d.y) <= 2 * tol && std::abs(d.z) <= 2 * tol;
                }
                if (same) {
                    used[y] = found = true;
                }
            }
        }
        if (!found) {
            const Vector3f& c = a[x][0];
            std::ostringstream s;
            s << "face at (" << c.x << " " << c.y << " " << c.z << ") differs";
            why = s.str();
            return false;
        }
    }
    return true;
}

// ============================================================================
// Rebuilding a fixture renumbered (P14) or doubled (P16)
// ============================================================================

struct Copy {
    std::vector<u32> vmap;  ///< Old vertex -> new.
    std::vector<u32> order; ///< New face i is old face order[i].
    Vector3f shift{0, 0, 0};
};

Mesh Assemble(const Mesh& src, const std::vector<Copy>& copies, u32 vertexCount) {
    const std::vector<std::vector<u32>> corners = CornerLists(src.faceSet());
    const auto p = Positions(src);
    const auto section = src.attributes.get<const u32>(geom::names::kSection, Domain::Face);
    std::vector<std::vector<u32>> faces;
    std::vector<Vector3f> places(vertexCount);
    std::vector<u32> sections;
    for (const Copy& c : copies) {
        for (u32 v = 0; v < p.size(); ++v) {
            places[c.vmap[v]] = p[v] + c.shift;
        }
        for (const u32 f : c.order) {
            std::vector<u32> face;
            for (const u32 v : corners[f]) {
                face.push_back(c.vmap[v]);
            }
            faces.push_back(face);
            sections.push_back(f < section.size() ? section[f] : 0u);
        }
    }
    Mesh out = FromFaces(faces, places, sections, std::max<u32>(1, static_cast<u32>(src.sections.size())));
    const auto sharp = src.attributes.get<const u8>(geom::names::kSharp, Domain::Edge);
    if (!sharp.empty()) {
        std::span<u8> put = out.attributes.getOrCreate<u8>(geom::names::kSharp, Domain::Edge, geom::AttrType::Bool);
        for (u32 e = 0; e < sharp.size(); ++e) {
            if (sharp[e] == 0 || src.topology().isDeleted(EdgeId(e))) {
                continue;
            }
            const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
            for (const Copy& c : copies) {
                const u32 ne = EdgeOf(out, c.vmap[src.topology().from(h).value()], c.vmap[src.topology().to(h).value()]);
                if (ne != kInvalidId) {
                    put[ne] = 1;
                }
            }
        }
    }
    geom::MaterialiseRows(out);
    return out;
}

/// Old element -> new, for each kind.
struct Remap {
    std::vector<u32> v, e, f;
};

Remap RemapOf(const Info& in, const Mesh& made, const Copy& c, u32 faceOffset) {
    Remap r;
    r.v = c.vmap;
    r.f.assign(in.fverts.size(), kInvalidId);
    for (u32 i = 0; i < c.order.size(); ++i) {
        r.f[c.order[i]] = faceOffset + i;
    }
    r.e.assign(in.everts.size(), kInvalidId);
    for (const u32 e : in.edges) {
        r.e[e] = EdgeOf(made, c.vmap[in.everts[e][0]], c.vmap[in.everts[e][1]]);
    }
    return r;
}

u32 MapId(const std::vector<u32>& m, u32 id) {
    return id < m.size() ? m[id] : id;
}

Input Mapped(const Input& in, const std::vector<const Remap*>& maps) {
    Input out = in;
    out.set = ElementSet{};
    for (const Remap* r : maps) {
        for (const u32 v : in.set.vertices) {
            out.set.vertices.push_back(MapId(r->v, v));
        }
        for (const u32 e : in.set.edges) {
            out.set.edges.push_back(MapId(r->e, e));
        }
        for (const u32 f : in.set.faces) {
            out.set.faces.push_back(MapId(r->f, f));
        }
    }
    out.set.normalise();
    const Remap& r = *maps.front();
    for (std::size_t k = 0; k < in.ids.size(); ++k) {
        out.ids[k] = MapId(in.kinds[k] == 'v' ? r.v : in.kinds[k] == 'e' ? r.e : r.f, in.ids[k]);
    }
    for (CutPoint* c : {&out.from, &out.to}) {
        c->vertex = MapId(r.v, c->vertex);
        c->edge = MapId(r.e, c->edge);
        c->face = MapId(r.f, c->face);
    }
    return out;
}

Input TransformedInput(const Input& in, const Xf& xf) {
    Input out = in;
    out.origin = xf.point(in.origin);
    out.normal = Length(in.normal) > 0.0f ? Unit(xf.turn(in.normal)) : in.normal;
    out.from.at = xf.point(in.from.at);
    out.to.at = xf.point(in.to.at);
    return out;
}

// ============================================================================
// The CSV
// ============================================================================

std::string OutPath() {
    const char* env = std::getenv("WDX_MESH_AUDIT_OUT");
    return env && *env ? env
                       : "C:/Users/ferna/AppData/Local/Temp/claude/332e53e181c462d4/"
                         "b8fce033-700d-4126-ba53-d4312e5b3836/scratchpad/mesh_audit_matrix.csv";
}

std::string Quoted(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) {
        return s;
    }
    std::string out = "\"";
    for (const char c : s) {
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    }
    return out + "\"";
}

struct Sink {
    FILE* csv = nullptr;
    FILE* progress = nullptr;
    u64 rows = 0, ok = 0, refused = 0, failed = 0;

    Sink() {
        const std::string path = OutPath();
        csv = std::fopen(path.c_str(), "ab");
        if (csv) {
            std::fseek(csv, 0, SEEK_END);
        }
        if (csv && std::ftell(csv) == 0) {
            std::fputs("op,fixture,shape,amount,options,outcome,failed\n", csv);
        }
        progress = std::fopen((path + ".progress").c_str(), "wb");
    }
    ~Sink() {
        if (csv) {
            std::fclose(csv);
        }
        if (progress) {
            std::fclose(progress);
        }
    }
    /// The case about to run, so a crash names it.
    void begin(const std::string& key) {
        if (progress) {
            std::rewind(progress);
            std::fprintf(progress, "%-400s\n", key.c_str());
            std::fflush(progress);
        }
    }
    void row(const std::string& op, const std::string& fixture, const std::string& shape, const std::string& amount,
             const std::string& option, const std::string& outcome, const std::string& why) {
        ++rows;
        ok += outcome == "ok" ? 1u : 0u;
        refused += outcome != "ok" && outcome.rfind("skip", 0) != 0 ? 1u : 0u;
        failed += why.empty() ? 0u : 1u;
        if (csv) {
            std::fprintf(csv, "%s,%s,%s,%s,%s,%s,%s\n", Quoted(op).c_str(), Quoted(fixture).c_str(),
                         Quoted(shape).c_str(), Quoted(amount).c_str(), Quoted(option).c_str(), Quoted(outcome).c_str(),
                         Quoted(why).c_str());
            std::fflush(csv);
        }
    }
};

Sink& TheSink() {
    static Sink sink;
    return sink;
}

// ============================================================================
// One case
// ============================================================================

/// Cases known to kill the process; the sweep records them without running.
const char* KnownCrash(const Op& op, const Fixture& fx, const Input& in) {
    const Topology& t = fx.mesh.topology();
    if (op.name == "Remove") {
        for (const u32 v : in.set.vertices) {
            if (v < t.vertexCount() && t.isBoundary(VertexId(v))) {
                return "REM-1";
            }
        }
    }
    return nullptr;
}

struct Outcome {
    Verdict verdict;
    Mesh out;
    std::string failed;
};

Outcome RunOne(const Op& op, const Mesh& mesh, const Input& in, u32 option, std::optional<f32> amount,
               f32 scale = 1.0f, const Fixture* fx = nullptr) {
    Outcome o;
    Mesh made;
    Contract contract;
    contract.joinsShellsOk = op.joins;
    contract.degenerateOk = true; // P4 is counted here, against the fixture's own.
    const bool p6 = ((op.p6Mask >> option) & 1u) != 0;
    if (fx && p6 && fx->linearFields) {
        contract.skinWidth = fx->width;
    }
    o.verdict = Run(
        mesh, [&](Mesh& m, const PointTable& p) { return op.plan(m, p, in, option, scale, &made); }, contract,
        &o.out, geom::kDefaultShadingAngle, amount);
    if (!fx) {
        return o;
    }
    std::string failed;
    const auto keep = [&](const std::string& check, const std::string& why) {
        failed += (failed.empty() ? "" : "; ") + check + ": " + why;
    };
    for (const auto& [check, why] : SplitFailed(o.verdict.failed)) {
        if ((check == "P6" && !p6) || fx->baseline.count(check)) {
            continue;
        }
        keep(check, why);
    }
    if (!o.verdict.refused) {
        const auto [faces, edges] = Degenerates(o.out);
        if (faces > fx->degFaces || edges > fx->degEdges) {
            keep("P4", std::to_string(faces) + " zero-area faces, " + std::to_string(edges) + " zero-length edges");
        }
        if (p6 && fx->width > 0.0f && fx->linearFields && ColorFieldError(o.out) > std::max<u32>(fx->colorBefore, 1) + 1) {
            keep("P6", "colour off the field by " + std::to_string(ColorFieldError(o.out)));
        }
        if (LiveFaces(made) > 0) {
            const std::string s = CheckStructure(made);
            if (!s.empty()) {
                keep("P1", "made mesh: " + s);
            }
        }
    }
    o.failed = failed;
    return o;
}

std::string RefusalName(const Verdict& v) {
    return v.refused ? geom::ToString(v.refusal) : "ok";
}

bool LengthAmount(const Op& op) {
    return op.amount == Amt::Ladder;
}

std::string P14(const Op& op, const Fixture& fx, const Info& info, const Input& in, u32 option,
                std::optional<f32> amount, const Outcome& base, u32 seed) {
    std::mt19937 rng(seed);
    Copy c;
    c.vmap.resize(fx.mesh.vertexCount());
    std::iota(c.vmap.begin(), c.vmap.end(), 0u);
    std::shuffle(c.vmap.begin(), c.vmap.end(), rng);
    c.order = info.faces;
    std::shuffle(c.order.begin(), c.order.end(), rng);
    const Mesh mesh = Assemble(fx.mesh, {c}, fx.mesh.vertexCount());
    const Remap r = RemapOf(info, mesh, c, 0);
    const Outcome o = RunOne(op, mesh, Mapped(in, {&r}), option, amount);
    std::string why;
    if (o.verdict.refusal != base.verdict.refusal) {
        return "P14: renumbered gives " + RefusalName(o.verdict) + ", not " + RefusalName(base.verdict);
    }
    if (!o.verdict.refused && !SamePolys(PolysOf(base.out), PolysOf(o.out), 1e-4f * info.diag, why)) {
        return "P14: renumbered " + why;
    }
    return {};
}

std::string P15(const Op& op, const Fixture& fx, const Info& info, const Input& in, u32 option,
                std::optional<f32> amount, const Outcome& base) {
    std::string out;
    for (const Xf& xf : Transforms()) {
        if (!in.affine || (xf.rotates && !in.rot)) {
            continue;
        }
        const Mesh mesh = Transformed(fx.mesh, xf);
        std::optional<f32> scaled = amount;
        if (amount && LengthAmount(op)) {
            scaled = *amount * xf.s;
        }
        const Outcome o = RunOne(op, mesh, TransformedInput(in, xf), option, scaled, xf.s);
        std::string why;
        if (o.verdict.refusal != base.verdict.refusal) {
            why = "gives " + RefusalName(o.verdict) + ", not " + RefusalName(base.verdict);
        } else if (!o.verdict.refused) {
            const f32 tol = 1e-3f * info.diag + (Length(xf.t) > 0.0f ? 1e-3f : 0.0f);
            SamePolys(PolysOf(base.out), PolysOf(o.out, &xf), tol, why);
        }
        if (!why.empty()) {
            out += (out.empty() ? "" : "; ") + std::string("P15: ") + xf.name + " " + why;
        }
    }
    return out;
}

std::string P16(const Op& op, const Fixture& fx, const Info& info, const Input& in, u32 option,
                std::optional<f32> amount, const Outcome& base) {
    const u32 n = fx.mesh.vertexCount();
    Copy a, b, alone;
    a.vmap.resize(n);
    std::iota(a.vmap.begin(), a.vmap.end(), 0u);
    a.order = info.faces;
    b = a;
    for (u32& v : b.vmap) {
        v += n;
    }
    b.shift = V3(3.0f * info.diag + 10.0f, 0.0f, 0.0f);
    alone = a;
    alone.shift = b.shift;
    const Mesh both = Assemble(fx.mesh, {a, b}, 2 * n);
    const Mesh second = Assemble(fx.mesh, {alone}, n);
    const Remap ra = RemapOf(info, both, a, 0), rb = RemapOf(info, both, b, static_cast<u32>(info.faces.size()));
    const Remap r2 = RemapOf(info, second, alone, 0);
    const Outcome u = RunOne(op, both, Mapped(in, {&ra, &rb}), option, amount);
    const Outcome o2 = RunOne(op, second, Mapped(in, {&r2}), option, amount);
    if (base.verdict.refusal != o2.verdict.refusal) {
        return {}; // the part alone is not invariant: P15's to say
    }
    if (u.verdict.refusal != base.verdict.refusal) {
        return "P16: two parts give " + RefusalName(u.verdict) + ", one gives " + RefusalName(base.verdict);
    }
    if (u.verdict.refused) {
        return {};
    }
    Polys parts = PolysOf(base.out);
    const Polys more = PolysOf(o2.out);
    parts.insert(parts.end(), more.begin(), more.end());
    std::string why;
    if (!SamePolys(parts, PolysOf(u.out), 1e-4f * (info.diag + Length(b.shift)), why)) {
        return "P16: two parts " + why;
    }
    return {};
}

std::string AmountLabel(const char* name, f32 value) {
    std::ostringstream s;
    s << name << "=" << value;
    return s.str();
}

void RunCase(const Op& op, const Fixture& fx, const Info& info, const Input& in, u32 option, bool checks) {
    Sink& sink = TheSink();
    const std::string& optionName = op.options[option];
    if (const char* why = KnownCrash(op, fx, in)) {
        sink.row(op.name, fx.name, in.shape, "-", optionName, std::string("skipped-crash:") + why, "");
        return;
    }
    sink.begin(op.name + " | " + fx.name + " | " + in.shape + " | " + optionName);
    std::vector<std::pair<std::string, std::optional<f32>>> ladder;
    u32 sample = 0;
    switch (op.amount) {
    case Amt::None:
        ladder = {{"-", std::nullopt}};
        break;
    case Amt::Hinge:
        for (const f32 a : {0.0f, 1.0f, 45.0f, 180.0f, -90.0f}) {
            ladder.push_back({AmountLabel("deg", a), a});
        }
        sample = 2;
        break;
    case Amt::Slide:
        for (const f32 a : {-1.0f, 0.0f, 0.5f, 1.0f}) {
            ladder.push_back({AmountLabel("slide", a), a});
        }
        sample = 2;
        break;
    case Amt::Ladder:
    case Amt::Planar: {
        Mesh probe = fx.mesh;
        Mesh made;
        const PointTable points = PointsOf(probe);
        ModelPlan plan = op.plan(probe, points, in, option, 1.0f, &made);
        if (plan.refused()) {
            ladder = {{"-", std::nullopt}};
            break;
        }
        geom::AmountRange(probe, plan);
        const f32 max = op.amount == Amt::Planar ? 1.0f : plan.amountMax < 1e30f ? plan.amountMax : op.nominal;
        const f32 neg = plan.amountMin > -1e30f && plan.amountMin < 0.0f ? 0.5f * plan.amountMin : -0.5f * max;
        ladder = {{"0", 0.0f},
                  {"1e-4", 1e-4f},
                  {AmountLabel("half", 0.5f * max), 0.5f * max},
                  {AmountLabel("max", max), max},
                  {AmountLabel("1.5max", 1.5f * max), 1.5f * max},
                  {AmountLabel("neg", neg), neg}};
        sample = 2;
        break;
    }
    }
    for (u32 k = 0; k < ladder.size(); ++k) {
        const Outcome o = RunOne(op, fx.mesh, in, option, ladder[k].second, 1.0f, &fx);
        std::string failed = o.failed;
        const auto more = [&](const std::string& s) {
            if (!s.empty()) {
                failed += (failed.empty() ? "" : "; ") + s;
            }
        };
        if (checks && k == sample && fx.plain && in.probe) {
            more(P14(op, fx, info, in, option, ladder[k].second, o, static_cast<u32>(sink.rows) + 7));
            more(P15(op, fx, info, in, option, ladder[k].second, o));
            // The default weld threshold follows the mesh's size, and 1000 spans
            // both copies: neither is per part.
            const bool relative = op.name.rfind("Weld", 0) == 0 && (option == 0 || option == 3);
            if (in.perPart && !relative) {
                more(P16(op, fx, info, in, option, ladder[k].second, o));
            }
        }
        sink.row(op.name, fx.name, in.shape, ladder[k].first, optionName, RefusalName(o.verdict), failed);
    }
}

void RunOp(const std::string& name) {
    const Op& op = OpNamed(name);
    const Bench& bench = TheBench();
    const auto start = std::chrono::steady_clock::now();
    const u64 rows = TheSink().rows;
    for (std::size_t i = 0; i < bench.fixtures.size(); ++i) {
        for (const Input& in : op.inputs(*bench.infos[i], bench.shapes[i])) {
            for (u32 k = 0; k < op.options.size(); ++k) {
                RunCase(op, bench.fixtures[i], *bench.infos[i], in, k, true);
            }
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("mesh audit %-18s %7llu rows %7.2f s\n", name.c_str(),
                static_cast<unsigned long long>(TheSink().rows - rows), seconds);
    std::fflush(stdout);
}

/// S17: duplicated and unsorted ids, and an id out of range, on three fixtures.
void RunS17(char group) {
    const Bench& bench = TheBench();
    for (const Op& op : Ops()) {
        if (op.group != group) {
            continue;
        }
        for (std::size_t i = 0; i < bench.fixtures.size(); ++i) {
            const Fixture& fx = bench.fixtures[i];
            if (fx.name != "QuadGrid" && fx.name != "Crack" && fx.name != "Box") {
                continue;
            }
            const std::vector<Input> inputs = op.inputs(*bench.infos[i], bench.shapes[i]);
            if (inputs.empty()) {
                continue;
            }
            std::vector<Input> cases;
            const u32 big = 1000;
            if (!op.kinds.empty()) {
                for (const Sel kind : op.kinds) {
                    const ShapeMap& s = bench.shapes[i][static_cast<u32>(kind)];
                    if (!s.count("S1")) {
                        continue;
                    }
                    std::vector<u32> dup = s.count("S2") ? s.at("S2") : s.at("S1");
                    std::reverse(dup.begin(), dup.end());
                    dup.push_back(dup.back());
                    std::vector<u32> oob = s.at("S1");
                    oob.push_back(big);
                    for (const auto& [label, ids] : {std::pair{"S17dup", dup}, std::pair{"S17oob", oob}}) {
                        Input x = inputs.front();
                        x.shape = std::string(1, "VEBF"[static_cast<u32>(kind)]) + ":" + label;
                        x.set = ElementSet{};
                        (kind == Sel::V ? x.set.vertices : kind == Sel::F ? x.set.faces : x.set.edges) = ids;
                        cases.push_back(std::move(x));
                    }
                }
            } else {
                Input x = inputs.front();
                x.shape = "S17oob:" + x.shape;
                if (!x.ids.empty()) {
                    x.ids[0] = big;
                }
                x.from.edge = x.from.edge != kInvalidId ? big : x.from.edge;
                x.from.vertex = x.from.vertex != kInvalidId ? big : x.from.vertex;
                cases.push_back(std::move(x));
            }
            for (const Input& in : cases) {
                RunCase(op, fx, *bench.infos[i], in, 0, false);
            }
        }
    }
}

} // namespace

// ============================================================================
// The matrix: one case per tool
// ============================================================================

#define MESH_AUDIT_OP(name) \
    TEST_CASE("mesh audit matrix: " name, "[.mesh-audit]") { RunOp(name); }

MESH_AUDIT_OP("Collapse")
MESH_AUDIT_OP("Weld")
MESH_AUDIT_OP("WeldOnto")
MESH_AUDIT_OP("TargetWeld")
MESH_AUDIT_OP("TargetWeldEdges")
MESH_AUDIT_OP("Remove")
MESH_AUDIT_OP("Delete")
MESH_AUDIT_OP("Cap")
MESH_AUDIT_OP("Flip")
MESH_AUDIT_OP("Sharpen")
MESH_AUDIT_OP("ToQuads")
MESH_AUDIT_OP("ToPolygons")
MESH_AUDIT_OP("Retriangulate")
MESH_AUDIT_OP("Triangulate")
MESH_AUDIT_OP("Turn")
MESH_AUDIT_OP("EditTriangulation")
MESH_AUDIT_OP("TurnEdge")
MESH_AUDIT_OP("Extrude")
MESH_AUDIT_OP("ExtrudeBorder")
MESH_AUDIT_OP("Inset")
MESH_AUDIT_OP("HingeFaces")
MESH_AUDIT_OP("HingeBorder")
MESH_AUDIT_OP("ChamferEdges")
MESH_AUDIT_OP("ChamferVertices")
MESH_AUDIT_OP("InsertVertex")
MESH_AUDIT_OP("Divide")
MESH_AUDIT_OP("ConnectVertices")
MESH_AUDIT_OP("ConnectEdges")
MESH_AUDIT_OP("Cut")
MESH_AUDIT_OP("Slice")
MESH_AUDIT_OP("Bridge")
MESH_AUDIT_OP("BridgeFaces")
MESH_AUDIT_OP("CreatePolygon")
MESH_AUDIT_OP("Symmetrize")
MESH_AUDIT_OP("Mirror")
MESH_AUDIT_OP("MakePlanarAxis")
MESH_AUDIT_OP("MakePlanarPlane")
MESH_AUDIT_OP("MakePlanarParts")
MESH_AUDIT_OP("Duplicate")
MESH_AUDIT_OP("Detach")
MESH_AUDIT_OP("DetachToElement")
MESH_AUDIT_OP("Subdivide")
MESH_AUDIT_OP("Smooth")
MESH_AUDIT_OP("OrientOutward")

TEST_CASE("mesh audit matrix S17: removal", "[.mesh-audit]") {
    RunS17('A');
}
TEST_CASE("mesh audit matrix S17: adding", "[.mesh-audit]") {
    RunS17('B');
}
TEST_CASE("mesh audit matrix S17: placement", "[.mesh-audit]") {
    RunS17('C');
}

TEST_CASE("mesh audit matrix P17: inverse and idempotent", "[.mesh-audit]") {
    // Flip twice and Mirror twice give the original polygons, winding and all;
    // Triangulate on triangles and a second Weld change nothing; Hard then Soft
    // leaves no sharp edge on a fixture that had none.
    Sink& sink = TheSink();
    const Bench& bench = TheBench();
    const auto twice = [&](const Mesh& mesh, const std::function<ModelPlan(Mesh&, const PointTable&)>& first,
                           const std::function<ModelPlan(Mesh&, const ModelPlan&)>& second, std::string& outcome) {
        Mesh work = mesh;
        ModelPlan a = first(work, PointsOf(work));
        if (a.refused()) {
            outcome = geom::ToString(a.refusal);
            return work;
        }
        geom::FinishTool(work, a);
        ModelPlan b = second(work, a);
        if (b.refused()) {
            outcome = std::string("2nd:") + geom::ToString(b.refusal);
            return work;
        }
        geom::FinishTool(work, b);
        outcome = "ok";
        return work;
    };
    for (std::size_t i = 0; i < bench.fixtures.size(); ++i) {
        const Fixture& fx = bench.fixtures[i];
        const Info& info = *bench.infos[i];
        if (!fx.plain) {
            continue;
        }
        const Polys before = PolysOf(fx.mesh);
        const f32 tol = 1e-4f * info.diag;
        const auto report = [&](const std::string& op, const std::string& shape, const std::string& outcome,
                                const Mesh& after, bool same, const std::string& extra = {}) {
            std::string why;
            if (outcome == "ok" && same) {
                SamePolys(before, PolysOf(after), tol, why);
            }
            why = why.empty() ? extra : "P17: " + why + (extra.empty() ? "" : "; " + extra);
            sink.row(op, fx.name, shape, "-", "", outcome, why);
        };
        for (const auto& [name, ids] : bench.shapes[i][3]) {
            std::string outcome;
            const Mesh after = twice(
                fx.mesh, [&](Mesh& m, const PointTable&) { return geom::PlanFlip(m, Faces(ids)); },
                [&](Mesh& m, const ModelPlan& a) { return geom::PlanFlip(m, a.selection); }, outcome);
            report("P17 Flip x2", "F:" + name, outcome, after, true);
        }
        for (const Input& in : WholeInputs(info, false)) {
            std::string outcome;
            geom::MirrorParams params;
            params.origin = in.origin;
            params.normal = in.normal;
            const Mesh after = twice(
                fx.mesh, [&](Mesh& m, const PointTable&) { return geom::PlanMirror(m, params); },
                [&](Mesh& m, const ModelPlan&) { return geom::PlanMirror(m, params); }, outcome);
            report("P17 Mirror x2", in.shape, outcome, after, true);
        }
        {
            Mesh work = fx.mesh;
            ModelPlan plan = geom::PlanTriangulate(work, AllFaces(work));
            const bool allTriangles = std::all_of(info.faces.begin(), info.faces.end(),
                                                  [&](u32 f) { return info.fverts[f].size() == 3; });
            if (allTriangles) {
                std::string extra;
                if (!plan.refused()) {
                    geom::FinishTool(work, plan);
                    if (LiveVertices(work) != LiveVertices(fx.mesh)) {
                        extra = "P17: vertices " + std::to_string(LiveVertices(work));
                    }
                }
                report("P17 Triangulate tris", "F:S7", RefusalName(Verdict{plan.refused(), plan.refusal}), work, true,
                       extra);
            }
        }
        for (const f32 threshold : {geom::DefaultWeldThreshold(fx.mesh), 0.1f, 1.5f}) {
            Mesh once = fx.mesh;
            ModelPlan a = geom::PlanWeld(once, PointsOf(once), AllVertices(once), threshold);
            if (a.refused()) {
                sink.row("P17 Weld x2", fx.name, "V:S7", std::to_string(threshold), "", geom::ToString(a.refusal), "");
                continue;
            }
            geom::FinishTool(once, a);
            Mesh again = once;
            ModelPlan b = geom::PlanWeld(again, PointsOf(again), AllVertices(again), threshold);
            std::string why;
            if (!b.refused()) {
                geom::FinishTool(again, b);
                if (LiveVertices(again) != LiveVertices(once)) {
                    why = "P17: second weld left " + std::to_string(LiveVertices(again)) + " vertices, not " +
                          std::to_string(LiveVertices(once));
                } else if (!SamePolys(PolysOf(once), PolysOf(again), tol, why)) {
                    why = "P17: second weld " + why;
                } else {
                    why.clear();
                }
            }
            sink.row("P17 Weld x2", fx.name, "V:S7", std::to_string(threshold), "",
                     b.refused() ? std::string("2nd:") + geom::ToString(b.refusal) : "ok", why);
        }
        if (SharpCount(fx.mesh) == 0) {
            for (const auto& [name, ids] : bench.shapes[i][1]) {
                std::string outcome;
                const Mesh after = twice(
                    fx.mesh, [&](Mesh& m, const PointTable&) { return geom::PlanSharpen(m, Edges(ids), true); },
                    [&](Mesh& m, const ModelPlan&) { return geom::PlanSharpen(m, Edges(ids), false); }, outcome);
                report("P17 Hard+Soft", "E:" + name, outcome, after, true,
                       SharpCount(after) != 0 ? "P17: " + std::to_string(SharpCount(after)) + " sharp edges left" : "");
            }
        }
    }
}

// ============================================================================
// What the matrix found (§2.8): the minimal case of each new cluster
// ============================================================================

namespace {

/// One fixture with its digest and shapes, for a minimal case. Built in place
/// and never moved: `info` reads `fx.mesh`.
struct Probe {
    Fixture fx;
    std::unique_ptr<Info> info;
    std::array<ShapeMap, 4> shapes;

    Probe(const char* name, Mesh mesh) {
        fx.name = name;
        fx.mesh = std::move(mesh);
        geom::MaterialiseRows(fx.mesh);
        info = std::make_unique<Info>(fx.mesh);
        shapes[3] = FaceShapes(*info);
        shapes[1] = EdgeShapes(*info, shapes[3]);
        shapes[0] = VertexShapes(*info, shapes[3], shapes[1]);
        shapes[2] = BorderShapes(*info);
    }
    std::vector<Input> inputs(const std::string& op, const std::string& shape) const {
        std::vector<Input> out;
        for (const Input& in : OpNamed(op).inputs(*info, shapes)) {
            if (in.shape == shape) {
                out.push_back(in);
            }
        }
        return out;
    }
    /// P15 for every input of that shape: "" when every transform agrees.
    std::string invariance(const std::string& op, const std::string& shape, u32 option,
                           std::optional<f32> amount) const {
        std::string out;
        for (const Input& in : inputs(op, shape)) {
            const Outcome base = RunOne(OpNamed(op), fx.mesh, in, option, amount);
            out += P15(OpNamed(op), fx, *info, in, option, amount, base);
        }
        return out;
    }
};

Mesh Rowed(Mesh mesh) {
    geom::MaterialiseRows(mesh);
    return mesh;
}

} // namespace

TEST_CASE("audit MTX-1: a refused Cut leaves the mesh as it was", "[wem][audit][cut]") {
    // One quad, cut from a point inside it to the middle of its own bottom edge:
    // refused NothingToSlice, so nothing may have changed (P10).
    const Mesh mesh = Rowed(QuadGrid(1, 1));
    CutPoint from;
    from.face = 0;
    from.at = V3(0.5f, 0.5f, 0.0f);
    const CutPoint to = OnEdge(EdgeAt(mesh, 0, 0, 1, 0), 0.5f);
    Mesh out;
    const Verdict v = Run(
        mesh, [&](Mesh& m, const PointTable& p) { return geom::PlanCut(m, p, from, to, V3(1, 0, 0)); }, {}, &out);
    INFO(geom::ToString(v.refusal) << " | " << v.failed << " | vertices " << LiveVertices(out) << " faces "
                                   << LiveFaces(out));
    REQUIRE(v.refused);
    CHECK(v.failed == "");
}

TEST_CASE("audit MTX-2: a refused Bridge of two faces leaves the mesh as it was", "[wem][audit][bridge]") {
    // Two corner faces of a flat 3 x 3 grid's bottom row: the band would fold, so WouldFold;
    // the refusal must leave the mesh byte-equal (P10).
    const Mesh mesh = Rowed(QuadGrid(3, 3));
    Mesh out;
    const Verdict v = Run(
        mesh, [&](Mesh& m, const PointTable& p) { return geom::PlanBridgeFaces(m, p, FaceId(0), FaceId(2)); }, {},
        &out);
    INFO(geom::ToString(v.refusal) << " | " << v.failed << " | faces " << LiveFaces(out));
    REQUIRE(v.refused);
    CHECK(v.failed == "");
}

TEST_CASE("audit MTX-3: a refused Bridge of two loops leaves the mesh as it was", "[wem][audit][bridge]") {
    // A double-sided card's two coincident borders: WouldFold, and nothing changed.
    const Mesh mesh = Rowed(DoubleCard());
    Mesh out;
    const Verdict v = Run(
        mesh,
        [&](Mesh& m, const PointTable& p) {
            return geom::PlanBridge(m, p, EdgeId(EdgeOf(m, 0, 1)), EdgeId(EdgeOf(m, 4, 5)));
        },
        {}, &out);
    INFO(geom::ToString(v.refusal) << " | " << v.failed << " | faces " << LiveFaces(out));
    REQUIRE(v.refused);
    CHECK(v.failed == "");
}

TEST_CASE("audit MTX-4: a refused Create Polygon leaves the mesh as it was", "[wem][audit][polygon]") {
    // Every run of three border vertices of an open fan, in border order: those
    // refused must leave the mesh byte-equal.
    const Mesh mesh = Rowed(OpenFan());
    const Info info(mesh);
    u32 refusals = 0;
    for (const auto& [edges, verts] : BorderLoops(info)) {
        for (std::size_t k = 0; k < verts.size(); ++k) {
            const std::vector<u32> run = {verts[k], verts[(k + 1) % verts.size()], verts[(k + 2) % verts.size()]};
            const Verdict v =
                Run(mesh, [&](Mesh& m, const PointTable& p) { return geom::PlanCreatePolygon(m, p, run); });
            INFO(run[0] << " " << run[1] << " " << run[2] << ": " << geom::ToString(v.refusal) << " | " << v.failed);
            refusals += v.refused ? 1u : 0u;
            CHECK((!v.refused || v.failed == ""));
        }
    }
    CHECK(refusals > 0u);
}

TEST_CASE("audit MTX-5: a refused Detach to Element leaves the mesh as it was", "[wem][audit][detach]") {
    // Two faces of a 3 x 3 grid touching only at (1, 1): a bowtie copy, so
    // WouldFold (as Duplicate does, DUP-2); the refusal must change nothing.
    const Mesh mesh = Rowed(QuadGrid(3, 3));
    Mesh out;
    const Verdict v = Run(
        mesh, [&](Mesh& m, const PointTable&) { return geom::PlanDetachToElement(m, Faces({0, 4})); }, {}, &out);
    INFO(geom::ToString(v.refusal) << " | " << v.failed << " | vertices " << LiveVertices(out));
    REQUIRE(v.refused);
    CHECK(v.failed == "");
}

TEST_CASE("audit MTX-6: Slice through a concave face's reflex corner cuts it in two", "[wem][audit][slice]") {
    // The L hexagon, sliced by x = 1: the plane enters at (1, 0) and leaves
    // through the reflex corner (1, 1), then runs along the edge to (1, 2). Max
    // cuts the L into a square and a rectangle.
    const Mesh mesh = Rowed(LShape());
    geom::SliceParams params;
    params.origin = V3(1, 1, 0);
    params.normal = V3(1, 0, 0);
    Mesh out;
    const Verdict v = Run(
        mesh, [&](Mesh& m, const PointTable& p) { return geom::PlanSlice(m, p, Faces({0}), params); }, {}, &out);
    INFO(geom::ToString(v.refusal) << " | " << v.failed << " | faces " << LiveFaces(out));
    REQUIRE_FALSE(v.refused);
    CHECK(v.failed == "");
    CHECK(LiveFaces(out) == 2u);
}

TEST_CASE("audit MTX-7: Orient Outward leaves a flat open sheet as it is, however it is turned",
          "[wem][audit][normals]") {
    // A flat sheet encloses no volume, so neither winding is "outward": it must
    // not be reversed, and turning it must not change that (P15).
    for (const Xf& xf : {Xf{"none"}, Transforms()[3]}) {
        Mesh mesh = Transformed(Rowed(TriGrid(3, 3)), xf);
        const ModelPlan plan = geom::PlanOrientOutward(mesh, ElementSet{});
        INFO(xf.name << ": " << geom::ToString(plan.refusal) << " changed " << plan.changed);
        CHECK(plan.changed == 0u);
    }
}

TEST_CASE("audit MTX-8: Connect on one vertex says no face holds two, not that nothing is selected",
          "[wem][audit][connect]") {
    // The kernel's own contract: NotOneFace when no face holds two selected
    // vertices. One selected vertex reads EmptySelection ("select something").
    Mesh mesh = Rowed(QuadGrid(3, 3));
    const ModelPlan plan = geom::PlanConnectVertices(mesh, PointsOf(mesh), Vertices({5}));
    CHECK(plan.refusal == ModelRefusal::NotOneFace);
}

TEST_CASE("audit MTX-9: Cut between two corners in line with a third makes no zero-area face",
          "[wem][audit][cut]") {
    // The mixed grid's pentagon has (4, 2), (3.5, 2) and (3, 2) in line. A cut
    // from (4, 2) to (3, 2) splits off the triangle the three make, which has no
    // area: it should be refused, as Connect should (CON-3).
    const Mesh mesh = Rowed(MixedGrid());
    const CutPoint from = OnVertex(VertexAt(mesh, 4, 2)), to = OnVertex(VertexAt(mesh, 3, 2));
    Mesh out;
    const Verdict v = Run(
        mesh, [&](Mesh& m, const PointTable& p) { return geom::PlanCut(m, p, from, to, V3(0, 1, 0)); }, {}, &out);
    INFO(geom::ToString(v.refusal) << " | " << v.failed << " | faces " << LiveFaces(out));
    CHECK(v.failed == "");
}

TEST_CASE("audit MTX-10: Bridge refuses loops or faces that touch at a point", "[wem][audit][bridge]") {
    // Two quads touching only at (1, 1), each its own shell. Bridging them joins
    // the coincident corners with a zero-length rung; the kernel refuses faces
    // sharing a VERTEX (SameLoop), but not a point.
    const Mesh mesh = Rowed(TouchingQuads());
    const Verdict loops = Run(mesh, [&](Mesh& m, const PointTable& p) {
        return geom::PlanBridge(m, p, EdgeId(EdgeOf(m, 0, 1)), EdgeId(EdgeOf(m, 4, 5)));
    });
    INFO("loops: " << geom::ToString(loops.refusal) << " | " << loops.failed);
    CHECK(loops.failed == "");
    const Verdict faces =
        Run(mesh, [&](Mesh& m, const PointTable& p) { return geom::PlanBridgeFaces(m, p, FaceId(0), FaceId(1)); });
    INFO("faces: " << geom::ToString(faces.refusal) << " | " << faces.failed);
    CHECK(faces.failed == "");
}

TEST_CASE("audit MTX-11: Extrude Border on a concave face is the same however the face is turned",
          "[wem][audit][extrude]") {
    // The L hexagon's border edge beside the reflex corner, extruded 0.25: a
    // 37 degree turn about (1, 2, 3) of the input must turn the output alike.
    const Probe probe("LShape", LShape());
    CHECK(probe.invariance("ExtrudeBorder", "B:S1", 0, 0.25f) == "");
}

TEST_CASE("audit MTX-12: Hinge gives no zero-area wall to a selection edge on the hinge line",
          "[wem][audit][hinge]") {
    // 3 x 3 grid, faces (1, 0) and (1, 1), hinged on (1, 0)-(1, 1). The edge
    // (1, 1)-(1, 2) lies on the hinge line: its far vertex is copied, but a turn
    // about the line cannot move it, so its wall is a zero-area triangle at
    // every angle. Pinning it like the hinge's own vertices keeps the wall out.
    const Mesh mesh = Rowed(QuadGrid(3, 3));
    const u32 hinge = EdgeAt(mesh, 1, 0, 1, 1);
    const Verdict v = Run(
        mesh, [&](Mesh& m, const PointTable& p) { return geom::PlanHingeFaces(m, p, Faces({1, 4}), hinge); }, {},
        nullptr, geom::kDefaultShadingAngle, 45.0f);
    INFO(geom::ToString(v.refusal) << " | " << v.failed);
    REQUIRE_FALSE(v.refused);
    CHECK(v.failed == "");
}

TEST_CASE("audit MTX-12 (Border): Hinge Border gives no wall to a border edge on the hinge line",
          "[wem][audit][hinge]") {
    // 3 x 3 grid, the border run (2, 0)-(0, 0)-(0, 1) hinged at (0, 0) about +X:
    // the bottom edges lie on the hinge line, so only the side edge turns.
    const Mesh mesh = Rowed(QuadGrid(3, 3));
    const ElementSet run = Edges({EdgeAt(mesh, 1, 0, 2, 0), EdgeAt(mesh, 0, 0, 1, 0), EdgeAt(mesh, 0, 0, 0, 1)});
    const u32 corner = VertexAt(mesh, 0, 0);
    const Verdict v = Run(
        mesh,
        [&](Mesh& m, const PointTable& p) {
            return geom::PlanHingeBorder(m, p, run, corner, Vector3f{1.0f, 0.0f, 0.0f});
        },
        {}, nullptr, geom::kDefaultShadingAngle, 45.0f);
    INFO(geom::ToString(v.refusal) << " | " << v.failed);
    REQUIRE_FALSE(v.refused);
    CHECK(v.failed == "");
    // Every edge on the line: nothing turns.
    const ElementSet along = Edges({EdgeAt(mesh, 1, 0, 2, 0), EdgeAt(mesh, 0, 0, 1, 0)});
    Mesh work = mesh;
    CHECK(geom::PlanHingeBorder(work, PointsOf(work), along, corner, Vector3f{1.0f, 0.0f, 0.0f}).refusal ==
          ModelRefusal::ZeroAmount);
}

TEST_CASE("audit MTX-13: Connect with two segments on every edge of a fan-capped cylinder is scale-invariant",
          "[wem][audit][connect]") {
    const Probe probe("CylFan", Cylinder(8, 2, Caps::Fan));
    CHECK(probe.invariance("ConnectEdges", "E:S7", 1, 0.5f) == "");
}

TEST_CASE("audit MTX-14: Cut across a fan cap's corner is the same at any scale or place", "[wem][audit][cut]") {
    const Probe probe("CylFan", Cylinder(8, 2, Caps::Fan));
    CHECK(probe.invariance("Cut", "corner", 0, std::nullopt) == "");
}

TEST_CASE("audit MTX-15: Divide in three on triangles leaves every stored cut valid", "[wem][audit][divide]") {
    // A 3 x 3 triangle grid, every edge cut in three: each triangle becomes a
    // 9-gon whose corners run in three straight lines, which a fan from a
    // middle point cuts validly. A row is left stale (P2) with no face of zero
    // area; Divide in two passes, and so does a 1 x 1 grid.
    const Mesh mesh = Rowed(TriGrid(3, 3));
    const Verdict v =
        Run(mesh, [&](Mesh& m, const PointTable&) { return geom::PlanDivide(m, AllEdges(m), 3); });
    INFO(geom::ToString(v.refusal) << " | " << v.failed);
    REQUIRE_FALSE(v.refused);
    CHECK(v.failed == "");
}

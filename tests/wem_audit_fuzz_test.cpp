// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The Mesh audit's three sweeps that need no hand-written case
/// (EDIT_MODE_MESH_AUDIT_PLAN.md §2.6.2-§2.6.4 in WhiteoutFlakes): the crack
/// census on real models, the chain fuzzer on the fixtures, and every operation
/// on every real mesh. All three are hidden (`[.mesh-audit]`); the chains the
/// fuzzer shrank are kept below as `audit FUZ-n`.
///
/// Environment: `WDX_MESH_AUDIT_OUT` (the CSVs' folder), `WDX_MESH_AUDIT_MODELS`
/// (a folder of extra `.mdx` files: the HD footman and Arthas as pulled from
/// CASC), `WEM_MDX_CORPUS_DIR` and `WEM_CORPUS_LIMIT` (the corpus files),
/// `WDX_FUZZ_CHAINS` (how many chains), `WDX_FUZZ_TRACE` (name each step on
/// stderr before running it, for a crash).

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/refine.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/writer.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <tuple>

#include "wem_audit_fixtures.h"
#include "wem_corpus_files.h"

using namespace audit;

namespace {

namespace fs = std::filesystem;

// ============================================================================
// Output
// ============================================================================

fs::path OutDir() {
    const char* env = std::getenv("WDX_MESH_AUDIT_OUT");
    const fs::path dir = env != nullptr && *env != '\0' ? fs::path(env) : fs::temp_directory_path() / "mesh-audit";
    std::error_code error;
    fs::create_directories(dir, error);
    return dir;
}

std::string Cell(const std::string& text) {
    if (text.find_first_of(",\"\n") == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') {
            out += '"';
        }
        out += c == '\n' ? ' ' : c;
    }
    return out + "\"";
}

bool Tracing() {
    static const bool on = std::getenv("WDX_FUZZ_TRACE") != nullptr;
    return on;
}

// ============================================================================
// Geometry
// ============================================================================

Vector3f Add(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x - b.x, a.y - b.y, a.z - b.z};
}
Vector3f Mul(const Vector3f& a, f32 s) {
    return Vector3f{a.x * s, a.y * s, a.z * s};
}
Vector3f Unit(const Vector3f& a) {
    const f32 l = Length(a);
    return l > 1e-20f ? Mul(a, 1.0f / l) : Vector3f{0.0f, 0.0f, 0.0f};
}

void Bounds(const Mesh& mesh, Vector3f& lo, Vector3f& hi) {
    lo = Vector3f{1e30f, 1e30f, 1e30f};
    hi = Vector3f{-1e30f, -1e30f, -1e30f};
    for (const u32 v : AllVertices(mesh).vertices) {
        const Vector3f& p = Positions(mesh)[v];
        lo = Vector3f{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = Vector3f{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
}

f32 Diagonal(const Mesh& mesh) {
    Vector3f lo, hi;
    Bounds(mesh, lo, hi);
    return std::max(Length(Sub(hi, lo)), 1e-6f);
}

Vector3f Centre(const Mesh& mesh) {
    Vector3f lo, hi;
    Bounds(mesh, lo, hi);
    return Mul(Add(lo, hi), 0.5f);
}

Vector3f FaceNormal(const Mesh& mesh, u32 f) {
    const Topology& t = mesh.topology();
    const auto p = Positions(mesh);
    Vector3f sum{0, 0, 0};
    for (const HalfedgeId h : t.fh(FaceId(f))) {
        sum = Add(sum, Cross(p[t.from(h).value()], p[t.to(h).value()]));
    }
    return Unit(sum);
}

Vector3f FaceCentre(const Mesh& mesh, u32 f) {
    const Topology& t = mesh.topology();
    const auto p = Positions(mesh);
    Vector3f sum{0, 0, 0};
    u32 n = 0;
    for (const HalfedgeId h : t.fh(FaceId(f))) {
        sum = Add(sum, p[t.from(h).value()]);
        ++n;
    }
    return Mul(sum, 1.0f / static_cast<f32>(std::max(n, 1u)));
}

/// The mesh as any finish leaves it: rows stored, canonical.
Mesh Finished(Mesh mesh) {
    ModelPlan none;
    geom::FinishTool(mesh, none);
    return mesh;
}

/// Zero-area faces plus zero-length edges, as `CheckDegenerate` counts them.
u32 DegenerateCount(const Mesh& mesh, f32 eps = 1e-6f) {
    const std::string why = CheckDegenerate(mesh, eps);
    if (why.empty()) {
        return 0;
    }
    u32 faces = 0, edges = 0;
    std::sscanf(why.c_str(), "%u zero-area faces, %u zero-length edges", &faces, &edges);
    return faces + edges;
}

// ============================================================================
// Selections (§2.4)
// ============================================================================

enum class Kind : u8 { V, E, F, B };
const char* KindName(Kind kind) {
    return kind == Kind::V ? "V" : kind == Kind::E ? "E" : kind == Kind::F ? "P" : "B";
}

enum Shape : u8 { kOne, kFew, kPatch, kRing, kLoop, kAll, kPrev, kShapeCount };
const char* const kShapeNames[] = {"one", "few", "patch", "ring", "loop", "all", "prev"};

using Rng = std::mt19937;

u32 Pick(Rng& rng, std::size_t n) {
    return n == 0 ? 0u : std::uniform_int_distribution<u32>(0, static_cast<u32>(n - 1))(rng);
}
f32 Uniform(Rng& rng, f32 lo, f32 hi) {
    return std::uniform_real_distribution<f32>(lo, hi)(rng);
}

/// Live vertices that lie on a face.
std::vector<u32> FacedVertices(const Mesh& mesh) {
    const Topology& t = mesh.topology();
    std::vector<u8> on(t.vertexCount(), 0);
    for (const u32 f : AllFaces(mesh).faces) {
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            on[t.from(h).value()] = 1;
        }
    }
    std::vector<u32> out;
    for (u32 v = 0; v < on.size(); ++v) {
        if (on[v]) {
            out.push_back(v);
        }
    }
    return out;
}

std::vector<u32> Live(const Mesh& mesh, Kind kind) {
    switch (kind) {
    case Kind::V:
        return FacedVertices(mesh);
    case Kind::E:
        return AllEdges(mesh).edges;
    case Kind::F:
        return AllFaces(mesh).faces;
    case Kind::B:
        return BorderEdges(mesh).edges;
    }
    return {};
}

/// Faces reached across edges from @p start, breadth first in shuffled order.
std::vector<u32> PatchFrom(const Mesh& mesh, Rng& rng, u32 start, u32 want) {
    const Topology& t = mesh.topology();
    std::vector<u32> out;
    std::set<u32> seen{start};
    std::vector<u32> queue{start};
    for (std::size_t at = 0; at < queue.size() && out.size() < want; ++at) {
        const u32 f = queue[at];
        out.push_back(f);
        std::vector<u32> next;
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            const FaceId g = t.face(Topology::opposite(h));
            if (g.valid() && seen.insert(g.value()).second) {
                next.push_back(g.value());
            }
        }
        std::shuffle(next.begin(), next.end(), rng);
        queue.insert(queue.end(), next.begin(), next.end());
    }
    return out;
}

/// The edge ring through @p edge across quads, and the faces it crosses.
void EdgeRing(const Mesh& mesh, u32 edge, u32 want, std::vector<u32>& edges, std::vector<u32>& faces) {
    const Topology& t = mesh.topology();
    edges = {edge};
    faces.clear();
    for (u32 side = 0; side < 2; ++side) {
        HalfedgeId h = Topology::halfedge(EdgeId(edge), side);
        while (edges.size() < want && !t.isBoundary(h) && t.valence(t.face(h)) == 4) {
            const u32 f = t.face(h).value();
            if (std::find(faces.begin(), faces.end(), f) != faces.end()) {
                break;
            }
            faces.push_back(f);
            const HalfedgeId across = t.next(t.next(h));
            const u32 e = Topology::edge(across).value();
            if (std::find(edges.begin(), edges.end(), e) != edges.end()) {
                break;
            }
            edges.push_back(e);
            h = Topology::opposite(across);
        }
    }
}

/// The edge loop through @p edge, straight across valence-4 vertices.
std::vector<u32> EdgeLoop(const Mesh& mesh, u32 edge, u32 want) {
    const Topology& t = mesh.topology();
    std::vector<u32> out{edge};
    for (u32 side = 0; side < 2; ++side) {
        HalfedgeId h = Topology::halfedge(EdgeId(edge), side);
        for (u32 guard = 0; out.size() < want && guard < 4096; ++guard) {
            const VertexId v = t.to(h);
            if (t.isBoundary(v) || t.valence(v) != 4 || t.isBoundary(h)) {
                break;
            }
            const HalfedgeId straight = t.next(Topology::opposite(t.next(h)));
            const u32 e = Topology::edge(straight).value();
            if (std::find(out.begin(), out.end(), e) != out.end()) {
                break;
            }
            out.push_back(e);
            h = straight;
        }
    }
    return out;
}

/// The border loop @p edge lies on, as its faceless halfedges in order.
std::vector<HalfedgeId> BorderLoop(const Mesh& mesh, u32 edge) {
    const Topology& t = mesh.topology();
    HalfedgeId h = Topology::halfedge(EdgeId(edge), 0);
    if (!t.isBoundary(h)) {
        h = Topology::opposite(h);
    }
    std::vector<HalfedgeId> out;
    if (!t.isBoundary(h)) {
        return out;
    }
    const HalfedgeId start = h;
    do {
        out.push_back(h);
        h = t.next(h);
    } while (h != start && out.size() < t.halfedgeCount());
    return out;
}

/// @p selection's elements of @p kind, converting from the levels it holds.
std::vector<u32> Convert(const Mesh& mesh, const ElementSet& selection, Kind kind) {
    const Topology& t = mesh.topology();
    std::set<u32> out;
    const auto liveV = [&](u32 v) { return v < t.vertexCount() && !t.isDeleted(VertexId(v)); };
    const auto liveE = [&](u32 e) { return e < t.edgeCount() && !t.isDeleted(EdgeId(e)); };
    const auto liveF = [&](u32 f) { return f < t.faceCount() && !t.isDeleted(FaceId(f)); };
    if (kind == Kind::V) {
        for (const u32 v : selection.vertices) {
            if (liveV(v)) out.insert(v);
        }
        for (const u32 e : selection.edges) {
            if (liveE(e)) {
                out.insert(t.from(Topology::halfedge(EdgeId(e), 0)).value());
                out.insert(t.to(Topology::halfedge(EdgeId(e), 0)).value());
            }
        }
        for (const u32 f : selection.faces) {
            if (liveF(f)) {
                for (const HalfedgeId h : t.fh(FaceId(f))) out.insert(t.from(h).value());
            }
        }
    } else if (kind == Kind::E || kind == Kind::B) {
        for (const u32 e : selection.edges) {
            if (liveE(e)) out.insert(e);
        }
        for (const u32 f : selection.faces) {
            if (liveF(f)) {
                for (const HalfedgeId h : t.fh(FaceId(f))) out.insert(Topology::edge(h).value());
            }
        }
        if (kind == Kind::B) {
            std::set<u32> border;
            for (const u32 e : out) {
                if (t.isBoundary(EdgeId(e))) border.insert(e);
            }
            out = border;
        }
    } else {
        for (const u32 f : selection.faces) {
            if (liveF(f)) out.insert(f);
        }
        if (out.empty()) {
            for (const u32 v : selection.vertices) {
                if (!liveV(v)) continue;
                for (const HalfedgeId h : t.voh(VertexId(v))) {
                    if (t.face(h).valid()) out.insert(t.face(h).value());
                }
            }
        }
    }
    return std::vector<u32>(out.begin(), out.end());
}

ElementSet Select(const Mesh& mesh, Kind kind, Shape shape, Rng& rng, const ElementSet& prev) {
    const Topology& t = mesh.topology();
    ElementSet out;
    std::vector<u32>& ids = kind == Kind::V ? out.vertices : kind == Kind::F ? out.faces : out.edges;
    const std::vector<u32> live = Live(mesh, kind);
    if (live.empty()) {
        return out;
    }
    const auto verticesOfEdges = [&](const std::vector<u32>& edges) {
        std::vector<u32> vs;
        for (const u32 e : edges) {
            vs.push_back(t.from(Topology::halfedge(EdgeId(e), 0)).value());
            vs.push_back(t.to(Topology::halfedge(EdgeId(e), 0)).value());
        }
        return vs;
    };
    switch (shape) {
    case kOne:
        ids = {live[Pick(rng, live.size())]};
        break;
    case kFew:
        for (u32 n = 2 + Pick(rng, 3); n > 0; --n) {
            ids.push_back(live[Pick(rng, live.size())]);
        }
        break;
    case kPatch:
        if (kind == Kind::B) {
            const std::vector<HalfedgeId> loop = BorderLoop(mesh, live[Pick(rng, live.size())]);
            const u32 start = Pick(rng, loop.size());
            const u32 length = std::min<u32>(static_cast<u32>(loop.size()), 2 + Pick(rng, 4));
            for (u32 k = 0; k < length; ++k) {
                ids.push_back(Topology::edge(loop[(start + k) % loop.size()]).value());
            }
        } else {
            const std::vector<u32> faces = AllFaces(mesh).faces;
            const std::vector<u32> patch = PatchFrom(mesh, rng, faces[Pick(rng, faces.size())], 2 + Pick(rng, 7));
            ElementSet asFaces;
            asFaces.faces = patch;
            ids = Convert(mesh, asFaces, kind);
        }
        break;
    case kRing:
        if (kind == Kind::B) {
            ids = {live[Pick(rng, live.size())], live[Pick(rng, live.size())]};
        } else {
            const std::vector<u32> edges = AllEdges(mesh).edges;
            std::vector<u32> ring, faces;
            EdgeRing(mesh, edges[Pick(rng, edges.size())], 3 + Pick(rng, 6), ring, faces);
            ids = kind == Kind::E ? ring : kind == Kind::F ? faces : verticesOfEdges(ring);
        }
        break;
    case kLoop:
        if (kind == Kind::B) {
            for (const HalfedgeId h : BorderLoop(mesh, live[Pick(rng, live.size())])) {
                ids.push_back(Topology::edge(h).value());
            }
        } else {
            const std::vector<u32> edges = AllEdges(mesh).edges;
            const u32 edge = edges[Pick(rng, edges.size())];
            if (kind == Kind::F) {
                std::vector<u32> ring;
                EdgeRing(mesh, edge, 64, ring, ids);
            } else {
                const std::vector<u32> loop = EdgeLoop(mesh, edge, 3 + Pick(rng, 12));
                ids = kind == Kind::E ? loop : verticesOfEdges(loop);
            }
        }
        break;
    case kAll:
        ids = live;
        break;
    case kPrev:
    default:
        ids = Convert(mesh, prev, kind);
        if (ids.empty()) {
            ids = {live[Pick(rng, live.size())]};
        }
        break;
    }
    out.normalise();
    return out;
}

// ============================================================================
// The operations (§1.1, §1.2)
// ============================================================================

struct Ctx {
    Rng rng;
    Shape shape = kOne;
    const ElementSet* prev = nullptr;
    f32 diag = 1.0f;
    bool skipped = false; ///< The op had nothing to act on: not a kernel refusal.
    std::string note;
};

ElementSet Sel(const Mesh& mesh, Ctx& c, std::initializer_list<Kind> kinds) {
    const Kind kind = *(kinds.begin() + Pick(c.rng, kinds.size()));
    const ElementSet empty;
    ElementSet out = Select(mesh, kind, c.shape, c.rng, c.prev != nullptr ? *c.prev : empty);
    const std::size_t n = out.vertices.size() + out.edges.size() + out.faces.size();
    c.note += std::string(KindName(kind)) + "x" + std::to_string(n);
    if (n <= 6) {
        c.note += "{";
        for (const u32 v : out.vertices) c.note += "v" + std::to_string(v) + " ";
        for (const u32 e : out.edges) c.note += "e" + std::to_string(e) + " ";
        for (const u32 f : out.faces) c.note += "f" + std::to_string(f) + " ";
        c.note += "}";
    }
    return out;
}

ModelPlan Skip(Ctx& c) {
    c.skipped = true;
    ModelPlan plan;
    plan.refusal = ModelRefusal::EmptySelection;
    return plan;
}

/// Picks an amount strictly inside the plan's range (never 0, never a root:
/// X3-X5 are confirmed already) and applies it.
void Amount(Mesh& m, ModelPlan& plan, Ctx& c, bool negative) {
    if (plan.refused() || plan.motions.empty()) {
        return;
    }
    geom::AmountRange(m, plan);
    // A finite range can still be absurd (3.7e6 on a unit grid): cap at the mesh's size.
    const f32 hi = std::min(plan.amountMax, c.diag);
    const f32 lo = std::max(plan.amountMin, -c.diag);
    const bool down = negative && (c.rng() & 1u) != 0;
    const f32 amount = (down ? lo : hi) * Uniform(c.rng, 0.1f, 0.9f);
    c.note += " amount " + std::to_string(amount) + " of [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
    geom::ApplyAmount(m, plan, amount);
}

void Fixed(Mesh& m, ModelPlan& plan, Ctx& c, f32 amount) {
    if (plan.refused() || plan.motions.empty()) {
        return;
    }
    geom::AmountRange(m, plan);
    c.note += " amount " + std::to_string(amount);
    geom::ApplyAmount(m, plan, amount);
}

u32 RandomBorderEdge(const Mesh& m, Ctx& c) {
    const std::vector<u32> border = BorderEdges(m).edges;
    return border.empty() ? kInvalidId : border[Pick(c.rng, border.size())];
}

enum : u8 { kWelds = 1, kMoves = 2 };

using OpFn = ModelPlan (*)(Mesh&, const PointTable&, Ctx&);

struct Op {
    const char* name;
    OpFn run;
    u8 flags = 0;
    u32 maxFaces = 4000; ///< The fuzzer skips the op past this many faces.
};

const std::vector<Op>& Ops() {
    static const std::vector<Op> ops = {
        {"Collapse", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             return geom::PlanCollapse(m, pt, Sel(m, c, {Kind::V, Kind::E, Kind::F}));
         }},
        {"Weld", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::V, Kind::B});
             const f32 options[] = {geom::DefaultWeldThreshold(m), 0.05f * c.diag, 0.3f * c.diag};
             const f32 threshold = options[Pick(c.rng, 3)];
             c.note += " threshold " + std::to_string(threshold);
             return geom::PlanWeld(m, pt, Vertices(Convert(m, s, Kind::V)), threshold);
         }, kWelds},
        {"TargetWeld", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const std::vector<u32> vs = FacedVertices(m);
             if (vs.empty()) return Skip(c);
             const u32 source = vs[Pick(c.rng, vs.size())];
             u32 target = vs[Pick(c.rng, vs.size())];
             if (c.rng() & 1u) {
                 std::vector<u32> near;
                 for (const HalfedgeId h : m.topology().voh(VertexId(source))) near.push_back(m.topology().to(h).value());
                 if (!near.empty()) target = near[Pick(c.rng, near.size())];
             }
             c.note += "v" + std::to_string(source) + "->v" + std::to_string(target);
             return geom::PlanTargetWeld(m, pt, VertexId(source), VertexId(target));
         }, kWelds},
        {"TargetWeldEdges", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const u32 a = RandomBorderEdge(m, c), b = RandomBorderEdge(m, c);
             if (a == kInvalidId) return Skip(c);
             c.note += "e" + std::to_string(a) + "->e" + std::to_string(b);
             return geom::PlanTargetWeldEdges(m, pt, EdgeId(a), EdgeId(b));
         }, kWelds},
        {"Remove", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             ElementSet s = Sel(m, c, {Kind::V, Kind::E});
             // REM-1 (confirmed, a crash): no border vertex, no crack twin.
             std::vector<u32> kept;
             for (const u32 v : s.vertices) {
                 if (!m.topology().isBoundary(VertexId(v)) && pt.membersOf(pt.pointOf[v]).size() == 1) kept.push_back(v);
             }
             if (kept.size() != s.vertices.size()) c.note += " (REM-1 filter)";
             s.vertices = kept;
             if (s.vertices.empty() && s.edges.empty()) return Skip(c);
             return geom::PlanRemove(m, pt, s);
         }},
        {"Delete", +[](Mesh& m, const PointTable&, Ctx& c) {
             return geom::PlanDelete(m, Sel(m, c, {Kind::V, Kind::E, Kind::B, Kind::F}));
         }},
        {"Cap", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const u32 e = RandomBorderEdge(m, c);
             if (e == kInvalidId) return Skip(c);
             c.note += "e" + std::to_string(e);
             return geom::PlanCap(m, pt, EdgeId(e));
         }},
        {"Flip", +[](Mesh& m, const PointTable&, Ctx& c) { return geom::PlanFlip(m, Sel(m, c, {Kind::F})); }},
        {"ToQuads", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             return geom::PlanJoinTriangles(m, pt, Sel(m, c, {Kind::F}));
         }},
        {"ToPolygons", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             return geom::PlanLimitedDissolve(m, pt, Sel(m, c, {Kind::F}));
         }},
        {"Retriangulate", +[](Mesh& m, const PointTable&, Ctx& c) {
             return geom::PlanRetriangulate(m, Sel(m, c, {Kind::F}));
         }},
        {"Triangulate", +[](Mesh& m, const PointTable&, Ctx& c) {
             return geom::PlanTriangulate(m, Sel(m, c, {Kind::F}));
         }},
        {"Extrude", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::F});
             const geom::ExtrudeType type = static_cast<geom::ExtrudeType>(Pick(c.rng, 3));
             c.note += " type " + std::to_string(static_cast<u32>(type));
             ModelPlan plan = geom::PlanExtrudeFaces(m, pt, s, type);
             Amount(m, plan, c, true);
             return plan;
         }, kMoves},
        {"ExtrudeBorder", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             ModelPlan plan = geom::PlanExtrudeBorder(m, pt, Sel(m, c, {Kind::B}));
             Amount(m, plan, c, false);
             return plan;
         }, kMoves},
        {"Inset", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::F});
             const geom::InsetType type = static_cast<geom::InsetType>(Pick(c.rng, 2));
             c.note += " type " + std::to_string(static_cast<u32>(type));
             ModelPlan plan = geom::PlanInset(m, pt, s, type);
             Amount(m, plan, c, false);
             return plan;
         }},
        {"Hinge", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::F});
             const Topology& t = m.topology();
             std::vector<u32> hinges;
             for (const u32 f : s.faces) {
                 for (const HalfedgeId h : t.fh(FaceId(f))) {
                     const FaceId g = t.face(Topology::opposite(h));
                     if (!g.valid() || !std::binary_search(s.faces.begin(), s.faces.end(), g.value()))
                         hinges.push_back(Topology::edge(h).value());
                 }
             }
             if (hinges.empty()) return Skip(c);
             const u32 hinge = hinges[Pick(c.rng, hinges.size())];
             const f32 angle = Uniform(c.rng, 15.0f, 75.0f) * ((c.rng() & 1u) ? 1.0f : -1.0f);
             c.note += " hinge e" + std::to_string(hinge);
             ModelPlan plan = geom::PlanHingeFaces(m, pt, s, hinge);
             Fixed(m, plan, c, angle);
             return plan;
         }, kMoves},
        {"HingeBorder", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::B});
             if (s.edges.empty()) return Skip(c);
             const Topology& t = m.topology();
             HalfedgeId h = Topology::halfedge(EdgeId(s.edges[Pick(c.rng, s.edges.size())]), 0);
             if (t.isBoundary(h)) h = Topology::opposite(h);
             const u32 vertex = (c.rng() & 1u) ? t.from(h).value() : t.to(h).value();
             const Vector3f axis = FaceNormal(m, t.face(h).value());
             const f32 angle = Uniform(c.rng, 15.0f, 75.0f) * ((c.rng() & 1u) ? 1.0f : -1.0f);
             c.note += " at v" + std::to_string(vertex);
             ModelPlan plan = geom::PlanHingeBorder(m, pt, s, vertex, axis);
             Fixed(m, plan, c, angle);
             return plan;
         }, kMoves},
        {"ChamferEdges", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::E});
             geom::ChamferParams params;
             const u32 segments[] = {1, 1, 2, 3};
             params.segments = segments[Pick(c.rng, 4)];
             c.note += " segments " + std::to_string(params.segments);
             ModelPlan plan = geom::PlanChamferEdges(m, pt, s, params);
             Amount(m, plan, c, false);
             return plan;
         }},
        {"ChamferVertices", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::V});
             geom::ChamferParams params;
             params.open = Pick(c.rng, 4) == 0;
             c.note += params.open ? " open" : "";
             ModelPlan plan = geom::PlanChamferVertices(m, pt, s, params);
             Amount(m, plan, c, false);
             return plan;
         }},
        {"InsertVertex", +[](Mesh& m, const PointTable&, Ctx& c) {
             const std::vector<u32> edges = AllEdges(m).edges;
             if (edges.empty()) return Skip(c);
             const u32 e = edges[Pick(c.rng, edges.size())];
             const f32 at = Uniform(c.rng, 0.1f, 0.9f);
             c.note += "e" + std::to_string(e) + " t " + std::to_string(at);
             return geom::PlanInsertVertex(m, EdgeId(e), at);
         }},
        {"Divide", +[](Mesh& m, const PointTable&, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::E});
             const u32 segments = 2 + Pick(c.rng, 3);
             c.note += " segments " + std::to_string(segments);
             return geom::PlanDivide(m, s, segments);
         }},
        {"ConnectVertices", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             return geom::PlanConnectVertices(m, pt, Sel(m, c, {Kind::V}));
         }},
        {"ConnectEdges", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::E});
             geom::ConnectParams params;
             params.segments = 1 + Pick(c.rng, 3);
             c.note += " segments " + std::to_string(params.segments);
             ModelPlan plan = geom::PlanConnectEdges(m, pt, s, params);
             Fixed(m, plan, c, Uniform(c.rng, -0.8f, 0.8f));
             return plan;
         }},
        {"Cut", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const Topology& t = m.topology();
             const std::vector<u32> faces = AllFaces(m).faces;
             if (faces.size() < 2) return Skip(c);
             const auto p = Positions(std::as_const(m));
             // A click on one of @p face's corners, or along one of its edges.
             const auto click = [&](u32 face, Vector3f& at) {
                 std::vector<HalfedgeId> hs;
                 for (const HalfedgeId h : t.fh(FaceId(face))) hs.push_back(h);
                 const HalfedgeId h = hs[Pick(c.rng, hs.size())];
                 geom::CutPoint point;
                 if (c.rng() & 1u) {
                     point.vertex = t.from(h).value();
                     at = p[point.vertex];
                     c.note += "v" + std::to_string(point.vertex) + " ";
                 } else {
                     const EdgeId e = Topology::edge(h);
                     const HalfedgeId first = Topology::halfedge(e, 0);
                     point.edge = e.value();
                     point.t = Uniform(c.rng, 0.2f, 0.8f);
                     at = Add(Mul(p[t.from(first).value()], 1.0f - point.t), Mul(p[t.to(first).value()], point.t));
                     c.note += "e" + std::to_string(point.edge) + "@" + std::to_string(point.t) + " ";
                 }
                 return point;
             };
             const u32 start = faces[Pick(c.rng, faces.size())];
             const std::vector<u32> patch = PatchFrom(m, c.rng, start, 2 + Pick(c.rng, 5));
             if (patch.size() < 2) return Skip(c);
             Vector3f a, b;
             const geom::CutPoint from = click(start, a);
             const geom::CutPoint to = click(patch[1 + Pick(c.rng, patch.size() - 1)], b);
             const Vector3f plane = Unit(Cross(Sub(b, a), FaceNormal(m, start)));
             if (Length(plane) < 0.5f) return Skip(c);
             return geom::PlanCut(m, pt, from, to, plane);
         }},
        {"Slice", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::F});
             const std::vector<u32> faces = AllFaces(m).faces;
             if (faces.empty()) return Skip(c);
             geom::SliceParams params;
             params.origin = FaceCentre(m, faces[Pick(c.rng, faces.size())]);
             std::normal_distribution<f32> gauss;
             params.normal = (c.rng() & 1u) ? Unit(Vector3f{gauss(c.rng), gauss(c.rng), gauss(c.rng)})
                                            : Vector3f{1.0f, 0.0f, 0.0f};
             params.mode = static_cast<geom::SliceMode>(Pick(c.rng, 3));
             params.keepPositive = (c.rng() & 1u) != 0;
             c.note += " mode " + std::to_string(static_cast<u32>(params.mode));
             return geom::PlanSlice(m, pt, s, params);
         }},
        {"Bridge", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const u32 a = RandomBorderEdge(m, c);
             if (a == kInvalidId) return Skip(c);
             std::set<u32> same;
             for (const HalfedgeId h : BorderLoop(m, a)) same.insert(Topology::edge(h).value());
             std::vector<u32> other;
             for (const u32 e : BorderEdges(m).edges) {
                 if (!same.count(e)) other.push_back(e);
             }
             const u32 b = other.empty() ? RandomBorderEdge(m, c) : other[Pick(c.rng, other.size())];
             geom::BridgeParams params;
             params.twist = static_cast<i32>(Pick(c.rng, 2));
             c.note += "e" + std::to_string(a) + " e" + std::to_string(b);
             return geom::PlanBridge(m, pt, EdgeId(a), EdgeId(b), params);
         }},
        {"BridgeFaces", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const std::vector<u32> faces = AllFaces(m).faces;
             if (faces.size() < 2) return Skip(c);
             const u32 a = faces[Pick(c.rng, faces.size())], b = faces[Pick(c.rng, faces.size())];
             c.note += "f" + std::to_string(a) + " f" + std::to_string(b);
             return geom::PlanBridgeFaces(m, pt, FaceId(a), FaceId(b));
         }},
        {"CreatePolygon", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const u32 e = RandomBorderEdge(m, c);
             if (e == kInvalidId) return Skip(c);
             const std::vector<HalfedgeId> loop = BorderLoop(m, e);
             std::vector<u32> vs;
             for (const HalfedgeId h : loop) vs.push_back(m.topology().from(h).value());
             if ((c.rng() & 1u) && vs.size() > 3) {
                 const u32 start = Pick(c.rng, vs.size());
                 const u32 length = 3 + Pick(c.rng, std::min<std::size_t>(vs.size() - 2, 4));
                 std::vector<u32> run;
                 for (u32 k = 0; k < length && k < vs.size(); ++k) run.push_back(vs[(start + k) % vs.size()]);
                 vs = run;
             }
             c.note += "loop of " + std::to_string(vs.size());
             return geom::PlanCreatePolygon(m, pt, vs);
         }},
        {"Symmetrize", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             geom::SymmetrizeParams params;
             params.axis = Pick(c.rng, 3);
             params.fromPositive = (c.rng() & 1u) != 0;
             if (c.rng() & 1u) {
                 params.origin = Centre(m);
                 params.normal = Vector3f{params.axis == 0 ? 1.0f : 0.0f, params.axis == 1 ? 1.0f : 0.0f,
                                          params.axis == 2 ? 1.0f : 0.0f};
                 c.note += "centre ";
             }
             c.note += "axis " + std::to_string(params.axis) + (params.fromPositive ? " +" : " -");
             return geom::PlanSymmetrize(m, pt, params);
         }, kMoves},
        {"Mirror", +[](Mesh& m, const PointTable&, Ctx& c) {
             geom::MirrorParams params;
             params.origin = Centre(m);
             const u32 axis = Pick(c.rng, 3);
             params.normal = Vector3f{axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f};
             c.note += "axis " + std::to_string(axis);
             return geom::PlanMirror(m, params);
         }, kMoves},
        {"MakePlanar", +[](Mesh& m, const PointTable& pt, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::V, Kind::E, Kind::F});
             const u32 axis = Pick(c.rng, 4);
             c.note += " axis " + std::to_string(axis);
             ModelPlan plan = geom::PlanMakePlanar(m, pt, s, axis);
             Fixed(m, plan, c, 1.0f);
             return plan;
         }, kMoves},
        {"Duplicate", +[](Mesh& m, const PointTable&, Ctx& c) { return geom::PlanDuplicate(m, Sel(m, c, {Kind::F})); }},
        {"DetachToElement", +[](Mesh& m, const PointTable&, Ctx& c) {
             return geom::PlanDetachToElement(m, Sel(m, c, {Kind::F}));
         }},
        {"Detach", +[](Mesh& m, const PointTable&, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::F});
             const bool keep = (c.rng() & 1u) != 0;
             c.note += keep ? " clone" : "";
             Mesh made;
             return geom::PlanDetach(m, s, made, keep);
         }},
        {"Subdivide", +[](Mesh& m, const PointTable&, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::F});
             geom::SubdivideParams params;
             params.scheme = static_cast<geom::SubdivideScheme>(Pick(c.rng, 2));
             params.creases = (c.rng() & 1u) != 0;
             c.note += params.scheme == geom::SubdivideScheme::Linear ? " linear" : " cc";
             return geom::PlanSubdivide(m, s, params);
         }, kMoves, 600},
        {"Smooth", +[](Mesh& m, const PointTable&, Ctx& c) {
             const ElementSet s = Sel(m, c, {Kind::V, Kind::F});
             geom::SmoothParams params;
             params.iterations = 1 + Pick(c.rng, 3);
             params.scheme = static_cast<geom::SmoothScheme>(Pick(c.rng, 2));
             params.pinBorders = (c.rng() & 1u) != 0;
             return geom::PlanSmooth(m, s, params);
         }, kMoves},
        {"OrientOutward", +[](Mesh& m, const PointTable&, Ctx& c) {
             return geom::PlanOrientOutward(m, Sel(m, c, {Kind::F}));
         }},
    };
    return ops;
}

u32 OpIndex(const std::string& name) {
    for (u32 k = 0; k < Ops().size(); ++k) {
        if (name == Ops()[k].name) {
            return k;
        }
    }
    FAIL("no op " << name);
    return 0;
}

// ============================================================================
// One press, with Run's checks (P1-P8, P10)
// ============================================================================

/// What the confirmed suspects already explain (§3; the coordinator's list),
/// for a failed check of @p op; "" when nothing does.
std::string KnownCause(const std::string& op, const std::string& check, const std::string& note) {
    static const std::set<std::string> removal = {"Collapse", "Weld", "TargetWeld", "TargetWeldEdges",
                                                  "Remove", "Delete", "Cap"};
    if (check == "P7" && removal.count(op)) return "X10";
    if (check == "P5" && (op == "Duplicate" || op == "DetachToElement")) return "X2";
    if (check == "P5" && op == "Slice" && note.find("mode 1") != std::string::npos) return "X2";
    if (check == "P10" && op == "Flip") return "FLP-3";
    if (check == "P5" && op == "Flip") return "X2/FLP-2";
    if (check == "P6" && op == "ChamferEdges") return "CHF-1/2";
    if (check == "P6" && op == "ConnectEdges") return "CON-1";
    // A face pinched off by a deletion is split at the pinch, by design (§3.7).
    if (check == "P5" && op == "Delete") return "design:pinch";
    if (check == "P5" && op == "Detach") return "design:pinch";
    if (check == "P5" && op == "Slice" && note.find("mode 2") != std::string::npos) return "design:pinch";
    // Max's Make Planar flattens a side face to zero area too.
    if (check == "P4" && op == "MakePlanar") return "design:flatten";
    return {};
}

/// What differs between two meshes, for P10's sentence.
std::string Difference(const Mesh& a, const Mesh& b) {
    if (CornerLists(a.faceSet()) != CornerLists(b.faceSet())) {
        return "faces";
    }
    const auto pa = Positions(a), pb = Positions(b);
    if (pa.size() != pb.size() || std::memcmp(pa.data(), pb.data(), pa.size_bytes()) != 0) {
        return "positions";
    }
    for (const geom::AttrLayer& layer : a.attributes.layers()) {
        const geom::AttrLayer* other = b.attributes.layer(layer.name, layer.domain);
        if (other == nullptr || other->data != layer.data) {
            return "layer " + layer.name;
        }
    }
    for (const geom::AttrLayer& layer : b.attributes.layers()) {
        if (a.attributes.layer(layer.name, layer.domain) == nullptr) {
            return "layer " + layer.name + " added";
        }
    }
    return "elsewhere (rows, props)";
}

struct Press {
    bool skipped = false;
    bool refused = false;
    ModelRefusal refusal = ModelRefusal::None;
    std::vector<std::pair<std::string, std::string>> failed; ///< (check, why), new causes only.
    std::vector<std::pair<std::string, std::string>> known;  ///< (check, cause).
    std::string note;
    Mesh out;
    ElementSet selection;
    f64 ms = 0.0;
};

/// `Run`'s body, keeping the finished plan's selection for the next step's
/// `prev` shape. Checks that already failed before the press are not charged
/// to it (a fixture's own normals, a uv field a mover left behind).
Press RunPress(const Mesh& mesh, u32 op, u32 seed, Shape shape, const ElementSet& prev, bool lean = false) {
    const Op& o = Ops()[op];
    Press press;
    Ctx c;
    c.rng.seed(seed);
    c.shape = shape;
    c.prev = &prev;
    c.diag = Diagonal(mesh);
    Mesh work = mesh;
    const PointTable points = PointsOf(work);
    const u32 shellsBefore = PointsAcrossShells(mesh);
    const f32 uvBefore = lean ? 1.0f : UvFieldError(mesh);
    const f32 staleBefore = lean ? 2.0f : NormalStaleness(mesh);
    const bool cleanBefore = CheckStructure(mesh).empty() && CheckFinite(mesh).empty();
    const u32 degenerateBefore = DegenerateCount(mesh);
    const auto started = std::chrono::steady_clock::now();
    ModelPlan made = o.run(work, points, c);
    press.note = c.note;
    const auto fail = [&](const char* check, const std::string& why) {
        if (why.empty()) return;
        const std::string cause = KnownCause(o.name, check, c.note);
        if (cause.empty()) {
            press.failed.emplace_back(check, why);
        } else {
            press.known.emplace_back(check, cause);
        }
    };
    if (c.skipped) {
        press.skipped = true;
        return press;
    }
    press.refused = made.refused();
    press.refusal = made.refusal;
    if (made.refused()) {
        if (!SameReflected(work, mesh)) {
            fail("P10", "refused but changed the mesh: " + Difference(work, mesh));
        }
        press.ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
        return press;
    }
    if (Tracing()) {
        std::cerr << "  planned: " << c.note << std::endl;
    }
    geom::FinishTool(work, made, geom::kDefaultShadingAngle);
    press.ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (Tracing()) {
        std::cerr << "  finished" << std::endl;
    }
    // The user's rule (§2.7): a mesh's last polygon is never taken away.
    if (LiveFaces(work) == 0) {
        fail("P0", "left the mesh with no face");
    }
    if (cleanBefore) {
        fail("P1", CheckStructure(work));
    }
    fail("P2", CheckCanonical(work));
    if (cleanBefore) {
        fail("P3", CheckFinite(work));
    }
    if (degenerateBefore == 0) {
        fail("P4", CheckDegenerate(work));
    }
    // Only from an input with no shared point: a tool that copies or cuts geometry
    // already touching another shell (F11, F12, a Duplicate) touches it again.
    const u32 shellsAfter = PointsAcrossShells(work);
    if (!(o.flags & kWelds) && (lean ? shellsAfter > shellsBefore : shellsBefore == 0 && shellsAfter > 0)) {
        fail("P5", std::to_string(shellsAfter - shellsBefore) + " points join shells");
    }
    // The welders move points onto others and keep the corners' values (design).
    static const std::set<std::string> welders = {"Collapse", "Weld", "TargetWeld", "TargetWeldEdges"};
    const f32 uvAfter = lean ? 0.0f : UvFieldError(work);
    if (!(o.flags & kMoves) && !welders.count(o.name) && uvBefore < 1e-3f && uvAfter > std::max(uvBefore, 1e-4f) * 1.01f + 1e-4f) {
        fail("P6", "uv off the field by " + std::to_string(uvAfter));
    }
    if (staleBefore <= 1.0f) {
        const f32 stale = NormalStaleness(work);
        if (stale > 1.0f) {
            fail("P7", "normals " + std::to_string(stale) + " deg from fresh");
        }
    }
    const Topology& t = std::as_const(work).topology();
    for (const u32 v : made.selection.vertices) {
        if (v >= t.vertexCount() || t.isDeleted(VertexId(v))) {
            fail("P8", "selected vertex not live");
            break;
        }
    }
    for (const u32 e : made.selection.edges) {
        if (e >= t.edgeCount() || t.isDeleted(EdgeId(e))) {
            fail("P8", "selected edge not live");
            break;
        }
    }
    for (const u32 f : made.selection.faces) {
        if (f >= t.faceCount() || t.isDeleted(FaceId(f))) {
            fail("P8", "selected face not live");
            break;
        }
    }
    press.selection = made.selection;
    press.out = std::move(work);
    return press;
}

// ============================================================================
// Fixtures (F1-F16, with Fields)
// ============================================================================

struct Fixture {
    std::string id;
    Mesh mesh;
};

const std::vector<Fixture>& Fixtures() {
    static const std::vector<Fixture> fixtures = [] {
        const auto field = [](Mesh mesh, f32 width) {
            Fields(mesh, width);
            return Finished(std::move(mesh));
        };
        Mesh seam = QuadGrid(4, 3);
        SeamUvs(seam, 2.0f);
        std::vector<Fixture> out;
        out.push_back({"F1", field(QuadGrid(4, 3), 4.0f)});
        out.push_back({"F1s", field(QuadGrid(3, 1), 3.0f)});
        out.push_back({"F2", field(TriGrid(3, 3), 3.0f)});
        out.push_back({"F3", field(MixedGrid(), 4.0f)});
        out.push_back({"F3L", field(LShape(), 2.0f)});
        out.push_back({"F4", field(Box(), 1.0f)});
        out.push_back({"F5n", field(Cylinder(6, 2, Caps::Ngon), 2.0f)});
        out.push_back({"F5f", field(Cylinder(6, 2, Caps::Fan), 2.0f)});
        out.push_back({"F6", field(Cylinder(6, 2, Caps::None), 2.0f)});
        out.push_back({"F7", field(UvSphere(8, 5), 2.0f)});
        out.push_back({"F8", field(Crack(4, 3, 2), 4.0f)});
        out.push_back({"F9", Finished(std::move(seam))});
        out.push_back({"F10", field(TwoSections(4, 3, 2), 4.0f)});
        out.push_back({"F11", field(DoubleCard(), 1.0f)});
        out.push_back({"F12", field(TouchingQuads(), 2.0f)});
        out.push_back({"F13", field(SmoothPrism(5), 2.0f)});
        out.push_back({"F14", field(ValenceTwo(), 2.0f)});
        out.push_back({"F15q", field(QuadHole(), 3.0f)});
        out.push_back({"F15t", field(TriangleHole(), 3.0f)});
        out.push_back({"F16", field(OpenFan(), 2.0f)});
        return out;
    }();
    return fixtures;
}

u32 FixtureIndex(const std::string& id) {
    for (u32 k = 0; k < Fixtures().size(); ++k) {
        if (Fixtures()[k].id == id) {
            return k;
        }
    }
    FAIL("no fixture " << id);
    return 0;
}

// ============================================================================
// Chains (§2.6.2)
// ============================================================================

struct StepSpec {
    u32 op = 0;
    u32 seed = 0;
    Shape shape = kOne;
};

struct Chain {
    u32 fixture = 0;
    std::vector<StepSpec> steps;
};

struct ChainResult {
    i32 failAt = -1;
    std::string signature; ///< op ":" check of the first new failure; "" when clean.
    std::string why;
    std::vector<std::string> log;
};

struct FuzzStats {
    u64 steps = 0, skipped = 0;
    std::map<std::string, u64> runs;                                    // op
    std::map<std::string, std::map<std::string, u64>> outcomes;         // op -> refusal / "ok"
    std::map<std::string, std::map<std::string, u64>> failures;         // op -> check
    std::map<std::string, std::map<std::string, u64>> knownFailures;    // op -> cause
};

std::string Describe(const Chain& chain) {
    std::ostringstream out;
    out << "{\"" << Fixtures()[chain.fixture].id << "\", {";
    for (std::size_t k = 0; k < chain.steps.size(); ++k) {
        out << (k ? ", " : "") << "{\"" << Ops()[chain.steps[k].op].name << "\", " << chain.steps[k].seed << "u, "
            << kShapeNames[chain.steps[k].shape] << "}";
    }
    out << "}}";
    return out.str();
}

ChainResult Replay(const Chain& chain, FuzzStats* stats = nullptr, std::ofstream* csv = nullptr, u64 chainId = 0) {
    ChainResult result;
    Mesh mesh = Fixtures()[chain.fixture].mesh;
    ElementSet prev;
    for (std::size_t k = 0; k < chain.steps.size(); ++k) {
        const StepSpec& step = chain.steps[k];
        const Op& op = Ops()[step.op];
        if (LiveFaces(mesh) == 0) {
            break;
        }
        if (LiveFaces(mesh) > op.maxFaces) {
            continue;
        }
        if (Tracing()) {
            std::cerr << "chain " << chainId << " step " << k << " " << op.name << " seed " << step.seed << " "
                      << kShapeNames[step.shape] << std::endl;
        }
        Press press = RunPress(mesh, step.op, step.seed, step.shape, prev);
        std::string outcome = press.skipped ? "skip" : press.refused ? geom::ToString(press.refusal) : "ok";
        for (const auto& [check, cause] : press.known) {
            outcome += " known:" + check + "=" + cause;
        }
        for (const auto& [check, why] : press.failed) {
            outcome += " FAIL:" + check + "=" + why;
        }
        result.log.push_back(std::string(op.name) + " [" + kShapeNames[step.shape] + "] " + press.note + " -> " +
                             outcome);
        if (stats != nullptr) {
            ++stats->steps;
            if (press.skipped) {
                ++stats->skipped;
            } else {
                ++stats->runs[op.name];
                ++stats->outcomes[op.name][press.refused ? geom::ToString(press.refusal) : "ok"];
                for (const auto& [check, why] : press.failed) ++stats->failures[op.name][check];
                for (const auto& [check, cause] : press.known) ++stats->knownFailures[op.name][check + "=" + cause];
            }
        }
        if (csv != nullptr) {
            *csv << chainId << "," << Fixtures()[chain.fixture].id << "," << k << "," << op.name << ","
                 << kShapeNames[step.shape] << "," << step.seed << "," << Cell(press.note) << "," << Cell(outcome)
                 << "," << (press.out.vertexCount() ? LiveFaces(press.out) : LiveFaces(mesh)) << "\n";
        }
        if (press.skipped) {
            continue;
        }
        if (!press.failed.empty()) {
            result.failAt = static_cast<i32>(k);
            result.signature = std::string(op.name) + ":" + press.failed.front().first;
            result.why = press.failed.front().second;
            return result;
        }
        if (press.refused) {
            continue; // the mesh as it was (P10 checked above)
        }
        const bool stale = std::any_of(press.known.begin(), press.known.end(),
                                       [](const auto& entry) { return entry.first == "P7"; });
        mesh = std::move(press.out);
        if (stale) {
            Shade(mesh); // X10 confirmed: go on from fresh normals
        }
        prev = press.selection;
    }
    return result;
}

Chain RandomChain(u64 seed) {
    Rng rng(static_cast<u32>(seed * 2654435761ull + 17u));
    Chain chain;
    chain.fixture = Pick(rng, Fixtures().size());
    const u32 length = 5 + Pick(rng, 16);
    for (u32 k = 0; k < length; ++k) {
        StepSpec step;
        step.op = Pick(rng, Ops().size());
        step.seed = static_cast<u32>(rng());
        step.shape = static_cast<Shape>(Pick(rng, kShapeCount));
        chain.steps.push_back(step);
    }
    return chain;
}

/// Drops steps, then shrinks shapes to One / Few, while the chain still fails
/// with @p signature.
Chain Shrink(Chain chain, const std::string& signature) {
    const auto same = [&](const Chain& candidate, Chain& trimmed) {
        const ChainResult r = Replay(candidate);
        if (r.signature != signature) {
            return false;
        }
        trimmed = candidate;
        trimmed.steps.resize(static_cast<std::size_t>(r.failAt) + 1);
        return true;
    };
    Chain trimmed;
    if (same(chain, trimmed)) {
        chain = trimmed;
    }
    for (bool progress = true; progress;) {
        progress = false;
        for (std::size_t k = 0; k + 1 < chain.steps.size(); ++k) {
            Chain candidate = chain;
            candidate.steps.erase(candidate.steps.begin() + static_cast<std::ptrdiff_t>(k));
            if (same(candidate, trimmed)) {
                chain = trimmed;
                progress = true;
                break;
            }
        }
    }
    for (std::size_t k = 0; k < chain.steps.size(); ++k) {
        for (const Shape shape : {kOne, kFew, kPatch}) {
            if (shape >= chain.steps[k].shape) {
                break;
            }
            Chain candidate = chain;
            candidate.steps[k].shape = shape;
            if (same(candidate, trimmed)) {
                chain = trimmed;
                break;
            }
        }
    }
    return chain;
}

Chain MakeChain(const std::string& fixture, std::initializer_list<std::tuple<const char*, u32, Shape>> steps) {
    Chain chain;
    chain.fixture = FixtureIndex(fixture);
    for (const auto& [op, seed, shape] : steps) {
        chain.steps.push_back(StepSpec{OpIndex(op), seed, shape});
    }
    return chain;
}

/// A kept chain must replay clean; on failure it prints the steps it took.
void ExpectClean(const Chain& chain) {
    const ChainResult result = Replay(chain);
    std::string log;
    for (const std::string& line : result.log) {
        log += "  " + line + "\n";
    }
    INFO("chain " << Describe(chain) << "\n" << log);
    CHECK(result.signature.empty());
}

// ============================================================================
// Real models (F23)
// ============================================================================

struct RealMesh {
    std::string model;
    std::string name;
    u32 rawVertices = 0;
    geom::PrepareReport report;
    Mesh mesh; ///< As the editor's view holds it: prepared, connectivity built.
};

std::vector<fs::path> RealModelFiles() {
    std::vector<fs::path> files;
    if (const char* env = std::getenv("WDX_MESH_AUDIT_MODELS"); env != nullptr && fs::is_directory(env)) {
        for (const auto& entry : fs::directory_iterator(env)) {
            if (entry.path().extension() == ".mdx") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
    }
    const std::string base = test::findCorpusBase("Corpus");
    if (!base.empty() && fs::exists(fs::path(base) / "Conversions" / "kaelthas.mdx")) {
        files.push_back(fs::path(base) / "Conversions" / "kaelthas.mdx");
    }
    const std::vector<fs::path> corpus = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL"});
    const std::size_t limit = test::sweepLimit(corpus.size(), 6);
    for (std::size_t k = 0; k < limit; ++k) {
        if (!test::isKnownBad(corpus[k])) {
            files.push_back(corpus[k]);
        }
    }
    return files;
}

const std::vector<RealMesh>& RealMeshes() {
    static const std::vector<RealMesh> meshes = [] {
        std::vector<RealMesh> out;
        const MdxConverter converter;
        for (const fs::path& file : RealModelFiles()) {
            test::trace(file);
            const std::vector<u8> bytes = test::readCorpusFile(file);
            if (bytes.empty()) {
                continue;
            }
            whiteout::mdx::Parser parser;
            const whiteout::mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
            const Result<Document> document = converter.fromMdx(source);
            if (!document.ok()) {
                std::cout << "  could not convert " << test::pathText(file) << "\n";
                continue;
            }
            for (const Model& model : document->models) {
                for (const Mesh& imported : model.meshes) {
                    RealMesh real;
                    real.model = test::pathText(file.filename());
                    real.name = imported.name;
                    real.rawVertices = imported.vertexCount();
                    real.mesh = imported;
                    real.report = geom::PrepareForModelling(real.mesh);
                    (void)real.mesh.ensureConnectivity();
                    if (real.mesh.vertexCount() == 0 || LiveFaces(real.mesh) == 0) {
                        continue;
                    }
                    out.push_back(std::move(real));
                }
            }
        }
        return out;
    }();
    return meshes;
}

struct CrackCount {
    u32 twinVertices = 0;
    u32 twinPoints = 0;
    u32 crossShellPoints = 0;
    u32 shells = 0;
};

/// §2.6.4: vertices sharing a point with another vertex of the same shell
/// with no edge between them.
CrackCount CountCracks(const Mesh& mesh) {
    CrackCount count;
    const PointTable points = PointsOf(mesh);
    const std::vector<u32> shell = ShellOfVertex(mesh);
    std::set<u32> shells;
    for (const u32 s : shell) {
        if (s != kInvalidId) shells.insert(s);
    }
    count.shells = static_cast<u32>(shells.size());
    for (u32 point = 0; point < points.pointCount; ++point) {
        const std::span<const u32> members = points.membersOf(point);
        if (members.size() < 2) {
            continue;
        }
        std::set<u32> twinned, across;
        for (std::size_t a = 0; a < members.size(); ++a) {
            const u32 va = members[a];
            if (va >= shell.size() || shell[va] == kInvalidId) continue;
            across.insert(shell[va]);
            for (std::size_t b = a + 1; b < members.size(); ++b) {
                const u32 vb = members[b];
                if (vb >= shell.size() || shell[vb] != shell[va]) continue;
                if (EdgeOf(mesh, va, vb) == kInvalidId) {
                    twinned.insert(va);
                    twinned.insert(vb);
                }
            }
        }
        if (!twinned.empty()) {
            ++count.twinPoints;
            count.twinVertices += static_cast<u32>(twinned.size());
        }
        count.crossShellPoints += across.size() > 1 ? 1u : 0u;
    }
    return count;
}

/// P11's `.wem` half: written and read back, equal by face corners, by edge
/// pair and by every layer. "" when equal.
std::string WemRoundTrip(const Mesh& mesh) {
    Document document;
    document.declare(ProfileId::Generic);
    document.defaultProfile = ProfileId::Generic;
    Model model;
    model.name = "audit";
    model.meshes.push_back(mesh);
    document.models.push_back(std::move(model));
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    std::optional<Document> read = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    if (!read || read->models.size() != 1 || read->models[0].meshes.size() != 1) {
        return "did not read back";
    }
    Mesh back = std::move(read->models[0].meshes[0]);
    (void)back.ensureConnectivity();
    if (CornerLists(mesh.faceSet()) != CornerLists(back.faceSet())) {
        return "face corners differ";
    }
    const auto pa = Positions(mesh), pb = Positions(std::as_const(back));
    if (pa.size() != pb.size() || std::memcmp(pa.data(), pb.data(), pa.size_bytes()) != 0) {
        return "positions differ";
    }
    const Topology& ta = mesh.topology();
    const Topology& tb = std::as_const(back).topology();
    for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
        const geom::AttrLayer* other = back.attributes.layer(layer.name, layer.domain);
        if (other == nullptr) {
            return "layer " + layer.name + " lost";
        }
        const std::size_t stride = geom::AttrTypeSize(layer.type);
        if (layer.domain == Domain::Halfedge) {
            for (const u32 f : AllFaces(mesh).faces) {
                std::vector<HalfedgeId> a, b;
                for (const HalfedgeId h : ta.fh(FaceId(f))) a.push_back(h);
                for (const HalfedgeId h : tb.fh(FaceId(f))) b.push_back(h);
                for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
                    if (std::memcmp(layer.data.data() + stride * a[k].index(),
                                    other->data.data() + stride * b[k].index(), stride) != 0) {
                        return "corner layer " + layer.name + " differs";
                    }
                }
            }
        } else if (layer.domain == Domain::Edge) {
            for (const u32 e : AllEdges(mesh).edges) {
                const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
                const HalfedgeId g = tb.findHalfedge(ta.from(h), ta.to(h));
                if (!g.valid()) {
                    return "edge lost";
                }
                if (std::memcmp(layer.data.data() + stride * e, other->data.data() + stride * Topology::edge(g).index(),
                                stride) != 0) {
                    return "edge layer " + layer.name + " differs";
                }
            }
        } else if (layer.data.size() != other->data.size() ||
                   std::memcmp(layer.data.data(), other->data.data(), layer.data.size()) != 0) {
            return "layer " + layer.name + " differs";
        }
    }
    return {};
}

} // namespace

// ============================================================================
// §2.6.4 Crack census
// ============================================================================

TEST_CASE("audit census: crack twins on the real models", "[.mesh-audit][census]") {
    const std::vector<RealMesh>& meshes = RealMeshes();
    if (meshes.empty()) {
        SKIP("no real model found");
    }
    std::ofstream csv(OutDir() / "census.csv");
    csv << "model,mesh,raw vertices,vertices,faces,shells,twin vertices,points with twins,cross-shell points,"
           "welded,kept binding,kept manifold,kept face corners\n";
    std::map<std::string, std::array<u64, 4>> perModel; // vertices, twins, points, meshes
    std::cout << "| model | mesh | vertices | twin vertices | points with twins | kept: binding / manifold |\n";
    std::cout << "|---|---|---|---|---|---|\n";
    for (const RealMesh& real : meshes) {
        const CrackCount count = CountCracks(real.mesh);
        csv << Cell(real.model) << "," << Cell(real.name) << "," << real.rawVertices << ","
            << LiveVertices(real.mesh) << "," << LiveFaces(real.mesh) << "," << count.shells << ","
            << count.twinVertices << "," << count.twinPoints << "," << count.crossShellPoints << ","
            << real.report.verticesWelded << "," << real.report.keptBinding << "," << real.report.keptNonManifold
            << "," << real.report.keptFaceCorners << "\n";
        std::array<u64, 4>& sum = perModel[real.model];
        sum[0] += LiveVertices(real.mesh);
        sum[1] += count.twinVertices;
        sum[2] += count.twinPoints;
        sum[3] += 1;
        if (count.twinVertices > 0) {
            std::cout << "| " << real.model << " | " << real.name << " | " << LiveVertices(real.mesh) << " | "
                      << count.twinVertices << " | " << count.twinPoints << " | " << real.report.keptBinding
                      << " / " << real.report.keptNonManifold << " |\n";
        }
    }
    std::cout << "\nper model: meshes, vertices, twin vertices, points with twins\n";
    for (const auto& [model, sum] : perModel) {
        std::cout << "  " << model << ": " << sum[3] << " meshes, " << sum[0] << " vertices, " << sum[1] << " twins, "
                  << sum[2] << " points\n";
    }
    CHECK(!meshes.empty());
}

// ============================================================================
// §2.6.2 The chain fuzzer
// ============================================================================

TEST_CASE("audit fuzz: random chains of kernel operations on the fixtures", "[.mesh-audit][fuzz]") {
    u64 chains = 2000;
    if (const char* env = std::getenv("WDX_FUZZ_CHAINS"); env != nullptr && *env != '\0') {
        chains = std::strtoull(env, nullptr, 10);
    }
    u64 first = 0;
    if (const char* env = std::getenv("WDX_FUZZ_FIRST"); env != nullptr && *env != '\0') {
        first = std::strtoull(env, nullptr, 10);
    }
    std::ofstream csv(OutDir() / "fuzz.csv");
    csv << "chain,fixture,step,op,shape,seed,selection,outcome,faces after\n";
    FuzzStats stats;
    std::map<std::string, std::pair<u64, Chain>> firstOf; // signature -> (count, first chain)
    std::map<std::string, std::string> whyOf;
    const auto started = std::chrono::steady_clock::now();
    u64 failedChains = 0;
    std::set<u64> crashing; // chains that kill the process: kept as `[.crash]` tests below
    if (const char* env = std::getenv("WDX_FUZZ_SKIP"); env != nullptr) {
        for (const char* at = env; *at != '\0';) {
            char* end = nullptr;
            crashing.insert(std::strtoull(at, &end, 10));
            at = *end == ',' ? end + 1 : end;
            if (end == at && *at != '\0') break;
        }
    }
    for (u64 id = first; id < first + chains; ++id) {
        if (crashing.count(id)) {
            continue;
        }
        const Chain chain = RandomChain(id);
        const ChainResult result = Replay(chain, &stats, &csv, id);
        if (!result.signature.empty()) {
            ++failedChains;
            auto [it, inserted] = firstOf.try_emplace(result.signature, 0, chain);
            ++it->second.first;
            if (inserted) {
                whyOf[result.signature] = result.why;
            }
        }
    }
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
    std::cout << "fuzz: " << chains << " chains, " << stats.steps << " steps (" << stats.skipped << " skipped), "
              << failedChains << " failing chains, " << seconds << " s\n";
    std::cout << "\nper op: runs | outcomes | new failures | known failures\n";
    for (const auto& [op, runs] : stats.runs) {
        std::cout << "  " << op << ": " << runs << " |";
        for (const auto& [what, n] : stats.outcomes[op]) std::cout << " " << what << "=" << n;
        std::cout << " |";
        for (const auto& [what, n] : stats.failures[op]) std::cout << " " << what << "=" << n;
        std::cout << " |";
        for (const auto& [what, n] : stats.knownFailures[op]) std::cout << " " << what << "=" << n;
        std::cout << "\n";
    }
    std::cout << "\nshrunk failing chains, by signature:\n";
    for (const auto& [signature, entry] : firstOf) {
        const Chain shrunk = Shrink(entry.second, signature);
        const ChainResult result = Replay(shrunk);
        std::cout << "- " << signature << " (" << entry.first << " chains): " << whyOf[signature] << "\n    "
                  << Describe(shrunk) << "\n";
        for (const std::string& line : result.log) {
            std::cout << "      " << line << "\n";
        }
    }
    CHECK(stats.steps > 0u);
}

// ============================================================================
// §2.6.3 Every operation on every real mesh
// ============================================================================

TEST_CASE("audit corpus: every operation on every real mesh", "[.mesh-audit][corpus]") {
    const std::vector<RealMesh>& meshes = RealMeshes();
    if (meshes.empty()) {
        SKIP("no real model found");
    }
    std::ofstream csv(OutDir() / "corpus.csv");
    csv << "model,mesh,vertices,faces,op,shape,seed,selection,outcome,ms,round trip\n";
    std::map<std::string, std::map<std::string, u64>> histogram; // op -> outcome
    std::map<std::string, std::map<std::string, u64>> failures;  // op -> check
    std::map<std::string, u64> knownCount;
    std::vector<std::string> slow, failLines, tripLines;
    u64 presses = 0;
    const std::vector<Shape> shapes = {kOne, kFew, kPatch, kRing, kLoop, kAll};
    for (u32 index = 0; index < meshes.size(); ++index) {
        const RealMesh& real = meshes[index];
        if (LiveFaces(real.mesh) < 8) {
            continue;
        }
        for (u32 op = 0; op < Ops().size(); ++op) {
            for (const Shape shape : shapes) {
                const u32 seed = index * 7919u + op * 131u + shape;
                if (Tracing()) {
                    std::cerr << real.model << " '" << real.name << "' " << Ops()[op].name << " "
                              << kShapeNames[shape] << std::endl;
                }
                Press press = RunPress(real.mesh, op, seed, shape, ElementSet{}, true);
                if (press.skipped) {
                    continue;
                }
                ++presses;
                const std::string name = Ops()[op].name;
                std::string outcome = press.refused ? geom::ToString(press.refusal) : "ok";
                ++histogram[name][outcome];
                for (const auto& [check, why] : press.failed) {
                    // P6 and P7 mean nothing on authored UVs and normals.
                    if (check == "P6" || check == "P7") continue;
                    ++failures[name][check];
                    outcome += " FAIL:" + check + "=" + why;
                    if (failLines.size() < 60) {
                        failLines.push_back(name + " " + check + " on " + real.model + " '" + real.name + "' [" +
                                            kShapeNames[shape] + "] " + press.note + ": " + why);
                    }
                }
                for (const auto& [check, cause] : press.known) {
                    if (check == "P6" || check == "P7") continue;
                    ++knownCount[name + " " + check + "=" + cause];
                }
                if (press.ms > 50.0) {
                    slow.push_back(name + " [" + kShapeNames[shape] + "] " + std::to_string(press.ms) + " ms on " +
                                   real.model + " '" + real.name + "' (" + std::to_string(LiveVertices(real.mesh)) +
                                   " v, " + std::to_string(LiveFaces(real.mesh)) + " f)");
                }
                std::string trip;
                if (!press.refused && shape == kPatch && LiveFaces(press.out) > 0) {
                    trip = WemRoundTrip(press.out);
                    if (!trip.empty()) {
                        ++failures[name]["P11"];
                        tripLines.push_back(name + " on " + real.model + " '" + real.name + "': " + trip);
                    }
                }
                csv << Cell(real.model) << "," << Cell(real.name) << "," << LiveVertices(real.mesh) << ","
                    << LiveFaces(real.mesh) << "," << name << "," << kShapeNames[shape] << "," << seed << ","
                    << Cell(press.note) << "," << Cell(outcome) << "," << press.ms << "," << Cell(trip) << "\n";
            }
        }
        csv.flush();
    }
    std::cout << "corpus: " << presses << " presses on " << meshes.size() << " meshes\n\nrefusal histogram per op:\n";
    for (const auto& [op, outcomes] : histogram) {
        std::cout << "  " << op << ":";
        for (const auto& [what, n] : outcomes) std::cout << " " << what << "=" << n;
        if (failures.count(op)) {
            std::cout << " | FAIL";
            for (const auto& [check, n] : failures[op]) std::cout << " " << check << "=" << n;
        }
        std::cout << "\n";
    }
    std::cout << "\nknown causes:\n";
    for (const auto& [what, n] : knownCount) std::cout << "  " << what << " x" << n << "\n";
    std::cout << "\ncheck failures (first 60):\n";
    for (const std::string& line : failLines) std::cout << "  " << line << "\n";
    std::cout << "\nround trips that differ:\n";
    for (const std::string& line : tripLines) std::cout << "  " << line << "\n";
    std::cout << "\npresses over 50 ms: " << slow.size() << "\n";
    for (const std::string& line : slow) std::cout << "  " << line << "\n";
    CHECK(presses > 0u);
}

// ============================================================================
// Replaying one chain, for shrinking a crash in child processes
// ============================================================================

/// `WDX_FUZZ_REPLAY` = a chain id, `WDX_FUZZ_DROP` = its step indices to leave
/// out (comma separated), `WDX_FUZZ_ONE` = step indices whose shape becomes One.
TEST_CASE("audit fuzz: replay one chain", "[.mesh-audit-replay]") {
    const char* id = std::getenv("WDX_FUZZ_REPLAY");
    if (id == nullptr) {
        SKIP("WDX_FUZZ_REPLAY not set");
    }
    const auto list = [](const char* name) {
        std::set<u32> out;
        if (const char* env = std::getenv(name); env != nullptr) {
            std::stringstream text(env);
            for (std::string item; std::getline(text, item, ',');) {
                if (!item.empty()) out.insert(static_cast<u32>(std::stoul(item)));
            }
        }
        return out;
    };
    const std::set<u32> drop = list("WDX_FUZZ_DROP"), one = list("WDX_FUZZ_ONE");
    const Chain full = RandomChain(std::strtoull(id, nullptr, 10));
    Chain chain;
    chain.fixture = full.fixture;
    for (u32 k = 0; k < full.steps.size(); ++k) {
        if (drop.count(k)) continue;
        StepSpec step = full.steps[k];
        if (one.count(k)) step.shape = kOne;
        chain.steps.push_back(step);
    }
    std::cout << Describe(chain) << std::endl;
    const ChainResult result = Replay(chain);
    for (const std::string& line : result.log) {
        std::cout << "  " << line << "\n";
    }
    CHECK(result.signature.empty());
}

// ============================================================================
// What the fuzzer found: the shrunk chains (seeds as the sweep printed them)
// ============================================================================

TEST_CASE("audit FUZ-1: a refused Symmetrize leaves the mesh as it was", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F9", {{"Symmetrize", 1466231305u, kOne}}));
}

TEST_CASE("audit FUZ-1: a refused Duplicate leaves the mesh as it was", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F15t", {{"Duplicate", 3182621166u, kFew}}));
}

TEST_CASE("audit FUZ-1: a refused Bridge leaves the mesh as it was", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F11", {{"Bridge", 1691903830u, kOne}}));
}

TEST_CASE("audit FUZ-1: a refused Bridge of polygons leaves the mesh as it was", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F10", {{"Delete", 3397193410u, kFew}, {"BridgeFaces", 4025383932u, kOne}}));
}

TEST_CASE("audit FUZ-1: a refused Detach to Element leaves the mesh as it was", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F9", {{"Extrude", 360028444u, kFew}, {"DetachToElement", 2983715770u, kFew}}));
}

TEST_CASE("audit FUZ-1: a refused Chamfer leaves the mesh as it was", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F2", {{"Cap", 616048891u, kOne}, {"ChamferEdges", 523338051u, kFew}}));
}

TEST_CASE("audit FUZ-1: a refused Create Polygon leaves the mesh as it was", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F15t", {{"CreatePolygon", 3722813597u, kOne}}));
}

TEST_CASE("audit FUZ-2: Hinge Border leaves every stored row a valid cut", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F8", {{"HingeBorder", 597083438u, kPatch}}));
}

TEST_CASE("audit FUZ-2: Target Weld leaves every stored row a valid cut", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F3L", {{"TargetWeld", 2384422823u, kOne}}));
}

TEST_CASE("audit FUZ-2: Make Planar leaves every stored row a valid cut", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F6", {{"Symmetrize", 2989167845u, kOne}, {"MakePlanar", 1891653110u, kPatch}}));
}

TEST_CASE("audit FUZ-3: Create Polygon's corners keep the surface's UVs", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F1", {{"CreatePolygon", 3937160047u, kOne}}));
}

TEST_CASE("audit FUZ-3: Remove keeps the ring's UVs", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F5n", {{"Remove", 860324046u, kFew}}));
}

TEST_CASE("audit FUZ-3: a vertex Chamfer's corners keep the surface's UVs", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F5f", {{"ChamferVertices", 3538713516u, kOne}}));
}

TEST_CASE("audit FUZ-4: Collapse of a few box faces drops no face in the repair", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F4", {{"Collapse", 4264513319u, kFew}}));
}

TEST_CASE("audit FUZ-5: Weld never leaves a mesh with no face", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F7", {{"Weld", 711171995u, kAll}}));
}

TEST_CASE("audit FUZ-6: Detach re-shades the border it opens", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F5n", {{"Detach", 2755618788u, kFew}}));
}

TEST_CASE("audit FUZ-6: Detach to Element re-shades the border it opens", "[wem][audit][fuzz]") {
    ExpectClean(MakeChain("F7", {{"DetachToElement", 1665342710u, kOne}}));
}

TEST_CASE("audit FUZ-6: Slice re-shades the faces it cut and the corners it leaves", "[wem][audit][fuzz]") {
    // Remove: three faces of the n-gon-capped cylinder (the fuzzer's chain 985).
    ExpectClean(MakeChain("F5n", {{"Slice", 1871568618u, kFew}}));
    ExpectClean(MakeChain("F6", {{"Slice", 401382775u, kPatch}}));
}

TEST_CASE("audit FUZ-10: Triangulate keeps the shading it was given", "[wem][audit][fuzz]") {
    // Decided: as Max (smoothing groups) and Blender (custom normals), the
    // triangles keep their polygon's corner normals; a fresh shade weighs the
    // new triangles' corners otherwise (2-36 degrees here), which the matrix
    // notes count as on purpose. The fuzzer's press: F5f, a few faces.
    const Mesh& before = Fixtures()[FixtureIndex("F5f")].mesh;
    Mesh after = before;
    ModelPlan plan = geom::PlanTriangulate(after, AllFaces(after));
    REQUIRE_FALSE(plan.refused());
    REQUIRE(plan.changed > 0u);
    geom::FinishTool(after, plan);
    const auto normalsAt = [](const Mesh& mesh) {
        const Topology& topology = mesh.topology();
        const auto normals = mesh.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
        std::vector<std::vector<Vector3f>> out(topology.vertexCount());
        for (u32 f = 0; f < topology.faceCount(); ++f) {
            if (topology.isDeleted(FaceId(f))) {
                continue;
            }
            for (const HalfedgeId h : topology.fh(FaceId(f))) {
                out[topology.from(h).value()].push_back(normals[h.index()]);
            }
        }
        return out;
    };
    const auto was = normalsAt(before);
    const auto now = normalsAt(after);
    REQUIRE(now.size() == was.size());
    u32 off = 0;
    for (u32 v = 0; v < now.size(); ++v) {
        for (const Vector3f& n : now[v]) {
            const bool kept = std::any_of(was[v].begin(), was[v].end(),
                                          [&](const Vector3f& m) { return Length(n - m) < 1e-5f; });
            off += kept ? 0u : 1u;
        }
    }
    CHECK(off == 0u);
}

TEST_CASE("audit FUZ-11: Hinge leaves a vertex on the hinge's line where it is, with no wall",
          "[wem][audit][fuzz]") {
    // F14's left face swung about 1-6; vertex 4 lies on the same line (x = 1), so
    // it cannot move: copying it makes a zero-length edge and a zero-area wall.
    const Mesh mesh = Finished(ValenceTwo());
    Mesh out;
    const Verdict verdict = Run(mesh, [](Mesh& m, const PointTable& pt) {
        ModelPlan plan = geom::PlanHingeFaces(m, pt, Faces({0}), EdgeOf(m, 1, 6));
        geom::ApplyAmount(m, plan, 45.0f);
        return plan;
    }, {}, &out);
    INFO(verdict.failed);
    CHECK_FALSE(verdict.refused);
    CHECK(CheckDegenerate(out).empty());
}

TEST_CASE("audit FUZ-12: Collapse of a whole closed mesh answers within 50 ms", "[wem][audit][fuzz][.perf]") {
    // The corpus sweep timed a Collapse of every vertex at 193 s on the HD footman body (9774 faces)
    // and 5.8 s on HD Arthas; both end CollapseIllegal. F24 stand-in: a closed sphere.
    Mesh mesh = Finished(UvSphere(64, 48));
    const PointTable points = PointsOf(mesh);
    const auto started = std::chrono::steady_clock::now();
    const ModelPlan plan = geom::PlanCollapse(mesh, points, AllVertices(mesh));
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    INFO(LiveFaces(mesh) << " faces, " << ms << " ms, " << geom::ToString(plan.refusal));
    CHECK(ms < 50.0);
}

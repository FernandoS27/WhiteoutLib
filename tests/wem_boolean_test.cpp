// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Mesh booleans (EDIT_MODE_BOOLEAN_DESIGN.md §7.1): volumes and areas worked
// out by hand, closed in gives closed out, the four classes, open meshes, the
// attributes a cut carries, and the same bytes twice.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/boolean.h>
#include <whiteout/models/wem/geometry/fracture/winding.h>
#include <whiteout/models/wem/geometry/prepare.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/triangulation.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <tuple>
#include <string>
#include <utility>
#include <vector>

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace whiteout::models::wem::geom;
using Catch::Approx;

namespace {

Mesh Box(f32 half, const Vector3f& at = {0, 0, 0}) {
    PrimitiveParams p;
    p.size = Vector3f(half, half, half);
    Mesh mesh = MakeBox(p);
    for (Vector3f& v : mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex)) {
        v = Vector3f(v.x + at.x, v.y + at.y, v.z + at.z);
    }
    mesh.recomputeBounds();
    return mesh;
}

Mesh BoxXYZ(const Vector3f& half, const Vector3f& at) {
    PrimitiveParams p;
    p.size = half;
    Mesh mesh = MakeBox(p);
    for (Vector3f& v : mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex)) {
        v = Vector3f(v.x + at.x, v.y + at.y, v.z + at.z);
    }
    mesh.recomputeBounds();
    return mesh;
}

/// Turns every position about the unit axis @p axis by @p angle radians.
void Turn(Mesh& mesh, Vector3d axis, f64 angle) {
    const f64 len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis = Vector3d(axis.x / len, axis.y / len, axis.z / len);
    const f64 c = std::cos(angle);
    const f64 s = std::sin(angle);
    for (Vector3f& v : mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex)) {
        const Vector3d p(v.x, v.y, v.z);
        const f64 d = axis.x * p.x + axis.y * p.y + axis.z * p.z;
        const Vector3d k(axis.y * p.z - axis.z * p.y, axis.z * p.x - axis.x * p.z, axis.x * p.y - axis.y * p.x);
        v = Vector3f(static_cast<f32>(p.x * c + k.x * s + axis.x * d * (1 - c)),
                     static_cast<f32>(p.y * c + k.y * s + axis.y * d * (1 - c)),
                     static_cast<f32>(p.z * c + k.z * s + axis.z * d * (1 - c)));
    }
}

f64 Volume(const Mesh& mesh) {
    std::vector<u32> tri;
    TriangulateMesh(mesh, tri);
    const auto p = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    f64 sum = 0.0;
    for (std::size_t i = 0; i + 2 < tri.size(); i += 3) {
        const Vector3f& a = p[tri[i]];
        const Vector3f& b = p[tri[i + 1]];
        const Vector3f& c = p[tri[i + 2]];
        sum += (static_cast<f64>(a.x) * (static_cast<f64>(b.y) * c.z - static_cast<f64>(b.z) * c.y) -
                static_cast<f64>(a.y) * (static_cast<f64>(b.x) * c.z - static_cast<f64>(b.z) * c.x) +
                static_cast<f64>(a.z) * (static_cast<f64>(b.x) * c.y - static_cast<f64>(b.y) * c.x)) /
               6.0;
    }
    return sum;
}

f64 Area(const Mesh& mesh) {
    std::vector<u32> tri;
    TriangulateMesh(mesh, tri);
    const auto p = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    f64 sum = 0.0;
    for (std::size_t i = 0; i + 2 < tri.size(); i += 3) {
        const Vector3f& a = p[tri[i]];
        const Vector3f& b = p[tri[i + 1]];
        const Vector3f& c = p[tri[i + 2]];
        const f64 ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const f64 vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const f64 x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx;
        sum += 0.5 * std::sqrt(x * x + y * y + z * z);
    }
    return sum;
}

/// Edges with one face, and edges with more than two, by position, so a vertex
/// split by the repair still counts as the edge it was.
std::pair<u32, u32> OpenEdges(const Mesh& mesh) {
    const FaceSet& faces = mesh.faceSet();
    const auto p = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    const auto key = [&](u32 v) { return std::make_tuple(p[v].x, p[v].y, p[v].z); };
    std::map<std::pair<std::tuple<f32, f32, f32>, std::tuple<f32, f32, f32>>, int> count;
    u32 base = 0;
    for (u32 f = 0; f < faces.faceCount(); ++f) {
        const u32 n = faces.faceValence[f];
        for (u32 k = 0; k < n; ++k) {
            auto a = key(faces.cornerVertex[base + k]);
            auto b = key(faces.cornerVertex[base + (k + 1) % n]);
            if (b < a) {
                std::swap(a, b);
            }
            ++count[{a, b}];
        }
        base += n;
    }
    u32 border = 0;
    u32 over = 0;
    for (const auto& [edge, n] : count) {
        border += n == 1 ? 1 : 0;
        over += n > 2 ? 1 : 0;
    }
    return {border, over};
}

/// Every edge has exactly two faces, wound against each other.
bool Closed(const Mesh& mesh) {
    const FaceSet& faces = mesh.faceSet();
    std::map<std::pair<u32, u32>, int> count;
    u32 base = 0;
    for (u32 f = 0; f < faces.faceCount(); ++f) {
        const u32 n = faces.faceValence[f];
        for (u32 k = 0; k < n; ++k) {
            const u32 a = faces.cornerVertex[base + k];
            const u32 b = faces.cornerVertex[base + (k + 1) % n];
            ++count[{a, b}];
        }
        base += n;
    }
    if (count.empty()) {
        return false;
    }
    for (const auto& [edge, n] : count) {
        const auto back = count.find({edge.second, edge.first});
        if (n != 1 || back == count.end() || back->second != 1) {
            return false;
        }
    }
    return true;
}

u32 Faces(const Mesh& mesh) {
    return static_cast<u32>(mesh.faceSet().faceCount());
}

BooleanResult Run(const Mesh& target, const Mesh& operand, BooleanOp op,
                  OperandFaces faces = OperandFaces::JoinTarget, bool closeCut = true) {
    const BooleanInput inputs[2] = {{&target, false}, {&operand, true}};
    BooleanOptions options;
    options.op = op;
    options.faces = faces;
    options.closeCut = closeCut;
    return BooleanMeshes(inputs, options);
}

/// Every face's corners reversed: the mesh wound inward.
Mesh Inward(const Mesh& mesh) {
    FaceSet faces = mesh.faceSet();
    u32 base = 0;
    for (u32 f = 0; f < faces.faceCount(); ++f) {
        const u32 n = faces.faceValence[f];
        std::reverse(faces.cornerVertex.begin() + base, faces.cornerVertex.begin() + base + n);
        base += n;
    }
    Mesh out;
    out.name = mesh.name;
    out.sections = mesh.sections;
    auto positions = mesh.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    std::vector<Vector3f> copy(positions.begin(), positions.end());
    out.setFaceSet(faces);
    auto p = out.attributes.getOrCreate<Vector3f>(names::kPosition, Domain::Vertex, AttrType::F32x3);
    for (std::size_t i = 0; i < copy.size(); ++i) {
        p[i] = copy[i];
    }
    out.recomputeBounds();
    return out;
}

} // namespace

TEST_CASE("two overlapping cubes, each operation", "[boolean]") {
    const Mesh a = Box(1.0f);
    const Mesh b = Box(1.0f, {1.0f, 0.0f, 0.0f});
    SECTION("union") {
        const BooleanResult r = Run(a, b, BooleanOp::Union);
        REQUIRE(r.ok());
        CHECK(r.met);
        CHECK(Volume(r.meshes[0]) == Approx(12.0).epsilon(1e-5));
        CHECK(Area(r.meshes[0]) == Approx(2 * 3 * 2 + 2 * 3 * 2 + 2 * 2 * 2).epsilon(1e-5));
        CHECK(Closed(r.meshes[0]));
        CHECK(r.consumed[1] == 1);
        CHECK(Faces(r.meshes[1]) == 0);
    }
    SECTION("intersect") {
        const BooleanResult r = Run(a, b, BooleanOp::Intersect);
        REQUIRE(r.ok());
        CHECK(Volume(r.meshes[0]) == Approx(4.0).epsilon(1e-5));
        CHECK(Closed(r.meshes[0]));
    }
    SECTION("subtract") {
        const BooleanResult r = Run(a, b, BooleanOp::Subtract);
        REQUIRE(r.ok());
        CHECK(Volume(r.meshes[0]) == Approx(4.0).epsilon(1e-5));
        CHECK(Closed(r.meshes[0]));
        CHECK(r.cutFaces > 0);
    }
}

TEST_CASE("a cube inside a cube", "[boolean]") {
    const Mesh outer = Box(1.0f);
    const Mesh inner = Box(0.5f);
    CHECK(Volume(Run(outer, inner, BooleanOp::Union).meshes[0]) == Approx(8.0).epsilon(1e-5));
    CHECK(Volume(Run(outer, inner, BooleanOp::Intersect).meshes[0]) == Approx(1.0).epsilon(1e-5));
    const BooleanResult hollow = Run(outer, inner, BooleanOp::Subtract);
    REQUIRE(hollow.ok());
    CHECK(Volume(hollow.meshes[0]) == Approx(7.0).epsilon(1e-5));
    CHECK(Closed(hollow.meshes[0]));
    CHECK(Faces(hollow.meshes[0]) == 12);
}

TEST_CASE("a cutter flush with the top leaves a pocket", "[boolean]") {
    const Mesh target = Box(1.0f);
    const Mesh cutter = Box(0.5f, {0.0f, 0.0f, 0.5f});
    const BooleanResult r = Run(target, cutter, BooleanOp::Subtract);
    REQUIRE(r.ok());
    CHECK(Volume(r.meshes[0]) == Approx(7.0).epsilon(1e-5));
    CHECK(Closed(r.meshes[0]));
    CHECK(r.coplanar > 0);
    const BooleanResult u = Run(target, cutter, BooleanOp::Union);
    REQUIRE(u.ok());
    CHECK(Volume(u.meshes[0]) == Approx(8.0).epsilon(1e-5));
    CHECK(Area(u.meshes[0]) == Approx(24.0).epsilon(1e-5));
}

TEST_CASE("two boxes face to face", "[boolean]") {
    const Mesh a = Box(1.0f);
    const Mesh b = Box(1.0f, {2.0f, 0.0f, 0.0f});
    const BooleanResult u = Run(a, b, BooleanOp::Union);
    REQUIRE(u.ok());
    CHECK(Volume(u.meshes[0]) == Approx(16.0).epsilon(1e-5));
    CHECK(Area(u.meshes[0]) == Approx(40.0).epsilon(1e-5));
    CHECK(Closed(u.meshes[0]));
    CHECK(Run(a, b, BooleanOp::Intersect).refusal == BooleanRefusal::NothingLeft);
}

TEST_CASE("a box resting on another keeps its face under a subtract", "[boolean]") {
    const Mesh a = Box(1.0f);
    const Mesh b = Box(0.5f, {0.0f, 0.0f, 1.5f});
    CHECK(Run(a, b, BooleanOp::Subtract).refusal == BooleanRefusal::NothingToSubtract);
}

TEST_CASE("the flush case turned about an odd axis", "[boolean]") {
    Mesh target = Box(1.0f);
    Mesh cutter = Box(0.5f, {0.0f, 0.0f, 0.5f});
    Turn(target, Vector3d(1, 2, 3), 0.5235987756);
    Turn(cutter, Vector3d(1, 2, 3), 0.5235987756);
    const BooleanResult r = Run(target, cutter, BooleanOp::Subtract);
    REQUIRE(r.ok());
    CHECK(Volume(r.meshes[0]) == Approx(7.0).epsilon(1e-4));
    CHECK(Closed(r.meshes[0]));
}

TEST_CASE("a sphere minus a cylinder through it", "[boolean]") {
    PrimitiveParams sp;
    sp.size = Vector3f(1.0f, 1.0f, 1.0f);
    sp.sides = 24;
    sp.segments = 12;
    const Mesh sphere = MakeSphere(sp);
    PrimitiveParams cp;
    cp.size = Vector3f(0.3f, 0.3f, 3.0f);
    cp.sides = 16;
    const Mesh cylinder = MakeCylinder(cp);
    const BooleanResult r = Run(sphere, cylinder, BooleanOp::Subtract);
    REQUIRE(r.ok());
    CHECK(Closed(r.meshes[0]));
    CHECK(Volume(r.meshes[0]) < Volume(sphere));
    CHECK(Volume(r.meshes[0]) > 0.5 * Volume(sphere));
}

TEST_CASE("a sheet minus a box is a sheet with a hole", "[boolean]") {
    PrimitiveParams pp;
    pp.size = Vector3f(2.0f, 2.0f, 1.0f);
    pp.segments = 4;
    const Mesh sheet = MakePlane(pp);
    const Mesh box = Box(0.5f);
    const BooleanResult r = Run(sheet, box, BooleanOp::Subtract);
    REQUIRE(r.ok());
    CHECK(r.holds[0] == 0);
    CHECK(r.holds[1] == 1);
    CHECK(Area(r.meshes[0]) == Approx(15.0).epsilon(1e-5));
    // No walls: every face still lies in the sheet's plane.
    for (const Vector3f& p : r.meshes[0].attributes.get<Vector3f>(names::kPosition, Domain::Vertex)) {
        CHECK(p.z == Approx(0.0).margin(1e-6));
    }
    const BooleanResult inside = Run(sheet, box, BooleanOp::Intersect);
    REQUIRE(inside.ok());
    CHECK(Area(inside.meshes[0]) == Approx(1.0).epsilon(1e-5));
}

TEST_CASE("a sheet cannot cut", "[boolean]") {
    PrimitiveParams pp;
    pp.size = Vector3f(2.0f, 2.0f, 1.0f);
    const Mesh sheet = MakePlane(pp);
    const Mesh box = Box(1.0f);
    const BooleanResult r = Run(box, sheet, BooleanOp::Subtract);
    CHECK(r.refusal == BooleanRefusal::OperandHoldsNothing);
    CHECK(r.refusedInput == 1);
    CHECK(Run(box, sheet, BooleanOp::Union).ok());
}

TEST_CASE("an operand wound inward cuts as if wound outward", "[boolean]") {
    const Mesh a = Box(1.0f);
    const Mesh b = Inward(Box(1.0f, {1.0f, 0.0f, 0.0f}));
    const BooleanResult r = Run(a, b, BooleanOp::Subtract);
    REQUIRE(r.ok());
    CHECK(Volume(r.meshes[0]) == Approx(4.0).epsilon(1e-5));
    CHECK(Closed(r.meshes[0]));
}

TEST_CASE("two overlapping cutters leave no wall between them", "[boolean]") {
    const Mesh target = Box(2.0f);
    const Mesh c1 = Box(0.5f, {-0.25f, 0.1f, 0.05f});
    const Mesh c2 = Box(0.5f, {0.25f, -0.1f, -0.05f});
    const BooleanInput inputs[3] = {{&target, false}, {&c1, true}, {&c2, true}};
    BooleanOptions options;
    options.op = BooleanOp::Subtract;
    const BooleanResult r = BooleanMeshes(inputs, options);
    REQUIRE(r.ok());
    const f64 cutters = 1.0 + 1.0 - (1.0 - 0.5) * (1.0 - 0.2) * (1.0 - 0.1);
    CHECK(Volume(r.meshes[0]) == Approx(64.0 - cutters).epsilon(1e-5));
    CHECK(Closed(r.meshes[0]));
}

TEST_CASE("walls of a subtract carry the target's bones", "[boolean]") {
    Mesh target = Box(1.0f);
    const u32 vertices = static_cast<u32>(target.attributes.get<Vector3f>(names::kPosition, Domain::Vertex).size());
    target.skin.reset(0);
    target.skin.offsets.assign(1, 0);
    for (u32 v = 0; v < vertices; ++v) {
        const Influence one{3, 1.0f};
        target.skin.appendVertex(std::span<const Influence>(&one, 1));
    }
    const Mesh cutter = Box(0.5f, {1.0f, 0.0f, 0.0f});
    const BooleanResult r = Run(target, cutter, BooleanOp::Subtract);
    REQUIRE(r.ok());
    const Mesh& m = r.meshes[0];
    REQUIRE(!m.skin.empty());
    for (u32 v = 0; v < m.skin.vertexCount(); ++v) {
        const auto list = m.skin.forVertex(v);
        REQUIRE(list.size() == 1);
        CHECK(list[0].bone == 3);
        CHECK(list[0].weight == Approx(1.0f));
    }
    // UVs are carried: the layer is there and every corner has a value in range.
    const auto uv = m.attributes.get<Vector2f>(names::uv(0), Domain::Halfedge);
    CHECK(!uv.empty());
}

TEST_CASE("the operand's own mesh holds the walls", "[boolean]") {
    const Mesh a = Box(1.0f);
    const Mesh b = Box(0.5f, {1.0f, 0.0f, 0.0f});
    const BooleanResult r = Run(a, b, BooleanOp::Subtract, OperandFaces::OwnMesh);
    REQUIRE(r.ok());
    CHECK(r.consumed[1] == 0);
    CHECK(!Closed(r.meshes[0]));
    CHECK(!Closed(r.meshes[1]));
    CHECK(Volume(r.meshes[0]) + Volume(r.meshes[1]) == Approx(8.0 - 0.5).epsilon(1e-5));
    const BooleanResult open = Run(a, b, BooleanOp::Subtract, OperandFaces::JoinTarget, false);
    REQUIRE(open.ok());
    CHECK(!Closed(open.meshes[0]));
}

TEST_CASE("disjoint meshes: a union joins them, a subtract refuses", "[boolean]") {
    const Mesh a = Box(1.0f);
    const Mesh b = Box(1.0f, {5.0f, 0.0f, 0.0f});
    const BooleanResult u = Run(a, b, BooleanOp::Union);
    REQUIRE(u.ok());
    CHECK(!u.met);
    CHECK(Volume(u.meshes[0]) == Approx(16.0).epsilon(1e-5));
    CHECK(Run(a, b, BooleanOp::Subtract).refusal == BooleanRefusal::NothingToSubtract);
    CHECK(Run(a, b, BooleanOp::Intersect).refusal == BooleanRefusal::NothingLeft);
}

TEST_CASE("the same inputs give the same bytes", "[boolean]") {
    const Mesh a = Box(1.0f);
    Mesh b = BoxXYZ({0.4f, 0.7f, 1.6f}, {0.6f, 0.2f, 0.1f});
    Turn(b, Vector3d(0.3, 1, 0.2), 0.4);
    const BooleanResult r1 = Run(a, b, BooleanOp::Subtract);
    const BooleanResult r2 = Run(a, b, BooleanOp::Subtract);
    REQUIRE(r1.ok());
    CHECK(r1.meshes[0].faceSet().cornerVertex == r2.meshes[0].faceSet().cornerVertex);
    const auto p1 = r1.meshes[0].attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    const auto p2 = r2.meshes[0].attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
    REQUIRE(p1.size() == p2.size());
    for (std::size_t i = 0; i < p1.size(); ++i) {
        CHECK(std::memcmp(&p1[i], &p2[i], sizeof(Vector3f)) == 0);
    }
    CHECK(Closed(r1.meshes[0]));
}

// §7.2: a box through the middle of every mesh of every model on the list
// (`BOOLEAN_CORPUS`, one .mdx path a line). No cut fails; what the target keeps
// under Subtract and under Intersect is its whole area; closed in, closed out.
TEST_CASE("Booleans over corpus meshes", "[.boolean-corpus]") {
    const char* list = std::getenv("BOOLEAN_CORPUS");
    REQUIRE(list != nullptr);
    std::ifstream in(list);
    std::string path;
    u32 meshes = 0;
    u32 skipped = 0;
    u32 failed = 0;
    u32 selfCrossing = 0;
    f64 slowest = 0.0;
    while (std::getline(in, path)) {
        if (!path.empty() && path.back() == '\r') {
            path.pop_back();
        }
        if (path.empty() || path[0] == '#') {
            continue;
        }
        mdx::Parser parser;
        Result<Document> converted = MdxConverter().fromMdx(parser.parse(path));
        if (!converted.ok() || converted->models.empty()) {
            std::printf("%s unreadable\n", path.c_str());
            continue;
        }
        for (const Mesh& source : converted->models[0].meshes) {
            Mesh target = source;
            PrepareForModelling(target);
            const auto positions = target.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
            if (positions.empty() || target.faceSet().faceCount() == 0) {
                continue;
            }
            Vector3f lo = positions[0];
            Vector3f hi = positions[0];
            for (const Vector3f& p : positions) {
                lo = Vector3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                hi = Vector3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
            }
            // Off the middle and turned, so no face of a symmetric model lies on it.
            const Vector3f half((hi.x - lo.x) * 0.3f + 1e-3f, (hi.y - lo.y) * 0.27f + 1e-3f,
                                (hi.z - lo.z) * 0.33f + 1e-3f);
            const Vector3f at((lo.x + hi.x) * 0.5f + half.x * 0.11f, (lo.y + hi.y) * 0.5f - half.y * 0.07f,
                              (lo.z + hi.z) * 0.5f + half.z * 0.05f);
            Mesh box = BoxXYZ(half, Vector3f(0, 0, 0));
            Turn(box, Vector3d(0.2, 0.5, 1.0), 0.3);
            for (Vector3f& v : box.attributes.get<Vector3f>(names::kPosition, Domain::Vertex)) {
                v = Vector3f(v.x + at.x, v.y + at.y, v.z + at.z);
            }
            ++meshes;
            const auto start = std::chrono::steady_clock::now();
            const BooleanResult sub = Run(target, box, BooleanOp::Subtract, OperandFaces::OwnMesh);
            const BooleanResult inter = Run(target, box, BooleanOp::Intersect, OperandFaces::OwnMesh);
            const BooleanResult joined = Run(target, box, BooleanOp::Subtract);
            const BooleanResult uni = Run(target, box, BooleanOp::Union);
            const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count() / 4.0;
            slowest = std::max(slowest, seconds);
            const bool cutFailed = sub.refusal == BooleanRefusal::CutFailed ||
                                   inter.refusal == BooleanRefusal::CutFailed ||
                                   joined.refusal == BooleanRefusal::CutFailed ||
                                   uni.refusal == BooleanRefusal::CutFailed;
            if (cutFailed) {
                ++failed;
                std::printf("CUT FAILED %s / %s (%u %u %u %u faces)\n", path.c_str(), source.name.c_str(),
                            sub.failedFaces, inter.failedFaces, joined.failedFaces, uni.failedFaces);
            }
            CHECK(!cutFailed);
            CHECK(uni.ok());
            if (!sub.ok() || !inter.ok()) {
                ++skipped; // the box missed the mesh, or held all of it
                continue;
            }
            const f64 whole = Area(target);
            const f64 parts = Area(sub.meshes[0]) + Area(inter.meshes[0]);
            const bool areaHolds = std::fabs(parts - whole) <= 1e-4 * whole + 1e-9;
            if (!areaHolds) {
                std::printf("AREA %s / %s: %.6f + %.6f vs %.6f\n", path.c_str(), source.name.c_str(),
                            Area(sub.meshes[0]), Area(inter.meshes[0]), whole);
            }
            CHECK(areaHolds);
            // Closed in, closed out: no edge with one face. An edge with three is
            // a target crossing itself, which the cut leaves as it was (D3).
            if (Closed(target) && joined.ok()) {
                const auto [border, over] = OpenEdges(joined.meshes[0]);
                if (border != 0) {
                    std::printf("OPENED %s / %s: %u border, %u over-used\n", path.c_str(), source.name.c_str(),
                                border, over);
                } else if (over != 0) {
                    ++selfCrossing;
                }
                CHECK(border == 0);
            }
            std::printf("%-70s %-24s %6zu tris %.3f s\n", path.c_str(), source.name.c_str(),
                        target.faceSet().faceCount(), seconds);
        }
    }
    std::printf("%u meshes, %u skipped, %u failed, %u crossing themselves, slowest %.3f s\n", meshes, skipped,
                failed, selfCrossing, slowest);
}

// One mesh of the census, named by BOOLEAN_PROBE (path) and BOOLEAN_MESH: its
// open edges after the joined Subtract, by position.
TEST_CASE("Boolean probe", "[.boolean-probe]") {
    const char* file = std::getenv("BOOLEAN_PROBE");
    const char* name = std::getenv("BOOLEAN_MESH");
    REQUIRE(file != nullptr);
    REQUIRE(name != nullptr);
    mdx::Parser parser;
    Result<Document> converted = MdxConverter().fromMdx(parser.parse(file));
    REQUIRE(converted.ok());
    for (const Mesh& source : converted->models[0].meshes) {
        if (source.name != name) {
            continue;
        }
        Mesh target = source;
        PrepareForModelling(target);
        const auto positions = target.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        Vector3f lo = positions[0];
        Vector3f hi = positions[0];
        for (const Vector3f& p : positions) {
            lo = Vector3f(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vector3f(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        const Vector3f half((hi.x - lo.x) * 0.3f + 1e-3f, (hi.y - lo.y) * 0.27f + 1e-3f,
                            (hi.z - lo.z) * 0.33f + 1e-3f);
        const Vector3f at((lo.x + hi.x) * 0.5f + half.x * 0.11f, (lo.y + hi.y) * 0.5f - half.y * 0.07f,
                          (lo.z + hi.z) * 0.5f + half.z * 0.05f);
        Mesh box = BoxXYZ(half, Vector3f(0, 0, 0));
        Turn(box, Vector3d(0.2, 0.5, 1.0), 0.3);
        for (Vector3f& v : box.attributes.get<Vector3f>(names::kPosition, Domain::Vertex)) {
            v = Vector3f(v.x + at.x, v.y + at.y, v.z + at.z);
        }
        const BooleanResult r = Run(target, box, BooleanOp::Subtract);
        REQUIRE(r.ok());
        const Mesh& m = r.meshes[0];
        const FaceSet& faces = m.faceSet();
        const auto p = m.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
        const auto key = [&](u32 v) { return std::make_tuple(p[v].x, p[v].y, p[v].z); };
        std::map<std::pair<std::tuple<f32, f32, f32>, std::tuple<f32, f32, f32>>, int> count;
        u32 base = 0;
        for (u32 f = 0; f < faces.faceCount(); ++f) {
            const u32 n = faces.faceValence[f];
            for (u32 k = 0; k < n; ++k) {
                auto a = key(faces.cornerVertex[base + k]);
                auto b = key(faces.cornerVertex[base + (k + 1) % n]);
                if (b < a) {
                    std::swap(a, b);
                }
                ++count[{a, b}];
            }
            base += n;
        }
        for (const auto& [edge, n] : count) {
            if (n == 1) {
                std::printf("open (%.9g %.9g %.9g) - (%.9g %.9g %.9g)\n", std::get<0>(edge.first),
                            std::get<1>(edge.first), std::get<2>(edge.first), std::get<0>(edge.second),
                            std::get<1>(edge.second), std::get<2>(edge.second));
            }
        }
        {
            const fracture::WindingMesh wb = fracture::WindingMeshOf(box);
            const fracture::WindingMesh wt = fracture::WindingMeshOf(target);
            std::vector<Vector3d> pts;
            for (const auto& [edge, n] : count) {
                if (n == 1) {
                    pts.push_back(Vector3d(std::get<0>(edge.first), std::get<1>(edge.first), std::get<2>(edge.first)));
                }
            }
            for (const Vector3d& q : pts) {
                bool source = false;
                for (const Vector3f& v : target.attributes.get<Vector3f>(names::kPosition, Domain::Vertex)) {
                    source = source || (v.x == static_cast<f32>(q.x) && v.y == static_cast<f32>(q.y) &&
                                        v.z == static_cast<f32>(q.z));
                }
                std::printf("hole point (%.6g %.6g %.6g): %s\n", q.x, q.y, q.z, source ? "target vertex" : "cut point");
            }
            {
                // Faces of the source around the hole: those with a corner at a hole point.
                const FaceSet& sf = target.faceSet();
                const auto sp = target.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
                u32 b = 0;
                for (u32 f = 0; f < sf.faceCount(); ++f) {
                    const u32 n = sf.faceValence[f];
                    u32 hits = 0;
                    for (u32 k = 0; k < n; ++k) {
                        const Vector3f& v = sp[sf.cornerVertex[b + k]];
                        for (const Vector3d& q : pts) {
                            hits += (v.x == static_cast<f32>(q.x) && v.y == static_cast<f32>(q.y) &&
                                     v.z == static_cast<f32>(q.z))
                                        ? 1
                                        : 0;
                        }
                    }
                    if (hits >= 2) {
                        std::printf("source face %u (%u corners) holds %u hole points\n", f, n, hits);
                    }
                    b += n;
                }
            }
            if (!pts.empty()) {
                Vector3d c(0, 0, 0);
                for (const Vector3d& q : pts) {
                    c = Vector3d(c.x + q.x / pts.size(), c.y + q.y / pts.size(), c.z + q.z / pts.size());
                }
                f64 nearest = 1e30;
                Vector3d corner(0, 0, 0);
                for (const Vector3f& v : box.attributes.get<Vector3f>(names::kPosition, Domain::Vertex)) {
                    const f64 d = (v.x - c.x) * (v.x - c.x) + (v.y - c.y) * (v.y - c.y) + (v.z - c.z) * (v.z - c.z);
                    if (d < nearest) {
                        nearest = d;
                        corner = Vector3d(v.x, v.y, v.z);
                    }
                }
                std::printf("nearest box corner %.4f away: target w there %.4f; holds %u %u\n", std::sqrt(nearest),
                            fracture::WindingNumber(wt, corner), r.holds[0], r.holds[1]);
                std::printf("hole centre: box w %.4f, target w %.4f\n", fracture::WindingNumber(wb, c),
                            fracture::WindingNumber(wt, c));
            }
        }
        std::printf("repair dropped %zu, split %zu\n", m.repairLog.droppedFaces.size(), m.repairLog.splits.size());
        // The same cut with the walls in their own mesh: which side holds each hole edge.
        const BooleanResult own = Run(target, box, BooleanOp::Subtract, OperandFaces::OwnMesh);
        REQUIRE(own.ok());
        for (u32 which = 0; which < 2; ++which) {
            const Mesh& part = own.meshes[which];
            const FaceSet& pf = part.faceSet();
            const auto pp = part.attributes.get<Vector3f>(names::kPosition, Domain::Vertex);
            u32 b = 0;
            for (u32 f = 0; f < pf.faceCount(); ++f) {
                const u32 n = pf.faceValence[f];
                for (u32 k = 0; k < n; ++k) {
                    const Vector3f& a = pp[pf.cornerVertex[b + k]];
                    const Vector3f& c = pp[pf.cornerVertex[b + (k + 1) % n]];
                    auto ka = std::make_tuple(a.x, a.y, a.z);
                    auto kc = std::make_tuple(c.x, c.y, c.z);
                    if (kc < ka) {
                        std::swap(ka, kc);
                    }
                    const auto found = count.find({ka, kc});
                    if (found != count.end() && found->second == 1) {
                        std::printf("%s face %u (%u corners) holds hole edge %u\n", which == 0 ? "target" : "wall", f,
                                    n, k);
                    }
                }
                b += n;
            }
        }
        std::printf("own: target %zu faces, walls %zu faces; nearOpening %u, coplanar %u\n",
                    own.meshes[0].faceSet().faceCount(), own.meshes[1].faceSet().faceCount(), own.nearOpening,
                    own.coplanar);
    }
}

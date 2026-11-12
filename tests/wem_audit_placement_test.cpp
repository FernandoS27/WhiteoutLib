// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The Mesh workspace audit, group C: Symmetrize, Mirror, Make Planar,
/// Duplicate / Detach, the primitives, Subdivide / Smooth / Orient Outward, the
/// lattice and Boolean (EDIT_MODE_MESH_AUDIT_PLAN.md §4.C, §4.E in
/// WhiteoutFlakes). Each test is named after its suspect; a confirmed one is
/// `[!shouldfail]` until its fix, a suspected crash hidden as `[.crash]`.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "wem_audit_fixtures.h"

#include <whiteout/models/wem/geometry/boolean.h>
#include <whiteout/models/wem/geometry/lattice.h>
#include <whiteout/models/wem/geometry/primitives.h>
#include <whiteout/models/wem/geometry/refine.h>
#include <whiteout/models/wem/physics/physics.h>
#include <whiteout/models/wem/physics/references.h>
#include <whiteout/models/wem/physics/soft.h>

#include <map>
#include <tuple>

using namespace audit;
using Catch::Approx;

namespace {

// ----------------------------------------------------------------------------
// Transforms (P15)
// ----------------------------------------------------------------------------

/// A similarity: scale, then a turn about a unit axis, then a move.
struct Similarity {
    f32 scale = 1.0f;
    Vector3f axis{0.0f, 0.0f, 1.0f};
    f32 radians = 0.0f;
    Vector3f offset{0.0f, 0.0f, 0.0f};
    const char* name = "identity";

    Vector3f turn(const Vector3f& p) const {
        const f64 len = std::sqrt(f64(axis.x) * axis.x + f64(axis.y) * axis.y + f64(axis.z) * axis.z);
        const f64 kx = axis.x / len, ky = axis.y / len, kz = axis.z / len;
        const f64 c = std::cos(radians), s = std::sin(radians);
        const f64 d = kx * p.x + ky * p.y + kz * p.z;
        const f64 cx = ky * p.z - kz * p.y, cy = kz * p.x - kx * p.z, cz = kx * p.y - ky * p.x;
        return Vector3f{static_cast<f32>(p.x * c + cx * s + kx * d * (1 - c)),
                        static_cast<f32>(p.y * c + cy * s + ky * d * (1 - c)),
                        static_cast<f32>(p.z * c + cz * s + kz * d * (1 - c))};
    }
    Vector3f point(const Vector3f& p) const {
        const Vector3f t = turn(Vector3f{p.x * scale, p.y * scale, p.z * scale});
        return Vector3f{t.x + offset.x, t.y + offset.y, t.z + offset.z};
    }
};

/// P15's transforms: translate (2000, 0, 0); scale 0.01; scale 100; 37 degrees about (1, 2, 3).
std::vector<Similarity> P15Transforms() {
    std::vector<Similarity> out;
    Similarity far;
    far.offset = {2000.0f, 0.0f, 0.0f};
    far.name = "translate 2000";
    out.push_back(far);
    Similarity small;
    small.scale = 0.01f;
    small.name = "scale 0.01";
    out.push_back(small);
    Similarity large;
    large.scale = 100.0f;
    large.name = "scale 100";
    out.push_back(large);
    Similarity turned;
    turned.axis = {1.0f, 2.0f, 3.0f};
    turned.radians = 37.0f * 3.14159265358979f / 180.0f;
    turned.name = "turn 37 about (1,2,3)";
    out.push_back(turned);
    return out;
}

/// @p mesh with every position through @p t, its normals turned with it, re-shaded.
Mesh Transformed(const Mesh& mesh, const Similarity& t) {
    Mesh out = mesh;
    for (Vector3f& p : out.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex)) {
        p = t.point(p);
    }
    for (Vector3f& n : out.attributes.get<Vector3f>(geom::names::kNormal, Domain::Halfedge)) {
        n = t.turn(n);
    }
    out.recomputeBounds();
    return out;
}

// ----------------------------------------------------------------------------
// Volumes and shells (the Boolean suite's own measures)
// ----------------------------------------------------------------------------

f64 SignedVolume(const Mesh& mesh) {
    std::vector<u32> tri;
    geom::TriangulateMesh(mesh, tri);
    const auto p = mesh.attributes.get<const Vector3f>(geom::names::kPosition, Domain::Vertex);
    f64 sum = 0.0;
    for (std::size_t i = 0; i + 2 < tri.size(); i += 3) {
        const Vector3f& a = p[tri[i]];
        const Vector3f& b = p[tri[i + 1]];
        const Vector3f& c = p[tri[i + 2]];
        sum += (f64(a.x) * (f64(b.y) * c.z - f64(b.z) * c.y) - f64(a.y) * (f64(b.x) * c.z - f64(b.z) * c.x) +
                f64(a.z) * (f64(b.x) * c.y - f64(b.y) * c.x)) /
               6.0;
    }
    return sum;
}

/// Every edge has exactly two faces, wound against each other (by vertex id).
bool ClosedShell(const Mesh& mesh) {
    const geom::FaceSet& faces = mesh.faceSet();
    std::map<std::pair<u32, u32>, int> count;
    u32 base = 0;
    for (u32 f = 0; f < faces.faceCount(); ++f) {
        const u32 n = faces.faceValence[f];
        for (u32 k = 0; k < n; ++k) {
            ++count[{faces.cornerVertex[base + k], faces.cornerVertex[base + (k + 1) % n]}];
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

u32 FaceTotal(const Mesh& mesh) {
    return static_cast<u32>(mesh.faceSet().faceCount());
}

/// A `MakeBox` cube of half-extent @p half (sharp edges, uv0) moved to @p at.
Mesh Cube(f32 half, const Vector3f& at = {0, 0, 0}, const Vector3f& halves = {0, 0, 0}) {
    geom::PrimitiveParams p;
    p.size = halves.x > 0.0f ? halves : Vector3f{half, half, half};
    Mesh mesh = geom::MakeBox(p);
    for (Vector3f& v : mesh.attributes.get<Vector3f>(geom::names::kPosition, Domain::Vertex)) {
        v = Vector3f{v.x + at.x, v.y + at.y, v.z + at.z};
    }
    mesh.recomputeBounds();
    return mesh;
}

geom::BooleanResult Boolean(std::vector<std::pair<const Mesh*, bool>> inputs, geom::BooleanOp op,
                            geom::OperandFaces faces = geom::OperandFaces::JoinTarget) {
    std::vector<geom::BooleanInput> in;
    for (const auto& [mesh, operand] : inputs) {
        in.push_back(geom::BooleanInput{mesh, operand});
    }
    geom::BooleanOptions options;
    options.op = op;
    options.faces = faces;
    return geom::BooleanMeshes(in, options);
}

/// A closed prism: @p outline (counter-clockwise in XY) from z = @p low to @p high.
Mesh Prism(const std::vector<std::pair<f32, f32>>& outline, f32 low, f32 high) {
    std::vector<Vector3f> p;
    const u32 n = static_cast<u32>(outline.size());
    for (const auto& [x, y] : outline) {
        p.push_back(Vector3f{x, y, low});
    }
    for (const auto& [x, y] : outline) {
        p.push_back(Vector3f{x, y, high});
    }
    std::vector<std::vector<u32>> faces;
    std::vector<u32> bottom, top;
    for (u32 i = 0; i < n; ++i) {
        bottom.push_back(n - 1 - i);
        top.push_back(n + i);
        faces.push_back({i, (i + 1) % n, n + (i + 1) % n, n + i});
    }
    faces.push_back(bottom);
    faces.push_back(top);
    return FromFaces(faces, p);
}

} // namespace

// ============================================================================
// §4.E Boolean
// ============================================================================

TEST_CASE("audit BOO-13: a union whose every input is empty is refused and does not crash",
          "[wem][audit][boolean]") {
    Mesh empty;
    empty.setFaceSet(geom::FaceSet{});
    const geom::BooleanResult r =
        Boolean({{&empty, false}, {&empty, true}}, geom::BooleanOp::Union);
    INFO(geom::ToString(r.refusal));
    CHECK_FALSE(r.ok());
}

TEST_CASE("audit BOO-1: Intersect into the operand's own mesh with the operand inside keeps the operand",
          "[wem][audit][boolean]") {
    // Max: the intersection is the inner box; the target keeps nothing.
    const Mesh outer = Cube(1.0f);
    const Mesh inner = Cube(0.5f);
    const geom::BooleanResult r =
        Boolean({{&outer, false}, {&inner, true}}, geom::BooleanOp::Intersect, geom::OperandFaces::OwnMesh);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    CHECK(SignedVolume(r.meshes[1]) == Approx(1.0).epsilon(1e-5));
    CHECK(FaceTotal(r.meshes[0]) == 0u);
}

TEST_CASE("audit BOO-1 case: Union into the operand's own mesh with the operand inside drops the operand",
          "[wem][audit][boolean]") {
    const Mesh outer = Cube(1.0f);
    const Mesh inner = Cube(0.5f);
    const geom::BooleanResult r =
        Boolean({{&outer, false}, {&inner, true}}, geom::BooleanOp::Union, geom::OperandFaces::OwnMesh);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    CHECK(SignedVolume(r.meshes[0]) == Approx(8.0).epsilon(1e-5));
    CHECK(ClosedShell(r.meshes[0]));
    CHECK(FaceTotal(r.meshes[1]) == 0u);
    CHECK(r.consumed[1] == 1u);
}

TEST_CASE("audit BOO-2: a Union joined into the target maps the operand's sections into the target's table",
          "[wem][audit][boolean]") {
    // The operand has two sections (its +X, +Y, -X sides in section 1); the
    // target has one. Every joined face must name a section the result has.
    const Mesh target = Cube(1.0f);
    Mesh operand = Cube(1.0f, {1.0f, 0.0f, 0.0f});
    operand.sections.push_back(operand.sections.front());
    operand.sections.back().materialSlot = 7;
    {
        const std::span<u32> sections = operand.faceSections();
        for (u32 f = 0; f < sections.size(); ++f) {
            sections[f] = f >= 3 ? 1u : 0u;
        }
    }
    const geom::BooleanResult r = Boolean({{&target, false}, {&operand, true}}, geom::BooleanOp::Union);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    const Mesh& out = r.meshes[0];
    u32 outOfRange = 0;
    for (const u32 s : out.faceSections()) {
        outOfRange += s >= out.sections.size() ? 1u : 0u;
    }
    INFO("sections in the result " << out.sections.size());
    CHECK(outOfRange == 0u);
}

TEST_CASE("audit BOO-2 case: a Subtract joined into the target puts a two-section operand's walls in range",
          "[wem][audit][boolean]") {
    const Mesh target = Cube(1.0f);
    Mesh operand = Cube(0.5f, {1.0f, 0.0f, 0.0f});
    operand.sections.push_back(operand.sections.front());
    {
        const std::span<u32> sections = operand.faceSections();
        for (u32 f = 0; f < sections.size(); ++f) {
            sections[f] = f >= 3 ? 1u : 0u;
        }
    }
    const geom::BooleanResult r = Boolean({{&target, false}, {&operand, true}}, geom::BooleanOp::Subtract);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    const Mesh& out = r.meshes[0];
    for (const u32 s : out.faceSections()) {
        CHECK(s < out.sections.size());
    }
    CHECK(SignedVolume(out) == Approx(7.5).epsilon(1e-5));
    CHECK(ClosedShell(out));
}

TEST_CASE("audit BOO-5: joined faces get usable normals, tangents and colours where only the target has them",
          "[wem][audit][boolean]") {
    Mesh target = Cube(1.0f);
    {
        for (auto& c : target.attributes.getOrCreate<std::array<u8, 4>>(geom::names::color(0), Domain::Halfedge,
                                                                       geom::AttrType::U8x4)) {
            c = {200, 100, 50, 255};
        }
        for (Vector4f& t :
             target.attributes.getOrCreate<Vector4f>(geom::names::kTangent, Domain::Halfedge, geom::AttrType::F32x4)) {
            t = {1.0f, 0.0f, 0.0f, 1.0f};
        }
    }
    // The operand: positions only, no normals, colours or tangents.
    Mesh operand = audit::Box();
    operand.attributes.remove(geom::names::kNormal, Domain::Halfedge);
    for (Vector3f& p : Positions(operand)) {
        p = Vector3f{p.x + 0.5f, p.y + 0.25f, p.z + 0.125f};
    }
    operand.recomputeBounds();
    const geom::BooleanResult r = Boolean({{&target, false}, {&operand, true}}, geom::BooleanOp::Union);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    const Mesh& out = r.meshes[0];
    const Topology& t = out.topology();
    const auto color = out.attributes.get<const std::array<u8, 4>>(geom::names::color(0), Domain::Halfedge);
    const auto tangent = out.attributes.get<const Vector4f>(geom::names::kTangent, Domain::Halfedge);
    u32 transparent = 0, flat = 0, corners = 0;
    for (u32 f = 0; f < t.faceCount(); ++f) {
        for (const HalfedgeId h : t.fh(FaceId(f))) {
            ++corners;
            transparent += h.index() < color.size() && color[h.index()][3] == 0 ? 1u : 0u;
            if (h.index() < tangent.size()) {
                const Vector4f& v = tangent[h.index()];
                flat += std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) < 0.5f ? 1u : 0u;
            }
        }
    }
    INFO(corners << " corners: " << transparent << " with alpha 0, " << flat << " with a zero tangent");
    CHECK(CheckFinite(out) == "");
    CHECK(transparent == 0u);
    CHECK(flat == 0u);
}

TEST_CASE("audit BOO-6: a Boolean renumbers cloth.bind.vertex with the vertices it names",
          "[wem][audit][boolean]") {
    // Each vertex names itself; after the cut, every corner of the target that
    // survives must still name a vertex at its own place.
    Mesh target = Cube(1.0f);
    {
        const auto bind = target.attributes.getOrCreate<std::array<u32, 4>>(geom::names::kClothBindVertex,
                                                                           Domain::Vertex, geom::AttrType::U32x4);
        for (u32 v = 0; v < bind.size(); ++v) {
            bind[v] = {v, kInvalidId, kInvalidId, kInvalidId};
        }
    }
    const Mesh cutter = Cube(0.5f, {1.0f, 0.0f, 0.0f});
    const geom::BooleanResult r = Boolean({{&target, false}, {&cutter, true}}, geom::BooleanOp::Subtract);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    const Mesh& out = r.meshes[0];
    const auto bind = out.attributes.get<const std::array<u32, 4>>(geom::names::kClothBindVertex, Domain::Vertex);
    const auto p = Positions(out);
    REQUIRE(bind.size() == p.size());
    u32 corners = 0, wrong = 0;
    for (u32 v = 0; v < p.size(); ++v) {
        if (std::abs(std::abs(p[v].x) - 1.0f) > 1e-6f || std::abs(std::abs(p[v].y) - 1.0f) > 1e-6f ||
            std::abs(std::abs(p[v].z) - 1.0f) > 1e-6f) {
            continue; // not one of the cube's own corners
        }
        ++corners;
        const u32 named = bind[v][0];
        wrong += named >= p.size() || Length(p[named] - p[v]) > 1e-6f ? 1u : 0u;
    }
    INFO(corners << " cube corners kept, " << wrong << " naming another place");
    CHECK(corners > 0u);
    CHECK(wrong == 0u);
}

TEST_CASE("audit BOO-6: a Boolean keeps the target's sc2Bounds and repairLog", "[wem][audit][boolean]") {
    Mesh target = Cube(1.0f);
    target.sc2Bounds.push_back(native::M3MeshSection{});
    target.sc2Bounds.back().nodeIndex = 3;
    target.repairLog.splits.push_back(geom::VertexSplit{0, 7});
    const Mesh cutter = Cube(0.5f, {1.0f, 0.0f, 0.0f});
    const geom::BooleanResult r = Boolean({{&target, false}, {&cutter, true}}, geom::BooleanOp::Subtract);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    CHECK(r.meshes[0].sc2Bounds.size() == 1u);
    CHECK_FALSE(r.meshes[0].repairLog.empty());
}

TEST_CASE("audit BOO-7: one operand's walls from two targets are one mesh with the operand's name, LOD and sections",
          "[wem][audit][boolean]") {
    const Mesh a = Cube(1.0f);
    const Mesh b = Cube(1.0f, {3.0f, 0.0f, 0.0f});
    Mesh cutter = Cube(0.0f, {1.5f, 0.0f, 0.0f}, {3.0f, 0.5f, 0.5f});
    cutter.name = "cutter";
    cutter.lodLevel = 1;
    const geom::BooleanResult r = Boolean({{&a, false}, {&b, false}, {&cutter, true}}, geom::BooleanOp::Subtract,
                                          geom::OperandFaces::OwnMesh);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    const Mesh& walls = r.meshes[2];
    CHECK(FaceTotal(walls) > 0u);
    INFO("walls: name '" << walls.name << "', LOD " << walls.lodLevel << ", " << walls.sections.size() << " sections");
    CHECK(walls.name == "cutter");
    CHECK(walls.lodLevel == 1u);
    CHECK(walls.sections.size() == 1u);
}

TEST_CASE("audit BOO-14 / P15: a Subtract is the same under a move, a scale and a turn", "[wem][audit][boolean]") {
    const Mesh a = Cube(1.0f);
    const Mesh b = Cube(0.5f, {1.0f, 0.25f, 0.1f});
    const geom::BooleanResult base = Boolean({{&a, false}, {&b, true}}, geom::BooleanOp::Subtract);
    REQUIRE(base.ok());
    const f64 volume = SignedVolume(base.meshes[0]);
    CHECK(volume == Approx(8.0 - 0.5 * 1.0 * 1.0).epsilon(1e-5));
    for (const Similarity& t : P15Transforms()) {
        INFO(t.name);
        const Mesh ta = Transformed(a, t);
        const Mesh tb = Transformed(b, t);
        const geom::BooleanResult r = Boolean({{&ta, false}, {&tb, true}}, geom::BooleanOp::Subtract);
        INFO(geom::ToString(r.refusal));
        REQUIRE(r.ok());
        const f64 scale3 = f64(t.scale) * t.scale * t.scale;
        INFO("volume " << SignedVolume(r.meshes[0]) / scale3 << " vs " << volume);
        CHECK(SignedVolume(r.meshes[0]) / scale3 == Approx(volume).epsilon(1e-3));
        CHECK(ClosedShell(r.meshes[0]));
        CHECK(CheckStructure(r.meshes[0]) == "");
        CHECK(FaceTotal(r.meshes[0]) == FaceTotal(base.meshes[0]));
    }
}

TEST_CASE("audit BOO case: identical meshes", "[wem][audit][boolean]") {
    const Mesh a = Cube(1.0f);
    const Mesh b = Cube(1.0f);
    const geom::BooleanResult u = Boolean({{&a, false}, {&b, true}}, geom::BooleanOp::Union);
    INFO("union " << geom::ToString(u.refusal));
    REQUIRE(u.ok());
    CHECK(SignedVolume(u.meshes[0]) == Approx(8.0).epsilon(1e-5));
    CHECK(ClosedShell(u.meshes[0]));
    const geom::BooleanResult i = Boolean({{&a, false}, {&b, true}}, geom::BooleanOp::Intersect);
    INFO("intersect " << geom::ToString(i.refusal));
    REQUIRE(i.ok());
    CHECK(SignedVolume(i.meshes[0]) == Approx(8.0).epsilon(1e-5));
    CHECK(ClosedShell(i.meshes[0]));
    // A minus itself leaves nothing: a refusal, not an empty mesh.
    const geom::BooleanResult s = Boolean({{&a, false}, {&b, true}}, geom::BooleanOp::Subtract);
    INFO("subtract " << geom::ToString(s.refusal));
    CHECK(s.refusal == geom::BooleanRefusal::NothingLeft);
}

TEST_CASE("audit BOO case: equal boxes face to face, under a Subtract", "[wem][audit][boolean]") {
    const Mesh a = Cube(1.0f);
    const Mesh b = Cube(1.0f, {2.0f, 0.0f, 0.0f});
    const geom::BooleanResult s = Boolean({{&a, false}, {&b, true}}, geom::BooleanOp::Subtract);
    INFO(geom::ToString(s.refusal));
    CHECK(s.refusal == geom::BooleanRefusal::NothingToSubtract);
}

namespace {

/// A Union of the unit-half cube with another at @p at, which touches it only
/// along an edge or at a corner.
void TouchingUnion(const Vector3f& at) {
    const Mesh a = Cube(1.0f);
    const Mesh b = Cube(1.0f, at);
    const geom::BooleanResult u = Boolean({{&a, false}, {&b, true}}, geom::BooleanOp::Union);
    INFO(geom::ToString(u.refusal));
    REQUIRE(u.ok());
    CHECK(SignedVolume(u.meshes[0]) == Approx(16.0).epsilon(1e-5));
    CHECK(CheckStructure(u.meshes[0]) == "");
    INFO("faces " << FaceTotal(u.meshes[0]));
    CHECK(FaceTotal(u.meshes[0]) == 12u); // nothing crosses, so nothing is cut
    const geom::BooleanResult s = Boolean({{&a, false}, {&b, true}}, geom::BooleanOp::Subtract);
    CHECK(s.refusal == geom::BooleanRefusal::NothingToSubtract);
}

} // namespace

TEST_CASE("audit BOO-16: boxes touching along an edge union to both with no face cut",
          "[wem][audit][boolean]") {
    TouchingUnion(Vector3f{2.0f, 2.0f, 0.0f});
}

TEST_CASE("audit BOO case: boxes touching at a corner union to both with no face cut", "[wem][audit][boolean]") {
    TouchingUnion(Vector3f{2.0f, 2.0f, 2.0f});
}

TEST_CASE("audit BOO case: two targets and a Union joined into the first", "[wem][audit][boolean]") {
    const Mesh a = Cube(1.0f);
    const Mesh b = Cube(1.0f, {3.0f, 0.0f, 0.0f});
    const Mesh bar = Cube(0.0f, {1.5f, 0.0f, 0.0f}, {2.5f, 0.5f, 0.5f});
    const geom::BooleanResult r = Boolean({{&a, false}, {&b, false}, {&bar, true}}, geom::BooleanOp::Union);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    CHECK(SignedVolume(r.meshes[0]) == Approx(8.0 + 8.0 + 5.0 - 2.0 - 2.0).epsilon(1e-5));
    CHECK(ClosedShell(r.meshes[0]));
    CHECK(CheckStructure(r.meshes[0]) == "");
    CHECK(r.consumed[1] == 1u);
    CHECK(r.consumed[2] == 1u);
}

TEST_CASE("audit BOO-15: an unskinned operand joined into a skinned target leaves no vertex unbound",
          "[wem][audit][boolean]") {
    // P6: a skinned mesh's every vertex has weights summing to 1. The operand
    // had no skin, so its vertices need one from somewhere (the target's, as a
    // Subtract's walls take it, D6).
    Mesh target = Cube(1.0f);
    target.skin.reset(target.vertexCount());
    for (u32 v = 0; v < target.vertexCount(); ++v) {
        const geom::Influence one{2, 1.0f};
        target.skin.assignVertex(v, std::span<const geom::Influence>(&one, 1));
    }
    const Mesh operand = Cube(0.5f, {1.0f, 0.0f, 0.0f});
    const geom::BooleanResult r = Boolean({{&target, false}, {&operand, true}}, geom::BooleanOp::Union);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    const Mesh& out = r.meshes[0];
    REQUIRE_FALSE(out.skin.empty());
    u32 unbound = 0;
    for (u32 v = 0; v < out.vertexCount(); ++v) {
        f32 sum = 0.0f;
        for (const geom::Influence& in : out.skin.forVertex(v)) {
            sum += in.weight;
        }
        unbound += std::abs(sum - 1.0f) > 1e-3f ? 1u : 0u;
    }
    INFO(unbound << " of " << out.vertexCount() << " vertices without weights summing to 1");
    CHECK(unbound == 0u);
}

TEST_CASE("audit BOO case: a cutter through a face with collinear corners", "[wem][audit][boolean]") {
    // The unit cube with an extra corner (0.5, 0, 1) on its top-front edge:
    // both faces round it hold three collinear corners.
    const Mesh target = FromFaces({{0, 3, 2, 1}, {4, 8, 5, 6, 7}, {0, 1, 5, 8, 4}, {1, 2, 6, 5}, {2, 3, 7, 6},
                                   {3, 0, 4, 7}},
                                  {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1},
                                   {0, 1, 1}, {0.5f, 0, 1}});
    const Mesh cutter = Cube(0.25f, {0.5f, 0.0f, 1.0f});
    const geom::BooleanResult r = Boolean({{&target, false}, {&cutter, true}}, geom::BooleanOp::Subtract);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    CHECK(SignedVolume(r.meshes[0]) == Approx(1.0 - 0.5 * 0.25 * 0.25).epsilon(1e-5));
    CHECK(ClosedShell(r.meshes[0]));
    CHECK(CheckStructure(r.meshes[0]) == "");
}

TEST_CASE("audit BOO case: a cutter through non-planar quads keeps A - B + A n B = A", "[wem][audit][boolean]") {
    Mesh target = audit::Box();
    Positions(target)[6].z = 1.2f; // (1, 1, 1) lifted: three quads bend
    Shade(target);
    target.recomputeBounds();
    const Mesh cutter = Cube(0.25f, {0.75f, 0.75f, 1.0f});
    const geom::BooleanResult s = Boolean({{&target, false}, {&cutter, true}}, geom::BooleanOp::Subtract);
    const geom::BooleanResult i = Boolean({{&target, false}, {&cutter, true}}, geom::BooleanOp::Intersect);
    INFO(geom::ToString(s.refusal) << " / " << geom::ToString(i.refusal));
    REQUIRE(s.ok());
    REQUIRE(i.ok());
    CHECK(SignedVolume(s.meshes[0]) + SignedVolume(i.meshes[0]) == Approx(SignedVolume(target)).epsilon(1e-5));
    CHECK(ClosedShell(s.meshes[0]));
    CHECK(ClosedShell(i.meshes[0]));
}

TEST_CASE("audit BOO case: an L-shaped cutter through a box", "[wem][audit][boolean]") {
    const Mesh target = Cube(2.0f);
    const Mesh cutter = Prism({{-1, -1}, {1, -1}, {1, 0}, {0, 0}, {0, 1}, {-1, 1}}, -3.0f, 3.0f);
    const geom::BooleanResult r = Boolean({{&target, false}, {&cutter, true}}, geom::BooleanOp::Subtract);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    CHECK(SignedVolume(r.meshes[0]) == Approx(64.0 - 3.0 * 4.0).epsilon(1e-5));
    CHECK(ClosedShell(r.meshes[0]));
    CHECK(CheckStructure(r.meshes[0]) == "");
}

TEST_CASE("audit BOO case: a tube cutter through a box", "[wem][audit][boolean]") {
    geom::LatheParams lathe;
    lathe.profile = {{0.5f, -3.0f}, {1.0f, -3.0f}, {1.0f, 3.0f}, {0.5f, 3.0f}, {0.5f, -3.0f}};
    lathe.sides = 16;
    const Mesh tube = geom::MakeLathe(lathe);
    REQUIRE(FaceTotal(tube) == 64u);
    const Mesh target = Cube(2.0f);
    const geom::BooleanResult r = Boolean({{&target, false}, {&tube, true}}, geom::BooleanOp::Subtract);
    INFO(geom::ToString(r.refusal));
    REQUIRE(r.ok());
    CHECK(r.holds[1] == 1u);
    const f64 ring = 0.5 * 16.0 * std::sin(6.283185307179586 / 16.0) * (1.0 - 0.25);
    CHECK(SignedVolume(r.meshes[0]) == Approx(64.0 - ring * 4.0).epsilon(1e-5));
    CHECK(ClosedShell(r.meshes[0]));
    CHECK(CheckStructure(r.meshes[0]) == "");
}

// ============================================================================
// §4.C Symmetrize, Mirror, Duplicate, Detach
// ============================================================================

namespace {

/// The largest distance from a live vertex of either mesh to the nearest live
/// vertex of the other: 0 when the two hold the same points.
f32 MaxPointGap(const Mesh& a, const Mesh& b) {
    const auto gap = [](const Mesh& from, const Mesh& to) {
        const auto p = Positions(from);
        const auto q = Positions(to);
        f32 worst = 0.0f;
        for (u32 v = 0; v < p.size(); ++v) {
            if (from.topology().isDeleted(VertexId(v))) {
                continue;
            }
            f32 best = 1e30f;
            for (u32 w = 0; w < q.size(); ++w) {
                if (!to.topology().isDeleted(VertexId(w))) {
                    best = std::min(best, Length(p[v] - q[w]));
                }
            }
            worst = std::max(worst, best);
        }
        return worst;
    };
    return std::max(gap(a, b), gap(b, a));
}

/// QuadGrid(@p w, @p h) moved by (@p dx, @p dy), z from @p height, re-shaded.
Mesh Sheet(u32 w, u32 h, f32 dx, f32 dy, const std::function<f32(f32, f32)>& height) {
    Mesh mesh = QuadGrid(w, h);
    for (Vector3f& p : Positions(mesh)) {
        p = Vector3f{p.x + dx, p.y + dy, 0.0f};
        p.z = height(p.x, p.y);
    }
    Shade(mesh);
    mesh.recomputeBounds();
    return mesh;
}

std::function<ModelPlan(Mesh&, const PointTable&)> Symmetrize(const geom::SymmetrizeParams& params,
                                                               std::vector<u32> bones = {}) {
    return [params, bones](Mesh& m, const PointTable& p) { return geom::PlanSymmetrize(m, p, params, bones); };
}

geom::SymmetrizeParams AcrossX() {
    geom::SymmetrizeParams params;
    params.axis = 0;
    params.fromPositive = true;
    return params;
}

/// The bone a face's corners are bound to: the heaviest influence of its
/// corner farthest from x = 0 (off the seam), else its section's rigid node,
/// else kInvalidId.
u32 BoneOfFace(const Mesh& mesh, u32 f) {
    const Topology& t = mesh.topology();
    const auto p = Positions(mesh);
    u32 v = kInvalidId;
    for (const HalfedgeId h : t.fh(FaceId(f))) {
        const u32 w = t.from(h).value();
        if (v == kInvalidId || std::abs(p[w].x) > std::abs(p[v].x)) {
            v = w;
        }
    }
    if (!mesh.skin.empty() && v < mesh.skin.vertexCount() && !mesh.skin.forVertex(v).empty()) {
        return mesh.skin.forVertex(v)[0].bone;
    }
    const auto sections = mesh.faceSections();
    const u32 s = f < sections.size() ? sections[f] : 0u;
    return s < mesh.sections.size() && mesh.sections[s].rigidNode ? *mesh.sections[s].rigidNode : kInvalidId;
}

Vector3f FaceCentre(const Mesh& mesh, u32 f) {
    const Topology& t = mesh.topology();
    const auto p = Positions(mesh);
    Vector3f c{0, 0, 0};
    u32 n = 0;
    for (const HalfedgeId h : t.fh(FaceId(f))) {
        c = c + p[t.from(h).value()];
        ++n;
    }
    return c * (1.0f / static_cast<f32>(n));
}

} // namespace

TEST_CASE("audit SYM-1: a box with a face on the plane symmetrizes to the doubled box",
          "[wem][audit][symmetrize]") {
    // x in [0, 1], its x = 0 side on the plane. Max's Symmetry: a closed box
    // from -1 to 1, the side that lay on the plane gone inside.
    const Mesh mesh = audit::Box();
    Mesh out;
    const Verdict verdict = Run(mesh, Symmetrize(AcrossX()), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 10u);
    CHECK(SignedVolume(out) == Approx(2.0).epsilon(1e-5));
}

TEST_CASE("audit SYM-1: a flat cape lying in the plane is left as it is", "[wem][audit][symmetrize]") {
    Mesh mesh = QuadGrid(2, 2);
    for (Vector3f& p : Positions(mesh)) {
        p = Vector3f{0.0f, p.x, p.y};
    }
    Shade(mesh);
    Mesh out;
    const Verdict verdict = Run(mesh, Symmetrize(AcrossX()), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    CHECK(verdict.refusal != ModelRefusal::WouldFold);
    if (!verdict.refused) {
        CHECK(verdict.failed == "");
        CHECK(LiveFaces(out) == 4u);
    }
}

TEST_CASE("audit SYM case: a half roof whose ridge is on the plane becomes the roof", "[wem][audit][symmetrize]") {
    const Mesh mesh = Sheet(2, 2, 0.0f, 0.0f, [](f32 x, f32) { return -x; });
    Mesh out;
    const Verdict verdict = Run(mesh, Symmetrize(AcrossX()), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 8u);
    CHECK(LiveVertices(out) == 15u); // the ridge's three are shared
}

TEST_CASE("audit SYM-11: a pyramid whose apex is on the plane is made, or refused as a pinch",
          "[wem][audit][symmetrize]") {
    // Two pyramids meeting at the apex: Max makes the bowtie; WEM forbids a
    // vertex with two fans, so the alternative is a refusal that says so (Q6).
    const Mesh mesh = FromFaces({{1, 2, 3, 4}, {0, 2, 1}, {0, 3, 2}, {0, 4, 3}, {0, 1, 4}},
                                {{0, 0, 0}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}});
    REQUIRE(SignedVolume(mesh) > 0.0);
    Mesh out;
    const Verdict verdict = Run(mesh, Symmetrize(AcrossX()), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    if (verdict.refused) {
        CHECK(verdict.refusal == ModelRefusal::WouldPinch);
    } else {
        CHECK(verdict.failed == "");
        CHECK(LiveFaces(out) == 10u);
    }
}

TEST_CASE("audit SYM-2: a half cylinder whose border sits 0.01 off the plane is welded at the seam",
          "[wem][audit][symmetrize]") {
    // Max's Symmetry welds the seam within a threshold the user sees (0.1 by
    // default), so no border is left along y = 0.
    std::vector<Vector3f> p;
    for (u32 r = 0; r <= 1; ++r) {
        for (u32 k = 0; k <= 6; ++k) {
            const f32 a = 3.14159265f * static_cast<f32>(k) / 6.0f;
            p.push_back(Vector3f{std::cos(a), std::sin(a) + 0.01f, static_cast<f32>(r)});
        }
    }
    std::vector<std::vector<u32>> faces;
    for (u32 k = 0; k < 6; ++k) {
        faces.push_back({k, k + 1, 7 + k + 1, 7 + k});
    }
    const Mesh mesh = FromFaces(faces, p);
    geom::SymmetrizeParams params;
    params.axis = 1;
    Mesh out;
    const Verdict verdict = Run(mesh, Symmetrize(params), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    const auto q = Positions(std::as_const(out));
    u32 seam = 0;
    for (const u32 e : BorderEdges(out).edges) {
        const HalfedgeId h = Topology::halfedge(EdgeId(e), 0);
        seam += std::abs(q[out.topology().from(h).value()].y) < 0.05f &&
                        std::abs(q[out.topology().to(h).value()].y) < 0.05f
                    ? 1u
                    : 0u;
    }
    INFO(seam << " border edges left along the seam");
    CHECK(seam == 0u);
}

TEST_CASE("audit SYM-4: the seam of a smooth surface is re-shaded across both halves",
          "[wem][audit][symmetrize]") {
    // A soft sheet curving down for x > 0, flat for x < 0: symmetrized from +,
    // the seam's normals must be the fresh ones, square to it.
    const Mesh mesh = Sheet(4, 2, -2.0f, 0.0f, [](f32 x, f32) { return x > 0.0f ? -0.2f * x * x : 0.0f; });
    Mesh out;
    const Verdict verdict = Run(mesh, Symmetrize(AcrossX()), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << " staleness " << NormalStaleness(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(NormalStaleness(out) < 1.0f);
}

TEST_CASE("audit SYM-5: a rigid section's mirrored half binds to the mirrored bone",
          "[wem][audit][symmetrize]") {
    Mesh mesh = Sheet(2, 1, -1.0f, 0.0f, [](f32, f32) { return 0.0f; });
    mesh.sections[0].rigidNode = 1u;
    const std::vector<u32> mirror = {0u, 2u, 1u};
    Mesh out;
    const Verdict verdict = Run(mesh, Symmetrize(AcrossX(), mirror), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    u32 copies = 0, wrong = 0;
    for (const u32 f : AllFaces(out).faces) {
        if (FaceCentre(out, f).x < 0.0f) {
            ++copies;
            wrong += BoneOfFace(out, f) != 2u ? 1u : 0u;
        } else {
            CHECK(BoneOfFace(out, f) == 1u);
        }
    }
    INFO(copies << " mirrored faces, " << wrong << " still on bone 1");
    CHECK(copies == 1u);
    CHECK(wrong == 0u);
}

TEST_CASE("audit SYM-5 case: a skinned half maps its paired bones and counts the unpaired",
          "[wem][audit][symmetrize]") {
    Mesh mesh = Sheet(2, 1, -1.0f, 0.0f, [](f32, f32) { return 0.0f; });
    mesh.skin.reset(mesh.vertexCount());
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        const geom::Influence two[2] = {{1, 0.75f}, {3, 0.25f}}; // 1 has a mirror (2); 3 has none
        mesh.skin.assignVertex(v, two);
    }
    const std::vector<u32> mirror = {kInvalidId, 2u, 1u, kInvalidId};
    Mesh out;
    ModelPlan plan;
    const Verdict verdict = Run(
        mesh,
        [&](Mesh& m, const PointTable& p) {
            plan = geom::PlanSymmetrize(m, p, AcrossX(), mirror);
            return plan;
        },
        {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(plan.kept == 2u); // the two copied vertices each kept bone 3
    for (const u32 f : AllFaces(out).faces) {
        CHECK(BoneOfFace(out, f) == (FaceCentre(out, f).x < 0.0f ? 2u : 1u));
    }
}

TEST_CASE("audit SYM-8: Symmetrize hardens no edge past the crease angle the user set",
          "[wem][audit][symmetrize]") {
    // One twisted quad across x = 0: the slice's edge has an 84 degree bend.
    // With the user's crease at 170 nothing may turn sharp; the inner slice
    // and delete finish at the 60 degree default.
    const Mesh mesh = FromFaces({{0, 1, 2, 3}}, {{-1, 0, 0}, {1, 0, 0}, {1, 1, 4}, {-1, 1, -4}});
    REQUIRE(SharpCount(mesh) == 0u);
    Mesh out;
    const Verdict verdict = Run(mesh, Symmetrize(AcrossX()), {}, &out, 170.0f * 3.14159265f / 180.0f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    INFO(SharpCount(out) << " sharp edges");
    CHECK(SharpCount(out) == 0u);
}

TEST_CASE("audit SYM-10: a refused Symmetrize leaves the mesh as it was", "[wem][audit][symmetrize]") {
    // §3.1: a refused plan leaves the mesh untouched. Compared against the
    // fixture as a finished tool leaves it (rows stored), so only the plan's
    // own writes count. The box with a face on the plane is made now (SYM-1),
    // so the box is moved to the far side, its +X face on the plane, which goes
    // inside with the rest: nothing is left to copy (NoSourceSide).
    Mesh finished = audit::Box();
    for (Vector3f& p : Positions(finished)) {
        p.x -= 1.0f;
    }
    finished.recomputeBounds();
    {
        ModelPlan settle;
        geom::FinishTool(finished, settle);
    }
    Mesh work = finished;
    const ModelPlan plan = geom::PlanSymmetrize(work, PointsOf(work), AcrossX());
    INFO(geom::ToString(plan.refusal) << ": " << LiveFaces(finished) << " faces before, " << LiveFaces(work)
                                      << " after");
    REQUIRE(plan.refused());
    CHECK(plan.refusal == ModelRefusal::NoSourceSide);
    CHECK(SameReflected(work, finished));
}

TEST_CASE("audit DUP-10: a refused Duplicate leaves the mesh as it was", "[wem][audit][duplicate]") {
    // The open fan's faces 0 and 2 are made now (DUP-2: a copy per fan), so the
    // refusal left is a selection naming no face (EmptySelection).
    Mesh finished = OpenFan();
    {
        ModelPlan settle;
        geom::FinishTool(finished, settle);
    }
    Mesh work = finished;
    const ModelPlan plan = geom::PlanDuplicate(work, Faces({99}));
    INFO(geom::ToString(plan.refusal) << ": " << LiveFaces(finished) << " faces before, " << LiveFaces(work)
                                      << " after");
    REQUIRE(plan.refused());
    CHECK(SameReflected(work, finished));
}

TEST_CASE("audit SYM case: a mesh wholly on the far side is refused NoSourceSide", "[wem][audit][symmetrize]") {
    const Mesh mesh = Sheet(2, 2, -3.0f, 0.0f, [](f32, f32) { return 0.0f; });
    const Verdict verdict = Run(mesh, Symmetrize(AcrossX()));
    CHECK(verdict.refusal == ModelRefusal::NoSourceSide);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit SYM P15: Symmetrize is the same under a move, a scale and a turn", "[wem][audit][symmetrize]") {
    const Mesh mesh =
        Sheet(4, 2, -2.0f, 0.0f, [](f32 x, f32 y) { return 0.1f * x * y + (x > 0.0f ? 0.2f * x : 0.0f); });
    geom::SymmetrizeParams plane = AcrossX();
    plane.origin = {0.0f, 0.0f, 0.0f};
    plane.normal = {1.0f, 0.0f, 0.0f};
    Mesh base;
    REQUIRE_FALSE(Run(mesh, Symmetrize(plane), {}, &base).refused);
    for (const Similarity& t : P15Transforms()) {
        INFO(t.name);
        geom::SymmetrizeParams moved = plane;
        moved.origin = t.point(plane.origin);
        moved.normal = t.turn(plane.normal);
        Mesh out;
        const Verdict verdict = Run(Transformed(mesh, t), Symmetrize(moved), {}, &out);
        INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(LiveFaces(out) == LiveFaces(base));
        CHECK(LiveVertices(out) == LiveVertices(base));
        const f32 gap = MaxPointGap(out, Transformed(base, t)) / t.scale;
        INFO("gap " << gap);
        CHECK(gap < 1e-3f);
    }
}

TEST_CASE("audit MIR: Mirror keeps winding, normals, tangents, and maps skin and rigid node",
          "[wem][audit][mirror]") {
    Mesh mesh = audit::Box();
    Fields(mesh, 1.0f);
    for (Vector4f& t :
         mesh.attributes.getOrCreate<Vector4f>(geom::names::kTangent, Domain::Halfedge, geom::AttrType::F32x4)) {
        t = {0.0f, 1.0f, 0.0f, 1.0f};
    }
    mesh.sections[0].rigidNode = 0u;
    const std::vector<Vector3f> before(Positions(mesh).begin(), Positions(mesh).end());
    geom::MirrorParams plane;
    plane.origin = {0.3f, 0.0f, 0.0f};
    plane.normal = {2.0f, 0.0f, 0.0f};
    const std::vector<u32> swap = {1u, 0u};
    ModelPlan plan = geom::PlanMirror(mesh, plane, swap);
    INFO(geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::FinishTool(mesh, plan);
    CHECK(CheckStructure(mesh) == "");
    CHECK(SignedVolume(mesh) == Approx(1.0).epsilon(1e-5)); // still wound outward
    CHECK(NormalStaleness(mesh) < 1.0f);
    const auto p = Positions(std::as_const(mesh));
    REQUIRE(p.size() == before.size());
    for (u32 v = 0; v < p.size(); ++v) {
        CHECK(p[v].x == Approx(0.6f - before[v].x).margin(1e-6));
        CHECK(p[v].y == before[v].y);
    }
    for (const Vector4f& t : mesh.attributes.get<const Vector4f>(geom::names::kTangent, Domain::Halfedge)) {
        CHECK(t.w == -1.0f);
        CHECK(t.y == 1.0f);
    }
    // Bone 0's weight is now what bone 1's was.
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        f32 w0 = 0.0f;
        for (const geom::Influence& in : mesh.skin.forVertex(v)) {
            w0 += in.bone == 0 ? in.weight : 0.0f;
        }
        CHECK(w0 == Approx(SkinField(before[v], 1.0f)).margin(1e-5));
    }
    CHECK(mesh.sections[0].rigidNode == 1u);
}

TEST_CASE("audit MIR P17: Mirror twice is the identity", "[wem][audit][mirror]") {
    Mesh mesh = audit::Box();
    Fields(mesh, 1.0f);
    for (Vector4f& t :
         mesh.attributes.getOrCreate<Vector4f>(geom::names::kTangent, Domain::Halfedge, geom::AttrType::F32x4)) {
        t = {0.0f, 1.0f, 0.0f, 1.0f};
    }
    {
        ModelPlan settle; // the fixture as a finished tool leaves it
        geom::FinishTool(mesh, settle);
    }
    const Mesh source = mesh;
    geom::MirrorParams plane;
    plane.origin = {0.3f, -0.2f, 0.1f};
    plane.normal = {1.0f, 2.0f, 3.0f};
    const std::vector<u32> swap = {1u, 0u};
    for (u32 k = 0; k < 2; ++k) {
        ModelPlan plan = geom::PlanMirror(mesh, plane, swap);
        REQUIRE_FALSE(plan.refused());
        geom::FinishTool(mesh, plan);
    }
    CHECK(CornerLists(mesh.faceSet()) == CornerLists(source.faceSet()));
    const auto p = Positions(std::as_const(mesh));
    const auto q = Positions(source);
    f32 worst = 0.0f;
    for (u32 v = 0; v < p.size(); ++v) {
        worst = std::max(worst, Length(p[v] - q[v]));
    }
    INFO("worst position drift " << worst);
    CHECK(worst < 1e-5f);
    const auto n = mesh.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    const auto m = source.attributes.get<const Vector3f>(geom::names::kNormal, Domain::Halfedge);
    REQUIRE(n.size() == m.size());
    f32 turned = 0.0f;
    for (u32 h = 0; h < n.size(); ++h) {
        turned = std::max(turned, Length(n[h] - m[h]));
    }
    INFO("worst normal drift " << turned);
    CHECK(turned < 1e-5f);
    CHECK(UvFieldError(mesh) < 1e-6f);
    CHECK(ColorFieldError(mesh) == 0u);
    CHECK(SkinFieldError(mesh, 1.0f) < 1e-6f);
    CHECK(mesh.sections.size() == source.sections.size());
}

TEST_CASE("audit MIR P15: Mirror is the same under a move, a scale and a turn", "[wem][audit][mirror]") {
    const Mesh mesh = Sheet(3, 2, -1.0f, 0.0f, [](f32 x, f32 y) { return 0.1f * x * y; });
    geom::MirrorParams plane;
    plane.origin = {0.25f, 0.0f, 0.0f};
    plane.normal = {1.0f, 0.5f, 0.0f};
    Mesh base = mesh;
    ModelPlan first = geom::PlanMirror(base, plane);
    REQUIRE_FALSE(first.refused());
    geom::FinishTool(base, first);
    for (const Similarity& t : P15Transforms()) {
        INFO(t.name);
        Mesh moved = Transformed(mesh, t);
        geom::MirrorParams turned;
        turned.origin = t.point(plane.origin);
        turned.normal = t.turn(plane.normal);
        ModelPlan plan = geom::PlanMirror(moved, turned);
        REQUIRE_FALSE(plan.refused());
        geom::FinishTool(moved, plan);
        const Mesh want = Transformed(base, t);
        const auto p = Positions(std::as_const(moved));
        const auto q = Positions(want);
        f32 worst = 0.0f;
        for (u32 v = 0; v < p.size(); ++v) {
            worst = std::max(worst, Length(p[v] - q[v]) / t.scale);
        }
        INFO("worst " << worst);
        CHECK(worst < 1e-3f);
        CHECK(CornerLists(moved.faceSet()) == CornerLists(base.faceSet()));
    }
}

TEST_CASE("audit DUP-1: a duplicate is not one point with its source", "[wem][audit][duplicate]") {
    // Q8 / X2: the copy must move alone, so no point may join it to the original.
    const Mesh mesh = QuadGrid(2, 1);
    Mesh out;
    const Verdict verdict =
        Run(mesh, [](Mesh& m, const PointTable&) { return geom::PlanDuplicate(m, Faces({0})); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 3u);
}

TEST_CASE("audit DUP-2: faces sharing only a vertex duplicate without a pinch",
          "[wem][audit][duplicate]") {
    // The open fan's first and last triangles meet only at its border vertex:
    // the copy needs that vertex once per fan, as Detach to Element makes it.
    const Mesh mesh = OpenFan();
    Contract loose;
    loose.joinsShellsOk = true;
    Mesh out;
    const Verdict verdict =
        Run(mesh, [](Mesh& m, const PointTable&) { return geom::PlanDuplicate(m, Faces({0, 2})); }, loose, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 5u);
}

TEST_CASE("audit DUP case: two shells touching at a point duplicate as two shells", "[wem][audit][duplicate]") {
    const Mesh mesh = TouchingQuads();
    Contract loose;
    loose.joinsShellsOk = true;
    Mesh out;
    const Verdict verdict =
        Run(mesh, [](Mesh& m, const PointTable&) { return geom::PlanDuplicate(m, Faces({0, 1})); }, loose, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 4u);
}

TEST_CASE("audit DET-1: a clone of every face is a new mesh, not refused", "[wem][audit][detach]") {
    Mesh mesh = audit::Box();
    Mesh made;
    ModelPlan plan = geom::PlanDetach(mesh, AllFaces(mesh), made, /*keep=*/true);
    INFO(geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    CHECK(LiveFaces(mesh) == 6u);
    CHECK(LiveFaces(made) == 6u);
}

TEST_CASE("audit DET-3: Detach to Element leaves the rest's border its own point",
          "[wem][audit][detach]") {
    const Mesh mesh = QuadGrid(2, 1);
    Mesh out;
    const Verdict verdict =
        Run(mesh, [](Mesh& m, const PointTable&) { return geom::PlanDetachToElement(m, Faces({0})); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
}

TEST_CASE("audit DET-4: Detach keeps both section tables whole, every face in range", "[wem][audit][detach]") {
    // Rewritten (KP): Blender's Separate and 3ds Max's Detach keep every
    // material slot on both objects, and the exporters leave a faceless
    // section out, so both meshes keep the source's two sections: no index a
    // channel or a cloth holds is renumbered. Two sections, the top row in
    // section 1; the top row detached.
    Mesh mesh = TwoSections(2, 2, 1);
    Mesh made;
    ModelPlan plan = geom::PlanDetach(mesh, Faces({2, 3}), made, /*keep=*/false);
    INFO(geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    geom::FinishTool(mesh, plan);
    INFO("source " << mesh.sections.size() << " sections, new mesh " << made.sections.size());
    CHECK(made.sections.size() == 2u);
    CHECK(mesh.sections.size() == 2u);
    CHECK(LiveFaces(made) == 2u);
    CHECK(LiveFaces(mesh) == 2u);
    for (const u32 s : made.faceSections()) {
        CHECK(s < made.sections.size());
    }
    for (const u32 s : mesh.faceSections()) {
        CHECK(s < mesh.sections.size());
    }
}

// ============================================================================
// §4.C Make Planar
// ============================================================================

namespace {

/// The 5 x 5 dome z = -0.25 (x^2 + y^2) over x, y in -2..2, turned 30 degrees
/// about X: its least-squares normal is the turned Z, by symmetry.
std::vector<Vector3f> TiltedDome(Vector3f* normal) {
    Similarity tilt;
    tilt.axis = {1.0f, 0.0f, 0.0f};
    tilt.radians = 30.0f * 3.14159265358979f / 180.0f;
    std::vector<Vector3f> out;
    for (i32 y = -2; y <= 2; ++y) {
        for (i32 x = -2; x <= 2; ++x) {
            const f32 fx = static_cast<f32>(x), fy = static_cast<f32>(y);
            out.push_back(tilt.point(Vector3f{fx, fy, -0.25f * (fx * fx + fy * fy)}));
        }
    }
    *normal = tilt.turn(Vector3f{0.0f, 0.0f, 1.0f});
    return out;
}

f32 DegreesBetweenLines(const Vector3f& a, const Vector3f& b) {
    const f32 c = std::abs(a.x * b.x + a.y * b.y + a.z * b.z) / (Length(a) * Length(b));
    return std::acos(std::min(1.0f, c)) * 57.2957795f;
}

std::function<ModelPlan(Mesh&, const PointTable&)> Planar(const ElementSet& selection, u32 axis) {
    return [selection, axis](Mesh& m, const PointTable& p) { return geom::PlanMakePlanar(m, p, selection, axis); };
}

/// The dome as a mesh: QuadGrid(4, 4) on the tilted dome's points.
Mesh DomeMesh() {
    Vector3f unused;
    const std::vector<Vector3f> points = TiltedDome(&unused);
    Mesh mesh = QuadGrid(4, 4);
    std::copy(points.begin(), points.end(), Positions(mesh).begin());
    Shade(mesh);
    mesh.recomputeBounds();
    return mesh;
}

/// The largest distance of @p mesh's live points from their own best plane,
/// over its size.
f32 Unflatness(const Mesh& mesh) {
    std::vector<Vector3f> live;
    for (const u32 v : AllVertices(mesh).vertices) {
        live.push_back(Positions(mesh)[v]);
    }
    Vector3f origin, normal;
    geom::FitPlane(live, &origin, &normal);
    f32 worst = 0.0f;
    for (const Vector3f& p : live) {
        worst = std::max(worst, std::abs((p - origin).dot(normal)));
    }
    return worst / std::max(1e-12f, Length(mesh.bounds.maximum - mesh.bounds.minimum));
}

} // namespace

TEST_CASE("audit PLN-1: FitPlane finds the least-squares plane of a tilted dome", "[wem][audit][planar]") {
    Vector3f want;
    const std::vector<Vector3f> points = TiltedDome(&want);
    Vector3f origin, normal;
    REQUIRE(geom::FitPlane(points, &origin, &normal));
    const f32 error = DegreesBetweenLines(normal, want);
    INFO("FitPlane's normal is " << error << " degrees off the least-squares one");
    CHECK(error < 0.05f);
}

TEST_CASE("audit PLN case: Best fit flattens the tilted dome, checks clean", "[wem][audit][planar]") {
    const Mesh mesh = DomeMesh();
    Contract contract;
    contract.normalsOk = false;
    Mesh out;
    const Verdict verdict = Run(mesh, Planar(AllVertices(mesh), 3), contract, &out, geom::kDefaultShadingAngle, 1.0f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed << "; unflatness " << Unflatness(out));
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(Unflatness(out) < 1e-5f);
}

TEST_CASE("audit PLN-1 / P15: Best fit is the same under a move, a scale and a turn",
          "[wem][audit][planar]") {
    const Mesh mesh = DomeMesh();
    Mesh base;
    REQUIRE_FALSE(Run(mesh, Planar(AllVertices(mesh), 3), {}, &base, geom::kDefaultShadingAngle, 1.0f).refused);
    for (const Similarity& t : P15Transforms()) {
        INFO(t.name);
        Mesh out;
        const Mesh moved = Transformed(mesh, t);
        const Verdict verdict =
            Run(moved, Planar(AllVertices(moved), 3), {}, &out, geom::kDefaultShadingAngle, 1.0f);
        INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        const f32 gap = MaxPointGap(out, Transformed(base, t)) / t.scale;
        INFO("gap " << gap << ", unflatness " << Unflatness(out));
        CHECK(gap < 1e-3f);
        CHECK(Unflatness(out) < 1e-4f);
    }
}

TEST_CASE("audit PLN-5: a plane already flat to rounding is refused as already flat",
          "[wem][audit][planar]") {
    // A flat grid turned 37 degrees about (1, 2, 3) and moved: flat but for
    // float rounding. Best fit and Z-like planes must say "already flat"
    // rather than journal a move of 1e-7.
    Similarity t = P15Transforms()[3];
    t.offset = {3.0f, -2.0f, 1.0f};
    const Mesh mesh = Transformed(QuadGrid(3, 3), t);
    Mesh work = mesh;
    const ModelPlan plan = geom::PlanMakePlanar(work, PointsOf(work), AllVertices(work), 3);
    f32 largest = 0.0f;
    for (const geom::VertexMotion& m : plan.motions) {
        largest = std::max(largest, Length(m.direction));
    }
    INFO(geom::ToString(plan.refusal) << ", " << plan.motions.size() << " motions, largest " << largest);
    CHECK(plan.refusal == ModelRefusal::ZeroAmount);
}

TEST_CASE("audit PLN-7: Best fit on collinear points moves nothing", "[wem][audit][planar]") {
    // (0,0,0), (1,1,1), (2,2,2): no plane to fit. FitPlane says false, and the
    // tool flattens onto world Z through their centre instead.
    Mesh mesh = QuadGrid(2, 2);
    for (Vector3f& p : Positions(mesh)) {
        p.z = p.x;
    }
    Shade(mesh);
    const ElementSet line = Vertices({VertexAt(mesh, 0, 0, 0), VertexAt(mesh, 1, 1, 1), VertexAt(mesh, 2, 2, 2)});
    Mesh out;
    const Verdict verdict = Run(mesh, Planar(line, 3), {}, &out, geom::kDefaultShadingAngle, 1.0f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    if (!verdict.refused) {
        CHECK(VertexAt(out, 0, 0, 0) != kInvalidId);
        CHECK(VertexAt(out, 2, 2, 2) != kInvalidId);
    }
}

TEST_CASE("audit PLN-3: Each part with no plane for any part says so, not already flat",
          "[wem][audit][planar]") {
    // Each part whose every part was collinear: the caller hands zero normals
    // (FitPlane's false). The kernel reads that as ZeroAmount, "already flat".
    Mesh mesh = QuadGrid(2, 2);
    const std::vector<geom::PlanarPart> parts = {
        {{0, 1, 2}, Vector3f{1, 0, 0}, Vector3f{0, 0, 0}},
        {{6, 7, 8}, Vector3f{1, 2, 0}, Vector3f{0, 0, 0}},
    };
    const ModelPlan plan = geom::PlanMakePlanar(mesh, parts);
    INFO(geom::ToString(plan.refusal));
    REQUIRE(plan.refused());
    CHECK(plan.refusal != ModelRefusal::ZeroAmount);
    CHECK(plan.refusal != ModelRefusal::EmptySelection);
}

TEST_CASE("audit PLN case: a crack's twins flatten together", "[wem][audit][planar]") {
    Mesh mesh = Crack(2, 2, 1);
    for (Vector3f& p : Positions(mesh)) {
        p.z = 0.1f * p.x * p.y;
    }
    Shade(mesh);
    Mesh out;
    const Verdict verdict = Run(mesh, Planar(AllVertices(mesh), 3), {}, &out, geom::kDefaultShadingAngle, 1.0f);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(Unflatness(out) < 1e-5f);
}

TEST_CASE("audit PLN case: two faces touching at a point are one part or two", "[wem][audit][planar]") {
    const Mesh mesh = TouchingQuads();
    const auto parts = geom::SelectionParts(mesh, PointsOf(mesh), Faces({0, 1}));
    INFO(parts.size() << " parts");
    CHECK(parts.size() >= 1u);
}

// ============================================================================
// §4.C Subdivide, Smooth, Orient Outward
// ============================================================================

namespace {

std::function<ModelPlan(Mesh&, const PointTable&)> Subdivide(const ElementSet& faces,
                                                             const geom::SubdivideParams& params) {
    return [faces, params](Mesh& m, const PointTable&) { return geom::PlanSubdivide(m, faces, params); };
}

std::function<ModelPlan(Mesh&, const PointTable&)> SmoothWith(const ElementSet& elements,
                                                              const geom::SmoothParams& params) {
    return [elements, params](Mesh& m, const PointTable&) { return geom::PlanSmooth(m, elements, params); };
}

/// @p mesh with each face's corners reversed: wound inward.
Mesh Reversed(const Mesh& mesh) {
    std::vector<std::vector<u32>> faces = CornerLists(mesh.faceSet());
    for (std::vector<u32>& f : faces) {
        std::reverse(f.begin(), f.end());
    }
    return FromFaces(faces, std::vector<Vector3f>(Positions(mesh).begin(), Positions(mesh).end()));
}

} // namespace

TEST_CASE("audit SUB-1: subdividing one side of a crack keeps its twins together",
          "[wem][audit][subdivide]") {
    // Crack(2, 2, 1); its left column (faces 0 and 2) by Catmull-Clark. The
    // right faces were not chosen, so (Q5: twins are one vertex) the crack's
    // corners hold; today the left twins move by the border rule alone.
    const Mesh mesh = Crack(2, 2, 1);
    Mesh out;
    const Verdict verdict = Run(mesh, Subdivide(Faces({0, 2}), {}), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    for (const f32 y : {0.0f, 1.0f, 2.0f}) {
        INFO("twins at (1, " << y << ")");
        CHECK(VertexAt(out, 1.0f, y) != kInvalidId);
        CHECK(VertexAt(out, 1.0f, y, 0.0f, 1) != kInvalidId);
    }
}

TEST_CASE("audit SUB-2: Catmull-Clark rounds a primitive box", "[wem][audit][subdivide]") {
    // Max's TurboSmooth / MSmooth rounds a box: smoothing groups are not
    // creases. MakeBox marks every edge sharp, and creases on (the default)
    // hold every one, so the box comes back a box.
    geom::PrimitiveParams params;
    Mesh mesh = geom::MakeBox(params);
    ModelPlan plan = geom::PlanSubdivide(mesh, ElementSet{}, geom::SubdivideParams{});
    REQUIRE_FALSE(plan.refused());
    geom::FinishTool(mesh, plan);
    u32 corners = 0;
    for (const Vector3f& p : Positions(std::as_const(mesh))) {
        corners += std::min({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) > 0.999f ? 1u : 0u;
    }
    INFO(corners << " vertices still on the box's corners");
    CHECK(corners == 0u);
}

TEST_CASE("audit SUB-3: subdividing a concave n-gon is made and checks clean; a fold is accepted (Q12)",
          "[wem][audit][subdivide]") {
    // The L (0,0) (3,0) (3,1) (1,1) (1,3) (0,3): its vertex mean (4/3, 4/3) is
    // outside it, in the notch. Rewritten (KP) to Q12: Max and Blender fold it
    // alike, so a folded quad is accepted; the rows it stores must not be stale.
    const Mesh mesh = FromFaces({{0, 1, 2, 3, 4, 5}}, {{0, 0, 0}, {3, 0, 0}, {3, 1, 0}, {1, 1, 0}, {1, 3, 0}, {0, 3, 0}});
    geom::SubdivideParams params;
    params.scheme = geom::SubdivideScheme::Linear;
    Mesh out;
    const Verdict verdict = Run(mesh, Subdivide(ElementSet{}, params), {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    u32 folded = 0;
    for (const u32 f : AllFaces(out).faces) {
        const auto p = Positions(out);
        Vector3f n{0, 0, 0};
        for (const HalfedgeId h : out.topology().fh(FaceId(f))) {
            n = n + Cross(p[out.topology().from(h).value()], p[out.topology().to(h).value()]);
        }
        folded += n.z <= 0.0f ? 1u : 0u;
    }
    INFO(folded << " of " << LiveFaces(out) << " faces face down or have no area");
    CHECK(verdict.failed == "");
    CHECK(LiveFaces(out) == 6u);
}

TEST_CASE("audit SUB P15: Subdivide is the same under a move, a scale and a turn", "[wem][audit][subdivide]") {
    const Mesh mesh = audit::Box();
    geom::SubdivideParams params;
    params.creases = false;
    Mesh base;
    REQUIRE_FALSE(Run(mesh, Subdivide(ElementSet{}, params), {}, &base).refused);
    for (const Similarity& t : P15Transforms()) {
        INFO(t.name);
        Mesh out;
        const Verdict verdict = Run(Transformed(mesh, t), Subdivide(ElementSet{}, params), {}, &out);
        INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        CHECK(verdict.failed == "");
        const f32 gap = MaxPointGap(out, Transformed(base, t)) / t.scale;
        INFO("gap " << gap);
        CHECK(gap < 1e-3f);
    }
}

TEST_CASE("audit SMO-1: Smooth sees the same neighbours with or without the drawn diagonals",
          "[wem][audit][smooth]") {
    // A quad grid and the same grid as the very triangles it draws: design
    // §2.3 takes neighbours from the point ring with drawn diagonals, so the
    // two smooth alike.
    Mesh quads = QuadGrid(4, 4);
    Positions(quads)[12].z = 1.0f; // (2, 2)
    Positions(quads)[6].z = 0.5f;  // (1, 1)
    Shade(quads);
    std::vector<u32> tris;
    geom::TriangulateMesh(quads, tris);
    std::vector<std::vector<u32>> loops;
    for (std::size_t k = 0; k + 2 < tris.size(); k += 3) {
        loops.push_back({tris[k], tris[k + 1], tris[k + 2]});
    }
    Mesh triangles = FromFaces(loops, std::vector<Vector3f>(Positions(quads).begin(), Positions(quads).end()));
    geom::SmoothParams params;
    ModelPlan a = geom::PlanSmooth(quads, ElementSet{}, params);
    ModelPlan b = geom::PlanSmooth(triangles, ElementSet{}, params);
    REQUIRE_FALSE(a.refused());
    REQUIRE_FALSE(b.refused());
    f32 worst = 0.0f;
    for (u32 v = 0; v < quads.vertexCount(); ++v) {
        worst = std::max(worst, Length(Positions(std::as_const(quads))[v] - Positions(std::as_const(triangles))[v]));
    }
    INFO("largest difference " << worst);
    CHECK(worst < 1e-5f);
}

TEST_CASE("audit SMO-2: a border-only selection with pinned borders is not refused as empty",
          "[wem][audit][smooth]") {
    Mesh mesh = QuadGrid(2, 2);
    std::vector<u32> border;
    for (u32 v = 0; v < 9; ++v) {
        if (v != 4) {
            border.push_back(v);
        }
    }
    const ModelPlan plan = geom::PlanSmooth(mesh, Vertices(border), geom::SmoothParams{});
    INFO(geom::ToString(plan.refusal));
    CHECK(plan.refusal != ModelRefusal::EmptySelection);
}

TEST_CASE("audit SMO-3: Smooth with borders free keeps a crack's twins together", "[wem][audit][smooth]") {
    Mesh mesh = Crack(2, 2, 1);
    geom::SmoothParams params;
    params.pinBorders = false;
    const ModelPlan plan = geom::PlanSmooth(mesh, ElementSet{}, params);
    REQUIRE_FALSE(plan.refused());
    const auto p = Positions(std::as_const(mesh));
    f32 apart = 0.0f;
    for (u32 y = 0; y <= 2; ++y) {
        apart = std::max(apart, Length(p[y * 3 + 1] - p[9 + y]));
    }
    INFO("twins " << apart << " apart");
    CHECK(apart < 1e-6f);
}

TEST_CASE("audit SMO-4: Smooth with pinned borders moves a crack's inner twins", "[wem][audit][smooth]") {
    // Q5: twins are one vertex, so the crack's middle (1, 1) is interior and
    // a bump there smooths; today both twins read as border and stay pinned.
    Mesh mesh = Crack(4, 2, 2);
    Positions(mesh)[7].z = 1.0f;  // left (2, 1)
    Positions(mesh)[16].z = 1.0f; // its twin
    Shade(mesh);
    const ModelPlan plan = geom::PlanSmooth(mesh, ElementSet{}, geom::SmoothParams{});
    INFO(geom::ToString(plan.refusal));
    REQUIRE_FALSE(plan.refused());
    const auto p = Positions(std::as_const(mesh));
    INFO("twins at z " << p[7].z << " and " << p[16].z);
    CHECK(p[7].z < 0.9f);
    CHECK(p[7].z == p[16].z);
}

TEST_CASE("audit SMO P15: Smooth is the same under a move, a scale and a turn", "[wem][audit][smooth]") {
    Mesh mesh = QuadGrid(4, 4);
    Positions(mesh)[12].z = 1.0f;
    Shade(mesh);
    geom::SmoothParams params;
    params.iterations = 3;
    params.scheme = geom::SmoothScheme::Taubin;
    Mesh base;
    REQUIRE_FALSE(Run(mesh, SmoothWith(ElementSet{}, params), {}, &base).refused);
    for (const Similarity& t : P15Transforms()) {
        INFO(t.name);
        Mesh out;
        const Verdict verdict = Run(Transformed(mesh, t), SmoothWith(ElementSet{}, params), {}, &out);
        INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
        REQUIRE_FALSE(verdict.refused);
        const f32 gap = MaxPointGap(out, Transformed(base, t)) / t.scale;
        INFO("gap " << gap);
        CHECK(gap < 1e-3f);
    }
}

TEST_CASE("audit NRM-1: Orient Outward from one face turns and re-shades the whole part", "[wem][audit][normals]") {
    const Mesh inside = Reversed(audit::Box());
    REQUIRE(SignedVolume(inside) < 0.0);
    Mesh out;
    const Verdict verdict =
        Run(inside, [](Mesh& m, const PointTable&) { return geom::PlanOrientOutward(m, Faces({0})); }, {}, &out);
    INFO(geom::ToString(verdict.refusal) << " " << verdict.failed);
    REQUIRE_FALSE(verdict.refused);
    CHECK(verdict.failed == "");
    CHECK(SignedVolume(out) == Approx(1.0).epsilon(1e-5));
}

// ============================================================================
// §4.C Primitives and the lattice
// ============================================================================

TEST_CASE("audit PRM-4: the default sphere is round, not a bipyramid", "[wem][audit][primitive]") {
    const Mesh sphere = geom::MakeSphere(geom::PrimitiveParams{});
    const f64 ratio = SignedVolume(sphere) / (4.0 / 3.0 * 3.14159265358979);
    INFO(LiveFaces(sphere) << " faces, " << LiveVertices(sphere) << " vertices, volume " << ratio
                           << " of the sphere's");
    CHECK(ratio > 0.75);
}

TEST_CASE("audit PRM-6: a cylinder and a cone are as tall as primitives.h says", "[wem][audit][primitive]") {
    // Rewritten (KP): the doc and the code disagreed. The code's half-extent
    // is what Create's draft stands a shape on its base by (mesh_primitive),
    // and it makes each round shape fit the box of the same size, so the doc
    // now says it: height 2 * size.z, as a box is 2 * size.
    geom::PrimitiveParams params;
    params.size = {1.0f, 1.0f, 2.0f};
    const Mesh cylinder = geom::MakeCylinder(params);
    const Mesh cone = geom::MakeCone(params);
    const f32 tall = cylinder.bounds.maximum.z - cylinder.bounds.minimum.z;
    const f32 coneTall = cone.bounds.maximum.z - cone.bounds.minimum.z;
    INFO("cylinder " << tall << " tall, cone " << coneTall << ", for size.z 2");
    CHECK(tall == Approx(4.0f));
    CHECK(coneTall == Approx(4.0f));
    CHECK(cylinder.bounds.minimum.z == Approx(-2.0f));
    // A box is documented as 2 * size, which it is.
    const Mesh box = geom::MakeBox(params);
    CHECK(box.bounds.maximum.z - box.bounds.minimum.z == Approx(4.0f));
}

TEST_CASE("audit PRM-7: a box of size 0 or a negative size is no degenerate or inside-out mesh",
          "[wem][audit][primitive]") {
    geom::PrimitiveParams zero;
    zero.size = {0.0f, 0.0f, 0.0f};
    const Mesh flat = geom::MakeBox(zero);
    INFO("size 0: " << FaceTotal(flat) << " faces, " << CheckDegenerate(flat));
    CHECK((FaceTotal(flat) == 0u || CheckDegenerate(flat) == ""));
    geom::PrimitiveParams negative;
    negative.size = {-1.0f, 1.0f, 1.0f};
    const Mesh turned = geom::MakeBox(negative);
    INFO("size (-1, 1, 1): volume " << SignedVolume(turned));
    CHECK((FaceTotal(turned) == 0u || SignedVolume(turned) > 0.0));
}

TEST_CASE("audit LAT-2: a lattice fitted round no points", "[wem][audit][lattice]") {
    // The viewer must refuse an empty part (LAT-2); the kernel has no way to
    // say "nothing": it fits a unit box at the origin. What it does, recorded.
    const Vector3f axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const u32 points[3] = {3, 3, 3};
    const geom::Lattice none = geom::FitLattice(std::span<const Vector3f>(), axes, points);
    INFO("origin " << none.origin.x << " " << none.origin.y << " " << none.origin.z << ", size " << none.size.x
                   << " " << none.size.y << " " << none.size.z << ", " << none.controls.size() << " controls");
    CHECK(none.size.x > 0.0f);
    // And at rest it moves nothing, bit for bit.
    const std::vector<Vector3f> some = {{0.1f, 0.2f, 0.3f}, {1.7f, -0.4f, 2.0f}, {0.5f, 0.5f, 0.5f}};
    const geom::Lattice fitted = geom::FitLattice(some, axes, points);
    std::vector<Vector3f> coordinates;
    for (const Vector3f& p : some) {
        coordinates.push_back(geom::LatticeCoordinates(fitted, p));
    }
    std::vector<Vector3f> out(some.size());
    geom::DeformByLattice(fitted, some, coordinates, out);
    for (std::size_t i = 0; i < some.size(); ++i) {
        CHECK(out[i].x == some[i].x);
        CHECK(out[i].y == some[i].y);
        CHECK(out[i].z == some[i].z);
    }
}

// ============================================================================
// §4.C Mirror and the soft bodies (MIR-2)
// ============================================================================

namespace {

/// Where @p place stands on @p mesh: its weights on its face's first triangle.
Vector3f PlaceOn(const Mesh& mesh, const SoftPlace& place) {
    const std::vector<std::vector<u32>> loops = CornerLists(mesh.faceSet());
    const std::vector<u32>& c = loops.at(place.face);
    const auto p = Positions(mesh);
    return p[c[0]] * place.weights.x + p[c[1]] * place.weights.y + p[c[2]] * place.weights.z;
}

} // namespace

TEST_CASE("audit MIR-2: a soft-body place goes with its face through a Mirror in place",
          "[wem][audit][mirror]") {
    // Two quads, x in 0..2; a place at the middle of face 1's first triangle.
    // Mirrored in x = 0 the face moves to x in -2..-1, and the place with it.
    // SoftFollowRenumberedFaces re-pins to the face nearest the OLD spot.
    const Mesh before = Sheet(2, 1, 0.0f, 0.0f, [](f32, f32) { return 0.0f; });
    Mesh after = before;
    geom::MirrorParams plane;
    ModelPlan plan = geom::PlanMirror(after, plane);
    REQUIRE_FALSE(plan.refused());
    geom::FinishTool(after, plan);
    PhysicsSet physics;
    SoftBody body;
    body.id = 1;
    body.meshes = {0};
    SoftEffect effect;
    effect.id = 1;
    effect.place.mesh = 0;
    effect.place.face = 1;
    effect.place.weights = {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
    body.effects.push_back(effect);
    physics.soft.push_back(body);
    const Vector3f stood = PlaceOn(before, effect.place);
    SoftFollowRenumberedFaces(physics, 0, before, after);
    const SoftPlace& now = physics.soft[0].effects[0].place;
    REQUIRE(now.face != kInvalidIndex);
    const Vector3f stands = PlaceOn(after, now);
    INFO("stood at x " << stood.x << " on face 1; now face " << now.face << " at x " << stands.x);
    CHECK(stands.x == Approx(-stood.x).margin(1e-4));
    CHECK(stands.y == Approx(stood.y).margin(1e-4));
}

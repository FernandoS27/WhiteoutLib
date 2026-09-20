// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The weight algebra (EDIT_MODE_SKIN_DESIGN.md §6, gate S2).
///
/// Every command, brush dab and generator of the Skin workspace ends in one of
/// these functions, so §6.1's rule is asserted here row by row -- including its
/// edge, where the only unlocked influence is lowered and its weight has to go
/// somewhere.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/model.h>
#include <whiteout/models/wem/skinning/ops.h>
#include <whiteout/models/wem/skinning/points.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// A chain of bones: root -> chest -> arm -> hand, plus a free "other".
NodeTree makeRig() {
    NodeTree tree;
    const auto add = [&](const char* name, u32 parent) {
        Node node;
        node.name = name;
        node.kind = NodeKind::Bone;
        node.parent = parent;
        node.resetPayloadForKind();
        tree.nodes.push_back(std::move(node));
    };
    add("root", kInvalidNode); // 0
    add("chest", 0);           // 1
    add("arm", 1);             // 2
    add("hand", 2);            // 3
    add("other", kInvalidNode); // 4
    return tree;
}

/// A strip of @p count triangles, each vertex its own point, all bound as
/// @p weights says.
Mesh makeStrip(u32 triangles, const std::vector<geom::Influence>& weights) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "body";
    builder.addSection(std::move(section));
    for (u32 t = 0; t < triangles; ++t) {
        for (u32 k = 0; k < 3; ++k) {
            const geom::VertexId id = builder.addVertex(
                Vector3f{static_cast<f32>(t * 2 + (k == 1 ? 1 : 0)), static_cast<f32>(k == 2 ? 1 : 0), 0.0f});
            for (const geom::Influence& influence : weights) {
                builder.addInfluence(id, influence.bone, influence.weight);
            }
        }
    }
    for (u32 t = 0; t < triangles; ++t) {
        const geom::FaceId face = builder.addTriangle(geom::VertexId(t * 3), geom::VertexId(t * 3 + 1),
                                                      geom::VertexId(t * 3 + 2), 0);
        for (u32 corner = 0; corner < 3; ++corner) {
            builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
        }
    }
    return builder.build().mesh;
}

/// One vertex's weights as a sorted (bone, weight) list, for comparing.
std::vector<std::pair<u32, f32>> weightsOf(const Mesh& mesh, u32 vertex) {
    std::vector<std::pair<u32, f32>> out;
    for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
        out.emplace_back(influence.bone, influence.weight);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool near(f32 a, f32 b, f32 tolerance = 1e-5f) {
    return std::abs(a - b) <= tolerance;
}

/// Every vertex sums to 1, and every vertex is heaviest first.
void checkNormalAndSorted(const Mesh& mesh) {
    for (u32 v = 0; v < mesh.skin.vertexCount(); ++v) {
        const std::span<const geom::Influence> influences = mesh.skin.forVertex(v);
        if (influences.empty()) {
            continue;
        }
        f32 total = 0.0f;
        for (std::size_t i = 0; i < influences.size(); ++i) {
            total += influences[i].weight;
            CHECK(std::isfinite(influences[i].weight));
            CHECK(influences[i].weight > 0.0f);
            if (i + 1 < influences.size()) {
                CHECK(influences[i].weight >= influences[i + 1].weight);
            }
        }
        CHECK(near(total, 1.0f, 1e-6f));
    }
}

/// Every point of @p mesh, in order.
std::vector<u32> allPoints(const skinning::PointTable& points) {
    std::vector<u32> out(points.pointCount);
    for (u32 p = 0; p < points.pointCount; ++p) {
        out[p] = p;
    }
    return out;
}

f32 weightOn(const Mesh& mesh, u32 vertex, u32 bone) {
    for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
        if (influence.bone == bone) {
            return influence.weight;
        }
    }
    return 0.0f;
}

constexpr u32 kRoot = 0;
constexpr u32 kChest = 1;
constexpr u32 kArm = 2;
constexpr u32 kHand = 3;

} // namespace

TEST_CASE("S2 the normalisation table, row by row", "[wem][skin][ops]") {
    struct Row {
        const char* name;
        std::vector<geom::Influence> before;
        u32 bone;
        f32 request;
        std::vector<std::pair<u32, f32>> after;
        bool lockChest = false;
        u32 refused = 0;
    };
    const std::vector<Row> rows = {
        {"arm to 0.9", {{kArm, 0.6f}, {kChest, 0.4f}}, kArm, 0.9f, {{kChest, 0.1f}, {kArm, 0.9f}}},
        {"the others keep their proportions",
         {{kArm, 0.6f}, {kChest, 0.3f}, {kRoot, 0.1f}},
         kArm,
         0.2f,
         {{kRoot, 0.2f}, {kChest, 0.6f}, {kArm, 0.2f}}},
        {"a locked bone clamps the target",
         {{kArm, 0.6f}, {kChest, 0.4f}},
         kArm,
         0.9f,
         {{kChest, 0.4f}, {kArm, 0.6f}},
         /*lockChest=*/true},
        {"the only unlocked bone gives to its parent",
         {{kArm, 1.0f}},
         kArm,
         0.3f,
         {{kChest, 0.7f}, {kArm, 0.3f}}},
        {"a root with nowhere to give is refused",
         {{kRoot, 1.0f}},
         kRoot,
         0.3f,
         {{kRoot, 1.0f}},
         /*lockChest=*/false,
         /*refused=*/1},
        {"adding a bone takes from the others in proportion",
         {{kArm, 0.5f}, {kChest, 0.5f}},
         kHand,
         0.2f,
         {{kChest, 0.4f}, {kArm, 0.4f}, {kHand, 0.2f}}},
    };

    for (const Row& row : rows) {
        CAPTURE(row.name);
        NodeTree rig = makeRig();
        rig.nodes[kChest].skin.locked = row.lockChest;
        Mesh mesh = makeStrip(1, row.before);
        const skinning::PointTable points = skinning::BuildPointTable(mesh);
        const std::vector<u32> scope = allPoints(points);
        const skinning::SkinResult result = skinning::Set(
            mesh, rig, points, skinning::SkinScope{scope, {}}, row.bone, row.request);
        // The count is per point, and every point of the fixture is the row.
        CHECK(result.refused == (row.refused != 0 ? scope.size() : 0u));

        std::vector<std::pair<u32, f32>> want = row.after;
        std::sort(want.begin(), want.end());
        const std::vector<std::pair<u32, f32>> got = weightsOf(mesh, 0);
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].first == want[i].first);
            CHECK(near(got[i].second, want[i].second));
        }
        checkNormalAndSorted(mesh);
    }
}

TEST_CASE("S2 a lock is bit-identical under every operation", "[wem][skin][ops]") {
    NodeTree rig = makeRig();
    rig.nodes[kChest].skin.locked = true;
    const std::vector<geom::Influence> start = {{kArm, 0.55f}, {kChest, 0.3f}, {kRoot, 0.15f}};

    const auto run = [&](const char* what, auto&& operation) {
        CAPTURE(what);
        Mesh mesh = makeStrip(2, start);
        const skinning::PointTable points = skinning::BuildPointTable(mesh);
        const std::vector<u32> scope = allPoints(points);
        operation(mesh, points, scope);
        for (u32 v = 0; v < mesh.skin.vertexCount(); ++v) {
            // Bit-identical, not near: a locked weight is not touched at all.
            CHECK(weightOn(mesh, v, kChest) == 0.3f);
        }
        checkNormalAndSorted(mesh);
    };
    const skinning::PointTable* unused = nullptr;
    (void)unused;

    run("Set", [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
        skinning::Set(mesh, rig, points, {scope, {}}, kArm, 0.9f);
    });
    run("Add", [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
        skinning::Add(mesh, rig, points, {scope, {}}, kHand, 0.3f);
    });
    run("Rigid", [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
        skinning::Rigid(mesh, rig, points, {scope, {}}, kHand);
    });
    run("Remove", [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
        skinning::Remove(mesh, rig, points, {scope, {}}, kArm);
    });
    run("Normalize",
        [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
            skinning::Normalize(mesh, rig, points, {scope, {}});
        });
    run("Prune", [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
        skinning::Prune(mesh, rig, points, {scope, {}}, 0.2f);
    });
    run("Limit", [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
        skinning::Limit(mesh, rig, points, {scope, {}}, 2);
    });
    run("Smooth", [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
        skinning::Smooth(mesh, rig, points, {scope, {}}, 0.5f, 2);
    });
    run("Sharpen",
        [&](Mesh& mesh, const skinning::PointTable& points, const std::vector<u32>& scope) {
            skinning::Sharpen(mesh, rig, points, {scope, {}}, 0.5f);
        });

    // A bone lock also refuses an operation aimed at the locked bone itself.
    Mesh mesh = makeStrip(1, start);
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> scope = allPoints(points);
    const skinning::SkinResult result =
        skinning::Set(mesh, rig, points, {scope, {}}, kChest, 0.8f);
    CHECK(result.changed == 0u);
    CHECK(result.locked == scope.size());
    CHECK(weightOn(mesh, 0, kChest) == 0.3f);
}

TEST_CASE("S2 a locked point is not written", "[wem][skin][ops]") {
    const NodeTree rig = makeRig();
    Mesh mesh = makeStrip(2, {{kArm, 0.5f}, {kChest, 0.5f}});
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> scope = allPoints(points);
    const std::vector<u32> locked{scope.front()};
    CHECK(skinning::SetPointsLocked(mesh, points, locked, true) == 1u);
    CHECK(skinning::PointLocked(mesh, points, scope.front()));

    const std::vector<std::pair<u32, f32>> before = weightsOf(mesh, points.membersOf(scope.front())[0]);
    const skinning::SkinResult result = skinning::Rigid(mesh, rig, points, {scope, {}}, kHand);
    CHECK(result.locked == 1u);
    CHECK(weightsOf(mesh, points.membersOf(scope.front())[0]) == before);
    // And the rest of the mesh was written.
    CHECK(result.changed == scope.size() - 1);

    CHECK(skinning::SetPointsLocked(mesh, points, locked, false) == 1u);
    CHECK_FALSE(skinning::PointLocked(mesh, points, scope.front()));
}

TEST_CASE("S2 Smooth reads outside its scope and writes only inside it", "[wem][skin][ops]") {
    const NodeTree rig = makeRig();
    // A strip whose triangles share their corners, so the points have a ring:
    // half of it on the arm, half on the chest, a hard edge to smooth.
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "strip";
    builder.addSection(std::move(section));
    constexpr u32 kSpan = 6;
    for (u32 v = 0; v < kSpan; ++v) {
        const geom::VertexId id =
            builder.addVertex(Vector3f{static_cast<f32>(v), static_cast<f32>(v % 2), 0.0f});
        builder.addInfluence(id, v < kSpan / 2 ? kArm : kChest, 1.0f);
    }
    for (u32 v = 0; v + 2 < kSpan; ++v) {
        const geom::FaceId face = builder.addTriangle(geom::VertexId(v), geom::VertexId(v + 1),
                                                      geom::VertexId(v + 2), 0);
        for (u32 corner = 0; corner < 3; ++corner) {
            builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
        }
    }
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);

    const std::vector<std::vector<std::pair<u32, f32>>> before = [&] {
        std::vector<std::vector<std::pair<u32, f32>>> out;
        for (u32 p = 0; p < points.pointCount; ++p) {
            out.push_back(weightsOf(mesh, points.membersOf(p)[0]));
        }
        return out;
    }();

    // One point, so every neighbour it reads is outside the scope. What it
    // should become is the mean of that whole ring, mixed at the strength --
    // and the ring is the number this case is really about.
    const u32 only = points.pointOfVertex(2);
    const std::span<const u32> ring = points.ringOf(only);
    REQUIRE(ring.size() >= 2u);
    f32 armMean = 0.0f;
    f32 chestMean = 0.0f;
    for (const u32 neighbour : ring) {
        const u32 vertex = points.membersOf(neighbour)[0];
        armMean += weightOn(mesh, vertex, kArm) / static_cast<f32>(ring.size());
        chestMean += weightOn(mesh, vertex, kChest) / static_cast<f32>(ring.size());
    }
    // A ring wholly on one side would make the mutant below look right.
    REQUIRE(chestMean > 0.0f);
    REQUIRE(armMean > 0.0f);

    constexpr f32 kStrength = 0.5f;
    const skinning::SkinResult result =
        skinning::Smooth(mesh, rig, points, {std::vector<u32>{only}, {}}, kStrength, 1);
    CHECK(result.changed == 1u);

    const f32 wantArm = (1.0f - kStrength) * 1.0f + kStrength * armMean;
    const f32 wantChest = kStrength * chestMean;
    const f32 total = wantArm + wantChest;
    const u32 vertex = points.membersOf(only)[0];
    CHECK(near(weightOn(mesh, vertex, kArm), wantArm / total));
    CHECK(near(weightOn(mesh, vertex, kChest), wantChest / total));

    for (u32 p = 0; p < points.pointCount; ++p) {
        CAPTURE(p);
        const u32 first = points.membersOf(p)[0];
        if (p != only) {
            CHECK(weightsOf(mesh, first) == before[p]);
        }
        for (const u32 member : points.membersOf(p)) {
            CHECK(weightsOf(mesh, member) == weightsOf(mesh, first));
        }
    }
    checkNormalAndSorted(mesh);
}

TEST_CASE("S2 Limit keeps the locked influences first", "[wem][skin][ops]") {
    NodeTree rig = makeRig();
    rig.nodes[kRoot].skin.locked = true;
    Mesh mesh = makeStrip(1, {{kArm, 0.5f}, {kChest, 0.3f}, {kHand, 0.15f}, {kRoot, 0.05f}});
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> scope = allPoints(points);
    skinning::Limit(mesh, rig, points, {scope, {}}, 2);

    // Root is the lightest and would have gone first, but it is locked.
    CHECK(weightOn(mesh, 0, kRoot) == 0.05f);
    CHECK(mesh.skin.forVertex(0).size() == 2u);
    checkNormalAndSorted(mesh);
}

TEST_CASE("S2 Replace merges into an existing entry", "[wem][skin][ops]") {
    const NodeTree rig = makeRig();
    Mesh mesh = makeStrip(1, {{kArm, 0.5f}, {kChest, 0.3f}, {kHand, 0.2f}});
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> scope = allPoints(points);
    skinning::Replace(mesh, rig, points, {scope, {}}, kHand, kArm);

    const std::vector<std::pair<u32, f32>> got = weightsOf(mesh, 0);
    REQUIRE(got.size() == 2u);
    CHECK(got[0].first == kChest);
    CHECK(near(got[0].second, 0.3f));
    CHECK(got[1].first == kArm);
    CHECK(near(got[1].second, 0.7f));
    checkNormalAndSorted(mesh);

    // A locked end refuses the move rather than half-doing it.
    NodeTree locked = makeRig();
    locked.nodes[kArm].skin.locked = true;
    Mesh other = makeStrip(1, {{kArm, 0.5f}, {kHand, 0.5f}});
    const skinning::PointTable otherPoints = skinning::BuildPointTable(other);
    const skinning::SkinResult result = skinning::Replace(
        other, locked, otherPoints, {allPoints(otherPoints), {}}, kHand, kArm);
    CHECK(result.changed == 0u);
    CHECK(weightOn(other, 0, kHand) == 0.5f);
}

TEST_CASE("S2 a bad weight is rejected, not propagated", "[wem][skin][ops]") {
    const NodeTree rig = makeRig();
    Mesh mesh = makeStrip(2, {{kArm, 0.5f}, {kChest, 0.5f}});
    // A document can hold these: WEM keeps what a file shipped, and `Validate`
    // reports them (§6.4). An operation must not spread them.
    mesh.skin.assignVertex(0, std::vector<geom::Influence>{{kArm, std::nanf("")}, {kChest, 0.5f}});
    mesh.skin.assignVertex(1, std::vector<geom::Influence>{{kArm, -0.5f}, {kChest, 0.5f}});
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    skinning::Normalize(mesh, rig, points, {allPoints(points), {}});

    for (u32 v = 0; v < mesh.skin.vertexCount(); ++v) {
        for (const geom::Influence& influence : mesh.skin.forVertex(v)) {
            CHECK(std::isfinite(influence.weight));
            CHECK(influence.weight > 0.0f);
        }
    }
    checkNormalAndSorted(mesh);
}

TEST_CASE("S2 a point writes every member the same", "[wem][skin][ops]") {
    const NodeTree rig = makeRig();
    // Two triangles that share a position but not a vertex: a UV seam.
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "seam";
    builder.addSection(std::move(section));
    const Vector3f shared{1.0f, 0.0f, 0.0f};
    for (u32 t = 0; t < 2; ++t) {
        const geom::VertexId a = builder.addVertex(shared);
        const geom::VertexId b = builder.addVertex(Vector3f{2.0f + static_cast<f32>(t), 0, 0});
        const geom::VertexId c = builder.addVertex(Vector3f{2.0f + static_cast<f32>(t), 1, 0});
        for (const geom::VertexId id : {a, b, c}) {
            builder.addInfluence(id, kArm, 1.0f);
        }
        const geom::FaceId face = builder.addTriangle(a, b, c, 0);
        for (u32 corner = 0; corner < 3; ++corner) {
            builder.setCornerAttr(face, corner, geom::names::uv(0),
                                  Vector2f{static_cast<f32>(t), 0});
        }
    }
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const u32 seam = points.pointOfVertex(0);
    REQUIRE(points.membersOf(seam).size() == 2u);

    const std::vector<u32> scope{seam};
    skinning::Set(mesh, rig, points, {scope, {}}, kChest, 0.4f);
    CHECK(weightsOf(mesh, points.membersOf(seam)[0]) ==
          weightsOf(mesh, points.membersOf(seam)[1]));
    checkNormalAndSorted(mesh);
}

TEST_CASE("S2 Unify gives a point's members their mean", "[wem][skin][ops]") {
    const NodeTree rig = makeRig();
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "seam";
    builder.addSection(std::move(section));
    const Vector3f shared{1.0f, 0.0f, 0.0f};
    std::vector<geom::VertexId> twins;
    for (u32 t = 0; t < 2; ++t) {
        const geom::VertexId a = builder.addVertex(shared);
        twins.push_back(a);
        const geom::VertexId b = builder.addVertex(Vector3f{2.0f + static_cast<f32>(t), 0, 0});
        const geom::VertexId c = builder.addVertex(Vector3f{2.0f + static_cast<f32>(t), 1, 0});
        builder.addInfluence(a, t == 0 ? kArm : kChest, 1.0f);
        builder.addInfluence(b, kArm, 1.0f);
        builder.addInfluence(c, kArm, 1.0f);
        const geom::FaceId face = builder.addTriangle(a, b, c, 0);
        for (u32 corner = 0; corner < 3; ++corner) {
            builder.setCornerAttr(face, corner, geom::names::uv(0),
                                  Vector2f{static_cast<f32>(t), 0});
        }
    }
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const u32 seam = points.pointOfVertex(twins[0].index());
    REQUIRE(points.membersOf(seam).size() == 2u);
    CHECK(skinning::DisagreeingPoints(mesh, points) == std::vector<u32>{seam});

    skinning::Unify(mesh, rig, points, {std::vector<u32>{seam}, {}});
    CHECK(skinning::DisagreeingPoints(mesh, points).empty());
    CHECK(near(weightOn(mesh, points.membersOf(seam)[0], kArm), 0.5f));
    CHECK(near(weightOn(mesh, points.membersOf(seam)[0], kChest), 0.5f));
    checkNormalAndSorted(mesh);
}

TEST_CASE("S2 a brush's strength is a partial write", "[wem][skin][ops]") {
    const NodeTree rig = makeRig();
    Mesh mesh = makeStrip(1, {{kArm, 1.0f}});
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> scope = allPoints(points);
    const std::vector<f32> falloff(scope.size(), 0.5f);
    skinning::Set(mesh, rig, points, {scope, falloff}, kChest, 1.0f);
    // Half way from 0 to 1 on the chest, and §6.1 takes the rest off the arm.
    CHECK(near(weightOn(mesh, 0, kChest), 0.5f));
    CHECK(near(weightOn(mesh, 0, kArm), 0.5f));
    checkNormalAndSorted(mesh);
}

TEST_CASE("S2 Soften blends a band between two rigid parts", "[wem][skin][ops]") {
    const NodeTree rig = makeRig();
    // A ladder of quads: the ends are rigid to arm and chest, the middle is the
    // band Soften writes.
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "hose";
    builder.addSection(std::move(section));
    constexpr u32 kRungs = 7;
    for (u32 r = 0; r < kRungs; ++r) {
        for (u32 side = 0; side < 2; ++side) {
            const geom::VertexId id = builder.addVertex(
                Vector3f{static_cast<f32>(r), static_cast<f32>(side), 0.0f});
            builder.addInfluence(id, r < kRungs / 2 ? kArm : kChest, 1.0f);
        }
    }
    for (u32 r = 0; r + 1 < kRungs; ++r) {
        const u32 base = r * 2;
        for (const auto& triangle : {std::array<u32, 3>{base, base + 1, base + 2},
                                     std::array<u32, 3>{base + 1, base + 3, base + 2}}) {
            const geom::FaceId face =
                builder.addTriangle(geom::VertexId(triangle[0]), geom::VertexId(triangle[1]),
                                    geom::VertexId(triangle[2]), 0);
            for (u32 corner = 0; corner < 3; ++corner) {
                builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
            }
        }
    }
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);

    std::vector<u32> band;
    for (u32 r = 2; r <= 4; ++r) {
        band.push_back(points.pointOfVertex(r * 2));
        band.push_back(points.pointOfVertex(r * 2 + 1));
    }
    const skinning::SkinResult result =
        skinning::Soften(mesh, rig, points, {band, {}}, kArm, kChest);
    CHECK(result.changed == band.size());
    // Along the band, the arm's share falls and the chest's rises.
    f32 previous = 2.0f;
    for (u32 r = 2; r <= 4; ++r) {
        const f32 arm = weightOn(mesh, r * 2, kArm);
        CHECK(arm < previous);
        CHECK(arm > 0.0f);
        CHECK(arm < 1.0f);
        previous = arm;
    }
    checkNormalAndSorted(mesh);
}

// ============================================================================
// The two things a generator needs of the algebra (§8.1, §8.3)
// ============================================================================

TEST_CASE("S2 a mesh no file skinned at all can be skinned", "[wem][skin][ops]") {
    // An EMPTY binding is not a binding of empty vertices: `assignVertex`
    // splices into an array with no row for the vertex yet, so before this was
    // handled at the write, skinning a fresh part wrote nothing at all and said
    // it had changed every point.
    const NodeTree rig = makeRig();
    Mesh mesh = makeStrip(2, {});
    REQUIRE(mesh.skin.empty());

    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    std::vector<u32> all(points.pointCount);
    for (u32 point = 0; point < points.pointCount; ++point) {
        all[point] = point;
    }

    const skinning::SkinResult result = skinning::Rigid(mesh, rig, points, {all, {}}, kArm);
    CHECK(result.changed == points.pointCount);
    REQUIRE_FALSE(mesh.skin.empty());
    CHECK(mesh.skin.vertexCount() == mesh.vertexCount());
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        CHECK(near(weightOn(mesh, v, kArm), 1.0f));
    }
    checkNormalAndSorted(mesh);
}

TEST_CASE("S2 a held bone keeps its weight exactly as a locked one does",
          "[wem][skin][ops]") {
    // A generator writes a BONE SET, and a point's weight on a bone outside it
    // is kept (§8.1). That is what a lock already means, so `SkinScope` says it
    // with a lock rather than with a second rule.
    const NodeTree rig = makeRig();
    Mesh mesh = makeStrip(2, {{kChest, 0.5f}, {kArm, 0.5f}});
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    std::vector<u32> all(points.pointCount);
    for (u32 point = 0; point < points.pointCount; ++point) {
        all[point] = point;
    }

    const std::vector<u32> held{kChest};
    const skinning::SkinResult result =
        skinning::Rigid(mesh, rig, points, {all, {}, held}, kHand);
    CHECK(result.changed == points.pointCount);
    for (u32 v = 0; v < mesh.vertexCount(); ++v) {
        CHECK(near(weightOn(mesh, v, kChest), 0.5f)); // outside the set, kept
        CHECK(near(weightOn(mesh, v, kHand), 0.5f));  // the target took the rest
        CHECK(near(weightOn(mesh, v, kArm), 0.0f));   // inside the set, replaced
    }
    checkNormalAndSorted(mesh);

    SECTION("and the parent the last unlocked weight falls back to is never a held one") {
        // Lowering the only unlocked influence sends its weight to the nearest
        // Bone ancestor that may take it. The chest is held, so the fallback
        // has to walk past it to the root rather than quietly writing a bone
        // the caller said to leave alone.
        Mesh single = makeStrip(1, {{kArm, 1.0f}});
        const skinning::PointTable table = skinning::BuildPointTable(single);
        std::vector<u32> scope(table.pointCount);
        for (u32 point = 0; point < table.pointCount; ++point) {
            scope[point] = point;
        }
        skinning::Set(single, rig, table, {scope, {}, held}, kArm, 0.25f);
        for (u32 v = 0; v < single.vertexCount(); ++v) {
            CHECK(near(weightOn(single, v, kArm), 0.25f));
            CHECK(near(weightOn(single, v, kChest), 0.0f));
            CHECK(near(weightOn(single, v, kRoot), 0.75f));
        }
    }
}

// ============================================================================
// Assign: a SET replaces the unlocked share (§7.4, §7.6, §8.2)
// ============================================================================

TEST_CASE("S2 Assign scales a set into the unlocked share", "[wem][skin][ops]") {
    NodeTree tree = makeRig();
    Mesh mesh = makeStrip(1, {{kRoot, 1.0f}});
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> scope = allPoints(points);

    // Deliberately not normalised: what a transfer's barycentric blend or an
    // envelope's falloffs hand over are proportions, not weights.
    const std::vector<geom::Influence> set{{kArm, 3.0f}, {kHand, 1.0f}};
    skinning::PointWeights given;
    for (u32 p = 0; p < points.pointCount; ++p) {
        given.add(set);
    }
    skinning::SkinScope span;
    span.points = scope;

    const skinning::SkinResult result = skinning::Assign(mesh, tree, points, span, given);
    CHECK(result.changed == points.pointCount);
    CHECK(result.refused == 0u);
    checkNormalAndSorted(mesh);
    CHECK(near(weightOn(mesh, 0, kArm), 0.75f));
    CHECK(near(weightOn(mesh, 0, kHand), 0.25f));
    CHECK(near(weightOn(mesh, 0, kRoot), 0.0f));

    SECTION("a strength is a blend from what the point held toward it") {
        Mesh half = makeStrip(1, {{kRoot, 1.0f}});
        const std::vector<f32> strengths(points.pointCount, 0.5f);
        skinning::SkinScope partial;
        partial.points = scope;
        partial.strength = strengths;
        skinning::Assign(half, tree, points, partial, given);
        checkNormalAndSorted(half);
        CHECK(near(weightOn(half, 0, kRoot), 0.5f));
        CHECK(near(weightOn(half, 0, kArm), 0.375f));
        CHECK(near(weightOn(half, 0, kHand), 0.125f));
    }
}

TEST_CASE("S2 Assign keeps a locked bone and drops it from what it was given",
          "[wem][skin][ops]") {
    NodeTree tree = makeRig();
    tree.nodes[kChest].skin.locked = true;
    Mesh mesh = makeStrip(1, {{kChest, 0.4f}, {kRoot, 0.6f}});
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> scope = allPoints(points);

    // The set names the locked bone as well, and heavily. It is dropped, and
    // the rest of the set shares the whole of `1 - L` rather than a part of it.
    const std::vector<geom::Influence> set{{kChest, 5.0f}, {kArm, 1.0f}};
    skinning::PointWeights given;
    for (u32 p = 0; p < points.pointCount; ++p) {
        given.add(set);
    }
    skinning::SkinScope span;
    span.points = scope;
    const skinning::SkinResult result = skinning::Assign(mesh, tree, points, span, given);
    CHECK(result.changed == points.pointCount);
    checkNormalAndSorted(mesh);
    CHECK(near(weightOn(mesh, 0, kChest), 0.4f));
    CHECK(near(weightOn(mesh, 0, kArm), 0.6f));
    CHECK(near(weightOn(mesh, 0, kRoot), 0.0f));

    SECTION("a set of nothing but locked bones is refused and changes nothing") {
        const std::vector<geom::Influence> only{{kChest, 1.0f}};
        skinning::PointWeights locked;
        for (u32 p = 0; p < points.pointCount; ++p) {
            locked.add(only);
        }
        Mesh untouched = makeStrip(1, {{kChest, 0.4f}, {kRoot, 0.6f}});
        const skinning::SkinResult refused =
            skinning::Assign(untouched, tree, points, span, locked);
        CHECK(refused.changed == 0u);
        CHECK(refused.refused == points.pointCount);
        CHECK(near(weightOn(untouched, 0, kRoot), 0.6f));
    }
}

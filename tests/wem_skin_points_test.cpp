// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Points, islands and the mirror maps (EDIT_MODE_SKIN_DESIGN.md §3.2-3.4,
/// §7.5; gate S3).
///
/// What one click means is the whole of this file: a UV seam is one point, two
/// meshes that touch are not, and a part seam is two islands where a UV seam is
/// one.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/nodes/tree.h>
#include <whiteout/models/wem/skinning/mirror.h>
#include <whiteout/models/wem/skinning/points.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// A triangle at @p offset, its three vertices appended in order.
void addTriangle(geom::MeshBuilder& builder, const Vector3f& a, const Vector3f& b,
                 const Vector3f& c, u32 section = 0, f32 uv = 0.0f) {
    const geom::VertexId ia = builder.addVertex(a);
    const geom::VertexId ib = builder.addVertex(b);
    const geom::VertexId ic = builder.addVertex(c);
    const geom::FaceId face = builder.addTriangle(ia, ib, ic, section);
    for (u32 corner = 0; corner < 3; ++corner) {
        builder.setCornerAttr(face, corner, geom::names::uv(0), Vector2f{uv, 0});
        builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
    }
}

geom::MeshBuilder startMesh(const char* name) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = name;
    builder.addSection(std::move(section));
    return builder;
}

} // namespace

TEST_CASE("S3 a UV seam is one point", "[wem][skin][points]") {
    // Two triangles meeting along an edge, each with its own UV, so the shared
    // edge is two pairs of co-located vertices and the half-edge topology does
    // not join them at all.
    geom::MeshBuilder builder = startMesh("seam");
    const Vector3f a{0, 0, 0};
    const Vector3f b{1, 0, 0};
    addTriangle(builder, a, b, Vector3f{0, 1, 0}, 0, 0.0f);
    addTriangle(builder, b, a, Vector3f{1, -1, 0}, 0, 1.0f);
    const Mesh mesh = builder.build().mesh;
    REQUIRE(mesh.vertexCount() == 6u);

    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    CHECK(points.pointCount == 4u);
    CHECK(points.pointOfVertex(0) == points.pointOfVertex(4)); // a and a
    CHECK(points.pointOfVertex(1) == points.pointOfVertex(3)); // b and b
    CHECK(points.membersOf(points.pointOfVertex(0)).size() == 2u);
    // And they are one island: a UV seam does not cut the surface.
    CHECK(points.islandCount == 1u);
}

TEST_CASE("S3 two meshes that touch are two points", "[wem][skin][points]") {
    // Points never span meshes (§1.3): a neck seam between a head and a body is
    // two parts that move differently, not one point.
    geom::MeshBuilder head = startMesh("head");
    addTriangle(head, Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{0, 1, 0});
    geom::MeshBuilder body = startMesh("body");
    addTriangle(body, Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{0, -1, 0});

    const Mesh headMesh = head.build().mesh;
    const Mesh bodyMesh = body.build().mesh;
    const skinning::PointTable headPoints = skinning::BuildPointTable(headMesh);
    const skinning::PointTable bodyPoints = skinning::BuildPointTable(bodyMesh);
    CHECK(headPoints.pointCount == 3u);
    CHECK(bodyPoints.pointCount == 3u);
}

TEST_CASE("S3 a part seam is two islands", "[wem][skin][points]") {
    // Two triangles that share nothing: two islands, however close they are.
    geom::MeshBuilder builder = startMesh("parts");
    addTriangle(builder, Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{0, 1, 0});
    addTriangle(builder, Vector3f{5, 0, 0}, Vector3f{6, 0, 0}, Vector3f{5, 1, 0});
    const Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    CHECK(points.pointCount == 6u);
    CHECK(points.islandCount == 2u);
    CHECK(points.islandOf[points.pointOfVertex(0)] != points.islandOf[points.pointOfVertex(3)]);
}

TEST_CASE("S3 the ring crosses a seam", "[wem][skin][points]") {
    geom::MeshBuilder builder = startMesh("seam");
    const Vector3f a{0, 0, 0};
    const Vector3f b{1, 0, 0};
    addTriangle(builder, a, b, Vector3f{0, 1, 0}, 0, 0.0f);
    addTriangle(builder, b, a, Vector3f{1, -1, 0}, 0, 1.0f);
    const Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);

    // The far corner of the second triangle is one ring from the first's
    // corner, through the welded seam.
    const u32 far = points.pointOfVertex(5);
    const u32 near = points.pointOfVertex(2);
    const std::vector<u32> grown = skinning::GrowPoints(points, std::vector<u32>{near}, 2);
    CHECK(std::find(grown.begin(), grown.end(), far) != grown.end());
    const std::vector<u32> one = skinning::GrowPoints(points, std::vector<u32>{near}, 1);
    CHECK(std::find(one.begin(), one.end(), far) == one.end());
}

TEST_CASE("S3 every naming form mirrors", "[wem][skin][mirror]") {
    struct Row {
        const char* name;
        const char* mirrored;
    };
    const Row rows[] = {
        {"Bone_Arm_L", "Bone_Arm_R"},   {"Bone_Arm_R", "Bone_Arm_L"},
        {"Bip01 L Thigh", "Bip01 R Thigh"}, {"LeftHand", "RightHand"},
        {"RightHand", "LeftHand"},      {"hand.l", "hand.r"},
        {"Lf_Foot", "Rt_Foot"},         {"foot_lf", "foot_rt"},
    };
    for (const Row& row : rows) {
        CAPTURE(row.name);
        CHECK(skinning::MirroredName(row.name) == row.mirrored);
    }
    // A name with no side at all has no mirror by name.
    CHECK(skinning::MirroredName("Bone_Chest").empty());
    CHECK(skinning::MirroredName("Root").empty());
}

TEST_CASE("S3 the bone map falls through name, position and self", "[wem][skin][mirror]") {
    NodeTree tree;
    const auto add = [&](const char* name, const Vector3f& pivot) {
        Node node;
        node.name = name;
        node.kind = NodeKind::Bone;
        node.parent = kInvalidNode;
        node.pivot = pivot;
        node.local = Transform::identity();
        node.local.translation = pivot;
        node.resetPayloadForKind();
        tree.nodes.push_back(std::move(node));
    };
    add("Bone_Chest", Vector3f{0, 0, 10});      // 0: on the plane -> itself
    add("Bone_Arm_L", Vector3f{0, 3, 8});       // 1: by name
    add("Bone_Arm_R", Vector3f{0, -3, 8});      // 2
    add("wing01", Vector3f{0, 5, 4});           // 3: by position
    add("wing02", Vector3f{0, -5, 4});          // 4
    add("tail", Vector3f{-4, 0, 2});            // 5: on the plane

    const std::vector<skinning::BoneMirror> map = skinning::BuildBoneMirror(tree);
    REQUIRE(map.size() == tree.size());
    CHECK(map[0].node == 0u);
    CHECK(map[0].source == skinning::MirrorSource::Self);
    CHECK(map[1].node == 2u);
    CHECK(map[1].source == skinning::MirrorSource::Name);
    CHECK(map[2].node == 1u);
    CHECK(map[3].node == 4u);
    CHECK(map[3].source == skinning::MirrorSource::Position);
    CHECK(map[4].node == 3u);
    CHECK(map[5].node == 5u);
    CHECK(map[5].source == skinning::MirrorSource::Self);

    // The saved override beats both, whatever the names say (§13.4).
    tree.nodes[1].skin.mirror = 3;
    const std::vector<skinning::BoneMirror> overridden = skinning::BuildBoneMirror(tree);
    CHECK(overridden[1].node == 3u);
    CHECK(overridden[1].source == skinning::MirrorSource::Override);
}

TEST_CASE("S3 the point map mirrors whatever the vertex order is", "[wem][skin][mirror]") {
    // Two triangles, one on each side of y = 0, with their corners appended in
    // different orders: a map by index would get this wrong.
    geom::MeshBuilder builder = startMesh("halves");
    addTriangle(builder, Vector3f{0, 1, 0}, Vector3f{1, 2, 0}, Vector3f{2, 1, 0});
    addTriangle(builder, Vector3f{2, -1, 0}, Vector3f{0, -1, 0}, Vector3f{1, -2, 0});
    const Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> map = skinning::BuildPointMirror(mesh, points);
    REQUIRE(map.size() == points.pointCount);

    const u32 left = points.pointOfVertex(0);  // (0, 1)
    const u32 right = points.pointOfVertex(4); // (0, -1)
    CHECK(map[left] == right);
    CHECK(map[right] == left);
    // Mirroring twice returns the original, for every matched point.
    u32 unmatched = 0;
    for (u32 p = 0; p < points.pointCount; ++p) {
        if (map[p] == kInvalidIndex) {
            ++unmatched;
            continue;
        }
        CHECK(map[map[p]] == p);
    }
    CHECK(unmatched == 0u);
}

TEST_CASE("S3 a point with no mirror is counted, not guessed", "[wem][skin][mirror]") {
    geom::MeshBuilder builder = startMesh("lopsided");
    addTriangle(builder, Vector3f{0, 1, 0}, Vector3f{1, 2, 0}, Vector3f{2, 1, 0});
    addTriangle(builder, Vector3f{9, -7, 3}, Vector3f{8, -9, 3}, Vector3f{7, -8, 3});
    const Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const std::vector<u32> map = skinning::BuildPointMirror(mesh, points);
    u32 unmatched = 0;
    for (const u32 other : map) {
        unmatched += other == kInvalidIndex ? 1 : 0;
    }
    CHECK(unmatched == points.pointCount);
}

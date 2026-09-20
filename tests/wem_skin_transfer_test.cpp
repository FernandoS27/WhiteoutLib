// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Weights taken from other meshes (EDIT_MODE_SKIN_DESIGN.md §7.6).
///
/// The question a transfer answers is "which bone carries the surface nearest
/// this point", so the arms here are about the three things that can go wrong
/// with it: the blend across a triangle, a point with nothing in reach, and a
/// source that has no weights to give.

#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/nodes/tree.h>
#include <whiteout/models/wem/skinning/points.h>
#include <whiteout/models/wem/skinning/transfer.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

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
    add("left", 0);            // 1
    add("right", 0);           // 2
    return tree;
}

constexpr u32 kLeft = 1;
constexpr u32 kRight = 2;

geom::MeshBuilder startMesh(const char* name) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = name;
    builder.addSection(std::move(section));
    return builder;
}

/// A quad in the z = @p z plane over x and y in `[0, 10]`, two triangles. When
/// @p skinned, its left edge binds `left` and its right edge `right`, so the
/// weight across it is a straight ramp in x and the barycentric blend has an
/// answer that can be written down.
Mesh makeSheet(f32 z, bool skinned) {
    geom::MeshBuilder builder = startMesh("sheet");
    const Vector3f corners[4]{{0, 0, z}, {10, 0, z}, {10, 10, z}, {0, 10, z}};
    geom::VertexId ids[4]{geom::VertexId(0), geom::VertexId(0), geom::VertexId(0),
                          geom::VertexId(0)};
    for (u32 i = 0; i < 4; ++i) {
        ids[i] = builder.addVertex(corners[i]);
        if (skinned) {
            builder.addInfluence(ids[i], corners[i].x > 5.0f ? kRight : kLeft, 1.0f);
        }
    }
    const geom::FaceId faces[2]{builder.addTriangle(ids[0], ids[1], ids[2], 0),
                                builder.addTriangle(ids[0], ids[2], ids[3], 0)};
    for (const geom::FaceId face : faces) {
        for (u32 corner = 0; corner < 3; ++corner) {
            builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
        }
    }
    return builder.build().mesh;
}

/// One triangle at @p z, its corners at @p a, @p b and @p c in the plane.
Mesh makePatch(f32 z, const Vector2f& a, const Vector2f& b, const Vector2f& c) {
    geom::MeshBuilder builder = startMesh("patch");
    const geom::VertexId ia = builder.addVertex(Vector3f{a.x, a.y, z});
    const geom::VertexId ib = builder.addVertex(Vector3f{b.x, b.y, z});
    const geom::VertexId ic = builder.addVertex(Vector3f{c.x, c.y, z});
    const geom::FaceId face = builder.addTriangle(ia, ib, ic, 0);
    for (u32 corner = 0; corner < 3; ++corner) {
        builder.setCornerAttr(face, corner, geom::names::kNormal, Vector3f{0, 0, 1});
    }
    return builder.build().mesh;
}

f32 weightOn(const Mesh& mesh, u32 vertex, u32 bone) {
    for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
        if (influence.bone == bone) {
            return influence.weight;
        }
    }
    return 0.0f;
}

bool near(f32 a, f32 b, f32 tolerance = 1e-4f) {
    return std::abs(a - b) <= tolerance;
}

} // namespace

TEST_CASE("a transfer blends the nearest triangle's corners", "[wem][skin][transfer]") {
    const NodeTree tree = makeRig();
    const Mesh source = makeSheet(0.0f, /*skinned=*/true);
    Mesh target = makePatch(0.2f, Vector2f{2, 2}, Vector2f{8, 2}, Vector2f{5, 8});
    const skinning::PointTable points = skinning::BuildPointTable(target);

    const Mesh* sources[1]{&source};
    const skinning::TransferResult result =
        skinning::Transfer(target, tree, points, {}, sources, {});
    CHECK(result.transferred == points.pointCount);
    CHECK(result.unreached == 0u);
    CHECK(result.weights.changed == points.pointCount);

    // The source's weight is a straight ramp in x -- 1 on `left` at x = 0 and 1
    // on `right` at x = 10 -- so the barycentric blend has an exact answer.
    CHECK(near(weightOn(target, 0, kLeft), 0.8f));
    CHECK(near(weightOn(target, 0, kRight), 0.2f));
    CHECK(near(weightOn(target, 1, kLeft), 0.2f));
    CHECK(near(weightOn(target, 1, kRight), 0.8f));
    CHECK(near(weightOn(target, 2, kLeft), 0.5f));
    CHECK(near(weightOn(target, 2, kRight), 0.5f));
}

TEST_CASE("a point with no source in reach keeps what it has", "[wem][skin][transfer]") {
    const NodeTree tree = makeRig();
    const Mesh source = makeSheet(0.0f, /*skinned=*/true);
    Mesh target = makePatch(40.0f, Vector2f{2, 2}, Vector2f{8, 2}, Vector2f{5, 8});
    const skinning::PointTable points = skinning::BuildPointTable(target);

    const Mesh* sources[1]{&source};
    skinning::TransferOptions options;
    options.maxDistance = 1.0f;
    const skinning::TransferResult result =
        skinning::Transfer(target, tree, points, {}, sources, options);
    CHECK(result.transferred == 0u);
    CHECK(result.unreached == points.pointCount);
    CHECK(result.weights.changed == 0u);
    for (u32 vertex = 0; vertex < target.vertexCount(); ++vertex) {
        CHECK(target.skin.forVertex(vertex).empty());
    }

    SECTION("and with no limit the same point reaches it") {
        const skinning::TransferResult far =
            skinning::Transfer(target, tree, points, {}, sources, {});
        CHECK(far.transferred == points.pointCount);
        CHECK(far.unreached == 0u);
    }
}

TEST_CASE("a source with no weights and the target itself are both skipped",
          "[wem][skin][transfer]") {
    const NodeTree tree = makeRig();
    const Mesh bare = makeSheet(0.0f, /*skinned=*/false);
    Mesh target = makePatch(0.2f, Vector2f{2, 2}, Vector2f{8, 2}, Vector2f{5, 8});
    const skinning::PointTable points = skinning::BuildPointTable(target);

    const Mesh* sources[2]{&bare, &target};
    const skinning::TransferResult result =
        skinning::Transfer(target, tree, points, {}, sources, {});
    CHECK(result.transferred == 0u);
    CHECK(result.unreached == points.pointCount);
    for (u32 vertex = 0; vertex < target.vertexCount(); ++vertex) {
        CHECK(target.skin.forVertex(vertex).empty());
    }
}

TEST_CASE("a transfer holds the bones outside its set", "[wem][skin][transfer]") {
    const NodeTree tree = makeRig();
    const Mesh source = makeSheet(0.0f, /*skinned=*/true);
    Mesh target = makePatch(0.2f, Vector2f{2, 2}, Vector2f{8, 2}, Vector2f{5, 8});
    // Everything starts on the root, which is outside the set below. The
    // binding has to be reset first: an empty one is not a binding of empty
    // vertices, and a splice into it would write nothing.
    target.skin.reset(target.vertexCount());
    const std::vector<geom::Influence> onRoot{{0, 1.0f}};
    for (u32 vertex = 0; vertex < target.vertexCount(); ++vertex) {
        target.skin.assignVertex(vertex, onRoot);
    }
    const skinning::PointTable points = skinning::BuildPointTable(target);

    const Mesh* sources[1]{&source};
    skinning::TransferOptions options;
    options.bones = {kLeft, kRight};
    const skinning::TransferResult result =
        skinning::Transfer(target, tree, points, {}, sources, options);
    CHECK(result.transferred == points.pointCount);
    for (u32 vertex = 0; vertex < target.vertexCount(); ++vertex) {
        // The root is held, so it keeps the whole of what it had and the
        // transferred set has nothing left to take.
        CHECK(near(weightOn(target, vertex, 0), 1.0f));
    }
}

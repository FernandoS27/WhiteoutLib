// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P1c — the node model. The design's requirement is that a node is
/// self-contained and that removing one is easy at runtime, so what is asserted
/// here is the *lifecycle*: remove marks, compact collects and remaps, every
/// §10.6 referencer follows, and a survivor's world bind pose does not move.

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/nodes/tree.h>
#include <whiteout/models/wem/nodes/visitor.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

Node makeNode(const char* name, NodeKind kind, u32 parent, const Vector3f& translation) {
    Node node;
    node.name = name;
    node.kind = kind;
    node.parent = parent;
    node.local.translation = translation;
    node.resetPayloadForKind();
    return node;
}

/// root -> hip -> thigh -> shin, plus a light on the hip and an attachment on
/// the shin. Six nodes, four kinds, a real depth.
NodeTree makeRig() {
    NodeTree tree;
    tree.poseSchema.push_back(PoseSchema{"bind", PoseSpace::Model, false});
    tree.add(makeNode("root", NodeKind::Bone, kInvalidNode, Vector3f{0, 0, 0}));
    tree.add(makeNode("hip", NodeKind::Bone, 0, Vector3f{0, 0, 1}));
    tree.add(makeNode("thigh", NodeKind::Bone, 1, Vector3f{0, 0, 1}));
    tree.add(makeNode("shin", NodeKind::Bone, 2, Vector3f{0, 0, 1}));
    tree.add(makeNode("hipLight", NodeKind::Light, 1, Vector3f{1, 0, 0}));
    tree.add(makeNode("footHook", NodeKind::Attachment, 3, Vector3f{0, 0, 0.5f}));
    return tree;
}

bool nearlyEqual(const Vector3f& a, const Vector3f& b, f32 tolerance = 1e-5f) {
    return std::abs(a.x - b.x) < tolerance && std::abs(a.y - b.y) < tolerance &&
           std::abs(a.z - b.z) < tolerance;
}

/// A single-triangle mesh whose three vertices bind to the given bones.
Mesh makeSkinnedMesh(const char* name, u32 boneA, u32 boneB, u32 boneC) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "s";
    builder.addSection(section);
    builder.addVertex(Vector3f{0, 0, 0});
    builder.addVertex(Vector3f{1, 0, 0});
    builder.addVertex(Vector3f{0, 1, 0});
    builder.addInfluence(geom::VertexId(0), boneA, 1.0f);
    builder.addInfluence(geom::VertexId(1), boneB, 0.5f);
    builder.addInfluence(geom::VertexId(1), boneC, 0.5f);
    builder.addInfluence(geom::VertexId(2), boneC, 1.0f);
    builder.addTriangle(geom::VertexId(0), geom::VertexId(1), geom::VertexId(2));
    auto outcome = builder.build();
    outcome.mesh.name = name;
    return std::move(outcome.mesh);
}

} // namespace

TEST_CASE("wem node tree hierarchy view is derived and deterministic", "[wem][nodes]") {
    NodeTree tree = makeRig();
    CHECK(tree.parentsPrecedeChildren());
    CHECK_FALSE(tree.hasHierarchy());

    tree.ensureHierarchy();
    CHECK(tree.hasHierarchy());

    const auto roots = tree.roots();
    REQUIRE(roots.size() == 1);
    CHECK(roots[0] == 0);

    // Children come out in index order, which is what makes traversal comparable
    // in a golden test.
    const auto hipChildren = tree.children(1);
    REQUIRE(hipChildren.size() == 2);
    CHECK(hipChildren[0] == 2);
    CHECK(hipChildren[1] == 4);

    // Depth-first pre-order, the node itself first.
    std::vector<u32> order;
    for (u32 n : tree.subtree(1)) {
        order.push_back(n);
    }
    CHECK(order == std::vector<u32>{1, 2, 3, 5, 4});

    CHECK(tree.depth(0) == 0);
    CHECK(tree.depth(3) == 3);
    CHECK(tree.depth(5) == 4);

    // Two subtree ranges can be held at once — the Euler-tour layout, not a
    // shared scratch buffer.
    const auto first = tree.subtree(2);
    const auto second = tree.subtree(4);
    CHECK(first.size() == 3);
    CHECK(second.size() == 1);

    // Any structural change drops the view.
    tree.add(makeNode("extra", NodeKind::Helper, 0, Vector3f{0, 0, 0}));
    CHECK_FALSE(tree.hasHierarchy());
}

TEST_CASE("wem node tree indexes by kind", "[wem][nodes]") {
    NodeTree tree = makeRig();
    const auto bones = tree.ofKind(NodeKind::Bone);
    REQUIRE(bones.size() == 4);
    CHECK(bones[0] == 0);
    CHECK(bones[3] == 3);

    const auto lights = tree.ofKind(NodeKind::Light);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0] == 4);

    CHECK(tree.ofKind(NodeKind::Camera).empty());
}

TEST_CASE("wem node world bind composes up the chain", "[wem][nodes]") {
    NodeTree tree = makeRig();
    CHECK(nearlyEqual(tree.worldBind(0).translation, Vector3f{0, 0, 0}));
    CHECK(nearlyEqual(tree.worldBind(1).translation, Vector3f{0, 0, 1}));
    CHECK(nearlyEqual(tree.worldBind(3).translation, Vector3f{0, 0, 3}));
    CHECK(nearlyEqual(tree.worldBind(5).translation, Vector3f{0, 0, 3.5f}));

    SECTION("an inherit flag stops the corresponding component") {
        tree.nodes[2].flags |= NodeFlags::DontInheritTranslation;
        // thigh no longer picks up the hip's offset, so it and everything under
        // it sit one unit lower.
        CHECK(nearlyEqual(tree.worldBind(2).translation, Vector3f{0, 0, 1}));
        CHECK(nearlyEqual(tree.worldBind(3).translation, Vector3f{0, 0, 2}));
    }

    SECTION("a model-space node's local is its world") {
        tree.nodes[2].flags |= NodeFlags::ModelSpace;
        tree.nodes[2].local.translation = Vector3f{5, 0, 0};
        CHECK(nearlyEqual(tree.worldBind(2).translation, Vector3f{5, 0, 0}));
        CHECK(nearlyEqual(tree.worldBind(3).translation, Vector3f{5, 0, 1}));
    }
}

TEST_CASE("wem node poses live on the node, schema on the tree", "[wem][nodes]") {
    // §10.5: a bone's poses leave with the bone. The schema is a list because D3
    // forces it — five transforms per bone, of which A and B genuinely differ.
    NodeTree tree = makeRig();
    tree.poseSchema.clear();
    tree.poseSchema.push_back(PoseSchema{"bindA", PoseSpace::Model, false});
    tree.poseSchema.push_back(PoseSchema{"bindAInverse", PoseSpace::Model, true});
    tree.poseSchema.push_back(PoseSchema{"local", PoseSpace::ParentRelative, false});
    tree.authoritativePose = 0;

    tree.conformPoses();
    for (u32 n : tree.ofKind(NodeKind::Bone)) {
        INFO("bone " << n);
        REQUIRE(tree.nodes[n].poses.size() == 3);
        CHECK(nearlyEqual(tree.nodes[n].poses[0].translation, tree.worldBind(n).translation));
        CHECK(nearlyEqual(tree.nodes[n].poses[2].translation, tree.nodes[n].local.translation));
        // The inverse pose really inverts.
        const Transform composed = Compose(tree.nodes[n].poses[0], tree.nodes[n].poses[1]);
        CHECK(nearlyEqual(composed.translation, Vector3f{0, 0, 0}));
    }

    // Non-bone kinds carry none.
    CHECK(tree.nodes[4].poses.empty());
    CHECK(tree.nodes[5].poses.empty());
}

TEST_CASE("wem node payload matches its kind", "[wem][nodes]") {
    NodeTree tree = makeRig();
    for (u32 i = 0; i < tree.size(); ++i) {
        INFO("node " << i << " (" << ToString(tree.nodes[i].kind) << ")");
        CHECK(tree.nodes[i].payloadMatchesKind());
    }

    // Changing the kind without resetting the payload is exactly the structural
    // error the redundancy exists to catch.
    tree.nodes[4].kind = NodeKind::Camera;
    CHECK_FALSE(tree.nodes[4].payloadMatchesKind());
    tree.nodes[4].resetPayloadForKind();
    CHECK(tree.nodes[4].payloadMatchesKind());
    CHECK(std::get_if<CameraPayload>(&tree.nodes[4].payload) != nullptr);
}

TEST_CASE("wem node visitation dispatches per kind", "[wem][nodes]") {
    NodeTree tree = makeRig();

    SECTION("compile-time overload set") {
        u32 bones = 0;
        u32 lights = 0;
        u32 others = 0;
        VisitAll(tree, overloaded{
                           [&](Node&, BonePayload&) { ++bones; },
                           [&](Node&, LightPayload&) { ++lights; },
                           [&](auto&, auto&) { ++others; },
                       });
        CHECK(bones == 4);
        CHECK(lights == 1);
        CHECK(others == 1); // the attachment
    }

    SECTION("runtime visitor, hierarchy order") {
        struct Recorder : NodeVisitor {
            std::vector<u32> order;
            u32 lights = 0;
            void OnBone(u32 id, Node&, BonePayload&) override {
                order.push_back(id);
            }
            void OnLight(u32 id, Node&, LightPayload&) override {
                order.push_back(id);
                ++lights;
            }
            void OnAttachment(u32 id, Node&, AttachmentPayload&) override {
                order.push_back(id);
            }
        };
        Recorder recorder;
        AcceptDepthFirst(tree, recorder);
        CHECK(recorder.lights == 1);
        CHECK(recorder.order == std::vector<u32>{0, 1, 2, 3, 5, 4});

        Recorder flat;
        Accept(tree, flat);
        CHECK(flat.order == std::vector<u32>{0, 1, 2, 3, 4, 5});
    }
}

TEST_CASE("wem node removal refuses to cut a skinned bone by default", "[wem][nodes]") {
    NodeTree tree = makeRig();
    Mesh mesh = makeSkinnedMesh("leg", 1, 2, 3);
    std::vector<Mesh> meshes;
    meshes.push_back(std::move(mesh));

    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(meshes.data(), meshes.size());

    auto result = RemoveNode(tree, 2, RemovePolicy::ReparentChildren, SkinPolicy::Refuse, true,
                             referencers);
    CHECK_FALSE(result.removed);
    CHECK(result.diagnostics.hasErrors());
    CHECK(result.diagnostics.countOf(DiagCode::NodeRemovalRefused) == 1);
    // Nothing moved.
    CHECK_FALSE(IsNodeRemoved(tree, 2));
    CHECK(tree.nodes[3].parent == 2);
}

TEST_CASE("wem node removal reassigns influences on request", "[wem][nodes]") {
    NodeTree tree = makeRig();
    std::vector<Mesh> meshes;
    meshes.push_back(makeSkinnedMesh("leg", 1, 2, 3));

    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(meshes.data(), meshes.size());

    const Transform shinWorldBefore = tree.worldBind(3);
    const Transform hookWorldBefore = tree.worldBind(5);

    auto result = RemoveNode(tree, 2, RemovePolicy::ReparentChildren,
                             SkinPolicy::ReassignToParent, true, referencers);
    REQUIRE(result.removed);
    CHECK_FALSE(result.diagnostics.hasErrors());
    CHECK(result.diagnostics.countOf(DiagCode::SkinInfluenceReassigned) == 1);

    // The thigh's influences moved to the hip, its parent.
    const auto& skin = meshes[0].skin;
    const auto vertex1 = skin.forVertex(1);
    REQUIRE(vertex1.size() == 2);
    std::vector<u32> bones{vertex1[0].bone, vertex1[1].bone};
    std::sort(bones.begin(), bones.end());
    CHECK(bones == std::vector<u32>{1, 3});

    // The shin was reparented onto the hip and — because preserveWorld is on —
    // has not moved in world space. Nor has anything under it.
    CHECK(tree.nodes[3].parent == 1);
    CHECK(nearlyEqual(tree.worldBind(3).translation, shinWorldBefore.translation));
    CHECK(nearlyEqual(tree.worldBind(5).translation, hookWorldBefore.translation));
    CHECK(result.diagnostics.countOf(DiagCode::BindPoseRecomposed) == 1);

    // The node is marked, not gone: every index a caller holds still resolves.
    CHECK(IsNodeRemoved(tree, 2));
    CHECK(tree.size() == 6);
}

TEST_CASE("wem node removal merges duplicate influences", "[wem][nodes]") {
    // A vertex weighted to both the removed bone and its parent must end up with
    // one influence, not two on the same bone.
    NodeTree tree = makeRig();
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = "s";
    builder.addSection(section);
    builder.addVertex(Vector3f{0, 0, 0});
    builder.addVertex(Vector3f{1, 0, 0});
    builder.addVertex(Vector3f{0, 1, 0});
    builder.addInfluence(geom::VertexId(0), 1, 0.4f); // hip
    builder.addInfluence(geom::VertexId(0), 2, 0.6f); // thigh -> becomes hip
    builder.addTriangle(geom::VertexId(0), geom::VertexId(1), geom::VertexId(2));
    std::vector<Mesh> meshes;
    meshes.push_back(std::move(builder.build().mesh));
    meshes[0].name = "merge";

    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(meshes.data(), meshes.size());
    auto result = RemoveNode(tree, 2, RemovePolicy::ReparentChildren,
                             SkinPolicy::ReassignToParent, true, referencers);
    REQUIRE(result.removed);

    const auto vertex0 = meshes[0].skin.forVertex(0);
    REQUIRE(vertex0.size() == 1);
    CHECK(vertex0[0].bone == 1);
    CHECK(vertex0[0].weight == 1.0f);
}

TEST_CASE("wem node removal takes the subtree when asked", "[wem][nodes]") {
    NodeTree tree = makeRig();
    std::vector<Mesh> meshes;
    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(meshes.data(), meshes.size());

    auto result = RemoveNode(tree, 2, RemovePolicy::RemoveSubtree, SkinPolicy::Refuse, true,
                             referencers);
    REQUIRE(result.removed);
    CHECK(result.removedNodes == std::vector<u32>{2, 3, 5});
    CHECK(IsNodeRemoved(tree, 3));
    CHECK(IsNodeRemoved(tree, 5));
    CHECK_FALSE(IsNodeRemoved(tree, 4));
}

TEST_CASE("wem compact nodes remaps every referencer", "[wem][nodes]") {
    NodeTree tree = makeRig();
    std::vector<Mesh> meshes;
    meshes.push_back(makeSkinnedMesh("leg", 1, 3, 3));
    meshes[0].sections[0].rigidNode = 4; // the light, deliberately not a bone

    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(meshes.data(), meshes.size());

    // Remove the thigh (nothing binds to it) and compact.
    auto removed = RemoveNode(tree, 2, RemovePolicy::ReparentChildren, SkinPolicy::Refuse, true,
                              referencers);
    REQUIRE(removed.removed);

    Diagnostics diagnostics;
    const auto remaps = CompactNodes(tree, referencers, diagnostics);
    INFO(diagnostics.formatHistogram());
    CHECK_FALSE(diagnostics.hasErrors());

    CHECK(remaps.newCount == 5);
    CHECK(tree.size() == 5);
    CHECK(remaps.nodes[2] == kInvalidNode);
    CHECK(remaps.nodes[3] == 2);
    CHECK(remaps.nodes[4] == 3);
    CHECK(remaps.nodes[5] == 4);

    // Parents-precede-children survives compaction, and the shin points at the
    // hip it was reparented to.
    CHECK(tree.parentsPrecedeChildren());
    CHECK(tree.nodes[2].name == "shin");
    CHECK(tree.nodes[2].parent == 1);
    CHECK(tree.nodes[4].parent == 2);

    // Both §10.6 rows followed the remap.
    for (const geom::Influence& influence : meshes[0].skin.influences) {
        CHECK(influence.bone != 3); // the old shin index
    }
    CHECK(meshes[0].skin.forVertex(1)[0].bone == 2);
    REQUIRE(meshes[0].sections[0].rigidNode.has_value());
    CHECK(*meshes[0].sections[0].rigidNode == 3);
}

TEST_CASE("wem node referencer check catches a bad bind", "[wem][nodes]") {
    NodeTree tree = makeRig();
    std::vector<Mesh> meshes;
    meshes.push_back(makeSkinnedMesh("leg", 1, 2, 3));

    Diagnostics clean;
    CheckNodeReferencers(tree, std::span<const Mesh>(meshes.data(), meshes.size()), clean);
    CHECK_FALSE(clean.hasErrors());

    SECTION("an influence outside the tree") {
        meshes[0].skin.influences[0].bone = 99;
        Diagnostics diagnostics;
        CheckNodeReferencers(tree, std::span<const Mesh>(meshes.data(), meshes.size()),
                             diagnostics);
        CHECK(diagnostics.countOf(DiagCode::DanglingNodeReference) == 1);
    }

    SECTION("an influence on a node that is not a bone") {
        // §5.6: indices are global node indices and must land on Bone nodes.
        meshes[0].skin.influences[0].bone = 4; // the light
        Diagnostics diagnostics;
        CheckNodeReferencers(tree, std::span<const Mesh>(meshes.data(), meshes.size()),
                             diagnostics);
        CHECK(diagnostics.countOf(DiagCode::DanglingNodeReference) == 1);
    }

    SECTION("a rigid section outside the tree") {
        meshes[0].sections[0].rigidNode = 42;
        Diagnostics diagnostics;
        CheckNodeReferencers(tree, std::span<const Mesh>(meshes.data(), meshes.size()),
                             diagnostics);
        CHECK(diagnostics.countOf(DiagCode::DanglingNodeReference) == 1);
    }
}

TEST_CASE("wem node tree can sort itself parents-first", "[wem][nodes]") {
    // Import order is not always hierarchy order; the invariant has to be
    // establishable, not merely assumed.
    NodeTree tree;
    tree.add(makeNode("child", NodeKind::Bone, 2, Vector3f{0, 0, 1}));
    tree.add(makeNode("grandchild", NodeKind::Bone, 0, Vector3f{0, 0, 1}));
    tree.add(makeNode("root", NodeKind::Bone, kInvalidNode, Vector3f{0, 0, 0}));
    CHECK_FALSE(tree.parentsPrecedeChildren());

    const auto remap = tree.sortParentsFirst();
    REQUIRE(remap.size() == 3);
    CHECK(tree.parentsPrecedeChildren());
    CHECK(tree.nodes[0].name == "root");
    CHECK(tree.nodes[1].name == "child");
    CHECK(tree.nodes[2].name == "grandchild");
    CHECK(remap[2] == 0);
    CHECK(remap[0] == 1);
    CHECK(remap[1] == 2);
    CHECK(nearlyEqual(tree.worldBind(2).translation, Vector3f{0, 0, 2}));
}

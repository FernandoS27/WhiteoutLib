// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Generated weights (EDIT_MODE_SKIN_DESIGN.md §8, gate S7).
///
/// Rigid per Island is the mechanical one-click: each island goes wholly to the
/// bone whose segment most of its points are nearest to. The question a gate can
/// actually ask of it is not "is it right" but "how often does it name the bone
/// an artist named", so the corpus arm measures exactly that over the shipped HD
/// models' RIGID islands — the ones that already go wholly to one bone, and so
/// have an answer to be compared with.
///
/// Heat and the voxel geodesic (§8.4-8.5) are held to what each promises on a
/// fixture -- heat flows along a limb, the geodesic reaches through the volume,
/// a lost island goes rigid, a stopped run writes nothing -- and S7's second
/// arm measures all three organic methods against the shipped weights, which
/// is how §8.5 says the default is chosen (Q11).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/skinning/generate.h>
#include <whiteout/models/wem/skinning/points.h>
#include <whiteout/utils/simple_thread_pool.h>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// A bone at @p position under @p parent. The tree is a pivot rig, so `local`
/// carries the offset from the parent's pivot and `pivot` the model-space joint.
u32 AddBone(NodeTree& tree, const char* name, u32 parent, const Vector3f& position) {
    Node bone;
    bone.name = name;
    bone.kind = NodeKind::Bone;
    bone.parent = parent;
    bone.pivot = position;
    bone.local.translation = position;
    if (parent != kInvalidNode) {
        const Vector3f& up = tree.nodes[parent].pivot;
        bone.local.translation = Vector3f{position.x - up.x, position.y - up.y, position.z - up.z};
    }
    return tree.add(std::move(bone));
}

geom::MeshBuilder StartMesh(const char* name) {
    geom::MeshBuilder builder;
    MeshSection section;
    section.name = name;
    builder.addSection(std::move(section));
    return builder;
}

/// A closed box of eight vertices centred on @p centre: one island, with enough
/// area on every face that the repair keeps them all.
void AddBox(geom::MeshBuilder& builder, const Vector3f& centre, f32 half) {
    // Corner c is (x from bit 0, y from bit 1, z from bit 2).
    geom::VertexId corners[8]{geom::VertexId(0), geom::VertexId(0), geom::VertexId(0),
                              geom::VertexId(0), geom::VertexId(0), geom::VertexId(0),
                              geom::VertexId(0), geom::VertexId(0)};
    for (u32 corner = 0; corner < 8; ++corner) {
        corners[corner] = builder.addVertex(Vector3f{centre.x + ((corner & 1) ? half : -half),
                                                     centre.y + ((corner & 2) ? half : -half),
                                                     centre.z + ((corner & 4) ? half : -half)});
    }
    static const u32 kFaces[12][3] = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6},
                                      {0, 1, 5}, {0, 5, 4}, {2, 6, 7}, {2, 7, 3},
                                      {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    for (const auto& face : kFaces) {
        builder.addTriangle(corners[face[0]], corners[face[1]], corners[face[2]]);
    }
}

/// The one bone @p vertex binds wholly, or `kInvalidNode` when it binds none or
/// splits between several. The shipped answer an island is measured against.
u32 SoleBoneOf(const Mesh& mesh, u32 vertex) {
    const std::span<const geom::Influence> influences = mesh.skin.forVertex(vertex);
    u32 sole = kInvalidNode;
    f32 total = 0.0f;
    for (const geom::Influence& influence : influences) {
        if (influence.weight <= 1e-3f) {
            continue;
        }
        total += influence.weight;
        if (sole != kInvalidNode) {
            return kInvalidNode;
        }
        sole = influence.bone;
    }
    return total > 0.5f ? sole : kInvalidNode;
}

} // namespace

// ============================================================================
// The segments (§8.2)
// ============================================================================

TEST_CASE("a bone's segment runs to each child bone, and a leaf is a point",
          "[wem][skin][generate]") {
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    const u32 armL = AddBone(tree, "armL", root, {-10, 0, 0});
    const u32 armR = AddBone(tree, "armR", root, {10, 0, 0});

    Node helper;
    helper.name = "muzzle";
    helper.kind = NodeKind::Helper;
    helper.parent = armR;
    helper.pivot = Vector3f{100, 0, 0};
    helper.local.translation = Vector3f{90, 0, 0};
    tree.add(std::move(helper));

    const skinning::BoneSegments segments = skinning::BuildBoneSegments(tree);
    REQUIRE(segments.bones.size() == 3u);

    // The root has two child bones, so two segments; each arm is a leaf, so one
    // degenerate segment each. The helper contributes nothing: a segment out to
    // a muzzle would pull weight off the body toward an attachment point.
    CHECK(segments.segments.size() == 4u);
    u32 rootSegments = 0;
    for (const auto& segment : segments.segments) {
        if (segment.bone == root) {
            ++rootSegments;
        }
        if (segment.bone == armR) {
            CHECK(segment.start.x == 10.0f);
            CHECK(segment.end.x == 10.0f);
        }
    }
    CHECK(rootSegments == 2u);

    // A point out by the left arm is nearest the left arm, not the root.
    CHECK(skinning::NearestBone(segments, Vector3f{-9, 0, 0}) == armL);
    CHECK(skinning::NearestBone(segments, Vector3f{9, 0, 0}) == armR);
    // And one ON the root's segment is the root's: it lies at distance 0 from
    // the two segments that start there, and the tie goes to the bone nearer
    // the root, which is the root itself.
    CHECK(skinning::NearestBone(segments, Vector3f{0, 0, 0}) == root);

    // A locked bone is not a candidate: a generator may not write one (§6.2).
    tree.nodes[armL].skin.locked = true;
    const skinning::BoneSegments unlocked = skinning::BuildBoneSegments(tree);
    CHECK(unlocked.bones.size() == 2u);
    CHECK(skinning::NearestBone(unlocked, Vector3f{-9, 0, 0}) == root);
}

TEST_CASE("the distance to a segment is the distance to its nearest point",
          "[wem][skin][generate]") {
    // Beside it, past either end, and on it.
    CHECK(skinning::DistanceToSegment({0, 5, 0}, {-10, 0, 0}, {10, 0, 0}) == 5.0f);
    CHECK(skinning::DistanceToSegment({20, 0, 0}, {-10, 0, 0}, {10, 0, 0}) == 10.0f);
    CHECK(skinning::DistanceToSegment({-20, 0, 0}, {-10, 0, 0}, {10, 0, 0}) == 10.0f);
    CHECK(skinning::DistanceToSegment({3, 0, 0}, {-10, 0, 0}, {10, 0, 0}) == 0.0f);
    // A degenerate segment is a point, with no division by zero in the way.
    CHECK(skinning::DistanceToSegment({0, 0, 4}, {0, 0, 0}, {0, 0, 0}) == 4.0f);
}

// ============================================================================
// The generator (§8.3)
// ============================================================================

TEST_CASE("Rigid per Island sends each island wholly to one bone",
          "[wem][skin][generate]") {
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    const u32 armL = AddBone(tree, "armL", root, {-20, 0, 0});
    const u32 armR = AddBone(tree, "armR", root, {20, 0, 0});

    geom::MeshBuilder builder = StartMesh("arms");
    AddBox(builder, {-20, 0, 0}, 2.0f); // around the left arm
    AddBox(builder, {20, 0, 0}, 2.0f);  // around the right arm
    Mesh mesh = builder.build().mesh;

    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    REQUIRE(points.islandCount == 2u);

    const skinning::GenerateResult result =
        skinning::RigidPerIsland(mesh, tree, points, {}, {});
    CHECK(result.islands == 2u);
    CHECK(result.unreached == 0u);
    CHECK(result.weights.changed == points.pointCount);

    for (u32 vertex = 0; vertex < 8; ++vertex) {
        CHECK(SoleBoneOf(mesh, vertex) == armL);
    }
    for (u32 vertex = 8; vertex < 16; ++vertex) {
        CHECK(SoleBoneOf(mesh, vertex) == armR);
    }

    SECTION("a corner nearer the other bone still follows its island") {
        // A box straddling the middle: most of it votes for the root, and the
        // whole island goes there even though four corners are nearer an arm.
        geom::MeshBuilder wide = StartMesh("straddle");
        AddBox(wide, {0, 0, 0}, 3.0f);
        Mesh straddle = wide.build().mesh;
        const skinning::PointTable table = skinning::BuildPointTable(straddle);
        skinning::RigidPerIsland(straddle, tree, table, {}, {});
        for (u32 vertex = 0; vertex < straddle.vertexCount(); ++vertex) {
            CHECK(SoleBoneOf(straddle, vertex) == root);
        }
    }

    SECTION("Split islands decides each point on its own") {
        geom::MeshBuilder wide = StartMesh("straddle");
        AddBox(wide, {0, 0, 0}, 25.0f); // corners past both arms
        Mesh straddle = wide.build().mesh;
        const skinning::PointTable table = skinning::BuildPointTable(straddle);
        skinning::GenerateOptions options;
        options.splitIslands = true;
        skinning::RigidPerIsland(straddle, tree, table, {}, options);

        std::map<u32, u32> perBone;
        for (u32 vertex = 0; vertex < straddle.vertexCount(); ++vertex) {
            ++perBone[SoleBoneOf(straddle, vertex)];
        }
        CHECK(perBone.size() > 1u);
        CHECK(perBone[armL] == 4u);
        CHECK(perBone[armR] == 4u);
    }
}

TEST_CASE("a bone set keeps the weight on the bones outside it",
          "[wem][skin][generate]") {
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    const u32 armL = AddBone(tree, "armL", root, {-20, 0, 0});
    const u32 armR = AddBone(tree, "armR", root, {20, 0, 0});

    geom::MeshBuilder builder = StartMesh("arm");
    AddBox(builder, {-20, 0, 0}, 2.0f);
    Mesh mesh = builder.build().mesh;
    mesh.skin.reset(mesh.vertexCount());
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        // Half on the chest, which the run is not allowed to touch.
        const std::vector<geom::Influence> held{{root, 0.5f}, {armR, 0.5f}};
        mesh.skin.assignVertex(vertex, held);
    }

    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    skinning::GenerateOptions options;
    options.bones = {armL, armR};
    skinning::RigidPerIsland(mesh, tree, points, {}, options);

    // The root is outside the set, so its 0.5 stayed; the rest went to the arm
    // the island is around, and the point still sums to one.
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        f32 onRoot = 0.0f;
        f32 onArm = 0.0f;
        f32 total = 0.0f;
        for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
            total += influence.weight;
            if (influence.bone == root) {
                onRoot = influence.weight;
            }
            if (influence.bone == armL) {
                onArm = influence.weight;
            }
        }
        CHECK(std::abs(onRoot - 0.5f) < 1e-4f);
        CHECK(std::abs(onArm - 0.5f) < 1e-4f);
        CHECK(std::abs(total - 1.0f) < 1e-4f);
    }
}

TEST_CASE("a locked point and a model with no bone are both left alone",
          "[wem][skin][generate]") {
    NodeTree tree;
    geom::MeshBuilder builder = StartMesh("box");
    AddBox(builder, {0, 0, 0}, 2.0f);
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);

    // No bone at all: every point is unreached and nothing is written.
    const skinning::GenerateResult none =
        skinning::RigidPerIsland(mesh, tree, points, {}, {});
    CHECK(none.unreached == points.pointCount);
    CHECK(none.weights.changed == 0u);
    CHECK(mesh.skin.empty());

    AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    std::vector<u32> locked(points.pointCount);
    for (u32 point = 0; point < points.pointCount; ++point) {
        locked[point] = point;
    }
    skinning::SetPointsLocked(mesh, points, locked, true);

    const skinning::GenerateResult held =
        skinning::RigidPerIsland(mesh, tree, points, {}, {});
    CHECK(held.weights.changed == 0u);
    CHECK(held.weights.locked == points.pointCount);
}

// ============================================================================
// S7, the corpus arm: the share Rigid per Island agrees with the artist on
// ============================================================================
TEST_CASE("S7: Rigid per Island against the shipped HD models' rigid islands",
          "[wem][skin][generate][corpus][.skincorpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 300);
    const MdxConverter converter;

    u32 models = 0;
    u32 islands = 0;      // every island of a skinned HD mesh
    u32 rigidIslands = 0; // the ones the file sends wholly to one bone
    u32 agreed = 0;       // and that the generator names the same bone for
    u32 adjacent = 0;     // or names its parent or one of its children
    u32 byJoint = 0;      // what the dumber rule below would agree on
    u32 toRoot = 0;       // how many the generator sent to the first bone

    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Model source;
        try {
            mdx::Parser parser;
            source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        } catch (...) {
            continue;
        }
        const Result<Document> document = converter.fromMdx(source);
        if (!document.ok() || document->models.empty()) {
            continue;
        }
        // HD only: the plan's arm is the Reforged models, whose four-lane skin
        // is what the workspace's tools write.
        if (std::find(document->profiles.begin(), document->profiles.end(),
                      ProfileId::Wc3Reforged) == document->profiles.end()) {
            continue;
        }
        ++models;

        const Model& model = document->models.front();
        const skinning::BoneSegments segments = skinning::BuildBoneSegments(model.nodes);
        // Each bone's joint, for the comparison rule: the start of any of its
        // segments, which is its own joint whether it is a chain or a leaf.
        std::map<u32, Vector3f> jointOf;
        for (const auto& segment : segments.segments) {
            jointOf.emplace(segment.bone, segment.start);
        }
        const u32 firstBone = segments.bones.empty() ? kInvalidNode : segments.bones.front();

        for (const Mesh& shipped : model.meshes) {
            if (shipped.skin.empty()) {
                continue;
            }
            const skinning::PointTable points = skinning::BuildPointTable(shipped);
            if (points.pointCount == 0) {
                continue;
            }
            const std::span<const Vector3f> positions =
                shipped.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);

            // What the FILE says, per island: the sole bone every one of its
            // points binds, or none.
            std::vector<u32> shippedBone(points.islandCount, kInvalidNode);
            std::vector<u8> rigid(points.islandCount, 1);
            std::vector<u8> seen(points.islandCount, 0);
            // And what a nearest-JOINT vote would say, the deliberately dumber
            // rule the segment model has to beat.
            std::vector<std::map<u32, u32>> jointVotes(points.islandCount);

            for (u32 point = 0; point < points.pointCount; ++point) {
                const u32 island = points.islandOf[point];
                if (island >= points.islandCount) {
                    continue;
                }
                seen[island] = 1;
                const std::span<const u32> members = points.membersOf(point);
                const u32 bone = members.empty() ? kInvalidNode : SoleBoneOf(shipped, members[0]);
                if (bone == kInvalidNode) {
                    rigid[island] = 0;
                } else if (shippedBone[island] == kInvalidNode) {
                    shippedBone[island] = bone;
                } else if (shippedBone[island] != bone) {
                    rigid[island] = 0;
                }

                if (members.empty() || members[0] >= positions.size()) {
                    continue;
                }
                const Vector3f& at = positions[members[0]];
                u32 nearest = kInvalidNode;
                f32 best = std::numeric_limits<f32>::max();
                for (const auto& [candidate, joint] : jointOf) {
                    const Vector3f gap{at.x - joint.x, at.y - joint.y, at.z - joint.z};
                    const f32 distance = gap.x * gap.x + gap.y * gap.y + gap.z * gap.z;
                    if (distance < best) {
                        best = distance;
                        nearest = candidate;
                    }
                }
                if (nearest != kInvalidNode) {
                    ++jointVotes[island][nearest];
                }
            }

            // What the generator says. It runs on a COPY, so the file's own
            // weights stay available for the comparison.
            Mesh generated = shipped;
            skinning::RigidPerIsland(generated, model.nodes, points, {}, {});

            for (u32 island = 0; island < points.islandCount; ++island) {
                if (seen[island] == 0) {
                    continue;
                }
                ++islands;
                if (rigid[island] == 0 || shippedBone[island] == kInvalidNode) {
                    continue;
                }
                ++rigidIslands;

                // One point of the island is enough: the generator writes the
                // island whole, which the fixture above already holds it to.
                u32 first = kInvalidNode;
                for (u32 point = 0; point < points.pointCount; ++point) {
                    if (points.islandOf[point] == island && !points.membersOf(point).empty()) {
                        first = points.membersOf(point).front();
                        break;
                    }
                }
                const u32 made = first == kInvalidNode ? kInvalidNode : SoleBoneOf(generated, first);
                if (made == firstBone) {
                    ++toRoot;
                }
                if (made == shippedBone[island]) {
                    ++agreed;
                    ++adjacent;
                } else if (made != kInvalidNode) {
                    // How far off a miss is: the difference between a heuristic
                    // that picks a neighbouring joint and one that is wrong
                    // about where the model is.
                    if (model.nodes.nodes[made].parent == shippedBone[island] ||
                        model.nodes.nodes[shippedBone[island]].parent == made) {
                        ++adjacent;
                    }
                }

                u32 jointWinner = kInvalidNode;
                u32 jointCount = 0;
                for (const auto& [bone, count] : jointVotes[island]) {
                    if (count > jointCount) {
                        jointCount = count;
                        jointWinner = bone;
                    }
                }
                if (jointWinner == shippedBone[island]) {
                    ++byJoint;
                }
            }
        }
    }

    if (rigidIslands == 0) {
        SKIP("no rigid islands in the HD models read");
    }
    const f64 rigidShare = 100.0 * static_cast<f64>(rigidIslands) / static_cast<f64>(islands);
    const f64 agreement = 100.0 * static_cast<f64>(agreed) / static_cast<f64>(rigidIslands);
    const f64 near = 100.0 * static_cast<f64>(adjacent) / static_cast<f64>(rigidIslands);
    const f64 joints = 100.0 * static_cast<f64>(byJoint) / static_cast<f64>(rigidIslands);
    std::cerr << "S7: " << models << " HD models, " << islands << " islands, " << rigidIslands
              << " rigid (" << rigidShare << "%); Rigid per Island names the shipped bone on "
              << agreed << " (" << agreement << "%), that bone or a neighbouring joint on "
              << adjacent << " (" << near << "%); nearest-joint alone would name it on "
              << byJoint << " (" << joints << "%); sent to the first bone " << toRoot
              << std::endl;

    // The gate is the MEASUREMENT -- the share is what EDIT_MODE_SKIN_DESIGN.md
    // §8.3 records, and a nearest-segment vote has no right answer to be held
    // to. What the two claims below catch is the model going wrong rather than
    // being imprecise:
    //
    //  - the segment model must beat the joints alone. It did not while a leaf
    //    bone could never win a tie against the parent whose segment ends on
    //    its joint, which is how that defect was found;
    //  - and it must not collapse: a rule that named almost nothing, or sent
    //    everything to the root, would still "measure".
    CHECK(agreed >= byJoint);
    CHECK(agreement > 20.0);
    CHECK(toRoot < rigidIslands / 4);
}

// ============================================================================
// Envelopes (§8.2)
// ============================================================================

namespace {

/// root -> mid -> tip, ten units apart along x.
NodeTree makeLimbRig() {
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    const u32 mid = AddBone(tree, "mid", root, {10, 0, 0});
    AddBone(tree, "tip", mid, {20, 0, 0});
    return tree;
}

/// Ten small boxes along that rig, each its own island -- so Rigid per Island
/// has to give every one of them a single bone, and an envelope does not.
Mesh makeLimbMesh() {
    geom::MeshBuilder builder = StartMesh("limb");
    for (u32 i = 0; i < 10; ++i) {
        AddBox(builder, {1.0f + 2.0f * static_cast<f32>(i), 0, 0}, 0.9f);
    }
    return builder.build().mesh;
}

u32 influenceCount(const Mesh& mesh, u32 vertex) {
    return static_cast<u32>(mesh.skin.forVertex(vertex).size());
}

} // namespace

TEST_CASE("an envelope is one inside and nothing outside", "[wem][skin][generate]") {
    Envelope envelope;
    envelope.innerStart = 1.0f;
    envelope.innerEnd = 1.0f;
    envelope.outerStart = 3.0f;
    envelope.outerEnd = 3.0f;
    envelope.falloff = EnvelopeFalloff::Linear;

    CHECK(skinning::EnvelopeWeight(envelope, {0.5f, 0.0f}) == 1.0f);
    CHECK(skinning::EnvelopeWeight(envelope, {1.0f, 0.0f}) == 1.0f);
    CHECK(skinning::EnvelopeWeight(envelope, {3.5f, 0.0f}) == 0.0f);
    CHECK(std::abs(skinning::EnvelopeWeight(envelope, {2.0f, 0.0f}) - 0.5f) < 1e-5f);

    SECTION("the radii are interpolated ALONG the segment") {
        envelope.innerEnd = 0.0f;
        envelope.outerEnd = 0.4f;
        // The same distance is inside at the start and past the end at the end.
        CHECK(skinning::EnvelopeWeight(envelope, {0.9f, 0.0f}) == 1.0f);
        CHECK(skinning::EnvelopeWeight(envelope, {0.9f, 1.0f}) == 0.0f);
    }

    SECTION("Hard is one right up to the outer radius") {
        envelope.falloff = EnvelopeFalloff::Hard;
        CHECK(skinning::EnvelopeWeight(envelope, {2.9f, 0.0f}) == 1.0f);
        CHECK(skinning::EnvelopeWeight(envelope, {3.1f, 0.0f}) == 0.0f);
    }

    SECTION("a Gaussian falls but does not reach zero before the outer radius") {
        envelope.falloff = EnvelopeFalloff::Gaussian;
        const f32 half = skinning::EnvelopeWeight(envelope, {2.0f, 0.0f});
        CHECK(half > 0.0f);
        CHECK(half < 1.0f);
        CHECK(skinning::EnvelopeWeight(envelope, {2.9f, 0.0f}) < half);
        CHECK(skinning::EnvelopeWeight(envelope, {3.0f, 0.0f}) == 0.0f);
    }
}

TEST_CASE("envelopes blend where Rigid per Island cannot", "[wem][skin][generate]") {
    const NodeTree tree = makeLimbRig();
    Mesh rigid = makeLimbMesh();
    const skinning::PointTable points = skinning::BuildPointTable(rigid);
    REQUIRE(points.islandCount == 10u);

    skinning::RigidPerIsland(rigid, tree, points, {}, {});
    for (u32 vertex = 0; vertex < rigid.vertexCount(); ++vertex) {
        CHECK(influenceCount(rigid, vertex) == 1u);
    }

    Mesh soft = makeLimbMesh();
    const skinning::GenerateResult result = skinning::EnvelopeWeights(soft, tree, points, {}, {});
    CHECK(result.weights.changed > 0u);
    u32 shared = 0;
    for (u32 vertex = 0; vertex < soft.vertexCount(); ++vertex) {
        shared += influenceCount(soft, vertex) > 1u ? 1u : 0u;
    }
    CHECK(shared > 0u);
}

TEST_CASE("the mechanical preset gives a point wholly to its nearest bone",
          "[wem][skin][generate]") {
    NodeTree tree = makeLimbRig();
    Mesh mesh = makeLimbMesh();
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    const skinning::BoneSegments segments = skinning::BuildBoneSegments(tree, {});
    const std::vector<Envelope> measured = skinning::MeasureEnvelopes(
        mesh, points, segments, skinning::EnvelopePreset::Mechanical);
    const skinning::MeshPoints one[1]{{&mesh, &points}};
    CHECK(skinning::MeasureEnvelope(one, segments, segments.bones.front(),
                                    skinning::EnvelopePreset::Mechanical) == measured.front());
    REQUIRE(measured.size() == segments.bones.size());
    for (std::size_t i = 0; i < segments.bones.size(); ++i) {
        CHECK(measured[i].falloff == EnvelopeFalloff::Hard);
        CHECK(measured[i].innerStart == measured[i].outerStart);
        tree.nodes[segments.bones[i]].skin.envelope = measured[i];
    }

    skinning::EnvelopeWeights(mesh, tree, points, {}, {});
    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        REQUIRE(influenceCount(mesh, vertex) == 1u);
        CHECK(mesh.skin.forVertex(vertex)[0].bone ==
              skinning::NearestBone(segments, positions[vertex]));
    }
}

TEST_CASE("a saved envelope is what the generator reads", "[wem][skin][generate]") {
    NodeTree tree = makeLimbRig();
    Mesh mesh = makeLimbMesh();
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    // An envelope of no radius reaches nothing, so every point falls through to
    // §8.1's rule -- rigid to the nearest bone, and counted.
    for (Node& node : tree.nodes) {
        node.skin.envelope = Envelope{};
    }
    const skinning::GenerateResult result = skinning::EnvelopeWeights(mesh, tree, points, {}, {});
    CHECK(result.unreached == points.pointCount);
    CHECK(result.weights.changed == points.pointCount);
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        CHECK(influenceCount(mesh, vertex) == 1u);
    }
}

// ============================================================================
// Heat and the voxel geodesic (§8.4-8.5)
// ============================================================================

namespace {

/// A closed cylinder along +x from 0 to @p length, @p sides round, one ring of
/// vertices per unit and a fan cap at each end: one island that heat can flow
/// along.
Mesh makeCylinder(u32 length, f32 radius, u32 sides) {
    geom::MeshBuilder builder = StartMesh("cylinder");
    std::vector<geom::VertexId> ring;
    for (u32 r = 0; r <= length; ++r) {
        for (u32 s = 0; s < sides; ++s) {
            const f32 angle = 2.0f * std::numbers::pi_v<f32> * static_cast<f32>(s) /
                              static_cast<f32>(sides);
            ring.push_back(builder.addVertex(
                Vector3f{static_cast<f32>(r), radius * std::cos(angle), radius * std::sin(angle)}));
        }
    }
    const geom::VertexId start = builder.addVertex(Vector3f{0, 0, 0});
    const geom::VertexId end = builder.addVertex(Vector3f{static_cast<f32>(length), 0, 0});
    const auto at = [&](u32 r, u32 s) { return ring[r * sides + (s % sides)]; };
    for (u32 r = 0; r < length; ++r) {
        for (u32 s = 0; s < sides; ++s) {
            builder.addTriangle(at(r, s), at(r, s + 1), at(r + 1, s + 1));
            builder.addTriangle(at(r, s), at(r + 1, s + 1), at(r + 1, s));
        }
    }
    for (u32 s = 0; s < sides; ++s) {
        builder.addTriangle(start, at(0, s + 1), at(0, s));
        builder.addTriangle(end, at(length, s), at(length, s + 1));
    }
    return builder.build().mesh;
}

/// @p bone's weight on @p vertex, 0 when it names none.
f32 WeightOn(const Mesh& mesh, u32 vertex, u32 bone) {
    for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
        if (influence.bone == bone) {
            return influence.weight;
        }
    }
    return 0.0f;
}

f32 TotalOf(const Mesh& mesh, u32 vertex) {
    f32 total = 0.0f;
    for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
        total += influence.weight;
    }
    return total;
}

bool SameSkin(const Mesh& a, const Mesh& b) {
    if (a.skin.offsets != b.skin.offsets || a.skin.influences.size() != b.skin.influences.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.skin.influences.size(); ++i) {
        if (a.skin.influences[i].bone != b.skin.influences[i].bone ||
            a.skin.influences[i].weight != b.skin.influences[i].weight) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("Heat on a two-bone cylinder is monotone along its axis",
          "[wem][skin][generate][heat]") {
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    const u32 tip = AddBone(tree, "tip", root, {10, 0, 0});
    Mesh mesh = makeCylinder(20, 2.0f, 12);
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    REQUIRE(points.islandCount == 1u);

    const skinning::GenerateResult result = skinning::HeatWeights(mesh, tree, points, {}, {});
    CHECK_FALSE(result.cancelled);
    CHECK(result.unreached == 0u);
    CHECK(result.weights.changed == points.pointCount);

    // The tip's weight, averaged round each ring, may only grow toward the tip
    // end: heat flows along the limb and never doubles back.
    const std::span<const Vector3f> positions =
        mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    std::map<i32, std::pair<f32, u32>> byRing;
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        CHECK(std::abs(TotalOf(mesh, vertex) - 1.0f) < 1e-4f);
        auto& [sum, count] = byRing[static_cast<i32>(std::lround(positions[vertex].x))];
        sum += WeightOn(mesh, vertex, tip);
        ++count;
    }
    f32 last = -1.0f;
    for (const auto& [x, entry] : byRing) {
        const f32 mean = entry.first / static_cast<f32>(entry.second);
        INFO("ring at x = " << x << " holds " << mean << " on the tip");
        CHECK(mean >= last - 1e-4f);
        last = mean;
    }
    const f32 first = byRing.begin()->second.first / static_cast<f32>(byRing.begin()->second.second);
    CHECK(first < 0.5f);
    CHECK(last > 0.5f);
    // And it is a blend, not a cut: somewhere along the limb both bones hold.
    u32 blended = 0;
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        blended += (WeightOn(mesh, vertex, root) > 0.1f && WeightOn(mesh, vertex, tip) > 0.1f);
    }
    CHECK(blended > 0u);
}

TEST_CASE("Heat sends an island that sees no bone wholly to one bone",
          "[wem][skin][generate][heat]") {
    // Two bones above a closed box that holds a second box. Every ray from the
    // inner box to a bone crosses the outer one, so the inner island sees no
    // bone, has H = 0 throughout, and is singular (§8.4).
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 20, 0});
    AddBone(tree, "tip", root, {20, 20, 0});
    geom::MeshBuilder builder = StartMesh("nested");
    AddBox(builder, {10, 0, 0}, 3.0f);
    AddBox(builder, {10, 0, 0}, 1.0f);
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    REQUIRE(points.islandCount == 2u);

    const skinning::GenerateResult result = skinning::HeatWeights(mesh, tree, points, {}, {});
    CHECK_FALSE(result.cancelled);
    // The inner box's eight points and only those: the outer box sees the bones
    // from its top, and is solved.
    CHECK(result.unreached == 8u);
    const u32 bone = SoleBoneOf(mesh, 8);
    CHECK(bone != kInvalidNode);
    for (u32 vertex = 8; vertex < 16; ++vertex) {
        CHECK(SoleBoneOf(mesh, vertex) == bone);
        CHECK(std::abs(TotalOf(mesh, vertex) - 1.0f) < 1e-4f);
    }
}

TEST_CASE("Geodesic reaches a part that touches the body but holds no bone",
          "[wem][skin][generate][geodesic]") {
    // Two closed boxes, two islands. The bones are inside the first, and the
    // second overlaps it without sharing a vertex. Over the surface the second
    // is infinitely far from every bone; through the volume it is next door.
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {-1, 0, 0});
    AddBone(tree, "tip", root, {1, 0, 0});

    geom::MeshBuilder builder = StartMesh("parts");
    AddBox(builder, {0, 0, 0}, 2.0f);
    AddBox(builder, {3.5f, 0, 0}, 2.0f);
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    REQUIRE(points.islandCount == 2u);

    const skinning::GenerateResult result = skinning::GeodesicWeights(mesh, tree, points, {}, {});
    CHECK_FALSE(result.cancelled);
    CHECK(result.unreached == 0u);
    CHECK(result.islands == 2u);
    for (u32 vertex = 0; vertex < mesh.vertexCount(); ++vertex) {
        CHECK(std::abs(TotalOf(mesh, vertex) - 1.0f) < 1e-4f);
    }

    SECTION("but not one across a gap") {
        // The same part moved off, past a gap no walk may cross. Its points
        // are unreached, and go rigid to the nearest bone instead (§8.1).
        geom::MeshBuilder apart = StartMesh("apart");
        AddBox(apart, {0, 0, 0}, 2.0f);
        AddBox(apart, {6.5f, 0, 0}, 2.0f);
        Mesh far = apart.build().mesh;
        const skinning::PointTable table = skinning::BuildPointTable(far);
        const skinning::GenerateResult gap = skinning::GeodesicWeights(far, tree, table, {}, {});
        CHECK(gap.unreached == 8u);
        for (u32 vertex = 8; vertex < 16; ++vertex) {
            CHECK(SoleBoneOf(far, vertex) != kInvalidNode);
        }
    }
}

TEST_CASE("a stopped Heat or Geodesic run writes nothing", "[wem][skin][generate]") {
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    AddBone(tree, "tip", root, {10, 0, 0});
    Mesh mesh = makeCylinder(20, 2.0f, 12);
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    // Something to keep: every point rigid to the root.
    std::vector<u32> all(points.pointCount);
    for (u32 point = 0; point < points.pointCount; ++point) {
        all[point] = point;
    }
    skinning::Rigid(mesh, tree, points, skinning::SkinScope{all, {}, {}}, root);
    const Mesh before = mesh;

    SECTION("stopped before it starts") {
        skinning::GenerateControl control;
        control.cancelled = [] { return true; };
        CHECK(skinning::HeatWeights(mesh, tree, points, {}, {}, control).cancelled);
        CHECK(SameSkin(mesh, before));
        CHECK(skinning::GeodesicWeights(mesh, tree, points, {}, {}, control).cancelled);
        CHECK(SameSkin(mesh, before));
    }
    SECTION("stopped part of the way through") {
        // The fourth question is answered "stop": past the set-up and into the
        // solves, where some bones are done and others are not.
        std::atomic<u32> asked{0};
        skinning::GenerateControl control;
        control.cancelled = [&asked] { return asked.fetch_add(1) + 1 > 3; };
        CHECK(skinning::HeatWeights(mesh, tree, points, {}, {}, control).cancelled);
        CHECK(SameSkin(mesh, before));
        asked = 0;
        CHECK(skinning::GeodesicWeights(mesh, tree, points, {}, {}, control).cancelled);
        CHECK(SameSkin(mesh, before));
    }
}

TEST_CASE("Heat and Geodesic answer the same on a pool as without one",
          "[wem][skin][generate]") {
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    const u32 mid = AddBone(tree, "mid", root, {7, 0, 0});
    AddBone(tree, "tip", mid, {14, 0, 0});
    const Mesh shape = makeCylinder(20, 2.0f, 12);
    const skinning::PointTable points = skinning::BuildPointTable(shape);

    utils::SimpleThreadPool pool(3);
    skinning::GenerateControl pooled;
    pooled.pool = &pool;
    u32 lastDone = 0;
    u32 lastTotal = 0;
    pooled.progress = [&](u32 done, u32 total) {
        lastDone = done;
        lastTotal = total;
    };

    Mesh alone = shape;
    Mesh shared = shape;
    skinning::HeatWeights(alone, tree, points, {}, {});
    skinning::HeatWeights(shared, tree, points, {}, {}, pooled);
    CHECK(SameSkin(alone, shared));
    CHECK(lastTotal > 0u);
    CHECK(lastDone == lastTotal);

    Mesh aloneG = shape;
    Mesh sharedG = shape;
    skinning::GeodesicWeights(aloneG, tree, points, {}, {});
    skinning::GeodesicWeights(sharedG, tree, points, {}, {}, pooled);
    CHECK(SameSkin(aloneG, sharedG));
    CHECK(lastDone == lastTotal);
}

// ============================================================================
// S7, the organic arm: Envelopes, Heat and Geodesic against the shipped HD
// weights, which is how §8.5 says the default is chosen (Q11)
// ============================================================================

namespace {

/// A vertex's weights, one entry per bone, normalised.
std::vector<geom::Influence> NormalisedOf(const Mesh& mesh, u32 vertex) {
    std::vector<geom::Influence> out;
    f32 total = 0.0f;
    for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
        if (!(influence.weight > 0.0f) || !std::isfinite(influence.weight)) {
            continue;
        }
        total += influence.weight;
        bool merged = false;
        for (geom::Influence& held : out) {
            if (held.bone == influence.bone) {
                held.weight += influence.weight;
                merged = true;
                break;
            }
        }
        if (!merged) {
            out.push_back(influence);
        }
    }
    for (geom::Influence& influence : out) {
        influence.weight /= total;
    }
    return out;
}

/// The heaviest bone, ties to the lower index.
u32 DominantOf(const std::vector<geom::Influence>& weights) {
    u32 best = kInvalidNode;
    f32 heaviest = -1.0f;
    for (const geom::Influence& influence : weights) {
        if (influence.weight > heaviest ||
            (influence.weight == heaviest && influence.bone < best)) {
            heaviest = influence.weight;
            best = influence.bone;
        }
    }
    return best;
}

/// The sum over bones of the weight difference: 0 for the same weights, 2 for
/// disjoint ones.
f32 DistanceL1(const std::vector<geom::Influence>& a, const std::vector<geom::Influence>& b) {
    f32 total = 0.0f;
    for (const geom::Influence& left : a) {
        f32 right = 0.0f;
        for (const geom::Influence& other : b) {
            if (other.bone == left.bone) {
                right = other.weight;
            }
        }
        total += std::abs(left.weight - right);
    }
    for (const geom::Influence& right : b) {
        bool seen = false;
        for (const geom::Influence& left : a) {
            seen = seen || left.bone == right.bone;
        }
        if (!seen) {
            total += right.weight;
        }
    }
    return total;
}

/// One method's score over one sample.
struct Score {
    u32 models = 0;
    u64 points = 0;
    u64 agreed = 0;
    u64 near = 0; ///< The same bone, or its parent or a child: off by one joint.
    f64 l1 = 0.0;
    f64 seconds = 0.0;

    /// Every point the file skins, read at its first member (§3.2).
    void add(const Mesh& shipped, const Mesh& made, const skinning::PointTable& table,
             const NodeTree& nodes) {
        for (u32 point = 0; point < table.pointCount; ++point) {
            const std::span<const u32> members = table.membersOf(point);
            if (members.empty()) {
                continue;
            }
            const std::vector<geom::Influence> file = NormalisedOf(shipped, members[0]);
            if (file.empty()) {
                continue; // the file says nothing, so there is nothing to agree with
            }
            const std::vector<geom::Influence> ours = NormalisedOf(made, members[0]);
            ++points;
            const u32 theirs = DominantOf(file);
            const u32 mine = DominantOf(ours);
            agreed += theirs == mine ? 1u : 0u;
            // How far off a miss is: a neighbouring joint is a disagreement
            // about where a bone's influence starts, anything else one about
            // where the model is.
            const bool adjacent = theirs < nodes.size() && mine < nodes.size() &&
                                  (nodes.nodes[theirs].parent == mine ||
                                   nodes.nodes[mine].parent == theirs);
            near += (theirs == mine || adjacent) ? 1u : 0u;
            l1 += DistanceL1(file, ours);
        }
    }
    f64 agreement() const {
        return points == 0 ? 0.0 : 100.0 * static_cast<f64>(agreed) / static_cast<f64>(points);
    }
    f64 nearShare() const {
        return points == 0 ? 0.0 : 100.0 * static_cast<f64>(near) / static_cast<f64>(points);
    }
    f64 meanL1() const {
        return points == 0 ? 0.0 : l1 / static_cast<f64>(points);
    }
};

/// The Reforged vehicles: what goes with the buildings as "mechanical".
bool IsVehicle(const std::string& path) {
    static const char* const kNames[] = {"ship",       "boat",          "wagon",
                                         "catapult",   "ballista",      "gyrocopter",
                                         "demolisher", "zeppelin",      "cannon",
                                         "infernalmachine", "warcart", "tinkerfactory",
                                         "tinkerrobot"};
    for (const char* name : kNames) {
        if (path.find(name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

enum Method : u32 { kEnvelopes, kHeat, kGeodesic, kRigid, kMethods };
constexpr const char* kMethodNames[kMethods] = {"Envelopes (Organic)", "Heat", "Geodesic",
                                                "Rigid per Island"};

} // namespace

TEST_CASE("S7: the organic generators against the shipped HD weights",
          "[wem][skin][generate][corpus][.skincorpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }

    // The two pools, sorted as `gather` sorts them.
    std::vector<std::filesystem::path> pools[2]; // characters, then buildings and vehicles
    for (const std::filesystem::path& file : files) {
        std::string text = test::pathText(file);
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (text.find("_hd.w3mod/") == std::string::npos ||
            text.find("portrait") != std::string::npos || test::isKnownBad(file)) {
            continue;
        }
        if (text.find("/buildings/") != std::string::npos || IsVehicle(text)) {
            pools[1].push_back(file);
        } else if (text.find("/units/") != std::string::npos) {
            pools[0].push_back(file);
        }
    }
    const char* const kGroups[2] = {"characters", "buildings and vehicles"};
    const std::size_t perGroup = test::sweepLimit(50, 50);

    const u32 threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    utils::SimpleThreadPool pool(threads);
    skinning::GenerateControl control;
    control.pool = &pool;

    const MdxConverter converter;
    Score scores[2][kMethods];
    for (u32 group = 0; group < 2; ++group) {
        const std::vector<std::filesystem::path>& from = pools[group];
        std::size_t cursor = 0;
        // Even steps through the sorted pool: a fixed sample. A pick that is
        // not a skinned Reforged model gives way to the next file after it.
        for (std::size_t k = 0; k < perGroup && cursor < from.size(); ++k) {
            for (std::size_t i = std::max(cursor, k * from.size() / perGroup); i < from.size();
                 ++i) {
                cursor = i + 1;
                test::trace(from[i]);
                const auto bytes = test::readCorpusFile(from[i]);
                if (bytes.empty()) {
                    continue;
                }
                mdx::Model source;
                try {
                    mdx::Parser parser;
                    source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
                } catch (...) {
                    continue;
                }
                const Result<Document> document = converter.fromMdx(source);
                if (!document.ok() || document->models.empty() ||
                    std::find(document->profiles.begin(), document->profiles.end(),
                              ProfileId::Wc3Reforged) == document->profiles.end()) {
                    continue;
                }
                const Model& model = document->models.front();
                std::vector<u32> meshes;
                // The import dropped every level of detail but LOD 0.
                for (u32 m = 0; m < model.meshes.size(); ++m) {
                    if (!model.meshes[m].skin.empty()) {
                        meshes.push_back(m);
                    }
                }
                if (meshes.empty() || skinning::BuildBoneSegments(model.nodes).bones.size() < 2) {
                    continue;
                }
                std::vector<skinning::PointTable> tables;
                for (const u32 m : meshes) {
                    tables.push_back(skinning::BuildPointTable(model.meshes[m]));
                }

                for (u32 method = 0; method < kMethods; ++method) {
                    // Each method starts from the geometry with its weights
                    // stripped, as a model skinned from scratch would.
                    std::vector<Mesh> made;
                    for (const u32 m : meshes) {
                        made.push_back(model.meshes[m]);
                        made.back().skin = geom::SkinBinding{};
                    }
                    const auto started = std::chrono::steady_clock::now();
                    if (method == kEnvelopes) {
                        std::vector<skinning::MeshPoints> all;
                        for (std::size_t i = 0; i < made.size(); ++i) {
                            all.push_back({&made[i], &tables[i]});
                        }
                        skinning::GenerateOptions options;
                        options.defaults = skinning::MeasureEnvelopes(
                            all, skinning::BuildBoneSegments(model.nodes),
                            skinning::EnvelopePreset::Organic);
                        for (std::size_t i = 0; i < made.size(); ++i) {
                            skinning::EnvelopeWeights(made[i], model.nodes, tables[i], {}, options);
                        }
                    } else if (method == kRigid) {
                        for (std::size_t i = 0; i < made.size(); ++i) {
                            skinning::RigidPerIsland(made[i], model.nodes, tables[i], {}, {});
                        }
                    } else {
                        std::vector<skinning::MeshScope> scopes;
                        for (std::size_t i = 0; i < made.size(); ++i) {
                            scopes.push_back({&made[i], &tables[i], {}});
                        }
                        const skinning::GeneratedRun run =
                            method == kHeat
                                ? skinning::SolveHeat(scopes, model.nodes, {}, control)
                                : skinning::SolveGeodesic(scopes, model.nodes, {}, control);
                        REQUIRE(run.meshes.size() == made.size());
                        for (std::size_t i = 0; i < made.size(); ++i) {
                            skinning::WriteGenerated(made[i], model.nodes, tables[i], run.meshes[i]);
                        }
                    }
                    Score& score = scores[group][method];
                    score.seconds += std::chrono::duration<f64>(std::chrono::steady_clock::now() -
                                                                started)
                                         .count();
                    ++score.models;
                    for (std::size_t i = 0; i < made.size(); ++i) {
                        score.add(model.meshes[meshes[i]], made[i], tables[i], model.nodes);
                    }
                }
                break; // this pick is taken
            }
        }
    }

    for (u32 group = 0; group < 2; ++group) {
        std::cerr << "S7 " << kGroups[group] << ": " << scores[group][0].models << " models, "
                  << scores[group][0].points << " points (" << threads << " pool threads)"
                  << std::endl;
        for (u32 method = 0; method < kMethods; ++method) {
            const Score& score = scores[group][method];
            std::cerr << "  " << kMethodNames[method] << ": dominant bone agrees on "
                      << score.agreement() << "% (that bone or a neighbouring joint on "
                      << score.nearShare() << "%), mean L1 " << score.meanL1() << ", "
                      << score.seconds << " s" << std::endl;
        }
    }

    // The gate is the MEASUREMENT (D§17): these numbers are what §8.5 records
    // and Q11 is answered from. What is asserted is only that no method
    // collapsed, since one that named almost nothing would still "measure".
    for (u32 group = 0; group < 2; ++group) {
        if (scores[group][0].points == 0) {
            continue;
        }
        for (u32 method = 0; method < kMethods; ++method) {
            INFO(kGroups[group] << " / " << kMethodNames[method]);
            CHECK(scores[group][method].points == scores[group][0].points);
            CHECK(scores[group][method].agreement() > 10.0);
        }
    }
}

TEST_CASE("S7 bench: Heat and Geodesic on the HD footman", "[wem][skin][generate][.skinbench]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    const std::string wanted = "_hd.w3mod/units/human/footman/footman.mdx";
    const std::filesystem::path* footman = nullptr;
    for (const std::filesystem::path& file : files) {
        const std::string text = test::pathText(file);
        if (text.size() >= wanted.size() &&
            text.compare(text.size() - wanted.size(), wanted.size(), wanted) == 0) {
            footman = &file;
        }
    }
    if (footman == nullptr) {
        SKIP("HD footman not found");
    }
    const auto bytes = test::readCorpusFile(*footman);
    mdx::Parser parser;
    const mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    const MdxConverter converter;
    const Result<Document> document = converter.fromMdx(source);
    REQUIRE(document.ok());
    const Model& model = document->models.front();

    // The import dropped every level of detail but LOD 0.
    std::vector<u32> meshes;
    for (u32 m = 0; m < model.meshes.size(); ++m) {
        meshes.push_back(m);
    }
    std::vector<skinning::PointTable> tables;
    u32 vertices = 0;
    u32 pointCount = 0;
    for (const u32 m : meshes) {
        tables.push_back(skinning::BuildPointTable(model.meshes[m]));
        vertices += model.meshes[m].vertexCount();
        pointCount += tables.back().pointCount;
    }
    std::vector<skinning::MeshScope> scopes;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        scopes.push_back({&model.meshes[meshes[i]], &tables[i], {}});
    }
    const u32 threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    utils::SimpleThreadPool pool(threads);
    skinning::GenerateControl pooled;
    pooled.pool = &pool;

    const auto time = [&](const char* name, auto&& solve) {
        const auto started = std::chrono::steady_clock::now();
        const skinning::GeneratedRun run = solve();
        const f64 seconds =
            std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
        u32 unreached = 0;
        for (const skinning::GeneratedWeights& one : run.meshes) {
            unreached += one.unreached;
        }
        std::cerr << "  " << name << ": " << seconds << " s, " << unreached << " unreached"
                  << std::endl;
        CHECK_FALSE(run.cancelled);
    };
    std::cerr << "S7 bench, HD footman LOD 0: " << scopes.size() << " meshes, " << vertices
              << " vertices, " << pointCount << " points, "
              << skinning::BuildBoneSegments(model.nodes).bones.size() << " bones, " << threads
              << " pool threads" << std::endl;
    time("Heat, one core", [&] { return skinning::SolveHeat(scopes, model.nodes); });
    time("Heat, pool", [&] { return skinning::SolveHeat(scopes, model.nodes, {}, pooled); });
    time("Geodesic, one core", [&] { return skinning::SolveGeodesic(scopes, model.nodes); });
    time("Geodesic, pool",
         [&] { return skinning::SolveGeodesic(scopes, model.nodes, {}, pooled); });
}

// ============================================================================
// Found by the review of 2026-09-22
// ============================================================================

TEST_CASE("a bone's segment runs through a helper to the bone below it",
          "[wem][skin][generate]") {
    // A joint that skins nothing is exported as a helper, and the limb still
    // runs across it: 13 of the corpus's 60 text MDLs have one. Stopping at the
    // helper made the abdomen a leaf and measured the chest's sphere to the
    // helper, so the middle of the limb belonged to neither.
    NodeTree tree;
    const u32 abdomen = AddBone(tree, "abdomen", kInvalidNode, {0, 0, 0});
    Node helper;
    helper.name = "spine";
    helper.kind = NodeKind::Helper;
    helper.parent = abdomen;
    helper.pivot = Vector3f{0, 0, 10};
    helper.local.translation = Vector3f{0, 0, 10};
    const u32 spine = tree.add(std::move(helper));
    const u32 chest = AddBone(tree, "chest", spine, {0, 0, 20});

    const skinning::BoneSegments segments = skinning::BuildBoneSegments(tree);
    REQUIRE(segments.bones.size() == 2u);
    bool across = false;
    for (const auto& segment : segments.segments) {
        if (segment.bone == abdomen) {
            CHECK(segment.radius == 0.0f);
            across = across || std::abs(segment.end.z - 20.0f) < 1e-4f;
        }
        if (segment.bone == chest) {
            // Half the way to the BONE above, not to the helper.
            CHECK(std::abs(segment.radius - 10.0f) < 1e-4f);
        }
    }
    CHECK(across);
    CHECK(skinning::NearestBone(segments, Vector3f{0, 0, 8}) == abdomen);
}

TEST_CASE("Rigid per Island writes a point no face holds", "[wem][skin][generate]") {
    // A point no face holds is on no island, and was skipped without being
    // written or counted -- §8.1 leaves no point empty.
    NodeTree tree;
    AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    geom::MeshBuilder builder = StartMesh("body");
    AddBox(builder, {0, 0, 0}, 1.0f);
    builder.addVertex(Vector3f{5, 5, 5});
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);
    u32 loose = kInvalidIndex;
    for (u32 point = 0; point < points.pointCount; ++point) {
        if (point >= points.islandOf.size() || points.islandOf[point] == kInvalidIndex) {
            loose = point;
        }
    }
    REQUIRE(loose != kInvalidIndex);

    const skinning::GenerateResult result = skinning::RigidPerIsland(mesh, tree, points, {});
    CHECK(result.islands == points.islandCount + 1);
    CHECK(result.weights.changed == points.pointCount);
    for (const u32 vertex : points.membersOf(loose)) {
        CHECK(SoleBoneOf(mesh, vertex) == 0u);
    }
}

TEST_CASE("Geodesic under a big rig stays inside a small mesh's grid", "[wem][skin][generate]") {
    // The grid spans the run's meshes and a leaf's sphere comes from the whole
    // skeleton: a 0.2-unit card under a rig 200 units tall swept a cube 10^5
    // cells a side for its one leaf, with Cancel only between bones.
    NodeTree tree;
    const u32 root = AddBone(tree, "root", kInvalidNode, {0, 0, 0});
    AddBone(tree, "head", root, {0, 0, 200});
    geom::MeshBuilder builder = StartMesh("card");
    AddBox(builder, {0, 0, 100}, 0.1f);
    Mesh mesh = builder.build().mesh;
    const skinning::PointTable points = skinning::BuildPointTable(mesh);

    const auto start = std::chrono::steady_clock::now();
    const skinning::GenerateResult result = skinning::GeodesicWeights(mesh, tree, points, {});
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    CHECK(took.count() < 10.0);
    CHECK_FALSE(result.cancelled);
    CHECK(result.weights.changed == points.pointCount);
}

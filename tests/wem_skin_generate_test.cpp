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

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/skinning/generate.h>
#include <whiteout/models/wem/skinning/points.h>

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

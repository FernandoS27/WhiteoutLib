// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// WEM's physics set (WEM_PHYSICS_DESIGN.md §3): the `.wem` container, the
// referencers that follow a node or mesh removal, the cloth bind layer across a
// vertex renumbering, validation and rescale.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/physics/references.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include "wem_material_fixture.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using namespace wemfix;

namespace {

Node Bone(const char* name, u32 parent, f32 x) {
    Node node;
    node.name = name;
    node.kind = NodeKind::Bone;
    node.resetPayloadForKind();
    node.parent = parent;
    node.local.translation = Vector3f{x, 0, 0};
    return node;
}

/// Two bones, a body on each joined by a cone twist, a cloth whose cage is
/// section 0 and which drives section 1, a collider on the second bone.
Document PhysicsDocument() {
    Document document = makeDocument(ProfileId::Sc2);
    Model& model = document.models[0];
    model.nodes.add(Bone("root", kInvalidNode, 0));
    model.nodes.add(Bone("arm", 0, 1));
    Mesh& mesh = model.meshes[0];
    mesh.sections[0].flags |= SectionFlags::ClothSimulated;
    mesh.sections[1].flags |= SectionFlags::ClothInfluenced;
    // The bound section's first vertex is moved by the cage's three.
    std::array<u32, 4> none{geom::kInvalidId, geom::kInvalidId, geom::kInvalidId, geom::kInvalidId};
    const auto bind = mesh.attributes.getOrCreate<std::array<u32, 4>>(
        geom::names::kClothBindVertex, geom::Domain::Vertex, geom::AttrType::U32x4);
    for (auto& lanes : bind) {
        lanes = none;
    }
    bind[3] = {0, 1, 2, geom::kInvalidId};

    PhysicsSet& physics = model.physics;
    PhysicsBody root;
    root.id = physics.allocateId();
    root.node = 0;
    PhysicsShape box;
    box.kind = PhysicsShapeKind::Box;
    box.halfExtents = {1.0f, 2.0f, 3.0f};
    box.material.density = 500.0f;
    root.shapes.push_back(box);
    root.sc2 = Sc2BodyExtension{};
    physics.bodies.push_back(root);

    PhysicsBody arm = root;
    arm.id = physics.allocateId();
    arm.node = 1;
    PhysicsShape hull;
    hull.kind = PhysicsShapeKind::ConvexHull;
    hull.points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    arm.shapes = {hull};
    physics.bodies.push_back(arm);

    PhysicsJoint joint;
    joint.id = physics.allocateId();
    joint.bodyA = root.id;
    joint.bodyB = arm.id;
    joint.kind = JointKind::ConeTwist;
    joint.frameA.data[3][0] = 1.0f; // the arm's origin, in the root's frame
    physics.joints.push_back(joint);

    ClothCollider collider;
    collider.id = physics.allocateId();
    collider.node = 1;
    collider.radius = 0.25f;
    collider.length = 2.0f;
    physics.colliders.push_back(collider);

    Cloth cloth;
    cloth.id = physics.allocateId();
    cloth.cage = SectionRef{0, 0};
    cloth.bindings.push_back(ClothBinding{SectionRef{0, 1}});
    cloth.colliders = {collider.id};
    cloth.sc2 = Sc2ClothParams{};
    physics.cloths.push_back(cloth);

    AnimChannel dynamic;
    dynamic.id = 77;
    dynamic.target.kind = TrackTarget::Kind::Physics;
    dynamic.target.sub = arm.id;
    dynamic.target.channel = Channel::PhysicsDynamic;
    dynamic.valueType = geom::AttrType::F32;
    model.animChannels.add(dynamic);
    return document;
}

template <class T>
void Append(std::vector<u8>& bytes, const T& value) {
    const u8* at = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), at, at + sizeof(T));
}

/// A stage of @p kind over the arm, naming @p rig or @p cloth, whose weight
/// rises from 0 to 1 over a one-second clip's first half.
void AddStage(Document& document, StageKind kind, u32 rig, u32 cloth) {
    Model& model = document.models[0];
    PoseStage stage;
    stage.id = 1;
    stage.name = "fall";
    stage.kind = kind;
    stage.driven = {1};
    stage.rig = rig;
    stage.cloth = cloth;
    model.poseStages.push_back(stage);

    AnimChannel weight;
    weight.id = model.animChannels.nextFreeId();
    weight.target.kind = TrackTarget::Kind::Node;
    weight.target.node = 1;
    weight.target.channel = Channel::StageWeight;
    weight.target.sub = StageSub(stage.id);
    weight.valueType = geom::AttrType::F32;
    model.animChannels.add(weight);

    Clip clip;
    clip.name = "Death";
    clip.model = 0;
    clip.duration = 1.0f;
    SubTrack track;
    track.channel = weight.id;
    track.times = {0.0f, 0.5f};
    Append(track.values, 0.0f);
    Append(track.values, 1.0f);
    clip.containers.emplace_back().subTracks.push_back(track);
    document.clips.push_back(clip);
}

/// A rig of the arm's body.
u32 AddRig(Document& document) {
    PhysicsSet& physics = document.models[0].physics;
    PhysicsRig rig;
    rig.id = physics.allocateId();
    rig.name = "arm";
    rig.bodies = {physics.bodies[1].id};
    physics.rigs.push_back(rig);
    return rig.id;
}

bool PhysicsErrors(const Diagnostics& report) {
    for (const Diagnostic& entry : report.bySeverity(Severity::Error)) {
        if (entry.code == DiagCode::PhysicsReferenceInvalid || entry.code == DiagCode::ClothTopologyInvalid ||
            entry.code == DiagCode::PhysicsShapeDegenerate) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("wem physics survives the .wem container", "[wem][physics]") {
    const Document document = PhysicsDocument();
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    const std::optional<Document> read = parser.parse(std::span<const u8>(bytes));
    REQUIRE(read.has_value());
    const PhysicsSet& a = document.models[0].physics;
    const PhysicsSet& b = read->models[0].physics;
    REQUIRE(b.bodies.size() == 2);
    REQUIRE(b.joints.size() == 1);
    REQUIRE(b.colliders.size() == 1);
    REQUIRE(b.cloths.size() == 1);
    CHECK(b.nextId == a.nextId);
    CHECK(b.bodies[0].shapes[0].halfExtents.z == 3.0f);
    CHECK(b.bodies[0].shapes[0].material.density == 500.0f);
    CHECK(b.bodies[0].sc2.has_value());
    CHECK(b.bodies[0].sc2->untracedFlags == 0x20u);
    CHECK(b.bodies[1].shapes[0].points.size() == 4);
    CHECK(b.joints[0].kind == JointKind::ConeTwist);
    CHECK(b.joints[0].frameA.data[3][0] == 1.0f);
    CHECK(b.colliders[0].length == 2.0f);
    CHECK(b.cloths[0].bindings[0].section == (SectionRef{0, 1}));
    CHECK(b.cloths[0].colliders == a.cloths[0].colliders);
    CHECK(b.cloths[0].sc2.has_value());
    const auto bind = read->models[0].meshes[0].attributes.get<std::array<u32, 4>>(
        geom::names::kClothBindVertex, geom::Domain::Vertex);
    REQUIRE(bind.size() == 6);
    CHECK(bind[3][2] == 2u);
}

TEST_CASE("wem physics a clean physics document validates", "[wem][physics]") {
    const Document document = PhysicsDocument();
    CHECK_FALSE(PhysicsErrors(Validate(document, ValidateLevel::Profile)));
}

TEST_CASE("wem physics validation names what dangles", "[wem][physics]") {
    Document document = PhysicsDocument();
    PhysicsSet& physics = document.models[0].physics;
    physics.joints[0].bodyB = 999;
    document.models[0].meshes[0].sections[0].flags = SectionFlags::None;
    const Diagnostics report = Validate(document, ValidateLevel::Structural);
    bool joint = false, cage = false;
    for (const Diagnostic& entry : report.bySeverity(Severity::Error)) {
        joint = joint || entry.code == DiagCode::PhysicsReferenceInvalid;
        cage = cage || entry.code == DiagCode::ClothTopologyInvalid;
    }
    CHECK(joint);
    CHECK(cage);
}

TEST_CASE("wem physics goes with its node", "[wem][physics]") {
    Document document = PhysicsDocument();
    Model& model = document.models[0];
    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(model.meshes);
    referencers.channels = &model.animChannels;
    referencers.stages = &model.poseStages;
    referencers.physics = &model.physics;
    const RemoveResult removed =
        RemoveNode(model.nodes, 1, RemovePolicy::ReparentChildren, SkinPolicy::Refuse, true, referencers);
    REQUIRE(removed.removed);
    Diagnostics out;
    CompactNodes(model.nodes, referencers, out);
    // The arm's body, the joint on it and the collider riding it are gone; the
    // cloth keeps its cage and drops the collider.
    REQUIRE(model.physics.bodies.size() == 1);
    CHECK(model.physics.bodies[0].node == 0);
    CHECK(model.physics.joints.empty());
    CHECK(model.physics.colliders.empty());
    REQUIRE(model.physics.cloths.size() == 1);
    CHECK(model.physics.cloths[0].colliders.empty());
    // Its switch is invalidated, not dropped.
    REQUIRE(model.animChannels.find(77) != nullptr);
    CHECK(model.animChannels.find(77)->target.sub == 0u);
    CHECK_FALSE(PhysicsErrors(Validate(document, ValidateLevel::Structural)));
}

TEST_CASE("wem a cloth goes with its cage mesh", "[wem][physics]") {
    Document document = PhysicsDocument();
    Diagnostics out;
    const std::vector<u8> drop{1};
    RemoveMeshes(document, 0, drop, out);
    CHECK(document.models[0].physics.cloths.empty());
    CHECK(document.models[0].physics.bodies.size() == 2);
}

TEST_CASE("wem a cloth goes when its cage is merged into another section", "[wem][physics]") {
    // A merge takes one-section meshes only: a cage alone, and another.
    Document document = PhysicsDocument();
    Model& model = document.models[0];
    model.meshes = {makeMesh({"cage"}), makeMesh({"other"})};
    model.meshes[0].sections[0].flags |= SectionFlags::ClothSimulated;
    Cloth& cloth = model.physics.cloths[0];
    cloth.cage = SectionRef{0, 0};
    cloth.bindings.clear();
    AddStage(document, StageKind::Cloth, 0, cloth.id);
    const std::vector<u32> both{0, 1};
    const MeshMergeResult merged = MergeMeshesInto(model, both, 0);
    REQUIRE(merged.ok);
    CHECK(model.physics.cloths.empty());
    CHECK(model.poseStages[0].cloth == 0u);
    CHECK_FALSE(PhysicsErrors(Validate(document, ValidateLevel::Structural)));
}

TEST_CASE("wem a cloth bind layer follows its vertices", "[wem][physics]") {
    Document document = PhysicsDocument();
    Mesh& mesh = document.models[0].meshes[0];
    // Drop vertex 1 and move the rest down: lanes naming it become unused.
    const std::vector<u32> remap{0, geom::kInvalidId, 1, 2, 3, 4};
    mesh.attributes.remapDomain(geom::Domain::Vertex, remap, 5);
    const auto bind =
        mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
    REQUIRE(bind.size() == 5);
    CHECK(bind[2][0] == 0u);
    CHECK(bind[2][1] == geom::kInvalidId);
    CHECK(bind[2][2] == 1u);
}

TEST_CASE("wem physics rescales its lengths and keeps its densities", "[wem][physics]") {
    Document document = PhysicsDocument();
    const RescaleResult result = RescaleDocument(document, 2.0f);
    REQUIRE(result.ok);
    const PhysicsSet& physics = document.models[0].physics;
    CHECK(physics.bodies[0].shapes[0].halfExtents.z == 6.0f);
    CHECK(physics.bodies[0].shapes[0].material.density == 500.0f);
    CHECK(physics.bodies[1].shapes[0].points[1].x == 2.0f);
    CHECK(physics.joints[0].frameA.data[3][0] == 2.0f);
    CHECK(physics.colliders[0].radius == 0.5f);
}

TEST_CASE("wem a stage keeps the rig and cloth it names through the container", "[wem][physics]") {
    Document document = PhysicsDocument();
    const u32 rig = AddRig(document);
    AddStage(document, StageKind::Ragdoll, rig, 0);
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    const std::optional<Document> read = parser.parse(std::span<const u8>(bytes));
    REQUIRE(read.has_value());
    REQUIRE(read->models[0].poseStages.size() == 1);
    CHECK(read->models[0].poseStages[0].rig == rig);
    CHECK(read->models[0].poseStages[0].cloth == 0u);
    REQUIRE(read->models[0].physics.rigs.size() == 1);
    CHECK(read->models[0].physics.rigs[0].bodies == document.models[0].physics.rigs[0].bodies);
}

TEST_CASE("wem a rig stage becomes switches for a game that runs it", "[wem][physics]") {
    Document document = PhysicsDocument();
    AddStage(document, StageKind::Ragdoll, AddRig(document), 0);
    Diagnostics report;
    BakeStages(document, ProfileId::Sc2, nullptr, report);
    CHECK(report.countOf(DiagCode::AnimStageNotBaked) == 0u);
    const Model& model = document.models[0];
    CHECK(model.poseStages.empty());
    // The arm's own switch (channel 77) takes the weight: off, then on at the
    // first 60 Hz sample over a half.
    const SubTrack* dynamic = FindSubTrack(document.clips[0], 77);
    REQUIRE(dynamic != nullptr);
    CHECK(dynamic->interp == Interpolation::Step);
    REQUIRE(dynamic->times.size() == 2);
    CHECK(dynamic->times[0] == 0.0f);
    CHECK(dynamic->times[1] == Catch::Approx(16.0f / 60.0f).margin(0.002));
    f32 values[2] = {};
    REQUIRE(dynamic->values.size() == sizeof(values));
    std::memcpy(values, dynamic->values.data(), sizeof(values));
    CHECK(values[0] == 0.0f);
    CHECK(values[1] == 1.0f);
    // Nothing was baked into the arm's own keys.
    for (const AnimChannel& channel : model.animChannels.channels) {
        CHECK_FALSE((channel.target.kind == TrackTarget::Kind::Node && channel.target.node == 1));
    }
}

TEST_CASE("wem a rig stage still needs the host for a game without physics", "[wem][physics]") {
    Document document = PhysicsDocument();
    AddStage(document, StageKind::Ragdoll, AddRig(document), 0);
    Diagnostics report;
    BakeStages(document, ProfileId::Wc3Classic, nullptr, report);
    CHECK(report.countOf(DiagCode::AnimStageNotBaked) == 1u);
}

TEST_CASE("wem a cloth stage with no weight turns its cloth on", "[wem][physics]") {
    Document document = PhysicsDocument();
    Model& model = document.models[0];
    Cloth& cloth = model.physics.cloths[0];
    cloth.active = false;
    PoseStage stage;
    stage.id = 1;
    stage.name = "cape";
    stage.kind = StageKind::Cloth;
    stage.driven = {1};
    stage.cloth = cloth.id;
    model.poseStages.push_back(stage);
    Diagnostics report;
    BakeStages(document, ProfileId::Sc2, nullptr, report);
    CHECK(report.countOf(DiagCode::AnimStageNotBaked) == 0u);
    CHECK(model.poseStages.empty());
    CHECK(model.physics.cloths[0].active);
}

TEST_CASE("wem a stage lets go of a cloth that went with its mesh", "[wem][physics]") {
    Document document = PhysicsDocument();
    AddStage(document, StageKind::Cloth, 0, document.models[0].physics.cloths[0].id);
    Diagnostics out;
    const std::vector<u8> drop{1};
    RemoveMeshes(document, 0, drop, out);
    REQUIRE(document.models[0].poseStages.size() == 1);
    CHECK(document.models[0].poseStages[0].cloth == 0u);
}

TEST_CASE("wem validation names a stage whose rig is not there", "[wem][physics]") {
    Document document = PhysicsDocument();
    AddStage(document, StageKind::Ragdoll, 999, 0);
    CHECK(PhysicsErrors(Validate(document, ValidateLevel::Structural)));
}

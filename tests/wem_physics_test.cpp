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
#include <set>
#include <span>
#include <vector>

#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/physics/crossing.h>
#include <whiteout/models/wem/physics/references.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include "wem_material_fixture.h"
#include "whiteout/models/wem/chunk_tags.h"

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

TEST_CASE("wem the World of Warcraft extensions survive the .wem container", "[wem][physics]") {
    Document document = PhysicsDocument();
    PhysicsSet& physics = document.models[0].physics;
    physics.bodies[1].wow = WowBodyExtension{0.25f, false, false, physics.bodies[0].id};
    physics.bodies[0].shapes[0].gameFlags = 3;
    physics.joints[0].motor = JointMotorMode::Velocity;
    physics.joints[0].maxMotorForce = 4.0f;
    PhysicsRig rig;
    rig.id = physics.allocateId();
    rig.wow = WowRigExtension{};
    rig.wow->kind = WowPhysicsKind::PrivateWorld;
    rig.wow->vegetation = WowVegetation{};
    rig.wow->allowList = {0xABCDu};
    physics.rigs.push_back(rig);
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    const std::optional<Document> read = parser.parse(std::span<const u8>(bytes));
    REQUIRE(read.has_value());
    const PhysicsSet& back = read->models[0].physics;
    REQUIRE(back.bodies[1].wow.has_value());
    CHECK(back.bodies[1].wow->followFactor == 0.25f);
    CHECK(back.bodies[1].wow->parent == physics.bodies[0].id);
    CHECK(back.bodies[0].shapes[0].gameFlags == 3);
    CHECK(back.joints[0].motor == JointMotorMode::Velocity);
    CHECK(back.joints[0].maxMotorForce == 4.0f);
    REQUIRE(back.rigs.size() == 1);
    REQUIRE(back.rigs[0].wow.has_value());
    CHECK(back.rigs[0].wow->kind == WowPhysicsKind::PrivateWorld);
    CHECK(back.rigs[0].wow->vegetation.has_value());
    CHECK(back.rigs[0].wow->allowList == std::vector<u32>{0xABCDu});
}

TEST_CASE("wem a body that goes lets go of what hung off it", "[wem][physics]") {
    Document document = PhysicsDocument();
    PhysicsSet& physics = document.models[0].physics;
    physics.bodies[0].motion = BodyMotion::Kinematic;
    physics.bodies[1].motion = BodyMotion::Dynamic;
    physics.bodies[1].wow = WowBodyExtension{};
    physics.bodies[1].wow->parent = physics.bodies[0].id;
    const std::vector<u32> remap{kInvalidNode, 0};
    RemapPhysicsNodes(physics, remap);
    REQUIRE(physics.bodies.size() == 1);
    CHECK(physics.bodies[0].wow->parent == 0u);
}

TEST_CASE("wem validation names a body hanging off a missing body", "[wem][physics]") {
    Document document = PhysicsDocument();
    PhysicsBody& body = document.models[0].physics.bodies[1];
    body.wow = WowBodyExtension{};
    body.wow->parent = 999;
    CHECK(PhysicsErrors(Validate(document, ValidateLevel::Structural)));
}

TEST_CASE("wem a rig stage is left to World of Warcraft without switches", "[wem][physics]") {
    Document document = PhysicsDocument();
    AddStage(document, StageKind::Ragdoll, AddRig(document), 0);
    Diagnostics report;
    BakeStages(document, ProfileId::Wow, nullptr, report);
    CHECK(report.countOf(DiagCode::AnimStageNotBaked) == 0u);
    CHECK(document.models[0].poseStages.empty());
    // The game simulates from creation: no switch is keyed.
    CHECK(FindSubTrack(document.clips[0], 77) == nullptr);
}

TEST_CASE("wem physics rescales a slide and a vegetation push", "[wem][physics]") {
    Document document = PhysicsDocument();
    PhysicsSet& physics = document.models[0].physics;
    PhysicsJoint& joint = physics.joints[0];
    joint.kind = JointKind::Prismatic;
    joint.lower = -1.0f;
    joint.upper = 2.0f;
    joint.referenceTranslation = 0.5f;
    PhysicsRig rig;
    rig.id = physics.allocateId();
    rig.wow = WowRigExtension{};
    rig.wow->vegetation = WowVegetation{};
    physics.rigs.push_back(rig);
    REQUIRE(RescaleDocument(document, 2.0f).ok);
    CHECK(physics.joints[0].upper == 4.0f);
    CHECK(physics.joints[0].referenceTranslation == 1.0f);
    CHECK(physics.rigs[0].wow->vegetation->posMaxPush == 2.5f);
    CHECK(physics.rigs[0].wow->vegetation->minPushDist == 16.0f); // squared
}

TEST_CASE("wem a capsule stated by its ends keeps them to the bit", "[wem][physics]") {
    PhysicsShape stated;
    stated.kind = PhysicsShapeKind::Capsule;
    stated.points = {{0.1f, -0.2f, 0.3f}, {-5.5f, 1e-3f, 7.25f}};
    const auto [a, b] = CapsuleEnds(stated);
    CHECK(a == stated.points[0]);
    CHECK(b == stated.points[1]);
    // StarCraft II's form of the same capsule: a centred frame and a length.
    PhysicsShape centred;
    centred.kind = PhysicsShapeKind::Capsule;
    centred.transform = CapsuleFrame(a, b, centred.length);
    const auto [c, d] = CapsuleEnds(centred);
    CHECK(c.x == Catch::Approx(a.x).margin(1e-5));
    CHECK(d.z == Catch::Approx(b.z).margin(1e-5));
}

// ============================================================================
// Crossings (WEM_PHYSICS_DESIGN.md §8.3)
// ============================================================================

namespace {

/// Two rigs over the two bodies: the root's from creation, the arm's on death,
/// and a Death and a Stand clip.
Document RiggedDocument() {
    Document document = PhysicsDocument();
    PhysicsSet& physics = document.models[0].physics;
    PhysicsRig always;
    always.id = physics.allocateId();
    always.name = "anchored";
    always.start = RigStart::Always;
    always.bodies = {physics.bodies[0].id};
    PhysicsRig death;
    death.id = physics.allocateId();
    death.name = "collapse";
    death.start = RigStart::OnDeath;
    death.bodies = {physics.bodies[1].id};
    physics.rigs = {always, death};
    for (const char* name : {"Death 01", "Stand"}) {
        Clip clip;
        clip.name = name;
        clip.model = 0;
        clip.duration = 1.0f;
        clip.containers.emplace_back();
        document.clips.push_back(clip);
    }
    return document;
}

} // namespace

TEST_CASE("wem a Death clip is one by its name", "[wem][physics]") {
    CHECK(IsDeathClipName("Death"));
    CHECK(IsDeathClipName("death 02"));
    CHECK(IsDeathClipName("Death Fire"));
    CHECK_FALSE(IsDeathClipName("Deathwing"));
    CHECK_FALSE(IsDeathClipName("Stand"));
}

TEST_CASE("wem a death rig comes on in the Death clips for StarCraft II", "[wem][physics]") {
    Document document = RiggedDocument();
    Diagnostics report;
    FitPhysicsToProfile(document, ProfileId::Sc2, report);
    const PhysicsSet& physics = document.models[0].physics;
    // The model's death rig is the one SC2 switches. The other, whose body
    // never moves, goes as a rig; its body stays as the arm's joint partner
    // (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §8.6). Both rest off.
    REQUIRE(physics.bodies.size() == 2);
    CHECK(report.countOf(DiagCode::PhysicsRigDropped) == 1u);
    CHECK_FALSE(physics.bodies[0].simulates);
    CHECK_FALSE(physics.bodies[1].simulates);
    // The arm's own switch (channel 77) is keyed on in the Death clip only.
    const auto clip = [&](const char* name) -> const Clip& {
        return *std::find_if(document.clips.begin(), document.clips.end(),
                             [&](const Clip& c) { return c.name == name; });
    };
    const SubTrack* death = FindSubTrack(clip("Death 01"), 77);
    REQUIRE(death != nullptr);
    CHECK(death->interp == Interpolation::Step);
    REQUIRE(death->times.size() == 1);
    f32 on = 0.0f;
    std::memcpy(&on, death->values.data(), sizeof on);
    CHECK(on == 1.0f);
    CHECK(FindSubTrack(clip("Stand"), 77) == nullptr);
}

TEST_CASE("wem World of Warcraft keeps the rigs that start from creation", "[wem][physics]") {
    Document document = RiggedDocument();
    Diagnostics report;
    FitPhysicsToProfile(document, ProfileId::Wow, report);
    const PhysicsSet& physics = document.models[0].physics;
    REQUIRE(physics.bodies.size() == 1);
    CHECK(physics.bodies[0].node == 0); // the root's, from the Always rig
    CHECK(physics.joints.empty());
    CHECK(report.countOf(DiagCode::PhysicsRigDropped) == 1u);
}

TEST_CASE("wem a cage over the target's particles is decimated", "[wem][physics]") {
    // A 20 x 20 grid of particles, its first row pinned, and one bound vertex.
    Document document = makeDocument(ProfileId::Sc2);
    Model& model = document.models[0];
    model.nodes.add(Bone("root", kInvalidNode, 0));
    geom::MeshBuilder builder;
    MeshSection cage;
    cage.flags = SectionFlags::Hidden | SectionFlags::ClothSimulated;
    const u32 cageSection = builder.addSection(cage);
    MeshSection drawn;
    drawn.flags = SectionFlags::ClothInfluenced;
    const u32 drawnSection = builder.addSection(drawn);
    constexpr u32 kSide = 20;
    for (u32 y = 0; y < kSide; ++y) {
        for (u32 x = 0; x < kSide; ++x) {
            const geom::VertexId v = builder.addVertex(Vector3f{static_cast<f32>(x), 0, -static_cast<f32>(y)});
            builder.setVertexAttr(v, geom::names::kClothMovable, static_cast<u8>(y == 0 ? 0 : 1));
            builder.addInfluence(v, 0, 1.0f);
        }
    }
    for (u32 y = 0; y + 1 < kSide; ++y) {
        for (u32 x = 0; x + 1 < kSide; ++x) {
            const u32 a = y * kSide + x;
            builder.addTriangle(geom::VertexId(a), geom::VertexId(a + 1), geom::VertexId(a + kSide), cageSection);
            builder.addTriangle(geom::VertexId(a + 1), geom::VertexId(a + kSide + 1), geom::VertexId(a + kSide),
                                cageSection);
        }
    }
    // A drawn triangle, each corner bound to a particle near the bottom.
    const u32 first = builder.vertexCount();
    for (u32 k = 0; k < 3; ++k) {
        builder.addVertex(Vector3f{static_cast<f32>(k), 1, -19});
    }
    builder.addTriangle(geom::VertexId(first), geom::VertexId(first + 1), geom::VertexId(first + 2), drawnSection);
    const std::array<u32, 4> none{geom::kInvalidId, geom::kInvalidId, geom::kInvalidId, geom::kInvalidId};
    for (u32 v = 0; v < builder.vertexCount(); ++v) {
        std::array<u32, 4> lanes = none;
        std::array<f32, 4> weights{0, 0, 0, 0};
        if (v >= first) {
            lanes[0] = (kSide - 1) * kSide + (v - first);
            weights[0] = 1.0f;
        }
        builder.setVertexAttr(geom::VertexId(v), geom::names::kClothBindVertex, lanes);
        builder.setVertexAttr(geom::VertexId(v), geom::names::kClothBindWeight, weights);
    }
    model.meshes = {builder.build().mesh};
    Cloth cloth;
    cloth.id = model.physics.allocateId();
    cloth.cage = SectionRef{0, cageSection};
    cloth.bindings.push_back(ClothBinding{SectionRef{0, drawnSection}});
    model.physics.cloths.push_back(cloth);

    Diagnostics report;
    FitPhysicsToProfile(document, ProfileId::Sc2, report);
    const Mesh& mesh = document.models[0].meshes[0];
    std::set<u32> particles;
    const geom::FaceSet& faces = mesh.faceSet();
    const std::span<const u32> sections = mesh.faceSections();
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceValence.size(); ++f) {
        for (u32 k = 0; k < faces.faceValence[f]; ++k) {
            if (sections[f] == cageSection) {
                particles.insert(faces.cornerVertex[corner + k]);
            }
        }
        corner += faces.faceValence[f];
    }
    CHECK(particles.size() <= 256u);
    CHECK(particles.size() > 100u);
    // The pinned top row survives as pins, and the binding still names the cage.
    const auto movable = mesh.attributes.get<u8>(geom::names::kClothMovable, geom::Domain::Vertex);
    std::size_t pinned = 0;
    for (const u32 v : particles) {
        pinned += movable[v] == 0 ? 1 : 0;
    }
    CHECK(pinned >= 2u);
    CHECK_FALSE(PhysicsErrors(Validate(document, ValidateLevel::Structural)));
    CHECK(report.countOf(DiagCode::ClothParticleLimit) == 1u);
}

TEST_CASE("wem physics rescales forces and torques at the fourth power", "[wem][physics]") {
    Document document = PhysicsDocument();
    PhysicsJoint& joint = document.models[0].physics.joints[0];
    joint.breakForce = 1.0f;
    joint.friction = JointFriction::Torque;
    joint.frictionAmount = 2.0f;
    REQUIRE(RescaleDocument(document, 2.0f).ok);
    CHECK(document.models[0].physics.joints[0].breakForce == 16.0f);
    CHECK(document.models[0].physics.joints[0].frictionAmount == 32.0f);
}

TEST_CASE("wem physics rescales a force field's strength, sizes and frame", "[wem][physics]") {
    Document document = PhysicsDocument();
    Node node;
    node.name = "field";
    node.kind = NodeKind::ForceField;
    ForceFieldPayload field;
    field.strength = 3.0f;
    field.width = 4.0f;
    field.transform.data[3][0] = 5.0f;
    node.payload = field;
    const u32 index = document.models[0].nodes.size();
    document.models[0].nodes.add(std::move(node));
    REQUIRE(RescaleDocument(document, 2.0f).ok);
    const auto& scaled = std::get<ForceFieldPayload>(document.models[0].nodes.nodes[index].payload);
    // The strength is an acceleration: a length a second squared.
    CHECK(scaled.strength == 6.0f);
    CHECK(scaled.width == 8.0f);
    CHECK(scaled.transform.data[3][0] == 10.0f);
    CHECK(scaled.transform.data[0][0] == 1.0f);
}


namespace {

/// The document with its authoring state set: a locked body and joint, and a
/// rig built as a ragdoll with a recipe off its defaults.
Document Authored() {
    Document document = PhysicsDocument();
    PhysicsSet& physics = document.models[0].physics;
    physics.bodies[1].locked = true;
    physics.joints[0].locked = true;
    PhysicsRig rig;
    rig.id = physics.allocateId();
    rig.name = "Ragdoll";
    rig.start = RigStart::OnDeath;
    rig.bodies = {physics.bodies[0].id, physics.bodies[1].id};
    RagdollRecipe recipe;
    recipe.torso = PhysicsShapeKind::Box;
    recipe.hullPoints = 12;
    recipe.tightness = 0.6f;
    recipe.range = 2;
    recipe.weldProps = false;
    recipe.material = 13;
    rig.recipe = recipe;
    physics.rigs.push_back(rig);
    return document;
}

} // namespace

TEST_CASE("wem physics carries its locks and a ragdoll's recipe", "[wem][physics]") {
    const Document document = Authored();
    Writer writer;
    const std::vector<u8> bytes = writer.write(document);
    Parser parser;
    const std::optional<Document> read = parser.parse(std::span<const u8>(bytes));
    REQUIRE(read.has_value());
    const PhysicsSet& back = read->models[0].physics;
    CHECK_FALSE(back.bodies[0].locked);
    CHECK(back.bodies[1].locked);
    CHECK(back.joints[0].locked);
    REQUIRE(back.rigs.size() == 1u);
    REQUIRE(back.rigs[0].recipe.has_value());
    const RagdollRecipe& recipe = *back.rigs[0].recipe;
    CHECK(recipe.torso == PhysicsShapeKind::Box);
    CHECK(recipe.head == PhysicsShapeKind::Capsule);
    CHECK(recipe.hullPoints == 12);
    CHECK(recipe.tightness == 0.6f);
    CHECK(recipe.range == 2);
    CHECK_FALSE(recipe.weldProps);
    CHECK(recipe.material == 13u);
    CHECK(recipe.angularDamping == 0.3f);
}

TEST_CASE("wem a physics chunk from before the locks reads unlocked, with no recipe", "[wem][physics]") {
    // What a build without the fields wrote: the records stamped v2, whose
    // reader stops before the v3 fields. Each chunk holds one record, so the
    // v3 bytes it leaves unread are the chunk's last, and nothing after them
    // moves.
    Document document = Authored();
    PhysicsSet& physics = document.models[0].physics;
    physics.bodies.erase(physics.bodies.begin());
    physics.joints[0].bodyA = physics.joints[0].bodyB;
    physics.rigs[0].bodies = {physics.bodies[0].id};
    Writer writer;
    std::vector<u8> bytes = writer.write(document);
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    u32 stamped = 0;
    for (u32 i = 0; i < header.indexCount; ++i) {
        IndexEntry entry{};
        const std::size_t at = header.indexOffset + i * sizeof(IndexEntry);
        std::memcpy(&entry, bytes.data() + at, sizeof(entry));
        if (entry.tag != ChunkTagTraits<PhysicsBody>::value && entry.tag != ChunkTagTraits<PhysicsJoint>::value &&
            entry.tag != ChunkTagTraits<PhysicsRig>::value)
            continue;
        CHECK(entry.version == 3u);
        entry.version = 2;
        std::memcpy(bytes.data() + at, &entry, sizeof(entry));
        ++stamped;
    }
    REQUIRE(stamped == 3u);
    Parser parser;
    const std::optional<Document> read = parser.parse(std::span<const u8>(bytes));
    REQUIRE(read.has_value());
    const PhysicsSet& back = read->models[0].physics;
    REQUIRE(back.bodies.size() == 1u);
    CHECK_FALSE(back.bodies[0].locked);
    REQUIRE(back.joints.size() == 1u);
    CHECK_FALSE(back.joints[0].locked);
    CHECK(back.joints[0].kind == JointKind::ConeTwist);
    REQUIRE(back.rigs.size() == 1u);
    CHECK(back.rigs[0].name == "Ragdoll");
    CHECK_FALSE(back.rigs[0].recipe.has_value());
    CHECK(back.bodies[0].shapes[0].points.size() == 4u);
}

// ---- Every kind, carried or checked (EDIT_MODE_PHYSICS_REDESIGN.md §24.4) ------

TEST_CASE("wem a cylinder's prism is as wide as its circle in area", "[wem][physics]") {
    PhysicsShape cylinder;
    cylinder.kind = PhysicsShapeKind::Cylinder;
    cylinder.radius = 2.0f;
    cylinder.length = 3.0f;
    const std::vector<Vector3f> prism = CylinderPrism(cylinder);
    REQUIRE(prism.size() == 32u);
    // The cap's polygon, by the shoelace over its top ring.
    f64 area = 0.0;
    std::vector<Vector3f> top;
    for (const Vector3f& p : prism) {
        CHECK(std::abs(std::abs(p.z) - 1.5f) < 1e-6f);
        if (p.z > 0.0f)
            top.push_back(p);
    }
    REQUIRE(top.size() == 16u);
    for (std::size_t i = 0; i < top.size(); ++i) {
        const Vector3f& a = top[i];
        const Vector3f& b = top[(i + 1) % top.size()];
        area += 0.5 * (static_cast<f64>(a.x) * b.y - static_cast<f64>(b.x) * a.y);
    }
    CHECK(area == Catch::Approx(3.14159265 * 4.0).epsilon(1e-5));
}

TEST_CASE("wem World of Warcraft carries a cylinder as a hull, StarCraft II as itself", "[wem][physics]") {
    const auto withCylinder = [] {
        Document document = PhysicsDocument();
        PhysicsShape& shape = document.models[0].physics.bodies[0].shapes[0];
        shape.kind = PhysicsShapeKind::Cylinder;
        shape.radius = 1.0f;
        shape.length = 2.0f;
        shape.points.clear();
        return document;
    };
    Document wow = withCylinder();
    const Model before = wow.models[0];
    // Checked for the game, the cylinder is a note that it goes as a hull,
    // never a warning that it is lost: as many warnings as the box had.
    const auto unsupported = [](const Model& model, Severity severity) {
        Diagnostics report;
        CheckPhysicsForProfile(model, ProfileId::Wow, report);
        u32 count = 0;
        for (const Diagnostic& entry : report.bySeverity(severity))
            count += entry.code == DiagCode::PhysicsUnsupported ? 1u : 0u;
        return count;
    };
    const Model boxed = PhysicsDocument().models[0];
    CHECK(unsupported(before, Severity::Warning) == unsupported(boxed, Severity::Warning));
    CHECK(unsupported(before, Severity::Info) == unsupported(boxed, Severity::Info) + 1u);
    Diagnostics report;
    FitPhysicsToProfile(wow, ProfileId::Wow, report);
    const PhysicsShape& carried = wow.models[0].physics.bodies[0].shapes[0];
    CHECK(carried.kind == PhysicsShapeKind::ConvexHull);
    CHECK(carried.points == CylinderPrism(before.physics.bodies[0].shapes[0]));
    Document sc2 = withCylinder();
    FitPhysicsToProfile(sc2, ProfileId::Sc2, report);
    CHECK(sc2.models[0].physics.bodies[0].shapes[0].kind == PhysicsShapeKind::Cylinder);
}

TEST_CASE("wem validation names a shape with no size and a flat hull", "[wem][physics]") {
    const auto degenerate = [](const Document& document) {
        const Diagnostics report = Validate(document, ValidateLevel::Structural);
        u32 count = 0;
        for (const Diagnostic& entry : report.all())
            count += entry.code == DiagCode::PhysicsShapeDegenerate ? 1u : 0u;
        return count;
    };
    const Document clean = PhysicsDocument();
    const u32 base = degenerate(clean);
    Document none = clean;
    PhysicsShape sphere;
    sphere.kind = PhysicsShapeKind::Sphere;
    sphere.radius = 0.0f;
    none.models[0].physics.bodies[0].shapes.push_back(sphere);
    CHECK(degenerate(none) == base + 1u);
    Document box = clean;
    PhysicsShape flatBox;
    flatBox.kind = PhysicsShapeKind::Box;
    flatBox.halfExtents = {1.0f, 1.0f, 0.0f};
    box.models[0].physics.bodies[0].shapes.push_back(flatBox);
    CHECK(degenerate(box) == base + 1u);
    Document flat = clean;
    PhysicsShape hull;
    hull.kind = PhysicsShapeKind::ConvexHull;
    hull.points = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{1, 1, 0}, Vector3f{0, 1, 0}};
    flat.models[0].physics.bodies[0].shapes.push_back(hull);
    CHECK(degenerate(flat) == base + 1u);
    hull.points.push_back(Vector3f{0.5f, 0.5f, 1.0f});
    flat.models[0].physics.bodies[0].shapes.back() = hull;
    CHECK(degenerate(flat) == base);
}

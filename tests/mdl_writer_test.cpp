// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MDL writer test suite.
// Tests Model → MDL text serialization, and round-trip (write MDL → parse MDL → compare).

#include <catch2/catch_all.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/mdx/types.h>
#include <whiteout/models/mdx/writer.h>

#include "whiteout/models/mdx/mdl_converter.h"
#include "whiteout/models/mdx/mdl_writer.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

using namespace whiteout;
using namespace whiteout::mdx;

// ============================================================================
// Helpers
// ============================================================================

static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static constexpr float EPS = 1e-4f;

static bool approx(float a, float b) { return std::fabs(a - b) < EPS; }

// Parse an MDL text string into a Model.
static Model parseMdl(const std::string& text) {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(text, issues);
    REQUIRE(issues.empty());
    return model;
}

// ============================================================================
// Basic block serialization tests
// ============================================================================

TEST_CASE("mdl_write_version", "[mdl_writer]") {
    Model model;
    model.version = 1000;
    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("FormatVersion 1000,") != std::string::npos);

    auto rt = parseMdl(mdl);
    CHECK(rt.version == 1000);
}

TEST_CASE("mdl_write_model_block", "[mdl_writer]") {
    Model model;
    model.version = 800;
    model.modelName = "TestModel";
    model.blendTime = 150;
    model.modelExtent.minimum = Vector3f(-10, -20, -30);
    model.modelExtent.maximum = Vector3f(10, 20, 30);
    model.modelExtent.boundsRadius = 50.0f;

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Model \"TestModel\"") != std::string::npos);
    CHECK(mdl.find("BlendTime 150,") != std::string::npos);
    CHECK(mdl.find("BoundsRadius 50.0,") != std::string::npos);

    auto rt = parseMdl(mdl);
    CHECK(rt.modelName == "TestModel");
    CHECK(rt.blendTime == 150);
    CHECK(approx(rt.modelExtent.boundsRadius, 50.0f));
    CHECK(approx(rt.modelExtent.minimum.x, -10.0f));
    CHECK(approx(rt.modelExtent.maximum.z, 30.0f));
}

TEST_CASE("mdl_write_sequences", "[mdl_writer]") {
    Model model;
    model.version = 800;
    Sequence seq;
    seq.name = "Stand";
    seq.intervalStart = 0;
    seq.intervalEnd = 3333;
    seq.moveSpeed = 0.0f;
    seq.flags = 0x1; // NonLooping
    seq.rarity = 0.0f;
    seq.extent.boundsRadius = 100.0f;
    model.sequences.push_back(seq);

    Sequence seq2;
    seq2.name = "Walk";
    seq2.intervalStart = 3334;
    seq2.intervalEnd = 6666;
    seq2.moveSpeed = 270.0f;
    model.sequences.push_back(seq2);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Sequences 2") != std::string::npos);
    CHECK(mdl.find("Anim \"Stand\"") != std::string::npos);
    CHECK(mdl.find("NonLooping,") != std::string::npos);
    CHECK(mdl.find("Anim \"Walk\"") != std::string::npos);
    CHECK(mdl.find("MoveSpeed 270.0,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.sequences.size() == 2);
    CHECK(rt.sequences[0].name == "Stand");
    CHECK(rt.sequences[0].intervalEnd == 3333);
    CHECK(rt.sequences[0].flags & 0x1);
    CHECK(rt.sequences[1].moveSpeed == 270.0f);
}

TEST_CASE("mdl_write_global_sequences", "[mdl_writer]") {
    Model model;
    model.version = 800;
    model.globalSequences = {1000, 2000, 500};

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("GlobalSequences 3") != std::string::npos);
    CHECK(mdl.find("Duration 1000,") != std::string::npos);
    CHECK(mdl.find("Duration 2000,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.globalSequences.size() == 3);
    CHECK(rt.globalSequences[0] == 1000);
    CHECK(rt.globalSequences[1] == 2000);
    CHECK(rt.globalSequences[2] == 500);
}

TEST_CASE("mdl_write_textures", "[mdl_writer]") {
    Model model;
    model.version = 800;
    Texture tex;
    tex.fileName = "Textures\\Armor.blp";
    tex.replaceableId = 0;
    tex.flags = 0x3; // WrapWidth | WrapHeight
    model.textures.push_back(tex);

    Texture tex2;
    tex2.fileName = "";
    tex2.replaceableId = 1; // team color
    model.textures.push_back(tex2);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Textures 2") != std::string::npos);
    CHECK(mdl.find("Image \"Textures\\Armor.blp\",") != std::string::npos);
    CHECK(mdl.find("WrapWidth,") != std::string::npos);
    CHECK(mdl.find("WrapHeight,") != std::string::npos);
    CHECK(mdl.find("ReplaceableId 1,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.textures.size() == 2);
    CHECK(rt.textures[0].fileName == "Textures\\Armor.blp");
    CHECK(rt.textures[0].flags == 0x3);
    CHECK(rt.textures[1].replaceableId == 1);
}

TEST_CASE("mdl_write_materials", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Material mat;
    mat.priorityPlane = 5;
    Layer layer;
    layer.filterMode = Layer::FilterMode::Blend;
    layer.shadingFlags = Layer::ShadingFlag::TwoSided;
    layer.textureId = 0;
    layer.alpha = 0.8f;
    mat.layers.push_back(layer);
    model.materials.push_back(mat);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Materials 1") != std::string::npos);
    CHECK(mdl.find("PriorityPlane 5,") != std::string::npos);
    CHECK(mdl.find("FilterMode Blend,") != std::string::npos);
    CHECK(mdl.find("TwoSided,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.materials.size() == 1);
    REQUIRE(rt.materials[0].layers.size() == 1);
    CHECK(rt.materials[0].priorityPlane == 5);
    CHECK(rt.materials[0].layers[0].filterMode == Layer::FilterMode::Blend);
    CHECK(hasFlag(rt.materials[0].layers[0].shadingFlags, Layer::ShadingFlag::TwoSided));
    CHECK(approx(rt.materials[0].layers[0].alpha, 0.8f));
}

TEST_CASE("mdl_write_geoset_basic", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Geoset geo;
    geo.vertexPositions = {Vector3f(0, 0, 0), Vector3f(1, 0, 0), Vector3f(0, 1, 0)};
    geo.vertexNormals = {Vector3f(0, 0, 1), Vector3f(0, 0, 1), Vector3f(0, 0, 1)};
    geo.faces = {0, 1, 2};
    geo.faceTypeGroups = {4};
    geo.faceGroups = {3};
    geo.vertexGroups = {0, 0, 0};
    geo.matrixGroups = {1};
    geo.matrixIndices = {0};
    geo.materialId = 0;
    geo.selectionGroup = 0;
    geo.textureCoordinateSets.push_back({Vector2f(0, 0), Vector2f(1, 0), Vector2f(0, 1)});
    geo.extent.boundsRadius = 1.0f;
    model.geosets.push_back(geo);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Vertices 3") != std::string::npos);
    CHECK(mdl.find("Normals 3") != std::string::npos);
    CHECK(mdl.find("Faces 1 3") != std::string::npos);
    CHECK(mdl.find("TVertices 3") != std::string::npos);
    CHECK(mdl.find("MaterialID 0,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.geosets.size() == 1);
    REQUIRE(rt.geosets[0].vertexPositions.size() == 3);
    CHECK(approx(rt.geosets[0].vertexPositions[1].x, 1.0f));
    REQUIRE(rt.geosets[0].faces.size() == 3);
    CHECK(rt.geosets[0].faces[2] == 2);
}

TEST_CASE("mdl_write_geoset_anim", "[mdl_writer]") {
    Model model;
    model.version = 800;

    GeosetAnimation ga;
    ga.alpha = 0.5f;
    ga.geosetId = 0;
    ga.flags = 0x1; // DropShadow
    model.geosetAnimations.push_back(ga);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("GeosetAnim") != std::string::npos);
    CHECK(mdl.find("DropShadow,") != std::string::npos);
    CHECK(mdl.find("GeosetId 0,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.geosetAnimations.size() == 1);
    CHECK(approx(rt.geosetAnimations[0].alpha, 0.5f));
    CHECK(rt.geosetAnimations[0].flags & 0x1);
}

TEST_CASE("mdl_write_bone", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Bone bone;
    bone.node.name = "Bone_Root";
    bone.node.objectId = 0;
    bone.node.parentId = Node::NO_PARENT;
    bone.node.type = Node::NodeType::Bone;
    bone.node.flags = Node::NodeFlag::Bone;
    bone.geosetId = Bone::MULTIPLE_GEOSETS;
    bone.geosetAnimationId = 0xFFFFFFFF;
    model.bones.push_back(bone);

    Bone bone2;
    bone2.node.name = "Bone_Spine";
    bone2.node.objectId = 1;
    bone2.node.parentId = 0;
    bone2.node.type = Node::NodeType::Bone;
    bone2.node.flags = Node::NodeFlag::Bone;
    bone2.geosetId = 0;
    bone2.geosetAnimationId = 1;
    model.bones.push_back(bone2);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Bone \"Bone_Root\"") != std::string::npos);
    CHECK(mdl.find("GeosetId Multiple,") != std::string::npos);
    CHECK(mdl.find("GeosetAnimId None,") != std::string::npos);
    CHECK(mdl.find("Bone \"Bone_Spine\"") != std::string::npos);
    CHECK(mdl.find("Parent 0,") != std::string::npos);
    CHECK(mdl.find("GeosetId 0,") != std::string::npos);
    CHECK(mdl.find("GeosetAnimId 1,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.bones.size() == 2);
    CHECK(rt.bones[0].node.name == "Bone_Root");
    CHECK(rt.bones[0].geosetId == Bone::MULTIPLE_GEOSETS);
    CHECK(rt.bones[0].geosetAnimationId == 0xFFFFFFFF);
    CHECK(rt.bones[1].node.parentId == 0);
    CHECK(rt.bones[1].geosetId == 0);
    CHECK(rt.bones[1].geosetAnimationId == 1);
}

TEST_CASE("mdl_write_helper", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Helper h;
    h.node.name = "Overhead Ref";
    h.node.objectId = 5;
    h.node.parentId = 0;
    h.node.flags = Node::NodeFlag::Billboarded;
    model.helpers.push_back(h);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Helper \"Overhead Ref\"") != std::string::npos);
    CHECK(mdl.find("Billboarded,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.helpers.size() == 1);
    CHECK(rt.helpers[0].node.name == "Overhead Ref");
    CHECK(hasFlag(rt.helpers[0].node.flags, Node::NodeFlag::Billboarded));
}

TEST_CASE("mdl_write_light", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Light light;
    light.node.name = "Light0";
    light.node.objectId = 10;
    light.node.type = Node::NodeType::Light;
    light.node.flags = Node::NodeFlag::Light;
    light.type = Light::LightType::Omni;
    light.attenuationStart = 50.0f;
    light.attenuationEnd = 200.0f;
    light.intensity = 1.5f;
    light.color = Vector3f(1, 0.5f, 0.25f);
    light.ambientIntensity = 0.3f;
    light.ambientColor = Vector3f(0.1f, 0.1f, 0.1f);
    model.lights.push_back(light);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Light \"Light0\"") != std::string::npos);
    CHECK(mdl.find("Omnidirectional,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.lights.size() == 1);
    CHECK(rt.lights[0].type == Light::LightType::Omni);
    CHECK(approx(rt.lights[0].attenuationStart, 50.0f));
    CHECK(approx(rt.lights[0].attenuationEnd, 200.0f));
    CHECK(approx(rt.lights[0].intensity, 1.5f));
}

TEST_CASE("mdl_write_attachment", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Attachment att;
    att.node.name = "Sprite Overhead";
    att.node.objectId = 20;
    att.node.type = Node::NodeType::Attachment;
    att.node.flags = Node::NodeFlag::Attachment;
    att.attachmentId = 11;
    att.path = "Objects\\effect.mdl";
    model.attachments.push_back(att);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Attachment \"Sprite Overhead\"") != std::string::npos);
    CHECK(mdl.find("AttachmentID 11,") != std::string::npos);
    CHECK(mdl.find("Path \"Objects\\effect.mdl\",") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.attachments.size() == 1);
    CHECK(rt.attachments[0].attachmentId == 11);
    CHECK(rt.attachments[0].path == "Objects\\effect.mdl");
}

TEST_CASE("mdl_write_pivot_points", "[mdl_writer]") {
    Model model;
    model.version = 800;
    model.pivotPoints = {Vector3f(0, 0, 50), Vector3f(10, 0, 100)};

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("PivotPoints 2") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.pivotPoints.size() == 2);
    CHECK(approx(rt.pivotPoints[0].z, 50.0f));
    CHECK(approx(rt.pivotPoints[1].x, 10.0f));
}

TEST_CASE("mdl_write_particle_emitter2", "[mdl_writer]") {
    Model model;
    model.version = 800;

    ParticleEmitter2 pe2;
    pe2.node.name = "SmokeEmitter";
    pe2.node.objectId = 30;
    pe2.node.type = Node::NodeType::ParticleEmitter2;
    pe2.node.flags = Node::NodeFlag::ParticleEmitter;
    pe2.speed = 100.0f;
    pe2.variation = 0.5f;
    pe2.latitude = 45.0f;
    pe2.gravity = 9.8f;
    pe2.lifespan = 2.0f;
    pe2.emissionRate = 50.0f;
    pe2.length = 10.0f;
    pe2.width = 10.0f;
    pe2.filterMode = 1; // Additive
    pe2.rows = 2;
    pe2.columns = 2;
    pe2.headOrTail = 0; // Head
    pe2.tailLength = 1.0f;
    pe2.time = 0.5f;
    pe2.segmentColor = {Vector3f(1, 0, 0), Vector3f(0, 1, 0), Vector3f(0, 0, 1)};
    pe2.segmentAlpha = {255, 128, 0};
    pe2.segmentScaling = {1.0f, 2.0f, 0.5f};
    pe2.headInterval = {0, 0, 1};
    pe2.headDecayInterval = {0, 0, 1};
    pe2.tailInterval = {0, 0, 1};
    pe2.tailDecayInterval = {0, 0, 1};
    pe2.textureId = 0;
    model.particleEmitters2.push_back(pe2);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("ParticleEmitter2 \"SmokeEmitter\"") != std::string::npos);
    CHECK(mdl.find("Additive,") != std::string::npos);
    CHECK(mdl.find("Head,") != std::string::npos);
    CHECK(mdl.find("SegmentColor") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.particleEmitters2.size() == 1);
    auto& rpe2 = rt.particleEmitters2[0];
    CHECK(approx(rpe2.speed, 100.0f));
    CHECK(approx(rpe2.gravity, 9.8f));
    CHECK(rpe2.filterMode == 1);
    CHECK(rpe2.rows == 2);
    CHECK(rpe2.columns == 2);
    CHECK(rpe2.headOrTail == 0);
    CHECK(rpe2.segmentAlpha[0] == 255);
    CHECK(rpe2.segmentAlpha[1] == 128);
    CHECK(rpe2.segmentAlpha[2] == 0);
}

TEST_CASE("mdl_write_ribbon_emitter", "[mdl_writer]") {
    Model model;
    model.version = 800;

    RibbonEmitter re;
    re.node.name = "Trail";
    re.node.objectId = 40;
    re.node.type = Node::NodeType::RibbonEmitter;
    re.node.flags = Node::NodeFlag::RibbonEmitter;
    re.heightAbove = 5.0f;
    re.heightBelow = 5.0f;
    re.alpha = 1.0f;
    re.color = Vector3f(1, 0, 0);
    re.lifespan = 0.5f;
    re.emissionRate = 30;
    re.materialId = 1;
    model.ribbonEmitters.push_back(re);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("RibbonEmitter \"Trail\"") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.ribbonEmitters.size() == 1);
    CHECK(approx(rt.ribbonEmitters[0].heightAbove, 5.0f));
    CHECK(rt.ribbonEmitters[0].materialId == 1);
}

TEST_CASE("mdl_write_event_object", "[mdl_writer]") {
    Model model;
    model.version = 800;

    EventObject ev;
    ev.node.name = "SNDxFOOT";
    ev.node.objectId = 50;
    ev.node.type = Node::NodeType::EventObject;
    ev.node.flags = Node::NodeFlag::EventObject;
    ev.eventTrackTimes = {100, 500, 900};
    model.eventObjects.push_back(ev);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("EventObject \"SNDxFOOT\"") != std::string::npos);
    CHECK(mdl.find("EventTrack 3") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.eventObjects.size() == 1);
    REQUIRE(rt.eventObjects[0].eventTrackTimes.size() == 3);
    CHECK(rt.eventObjects[0].eventTrackTimes[0] == 100);
    CHECK(rt.eventObjects[0].eventTrackTimes[2] == 900);
}

TEST_CASE("mdl_write_camera", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Camera cam;
    cam.name = "Portrait";
    cam.position = Vector3f(0, -100, 80);
    cam.fieldOfView = 0.8f;
    cam.farClippingPlane = 1000.0f;
    cam.nearClippingPlane = 1.0f;
    cam.targetPosition = Vector3f(0, 0, 60);
    model.cameras.push_back(cam);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Camera \"Portrait\"") != std::string::npos);
    CHECK(mdl.find("FieldOfView") != std::string::npos);
    CHECK(mdl.find("Target") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.cameras.size() == 1);
    CHECK(rt.cameras[0].name == "Portrait");
    CHECK(approx(rt.cameras[0].position.y, -100.0f));
    CHECK(approx(rt.cameras[0].fieldOfView, 0.8f));
    CHECK(approx(rt.cameras[0].targetPosition.z, 60.0f));
}

TEST_CASE("mdl_write_collision_shape", "[mdl_writer]") {
    Model model;
    model.version = 800;

    CollisionShape cs;
    cs.node.name = "CollBox";
    cs.node.objectId = 60;
    cs.node.type = Node::NodeType::CollisionShape;
    cs.node.flags = Node::NodeFlag::CollisionShape;
    cs.type = CollisionShape::ShapeType::Box;
    cs.vertices = {Vector3f(-5, -5, 0), Vector3f(5, 5, 10)};
    model.collisionShapes.push_back(cs);

    CollisionShape sphere;
    sphere.node.name = "CollSphere";
    sphere.node.objectId = 61;
    sphere.node.type = Node::NodeType::CollisionShape;
    sphere.node.flags = Node::NodeFlag::CollisionShape;
    sphere.type = CollisionShape::ShapeType::Sphere;
    sphere.vertices = {Vector3f(0, 0, 50)};
    sphere.radius = 25.0f;
    model.collisionShapes.push_back(sphere);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Box,") != std::string::npos);
    CHECK(mdl.find("Sphere,") != std::string::npos);
    CHECK(mdl.find("BoundsRadius 25.0,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.collisionShapes.size() == 2);
    CHECK(rt.collisionShapes[0].type == CollisionShape::ShapeType::Box);
    CHECK(rt.collisionShapes[0].vertices.size() == 2);
    CHECK(rt.collisionShapes[1].type == CollisionShape::ShapeType::Sphere);
    CHECK(approx(rt.collisionShapes[1].radius, 25.0f));
}

TEST_CASE("mdl_write_face_effect", "[mdl_writer]") {
    Model model;
    model.version = 1000;

    FaceEffect fe;
    fe.target = "Face";
    fe.path = "war3mapImported\\face.facefx";
    model.faceEffects.push_back(fe);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("FaceFX \"Face\"") != std::string::npos);
    CHECK(mdl.find("Path \"war3mapImported\\face.facefx\",") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.faceEffects.size() == 1);
    CHECK(rt.faceEffects[0].target == "Face");
    CHECK(rt.faceEffects[0].path == "war3mapImported\\face.facefx");
}

TEST_CASE("mdl_write_bind_pose", "[mdl_writer]") {
    Model model;
    model.version = 1000;

    std::array<f32, 12> mat = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    model.bindPoses.push_back(mat);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("BindPose") != std::string::npos);
    CHECK(mdl.find("Matrices 1") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.bindPoses.size() == 1);
    CHECK(approx(rt.bindPoses[0][0], 1.0f));
    CHECK(approx(rt.bindPoses[0][5], 1.0f));
    CHECK(approx(rt.bindPoses[0][10], 1.0f));
    CHECK(approx(rt.bindPoses[0][11], 0.0f));
}

TEST_CASE("mdl_write_animated_track", "[mdl_writer]") {
    // Test that animation tracks survive round-trip
    Model model;
    model.version = 800;

    Bone bone;
    bone.node.name = "AnimBone";
    bone.node.objectId = 0;
    bone.node.type = Node::NodeType::Bone;
    bone.node.flags = Node::NodeFlag::Bone;
    bone.geosetId = Bone::MULTIPLE_GEOSETS;
    bone.geosetAnimationId = 0xFFFFFFFF;

    // Build a translation track with 2 Linear keyframes
    auto& tr = bone.node.translationTracks;
    tr.isUsed = true;
    tr.interpolationType = InterpolationType::Linear;
    tr.globalSequenceId = 0;
    tr.keyCount = 2;

    // Key 1: frame 0, value (0,0,0)
    auto pushKey = [](std::vector<u8>& buf, u32 frame, Vector3f val) {
        auto pushBytes = [&](const auto& v) {
            const u8* p = reinterpret_cast<const u8*>(&v);
            buf.insert(buf.end(), p, p + sizeof(v));
        };
        pushBytes(frame);
        pushBytes(val);
    };
    pushKey(tr.keys_data, 0, Vector3f(0, 0, 0));
    pushKey(tr.keys_data, 1000, Vector3f(10, 20, 30));

    model.bones.push_back(bone);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Translation 2") != std::string::npos);
    CHECK(mdl.find("Linear,") != std::string::npos);
    CHECK(mdl.find("0:") != std::string::npos);
    CHECK(mdl.find("1000:") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.bones.size() == 1);
    auto& rtTr = rt.bones[0].node.translationTracks;
    CHECK(rtTr.isUsed);
    CHECK(rtTr.interpolationType == InterpolationType::Linear);
    CHECK(rtTr.keyCount == 2);
    auto keys = rtTr.keys();
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].frame == 0);
    CHECK(approx(keys[0].value.x, 0.0f));
    CHECK(keys[1].frame == 1000);
    CHECK(approx(keys[1].value.x, 10.0f));
    CHECK(approx(keys[1].value.z, 30.0f));
}

TEST_CASE("mdl_write_hermite_track", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Bone bone;
    bone.node.name = "HermiteBone";
    bone.node.objectId = 0;
    bone.node.type = Node::NodeType::Bone;
    bone.node.flags = Node::NodeFlag::Bone;
    bone.geosetId = Bone::MULTIPLE_GEOSETS;
    bone.geosetAnimationId = 0xFFFFFFFF;

    auto& tr = bone.node.translationTracks;
    tr.isUsed = true;
    tr.interpolationType = InterpolationType::Hermite;
    tr.globalSequenceId = 0;
    tr.keyCount = 1;

    // Tangent key: frame, value, inTan, outTan
    auto pushTangentKey = [](std::vector<u8>& buf, u32 frame, Vector3f val, Vector3f inTan,
                             Vector3f outTan) {
        auto pushBytes = [&](const auto& v) {
            const u8* p = reinterpret_cast<const u8*>(&v);
            buf.insert(buf.end(), p, p + sizeof(v));
        };
        pushBytes(frame);
        pushBytes(val);
        pushBytes(inTan);
        pushBytes(outTan);
    };
    pushTangentKey(tr.keys_data, 500, Vector3f(1, 2, 3), Vector3f(0.1f, 0.2f, 0.3f),
                   Vector3f(0.4f, 0.5f, 0.6f));

    model.bones.push_back(bone);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Translation 1") != std::string::npos);
    CHECK(mdl.find("Hermite,") != std::string::npos);
    CHECK(mdl.find("InTan") != std::string::npos);
    CHECK(mdl.find("OutTan") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.bones.size() == 1);
    auto& rtTr = rt.bones[0].node.translationTracks;
    CHECK(rtTr.interpolationType == InterpolationType::Hermite);
    auto tkeys = rtTr.tangentKeys();
    REQUIRE(tkeys.size() == 1);
    CHECK(tkeys[0].frame == 500);
    CHECK(approx(tkeys[0].value.x, 1.0f));
    CHECK(approx(tkeys[0].inTan.x, 0.1f));
    CHECK(approx(tkeys[0].outTan.z, 0.6f));
}

TEST_CASE("mdl_write_popcorn_emitter", "[mdl_writer]") {
    Model model;
    model.version = 1000;

    CornEmitter ce;
    ce.node.name = "PopcornFire";
    ce.node.objectId = 70;
    ce.node.type = Node::NodeType::CornEmitter;
    ce.lifeSpan = 3.0f;
    ce.emissionRate = 100.0f;
    ce.speed = 50.0f;
    ce.path = "Effects\\fire.pkfx";
    model.cornEmitters.push_back(ce);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("ParticleEmitterPopcorn \"PopcornFire\"") != std::string::npos);
    CHECK(mdl.find("Path \"Effects\\fire.pkfx\",") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.cornEmitters.size() == 1);
    CHECK(rt.cornEmitters[0].node.name == "PopcornFire");
    CHECK(approx(rt.cornEmitters[0].lifeSpan, 3.0f));
    CHECK(rt.cornEmitters[0].path == "Effects\\fire.pkfx");
}

TEST_CASE("mdl_write_via_writer_class", "[mdl_writer]") {
    // Test the Writer class public API with MDLXFormat::MDL
    Model model;
    model.version = 800;
    model.modelName = "WriterTest";

    Sequence seq;
    seq.name = "Stand";
    seq.intervalStart = 0;
    seq.intervalEnd = 1000;
    model.sequences.push_back(seq);

    Writer writer;
    auto data = writer.write(model, MDLXFormat::MDL);

    std::string text(data.begin(), data.end());
    CHECK(text.find("Model \"WriterTest\"") != std::string::npos);
    CHECK(text.find("Anim \"Stand\"") != std::string::npos);
}

// ============================================================================
// Corpus round-trip tests: parse .mdl → write MDL → parse MDL → compare
// ============================================================================

static const char* CORPUS_SAURUS = "Corpus/MDL/saurus_warrior_unit_run.mdl";
static const char* CORPUS_ZOVAAL = "Corpus/MDL/Zovaal/Zovaal the Banished One.mdl";

TEST_CASE("mdl_writer_corpus_saurus_roundtrip", "[mdl_writer][corpus]") {
    auto src = readFile(CORPUS_SAURUS);
    if (src.empty()) {
        SKIP("Corpus file not found: " << CORPUS_SAURUS);
    }

    std::vector<std::string> issues1;
    auto model1 = convertMdlToModel(src, issues1);
    // Ignore parse issues for corpus files

    // Write back to MDL
    std::string mdlOut = writeModelToMdl(model1);
    REQUIRE(!mdlOut.empty());

    // Parse the written MDL
    std::vector<std::string> issues2;
    auto model2 = convertMdlToModel(mdlOut, issues2);

    // Compare key fields
    CHECK(model2.version == model1.version);
    CHECK(model2.modelName == model1.modelName);
    CHECK(model2.sequences.size() == model1.sequences.size());
    CHECK(model2.textures.size() == model1.textures.size());
    CHECK(model2.materials.size() == model1.materials.size());
    CHECK(model2.geosets.size() == model1.geosets.size());
    CHECK(model2.bones.size() == model1.bones.size());
    CHECK(model2.helpers.size() == model1.helpers.size());
    CHECK(model2.attachments.size() == model1.attachments.size());
    CHECK(model2.pivotPoints.size() == model1.pivotPoints.size());
    CHECK(model2.cameras.size() == model1.cameras.size());
    CHECK(model2.collisionShapes.size() == model1.collisionShapes.size());
    CHECK(model2.geosetAnimations.size() == model1.geosetAnimations.size());
    CHECK(model2.lights.size() == model1.lights.size());
    CHECK(model2.eventObjects.size() == model1.eventObjects.size());
    CHECK(model2.particleEmitters2.size() == model1.particleEmitters2.size());

    // Spot check vertex counts
    if (!model1.geosets.empty() && !model2.geosets.empty()) {
        CHECK(model2.geosets[0].vertexPositions.size() ==
              model1.geosets[0].vertexPositions.size());
        CHECK(model2.geosets[0].faces.size() == model1.geosets[0].faces.size());
    }

    // Spot check bone names
    for (size_t i = 0; i < std::min(model1.bones.size(), model2.bones.size()); ++i) {
        CHECK(model2.bones[i].node.name == model1.bones[i].node.name);
    }
}

TEST_CASE("mdl_writer_corpus_zovaal_roundtrip", "[mdl_writer][corpus]") {
    auto src = readFile(CORPUS_ZOVAAL);
    if (src.empty()) {
        SKIP("Corpus file not found: " << CORPUS_ZOVAAL);
    }

    std::vector<std::string> issues1;
    auto model1 = convertMdlToModel(src, issues1);

    std::string mdlOut = writeModelToMdl(model1);
    REQUIRE(!mdlOut.empty());

    std::vector<std::string> issues2;
    auto model2 = convertMdlToModel(mdlOut, issues2);

    CHECK(model2.version == model1.version);
    CHECK(model2.modelName == model1.modelName);
    CHECK(model2.sequences.size() == model1.sequences.size());
    CHECK(model2.textures.size() == model1.textures.size());
    CHECK(model2.materials.size() == model1.materials.size());
    CHECK(model2.geosets.size() == model1.geosets.size());
    CHECK(model2.bones.size() == model1.bones.size());
    CHECK(model2.pivotPoints.size() == model1.pivotPoints.size());
    CHECK(model2.cornEmitters.size() == model1.cornEmitters.size());
    CHECK(model2.faceEffects.size() == model1.faceEffects.size());
    CHECK(model2.bindPoses.size() == model1.bindPoses.size());
}

TEST_CASE("mdl_write_node_flags_roundtrip", "[mdl_writer]") {
    Model model;
    model.version = 800;

    Helper h;
    h.node.name = "FlagTest";
    h.node.objectId = 0;
    h.node.flags = Node::NodeFlag::DontInheritTranslation | Node::NodeFlag::DontInheritRotation |
                   Node::NodeFlag::BillboardedLockZ;
    model.helpers.push_back(h);

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("DontInheritTranslation,") != std::string::npos);
    CHECK(mdl.find("DontInheritRotation,") != std::string::npos);
    CHECK(mdl.find("BillboardedLockZ,") != std::string::npos);

    auto rt = parseMdl(mdl);
    REQUIRE(rt.helpers.size() == 1);
    CHECK(hasFlag(rt.helpers[0].node.flags, Node::NodeFlag::DontInheritTranslation));
    CHECK(hasFlag(rt.helpers[0].node.flags, Node::NodeFlag::DontInheritRotation));
    CHECK(hasFlag(rt.helpers[0].node.flags, Node::NodeFlag::BillboardedLockZ));
}

TEST_CASE("mdl_write_empty_model", "[mdl_writer]") {
    // An empty model should still produce valid MDL
    Model model;
    model.version = 800;
    model.modelName = "Empty";

    std::string mdl = writeModelToMdl(model);
    CHECK(mdl.find("Version") != std::string::npos);
    CHECK(mdl.find("Model \"Empty\"") != std::string::npos);

    auto rt = parseMdl(mdl);
    CHECK(rt.version == 800);
    CHECK(rt.modelName == "Empty");
    CHECK(rt.sequences.empty());
    CHECK(rt.geosets.empty());
    CHECK(rt.bones.empty());
}

TEST_CASE("mdl_write_layer_all_filter_modes", "[mdl_writer]") {
    // Verify all filter modes round-trip
    const Layer::FilterMode modes[] = {
        Layer::FilterMode::None,        Layer::FilterMode::Transparent,
        Layer::FilterMode::Blend,       Layer::FilterMode::Additive,
        Layer::FilterMode::AddAlpha,    Layer::FilterMode::Modulate,
        Layer::FilterMode::Modulate2x,
    };

    for (auto fm : modes) {
        Model model;
        model.version = 800;
        Material mat;
        Layer layer;
        layer.filterMode = fm;
        mat.layers.push_back(layer);
        model.materials.push_back(mat);

        std::string mdl = writeModelToMdl(model);
        auto rt = parseMdl(mdl);
        REQUIRE(rt.materials.size() == 1);
        REQUIRE(rt.materials[0].layers.size() == 1);
        CHECK(rt.materials[0].layers[0].filterMode == fm);
    }
}

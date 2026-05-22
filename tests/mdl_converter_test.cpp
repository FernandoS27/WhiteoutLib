// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MDL-to-Model converter test suite.
// Tests the semantic conversion from MDL text → mdx::Model.

#include <catch2/catch_all.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/mdx/types.h>

#include "whiteout/models/mdx/mdl_converter.h"

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

static bool approx(float a, float b) {
    return std::fabs(a - b) < EPS;
}

// ============================================================================
// Synthetic unit tests for specific block conversions
// ============================================================================

TEST_CASE("version_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Version {
    FormatVersion 1000,
}
)", issues);
    REQUIRE(issues.empty());
    CHECK(model.version == 1000);
}

TEST_CASE("model_block_basic", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Version {
    FormatVersion 800,
}
Model "TestModel" {
    BlendTime 150,
    MinimumExtent { -100.0, -100.0, -10.0 },
    MaximumExtent { 100.0, 100.0, 200.0 },
    BoundsRadius 250.0,
}
)", issues);
    REQUIRE(issues.empty());
    CHECK(model.modelName == "TestModel");
    CHECK(model.blendTime == 150);
    CHECK(approx(model.modelExtent.boundsRadius, 250.0f));
    CHECK(approx(model.modelExtent.minimum.x, -100.0f));
    CHECK(approx(model.modelExtent.maximum.z, 200.0f));
}

TEST_CASE("sequences_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Sequences 2 {
    Anim "Stand" {
        Interval { 0, 3333 },
        Rarity 1.0,
        MoveSpeed 0.0,
        MinimumExtent { -50.0, -50.0, 0.0 },
        MaximumExtent { 50.0, 50.0, 100.0 },
        BoundsRadius 80.0,
    }
    Anim "Walk" {
        Interval { 3334, 6667 },
        NonLooping,
        MoveSpeed 270.0,
    }
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.sequences.size() == 2);
    CHECK(model.sequences[0].name == "Stand");
    CHECK(model.sequences[0].intervalStart == 0);
    CHECK(model.sequences[0].intervalEnd == 3333);
    CHECK(approx(model.sequences[0].rarity, 1.0f));
    CHECK(approx(model.sequences[0].extent.boundsRadius, 80.0f));
    CHECK(model.sequences[1].name == "Walk");
    CHECK(model.sequences[1].intervalStart == 3334);
    CHECK(hasFlag(model.sequences[1].flags, Sequence::Flag::NonLooping));
    CHECK(approx(model.sequences[1].moveSpeed, 270.0f));
}

TEST_CASE("textures_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Textures 2 {
    Bitmap {
        Image "Textures\Diffuse.blp",
        WrapWidth,
        WrapHeight,
    }
    Bitmap {
        Image "",
        ReplaceableId 1,
    }
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.textures.size() == 2);
    CHECK(model.textures[0].fileName == "Textures\\Diffuse.blp");
    CHECK(hasFlag(model.textures[0].flags, Texture::Flag::WrapWidth));
    CHECK(hasFlag(model.textures[0].flags, Texture::Flag::WrapHeight));
    CHECK(model.textures[1].replaceableId == 1);
}

TEST_CASE("materials_with_layer", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Materials 1 {
    Material {
        Layer {
            FilterMode Blend,
            Unshaded,
            TwoSided,
            static TextureID 0,
            static Alpha 0.8,
        }
    }
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.materials.size() == 1);
    REQUIRE(model.materials[0].layers.size() == 1);
    auto& layer = model.materials[0].layers[0];
    CHECK(layer.filterMode == Layer::FilterMode::Blend);
    CHECK((static_cast<u32>(layer.shadingFlags) & 0x1) != 0); // Unshaded
    CHECK((static_cast<u32>(layer.shadingFlags) & 0x10) != 0); // TwoSided
}

TEST_CASE("bone_with_tracks", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Bone "bone_root" {
    ObjectId 0,
    GeosetId Multiple,
    GeosetAnimId None,
    Translation 2 {
        Linear,
        0: { 0.0, 0.0, 0.0 },
        1000: { 10.0, 20.0, 30.0 },
    }
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.bones.size() == 1);
    CHECK(model.bones[0].node.name == "bone_root");
    CHECK(model.bones[0].node.objectId == 0);
    CHECK(model.bones[0].geosetId == Bone::MULTIPLE_GEOSETS);
    CHECK(model.bones[0].geosetAnimationId == 0xFFFFFFFF);
    CHECK(model.bones[0].node.translationTracks.isUsed);
    CHECK(model.bones[0].node.translationTracks.keyCount == 2);
    CHECK(model.bones[0].node.translationTracks.interpolationType ==
          InterpolationType::Linear);
    // Verify key data (timestamps and values are stored separately).
    auto& tr = model.bones[0].node.translationTracks;
    auto values = tr.keys();
    REQUIRE(values.size() == 2);
    REQUIRE(tr.timestamps.size() == 2);
    CHECK(tr.timestamps[0] == 0);
    CHECK(approx(values[0].x, 0.0f));
    CHECK(tr.timestamps[1] == 1000);
    CHECK(approx(values[1].x, 10.0f));
    CHECK(approx(values[1].z, 30.0f));
}

TEST_CASE("bone_with_hermite_tangents", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Bone "bone_arm" {
    ObjectId 1,
    Parent 0,
    GeosetId 0,
    GeosetAnimId 0,
    Rotation 2 {
        Hermite,
        0: { 0.0, 0.0, 0.0, 1.0 },
            InTan { 0.0, 0.0, 0.0, 1.0 },
            OutTan { 0.0, 0.0, 0.0, 1.0 },
        500: { 0.0, 0.707, 0.0, 0.707 },
            InTan { 0.0, 0.5, 0.0, 0.866 },
            OutTan { 0.0, 0.5, 0.0, 0.866 },
    }
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.bones.size() == 1);
    CHECK(model.bones[0].node.parentId == 0);
    CHECK(model.bones[0].geosetId == 0);
    auto& rot = model.bones[0].node.rotationTracks;
    CHECK(rot.isUsed);
    CHECK(rot.interpolationType == InterpolationType::Hermite);
    CHECK(rot.keyCount == 2);
    auto tkeys = rot.tangentKeys();
    REQUIRE(tkeys.size() == 2);
    REQUIRE(rot.timestamps.size() == 2);
    CHECK(rot.timestamps[0] == 0);
    CHECK(approx(tkeys[0].value.w, 1.0f));
    CHECK(approx(tkeys[1].value.y, 0.707f));
}

TEST_CASE("helper_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Helper "helper_ref" {
    ObjectId 5,
    Parent 0,
    Billboarded,
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.helpers.size() == 1);
    CHECK(model.helpers[0].node.name == "helper_ref");
    CHECK(model.helpers[0].node.objectId == 5);
    CHECK(model.helpers[0].node.parentId == 0);
    CHECK((static_cast<u32>(model.helpers[0].node.flags) &
           static_cast<u32>(Node::NodeFlag::Billboarded)) != 0);
}

TEST_CASE("attachment_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Attachment "Origin Ref" {
    ObjectId 10,
    Parent 2,
    AttachmentID 0,
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.attachments.size() == 1);
    CHECK(model.attachments[0].node.name == "Origin Ref");
    CHECK(model.attachments[0].attachmentId == 0);
}

TEST_CASE("pivot_points", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
PivotPoints 3 {
    { 0.0, 0.0, 0.0 },
    { 10.0, 20.0, 30.0 },
    { -5.0, -10.0, 50.0 },
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.pivotPoints.size() == 3);
    CHECK(approx(model.pivotPoints[0].x, 0.0f));
    CHECK(approx(model.pivotPoints[1].y, 20.0f));
    CHECK(approx(model.pivotPoints[2].z, 50.0f));
}

TEST_CASE("geoset_basic", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Geoset {
    Vertices 3 {
        { 0.0, 0.0, 0.0 },
        { 1.0, 0.0, 0.0 },
        { 0.0, 1.0, 0.0 },
    }
    Normals 3 {
        { 0.0, 0.0, 1.0 },
        { 0.0, 0.0, 1.0 },
        { 0.0, 0.0, 1.0 },
    }
    TVertices 3 {
        { 0.0, 0.0 },
        { 1.0, 0.0 },
        { 0.0, 1.0 },
    }
    VertexGroup {
        0,
        0,
        0,
    }
    Faces 1 3 {
        Triangles {
            { 0, 1, 2 },
        }
    }
    Groups 1 1 {
        Matrices { 0 },
    }
    MaterialID 0,
    SelectionGroup 0,
    MinimumExtent { 0.0, 0.0, 0.0 },
    MaximumExtent { 1.0, 1.0, 0.0 },
    BoundsRadius 1.0,
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.geosets.size() == 1);
    auto& geo = model.geosets[0];
    REQUIRE(geo.vertexPositions.size() == 3);
    REQUIRE(geo.vertexNormals.size() == 3);
    REQUIRE(geo.textureCoordinateSets.size() == 1);
    REQUIRE(geo.textureCoordinateSets[0].size() == 3);
    REQUIRE(geo.vertexGroups.size() == 3);
    REQUIRE(geo.faces.size() == 3);
    CHECK(geo.faces[0] == 0);
    CHECK(geo.faces[1] == 1);
    CHECK(geo.faces[2] == 2);
    CHECK(geo.materialId == 0);
    CHECK(geo.matrixGroups.size() == 1);
    CHECK(geo.matrixIndices.size() == 1);
    CHECK(approx(geo.extent.boundsRadius, 1.0f));
}

TEST_CASE("geoset_anim_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
GeosetAnim {
    Alpha 1 {
        DontInterp,
        0: 1.0,
    }
    GeosetId 0,
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.geosetAnimations.size() == 1);
    CHECK(model.geosetAnimations[0].geosetId == 0);
    CHECK(model.geosetAnimations[0].alphaTracks.isUsed);
    CHECK(model.geosetAnimations[0].alphaTracks.keyCount == 1);
}

TEST_CASE("event_object", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
EventObject "SNDxFOOT" {
    ObjectId 50,
    EventTrack 3 {
        100,
        500,
        1200,
    }
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.eventObjects.size() == 1);
    CHECK(model.eventObjects[0].node.name == "SNDxFOOT");
    REQUIRE(model.eventObjects[0].eventTrackTimes.size() == 3);
    CHECK(model.eventObjects[0].eventTrackTimes[0] == 100);
    CHECK(model.eventObjects[0].eventTrackTimes[1] == 500);
    CHECK(model.eventObjects[0].eventTrackTimes[2] == 1200);
}

TEST_CASE("camera_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
Camera "Portrait" {
    Position { 10.0, 20.0, 30.0 },
    FieldOfView 0.9,
    FarClip 1000.0,
    NearClip 0.1,
    Target {
        Position { 0.0, 5.0, 15.0 },
    }
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.cameras.size() == 1);
    CHECK(model.cameras[0].name == "Portrait");
    CHECK(approx(model.cameras[0].position.x, 10.0f));
    CHECK(approx(model.cameras[0].fieldOfView, 0.9f));
    CHECK(approx(model.cameras[0].farClippingPlane, 1000.0f));
    CHECK(approx(model.cameras[0].targetPosition.y, 5.0f));
}

TEST_CASE("collision_shape_sphere", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
CollisionShape "col_body" {
    ObjectId 99,
    Sphere,
    Vertices 1 {
        { 0.0, 5.0, 30.0 },
    }
    BoundsRadius 20.0,
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.collisionShapes.size() == 1);
    CHECK(model.collisionShapes[0].type == CollisionShape::ShapeType::Sphere);
    CHECK(approx(model.collisionShapes[0].radius, 20.0f));
    REQUIRE(model.collisionShapes[0].vertices.size() == 1);
    CHECK(approx(model.collisionShapes[0].vertices[0].y, 5.0f));
}

TEST_CASE("face_fx_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
FaceFX "face" {
    Path "fx_data\face.fx",
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.faceEffects.size() == 1);
    CHECK(model.faceEffects[0].name == "face");
    CHECK(model.faceEffects[0].path == "fx_data\\face.fx");
}

TEST_CASE("bind_pose_block", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
BindPose {
    Matrices 2 {
        { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0 },
        { 0.5, 0.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.5, 10.0, 20.0, 30.0 },
    }
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.bindPoses.size() == 2);
    CHECK(approx(model.bindPoses[0][0], 1.0f));
    CHECK(approx(model.bindPoses[1][4], 0.5f));
    CHECK(approx(model.bindPoses[1][9], 10.0f));
}

TEST_CASE("particle_emitter_popcorn_as_corn", "[mdl_converter]") {
    std::vector<std::string> issues;
    auto model = convertMdlToModel(R"(
ParticleEmitterPopcorn "fx_sparkle" {
    ObjectId 20,
    static LifeSpan 1.5,
    static EmissionRate 10.0,
    static Speed 50.0,
    Path "Particles\sparkle.m3",
    AnimVisibilityGuide "attack",
}
)", issues);
    REQUIRE(issues.empty());
    REQUIRE(model.cornEmitters.size() == 1);
    CHECK(model.cornEmitters[0].node.name == "fx_sparkle");
    CHECK(model.cornEmitters[0].path == "Particles\\sparkle.m3");
    CHECK(model.cornEmitters[0].animVisibilityGuide == "attack");
}

// ============================================================================
// Parser integration: parse via Parser class with MDL format
// ============================================================================

TEST_CASE("parser_mdl_format_flag", "[mdl_converter]") {
    // Test that Parser::parse(buffer, MDLXFormat::MDL) works
    std::string source = R"(
Version {
    FormatVersion 800,
}
Model "ParserTest" {
    BlendTime 150,
}
)";
    std::vector<u8> buf(source.begin(), source.end());
    Parser parser;
    Model model = parser.parse(std::span<const u8>(buf), MDLXFormat::MDL);
    CHECK(model.version == 800);
    CHECK(model.modelName == "ParserTest");
    CHECK(model.blendTime == 150);
}

// ============================================================================
// Corpus integration tests
// ============================================================================

static const char* CORPUS_V800 = "Corpus/MDL/Ace/Ace.mdl";
static const char* CORPUS_ZOVAAL = "Corpus/MDL/Zovaal/Zovaal the Banished One.mdl";
static const char* CORPUS_ARTHAS = "Corpus/MDL/war3.w3mod/units/human/arthas/arthas.mdl";
static const char* CORPUS_DEMONHUNTER =
    "Corpus/MDL/Hero Female Demon Hunter Dominated/HeroFemaleDemonHunterDominated.mdl";

TEST_CASE("corpus_v800_model", "[mdl_converter][corpus]") {
    auto source = readFile(CORPUS_V800);
    REQUIRE(!source.empty());

    std::vector<std::string> issues;
    auto model = convertMdlToModel(source, issues);

    // Saurus warrior v800: no parse errors expected
    for (auto& i : issues) {
        INFO("Issue: " << i);
    }

    CHECK(model.version == 800);
    CHECK(!model.modelName.empty());
    CHECK(model.sequences.size() > 0);
    CHECK(model.textures.size() > 0);
    CHECK(model.materials.size() > 0);
    CHECK(model.geosets.size() > 0);
    CHECK(model.bones.size() > 0);
    CHECK(model.pivotPoints.size() > 0);

    // Verify geosets have geometry
    for (auto& g : model.geosets) {
        CHECK(g.vertexPositions.size() > 0);
        CHECK(g.faces.size() > 0);
    }
}

TEST_CASE("corpus_zovaal", "[mdl_converter][corpus]") {
    auto source = readFile(CORPUS_ZOVAAL);
    REQUIRE(!source.empty());

    std::vector<std::string> issues;
    auto model = convertMdlToModel(source, issues);

    CHECK(model.version == 1000);
    CHECK(!model.modelName.empty());
    CHECK(model.sequences.size() > 5); // Zovaal has many sequences
    CHECK(model.textures.size() > 5);
    CHECK(model.materials.size() > 5);
    CHECK(model.geosets.size() > 30);
    CHECK(model.bones.size() > 100);
    CHECK(model.attachments.size() > 5);
    CHECK(model.pivotPoints.size() > 100);
    CHECK(model.particleEmitters2.size() >= 1);
    CHECK(model.cornEmitters.size() >= 1); // ParticleEmitterPopcorn → CornEmitter
    CHECK(model.ribbonEmitters.size() >= 1);
    CHECK(model.cameras.size() >= 1);
    CHECK(model.eventObjects.size() >= 1);
    CHECK(model.collisionShapes.size() >= 1);
    CHECK(model.faceEffects.size() >= 1);
    CHECK(model.bindPoses.size() > 0);
}

TEST_CASE("corpus_arthas", "[mdl_converter][corpus]") {
    auto source = readFile(CORPUS_ARTHAS);
    REQUIRE(!source.empty());

    std::vector<std::string> issues;
    auto model = convertMdlToModel(source, issues);

    CHECK(model.version == 1200);
    CHECK(model.geosets.size() > 0);
    CHECK(model.bones.size() > 0);
    CHECK(model.materials.size() > 0);
}

TEST_CASE("corpus_demonhunter", "[mdl_converter][corpus]") {
    auto source = readFile(CORPUS_DEMONHUNTER);
    REQUIRE(!source.empty());

    std::vector<std::string> issues;
    auto model = convertMdlToModel(source, issues);

    CHECK(model.version == 1100);
    CHECK(model.geosets.size() > 0);
    CHECK(model.bones.size() > 0);
    CHECK(model.cornEmitters.size() >= 1); // DemonHunter has ParticleEmitterPopcorn
}

// ============================================================================
// Parser file extension detection
// ============================================================================

TEST_CASE("parser_file_extension_detection", "[mdl_converter]") {
    // Test the file-path parse method with a real .mdl file
    Parser parser;
    Model model = parser.parse(std::string(CORPUS_V800));
    CHECK(model.version == 800);
    CHECK(model.geosets.size() > 0);
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// EDIT_MODE_SC2_PROFILE_PLAN.md S1.3-S1.6: what a shipped `.m3` holds beside
/// its rig and its sequences -- billboards, leg IK, turrets, hit tests,
/// projectors, shadow boxes, lights, cameras, the whole event key -- comes out
/// as the record it was read from, straight and through a `.wem`; and a `.wem`
/// written before each of those blocks reads with none.

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/profile.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include "whiteout/common/binary_reader.h"
#include "whiteout/common/binary_writer.h"
#include "whiteout/common/streams.h"
#include "whiteout/models/wem/binary_read_visitor.h"
#include "whiteout/models/wem/binary_write_visitor.h"
#include "whiteout/models/wem/chunk_tags.h"
#include "wem_material_fixture.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

constexpr u32 kEventStream = 0x063F6D59u;

template <class T>
m3::AnimRef<T> still(const T& value) {
    m3::AnimRef<T> ref;
    ref.initValue = value;
    return ref;
}

m3::Bone bone(const char* name, u16 parent) {
    m3::Bone out;
    out.name = std::string(name);
    out.parentIndex = parent;
    out.position = still(Vector3f{0, 0, parent == 0xFFFF ? 0.0f : 1.0f});
    out.rotation = still(Quaternion{0, 0, 0, 1});
    out.scale = still(Vector3f{1, 1, 1});
    out.visibility = still(1u);
    return out;
}

/// A leg, a gun and a lamp, and one of every record S1 carries on them.
m3::Model shippedModel() {
    m3::Model model;
    model.name = "carry.m3";
    model.bones = {bone("Bone_Root", 0xFFFF), bone("Bone_Thigh", 0), bone("Bone_Shin", 1),
                   bone("Bone_Foot", 2),      bone("Bone_Gun", 0),    bone("Bone_Lamp", 0)};

    m3::Sequence sequence;
    sequence.name = "Stand";
    sequence.endFrame = 1000;
    model.sequences.push_back(sequence);
    m3::AnimationGroup group;
    group.name = "Stand";
    group.subtrackIndices = {0};
    model.animationGroups.push_back(group);
    m3::SubTrackContainer stc;
    stc.name = "Stand_full";
    m3::AnimBlock<m3::Event> events{};
    events.timestamps = {500};
    m3::Event hit;
    hit.name = "Evt_Hit";
    hit.boneIndex = 4;
    hit.optionString = "Splash";
    hit.extraParameter = 7;
    hit.unknown = 3;
    hit.rttChannelIndex = 2;
    hit.transform.data[3][2] = 2.5f;
    events.keys.push_back(hit);
    stc.sdev.push_back(events);
    stc.animIds.push_back(kEventStream);
    stc.animRefs.push_back(0);
    model.subTrackCollections.push_back(stc);

    m3::BillboardBehavior billboard;
    billboard.boneIndex = 4;
    billboard.billboardType = 0;
    billboard.cameraLookAt = 0;
    billboard.up = Quaternion{0, 0, 0.7071068f, 0.7071068f};
    billboard.forward = Quaternion{0, 0, 0, 1};
    model.billboardBehaviors.push_back(billboard);

    m3::IKJoint leg;
    leg.boneIndex1 = 1;
    leg.boneIndex2 = 3;
    leg.raycastUp = 3.0f;
    leg.raycastDown = -3.0f;
    leg.maxSpeed = 0.25f;
    leg.goalThreshold = 0.125f;
    model.ikJoints.push_back(leg);

    m3::TurretBehavior turret;
    turret.boneIndex = 4;
    turret.unknown1 = Vector4f{0, 0, 0.7071068f, 0.7071068f};
    turret.useAsMainTurret = 1;
    turret.turretGroupId = 1;
    turret.yawLimited = 2;
    turret.yawMin = -1.5f;
    turret.yawMax = 1.5f;
    turret.yawWeight = 0.75f;
    turret.pitchWeight = 0.5f;
    turret.unknown3 = 0.25f;
    turret.unknown4 = 2.0f;
    turret.mainBoneOffset = Vector3f{0, 0.5f, 0};
    model.turretBehaviors.push_back(turret);
    // An empty group before the one the turret is in: both come back, in order.
    m3::TriggerData head;
    head.name = "Head";
    m3::TriggerData chest;
    chest.name = "Chest";
    chest.dataIndices = {0};
    model.triggerData = {head, chest};

    model.tightHitTestObject.shapeType = m3::HitTestShapeType::Cylinder;
    model.tightHitTestObject.sizeX = 1.25f;
    model.tightHitTestObject.sizeY = 3.0f;
    m3::HitTestShape fuzzy;
    fuzzy.boneIndex = 4;
    fuzzy.sizeX = 0.5f;
    model.fuzzyHitTestObjects.push_back(fuzzy);

    m3::Projector projector;
    projector.bone = 4;
    projector.fieldOfView = still(0.6f);
    projector.falloff = 0.25f;
    projector.alphaInit = 0.5f;
    projector.alphaMid = 1.0f;
    projector.lifetimeHold = 2.0f;
    projector.layer = 3;
    projector.lodCut = 1;
    model.projections.push_back(projector);

    m3::ShadowBox box;
    box.boneIndex = 5;
    box.length = still(1.0f);
    box.width = still(2.0f);
    box.height = still(3.0f);
    model.shadowBoxes.push_back(box);

    m3::Light light;
    light.boneIndex = 5;
    light.lodCut = 2;
    light.shadowLodCut = 1;
    light.diffuseColor = still(Vector3f{1, 0.5f, 0.25f});
    light.intensityMultiplier = still(1.0f);
    light.specularMultiplier = still(0.5f);
    light.attenuationStart = still(1.0f);
    light.attenuationEnd = 6.0f;
    model.lights.push_back(light);

    m3::Camera camera;
    camera.boneIndex = 0;
    camera.name = "CameraPortrait";
    camera.fieldOfView = still(0.8f);
    camera.dofType = 3;
    camera.bokehFStop = still(2.8f);
    model.cameras.push_back(camera);
    return model;
}

std::string trimmed(std::string name) {
    while (!name.empty() && name.back() == '\0') {
        name.pop_back();
    }
    return name;
}

std::string boneName(const m3::Model& model, u32 index) {
    return index < model.bones.size() ? trimmed(model.bones[index].name) : std::string("<none>");
}

/// What `shippedModel` holds, read off an export of it.
void checkRecords(const m3::Model& out) {
    REQUIRE(out.billboardBehaviors.size() == 1u);
    const m3::BillboardBehavior& billboard = out.billboardBehaviors[0];
    CHECK(boneName(out, billboard.boneIndex) == "Bone_Gun");
    CHECK(billboard.billboardType == 0);
    CHECK(billboard.cameraLookAt == 0);
    CHECK(billboard.up.z == 0.7071068f);
    CHECK(billboard.forward.w == 1.0f);

    REQUIRE(out.ikJoints.size() == 1u);
    CHECK(boneName(out, out.ikJoints[0].boneIndex1) == "Bone_Thigh");
    CHECK(boneName(out, out.ikJoints[0].boneIndex2) == "Bone_Foot");
    CHECK(out.ikJoints[0].raycastUp == 3.0f);
    CHECK(out.ikJoints[0].raycastDown == -3.0f);
    CHECK(out.ikJoints[0].maxSpeed == 0.25f);
    CHECK(out.ikJoints[0].goalThreshold == 0.125f);

    REQUIRE(out.turretBehaviors.size() == 1u);
    const m3::TurretBehavior& turret = out.turretBehaviors[0];
    CHECK(boneName(out, turret.boneIndex) == "Bone_Gun");
    CHECK(turret.unknown1.z == 0.7071068f);
    CHECK(turret.useAsMainTurret == 1);
    CHECK(turret.turretGroupId == 1);
    CHECK(turret.yawLimited == 2u);
    CHECK(turret.yawMin == -1.5f);
    CHECK(turret.yawMax == 1.5f);
    CHECK(turret.yawWeight == 0.75f);
    CHECK(turret.pitchLimited == 0u);
    CHECK(turret.pitchWeight == 0.5f);
    CHECK(turret.unknown3 == 0.25f);
    CHECK(turret.unknown4 == 2.0f);
    CHECK(turret.mainBoneOffset.y == 0.5f);
    REQUIRE(out.triggerData.size() == 2u);
    CHECK(trimmed(out.triggerData[0].name) == "Head");
    CHECK(out.triggerData[0].dataIndices.empty());
    CHECK(trimmed(out.triggerData[1].name) == "Chest");
    CHECK(out.triggerData[1].dataIndices == std::vector<u32>{0});

    CHECK(out.tightHitTestObject.shapeType == m3::HitTestShapeType::Cylinder);
    CHECK(boneName(out, out.tightHitTestObject.boneIndex) == "Bone_Root");
    CHECK(out.tightHitTestObject.sizeX == 1.25f);
    CHECK(out.tightHitTestObject.sizeY == 3.0f);
    REQUIRE(out.fuzzyHitTestObjects.size() == 1u);
    CHECK(boneName(out, out.fuzzyHitTestObjects[0].boneIndex) == "Bone_Gun");
    CHECK(out.fuzzyHitTestObjects[0].sizeX == 0.5f);

    REQUIRE(out.projections.size() == 1u);
    const m3::Projector& projector = out.projections[0];
    CHECK(boneName(out, projector.bone) == "Bone_Gun");
    CHECK(projector.fieldOfView.initValue == 0.6f);
    CHECK(projector.falloff == 0.25f);
    CHECK(projector.alphaInit == 0.5f);
    CHECK(projector.alphaMid == 1.0f);
    CHECK(projector.lifetimeHold == 2.0f);
    CHECK(projector.layer == 3u);
    CHECK(projector.lodCut == 1u);

    REQUIRE(out.shadowBoxes.size() == 1u);
    CHECK(boneName(out, out.shadowBoxes[0].boneIndex) == "Bone_Lamp");
    CHECK(out.shadowBoxes[0].length.initValue == 1.0f);
    CHECK(out.shadowBoxes[0].width.initValue == 2.0f);
    CHECK(out.shadowBoxes[0].height.initValue == 3.0f);

    REQUIRE(out.lights.size() == 1u);
    const m3::Light& light = out.lights[0];
    CHECK(boneName(out, light.boneIndex) == "Bone_Lamp");
    CHECK(light.lodCut == 2u);
    CHECK(light.shadowLodCut == 1u);
    CHECK(light.diffuseColor.initValue.y == 0.5f);
    CHECK(light.specularMultiplier.initValue == 0.5f);
    CHECK(light.attenuationEnd == 6.0f);

    REQUIRE(out.cameras.size() == 1u);
    CHECK(trimmed(out.cameras[0].name) == "CameraPortrait");
    CHECK(out.cameras[0].fieldOfView.initValue == 0.8f);
    CHECK(out.cameras[0].dofType == 3u);
    CHECK(out.cameras[0].bokehFStop.initValue == 2.8f);

    const m3::Event* hit = nullptr;
    for (const m3::SubTrackContainer& stc : out.subTrackCollections) {
        for (const m3::AnimBlock<m3::Event>& block : stc.sdev) {
            for (const m3::Event& key : block.keys) {
                if (trimmed(key.name) == "Evt_Hit") {
                    hit = &key;
                }
            }
        }
    }
    REQUIRE(hit != nullptr);
    CHECK(boneName(out, hit->boneIndex) == "Bone_Gun");
    CHECK(trimmed(hit->optionString) == "Splash");
    CHECK(hit->extraParameter == 7u);
    CHECK(hit->unknown == 3u);
    CHECK(hit->rttChannelIndex == 2u);
    CHECK(hit->transform.data[3][2] == 2.5f);
}

std::vector<u8> writeWem(const Document& document) {
    std::vector<u8> bytes;
    common::vector_streambuf streambuf(bytes);
    std::ostream out(&streambuf);
    common::BinaryWriter writer(out);
    BinaryWriteVisitor visitor(writer);
    visitor.write(document, kCurrentVersion, {});
    return bytes;
}

Document readWem(const std::vector<u8>& bytes, std::vector<std::string>& issues) {
    common::span_streambuf streambuf(std::span<const u8>(bytes.data(), bytes.size()));
    std::istream in(&streambuf);
    common::BinaryReader reader(in);
    BinaryReadVisitor visitor(reader);
    Document document;
    visitor.read(document, kCurrentVersion);
    issues = visitor.issues();
    return document;
}

struct Chunk {
    u32 slot = 0;
    IndexEntry entry{};
    u32 end = 0; ///< Where the next chunk, or the index, starts.
};

std::vector<Chunk> chunksTagged(const std::vector<u8>& bytes, u32 tag) {
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::vector<IndexEntry> entries(header.indexCount);
    std::memcpy(entries.data(), bytes.data() + header.indexOffset, entries.size() * sizeof(IndexEntry));
    std::vector<Chunk> out;
    for (u32 i = 0; i < header.indexCount; ++i) {
        if (entries[i].tag != tag) {
            continue;
        }
        Chunk chunk{i, entries[i], header.indexOffset > entries[i].offset ? header.indexOffset
                                                                          : static_cast<u32>(bytes.size())};
        for (const IndexEntry& other : entries) {
            if (other.offset > chunk.entry.offset && other.offset < chunk.end) {
                chunk.end = other.offset;
            }
        }
        out.push_back(chunk);
    }
    return out;
}

/// Cuts @p cut out of @p chunk at each of @p at (back to front), slides the
/// rest of it back and refills its tail with the alignment fill, so nothing
/// outside it moves; then stamps it @p version.
void cutChunk(std::vector<u8>& bytes, const Chunk& chunk, std::vector<u32> at, std::span<const u8> cut, u32 version) {
    for (auto p = at.rbegin(); p != at.rend(); ++p) {
        REQUIRE(*p + cut.size() <= chunk.end);
        REQUIRE(std::memcmp(bytes.data() + *p, cut.data(), cut.size()) == 0);
        std::memmove(bytes.data() + *p, bytes.data() + *p + cut.size(), chunk.end - *p - cut.size());
    }
    std::memset(bytes.data() + chunk.end - cut.size() * at.size(), 0xAA, cut.size() * at.size());
    IndexEntry entry = chunk.entry;
    entry.version = version;
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::memcpy(bytes.data() + header.indexOffset + chunk.slot * sizeof(IndexEntry), &entry, sizeof(entry));
}

/// The file a writer before @p version + 1 made, for a field that follows a
/// marker: in every @p tag chunk, @p cut is cut from right after each copy of
/// @p marker. Returns how many it cut.
template <class T>
u32 forgeAfter(std::vector<u8>& bytes, u32 tag, const T& marker, std::span<const u8> cut, u32 version) {
    u32 total = 0;
    for (const Chunk& chunk : chunksTagged(bytes, tag)) {
        std::vector<u32> at;
        for (u32 p = chunk.entry.offset; p + sizeof(T) <= chunk.end; ++p) {
            if (std::memcmp(bytes.data() + p, &marker, sizeof(T)) == 0) {
                at.push_back(p + static_cast<u32>(sizeof(T)));
                p += static_cast<u32>(sizeof(T)) - 1;
            }
        }
        if (!at.empty()) {
            cutChunk(bytes, chunk, at, cut, version);
            total += static_cast<u32>(at.size());
        }
    }
    return total;
}

/// The same for the last field of a one-record chunk: the record ends where
/// the alignment fill starts.
u32 forgeTail(std::vector<u8>& bytes, u32 tag, std::span<const u8> cut, u32 version) {
    u32 total = 0;
    for (const Chunk& chunk : chunksTagged(bytes, tag)) {
        REQUIRE(chunk.entry.count == 1u);
        u32 last = chunk.end;
        while (last > chunk.entry.offset && bytes[last - 1] == 0xAA) {
            --last;
        }
        REQUIRE(last >= chunk.entry.offset + cut.size());
        cutChunk(bytes, chunk, {last - static_cast<u32>(cut.size())}, cut, version);
        ++total;
    }
    return total;
}

template <class T>
std::vector<u8> bytesOf(const T& value) {
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

} // namespace

TEST_CASE("wem m3 every record S1 carries comes back as it was read", "[wem][m3][s1]") {
    M3Converter converter;
    Result<Document> read = converter.fromM3(shippedModel(), ProfileId::Sc2);
    REQUIRE(read.ok());
    const Document& document = *read.value;
    const Model& model = document.models[0];

    // What the editor sees of them.
    REQUIRE(model.nodes.nodes[4].sc2Billboards.size() == 1u);
    CHECK(model.nodes.nodes[4].sc2Billboards[0].place == 0u);
    CHECK(model.nodes.nodes[5].sc2ShadowBox.has_value());
    REQUIRE(model.poseStages.size() == 2u);
    CHECK(model.poseStages[0].kind == StageKind::Sc2LegIk);
    CHECK(model.poseStages[0].driven == std::vector<u32>{1, 2, 3});
    CHECK(model.poseStages[1].kind == StageKind::Sc2Turret);
    CHECK(model.poseStages[1].driven == std::vector<u32>{4});
    REQUIRE(model.poseStages[1].turret.has_value());
    CHECK(model.poseStages[1].turret->group == "Chest");
    u32 tight = 0, fuzzy = 0, projectors = 0;
    for (const Node& node : model.nodes.nodes) {
        if (const auto* collision = std::get_if<CollisionPayload>(&node.payload)) {
            tight += collision->shape.hitTest == Sc2HitTest::Tight ? 1 : 0;
            fuzzy += collision->shape.hitTest == Sc2HitTest::Fuzzy ? 1 : 0;
        }
        if (node.kind == NodeKind::Projector) {
            ++projectors;
            CHECK(node.parent == 4u);
        }
    }
    CHECK(tight == 1u);
    CHECK(fuzzy == 1u);
    CHECK(projectors == 1u);
    REQUIRE(!document.clips.empty());
    REQUIRE(document.clips[0].events.size() == 1u);
    CHECK(document.clips[0].events[0].option == "Splash");
    CHECK(Validate(document, ValidateLevel::Profile).countOf(DiagCode::AnimStageInvalid) == 0u);

    SECTION("straight") {
        const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
        REQUIRE(written.ok());
        checkRecords(*written);
    }
    SECTION("through a .wem") {
        const std::vector<u8> bytes = Writer().write(document);
        const std::optional<Document> again = Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
        REQUIRE(again.has_value());
        const Result<m3::Model> written = converter.toM3(*again, ProfileId::Sc2, 29);
        REQUIRE(written.ok());
        checkRecords(*written);
    }
    SECTION("read and written as Heroes") {
        Result<Document> heroes = converter.fromM3(shippedModel(), ProfileId::Heroes);
        REQUIRE(heroes.ok());
        const Result<m3::Model> written = converter.toM3(*heroes.value, ProfileId::Heroes, 30);
        REQUIRE(written.ok());
        checkRecords(*written);
    }
}

TEST_CASE("wem m3 a stream no record of the model declares passes through under its own id", "[wem][m3][s1]") {
    constexpr u32 kUndeclared = 0x5EA70001u;
    m3::Model model = shippedModel();
    m3::AnimBlock<Vector3f> block{};
    block.timestamps = {0, 400, 1000};
    block.keys = {Vector3f{0, 0, 0}, Vector3f{1, 2, 3}, Vector3f{0, 0, 1}};
    model.subTrackCollections[0].sd3v.push_back(block);
    model.subTrackCollections[0].animIds.push_back(kUndeclared);
    model.subTrackCollections[0].animRefs.push_back((2u << 16) | 0u);

    M3Converter converter;
    Result<Document> read = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(read.ok());
    const Result<m3::Model> written = converter.toM3(*read.value, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    const m3::AnimBlock<Vector3f>* found = nullptr;
    for (const m3::SubTrackContainer& stc : written->subTrackCollections) {
        for (std::size_t i = 0; i < stc.animIds.size(); ++i) {
            if (stc.animIds[i] == kUndeclared && (stc.animRefs[i] >> 16) == 2u) {
                found = &stc.sd3v[stc.animRefs[i] & 0xFFFFu];
            }
        }
    }
    REQUIRE(found != nullptr);
    CHECK(found->timestamps == block.timestamps);
    REQUIRE(found->keys.size() == 3u);
    CHECK(found->keys[1].x == 1.0f);
    CHECK(found->keys[1].z == 3.0f);
}

TEST_CASE("wem m3 turret groups come back as listed, a shared name and a repeated turret too", "[wem][m3][s1]") {
    m3::Model model = shippedModel();
    model.turretBehaviors.push_back(model.turretBehaviors[0]);
    model.turretBehaviors[1].boneIndex = 5;
    m3::TriggerData first;
    first.name = "EyeRight";
    first.dataIndices = {0};
    m3::TriggerData second = first;
    second.dataIndices = {1, 1};
    model.triggerData = {first, second};

    M3Converter converter;
    Result<Document> read = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(read.ok());
    const Result<m3::Model> written = converter.toM3(*read.value, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->triggerData.size() == 2u);
    CHECK(trimmed(written->triggerData[0].name) == "EyeRight");
    CHECK(written->triggerData[0].dataIndices == std::vector<u32>{0});
    CHECK(trimmed(written->triggerData[1].name) == "EyeRight");
    CHECK(written->triggerData[1].dataIndices == std::vector<u32>{1, 1});
}

TEST_CASE("wem a game's own stage stays for the game that runs it and is left out for any other",
          "[wem][anim][stages][s1]") {
    M3Converter converter;
    Result<Document> read = converter.fromM3(shippedModel(), ProfileId::Sc2);
    REQUIRE(read.ok());
    CHECK(Profile(ProfileId::Sc2).nativeStages == Profile(ProfileId::Heroes).nativeStages);
    CHECK(Profile(ProfileId::Wc3Reforged).nativeStages == 0u);

    Document heroes = *read.value;
    Diagnostics kept;
    BakeStages(heroes, ProfileId::Heroes, nullptr, kept);
    CHECK(heroes.models[0].poseStages.size() == 2u);
    CHECK(kept.countOf(DiagCode::AnimStageNotBaked) == 0u);

    Document wc3 = *read.value;
    Diagnostics dropped;
    BakeStages(wc3, ProfileId::Wc3Reforged, nullptr, dropped);
    CHECK(wc3.models[0].poseStages.empty());
    CHECK(dropped.countOf(DiagCode::AnimStageNotBaked) == 2u);
    // Dropped, never baked: nothing keys the leg or the gun.
    for (const Clip& clip : wc3.clips) {
        for (const SubTrackContainer& container : clip.containers) {
            CHECK(container.subTracks.empty());
        }
    }
}

TEST_CASE("wem a turret turns one node and a leg runs down one chain", "[wem][anim][stages][s1]") {
    M3Converter converter;
    Result<Document> read = converter.fromM3(shippedModel(), ProfileId::Sc2);
    REQUIRE(read.ok());
    Document document = *read.value;
    document.models[0].poseStages[1].driven = {4, 5};
    document.models[0].poseStages[0].driven = {1, 3};
    CHECK(Validate(document, ValidateLevel::Profile).countOf(DiagCode::AnimStageInvalid) == 2u);
}

TEST_CASE("wem a .wem written before S1 reads with none of its blocks", "[wem][format][s1]") {
    constexpr f32 kNodeMarker = 4321.0625f;
    constexpr u32 kStageMarker = 0x7E57C107u;
    constexpr i32 kSetMarker = 0x7E575E7;
    constexpr u32 kEventMarker = 0x7E57E7E7u;

    Document document = wemfix::makeDocument(ProfileId::Sc2);
    Model& model = document.models[0];
    for (const char* name : {"a", "b"}) {
        Node node;
        node.name = name;
        node.kind = NodeKind::Helper;
        node.resetPayloadForKind();
        node.display.helper.inner = kNodeMarker;
        model.nodes.add(std::move(node));
    }
    PoseStage stage;
    stage.name = "look";
    stage.kind = StageKind::LookAt;
    stage.driven = {1};
    stage.cloth = kStageMarker;
    model.poseStages.push_back(stage);
    model.trackSets.push_back(TrackSet{"Upper", {}});
    model.profileSets[0].materials[0].SetNativeAuthoritative(native::M3Material{});
    Clip clip;
    clip.name = "Stand";
    clip.model = 0;
    clip.trackSets.push_back(ClipTrackSet{0, kSetMarker, false});
    ClipEvent event;
    event.name = "Evt_Hit";
    event.value = kEventMarker;
    clip.events.push_back(event);
    document.clips.push_back(clip);

    std::vector<u8> bytes = writeWem(document);
    const std::vector<u8> twoEmptyOptionals(2, 0);
    // NODE v21: no billboards (an empty list's count), no shadow box, after
    // the helper's gizmo.
    CHECK(forgeAfter(bytes, ChunkTagTraits<Node>::value, kNodeMarker, std::vector<u8>(5, 0), 21) == 2u);
    // PSTG v2: no leg IK, no turret, after `cloth`.
    CHECK(forgeAfter(bytes, ChunkTagTraits<PoseStage>::value, kStageMarker, twoEmptyOptionals, 2) == 1u);
    // CTSU v1: no opacity, after the priority.
    CHECK(forgeAfter(bytes, ChunkTagTraits<ClipTrackSet>::value, kSetMarker, std::vector<u8>{0}, 1) == 1u);
    // CLEV v1: none of the rest of the key, after the value: an empty option,
    // the identity, then extra, unknown and rtt.
    std::vector<u8> eventTail(sizeof(Reference), 0);
    const Matrix44f identity = Matrix44f::identity();
    for (const std::vector<u8>& part : {bytesOf(identity), bytesOf(0u), bytesOf(0xFFFFFFFFu), bytesOf(0u)}) {
        eventTail.insert(eventTail.end(), part.begin(), part.end());
    }
    CHECK(forgeAfter(bytes, ChunkTagTraits<ClipEvent>::value, kEventMarker, eventTail, 1) == 1u);
    // MESH v2: no bounds sections, the record's last field.
    CHECK(forgeTail(bytes, ChunkTagTraits<Mesh>::value, bytesOf(0u), 2) == 1u);
    // NM3_ v1: no record versions, the record's last field.
    CHECK(forgeTail(bytes, ChunkTagTraits<native::M3Material>::value, std::vector<u8>(sizeof(Reference), 0), 1) ==
          1u);

    std::vector<std::string> issues;
    const Document older = readWem(bytes, issues);
    CHECK(issues.empty());
    REQUIRE(older.models.size() == 1u);
    const Model& read = older.models[0];
    REQUIRE(read.nodes.size() == model.nodes.size());
    for (u32 n = 0; n < read.nodes.size(); ++n) {
        CHECK(read.nodes.nodes[n].name == model.nodes.nodes[n].name);
        CHECK(read.nodes.nodes[n].sc2Billboards.empty());
        CHECK(!read.nodes.nodes[n].sc2ShadowBox.has_value());
    }
    REQUIRE(read.poseStages.size() == 1u);
    CHECK(read.poseStages[0].cloth == kStageMarker);
    CHECK(!read.poseStages[0].legIk.has_value());
    CHECK(!read.poseStages[0].turret.has_value());
    REQUIRE(read.meshes.size() == 1u);
    CHECK(read.meshes[0].sc2Bounds.empty());
    CHECK(read.meshes[0].sections.size() == model.meshes[0].sections.size());
    const Material& material = read.profileSets[0].materials[0];
    REQUIRE(std::holds_alternative<native::M3Material>(material.Native()));
    CHECK(std::get<native::M3Material>(material.Native()).recordVersions.empty());
    REQUIRE(older.clips.size() == 1u);
    REQUIRE(older.clips[0].trackSets.size() == 1u);
    CHECK(older.clips[0].trackSets[0].priority == kSetMarker);
    // A v1 use reads as transparent.
    CHECK(older.clips[0].trackSets[0].concurrent);
    REQUIRE(older.clips[0].events.size() == 1u);
    const ClipEvent& key = older.clips[0].events[0];
    CHECK(key.name == "Evt_Hit");
    CHECK(key.value == kEventMarker);
    CHECK(key.option.empty());
    CHECK(key.unknown == 0xFFFFFFFFu);
    CHECK(key.extra == 0u);
}

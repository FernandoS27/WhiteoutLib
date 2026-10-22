// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// EDIT_MODE_SC2_PROFILE_PLAN.md S1.3-S1.6: what a shipped `.m3` holds beside
/// its rig and its sequences -- billboards, leg IK, turrets, hit tests,
/// projectors, shadow boxes, lights, cameras, the whole event key -- comes out
/// as the record it was read from, straight and through a `.wem`; and a `.wem`
/// written before each of those blocks reads with none.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/physics_upgrade.h>
#include <whiteout/models/m3/writer.h>
#include <whiteout/models/wem/anim/m3_math.h>
#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/attributes.h>
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
    // the identity, then extra, unknown and rtt, and v3's empty bag.
    std::vector<u8> eventTail(sizeof(Reference), 0);
    const Matrix44f identity = Matrix44f::identity();
    for (const std::vector<u8>& part : {bytesOf(identity), bytesOf(0u), bytesOf(0xFFFFFFFFu), bytesOf(0u),
                                        std::vector<u8>(sizeof(Reference), 0)}) {
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

TEST_CASE("wem m3 the export map names each region's section, vertices and faces", "[wem][m3][s3]") {
    // EDIT_MODE_SC2_PROFILE_PLAN.md S3.2: the drawn file's M3 arm reaches a
    // drawn vertex or triangle through these, so each must be the export's.
    const Document document = wemfix::makeDocument(ProfileId::Sc2);
    const Mesh& mesh = document.models[0].meshes[0];
    M3ExportMap map;
    const Result<m3::Model> written = M3Converter{}.toM3(document, ProfileId::Sc2, 0, {}, &map);
    REQUIRE(written.ok());
    REQUIRE(written->divisions.size() == 1u);
    const m3::MeshDivision& division = written->divisions[0];
    REQUIRE(map.regionElements.size() == division.regions.size());
    REQUIRE(map.regionMesh.size() == division.regions.size());
    REQUIRE(map.regionSection.size() == division.regions.size());
    REQUIRE(map.regionPalette.size() == division.regions.size());
    const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
    for (u32 r = 0; r < division.regions.size(); ++r) {
        CAPTURE(r);
        const m3::Region& region = division.regions[r];
        const DrawnElements& drawn = map.regionElements[r];
        CHECK(map.regionMesh[r] == 0u);
        CHECK(map.regionSection[r] == r);
        CHECK(map.sectionRegion[0][r] == r);
        REQUIRE(drawn.vertices.size() == region.vertexCount);
        REQUIRE(drawn.halfedges.size() == region.vertexCount);
        REQUIRE(drawn.triangles.size() == region.indexCount);
        REQUIRE(drawn.triangleFaces.size() == region.indexCount / 3);
        for (u32 i = 0; i < region.indexCount; ++i) {
            CHECK(drawn.triangles[i] == division.faces[region.firstIndex + i]);
            REQUIRE(drawn.triangles[i] < drawn.vertices.size());
        }
        // One triangle per section, on its own three vertices, in face order.
        for (const u32 face : drawn.triangleFaces) {
            CHECK(face == r);
        }
        for (const u32 vertex : drawn.vertices) {
            REQUIRE(vertex < positions.size());
            CHECK(positions[vertex].x >= static_cast<f32>(r) * 4.0f);
            CHECK(positions[vertex].x <= static_cast<f32>(r) * 4.0f + 1.0f);
        }
    }
    // The preview's map is the same export, run for the map alone.
    const M3ExportMap again = M3ExportMapOf(document, ProfileId::Sc2);
    CHECK(again.regionSection == map.regionSection);
    CHECK(again.regionPalette == map.regionPalette);
    CHECK(again.clipSequence == map.clipSequence);
}

TEST_CASE("wem m3 a bone's scale crosses into WEM's axes and back, and an older file's is traded on read",
          "[wem][m3][s3]") {
    // G-S3e found it: a scale kept in the file's axes poses a bone scaled
    // unevenly another way than the game does.
    m3::Model shipped = shippedModel();
    shipped.bones[4].scale.initValue = Vector3f{2, 3, 4};
    shipped.bones[4].rotation.initValue = Quaternion{0, 0, 0.3826834f, 0.9238795f};
    M3Converter converter;
    Result<Document> read = converter.fromM3(shipped, ProfileId::Sc2);
    REQUIRE(read.ok());
    Document document = *read.value;
    Node& gun = document.models[0].nodes.nodes[4];
    CHECK(gun.local.scale.x == 3.0f);
    CHECK(gun.local.scale.y == 2.0f);
    CHECK(gun.local.scale.z == 4.0f);
    // The local in WEM's axes is the file's, conjugated: the frame the game
    // composes.
    Transform file;
    file.translation = shipped.bones[4].position.initValue;
    file.rotation = shipped.bones[4].rotation.initValue;
    file.scale = shipped.bones[4].scale.initValue;
    const Matrix44f back = M3FrameOf(ToMatrix(gun.local));
    const Matrix44f wanted = ToMatrix(file);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK(std::abs(back.data[r][c] - wanted.data[r][c]) < 1e-5f);
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    const auto gunBone = std::find_if(written->bones.begin(), written->bones.end(),
                                      [](const m3::Bone& b) { return b.name == "Bone_Gun"; });
    REQUIRE(gunBone != written->bones.end());
    CHECK(gunBone->scale.initValue.x == 2.0f);
    CHECK(gunBone->scale.initValue.y == 3.0f);

    // A scale key on it, as a NODE v22 file kept it: in the file's axes.
    Model& model = document.models[0];
    AnimChannel channel;
    channel.id = model.animChannels.nextFreeId();
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = 4;
    channel.target.channel = Channel::Scale;
    channel.valueType = geom::AttrType::F32x3;
    const Vector3f rest{3, 2, 4}, key{6, 5, 7};
    channel.initValue.resize(sizeof(rest));
    std::memcpy(channel.initValue.data(), &rest, sizeof(rest));
    model.animChannels.add(channel);
    Clip clip;
    clip.name = "Grow";
    clip.model = 0;
    clip.duration = 1.0f;
    SubTrack track;
    track.channel = channel.id;
    track.times = {0.0f};
    track.values.resize(sizeof(key));
    std::memcpy(track.values.data(), &key, sizeof(key));
    clip.containers.emplace_back().subTracks.push_back(track);
    document.clips.push_back(clip);

    std::vector<u8> bytes = writeWem(document);
    const auto reread = [&] {
        std::optional<Document> parsed = Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
        REQUIRE(parsed);
        return *parsed;
    };
    const auto scaleOf = [](const Document& d) { return d.models[0].nodes.nodes[4].local.scale; };
    const auto keyOf = [](const Document& d) {
        Vector3f v;
        std::memcpy(&v, d.clips.back().containers[0].subTracks[0].values.data(), sizeof(v));
        return v;
    };
    // Written now, it reads as written.
    {
        const Document now = reread();
        CHECK(scaleOf(now).x == 3.0f);
        CHECK(keyOf(now).x == 6.0f);
    }
    // Stamped v22, it was the file's: traded, rest, channel rest and key.
    for (const Chunk& chunk : chunksTagged(bytes, ChunkTagTraits<Node>::value)) {
        IndexEntry entry = chunk.entry;
        entry.version = 22;
        WEMHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        std::memcpy(bytes.data() + header.indexOffset + chunk.slot * sizeof(IndexEntry), &entry, sizeof(entry));
    }
    const Document old = reread();
    CHECK(scaleOf(old).x == 2.0f);
    CHECK(scaleOf(old).y == 3.0f);
    CHECK(keyOf(old).x == 5.0f);
    CHECK(keyOf(old).y == 6.0f);
    Vector3f channelRest;
    std::memcpy(&channelRest, old.models[0].animChannels.channels.back().initValue.data(), sizeof(channelRest));
    CHECK(channelRest.x == 2.0f);
}

TEST_CASE("wem m3 a light's kind is its LITE type, and an older file's is re-typed on read", "[wem][m3][s3]") {
    // Read one off, an omni came in as a spot and went out as a spot again, so
    // a round trip hid it while every shipped light drew as a shut cone.
    m3::Model shipped = shippedModel();
    shipped.lights.push_back(shipped.lights[0]);
    shipped.lights[1].lightType = m3::LightType::Spot;
    REQUIRE(static_cast<u16>(shipped.lights[0].lightType) == 1u);
    M3Converter converter;
    Result<Document> read = converter.fromM3(shipped, ProfileId::Sc2);
    REQUIRE(read.ok());
    Document document = *read.value;
    const auto lightsOf = [](Document& d) {
        std::vector<LightPayload*> out;
        for (Node& node : d.models[0].nodes.nodes)
            if (auto* light = std::get_if<LightPayload>(&node.payload))
                out.push_back(light);
        return out;
    };
    std::vector<LightPayload*> lights = lightsOf(document);
    REQUIRE(lights.size() == 2);
    CHECK(lights[0]->kind == LightKind::Omni);
    CHECK(lights[1]->kind == LightKind::Spot);
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->lights.size() == 2);
    CHECK(static_cast<u16>(written->lights[0].lightType) == 1u);
    CHECK(static_cast<u16>(written->lights[1].lightType) == 2u);

    // The kinds the old reading made, beside the records as the file wrote them.
    lights[0]->kind = LightKind::Spot;
    lights[1]->kind = LightKind::Directional;
    std::vector<u8> bytes = writeWem(document);
    const auto reread = [&] {
        std::optional<Document> parsed = Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
        REQUIRE(parsed);
        return *parsed;
    };
    // Written now, a kind is the document's own and reads as written.
    {
        Document now = reread();
        CHECK(lightsOf(now)[0]->kind == LightKind::Spot);
        CHECK(lightsOf(now)[1]->kind == LightKind::Directional);
    }
    // Stamped v24, they were the old reading's: re-typed.
    for (const Chunk& chunk : chunksTagged(bytes, ChunkTagTraits<Node>::value)) {
        IndexEntry entry = chunk.entry;
        entry.version = 24;
        WEMHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        std::memcpy(bytes.data() + header.indexOffset + chunk.slot * sizeof(IndexEntry), &entry, sizeof(entry));
    }
    Document old = reread();
    CHECK(lightsOf(old)[0]->kind == LightKind::Omni);
    CHECK(lightsOf(old)[1]->kind == LightKind::Spot);
}

namespace {

m3::TextureLayer paintLayer(const char* path) {
    m3::TextureLayer layer;
    layer.texturePath = path;
    layer.color.initValue = m3::ColorBGRA{255, 255, 255, 255};
    layer.mapAlpha.initValue = 1.0f;
    return layer;
}

/// `shippedModel()` wearing one Standard material: a diffuse and a specular.
m3::Model paintedModel() {
    m3::Model model = shippedModel();
    m3::StandardMaterial standard;
    standard.name = "painted";
    standard.diffuseLayer = paintLayer("diffuse.dds");
    standard.specularLayer = paintLayer("spec.dds");
    model.standardMaterials.push_back(standard);
    m3::MaterialMap entry;
    entry.materialType = m3::MaterialType::Standard;
    entry.materialIndex = 0;
    model.materialMaps.push_back(entry);
    return model;
}

native::M3Material blockOf(const Document& document) {
    return std::get<native::M3Material>(document.models[0].setFor(ProfileId::Sc2)->materials[0].Native());
}

native::M3Standard& standardOf(native::M3Material& block) {
    return std::get<native::M3Standard>(block.body);
}

std::vector<u8> writtenM3(const Document& document) {
    const Result<m3::Model> written = M3Converter{}.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    return m3::Writer().write(*written);
}

AnimChannel layerChannel(Model& model, u32 ordinal) {
    AnimChannel channel;
    channel.id = model.animChannels.nextFreeId();
    channel.target.kind = TrackTarget::Kind::MaterialLayer;
    channel.target.material.profile = ProfileId::Sc2;
    channel.target.material.slot = 0;
    channel.target.sub = ordinal;
    channel.target.channel = Channel::Alpha;
    channel.valueType = geom::AttrType::F32;
    return channel;
}

} // namespace

TEST_CASE("wem m3 a material block edited in place writes back that field and nothing else", "[wem][m3][s4]") {
    const M3Converter converter;
    Result<Document> read = converter.fromM3(paintedModel(), ProfileId::Sc2);
    REQUIRE(read.ok());
    Document document = *read.value;
    const std::vector<u8> was = writtenM3(document);

    // Unchanged, it is the same file.
    MaterialBlockResult same = converter.setMaterialBlock(document, {0, ProfileId::Sc2, 0, blockOf(document)});
    REQUIRE(same.ok);
    CHECK(writtenM3(document) == was);
    CHECK(document.models[0].setFor(ProfileId::Sc2)->materials[0].sync() == NativeSync::InSync);

    // One field: that field, and the file otherwise as it was.
    native::M3Material block = blockOf(document);
    standardOf(block).priority = 3;
    REQUIRE(converter.setMaterialBlock(document, {0, ProfileId::Sc2, 0, block}).ok);
    const Result<m3::Model> edited = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(edited.ok());
    REQUIRE(edited->standardMaterials.size() == 1u);
    CHECK(edited->standardMaterials[0].priority == 3);
    m3::Model restored = *edited;
    restored.standardMaterials[0].priority = 0;
    CHECK(m3::Writer().write(restored) == was);

    // Not a StarCraft set: refused, nothing written.
    CHECK_FALSE(converter.setMaterialBlock(document, {0, ProfileId::Wc3Reforged, 0, block}).ok);
}

TEST_CASE("wem m3 a block edit adds its new paths and carries a channel by its layer", "[wem][m3][s4]") {
    const M3Converter converter;
    Result<Document> read = converter.fromM3(paintedModel(), ProfileId::Sc2);
    REQUIRE(read.ok());
    Document document = *read.value;
    Model& model = document.models[0];
    // The specular is ordinal 1, after the diffuse.
    const u32 id = model.animChannels.channels[model.animChannels.add(layerChannel(model, 1))].id;
    const auto target = [&]() -> const TrackTarget& { return model.animChannels.find(id)->target; };
    const std::size_t textures = document.textures.size();

    // A decal goes between them: a new texture, and the channel follows its layer.
    native::M3Material block = blockOf(document);
    standardOf(block).decalLayer = native::M3TextureLayer{};
    standardOf(block).decalLayer->texturePath = "decal.dds";
    standardOf(block).decalLayer->mapAlpha.initValue = 1.0f;
    MaterialBlockResult added = converter.setMaterialBlock(document, {0, ProfileId::Sc2, 0, block});
    REQUIRE(added.ok);
    CHECK(document.textures.size() == textures + 1);
    CHECK(document.textures.back().path == "decal.dds");
    CHECK(added.channelsRemapped == 1u);
    CHECK(target().sub == 2u);

    // The specular gone: the channel is invalidated, never dropped.
    block = blockOf(document);
    standardOf(block).specularLayer.reset();
    MaterialBlockResult removed = converter.setMaterialBlock(document, {0, ProfileId::Sc2, 0, block});
    REQUIRE(removed.ok);
    CHECK(removed.channelsInvalidated == 1u);
    CHECK(target().material.slot == kInvalidIndex);
}

TEST_CASE("wem m3 a channel the block does not name binds to its layer on export", "[wem][m3][s4]") {
    const M3Converter converter;
    Result<Document> read = converter.fromM3(paintedModel(), ProfileId::Sc2);
    REQUIRE(read.ok());
    Document document = *read.value;
    Model& model = document.models[0];
    const AnimChannel made = model.animChannels.channels[model.animChannels.add(layerChannel(model, 0))];
    REQUIRE_FALSE(document.clips.empty());
    SubTrack track;
    track.channel = made.id;
    track.times = {0.0f, 0.5f};
    for (const f32 alpha : {1.0f, 0.25f}) {
        const u8* bytes = reinterpret_cast<const u8*>(&alpha);
        track.values.insert(track.values.end(), bytes, bytes + sizeof(alpha));
    }
    document.clips[0].containers[0].subTracks.push_back(track);

    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->standardMaterials.size() == 1u);
    const m3::AnimRef<f32>& alpha = written->standardMaterials[0].diffuseLayer->mapAlpha;
    CHECK(alpha.animId != 0u);
    CHECK(alpha.flags == 0x6);
    bool streamed = false;
    for (const m3::SubTrackContainer& stc : written->subTrackCollections)
        streamed = streamed || std::find(stc.animIds.begin(), stc.animIds.end(), alpha.animId) != stc.animIds.end();
    CHECK(streamed);
    // The specular, which no channel names, keeps its rest.
    CHECK(written->standardMaterials[0].specularLayer->mapAlpha.flags == 0);
}

TEST_CASE("wem m3 a new material takes the model's style and one diffuse over its colour map", "[wem][m3][s4]") {
    const M3Converter converter;
    m3::Model styled = paintedModel();
    styled.standardMaterials[0].blendMode = m3::BlendMode::AlphaBlend;
    styled.standardMaterials[0].flags = m3::MaterialFlag::GeometryVisible | m3::MaterialFlag::TwoSided |
                                        m3::MaterialFlag::NormalBlend;
    Result<Document> read = converter.fromM3(styled, ProfileId::Sc2);
    REQUIRE(read.ok());
    const Document& document = *read.value;
    REQUIRE(document.textures.size() >= 2u);

    native::M3Material made = converter.defaultMaterialBlock(document, 0, ProfileId::Sc2, 1);
    const native::M3Standard& standard = standardOf(made);
    CHECK(standard.blendMode == static_cast<native::M3BlendMode>(m3::BlendMode::AlphaBlend));
    // The style's flags, but not the normal blend: no factors came with it.
    CHECK(static_cast<u32>(standard.flags) ==
          static_cast<u32>(m3::MaterialFlag::GeometryVisible | m3::MaterialFlag::TwoSided));
    REQUIRE(standard.diffuseLayer.has_value());
    CHECK(standard.diffuseLayer->texturePath == document.textures[1].path);
    CHECK(standard.diffuseLayer->rgbMultiply.initValue == 1.0f);
    CHECK_FALSE(standard.specularLayer.has_value());

    // A model with no material yet gets the drawn floor.
    Result<Document> bare = converter.fromM3(shippedModel(), ProfileId::Sc2);
    REQUIRE(bare.ok());
    Document empty = *bare.value;
    empty.textures.push_back(TextureRef{});
    empty.textures.back().path = "a.dds";
    native::M3Material floor = converter.defaultMaterialBlock(empty, 0, ProfileId::Sc2, 0);
    CHECK(static_cast<u32>(standardOf(floor).flags) == static_cast<u32>(m3::MaterialFlag::GeometryVisible));
    CHECK(standardOf(floor).diffuseLayer->texturePath == "a.dds");
}

TEST_CASE("wem m3 the export map names each emitter node's runtime emitter", "[wem][m3][s5]") {
    // EDIT_MODE_SC2_PROFILE_PLAN.md S5.3: a PAR_ is its own emitter, a PARC
    // a slot of its source's, a RIB_ its ribbon.
    m3::Model model = shippedModel();
    m3::ParticleEmitter first;
    first.boneIndex = 4;
    m3::ParticleEmitter second;
    second.boneIndex = 5;
    second.copyIndices = {0};
    model.particleEmitters = {first, second};
    m3::ParticleEmitterCopy copy;
    copy.boneIndex = 2;
    model.particleEmitterCopies = {copy};
    m3::RibbonEmitter ribbon;
    ribbon.boneIndex = 3;
    model.ribbonEmitters = {ribbon};
    const M3Converter converter;
    Result<Document> read = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(read.ok());
    const Document& document = *read.value;
    M3ExportMap map;
    REQUIRE(converter.toM3(document, ProfileId::Sc2, 29, {}, &map).ok());
    const NodeTree& tree = document.models[0].nodes;
    REQUIRE(map.nodeEmitter.size() == tree.size());
    std::vector<u32> particles, copies, ribbons;
    for (u32 n = 0; n < tree.size(); ++n) {
        const Node& node = tree.nodes[n];
        if (const auto* particle = std::get_if<Sc2ParticleEmitterPayload>(&node.payload))
            (particle->isCopy() ? copies : particles).push_back(n);
        else if (node.kind == NodeKind::Sc2RibbonEmitter)
            ribbons.push_back(n);
        else
            CHECK(map.nodeEmitter[n] == kInvalidIndex);
    }
    REQUIRE(particles.size() == 2u);
    REQUIRE(copies.size() == 1u);
    REQUIRE(ribbons.size() == 1u);
    CHECK(map.nodeEmitter[particles[0]] == 0u);
    CHECK(map.nodeEmitter[particles[1]] == 1u);
    CHECK(map.nodeEmitter[copies[0]] == 1u);
    CHECK(map.nodeEmitter[ribbons[0]] == 0u);
}

TEST_CASE("wem m3 a model particle's orientation words cross raw, and an older file's link reads back as the index",
          "[wem][m3][x0]") {
    // EDIT_MODE_SC2_EFFECTS_PLAN.md X0 L2: PAR_+0x5D0 / +0x5D4 are the model
    // pose's legacy orientation and its variant, whatever ribbon they index.
    m3::Model model = shippedModel();
    m3::ParticleEmitter oriented;
    oriented.boneIndex = 4;
    oriented.spawnRibbonOnBounceChance = 0.25f;
    oriented.ribbonLinkIndex = 1;
    m3::ParticleEmitter past;
    past.boneIndex = 5;
    past.ribbonLinkIndex = 7; // indexes no ribbon, and still crosses
    model.particleEmitters = {oriented, past};
    m3::RibbonEmitter ribbon;
    ribbon.boneIndex = 3;
    model.ribbonEmitters = {ribbon, ribbon};
    const M3Converter converter;
    Result<Document> read = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(read.ok());
    Document document = *read.value;
    std::vector<u32> particles, ribbons;
    for (u32 n = 0; n < document.models[0].nodes.size(); ++n) {
        if (std::holds_alternative<Sc2ParticleEmitterPayload>(document.models[0].nodes.nodes[n].payload))
            particles.push_back(n);
        if (document.models[0].nodes.nodes[n].kind == NodeKind::Sc2RibbonEmitter)
            ribbons.push_back(n);
    }
    REQUIRE(particles.size() == 2u);
    REQUIRE(ribbons.size() == 2u);
    auto& first = std::get<Sc2ParticleEmitterPayload>(document.models[0].nodes.nodes[particles[0]].payload);
    auto& second = std::get<Sc2ParticleEmitterPayload>(document.models[0].nodes.nodes[particles[1]].payload);
    CHECK(first.modelOrientation == 0.25f);
    CHECK(first.modelOrientationVariant == 1);
    CHECK(second.modelOrientationVariant == 7);
    CHECK(first.ribbonLink == kInvalidNode);
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->particleEmitters.size() == 2u);
    CHECK(written->particleEmitters[0].spawnRibbonOnBounceChance == 0.25f);
    CHECK(written->particleEmitters[0].ribbonLinkIndex == 1);
    CHECK(written->particleEmitters[1].ribbonLinkIndex == 7);

    // As a NODE v25 file kept it: the variant was a link to the ribbon it
    // indexed, and none where it indexed none.
    first.ribbonLink = ribbons[1];
    second.ribbonLink = kInvalidNode;
    // The variant follows the marker, and is cut: an older chunk had none.
    constexpr f32 kMarker = 1234.5f;
    constexpr i32 kCut = 3;
    first.noiseSmoothness = second.noiseSmoothness = kMarker;
    first.modelOrientationVariant = second.modelOrientationVariant = kCut;
    std::vector<u8> bytes = writeWem(document);
    // Both of v26's fields go: the variant, then the splat's projector (L4).
    std::vector<u8> cut = bytesOf(kCut);
    const std::vector<u8> projector = bytesOf(kInvalidNode);
    cut.insert(cut.end(), projector.begin(), projector.end());
    REQUIRE(forgeAfter(bytes, ChunkTagTraits<Node>::value, kMarker, cut, 25) == 2u);
    const std::optional<Document> old = Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(old);
    const auto& oldFirst = std::get<Sc2ParticleEmitterPayload>(old->models[0].nodes.nodes[particles[0]].payload);
    const auto& oldSecond = std::get<Sc2ParticleEmitterPayload>(old->models[0].nodes.nodes[particles[1]].payload);
    CHECK(oldFirst.modelOrientationVariant == 1);
    CHECK(oldSecond.modelOrientationVariant == -1);
    CHECK(oldFirst.ribbonLink == kInvalidNode);
    CHECK(oldFirst.modelOrientation == 0.25f);
}

TEST_CASE("wem m3 the export derives a system's local fallback channels from the model's own fields",
          "[wem][m3][x0]") {
    // EDIT_MODE_SC2_EFFECTS_PLAN.md X0 L6: the runtime collects fields through
    // the fallback pair, and the exporter writes its local half as the local
    // mask over the fields' channels. A field made in the editor pushes then.
    m3::Model model = shippedModel();
    m3::ParticleEmitter particle;
    particle.boneIndex = 4;
    particle.localForces = 0x3;
    particle.worldForces = 0x10;
    particle.worldForcesFallback = 0; // stale, as an edit leaves it
    model.particleEmitters = {particle};
    m3::RibbonEmitter ribbon;
    ribbon.boneIndex = 3;
    ribbon.localForces = 0x6;
    model.ribbonEmitters = {ribbon};
    m3::Force force;
    force.boneIndex = 2;
    force.localChannels = 0x2;
    force.forceVersion(m3::kCurrentForceVersion);
    model.forces = {force};
    const M3Converter converter;
    Result<Document> read = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(read.ok());
    const Result<m3::Model> written = converter.toM3(*read.value, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->particleEmitters.size() == 1u);
    REQUIRE(written->ribbonEmitters.size() == 1u);
    CHECK(written->particleEmitters[0].localForcesFallback == 0x2u);
    CHECK(written->particleEmitters[0].worldForcesFallback == 0x10u);
    CHECK(written->ribbonEmitters[0].localForcesFallback == 0x2u);
}

TEST_CASE("wem m3 a spline point's end tangent and end offset cross whole, through a .wem too", "[wem][m3][x0]") {
    m3::Model model = shippedModel();
    m3::RibbonEmitter ribbon;
    ribbon.boneIndex = 3;
    m3::SplineRibbon point;
    point.boneIndex = 2;
    point.endTangent = Vector3f{0.25f, -0.5f, 1.44f};
    point.endOffset = Vector3f{0.125f, 0.75f, -2.0f};
    ribbon.splineRibbons = {point};
    model.ribbonEmitters = {ribbon};
    const M3Converter converter;
    Result<Document> read = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(read.ok());
    const std::vector<u8> bytes = writeWem(*read.value);
    const std::optional<Document> reread = Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(reread);
    for (const Document* document : {static_cast<const Document*>(&*read.value), static_cast<const Document*>(&*reread)}) {
        const Result<m3::Model> written = converter.toM3(*document, ProfileId::Sc2, 29);
        REQUIRE(written.ok());
        REQUIRE(written->ribbonEmitters.size() == 1u);
        REQUIRE(written->ribbonEmitters[0].splineRibbons.size() == 1u);
        const m3::SplineRibbon& back = written->ribbonEmitters[0].splineRibbons[0];
        CHECK(back.endTangent.x == 0.25f);
        CHECK(back.endTangent.y == -0.5f);
        CHECK(back.endTangent.z == 1.44f);
        CHECK(back.endOffset.x == 0.125f);
        CHECK(back.endOffset.y == 0.75f);
        CHECK(back.endOffset.z == -2.0f);
    }
}

TEST_CASE("wem m3 a splat's projector is a node link, and an older file's index reads back as that node",
          "[wem][m3][x7]") {
    // EDIT_MODE_SC2_EFFECTS_PLAN.md X7 L4: PAR_'s splatProjectionIndex names
    // a PROJ, which WEM holds as a Projector node since NODE v22.
    m3::Model model = shippedModel();
    m3::ParticleEmitter splatting;
    splatting.boneIndex = 4;
    splatting.splatProjectionIndex = 1;
    splatting.splatChance = 0.5f;
    m3::ParticleEmitter plain;
    plain.boneIndex = 5;
    plain.splatProjectionIndex = 9; // past the projectors: none
    model.particleEmitters = {splatting, plain};
    m3::Projector projector;
    projector.bone = 3;
    model.projections = {projector, projector};
    const M3Converter converter;
    Result<Document> read = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(read.ok());
    Document document = *read.value;
    std::vector<u32> particles, projectors;
    for (u32 n = 0; n < document.models[0].nodes.size(); ++n) {
        if (std::holds_alternative<Sc2ParticleEmitterPayload>(document.models[0].nodes.nodes[n].payload))
            particles.push_back(n);
        if (document.models[0].nodes.nodes[n].kind == NodeKind::Projector)
            projectors.push_back(n);
    }
    REQUIRE(particles.size() == 2u);
    REQUIRE(projectors.size() == 2u);
    auto& first = std::get<Sc2ParticleEmitterPayload>(document.models[0].nodes.nodes[particles[0]].payload);
    auto& second = std::get<Sc2ParticleEmitterPayload>(document.models[0].nodes.nodes[particles[1]].payload);
    CHECK(first.splatProjector == projectors[1]);
    CHECK(second.splatProjector == kInvalidNode);
    CHECK(first.splatProjectionIndex == -1);
    // A link names a projector, and nothing else.
    {
        Document wrong = document;
        std::get<Sc2ParticleEmitterPayload>(wrong.models[0].nodes.nodes[particles[0]].payload).splatProjector =
            particles[1];
        CHECK(Validate(wrong, ValidateLevel::Structural).countOf(DiagCode::DanglingNodeReference) > 0u);
        CHECK(Validate(document, ValidateLevel::Structural).countOf(DiagCode::DanglingNodeReference) == 0u);
    }
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->particleEmitters.size() == 2u);
    CHECK(written->particleEmitters[0].splatProjectionIndex == 1);
    CHECK(written->particleEmitters[1].splatProjectionIndex == -1);

    // As a NODE v25 file kept it: the file's index, and neither v26 field
    // (each record holds the same two, so one cut serves both).
    constexpr f32 kMarker = 4321.5f;
    constexpr i32 kVariant = 3;
    first.noiseSmoothness = second.noiseSmoothness = kMarker;
    first.modelOrientationVariant = second.modelOrientationVariant = kVariant;
    first.splatProjector = second.splatProjector = kInvalidNode;
    first.splatProjectionIndex = 1;
    second.splatProjectionIndex = 9;
    std::vector<u8> bytes = writeWem(document);
    std::vector<u8> cut = bytesOf(kVariant);
    const std::vector<u8> none = bytesOf(kInvalidNode);
    cut.insert(cut.end(), none.begin(), none.end());
    REQUIRE(forgeAfter(bytes, ChunkTagTraits<Node>::value, kMarker, cut, 25) == 2u);
    const std::optional<Document> old = Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(old);
    const auto& oldFirst = std::get<Sc2ParticleEmitterPayload>(old->models[0].nodes.nodes[particles[0]].payload);
    const auto& oldSecond = std::get<Sc2ParticleEmitterPayload>(old->models[0].nodes.nodes[particles[1]].payload);
    CHECK(oldFirst.splatProjector == projectors[1]);
    CHECK(oldSecond.splatProjector == kInvalidNode);
    CHECK(oldFirst.splatProjectionIndex == -1);
    CHECK(oldFirst.splatChance == 0.5f);
}

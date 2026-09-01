// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P4 step 2 — the v3 container, round-tripped.
///
/// The parity test proved the *visitors* do not change the bytes. This one
/// proves the v3 *document* survives them: every `reflect()` written across P1's
/// geometry, P1's nodes and P3's materials is instantiated here, because a
/// `reflect()` is a template and an uninstantiated one compiles no matter what
/// it says.
///
/// The fixture reaches the awkward cases rather than the typical ones: every
/// material kind, so all four body chunks are exercised; both feature payloads;
/// all three `TextureKey` alternatives plus the empty one; a node of every kind,
/// so every payload alternative is visited; an optional that is set beside one
/// that is not; a repair log — the structure nothing produces unless the input
/// was non-manifold, and therefore the one most likely to be forgotten; and a
/// clip whose sub-tracks include a Hermite one, because that is the only
/// interpolation whose keys are three values wide (§10.8.2).

#include <cstring>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/document.h>

#include "test_helpers.h"
#include "whiteout/common/binary_reader.h"
#include "whiteout/common/binary_writer.h"
#include "whiteout/common/streams.h"
#include "whiteout/models/wem/binary_read_visitor.h"
#include "whiteout/models/wem/binary_write_visitor.h"
#include "whiteout/models/wem/text_dump.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

std::vector<u8> writeDocument(const Document& document) {
    std::vector<u8> buffer;
    buffer.reserve(64 * 1024);
    common::vector_streambuf streambuf(buffer);
    std::ostream out(&streambuf);
    common::BinaryWriter writer(out);
    BinaryWriteVisitor visitor(writer);
    visitor.write(document, kCurrentVersion);
    buffer.shrink_to_fit();
    return buffer;
}

Document readDocument(const std::vector<u8>& bytes, std::vector<std::string>& issues) {
    common::span_streambuf streambuf(std::span<const u8>(bytes.data(), bytes.size()));
    std::istream in(&streambuf);
    common::BinaryReader reader(in);
    Document document;
    BinaryReadVisitor visitor(reader);
    visitor.read(document, kCurrentVersion);
    issues = visitor.issues();
    return document;
}

std::string dump(const Document& document) {
    std::ostringstream out;
    TextDump(out, "document", const_cast<Document&>(document));
    return out.str();
}

/// One value of @p T as the bytes a channel's stream holds. `SubTrack::values`
/// is a typed blob and the type lives on the channel, so a fixture writing keys
/// by hand is the honest way to prove the reader agrees about the width.
template <class T>
std::vector<u8> valueBytes(const T& value) {
    std::vector<u8> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

std::vector<u8> concat(std::vector<u8> first, const std::vector<u8>& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

SubTrack makeTrack(u32 channel, Interpolation interp, std::vector<f32> times,
                   std::vector<u8> values) {
    SubTrack track;
    track.channel = channel;
    track.interp = interp;
    track.times = std::move(times);
    track.values = std::move(values);
    return track;
}

TextureInput makeInput(u32 texture, u32 uvSet) {
    TextureInput input;
    input.texture = texture;
    input.uvSet = uvSet;
    input.constant = {0.25f, 0.5f, 0.75f, 1.0f};
    input.mapping = UVMappingMode::EnvSphere;
    input.colorSpace = ColorSpace::Linear;
    input.wrapU = WrapMode::Clamp;
    input.wrapV = WrapMode::Repeat;
    input.uvTransform.m[0][2] = 0.5f;
    input.uvTransform.m[1][1] = 2.0f;
    input.weight = 0.75f;
    return input;
}

/// One material per kind, so every body chunk is written and read.
Material makeMaterial(MaterialKind kind) {
    Material material;
    material.name = std::string("mat-") + ToString(kind);
    CommonMaterial& common = material.InitCommon();
    common.blend = BlendMode::AlphaBlend;
    common.cull = CullMode::None;
    common.depth.test = true;
    common.depth.write = false;
    common.depth.bias = -0.5f;
    common.alphaTestThreshold = 0.75f;
    common.priorityPlane = -2;
    common.flags = MaterialFlags::Unlit | MaterialFlags::SortNearZ;
    common.setKind(kind);

    switch (kind) {
    case MaterialKind::Composite: {
        CompositeBody& body = *common.composite();
        body.layers.push_back(
            CompositeLayer{makeInput(0, 0), SurfaceChannel::Color, CompositeOp::Set});
        body.layers.push_back(
            CompositeLayer{makeInput(1, 1), SurfaceChannel::Emissive, CompositeOp::Add});
        body.specularExponent = 12.5f;
        body.environmentFactor = 0.25f;
        break;
    }
    case MaterialKind::Combiners: {
        CombinersBody& body = *common.combiners();
        body.stages.push_back(
            CombinerStage{makeInput(0, 0), CombinerOp::Opaque, CombinerOp::Opaque});
        body.stages.push_back(CombinerStage{makeInput(1, 0), CombinerOp::Mod2x, CombinerOp::Mod});
        break;
    }
    case MaterialKind::LegacyDeferred: {
        LegacyDeferredBody& body = *common.legacy();
        body.set(LegacySlot::Diffuse, makeInput(0, 0));
        body.set(LegacySlot::Normal, makeInput(1, 0));
        body.set(LegacySlot::Emissive, makeInput(2, 1));
        body.specularExponent = 30.0f;
        break;
    }
    case MaterialKind::PBRDeferred: {
        PbrDeferredBody& body = *common.pbr();
        body.set(PbrSlot::BaseColor, makeInput(0, 0));
        body.set(PbrSlot::Orm, makeInput(1, 0));
        body.metallicFactor = 0.25f;
        body.roughnessFactor = 0.75f;
        break;
    }
    case MaterialKind::Count:
        break;
    }

    MaterialFeature fresnel;
    fresnel.id = 1;
    fresnel.layer = 0;
    fresnel.payload = FresnelFeature{{0.2f, 0.4f, 0.6f}, 3.0f, 0.1f, 0.9f, 0.5f};
    common.features.push_back(fresnel);

    MaterialFeature uv;
    uv.id = 2;
    uv.layer = kWholeMaterial;
    UvAnimationFeature animation;
    animation.scrollRate = {0.5f, -0.25f};
    animation.rotateRate = 1.5f;
    uv.payload = animation;
    common.features.push_back(uv);
    return material;
}

Node makeNode(const std::string& name, NodeKind kind, u32 parent) {
    Node node;
    node.name = name;
    node.parent = parent;
    node.kind = kind;
    node.pivot = {1.0f, 2.0f, 3.0f};
    node.local.translation = {0.5f, 0.0f, -0.5f};
    node.local.scale = {2.0f, 2.0f, 2.0f};
    node.uniformScaleOnly = true;
    node.poses.push_back(Transform::identity());
    node.resetPayloadForKind();
    node.native.set("sourceIndex", 7);
    return node;
}

Document makeDocument() {
    Document document;
    document.name = "v3-fixture";
    document.declare(ProfileId::Wc3Classic);
    document.declare(ProfileId::Sc2);
    document.defaultProfile = ProfileId::Sc2;
    document.unitScale = 0.5f;
    document.bounds.minimum = {-1, -1, -1};
    document.bounds.maximum = {1, 1, 1};
    document.bounds.sphereRadius = 1.75f;

    // All four key alternatives, including the empty one.
    TextureRef byPath;
    byPath.key = TexturePath{"textures/base.blp"};
    byPath.path = "textures/base.blp";
    byPath.replaceableId = 1;
    document.textures.push_back(byPath);

    TextureRef byId;
    byId.key = TextureFileDataId{1234567};
    byId.path = "world/generic.blp";
    byId.slotType = 5;
    byId.declaredSpace = ColorSpace::Srgb;
    document.textures.push_back(byId);

    TextureRef bySno;
    bySno.key = TextureSnoId{44, 8921};
    document.textures.push_back(bySno);

    document.textures.push_back(TextureRef{});

    Model model;
    model.name = "model0";
    model.addSlot("body");
    model.addSlot("cloak");

    geom::FaceSet faces;
    faces.vertexCount = 4;
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(2, 1, 3);

    Mesh mesh;
    mesh.name = "mesh0";
    mesh.lodLevel = 1;
    mesh.setFaceSet(faces);
    geom::AttrLayer& position =
        mesh.attributes.create(geom::names::kPosition, geom::Domain::Vertex, geom::AttrType::F32x3);
    position.data.assign(4 * 3 * sizeof(f32), 0);
    mesh.attributes.create(geom::names::kSection, geom::Domain::Face, geom::AttrType::U32);

    mesh.skin.reset(4);
    const geom::Influence one{0, 1.0f};
    for (u32 i = 0; i < 4; ++i) {
        mesh.skin.appendVertex(std::span<const geom::Influence>(&one, 1));
    }

    MeshSection section;
    section.name = "section0";
    section.materialSlot = 0;
    section.selectionGroup = 21;
    section.flags = SectionFlags::Hidden;
    section.rigidNode = 3; // the optional that IS set
    mesh.sections.push_back(section);

    MeshSection second;
    second.name = "section1";
    second.materialSlot = 1; // and the one that is not
    mesh.sections.push_back(second);

    // A repair log, because nothing produces one unless the input was bad.
    mesh.repairLog.splits.push_back(geom::VertexSplit{1, 3});
    mesh.repairLog.droppedFaces.push_back(geom::FaceRecord{{0, 1, 2}, 0, 5});
    model.meshes.push_back(std::move(mesh));

    model.nodes.poseSchema.push_back(PoseSchema{"bind", PoseSpace::Model, false});
    model.nodes.add(makeNode("root", NodeKind::Helper, kInvalidNode));
    model.nodes.add(makeNode("bone", NodeKind::Bone, 0));
    model.nodes.add(makeNode("hand", NodeKind::Attachment, 1));
    model.nodes.add(makeNode("lamp", NodeKind::Light, 0));
    model.nodes.add(makeNode("cam", NodeKind::Camera, 0));
    model.nodes.add(makeNode("dust", NodeKind::ParticleEmitter, 1));
    model.nodes.add(makeNode("trail", NodeKind::RibbonEmitter, 1));
    model.nodes.add(makeNode("hit", NodeKind::Event, 1));
    model.nodes.add(makeNode("hull", NodeKind::CollisionShape, 0));

    ProfileMaterialSet wc3;
    wc3.profile = ProfileId::Wc3Classic;
    wc3.looks.add("A", 1);
    wc3.materials.push_back(makeMaterial(MaterialKind::Composite));
    wc3.materials.push_back(makeMaterial(MaterialKind::Combiners));
    wc3.resizeBindings(model.materialSlots.size());
    wc3.slotBindings[0].byLook[0] = 0;
    wc3.slotBindings[1].byLook[0] = 1;
    model.profileSets.push_back(std::move(wc3));

    ProfileMaterialSet sc2;
    sc2.profile = ProfileId::Sc2;
    sc2.looks.add("A", 0);
    sc2.looks.add("B", 2);
    sc2.materials.push_back(makeMaterial(MaterialKind::LegacyDeferred));
    sc2.materials.push_back(makeMaterial(MaterialKind::PBRDeferred));
    sc2.resizeBindings(model.materialSlots.size());
    sc2.slotBindings[0].byLook[0] = 0;
    sc2.slotBindings[0].byLook[1] = 1;
    // Slot 1 is left unbound in this profile on purpose — a hole must survive.
    model.profileSets.push_back(std::move(sc2));

    // --- animation (§10.8) ---------------------------------------------------
    //
    // One channel per target kind, one sub-track per interpolation width, and a
    // container of each transparency — the axes that decide how the bytes are
    // laid out.
    AnimChannel bone;
    bone.id = 7;
    bone.target.kind = TrackTarget::Kind::Node;
    bone.target.node = 1;
    bone.target.channel = Channel::Rotation;
    bone.valueType = geom::AttrType::Quat;
    bone.initValue = valueBytes(Quaternion{0, 0, 0, 1});
    model.animChannels.add(bone);

    AnimChannel layerAlpha;
    layerAlpha.id = 9;
    layerAlpha.target.kind = TrackTarget::Kind::MaterialLayer;
    layerAlpha.target.material.profile = ProfileId::Wc3Classic;
    layerAlpha.target.material.slot = 0;
    layerAlpha.target.material.look = 0;
    layerAlpha.target.sub = 1;
    layerAlpha.target.channel = Channel::Alpha;
    layerAlpha.valueType = geom::AttrType::F32;
    model.animChannels.add(layerAlpha); // no rest value: the empty case

    AnimChannel scroll;
    scroll.id = 11;
    scroll.target.kind = TrackTarget::Kind::MaterialFeature;
    scroll.target.material.profile = ProfileId::Sc2;
    scroll.target.material.slot = 0;
    scroll.target.material.look = 1;
    scroll.target.sub = 3; // a feature id, which nothing renumbers
    scroll.target.channel = Channel::UvTranslate;
    scroll.valueType = geom::AttrType::F32x3;
    scroll.initValue = valueBytes(Vector3f{0, 0, 0});
    model.animChannels.add(scroll);

    model.animSet = 0;
    model.bounds = document.bounds;
    document.models.push_back(std::move(model));

    Clip clip;
    clip.name = "Stand";
    clip.model = 0;
    clip.duration = 2.5f;
    clip.looping = true;
    clip.flags = ClipFlags::AutoPlay | ClipFlags::WorldClocked;
    clip.native.set("sourceSequence", static_cast<i64>(3));

    SubTrackContainer base;
    base.name = "base";
    base.priority = 0;
    base.concurrent = false;
    base.native.set("interpTypeRow", static_cast<i64>(2));
    base.subTracks.push_back(makeTrack(7, Interpolation::Slerp,
                                       {0.0f, 1.25f},
                                       concat(valueBytes(Quaternion{0, 0, 0, 1}),
                                              valueBytes(Quaternion{0, 1, 0, 0}))));
    // Hermite: value, inTangent, outTangent per key, sharing the stream.
    base.subTracks.push_back(makeTrack(9, Interpolation::Hermite, {0.0f},
                                       concat(valueBytes(1.0f),
                                              concat(valueBytes(0.0f), valueBytes(-0.5f)))));
    clip.containers.push_back(std::move(base));

    SubTrackContainer overlay;
    overlay.name = "overlay";
    overlay.priority = 4;
    overlay.concurrent = true;
    overlay.subTracks.push_back(makeTrack(11, Interpolation::Step, {0.0f, 0.5f},
                                          concat(valueBytes(Vector3f{0, 0, 0}),
                                                 valueBytes(Vector3f{1, 0, 0}))));
    clip.containers.push_back(std::move(overlay));

    clip.events.push_back(ClipEvent{0.75f, 7, "footstep", 42});
    document.clips.push_back(std::move(clip));

    AnimSet core;
    core.name = "core";
    core.byTag.push_back(AnimTag{0x1234, 0});
    document.animSets.push_back(std::move(core));

    return document;
}

} // namespace

TEST_CASE("wem a v3 document survives write and read", "[wem][format][v3]") {
    const Document original = makeDocument();
    const std::vector<u8> bytes = writeDocument(original);
    REQUIRE(bytes.size() > 0);

    std::vector<std::string> issues;
    const Document reread = readDocument(bytes, issues);
    for (const std::string& issue : issues) {
        UNSCOPED_INFO(issue);
    }
    CHECK(issues.empty());

    // The dump is the diff: a structural difference shows up as a line, not as a
    // byte offset nobody can read.
    CHECK(dump(original) == dump(reread));

    // And the bytes close: writing what was read reproduces the file.
    CHECK(writeDocument(reread) == bytes);
}

TEST_CASE("wem a v3 file says it is v3", "[wem][format][v3]") {
    const std::vector<u8> bytes = writeDocument(makeDocument());
    REQUIRE(bytes.size() >= 32);

    u32 magic = 0;
    u32 version = 0;
    std::memcpy(&magic, bytes.data(), 4);
    std::memcpy(&version, bytes.data() + 4, 4);
    CHECK(magic == kWoemMagic);
    CHECK(version == 3u);
}

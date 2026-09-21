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

TEST_CASE("wem a camera's target survives write and read", "[wem][format][nodes]") {
    Document original = makeDocument();
    NodeTree& nodes = original.models.front().nodes;
    REQUIRE_FALSE(nodes.ofKind(NodeKind::Camera).empty());
    const u32 camera = nodes.ofKind(NodeKind::Camera)[0];
    std::get<CameraPayload>(nodes.nodes[camera].payload).target = Vector3f{12, -3, 45};

    std::vector<std::string> issues;
    const Document reread = readDocument(writeDocument(original), issues);
    CHECK(issues.empty());
    const Node& node = reread.models.front().nodes.nodes[camera];
    REQUIRE(node.kind == NodeKind::Camera);
    CHECK(std::get<CameraPayload>(node.payload).target == Vector3f(12, -3, 45));
}

TEST_CASE("wem a bone gate survives write and read", "[wem][format][nodes]") {
    Document original = makeDocument();
    NodeTree& nodes = original.models.front().nodes;
    REQUIRE_FALSE(nodes.ofKind(NodeKind::Bone).empty());
    const u32 bone = nodes.ofKind(NodeKind::Bone)[0];
    std::get<BonePayload>(nodes.nodes[bone].payload).gateMesh = 1;

    std::vector<std::string> issues;
    const Document reread = readDocument(writeDocument(original), issues);
    CHECK(issues.empty());
    const Node& node = reread.models.front().nodes.nodes[bone];
    REQUIRE(node.kind == NodeKind::Bone);
    CHECK(std::get<BonePayload>(node.payload).gateMesh == 1u);
    CHECK(dump(original) == dump(reread));
}

namespace {

/// Turns a v5 file into the v4 file an older writer produced: the four bytes of
/// @p pattern -- a bone's `gateMesh`, set to it so it can be found -- are cut
/// out of the `NODE` chunk, the rest of the chunk slides back over them and its
/// tail is refilled with the alignment fill, so no offset moves; then the chunk
/// is stamped v4.
void cutGateFromNodeChunk(std::vector<u8>& bytes, u32 pattern) {
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::vector<IndexEntry> entries(header.indexCount);
    std::memcpy(entries.data(), bytes.data() + header.indexOffset,
                entries.size() * sizeof(IndexEntry));

    for (u32 i = 0; i < header.indexCount; ++i) {
        IndexEntry& entry = entries[i];
        if (entry.tag != ChunkTagTraits<Node>::value) {
            continue;
        }
        u32 end = header.indexOffset > entry.offset ? header.indexOffset : u32(bytes.size());
        for (const IndexEntry& other : entries) {
            if (other.offset > entry.offset && other.offset < end) {
                end = other.offset;
            }
        }
        u32 at = end;
        u32 found = 0;
        for (u32 p = entry.offset; p + 4 <= end; ++p) {
            u32 word = 0;
            std::memcpy(&word, bytes.data() + p, 4);
            if (word == pattern) {
                at = p;
                ++found;
            }
        }
        if (found == 0) {
            continue;
        }
        REQUIRE(found == 1u);
        std::memmove(bytes.data() + at, bytes.data() + at + 4, end - at - 4);
        std::memset(bytes.data() + end - 4, 0xAA, 4);
        entry.version = 4;
        std::memcpy(bytes.data() + header.indexOffset + i * sizeof(IndexEntry), &entry,
                    sizeof(IndexEntry));
        return;
    }
    FAIL("no NODE chunk holds the pattern");
}

/// The other half of an older `NODE`: every record's empty skin setup (v6,
/// EDIT_MODE_SKIN_DESIGN.md §13.4) cut out, so what is left is the record a
/// build before it wrote. The setup is the record's last field and an empty one
/// is a known 19 bytes -- `locked`, two empty optionals, an invalid `mirror` and
/// the null reference of an empty vector -- so it can be found and cut whole.
void cutSkinFromNodeChunk(std::vector<u8>& bytes) {
    constexpr u8 kEmptySetup[19] = {0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0,
                                    0, 0, 0, 0,    0,    0,    0,    0, 0};
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::vector<IndexEntry> entries(header.indexCount);
    std::memcpy(entries.data(), bytes.data() + header.indexOffset,
                entries.size() * sizeof(IndexEntry));

    for (const IndexEntry& entry : entries) {
        if (entry.tag != ChunkTagTraits<Node>::value) {
            continue;
        }
        u32 end = header.indexOffset > entry.offset ? header.indexOffset : u32(bytes.size());
        for (const IndexEntry& other : entries) {
            if (other.offset > entry.offset && other.offset < end) {
                end = other.offset;
            }
        }
        std::vector<u32> found;
        for (u32 at = entry.offset; at + sizeof(kEmptySetup) <= end; ++at) {
            if (std::memcmp(bytes.data() + at, kEmptySetup, sizeof(kEmptySetup)) == 0) {
                found.push_back(at);
                at += static_cast<u32>(sizeof(kEmptySetup)) - 1;
            }
        }
        REQUIRE(found.size() == entry.count);
        for (auto at = found.rbegin(); at != found.rend(); ++at) {
            std::memmove(bytes.data() + *at, bytes.data() + *at + sizeof(kEmptySetup),
                         end - *at - sizeof(kEmptySetup));
        }
        std::memset(bytes.data() + end - sizeof(kEmptySetup) * found.size(), 0xAA,
                    sizeof(kEmptySetup) * found.size());
        return;
    }
    FAIL("no NODE chunk to cut the setup from");
}

/// The third half of an older `NODE`: every default light's 3.0 terms (v7) cut
/// out -- `shadowCasting` as one byte, then the shadow range and the three
/// falloff floats. A default light's are the game's substitutes, 0.0005 and
/// 1e-5, which no other field of the fixture holds, so the run can be found
/// and cut whole.
void cutLightTermsFromNodeChunk(std::vector<u8>& bytes, u32 lights) {
    u8 terms[21] = {};
    const f32 quadratic = 0.0005f;
    const f32 damping = 0.00001f;
    std::memcpy(terms + 9, &quadratic, sizeof(f32));
    std::memcpy(terms + 17, &damping, sizeof(f32));

    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::vector<IndexEntry> entries(header.indexCount);
    std::memcpy(entries.data(), bytes.data() + header.indexOffset,
                entries.size() * sizeof(IndexEntry));

    for (const IndexEntry& entry : entries) {
        if (entry.tag != ChunkTagTraits<Node>::value) {
            continue;
        }
        u32 end = header.indexOffset > entry.offset ? header.indexOffset : u32(bytes.size());
        for (const IndexEntry& other : entries) {
            if (other.offset > entry.offset && other.offset < end) {
                end = other.offset;
            }
        }
        std::vector<u32> found;
        for (u32 at = entry.offset; at + sizeof(terms) <= end; ++at) {
            if (std::memcmp(bytes.data() + at, terms, sizeof(terms)) == 0) {
                found.push_back(at);
                at += static_cast<u32>(sizeof(terms)) - 1;
            }
        }
        REQUIRE(found.size() == lights);
        for (auto at = found.rbegin(); at != found.rend(); ++at) {
            std::memmove(bytes.data() + *at, bytes.data() + *at + sizeof(terms),
                         end - *at - sizeof(terms));
        }
        std::memset(bytes.data() + end - sizeof(terms) * found.size(), 0xAA,
                    sizeof(terms) * found.size());
        return;
    }
    FAIL("no NODE chunk to cut the light terms from");
}

} // namespace

TEST_CASE("wem a v4 bone reads its gate from the MDX bag pair", "[wem][format][nodes]") {
    // What every `.wem` written before v5 holds: the file's raw pair in the bag.
    Document original = makeDocument();
    NodeTree& nodes = original.models.front().nodes;
    const u32 gated = nodes.ofKind(NodeKind::Bone)[0];
    const u32 ungated = nodes.add(makeNode("ungated", NodeKind::Bone, 0));
    const u32 unlinked = nodes.add(makeNode("unlinked", NodeKind::Bone, 0));
    const auto seed = [&](u32 node, u32 pattern, i64 geosetId, i64 record) {
        std::get<BonePayload>(nodes.nodes[node].payload).gateMesh = pattern;
        nodes.nodes[node].native.set("geosetId", geosetId);
        nodes.nodes[node].native.set("geosetAnimationId", record);
    };
    seed(gated, 0x5EA7C0DE, 2, 7);
    seed(ungated, 0x5EA7C0DF, 3, 0xFFFFFFFF);
    seed(unlinked, 0x5EA7C0E0, 0xFFFFFFFF, 0xFFFFFFFF);

    std::vector<u8> bytes = writeDocument(original);
    // A v4 record holds neither the gate (v5), the skin setup (v6) nor a
    // light's 3.0 terms (v7).
    cutLightTermsFromNodeChunk(bytes, static_cast<u32>(nodes.ofKind(NodeKind::Light).size()));
    cutSkinFromNodeChunk(bytes);
    for (const u32 pattern : {0x5EA7C0DEu, 0x5EA7C0DFu, 0x5EA7C0E0u}) {
        cutGateFromNodeChunk(bytes, pattern);
    }

    std::vector<std::string> issues;
    const Document reread = readDocument(bytes, issues);
    CHECK(issues.empty());
    const NodeTree& read = reread.models.front().nodes;
    REQUIRE(read.size() == nodes.size());
    // Everything around the cut reads as it was written.
    for (u32 i = 0; i < read.size(); ++i) {
        CHECK(read.nodes[i].name == nodes.nodes[i].name);
        CHECK(read.nodes[i].kind == nodes.nodes[i].kind);
        CHECK(read.nodes[i].native.value("sourceIndex") == 7);
    }

    const auto gateOf = [&](u32 node) { return std::get<BonePayload>(read.nodes[node].payload).gateMesh; };
    // Trusted to `geosetId`: best effort, as the migration says.
    CHECK(gateOf(gated) == 2u);
    CHECK(read.nodes[gated].native.find("geosetId") == nullptr);
    CHECK(read.nodes[gated].native.find("geosetAnimationId") == nullptr);
    // No record, so no gate -- and the file's geoset, which the export would
    // not write back by itself, stays.
    CHECK(gateOf(ungated) == kInvalidIndex);
    REQUIRE(read.nodes[ungated].native.find("geosetId") != nullptr);
    CHECK(read.nodes[ungated].native.find("geosetId")->value == 3);
    CHECK(read.nodes[ungated].native.find("geosetAnimationId") == nullptr);
    CHECK(gateOf(unlinked) == kInvalidIndex);
    CHECK(read.nodes[unlinked].native.find("geosetId") == nullptr);
    CHECK(read.nodes[unlinked].native.find("geosetAnimationId") == nullptr);

    // And a v5 write of the migrated document keeps what the migration made.
    const Document again = readDocument(writeDocument(reread), issues);
    CHECK(dump(again) == dump(reread));
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

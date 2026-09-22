// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM §10.9 — the emitter-system node kinds.
///
/// Warcraft III's `PREM`/`PRE2`/`RIBB` and StarCraft II's `PAR_`/`PARC`/`RIB_`
/// are node kinds whose payloads are the systems whole, gated by the profiles of
/// the game that runs them. The cases are the four places that gate and those
/// payloads have to hold: the profile registry, `Validate`, each converter in
/// both directions, and the operations that edit a tree (compaction, rescale).

#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/cross/m2_wc3_emitters.h>
#include <whiteout/models/m2/structures.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

using namespace whiteout;
using namespace whiteout::models::wem;
namespace cross = whiteout::models::cross;

namespace {

constexpr u32 Sub(Sc2ParticleProperty property, u32 element = 0) {
    return EmitterPropertySub(static_cast<u32>(property), element);
}

constexpr u32 Sub(Sc2RibbonProperty property, u32 element = 0) {
    return EmitterPropertySub(static_cast<u32>(property), element);
}

Node makeNode(const std::string& name, NodeKind kind, u32 parent = kInvalidNode) {
    Node node;
    node.name = name;
    node.kind = kind;
    node.parent = parent;
    node.resetPayloadForKind();
    return node;
}

/// A document with one model, one slot and @p profile declared, and nothing
/// else to validate.
Document makeDocument(ProfileId profile) {
    Document document;
    document.declare(profile);
    document.defaultProfile = profile;
    Model model;
    model.name = "fx";
    model.materialSlots.push_back("slot");
    model.nodes.add(makeNode("root", NodeKind::Bone));
    model.nodes.nodes[0].poses.push_back(Transform{});
    model.nodes.poseSchema.push_back(PoseSchema{});
    document.models.push_back(std::move(model));
    return document;
}

bool hasError(const Diagnostics& report, DiagCode code) {
    for (const Diagnostic& entry : report.byCode(code)) {
        if (entry.severity == Severity::Error) {
            return true;
        }
    }
    return false;
}

std::vector<u8> write(const Document& document) {
    Writer writer;
    return writer.write(document);
}

Document read(const std::vector<u8>& bytes) {
    Parser parser;
    std::optional<Document> document = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(document.has_value());
    return std::move(*document);
}

const AnimChannel* channelFor(const Model& model, u32 node, u32 sub) {
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == node &&
            channel.target.channel == Channel::EmitterProperty && channel.target.sub == sub) {
            return &channel;
        }
    }
    return nullptr;
}

// ---- fixtures: an .mdx and an .m3 with each system ---------------------------

mdx::Node mdxNode(const std::string& name, u32 objectId, u32 parentId) {
    mdx::Node node;
    node.name = name;
    node.objectId = objectId;
    node.parentId = parentId;
    return node;
}

template <class T>
mdx::Track<T> mdxTrack(std::vector<u32> times, std::vector<T> values) {
    mdx::Track<T> track;
    track.isUsed = true;
    track.interpolationType = mdx::InterpolationType::Linear;
    track.keyCount = times.size();
    track.timestamps = std::move(times);
    track.keys_data = std::move(values);
    return track;
}

mdx::Model makeMdx() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "fx";

    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 1000;
    model.sequences.push_back(stand);

    mdx::Texture texture;
    texture.fileName = "textures/body.blp";
    model.textures.push_back(texture);
    mdx::Texture spark;
    spark.fileName = "textures/spark.blp";
    model.textures.push_back(spark);

    mdx::Material material;
    mdx::Layer layer;
    layer.textureId = 0;
    layer.textureAnimationId = 0xFFFFFFFF;
    material.layers.push_back(layer);
    model.materials.push_back(material);
    model.materials.push_back(material);

    mdx::Bone root;
    root.node = mdxNode("root", 0, mdx::Node::NO_PARENT);
    model.bones.push_back(root);

    mdx::ParticleEmitter prem;
    prem.node = mdxNode("prem", 1, 0);
    prem.node.flags = mdx::Node::NodeFlag::EmitterUsesMdl;
    prem.emissionRate = 3.0f;
    prem.gravity = -9.0f;
    prem.longitude = 0.25f;
    prem.latitude = 0.5f;
    prem.lifespan = 1.5f;
    prem.initialVelocity = 40.0f;
    prem.spawnModelFileName = "Abilities\\Spells\\Fire.mdl";
    prem.lifespanTracks = mdxTrack<f32>({0, 1000}, {1.5f, 2.5f});
    model.particleEmitters.push_back(prem);

    mdx::ParticleEmitter2 pre2;
    pre2.node = mdxNode("pre2", 2, 0);
    pre2.node.flags = mdx::Node::NodeFlag::Unshaded | mdx::Node::NodeFlag::LineEmitter |
                      mdx::Node::NodeFlag::XYQuad;
    pre2.speed = 120.0f;
    pre2.variation = 0.2f;
    pre2.latitude = 0.3f;
    pre2.gravity = 50.0f;
    pre2.lifespan = 0.8f;
    pre2.emissionRate = 25.0f;
    pre2.length = 10.0f;
    pre2.width = 12.0f;
    pre2.filterMode = 1;
    pre2.rows = 4;
    pre2.columns = 2;
    pre2.headOrTail = 2;
    pre2.tailLength = 1.5f;
    pre2.time = 0.4f;
    pre2.segmentColor = {Vector3f{1, 0, 0}, Vector3f{0, 1, 0}, Vector3f{0, 0, 1}};
    pre2.segmentAlpha = {255, 128, 0};
    pre2.segmentScaling = {5.0f, 10.0f, 20.0f};
    pre2.headInterval = {0, 3, 1};
    pre2.headDecayInterval = {4, 7, 2};
    pre2.tailInterval = {1, 2, 3};
    pre2.tailDecayInterval = {5, 6, 4};
    pre2.textureId = 1;
    pre2.squirt = 1;
    pre2.priorityPlane = -2;
    pre2.replaceableId = 0;
    pre2.speedTracks = mdxTrack<f32>({0, 500, 1000}, {120.0f, 60.0f, 0.0f});
    pre2.widthTracks = mdxTrack<f32>({0, 1000}, {12.0f, 24.0f});
    model.particleEmitters2.push_back(pre2);

    mdx::RibbonEmitter ribb;
    ribb.node = mdxNode("ribb", 3, 0);
    ribb.heightAbove = 15.0f;
    ribb.heightBelow = 5.0f;
    ribb.alpha = 0.75f;
    ribb.color = Vector3f{0.5f, 0.25f, 1.0f};
    ribb.lifespan = 0.6f;
    ribb.textureSlot = 2;
    ribb.emissionRate = 30;
    ribb.rows = 2;
    ribb.columns = 3;
    ribb.materialId = 1;
    ribb.gravity = 7.0f;
    ribb.heightAboveTracks = mdxTrack<f32>({0, 1000}, {15.0f, 30.0f});
    model.ribbonEmitters.push_back(ribb);

    model.pivotPoints = {Vector3f{0, 0, 0}, Vector3f{0, 0, 10}, Vector3f{0, 5, 0},
                         Vector3f{1, 2, 3}};
    return model;
}

/// A v1800 model with a PRE2, a PopcornFX emitter and a ribbon -- CORN between
/// the two, where Reforged numbers it -- and every CORN field and track set to
/// something its default is not. The colour keys are blue first, as the file
/// stores them.
mdx::Model makeReforgedMdx() {
    mdx::Model model = makeMdx();
    model.version = 1800;
    model.particleEmitters.clear();
    model.particleEmitters2[0].node.objectId = 1;
    model.ribbonEmitters[0].node.objectId = 3;

    mdx::CornEmitter corn;
    corn.node = mdxNode("corn", 2, 0);
    // Unfogged without scaling, so reading either bit as the other shows.
    corn.node.flags = mdx::Node::NodeFlag::ParticleEmitter | mdx::Node::NodeFlag::Unshaded |
                      mdx::Node::NodeFlag::PopcornUnfogged;
    corn.lifeSpan = 1.5f;
    corn.emissionRate = 0.5f;
    corn.speed = 2.0f;
    corn.color = Vector3f{1.0f, 0.5f, 0.25f};
    corn.alpha = 0.75f;
    corn.replaceableId = 1;
    corn.path = "Abilities\\Spells\\Fire\\fire.pkb";
    corn.animVisibilityGuide = "Stand";
    corn.lifeSpanTracks = mdxTrack<f32>({0, 1000}, {1.5f, 3.0f});
    corn.emissionRateTracks = mdxTrack<f32>({0, 500}, {0.5f, 0.0f});
    corn.speedTracks = mdxTrack<f32>({0, 1000}, {2.0f, 1.0f});
    corn.colorTracks = mdxTrack<Vector3f>({0, 1000}, {Vector3f{0.25f, 0.5f, 1.0f},
                                                      Vector3f{0.0f, 0.0f, 1.0f}});
    corn.alphaTracks = mdxTrack<f32>({0, 1000}, {0.75f, 0.0f});
    corn.visibilityTracks = mdxTrack<f32>({0, 800}, {1.0f, 0.0f});
    corn.visibilityTracks.interpolationType = mdx::InterpolationType::None;
    model.cornEmitters.push_back(corn);

    model.pivotPoints = {Vector3f{0, 0, 0}, Vector3f{0, 5, 0}, Vector3f{0, 0, 20},
                         Vector3f{1, 2, 3}};
    return model;
}

template <class T>
m3::AnimRef<T> ref(u32 animId, const T& initValue, const T& nullValue = T{}) {
    m3::AnimRef<T> out;
    out.animId = animId;
    out.initValue = initValue;
    out.nullValue = nullValue;
    return out;
}

constexpr u32 Ref(u32 slot, u32 block) {
    return (slot << 16) | block;
}

constexpr u32 kRateId = 0x7001;
constexpr u32 kSquirtId = 0x7002;
constexpr u32 kActiveId = 0x7003;
constexpr u32 kCopyRateId = 0x7004;
constexpr u32 kUnkeyedId = 0x7005;
constexpr u32 kSplineId = 0x7006;

/// Two bones, two `PAR_` naming each other, one `PARC`, and a `RIB_` with an
/// `SRIB` riding the root -- every link the payloads carry, and one key in each
/// of the four streams an emitter property is written in.
m3::Model makeM3() {
    m3::Model model;
    model.name = "fx.m3";
    model.setVersion(29);

    m3::Sequence sequence;
    sequence.name = "Stand";
    sequence.startFrame = 0;
    sequence.endFrame = 1000;
    model.sequences.push_back(sequence);
    m3::AnimationGroup group;
    group.name = "Stand";
    group.subtrackIndices = {0};
    model.animationGroups.push_back(group);

    m3::SubTrackContainer stc;
    stc.name = "Stand";
    m3::AnimBlock<f32> rate;
    rate.timestamps = {0, 1000};
    rate.keys = {10.0f, 40.0f};
    stc.sdr3.push_back(rate);
    stc.animIds.push_back(kRateId);
    stc.animRefs.push_back(Ref(5, 0));
    m3::AnimBlock<f32> copyRate;
    copyRate.timestamps = {0, 500};
    copyRate.keys = {1.0f, 2.0f};
    stc.sdr3.push_back(copyRate);
    stc.animIds.push_back(kCopyRateId);
    stc.animRefs.push_back(Ref(5, 1));
    m3::AnimBlock<i16> squirt;
    squirt.timestamps = {0, 200};
    squirt.keys = {0, 12};
    stc.sds6.push_back(squirt);
    stc.animIds.push_back(kSquirtId);
    stc.animRefs.push_back(Ref(7, 0));
    m3::AnimBlock<m3::Flag> active;
    active.timestamps = {0, 800};
    active.keys = {m3::Flag{1}, m3::Flag{0}};
    stc.sdfg.push_back(active);
    stc.animIds.push_back(kActiveId);
    stc.animRefs.push_back(Ref(11, 0));
    m3::AnimBlock<Vector3f> spline;
    spline.timestamps = {0, 1000};
    spline.keys = {Vector3f{0, 0, 0}, Vector3f{0, 0, 9}};
    stc.sd3v.push_back(spline);
    stc.animIds.push_back(kSplineId);
    stc.animRefs.push_back(Ref(2, 0));
    model.subTrackCollections.push_back(stc);

    for (int b = 0; b < 2; ++b) {
        m3::Bone bone;
        bone.name = b == 0 ? "Root" : "Hand";
        bone.parentIndex = b == 0 ? 0xFFFFu : 0u;
        bone.rotation.initValue = Quaternion{0, 0, 0, 1};
        bone.scale.initValue = Vector3f{1, 1, 1};
        bone.visibility.initValue = 1u;
        model.bones.push_back(bone);
    }

    m3::MaterialMap map;
    map.materialType = m3::MaterialType::Standard;
    map.materialIndex = 0;
    model.materialMaps.push_back(map);
    m3::StandardMaterial material;
    material.name = "spark";
    model.standardMaterials.push_back(material);

    m3::ParticleEmitter first;
    first.boneIndex = 1;
    first.materialIndex = 0;
    first.emissionRate = ref(kRateId, 10.0f);
    first.squirtAmount = ref<u16>(kSquirtId, 0);
    first.initialSpeed = ref(kUnkeyedId, 3.5f, 0.0f);
    first.colorStart = ref(0, m3::ColorBGRA{10, 20, 30, 255});
    first.sizeAnimation = ref(0, Vector3f{1, 2, 3});
    first.uvTiling = ref(0, Vector2f{2, 2}, Vector2f{1, 1});
    first.lifetime = ref(0, 2.0f);
    first.pitchType = 3;
    first.pitchAmplitude = ref(0, 0.5f);
    first.emitterShape = m3::EmitterShape::Spline;
    first.splineLineData.push_back(ref(0, Vector3f{1, 0, 0}));
    first.splineLineData.push_back(ref(kSplineId, Vector3f{0, 0, 0}));
    first.shapeRegions = {0};
    first.instanceType = m3::ParticleInstanceType::Tail;
    first.flags = m3::ParticleFlag::Sort | m3::ParticleFlag::LitParts;
    first.additionalFlags = m3::ParticleAdditionalFlag::WorldSpace;
    first.trailLinkIndex = 1;
    first.trailChance = 0.5f;
    first.ribbonLinkIndex = 0;
    first.modelPaths = {std::string("Assets\\Rock.m3")};
    first.maxParticles = 64;
    first.gravity = -1.5f;
    first.killRadius = 30.0f;
    first.copyIndices = {0};
    model.particleEmitters.push_back(first);

    m3::ParticleEmitter second;
    second.boneIndex = 0;
    second.materialIndex = 0;
    second.collisionSpawnIndex = 0;
    second.emissionRate = ref(0, 5.0f);
    model.particleEmitters.push_back(second);

    m3::ParticleEmitterCopy copy;
    copy.boneIndex = 0;
    copy.emissionRate = ref(kCopyRateId, 1.0f);
    copy.squirtAmount = ref<u16>(0, 4);
    model.particleEmitterCopies.push_back(copy);

    m3::RibbonEmitter ribbon;
    ribbon.boneIndex = 1;
    ribbon.materialIndex = 0;
    ribbon.active = ref<u32>(kActiveId, 1u, 0u);
    ribbon.lifetime = ref(0, 0.7f);
    ribbon.ribbonType = m3::RibbonType::Cylinder;
    ribbon.edges = 6;
    ribbon.divisions = 20.0f;
    ribbon.yawType = 2;
    ribbon.yawAmplitude = ref(0, 0.1f);
    m3::SplineRibbon point;
    point.boneIndex = 0;
    point.emissionOffset = Vector3f{0, 1, 0};
    point.velocity = ref(0, 4.0f);
    point.yawType = 1;
    point.yawAmplitude = ref(0, 0.25f);
    ribbon.splineRibbons.push_back(point);
    model.ribbonEmitters.push_back(ribbon);
    return model;
}

Document fromM3(const m3::Model& model) {
    M3Converter converter;
    Result<Document> result = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(result.ok());
    return std::move(*result.value);
}

Document fromMdx(const mdx::Model& model) {
    MdxConverter converter;
    Result<Document> result = converter.fromMdx(model);
    REQUIRE(result.ok());
    return std::move(*result.value);
}

u32 nodeNamed(const Model& model, const std::string& name) {
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        if (model.nodes.nodes[n].name == name) {
            return n;
        }
    }
    return kInvalidNode;
}

/// The key stream @p animId plays in, as `(slot << 16) | block`.
u32 streamOf(const m3::Model& model, u32 animId) {
    for (const m3::SubTrackContainer& stc : model.subTrackCollections) {
        for (std::size_t k = 0; k < stc.animIds.size(); ++k) {
            if (stc.animIds[k] == animId) {
                return stc.animRefs[k];
            }
        }
    }
    return kInvalidIndex;
}

} // namespace

// ============================================================================
// The gate
// ============================================================================

TEST_CASE("wem emitter systems are gated by the profile registry", "[wem][node][emitter]") {
    for (u32 p = 0; p < static_cast<u32>(ProfileId::Count); ++p) {
        const ProfileId profile = static_cast<ProfileId>(p);
        const bool warcraft = profile == ProfileId::Wc3Classic || profile == ProfileId::Wc3Reforged;
        const bool starcraft = profile == ProfileId::Sc2 || profile == ProfileId::Heroes;
        const bool wow = profile == ProfileId::Wow;
        INFO(ToString(profile));
        for (u32 k = 0; k < static_cast<u32>(NodeKind::Count); ++k) {
            const NodeKind kind = static_cast<NodeKind>(k);
            INFO(ToString(kind));
            const bool expected = HasNodeKind(kSharedNodeKinds, kind) ||
                                  (warcraft && HasNodeKind(kWc3NodeKinds, kind)) ||
                                  (starcraft && HasNodeKind(kSc2NodeKinds, kind)) ||
                                  (wow && HasNodeKind(kWowNodeKinds, kind));
            CHECK(CarriesNodeKind(profile, kind) == expected);
        }
    }
    CHECK(ProfilesCarryingNodeKind(NodeKind::Wc3ParticleEmitter2) ==
          (ProfileBit(ProfileId::Wc3Classic) | ProfileBit(ProfileId::Wc3Reforged)));
    CHECK(ProfilesCarryingNodeKind(NodeKind::Sc2RibbonEmitter) ==
          (ProfileBit(ProfileId::Sc2) | ProfileBit(ProfileId::Heroes)));
    // Generic has no exporter and no game: it carries the shared kinds alone.
    CHECK_FALSE(CarriesNodeKind(ProfileId::Generic, NodeKind::Sc2ParticleEmitter));
    CHECK(IsEmitterSystemKind(NodeKind::Wc3RibbonEmitter));
    CHECK_FALSE(IsEmitterSystemKind(NodeKind::RibbonEmitter));
    CHECK(IsRibbonEmitterKind(NodeKind::Sc2RibbonEmitter));
    CHECK(IsParticleEmitterKind(NodeKind::Wc3ParticleEmitter1));
    CHECK(ProfilesCarryingNodeKind(NodeKind::M2ParticleEmitter) == ProfileBit(ProfileId::Wow));
    CHECK(IsEmitterSystemKind(NodeKind::M2ParticleEmitter));
    CHECK(IsParticleEmitterKind(NodeKind::M2ParticleEmitter));
}

TEST_CASE("wem every emitter system declares a closed property table", "[wem][node][emitter]") {
    CHECK(EmitterPropertyCount(NodeKind::Wc3ParticleEmitter1) ==
          static_cast<u32>(Wc3Particle1Property::Count));
    CHECK(EmitterPropertyCount(NodeKind::Wc3ParticleEmitter2) ==
          static_cast<u32>(Wc3Particle2Property::Count));
    CHECK(EmitterPropertyCount(NodeKind::Wc3RibbonEmitter) ==
          static_cast<u32>(Wc3RibbonProperty::Count));
    CHECK(EmitterPropertyCount(NodeKind::Wc3CornEmitter) ==
          static_cast<u32>(Wc3CornProperty::Count));
    CHECK(EmitterPropertyCount(NodeKind::Sc2ParticleEmitter) ==
          static_cast<u32>(Sc2ParticleProperty::Count));
    CHECK(EmitterPropertyCount(NodeKind::Sc2RibbonEmitter) ==
          static_cast<u32>(Sc2RibbonProperty::Count));
    CHECK(EmitterPropertyCount(NodeKind::M2ParticleEmitter) ==
          static_cast<u32>(M2ParticleProperty::Count));
    // WoW's gravity keys a vector, and a rescale restates it.
    const EmitterPropertyDesc* gravity = FindEmitterProperty(
        NodeKind::M2ParticleEmitter, EmitterPropertySub(static_cast<u32>(M2ParticleProperty::Gravity)));
    REQUIRE(gravity != nullptr);
    CHECK(gravity->type == geom::AttrType::F32x3);
    CHECK(gravity->length);
    CHECK(EmitterPropertyCount(NodeKind::Bone) == 0u);
    CHECK(EmitterPropertyCount(NodeKind::ParticleEmitter) == 0u);

    const EmitterPropertyDesc* squirt =
        FindEmitterProperty(NodeKind::Sc2ParticleEmitter, Sub(Sc2ParticleProperty::SquirtAmount));
    REQUIRE(squirt != nullptr);
    CHECK(std::string(squirt->name) == "squirtAmount");
    CHECK(squirt->type == geom::AttrType::U32);
    const EmitterPropertyDesc* color =
        FindEmitterProperty(NodeKind::Sc2RibbonEmitter, Sub(Sc2RibbonProperty::ColorMid));
    REQUIRE(color != nullptr);
    CHECK(color->type == geom::AttrType::F32x4);
    // A CORN's properties multiply its effect's own, so none is a length.
    for (u32 p = 0; p < static_cast<u32>(Wc3CornProperty::Count); ++p) {
        const EmitterPropertyDesc* multiplier =
            FindEmitterProperty(NodeKind::Wc3CornEmitter, EmitterPropertySub(p));
        REQUIRE(multiplier != nullptr);
        CHECK(multiplier->type == geom::AttrType::F32);
        CHECK_FALSE(multiplier->length);
    }

    // An element is only a per-element property's to name.
    CHECK(FindEmitterProperty(NodeKind::Sc2ParticleEmitter,
                              Sub(Sc2ParticleProperty::SplinePoint, 7)) != nullptr);
    CHECK(FindEmitterProperty(NodeKind::Sc2ParticleEmitter,
                              Sub(Sc2ParticleProperty::EmissionRate, 1)) == nullptr);
    CHECK(FindEmitterProperty(NodeKind::Sc2ParticleEmitter,
                              static_cast<u32>(Sc2ParticleProperty::Count)) == nullptr);
    CHECK(FindEmitterProperty(NodeKind::Bone, 0) == nullptr);
}

// ============================================================================
// Validate
// ============================================================================

TEST_CASE("wem a system no declared profile carries is structural", "[wem][node][emitter]") {
    Document document = makeDocument(ProfileId::Sc2);
    document.models[0].nodes.add(makeNode("pre2", NodeKind::Wc3ParticleEmitter2, 0));
    CHECK(hasError(Validate(document, ValidateLevel::Structural), DiagCode::NodeKindNotCarried));

    // Declaring a game that runs it is what makes the node legal.
    document.declare(ProfileId::Wc3Classic);
    CHECK_FALSE(
        hasError(Validate(document, ValidateLevel::Structural), DiagCode::NodeKindNotCarried));
}

TEST_CASE("wem an emitter property channel answers to its node's table", "[wem][node][emitter]") {
    Document document = makeDocument(ProfileId::Sc2);
    Model& model = document.models[0];
    model.nodes.add(makeNode("par", NodeKind::Sc2ParticleEmitter, 0));

    AnimChannel good;
    good.id = 1;
    good.target.node = 1;
    good.target.channel = Channel::EmitterProperty;
    good.target.sub = Sub(Sc2ParticleProperty::ColorStart);
    good.valueType = geom::AttrType::F32x4;
    model.animChannels.add(good);
    CHECK_FALSE(hasError(Validate(document, ValidateLevel::Structural), DiagCode::IndexOutOfRange));
    CHECK_FALSE(
        hasError(Validate(document, ValidateLevel::Structural), DiagCode::AttributeCountMismatch));

    SECTION("a property keyed as the wrong type") {
        model.animChannels.channels[0].valueType = geom::AttrType::F32;
        CHECK(hasError(Validate(document, ValidateLevel::Structural),
                       DiagCode::AttributeCountMismatch));
    }
    SECTION("a property on a node that has no such table") {
        model.animChannels.channels[0].target.node = 0;
        CHECK(hasError(Validate(document, ValidateLevel::Structural), DiagCode::IndexOutOfRange));
    }
    SECTION("a property past the kind's enum") {
        model.animChannels.channels[0].target.sub =
            static_cast<u32>(Sc2ParticleProperty::Count) + 3;
        CHECK(hasError(Validate(document, ValidateLevel::Structural), DiagCode::IndexOutOfRange));
    }
}

TEST_CASE("wem an emitter's links name nodes of the kind they need", "[wem][node][emitter]") {
    Document document = makeDocument(ProfileId::Sc2);
    Model& model = document.models[0];
    model.nodes.add(makeNode("par", NodeKind::Sc2ParticleEmitter, 0));
    model.nodes.add(makeNode("rib", NodeKind::Sc2RibbonEmitter, 0));
    model.nodes.add(makeNode("copy", NodeKind::Sc2ParticleEmitter, 0));
    auto& copy = std::get<Sc2ParticleEmitterPayload>(model.nodes.nodes[3].payload);
    copy.copyOf = 1;
    auto& source = std::get<Sc2ParticleEmitterPayload>(model.nodes.nodes[1].payload);
    source.ribbonLink = 2;
    source.materialSlot = 0;
    CHECK_FALSE(
        hasError(Validate(document, ValidateLevel::Structural), DiagCode::DanglingNodeReference));

    SECTION("a copy of a ribbon") {
        copy.copyOf = 2;
        CHECK(hasError(Validate(document, ValidateLevel::Structural),
                       DiagCode::DanglingNodeReference));
    }
    SECTION("a copy of a copy") {
        source.copyOf = 3;
        CHECK(hasError(Validate(document, ValidateLevel::Structural),
                       DiagCode::DanglingNodeReference));
    }
    SECTION("a bounce that spawns a particle emitter") {
        source.ribbonLink = 3;
        CHECK(hasError(Validate(document, ValidateLevel::Structural),
                       DiagCode::DanglingNodeReference));
    }
    SECTION("a link past the tree") {
        source.trailLink = 40;
        CHECK(hasError(Validate(document, ValidateLevel::Structural),
                       DiagCode::DanglingNodeReference));
    }
    SECTION("a material slot past the model's") {
        source.materialSlot = 5;
        CHECK(hasError(Validate(document, ValidateLevel::Structural), DiagCode::IndexOutOfRange));
    }
}

// ============================================================================
// Serialization
// ============================================================================

TEST_CASE("wem every emitter payload survives the binary round trip", "[wem][node][emitter]") {
    Document document = makeDocument(ProfileId::Sc2);
    document.declare(ProfileId::Wc3Classic);
    Model& model = document.models[0];

    Node pre2 = makeNode("pre2", NodeKind::Wc3ParticleEmitter2, 0);
    auto& p2 = std::get<Wc3ParticleEmitter2Payload>(pre2.payload);
    p2.speed = 3.0f;
    p2.middle.color = Vector3f{0.1f, 0.2f, 0.3f};
    p2.tailDecay.repeat = 9;
    p2.xyQuad = true;
    model.nodes.add(std::move(pre2));

    Node prem = makeNode("prem", NodeKind::Wc3ParticleEmitter1, 0);
    std::get<Wc3ParticleEmitter1Payload>(prem.payload).spawnModel.path = "a.mdl";
    model.nodes.add(std::move(prem));

    Node ribb = makeNode("ribb", NodeKind::Wc3RibbonEmitter, 0);
    std::get<Wc3RibbonEmitterPayload>(ribb.payload).heightBelow = 4.0f;
    model.nodes.add(std::move(ribb));

    Node par = makeNode("par", NodeKind::Sc2ParticleEmitter, 0);
    auto& sp = std::get<Sc2ParticleEmitterPayload>(par.payload);
    sp.emissionRate.nullValue = 2.0f;
    sp.colorEnd.initValue = Vector4f{1, 0.5f, 0.25f, 1};
    sp.splinePoints.resize(2);
    sp.splinePoints[1].initValue = Vector3f{4, 5, 6};
    sp.models.push_back(AssetKey{AssetKey::kNoId, AssetKey::kNoId, "rock.m3"});
    sp.shapeSections = {2, 3};
    sp.horizontal.type = 7;
    model.nodes.add(std::move(par));

    Node copy = makeNode("copy", NodeKind::Sc2ParticleEmitter, 0);
    std::get<Sc2ParticleEmitterPayload>(copy.payload).copyOf = 4;
    model.nodes.add(std::move(copy));

    Node rib = makeNode("rib", NodeKind::Sc2RibbonEmitter, 0);
    auto& sr = std::get<Sc2RibbonEmitterPayload>(rib.payload);
    sr.active.initValue = 1;
    sr.splinePoints.resize(1);
    sr.splinePoints[0].node = 0;
    sr.splinePoints[0].velocityVariation.frequency.initValue = 8.0f;
    model.nodes.add(std::move(rib));

    document.declare(ProfileId::Wow);
    Node m2 = makeNode("m2", NodeKind::M2ParticleEmitter, 0);
    auto& mp = std::get<M2ParticleEmitterPayload>(m2.payload);
    mp.flags = 0x60830431u;
    mp.gravity = Vector3f{0, 0.5f, -2};
    mp.alphaTimes = {0, 0.175f, 1};
    mp.alphas = {0, 0.787f, 0};
    mp.scales = {Vector2f{0.4f, 0.4f}};
    mp.headCells = {3};
    mp.splinePoints = {Vector3f{1, 2, 3}};
    model.nodes.add(std::move(m2));

    Node corn = makeNode("corn", NodeKind::Wc3CornEmitter, 0);
    auto& wc = std::get<Wc3CornEmitterPayload>(corn.payload);
    wc.emissionRate = 0.25f;
    wc.color = Vector3f{0.5f, 0.25f, 1.0f};
    wc.effect.path = "fx.pkb";
    wc.animVisibilityGuide = "Birth";
    wc.popcornScaling = true;
    model.nodes.add(std::move(corn));

    const std::vector<u8> bytes = write(document);
    const Document back = read(bytes);
    // A field the reader skipped would be written back as its default, so the
    // second write matching the first byte for byte is every field read.
    CHECK(write(back) == bytes);

    const NodeTree& tree = back.models[0].nodes;
    REQUIRE(tree.size() == model.nodes.size());
    for (u32 n = 0; n < tree.size(); ++n) {
        CHECK(tree.nodes[n].kind == model.nodes.nodes[n].kind);
        CHECK(tree.nodes[n].payloadMatchesKind());
    }
    const auto& rp2 = std::get<Wc3ParticleEmitter2Payload>(tree.nodes[1].payload);
    CHECK(rp2.middle.color.y == 0.2f);
    CHECK(rp2.tailDecay.repeat == 9u);
    CHECK(rp2.xyQuad);
    const auto& rsp = std::get<Sc2ParticleEmitterPayload>(tree.nodes[4].payload);
    CHECK(rsp.emissionRate.nullValue == 2.0f);
    REQUIRE(rsp.splinePoints.size() == 2u);
    CHECK(rsp.splinePoints[1].initValue.z == 6.0f);
    REQUIRE(rsp.models.size() == 1u);
    CHECK(rsp.models[0].path == "rock.m3");
    CHECK(rsp.shapeSections == std::vector<u32>{2, 3});
    CHECK(std::get<Sc2ParticleEmitterPayload>(tree.nodes[5].payload).copyOf == 4u);
    const auto& rsr = std::get<Sc2RibbonEmitterPayload>(tree.nodes[6].payload);
    REQUIRE(rsr.splinePoints.size() == 1u);
    CHECK(rsr.splinePoints[0].velocityVariation.frequency.initValue == 8.0f);
    const auto& rm2 = std::get<M2ParticleEmitterPayload>(tree.nodes[7].payload);
    CHECK(rm2.flags == 0x60830431u);
    CHECK(rm2.gravity.y == 0.5f);
    CHECK(rm2.alphas == std::vector<f32>{0, 0.787f, 0});
    REQUIRE(rm2.scales.size() == 1u);
    CHECK(rm2.scales[0].x == 0.4f);
    CHECK(rm2.headCells == std::vector<u32>{3});
    REQUIRE(rm2.splinePoints.size() == 1u);
    CHECK(rm2.splinePoints[0].z == 3.0f);
    const auto& rwc = std::get<Wc3CornEmitterPayload>(tree.nodes[8].payload);
    CHECK(rwc.emissionRate == 0.25f);
    CHECK(rwc.color.z == 1.0f);
    CHECK(rwc.effect.path == "fx.pkb");
    CHECK(rwc.animVisibilityGuide == "Birth");
    CHECK(rwc.popcornScaling);
    CHECK_FALSE(rwc.unfogged);
}

// ============================================================================
// Warcraft III
// ============================================================================

TEST_CASE("wem mdx carries all three Warcraft III systems both ways", "[wem][node][emitter]") {
    const mdx::Model source = makeMdx();
    const Document document = fromMdx(source);
    const Model& model = document.models[0];
    CHECK_FALSE(Validate(document, ValidateLevel::Profile).hasErrors());

    const u32 premNode = nodeNamed(model, "prem");
    const u32 pre2Node = nodeNamed(model, "pre2");
    const u32 ribbNode = nodeNamed(model, "ribb");
    REQUIRE(premNode != kInvalidNode);
    REQUIRE(pre2Node != kInvalidNode);
    REQUIRE(ribbNode != kInvalidNode);
    CHECK(model.nodes.nodes[premNode].kind == NodeKind::Wc3ParticleEmitter1);
    CHECK(model.nodes.nodes[pre2Node].kind == NodeKind::Wc3ParticleEmitter2);
    CHECK(model.nodes.nodes[ribbNode].kind == NodeKind::Wc3RibbonEmitter);

    const auto& p1 = std::get<Wc3ParticleEmitter1Payload>(model.nodes.nodes[premNode].payload);
    CHECK(p1.speed == 40.0f);
    CHECK(p1.spawnModel.path == "Abilities\\Spells\\Fire.mdl");
    CHECK(p1.usesMdl);
    CHECK_FALSE(p1.usesTga);
    const auto& p2 = std::get<Wc3ParticleEmitter2Payload>(model.nodes.nodes[pre2Node].payload);
    CHECK(p2.texture == 1u);
    CHECK(p2.filter == Wc3ParticleFilter::Additive);
    CHECK(p2.headOrTail == Wc3ParticleHeadOrTail::Both);
    CHECK(p2.middle.alpha == 128);
    CHECK(p2.tailLife.repeat == 3u);
    CHECK(p2.unshaded);
    CHECK(p2.lineEmitter);
    CHECK(p2.xyQuad);
    CHECK_FALSE(p2.unfogged);
    const auto& r = std::get<Wc3RibbonEmitterPayload>(model.nodes.nodes[ribbNode].payload);
    CHECK(r.materialSlot == 1u);
    CHECK(r.color.z == 1.0f);

    // Each keyed property is a channel of its own kind's table.
    CHECK(channelFor(model, pre2Node, EmitterPropertySub(static_cast<u32>(
                                          Wc3Particle2Property::Speed))) != nullptr);
    CHECK(channelFor(model, pre2Node, EmitterPropertySub(static_cast<u32>(
                                          Wc3Particle2Property::Width))) != nullptr);
    CHECK(channelFor(model, premNode, EmitterPropertySub(static_cast<u32>(
                                          Wc3Particle1Property::Lifespan))) != nullptr);
    CHECK(channelFor(model, ribbNode, EmitterPropertySub(static_cast<u32>(
                                          Wc3RibbonProperty::HeightAbove))) != nullptr);

    MdxConverter converter;
    Result<mdx::Model> back = converter.toMdx(document, ProfileId::Wc3Classic, 800);
    REQUIRE(back.ok());
    REQUIRE(back->particleEmitters.size() == 1u);
    REQUIRE(back->particleEmitters2.size() == 1u);
    REQUIRE(back->ribbonEmitters.size() == 1u);

    const mdx::ParticleEmitter& e1 = back->particleEmitters[0];
    const mdx::ParticleEmitter& s1 = source.particleEmitters[0];
    CHECK(e1.emissionRate == s1.emissionRate);
    CHECK(e1.gravity == s1.gravity);
    CHECK(e1.longitude == s1.longitude);
    CHECK(e1.latitude == s1.latitude);
    CHECK(e1.lifespan == s1.lifespan);
    CHECK(e1.initialVelocity == s1.initialVelocity);
    CHECK(e1.spawnModelFileName == s1.spawnModelFileName);
    CHECK(static_cast<u32>(e1.node.flags) == static_cast<u32>(s1.node.flags));
    CHECK(e1.lifespanTracks.timestamps == s1.lifespanTracks.timestamps);
    CHECK(e1.lifespanTracks.keys_data == s1.lifespanTracks.keys_data);

    const mdx::ParticleEmitter2& e2 = back->particleEmitters2[0];
    const mdx::ParticleEmitter2& s2 = source.particleEmitters2[0];
    CHECK(e2.speed == s2.speed);
    CHECK(e2.variation == s2.variation);
    CHECK(e2.width == s2.width);
    CHECK(e2.length == s2.length);
    CHECK(e2.filterMode == s2.filterMode);
    CHECK(e2.rows == s2.rows);
    CHECK(e2.columns == s2.columns);
    CHECK(e2.headOrTail == s2.headOrTail);
    CHECK(e2.tailLength == s2.tailLength);
    CHECK(e2.time == s2.time);
    CHECK(e2.segmentColor[2].z == s2.segmentColor[2].z);
    CHECK(e2.segmentAlpha == s2.segmentAlpha);
    CHECK(e2.segmentScaling == s2.segmentScaling);
    CHECK(e2.headInterval == s2.headInterval);
    CHECK(e2.headDecayInterval == s2.headDecayInterval);
    CHECK(e2.tailInterval == s2.tailInterval);
    CHECK(e2.tailDecayInterval == s2.tailDecayInterval);
    CHECK(e2.textureId == s2.textureId);
    CHECK(e2.squirt == s2.squirt);
    CHECK(e2.priorityPlane == s2.priorityPlane);
    CHECK(static_cast<u32>(e2.node.flags) == static_cast<u32>(s2.node.flags));
    CHECK(e2.speedTracks.timestamps == s2.speedTracks.timestamps);
    CHECK(e2.speedTracks.keys_data == s2.speedTracks.keys_data);
    CHECK(e2.widthTracks.keys_data == s2.widthTracks.keys_data);
    CHECK_FALSE(e2.lengthTracks.isUsed);

    const mdx::RibbonEmitter& er = back->ribbonEmitters[0];
    const mdx::RibbonEmitter& sr = source.ribbonEmitters[0];
    CHECK(er.heightAbove == sr.heightAbove);
    CHECK(er.heightBelow == sr.heightBelow);
    CHECK(er.alpha == sr.alpha);
    CHECK(er.color.y == sr.color.y);
    CHECK(er.lifespan == sr.lifespan);
    CHECK(er.textureSlot == sr.textureSlot);
    CHECK(er.emissionRate == sr.emissionRate);
    CHECK(er.rows == sr.rows);
    CHECK(er.columns == sr.columns);
    CHECK(er.materialId == sr.materialId);
    CHECK(er.gravity == sr.gravity);
    CHECK(er.heightAboveTracks.keys_data == sr.heightAboveTracks.keys_data);

    // MDX numbers nodes by chunk -- PREM before PRE2 before RIBB, the source's
    // own order.
    CHECK(e1.node.objectId < e2.node.objectId);
    CHECK(e2.node.objectId < er.node.objectId);
}

TEST_CASE("wem mdx carries a PopcornFX emitter both ways", "[wem][node][emitter]") {
    // CORN imported as a generic reference holding only its path, and exported
    // as an empty PRE2: every Reforged effect an edit rebuilt was gone (1,374
    // of 1,382 shipped v1800 files lost every one).
    const mdx::Model source = makeReforgedMdx();
    const Document document = fromMdx(source);
    const Model& model = document.models[0];
    CHECK_FALSE(Validate(document, ValidateLevel::Profile).hasErrors());

    const u32 cornNode = nodeNamed(model, "corn");
    REQUIRE(cornNode != kInvalidNode);
    CHECK(model.nodes.nodes[cornNode].kind == NodeKind::Wc3CornEmitter);
    const auto& payload = std::get<Wc3CornEmitterPayload>(model.nodes.nodes[cornNode].payload);
    CHECK(payload.lifespan == 1.5f);
    CHECK(payload.emissionRate == 0.5f);
    CHECK(payload.speed == 2.0f);
    CHECK(payload.color.x == 1.0f);
    CHECK(payload.color.z == 0.25f);
    CHECK(payload.alpha == 0.75f);
    CHECK(payload.replaceableId == 1u);
    CHECK(payload.effect.path == source.cornEmitters[0].path);
    CHECK(payload.animVisibilityGuide == "Stand");
    CHECK(payload.unshaded);
    CHECK_FALSE(payload.sortPrimsFarZ);
    // 0x20000 and 0x40000 are CORN's own, not PRE2's `LineEmitter`/`Unfogged`.
    CHECK(payload.unfogged);
    CHECK_FALSE(payload.popcornScaling);

    // The three multipliers are its own properties; colour, alpha and
    // visibility the shared channels, the colour RGB like every other.
    for (const Wc3CornProperty property :
         {Wc3CornProperty::Lifespan, Wc3CornProperty::EmissionRate, Wc3CornProperty::Speed}) {
        CHECK(channelFor(model, cornNode, EmitterPropertySub(static_cast<u32>(property))) !=
              nullptr);
    }
    const AnimChannel* colour = nullptr;
    u32 shared = 0;
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.node != cornNode) {
            continue;
        }
        if (channel.target.channel == Channel::Color) {
            colour = &channel;
        }
        shared += channel.target.channel == Channel::Color ||
                          channel.target.channel == Channel::Alpha ||
                          channel.target.channel == Channel::Visibility
                      ? 1
                      : 0;
    }
    CHECK(shared == 3u);
    REQUIRE(colour != nullptr);
    const SubTrack* colourKeys = nullptr;
    for (const SubTrackContainer& container : document.clips[0].containers) {
        colourKeys = colourKeys != nullptr ? colourKeys : container.find(colour->id);
    }
    REQUIRE(colourKeys != nullptr);
    Vector3f first{};
    std::memcpy(&first, colourKeys->values.data(), sizeof(Vector3f));
    CHECK(first.x == 1.0f); // red first once it is WEM's
    CHECK(first.z == 0.25f);

    // Through the file and back out, record for record.
    const Document reread = read(write(document));
    MdxConverter converter;
    Result<mdx::Model> back = converter.toMdx(reread, ProfileId::Wc3Reforged, 1800);
    REQUIRE(back.ok());
    CHECK(back->particleEmitters2.size() == 1u); // no CORN written as a PRE2
    REQUIRE(back->cornEmitters.size() == 1u);
    const mdx::CornEmitter& out = back->cornEmitters[0];
    const mdx::CornEmitter& in = source.cornEmitters[0];
    CHECK(out.node.name == in.node.name);
    CHECK(static_cast<u32>(out.node.flags) == static_cast<u32>(in.node.flags));
    CHECK(out.lifeSpan == in.lifeSpan);
    CHECK(out.emissionRate == in.emissionRate);
    CHECK(out.speed == in.speed);
    CHECK(out.color == in.color);
    CHECK(out.alpha == in.alpha);
    CHECK(out.replaceableId == in.replaceableId);
    CHECK(out.path == in.path);
    CHECK(out.animVisibilityGuide == in.animVisibilityGuide);
    const auto sameTrack = [](const auto& a, const auto& b) {
        return a.isUsed == b.isUsed && a.timestamps == b.timestamps &&
               a.keys_data == b.keys_data && a.interpolationType == b.interpolationType;
    };
    CHECK(sameTrack(out.lifeSpanTracks, in.lifeSpanTracks));
    CHECK(sameTrack(out.emissionRateTracks, in.emissionRateTracks));
    CHECK(sameTrack(out.speedTracks, in.speedTracks));
    CHECK(sameTrack(out.colorTracks, in.colorTracks)); // blue first again
    CHECK(sameTrack(out.alphaTracks, in.alphaTracks));
    CHECK(sameTrack(out.visibilityTracks, in.visibilityTracks));

    // Numbered where Reforged numbers it: after PRE2, before RIBB.
    REQUIRE(back->ribbonEmitters.size() == 1u);
    CHECK(back->particleEmitters2[0].node.objectId < out.node.objectId);
    CHECK(out.node.objectId < back->ribbonEmitters[0].node.objectId);
    CHECK(back->pivotPoints[out.node.objectId] == source.pivotPoints[in.node.objectId]);

    // An edit to a typed flag wins over the raw word the import kept.
    Document edited = reread;
    Model& editedModel = edited.models[0];
    auto& flags =
        std::get<Wc3CornEmitterPayload>(editedModel.nodes.nodes[nodeNamed(editedModel, "corn")].payload);
    flags.unfogged = false;
    flags.popcornScaling = true;
    Result<mdx::Model> flipped = converter.toMdx(edited, ProfileId::Wc3Reforged, 1800);
    REQUIRE(flipped.ok());
    REQUIRE(flipped->cornEmitters.size() == 1u);
    const u32 bits = static_cast<u32>(flipped->cornEmitters[0].node.flags);
    CHECK((bits & static_cast<u32>(mdx::Node::NodeFlag::PopcornUnfogged)) == 0u);
    CHECK((bits & static_cast<u32>(mdx::Node::NodeFlag::PopcornScaling)) != 0u);
}

TEST_CASE("wem a PopcornFX emitter crosses to StarCraft II as its placement",
          "[wem][node][emitter]") {
    Document document = fromMdx(makeReforgedMdx());
    REQUIRE(DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Sc2).ok);
    M3Converter converter;
    Result<m3::Model> out = converter.toM3(document, ProfileId::Sc2);
    REQUIRE(out.ok());
    // StarCraft II runs no PopcornFX: the node is written as its placement,
    // said once for the kind, and none of its channels becomes a stream.
    bool named = false;
    for (const Diagnostic& entry : out.diagnostics.byCode(DiagCode::NodeKindNotCarried)) {
        named = named || entry.message.find("wc3_corn_emitter") != std::string::npos;
    }
    CHECK(named);
    const Model& model = document.models[0];
    const u32 cornNode = nodeNamed(model, "corn");
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.node == cornNode && channel.target.channel == Channel::EmitterProperty) {
            CHECK(streamOf(*out, channel.id) == kInvalidIndex);
        }
    }
}

// ============================================================================
// StarCraft II
// ============================================================================

TEST_CASE("wem m3 carries PAR_, PARC and RIB_ both ways", "[wem][node][emitter]") {
    const m3::Model source = makeM3();
    const Document document = fromM3(source);
    const Model& model = document.models[0];
    CHECK_FALSE(Validate(document, ValidateLevel::Profile).hasErrors());

    const u32 first = nodeNamed(model, "particle_0");
    const u32 second = nodeNamed(model, "particle_1");
    const u32 copyNode = nodeNamed(model, "particle_0_copy_1");
    const u32 ribbonNode = nodeNamed(model, "ribbon_0");
    REQUIRE(first != kInvalidNode);
    REQUIRE(second != kInvalidNode);
    REQUIRE(copyNode != kInvalidNode);
    REQUIRE(ribbonNode != kInvalidNode);

    // The placement is the bone the record names.
    CHECK(model.nodes.nodes[first].parent == 1u);
    CHECK(model.nodes.nodes[second].parent == 0u);
    CHECK(model.nodes.nodes[copyNode].parent == 0u);
    CHECK(model.nodes.nodes[ribbonNode].parent == 1u);

    const auto& p = std::get<Sc2ParticleEmitterPayload>(model.nodes.nodes[first].payload);
    CHECK(p.trailLink == second);
    CHECK(p.ribbonLink == ribbonNode);
    CHECK(p.materialSlot == 0u);
    CHECK(p.initialSpeed.initValue == 3.5f);
    CHECK(p.colorStart.initValue.x == Catch::Approx(30.0f / 255.0f));
    CHECK(p.uvTiling.nullValue.x == 1.0f);
    CHECK(p.pitch.type == 3u);
    CHECK(p.emitterShape == Sc2EmitterShape::Spline);
    REQUIRE(p.splinePoints.size() == 2u);
    CHECK(p.models.size() == 1u);
    CHECK(std::get<Sc2ParticleEmitterPayload>(model.nodes.nodes[second].payload).collisionSpawn ==
          first);
    const auto& c = std::get<Sc2ParticleEmitterPayload>(model.nodes.nodes[copyNode].payload);
    CHECK(c.copyOf == first);
    CHECK(c.squirtAmount.initValue == 4u);
    const auto& r = std::get<Sc2RibbonEmitterPayload>(model.nodes.nodes[ribbonNode].payload);
    REQUIRE(r.splinePoints.size() == 1u);
    CHECK(r.splinePoints[0].node == 0u);
    CHECK(r.active.initValue == 1u);
    CHECK(r.active.nullValue == 0u);

    // Every AnimRef with an id is a channel, keyed or not, typed by its property.
    const AnimChannel* rate = channelFor(model, first, Sub(Sc2ParticleProperty::EmissionRate));
    REQUIRE(rate != nullptr);
    CHECK(rate->id == kRateId);
    const AnimChannel* squirt = channelFor(model, first, Sub(Sc2ParticleProperty::SquirtAmount));
    REQUIRE(squirt != nullptr);
    CHECK(squirt->valueType == geom::AttrType::U32);
    const AnimChannel* unkeyed = channelFor(model, first, Sub(Sc2ParticleProperty::InitialSpeed));
    REQUIRE(unkeyed != nullptr);
    CHECK(unkeyed->id == kUnkeyedId);
    CHECK(channelFor(model, first, Sub(Sc2ParticleProperty::SplinePoint, 1)) != nullptr);
    CHECK(channelFor(model, copyNode, Sub(Sc2ParticleProperty::EmissionRate)) != nullptr);
    const AnimChannel* active = channelFor(model, ribbonNode, Sub(Sc2RibbonProperty::Active));
    REQUIRE(active != nullptr);
    CHECK(active->valueType == geom::AttrType::F32);

    // The squirt keys decode out of SDS6 as the counts they are.
    const SubTrack* squirtKeys = document.clips[0].containers[0].find(kSquirtId);
    REQUIRE(squirtKeys != nullptr);
    REQUIRE(squirtKeys->values.size() == 2 * sizeof(u32));
    u32 last = 0;
    std::memcpy(&last, squirtKeys->values.data() + sizeof(u32), sizeof(u32));
    CHECK(last == 12u);

    M3Converter converter;
    Result<m3::Model> back = converter.toM3(document, ProfileId::Sc2);
    REQUIRE(back.ok());
    REQUIRE(back->particleEmitters.size() == 2u);
    REQUIRE(back->particleEmitterCopies.size() == 1u);
    REQUIRE(back->ribbonEmitters.size() == 1u);

    const m3::ParticleEmitter& e = back->particleEmitters[0];
    const m3::ParticleEmitter& s = source.particleEmitters[0];
    CHECK(e.boneIndex == 1u);
    CHECK(e.materialIndex == 0u);
    CHECK(e.trailLinkIndex == 1);
    CHECK(e.ribbonLinkIndex == 0);
    CHECK(e.trailChance == s.trailChance);
    CHECK(e.copyIndices == std::vector<u32>{0});
    CHECK(e.initialSpeed.initValue == s.initialSpeed.initValue);
    CHECK(e.colorStart.initValue.b == s.colorStart.initValue.b);
    CHECK(e.colorStart.initValue.r == s.colorStart.initValue.r);
    CHECK(e.uvTiling.nullValue.x == s.uvTiling.nullValue.x);
    CHECK(e.sizeAnimation.initValue.z == s.sizeAnimation.initValue.z);
    CHECK(e.pitchType == s.pitchType);
    CHECK(e.pitchAmplitude.initValue == s.pitchAmplitude.initValue);
    CHECK(e.emitterShape == s.emitterShape);
    CHECK(e.shapeRegions == s.shapeRegions);
    CHECK(e.instanceType == s.instanceType);
    CHECK(static_cast<u32>(e.flags) == static_cast<u32>(s.flags));
    CHECK(static_cast<u32>(e.additionalFlags) == static_cast<u32>(s.additionalFlags));
    CHECK(e.maxParticles == s.maxParticles);
    CHECK(e.killRadius == s.killRadius);
    CHECK(e.gravity == s.gravity);
    REQUIRE(e.modelPaths.size() == 1u);
    CHECK(e.modelPaths[0] == "Assets\\Rock.m3");
    REQUIRE(e.splineLineData.size() == 2u);
    CHECK(e.splineLineData[0].initValue.x == 1.0f);
    CHECK(back->particleEmitters[1].collisionSpawnIndex == 0);
    CHECK(back->particleEmitters[1].boneIndex == 0u);

    // Ids cross verbatim, keyed or not; a keyed one is bound and plays in the
    // stream its property is shipped in.
    CHECK(e.emissionRate.animId == kRateId);
    CHECK(e.emissionRate.flags == 0x6);
    CHECK(e.initialSpeed.animId == kUnkeyedId);
    CHECK(e.initialSpeed.flags == 0);
    CHECK(e.squirtAmount.animId == kSquirtId);
    CHECK(streamOf(*back, kRateId) >> 16 == 5u);
    CHECK(streamOf(*back, kSquirtId) >> 16 == 7u);
    CHECK(streamOf(*back, kActiveId) >> 16 == 11u);
    CHECK(streamOf(*back, kSplineId) >> 16 == 2u);
    CHECK(streamOf(*back, kCopyRateId) >> 16 == 5u);
    CHECK(e.splineLineData[1].animId == kSplineId);

    const m3::ParticleEmitterCopy& copy = back->particleEmitterCopies[0];
    CHECK(copy.boneIndex == 0u);
    CHECK(copy.emissionRate.animId == kCopyRateId);
    CHECK(copy.squirtAmount.initValue == 4u);

    const m3::RibbonEmitter& rib = back->ribbonEmitters[0];
    CHECK(rib.boneIndex == 1u);
    CHECK(rib.boneIndexFallback == 0u);
    CHECK(rib.active.animId == kActiveId);
    CHECK(rib.active.initValue == 1u);
    CHECK(rib.active.nullValue == 0u);
    CHECK(rib.ribbonType == m3::RibbonType::Cylinder);
    CHECK(rib.edges == 6u);
    CHECK(rib.yawType == 2u);
    REQUIRE(rib.splineRibbons.size() == 1u);
    CHECK(rib.splineRibbons[0].boneIndex == 0u);
    CHECK(rib.splineRibbons[0].emissionOffset.y == 1.0f);
    CHECK(rib.splineRibbons[0].yawAmplitude.initValue == 0.25f);
}

// ============================================================================
// Across profiles
// ============================================================================

TEST_CASE("wem an export writes a system its game does not run as its placement",
          "[wem][node][emitter]") {
    SECTION("StarCraft II systems into Warcraft III") {
        Document document = fromM3(makeM3());
        const DeriveResult derived =
            DeriveProfile(document, ProfileId::Sc2, ProfileId::Wc3Classic);
        REQUIRE(derived.ok);
        CHECK(derived.diagnostics.countOf(DiagCode::NodeKindNotCarried) == 1u);

        MdxConverter converter;
        Result<mdx::Model> out = converter.toMdx(document, ProfileId::Wc3Classic, 800);
        REQUIRE(out.ok());
        CHECK(out->particleEmitters.empty());
        CHECK(out->particleEmitters2.empty());
        CHECK(out->ribbonEmitters.empty());
        // Two bones, and every emitter node a helper: the tree keeps its shape.
        CHECK(out->helpers.size() == 4u);
        CHECK(out.diagnostics.countOf(DiagCode::NodeKindNotCarried) == 2u);
        CHECK(out.diagnostics.countOf(DiagCode::AnimTrackDropped) == 0u);
    }
    SECTION("Warcraft III systems into StarCraft II") {
        Document document = fromMdx(makeMdx());
        const DeriveResult derived =
            DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Sc2);
        REQUIRE(derived.ok);
        CHECK(derived.diagnostics.countOf(DiagCode::NodeKindNotCarried) == 1u);

        M3Converter converter;
        Result<m3::Model> out = converter.toM3(document, ProfileId::Sc2);
        REQUIRE(out.ok());
        CHECK(out->particleEmitters.empty());
        CHECK(out->ribbonEmitters.empty());
        CHECK(out.diagnostics.countOf(DiagCode::NodeKindNotCarried) == 3u);
        // Their property tracks key nothing StarCraft II holds, so none is
        // written as a stream no AnimRef reads.
        const Model& model = document.models[0];
        for (const AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.channel == Channel::EmitterProperty) {
                CHECK(streamOf(*out, channel.id == 0 ? kInvalidIndex : channel.id) ==
                      kInvalidIndex);
            }
        }
    }
}

// ============================================================================
// Editing
// ============================================================================

TEST_CASE("wem removing a particle emitter takes its copies and clears its links",
          "[wem][node][emitter]") {
    Document document = fromM3(makeM3());
    Model& model = document.models[0];
    const u32 first = nodeNamed(model, "particle_0");
    const u32 second = nodeNamed(model, "particle_1");
    const u32 copyNode = nodeNamed(model, "particle_0_copy_1");
    const u32 ribbonNode = nodeNamed(model, "ribbon_0");

    NodeReferencers referencers;
    referencers.meshes = model.meshes;
    referencers.channels = &model.animChannels;
    referencers.clips = document.clips;
    const RemoveResult removed =
        RemoveNode(model.nodes, first, RemovePolicy::ReparentChildren, SkinPolicy::Refuse, true,
                   referencers);
    REQUIRE(removed.removed);
    CHECK(removed.removedNodes == std::vector<u32>{first, copyNode});

    Diagnostics out;
    const NodeRemaps remaps = CompactNodes(model.nodes, referencers, out);
    const u32 secondNow = remaps.nodes[second];
    const u32 ribbonNow = remaps.nodes[ribbonNode];
    REQUIRE(secondNow != kInvalidNode);
    REQUIRE(ribbonNow != kInvalidNode);
    CHECK(model.nodes.nodes[ribbonNow].kind == NodeKind::Sc2RibbonEmitter);
    // Its collision spawn was the emitter that went.
    CHECK(std::get<Sc2ParticleEmitterPayload>(model.nodes.nodes[secondNow].payload)
              .collisionSpawn == kInvalidNode);
    CHECK(out.countOf(DiagCode::DanglingNodeReference) == 1u);
    CHECK_FALSE(Validate(document, ValidateLevel::Structural).hasErrors());
}

TEST_CASE("wem a rescale restates an emitter's lengths and nothing else",
          "[wem][node][emitter]") {
    Document document = fromMdx(makeMdx());
    Model& model = document.models[0];
    const u32 pre2Node = nodeNamed(model, "pre2");
    const u32 speed = channelFor(model, pre2Node,
                                 EmitterPropertySub(static_cast<u32>(Wc3Particle2Property::Speed)))
                          ->id;

    REQUIRE(RescaleDocument(document, 2.0f).ok);
    const auto& p2 = std::get<Wc3ParticleEmitter2Payload>(model.nodes.nodes[pre2Node].payload);
    CHECK(p2.speed == 240.0f);
    CHECK(p2.width == 24.0f);
    CHECK(p2.end.scaling == 40.0f);
    CHECK(p2.latitude == 0.3f);
    CHECK(p2.emissionRate == 25.0f);
    CHECK(p2.variation == 0.2f);

    const SubTrack* keys = nullptr;
    for (const SubTrackContainer& container : document.clips[0].containers) {
        keys = keys != nullptr ? keys : container.find(speed);
    }
    REQUIRE(keys != nullptr);
    f32 first = 0;
    std::memcpy(&first, keys->values.data(), sizeof(f32));
    CHECK(first == 240.0f);
}

// ============================================================================
// World of Warcraft
// ============================================================================

namespace {

/// One bone, one triangle, and one particle emitter hung off the bone.
m2::Model makeM2WithEmitter() {
    m2::Model model;
    model.modelName = "fx";
    m2::Sequence stand;
    stand.duration = 1000;
    model.sequences.push_back(stand);
    m2::Bone bone;
    bone.parentBoneId = -1;
    bone.pivot = Vector3f{0, 0, 1};
    model.bones.push_back(bone);
    for (const char* name : {"body.blp", "fire.blp"}) {
        m2::Texture texture;
        texture.filename = name;
        model.textures.push_back(texture);
    }
    model.textureCombos = {0};
    model.textureCoordCombos = {0};
    model.textureWeightCombos = {0};
    model.textureTransformCombos = {0xFFFF};
    model.materials.push_back(m2::Material{});
    model.vertices.resize(3);
    for (std::size_t v = 0; v < 3; ++v) {
        model.vertices[v].position = Vector3f{static_cast<f32>(v), 0, 0};
        model.vertices[v].boneWeights = {255, 0, 0, 0};
    }
    m2::SkinProfile skin;
    skin.vertices = {0, 1, 2};
    skin.indices = {0, 1, 2};
    m2::SkinSection submesh;
    submesh.indexCount = 3;
    submesh.vertexCount = 3;
    skin.submeshes.push_back(submesh);
    m2::Batch batch;
    batch.textureCount = 1;
    skin.batches.push_back(batch);
    model.skinProfiles.push_back(std::move(skin));

    m2::ParticleEmitter emitter;
    emitter.flags = static_cast<m2::ParticleFlag>(0x10u | 0x20000u); // rides its emitter; head
    emitter.boneId = 0;
    emitter.position = Vector3f{0, 0, 3};
    emitter.textureId = 1;
    emitter.blendingType = static_cast<m2::ParticleBlending>(4); // SRC_ALPHA, ONE
    emitter.emitterType = m2::ParticleEmitterType::Plane;
    emitter.rows = 2;
    emitter.columns = 2;
    const auto track = [](f32 value) {
        m2::AnimationTrack<f32> t;
        t.timestamps = {{0}};
        t.values = {{value}};
        return t;
    };
    emitter.emissionSpeed = track(0.5f);
    emitter.verticalRange = track(0.5f);
    emitter.gravity = track(2.0f);
    emitter.lifespan = track(2.0f);
    emitter.emissionRate = track(25.0f);
    emitter.emissionAreaWidth = track(1.0f);
    emitter.emissionAreaLength = track(3.0f);
    emitter.colorTrack.timestamps = {unorm16::from_raw(0), unorm16::from_raw(16383),
                                     unorm16::from_raw(32767)};
    emitter.colorTrack.values = {Vector3f{255, 0, 0}, Vector3f{0, 255, 0}, Vector3f{0, 0, 255}};
    emitter.alphaTrack.timestamps = {unorm16::from_raw(0), unorm16::from_raw(5734),
                                     unorm16::from_raw(32767)};
    emitter.alphaTrack.values = {unorm16::from_raw(0), unorm16::from_raw(32767),
                                 unorm16::from_raw(0)};
    emitter.scaleTrack.timestamps = {unorm16::from_raw(0)};
    emitter.scaleTrack.values = {Vector2f{0.4f, 0.4f}};
    emitter.headUVScroll.timestamps = {unorm16::from_raw(0), unorm16::from_raw(32767)};
    emitter.headUVScroll.values = {unorm16::from_raw(1), unorm16::from_raw(3)};
    emitter.twinkleScale = Vector2f{1.2f, 1.2f};
    model.particleEmitters.push_back(emitter);
    return model;
}

u32 m2EmitterNode(const Model& model) {
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        if (IsParticleEmitterKind(model.nodes.nodes[n].kind)) {
            return n;
        }
    }
    return kInvalidNode;
}

} // namespace

TEST_CASE("wem m2 an emitter is a node on its bone, its record whole", "[wem][node][emitter]") {
    // Before: fromM2 read no emitter at all, and a WoW creature's fire and
    // sparkles never reached any export.
    M2Converter converter;
    Result<Document> converted = converter.fromM2(makeM2WithEmitter(), 272);
    REQUIRE(converted.ok());
    const Model& model = converted.value->models[0];
    const u32 n = m2EmitterNode(model);
    REQUIRE(n != kInvalidNode);
    const Node& node = model.nodes.nodes[n];
    CHECK(node.kind == NodeKind::M2ParticleEmitter);
    CHECK(node.parent == 0u);
    CHECK(node.pivot.z == 3.0f);
    const auto& p = std::get<M2ParticleEmitterPayload>(node.payload);
    CHECK(p.texture == 1u);
    CHECK(p.blend == 4u);
    CHECK(p.rows == 2u);
    CHECK(p.gravity.z == -2.0f); // a plain key pulls down -Z
    CHECK(p.emissionRate == 25.0f);
    // Lifetime blocks: fixed16 times, 0..255 colours, raw cells.
    REQUIRE(p.colors.size() == 3u);
    CHECK(p.colors[1].y == 1.0f);
    REQUIRE(p.alphaTimes.size() == 3u);
    CHECK(p.alphaTimes[1] == Catch::Approx(0.175f).margin(0.001f));
    CHECK(p.alphas[1] == 1.0f);
    CHECK(p.headCells == std::vector<u32>{1, 3});
    // Keyed properties are the kind's own channels.
    CHECK(channelFor(model, n, EmitterPropertySub(static_cast<u32>(M2ParticleProperty::Speed))) !=
          nullptr);
    const AnimChannel* gravity =
        channelFor(model, n, EmitterPropertySub(static_cast<u32>(M2ParticleProperty::Gravity)));
    REQUIRE(gravity != nullptr);
    CHECK(gravity->valueType == geom::AttrType::F32x3);
    // Only World of Warcraft runs it, and the document declares it.
    CHECK_FALSE(hasError(Validate(*converted.value, ValidateLevel::Structural),
                         DiagCode::NodeKindNotCarried));
}

TEST_CASE("wem an m2 emitter crosses to a PRE2 in place", "[wem][node][emitter]") {
    M2Converter converter;
    Result<Document> converted = converter.fromM2(makeM2WithEmitter(), 272);
    REQUIRE(converted.ok());
    Document& document = *converted.value;
    Model& model = document.models[0];
    const u32 n = m2EmitterNode(model);
    REQUIRE(n != kInvalidNode);
    const u32 latitude =
        channelFor(model, n, EmitterPropertySub(static_cast<u32>(M2ParticleProperty::VerticalRange)))
            ->id;
    const u32 lifespan =
        channelFor(model, n, EmitterPropertySub(static_cast<u32>(M2ParticleProperty::Lifespan)))->id;

    const cross::M2EmitterReport report = cross::CrossM2Emitters(document);
    CHECK(report.particles == 1u);

    const Node& node = model.nodes.nodes[n];
    REQUIRE(node.kind == NodeKind::Wc3ParticleEmitter2);
    CHECK(node.parent == 0u);
    const auto& p = std::get<Wc3ParticleEmitter2Payload>(node.payload);
    CHECK(p.filter == Wc3ParticleFilter::Additive);
    CHECK(p.speed == 0.5f);
    CHECK(p.latitude == Catch::Approx(28.6479f));
    CHECK(p.gravity == 2.0f);
    // WoW turns the record a quarter about Z: its width is PRE2's length.
    CHECK(p.width == 3.0f);
    CHECK(p.length == 1.0f);
    CHECK(p.texture == 1u);
    CHECK(p.rows == 2u);
    CHECK(p.headOrTail == Wc3ParticleHeadOrTail::Head);
    // The middle segment is alpha's middle key, where the particle peaks.
    CHECK(p.time == Catch::Approx(0.175f).margin(0.001f));
    CHECK(p.middle.alpha == 255);
    CHECK(p.start.alpha == 0);
    // Twinkle's constant multiplier rides the size.
    CHECK(p.start.scaling == Catch::Approx(0.48f));
    CHECK(p.headLife.start == 1u);
    CHECK(p.headDecay.end == 3u);
    // File bit 0x10 rides the emitter: PRE2's model space, the local its world.
    CHECK(hasFlag(node.flags, NodeFlags::ModelSpace));
    CHECK(node.local.translation.z == node.pivot.z);

    // Channels: renamed and restated, or gone with their keys.
    const AnimChannel* lat = model.animChannels.find(latitude);
    REQUIRE(lat != nullptr);
    CHECK(lat->target.sub == EmitterPropertySub(static_cast<u32>(Wc3Particle2Property::Latitude)));
    f32 key = 0;
    for (const Clip& clip : document.clips) {
        for (const SubTrackContainer& container : clip.containers) {
            if (const SubTrack* track = container.find(latitude)) {
                std::memcpy(&key, track->values.data(), sizeof(f32));
            }
            CHECK(container.find(lifespan) == nullptr);
        }
    }
    CHECK(key == Catch::Approx(28.6479f));
    CHECK(model.animChannels.find(lifespan) == nullptr);
    const AnimChannel* gravity = channelFor(
        model, n, EmitterPropertySub(static_cast<u32>(Wc3Particle2Property::Gravity)));
    REQUIRE(gravity != nullptr);
    CHECK(gravity->valueType == geom::AttrType::F32);
}

namespace {

/// The fixture imported, its emitter's record handed to @p edit, then crossed.
Wc3ParticleEmitter2Payload crossEdited(const std::function<void(M2ParticleEmitterPayload&)>& edit,
                                       const cross::M2EmitterOptions& options = {},
                                       Document* keep = nullptr) {
    M2Converter converter;
    Result<Document> converted = converter.fromM2(makeM2WithEmitter(), 272);
    REQUIRE(converted.ok());
    Document& document = keep != nullptr ? (*keep = *converted.value) : *converted.value;
    Model& model = document.models[0];
    const u32 n = m2EmitterNode(model);
    REQUIRE(n != kInvalidNode);
    edit(std::get<M2ParticleEmitterPayload>(model.nodes.nodes[n].payload));
    CHECK(cross::CrossM2Emitters(document, options).particles == 1u);
    return std::get<Wc3ParticleEmitter2Payload>(model.nodes.nodes[n].payload);
}

} // namespace

TEST_CASE("wem an m2 particle lying along its velocity crosses as a PRE2 tail",
          "[wem][node][emitter]") {
    // Dimensius' needles: velocity-oriented (0x4) and far longer than wide. A
    // PRE2 head is a square facing the camera, and they came out as wide
    // sparkles; PRE2's tail is the quad that lies along the velocity.
    const Wc3ParticleEmitter2Payload p = crossEdited([](M2ParticleEmitterPayload& m) {
        m.flags |= static_cast<u32>(m2::ParticleFlag::VelocityOrient);
        m.scaleTimes = {0.0f};
        m.scales = {Vector2f{2.0f, 0.2f}};
    });
    CHECK(p.headOrTail == Wc3ParticleHeadOrTail::Tail);
    // As wide as the particle (twinkle 1.2 on top), and as long: the speed
    // times the tail length is its whole length, both halves.
    CHECK(p.middle.scaling == Catch::Approx(0.2f * 1.2f));
    CHECK(p.speed * p.tailLength == Catch::Approx(2.0f * 2.0f * 1.2f));
    // The tail draws the head's cells.
    CHECK(p.tailLife.start == p.headLife.start);
    CHECK(p.tailDecay.end == p.headDecay.end);
}

TEST_CASE("wem an m2 particle that is not square keeps its area in PRE2",
          "[wem][node][emitter]") {
    // Not velocity-oriented, so it stays a head: the side that keeps its area
    // keeps its light. By its width, Dimensius' flat eye flare was a square
    // four times too large.
    const Wc3ParticleEmitter2Payload p = crossEdited([](M2ParticleEmitterPayload& m) {
        m.scaleTimes = {0.0f};
        m.scales = {Vector2f{0.8f, 0.2f}};
    });
    CHECK(p.headOrTail == Wc3ParticleHeadOrTail::Head);
    CHECK(p.middle.scaling == Catch::Approx(0.4f * 1.2f));
}

TEST_CASE("wem an imploding m2 sphere crosses as its time reverse", "[wem][node][emitter]") {
    // Inward (a negative speed) with the implosion filter: each particle dies
    // at the centre. PRE2 only shoots outward, so the reverse -- a burst from
    // the centre over the inward flight, the curves read backwards.
    Document document;
    const Wc3ParticleEmitter2Payload p = crossEdited(
        [](M2ParticleEmitterPayload& m) {
            m.emitterType = 2;
            m.flags |= static_cast<u32>(m2::ParticleFlag::ImplosionFilter);
            m.speed = -4.0f;
            m.width = 1.0f; // the sphere's radii
            m.length = 3.0f;
            m.lifespan = 2.0f;
        },
        {}, &document);
    CHECK(p.speed == 4.0f);
    CHECK(p.width == 0.0f);
    CHECK(p.length == 0.0f);
    CHECK(p.latitude == 180.0f);
    // Out to the outer radius: 3 / 4 of a second, 0.375 of the WoW life.
    CHECK(p.lifespan == Catch::Approx(0.75f));
    // Birth at the centre is the WoW particle's arrival; death at the rim is
    // its birth (red, invisible). Colour runs red -> green -> blue over life.
    CHECK(p.end.color.x == Catch::Approx(1.0f));
    CHECK(p.end.alpha == 0);
    CHECK(p.start.color.x == Catch::Approx(0.25f).margin(0.001f)); // fixed16 key times
    CHECK(p.start.color.y == Catch::Approx(0.75f).margin(0.001f));
    // Alpha's middle key (0.175) inside the flight, reversed.
    CHECK(p.time == Catch::Approx(1.0f - 0.175f / 0.375f).margin(0.002f));
    // The keyed speed flies outward too.
    const Model& model = document.models[0];
    const AnimChannel* speed = channelFor(
        model, m2EmitterNode(model), EmitterPropertySub(static_cast<u32>(Wc3Particle2Property::Speed)));
    REQUIRE(speed != nullptr);
    f32 key = 0;
    for (const Clip& clip : document.clips) {
        for (const SubTrackContainer& container : clip.containers) {
            if (const SubTrack* track = container.find(speed->id)) {
                std::memcpy(&key, track->values.data(), sizeof(f32));
            }
        }
    }
    CHECK(key == -0.5f);
}

TEST_CASE("wem an m2 colour curve past three keys meets PRE2 at its visible mean",
          "[wem][node][emitter]") {
    // Dimensius' eye flare runs pale blue to red, and the middle sample kept
    // only the red. The middle segment takes what the particle shows while it
    // is visible: here, alpha constant, the plain mean of the curve.
    const Wc3ParticleEmitter2Payload p = crossEdited([](M2ParticleEmitterPayload& m) {
        m.colorTimes = {0.0f, 0.25f, 0.5f, 1.0f};
        m.colors = {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}, Vector3f{1, 0, 0}, Vector3f{0, 0, 0}};
        m.alphaTimes = {0.0f};
        m.alphas = {1.0f};
    });
    CHECK(p.middle.color.x == Catch::Approx(0.375f));
    CHECK(p.middle.color.y == Catch::Approx(0.0f).margin(1e-6));
    CHECK(p.middle.color.z == Catch::Approx(0.25f));
}

TEST_CASE("wem an m2 multi-texture particle folds its other maps in as their mean",
          "[wem][node][emitter]") {
    // `texColor = t0*t1*t2*4` with three colours, `texAlpha = t0.a*t1.a*t2.a*2`
    // without Modx4. PRE2 samples t0 alone; t1 and t2 arrive as their means.
    cross::M2EmitterOptions options;
    options.textureMean = [](const TextureRef& ref) -> std::optional<Vector4f> {
        if (ref.path.find("body") != std::string::npos) {
            return Vector4f{0.5f, 0.5f, 0.5f, 0.5f};
        }
        return Vector4f{0.25f, 1.0f, 1.0f, 1.0f};
    };
    const auto multi = [](M2ParticleEmitterPayload& m) {
        m.flags |= static_cast<u32>(m2::ParticleFlag::MultiTexture) |
                   static_cast<u32>(m2::ParticleFlag::MultitexUse3Colors);
        m.texture2 = 0; // body.blp
        m.texture3 = 1; // fire.blp
    };
    const Wc3ParticleEmitter2Payload plain = crossEdited(multi);
    const Wc3ParticleEmitter2Payload p = crossEdited(multi, options);
    // Colour times 4 * (0.5 * 0.25, 0.5, 0.5), clamped at 1; alpha times 2 * 0.5.
    CHECK(p.start.color.x == Catch::Approx(plain.start.color.x * 0.5f));
    CHECK(p.middle.color.y == Catch::Approx(std::min(plain.middle.color.y * 2.0f, 1.0f)));
    CHECK(p.end.color.z == Catch::Approx(1.0f));
    CHECK(p.middle.alpha == plain.middle.alpha);
    // Without a host to read the maps, nothing folds.
    CHECK(plain.middle.color.x == Catch::Approx(0.65f).margin(0.01f));
}

TEST_CASE("wem a rescale restates an m2 emitter's lengths", "[wem][node][emitter]") {
    M2Converter converter;
    Result<Document> converted = converter.fromM2(makeM2WithEmitter(), 272);
    REQUIRE(converted.ok());
    REQUIRE(RescaleDocument(*converted.value, 100.0f).ok);
    const Model& model = converted.value->models[0];
    const auto& p =
        std::get<M2ParticleEmitterPayload>(model.nodes.nodes[m2EmitterNode(model)].payload);
    CHECK(p.speed == 50.0f);
    CHECK(p.gravity.z == -200.0f);
    CHECK(p.length == 300.0f);
    CHECK(p.scales[0].x == Catch::Approx(40.0f));
    CHECK(p.lifespan == 2.0f);
    CHECK(p.verticalRange == 0.5f);
}

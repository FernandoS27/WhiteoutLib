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

#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

using namespace whiteout;
using namespace whiteout::models::wem;

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
        INFO(ToString(profile));
        for (u32 k = 0; k < static_cast<u32>(NodeKind::Count); ++k) {
            const NodeKind kind = static_cast<NodeKind>(k);
            INFO(ToString(kind));
            const bool expected = HasNodeKind(kSharedNodeKinds, kind) ||
                                  (warcraft && HasNodeKind(kWc3NodeKinds, kind)) ||
                                  (starcraft && HasNodeKind(kSc2NodeKinds, kind));
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
}

TEST_CASE("wem every emitter system declares a closed property table", "[wem][node][emitter]") {
    CHECK(EmitterPropertyCount(NodeKind::Wc3ParticleEmitter1) ==
          static_cast<u32>(Wc3Particle1Property::Count));
    CHECK(EmitterPropertyCount(NodeKind::Wc3ParticleEmitter2) ==
          static_cast<u32>(Wc3Particle2Property::Count));
    CHECK(EmitterPropertyCount(NodeKind::Wc3RibbonEmitter) ==
          static_cast<u32>(Wc3RibbonProperty::Count));
    CHECK(EmitterPropertyCount(NodeKind::Sc2ParticleEmitter) ==
          static_cast<u32>(Sc2ParticleProperty::Count));
    CHECK(EmitterPropertyCount(NodeKind::Sc2RibbonEmitter) ==
          static_cast<u32>(Sc2RibbonProperty::Count));
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

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Warcraft III `.mdx` -> StarCraft II `.m3`: the effects
/// (WC3_TO_SC2_COMPLETION_PLAN.md).
///
/// The node carriers `toM3` makes for Warcraft III's effect, light and camera
/// nodes, the join the native crossing reads (`M3ExportMap`), the one stream
/// writer and the one track cut both directions share, the emitter systems
/// restated inside WEM (`cross/wc3_sc2_emitters`) and the cameras and hit
/// tests crossed natively (`cross/mdx_m3_effects`). Every claim about a written
/// file is read back from the bytes the writer produced, never from the struct
/// the converter filled.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/cross/mdx_m3_effects.h>
#include <whiteout/models/cross/wc3_sc2_emitters.h>
#include <whiteout/models/m3/parser.h>
#include <whiteout/models/m3/writer.h>
#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/models/wem/writer.h>

#include "whiteout/models/wem/converters/m3_track_sink.h"
#include "whiteout/models/wem/converters/mdx_track_slicer.h"

using namespace whiteout;
using namespace whiteout::models::wem;
namespace cross = whiteout::models::cross;

namespace {

mdx::Node makeNode(const std::string& name, u32 objectId, u32 parentId) {
    mdx::Node node;
    node.name = name;
    node.objectId = objectId;
    node.parentId = parentId;
    return node;
}

template <class T>
mdx::Track<T> makeTrack(mdx::InterpolationType interp, std::vector<u32> times,
                        std::vector<T> values) {
    mdx::Track<T> track;
    track.isUsed = true;
    track.interpolationType = interp;
    track.keyCount = times.size();
    track.timestamps = std::move(times);
    track.keys_data = std::move(values);
    return track;
}

/// Two sequences over one timeline and a root bone with one skinned triangle.
mdx::Model makeModel() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "effects";

    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 1000;
    model.sequences.push_back(stand);

    mdx::Sequence walk;
    walk.name = "Walk";
    walk.intervalStart = 2000;
    walk.intervalEnd = 3000;
    model.sequences.push_back(walk);

    mdx::Texture texture;
    texture.fileName = "textures/body.blp";
    model.textures.push_back(texture);

    mdx::Material material;
    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::None;
    layer.textureId = 0;
    layer.textureAnimationId = 0xFFFFFFFF;
    material.layers.push_back(layer);
    model.materials.push_back(material);

    mdx::Bone root;
    root.node = makeNode("root", 0, mdx::Node::NO_PARENT);
    model.bones.push_back(root);
    model.pivotPoints = {Vector3f{0, 0, 0}};

    mdx::Geoset geoset;
    geoset.lodName = "body";
    geoset.vertexPositions = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{1, 1, 0}};
    geoset.vertexNormals = {Vector3f{0, 0, 1}, Vector3f{0, 0, 1}, Vector3f{0, 0, 1}};
    geoset.textureCoordinateSets.push_back({Vector2f{0, 0}, Vector2f{1, 0}, Vector2f{1, 1}});
    geoset.faces = {0, 1, 2};
    geoset.materialId = 0;
    geoset.vertexGroups = {0, 0, 0};
    geoset.matrixGroups = {1};
    geoset.matrixIndices = {0};
    model.geosets.push_back(geoset);
    return model;
}

/// Adds a node's pivot at its object id.
void setPivot(mdx::Model& model, u32 objectId, const Vector3f& pivot) {
    if (model.pivotPoints.size() <= objectId) {
        model.pivotPoints.resize(objectId + 1, Vector3f{0, 0, 0});
    }
    model.pivotPoints[objectId] = pivot;
}

mdx::ParticleEmitter2 makeEmitter(const std::string& name, u32 objectId, u32 parentId) {
    mdx::ParticleEmitter2 emitter;
    emitter.node = makeNode(name, objectId, parentId);
    emitter.speed = 100.0f;
    emitter.lifespan = 1.0f;
    emitter.emissionRate = 10.0f;
    emitter.width = 20.0f;
    emitter.length = 20.0f;
    emitter.segmentScaling = {10.0f, 10.0f, 10.0f};
    emitter.segmentAlpha = {255, 255, 255};
    emitter.segmentColor = {Vector3f{1, 1, 1}, Vector3f{1, 1, 1}, Vector3f{1, 1, 1}};
    emitter.time = 0.5f;
    emitter.rows = 1;
    emitter.columns = 1;
    return emitter;
}

/// The document an `.m3` export stages: Warcraft III's set derived into
/// StarCraft II's, the rig restated, the lengths in StarCraft II's units.
Document stage(const mdx::Model& model) {
    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(model);
    REQUIRE(imported.ok());
    Document document = std::move(*imported.value);
    document.declare(ProfileId::Sc2);
    REQUIRE(DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Sc2).ok);
    REQUIRE(RetargetSkeleton(document, ProfileId::Sc2).ok);
    REQUIRE(RescaleDocument(document, RescaleFactorBetween(ProfileId::Wc3Classic, ProfileId::Sc2))
                .ok);
    return document;
}

struct Written {
    m3::Model model;
    M3ExportMap map;
};

/// `toM3`, then the writer, then the parser: every assertion reads the file.
Written writeAndRead(const Document& document, bool carriers) {
    M3Converter converter;
    M3ExportSettings settings;
    settings.effectNodeBones = carriers;
    Written result;
    Result<m3::Model> converted =
        converter.toM3(document, ProfileId::Sc2, 29, settings, &result.map);
    REQUIRE(converted.ok());
    m3::Writer writer;
    const std::vector<u8> bytes = writer.write(*converted);
    m3::Parser parser;
    result.model = parser.parse(std::span<const u8>(bytes));
    return result;
}

u32 nodeWithObjectId(const Document& document, u32 objectId) {
    const NodeTree& nodes = document.models.front().nodes;
    for (u32 n = 0; n < nodes.size(); ++n) {
        const NodeNative::Entry* entry = nodes.nodes[n].native.find("objectId");
        if (entry != nullptr && nodes.nodes[n].native.value("objectId") == objectId) {
            return n;
        }
    }
    return kInvalidIndex;
}

u32 boneNamed(const m3::Model& model, const std::string& name) {
    for (u32 b = 0; b < model.bones.size(); ++b) {
        std::string bone = model.bones[b].name;
        while (!bone.empty() && bone.back() == '\0') {
            bone.pop_back();
        }
        if (bone == name) {
            return b;
        }
    }
    return kInvalidIndex;
}

u32 sequenceNamed(const m3::Model& model, const std::string& name) {
    for (u32 s = 0; s < model.sequences.size(); ++s) {
        std::string sequence = model.sequences[s].name;
        while (!sequence.empty() && sequence.back() == '\0') {
            sequence.pop_back();
        }
        if (sequence == name) {
            return s;
        }
    }
    return kInvalidIndex;
}

/// The `(slot << 16) | block` the container at @p stc names @p animId with.
u32 refFor(const m3::Model& model, u32 stc, u32 animId) {
    const m3::SubTrackContainer& container = model.subTrackCollections.at(stc);
    for (std::size_t j = 0; j < container.animIds.size(); ++j) {
        if (container.animIds[j] == animId) {
            return container.animRefs[j];
        }
    }
    return kInvalidIndex;
}

} // namespace

// ============================================================================
// C1 -- the seam
// ============================================================================

TEST_CASE("wem m3 sink: a colour stream resamples its alpha and rounds to the byte",
          "[wem][m3][sink]") {
    SubTrack color;
    color.interp = Interpolation::Linear;
    color.times = {0.0f, 1.0f};
    const f32 colorKeys[] = {0.5f, 0.0f, 0.0f, 0.5f, 1.0f, 0.0f};
    color.values.assign(reinterpret_cast<const u8*>(colorKeys),
                        reinterpret_cast<const u8*>(colorKeys) + sizeof(colorKeys));
    SubTrack alpha;
    alpha.interp = Interpolation::Linear;
    alpha.times = {0.5f};
    const f32 alphaKey = 0.25f;
    alpha.values.assign(reinterpret_cast<const u8*>(&alphaKey),
                        reinterpret_cast<const u8*>(&alphaKey) + sizeof(alphaKey));

    const SubTrack merged = m3_sink::MergeColorAlpha(&color, &alpha, Vector3f{1, 1, 1}, 1.0f);
    REQUIRE(merged.times == std::vector<f32>{0.0f, 0.5f, 1.0f});
    REQUIRE(merged.values.size() == 3 * 4 * sizeof(f32));

    m3::SubTrackContainer stc;
    m3_sink::StreamSpec spec;
    spec.stream = m3_sink::Stream::Sdcc;
    spec.type = geom::AttrType::F32x4;
    const u32 ref = m3_sink::WriteStream(stc, spec, merged, 100, 1.0f, false);
    REQUIRE(ref == (4u << 16));
    REQUIRE(stc.sdcc.size() == 1);
    const m3::AnimBlock<m3::ColorBGRA>& block = stc.sdcc[0];
    CHECK(block.timestamps == std::vector<i32>{100, 600, 1100});
    CHECK(block.endFrame == 1100u);
    REQUIRE(block.keys.size() == 3);
    // 0.5 is byte 128, never 127: the round is to nearest.
    CHECK(block.keys[0].r == 128);
    CHECK(block.keys[0].g == 0);
    // The alpha key sits between two colour keys: the colour there is the
    // halfway colour, and the alpha holds its one key on both sides.
    CHECK(block.keys[1].r == 128);
    CHECK(block.keys[1].g == 128);
    CHECK(block.keys[1].b == 0);
    CHECK(block.keys[1].a == 64);
    CHECK(block.keys[2].g == 255);
    CHECK(block.keys[2].a == 64);
}

TEST_CASE("wem m3 sink: a container stays sorted and its state lists the same ids",
          "[wem][m3][sink]") {
    m3::Model model;
    m3::SubTrackContainer stc;
    stc.animIds = {5, 9};
    stc.animRefs = {50, 90};
    stc.animationStateIndex = 0;
    model.subTrackCollections.push_back(stc);
    m3::AnimationState state;
    state.animIds = {5, 9};
    model.animationStates.push_back(state);

    m3_sink::AddToContainer(model, 0, 7, 70);
    CHECK(model.subTrackCollections[0].animIds == std::vector<u32>{5, 7, 9});
    CHECK(model.subTrackCollections[0].animRefs == std::vector<u32>{50, 70, 90});
    CHECK(model.animationStates[0].animIds == std::vector<u32>{5, 7, 9});
}

TEST_CASE("wem mdx slicer: an emitter property cuts into the windows a node track does",
          "[wem][mdx][slice]") {
    mdx::Model model = makeModel();
    // The same keys on an attachment's visibility (which WEM imports) and on
    // an emitter's emission rate (which it does not): the two cuts must agree
    // key for key, bracket keys included.
    const std::vector<u32> times = {0, 500, 1500, 2500};
    const std::vector<f32> values = {1.0f, 2.0f, 3.0f, 4.0f};
    mdx::Attachment attachment;
    attachment.node = makeNode("Origin Ref", 1, 0);
    attachment.visibilityTracks = makeTrack(mdx::InterpolationType::Linear, times, values);
    model.attachments.push_back(attachment);
    mdx::ParticleEmitter2 emitter = makeEmitter("Smoke", 2, 0);
    emitter.emissionRateTracks = makeTrack(mdx::InterpolationType::Linear, times, values);
    model.particleEmitters2.push_back(emitter);
    setPivot(model, 2, Vector3f{0, 0, 0});

    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(model);
    REQUIRE(imported.ok());
    const Document& document = *imported.value;

    const std::vector<mdx_slice::ClipCut> cuts =
        mdx_slice::CutForClips(model.particleEmitters2[0].emissionRateTracks, document, 0);
    REQUIRE(cuts.size() == 2);
    for (const mdx_slice::ClipCut& cut : cuts) {
        const Clip& clip = document.clips.at(cut.clip);
        const SubTrack* imported = nullptr;
        for (const SubTrack& track : clip.containers.at(0).subTracks) {
            const AnimChannel* channel = document.models[0].animChannels.find(track.channel);
            if (channel != nullptr && channel->target.channel == Channel::Visibility) {
                imported = &track;
            }
        }
        REQUIRE(imported != nullptr);
        CHECK(cut.track.times == imported->times);
        CHECK(cut.track.values == imported->values);
        CHECK(cut.track.interp == imported->interp);
    }
    // Stand keeps the key after its window; Walk the one before it.
    CHECK(cuts[0].track.times == std::vector<f32>{0.0f, 0.5f, 1.5f});
    CHECK(cuts[1].track.times == std::vector<f32>{-0.5f, 0.5f});
}

TEST_CASE("wem mdx a global sequence only an emitter property keys gets its clip",
          "[wem][mdx][slice]") {
    mdx::Model model = makeModel();
    model.globalSequences = {750};
    mdx::ParticleEmitter2 emitter = makeEmitter("Smoke", 1, 0);
    emitter.emissionRateTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 750}, std::vector<f32>{0.0f, 20.0f});
    emitter.emissionRateTracks.globalSequenceId = 0;
    model.particleEmitters2.push_back(emitter);
    setPivot(model, 1, Vector3f{0, 0, 0});

    MdxConverter converter;
    Result<Document> with = converter.fromMdx(model);
    REQUIRE(with.ok());
    const Clip* loop = nullptr;
    for (const Clip& clip : with->clips) {
        if (clip.native.value("globalSequenceId", -1) == 0) {
            loop = &clip;
        }
    }
    REQUIRE(loop != nullptr);
    CHECK(hasFlag(loop->flags, ClipFlags::AutoPlay));
    CHECK(hasFlag(loop->flags, ClipFlags::WorldClocked));
    CHECK(loop->duration == Catch::Approx(0.75f));
    const std::vector<mdx_slice::ClipCut> cuts =
        mdx_slice::CutForClips(model.particleEmitters2[0].emissionRateTracks, *with, 0);
    REQUIRE(cuts.size() == 1);
    CHECK(cuts[0].track.times == std::vector<f32>{0.0f, 0.75f});

    // And no loop is invented for a global sequence nothing keys.
    model.particleEmitters2[0].emissionRateTracks = mdx::Track<f32>{};
    Result<Document> without = converter.fromMdx(model);
    REQUIRE(without.ok());
    CHECK(std::none_of(without->clips.begin(), without->clips.end(), [](const Clip& clip) {
        return clip.native.value("globalSequenceId", -1) >= 0;
    }));
}

TEST_CASE("wem m3 an emitter node is a bone that keys its own visibility",
          "[wem][m3][carriers]") {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 emitter = makeEmitter("Smoke", 1, 0);
    emitter.visibilityTracks =
        makeTrack(mdx::InterpolationType::None, {0, 500}, std::vector<f32>{1.0f, 0.0f});
    model.particleEmitters2.push_back(emitter);
    setPivot(model, 1, Vector3f{10, 20, 30});
    const Document document = stage(model);

    const Written off = writeAndRead(document, false);
    CHECK(boneNamed(off.model, "Smoke") == kInvalidIndex);

    const Written on = writeAndRead(document, true);
    const u32 bone = boneNamed(on.model, "Smoke");
    REQUIRE(bone != kInvalidIndex);
    CHECK(boneNamed(on.model, "Smoke_Vis") == kInvalidIndex);
    // The pivot, in StarCraft II's basis and units: (x, y, z) -> (y, -x, z) / 100.
    const m3::Bone& written = on.model.bones[bone];
    CHECK(written.position.initValue.x == Catch::Approx(0.20f));
    CHECK(written.position.initValue.y == Catch::Approx(-0.10f));
    CHECK(written.position.initValue.z == Catch::Approx(0.30f));
    CHECK(written.parentIndex == boneNamed(on.model, "root"));

    // The visibility is the SDFG stream of Stand's container, keys 1 then 0.
    REQUIRE(written.visibility.animId != 0u);
    CHECK(written.visibility.flags == 0x6);
    const u32 stand = sequenceNamed(on.model, "Stand");
    REQUIRE(stand != kInvalidIndex);
    const u32 stc = on.model.animationGroups.at(stand).subtrackIndices.at(0);
    const u32 ref = refFor(on.model, stc, written.visibility.animId);
    REQUIRE(ref != kInvalidIndex);
    CHECK((ref >> 16) == 11u);
    const auto& block = on.model.subTrackCollections[stc].sdfg.at(ref & 0xFFFF);
    REQUIRE(block.keys.size() >= 2);
    CHECK(block.keys[0].value == 1u);
    CHECK(block.keys[1].value == 0u);

    const u32 node = nodeWithObjectId(document, 1);
    REQUIRE(node != kInvalidIndex);
    CHECK(on.map.nodeBone.at(node) == bone);
    CHECK(on.map.nodeVisBone.at(node) == bone);
}

TEST_CASE("wem m3 a keyed visibility with children moves to a leaf of its own",
          "[wem][m3][carriers]") {
    mdx::Model model = makeModel();
    mdx::Attachment sprite;
    sprite.node = makeNode("Sprite First Ref", 1, 0);
    sprite.visibilityTracks =
        makeTrack(mdx::InterpolationType::None, {0, 500}, std::vector<f32>{0.0f, 1.0f});
    model.attachments.push_back(sprite);
    setPivot(model, 1, Vector3f{0, 0, 50});
    mdx::ParticleEmitter2 smoke = makeEmitter("Smoke", 2, 1);
    model.particleEmitters2.push_back(smoke);
    setPivot(model, 2, Vector3f{0, 0, 60});
    const Document document = stage(model);

    const Written on = writeAndRead(document, true);
    const u32 owner = boneNamed(on.model, "Sprite First Ref");
    const u32 leaf = boneNamed(on.model, "Sprite First Ref_Vis");
    const u32 child = boneNamed(on.model, "Smoke");
    REQUIRE(owner != kInvalidIndex);
    REQUIRE(leaf != kInvalidIndex);
    REQUIRE(child != kInvalidIndex);
    CHECK(owner < leaf);
    // The leaf alone hides: the owner, and the emitter under it, do not.
    CHECK(on.model.bones[leaf].parentIndex == owner);
    CHECK(on.model.bones[child].parentIndex == owner);
    CHECK(on.model.bones[owner].visibility.animId == 0u);
    CHECK(on.model.bones[owner].visibility.initValue == 1u);
    CHECK(on.model.bones[leaf].visibility.animId != 0u);
    // An identity child: it sits exactly where its owner does.
    CHECK(on.model.bones[leaf].position.initValue.z == Catch::Approx(0.0f));
    // The attachment point itself hides with the node.
    REQUIRE(on.model.attachmentPoints.size() == 1);
    CHECK(on.model.attachmentPoints[0].boneIndex == leaf);
    const u32 node = nodeWithObjectId(document, 1);
    CHECK(on.map.nodeBone.at(node) == owner);
    CHECK(on.map.nodeVisBone.at(node) == leaf);
    // An identity child binds where its owner does.
    CHECK(std::memcmp(&on.model.initialReference[leaf].matrix,
                      &on.model.initialReference[owner].matrix, sizeof(Matrix44f)) == 0);
}

TEST_CASE("wem m3 a child that shares its parent's visibility needs no leaf",
          "[wem][m3][carriers]") {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 smoke = makeEmitter("Smoke", 1, 0);
    smoke.visibilityTracks =
        makeTrack(mdx::InterpolationType::None, {0, 500}, std::vector<f32>{1.0f, 0.0f});
    model.particleEmitters2.push_back(smoke);
    setPivot(model, 1, Vector3f{0, 0, 60});
    mdx::Helper offset;
    offset.node = makeNode("Smoke Offset", 2, 1);
    model.helpers.push_back(offset);
    setPivot(model, 2, Vector3f{0, 5, 60});
    Document document = stage(model);

    CHECK(boneNamed(writeAndRead(document, true).model, "Smoke_Vis") != kInvalidIndex);
    const u32 helper = nodeWithObjectId(document, 2);
    REQUIRE(helper != kInvalidIndex);
    document.models[0].nodes.nodes[helper].native.set(kNodeSharesParentVisibility,
                                                      static_cast<i64>(1));
    const Written on = writeAndRead(document, true);
    CHECK(boneNamed(on.model, "Smoke_Vis") == kInvalidIndex);
    CHECK(on.model.bones[boneNamed(on.model, "Smoke")].visibility.animId != 0u);
}

TEST_CASE("wem m3 a light moves on its carrier bone", "[wem][m3][carriers]") {
    mdx::Model model = makeModel();
    mdx::Light light;
    light.node = makeNode("Glow", 1, 0);
    light.type = mdx::Light::LightType::Omni;
    light.attenuationStart = 10.0f;
    light.attenuationEnd = 80.0f;
    light.color = Vector3f{1, 0.5f, 0};
    light.intensity = 2.0f;
    light.node.translationTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                             std::vector<Vector3f>{{0, 0, 0}, {0, 0, 100}});
    model.lights.push_back(light);
    setPivot(model, 1, Vector3f{0, 0, 0});
    const Document document = stage(model);

    const Written off = writeAndRead(document, false);
    REQUIRE(off.model.lights.size() == 1);
    CHECK(boneNamed(off.model, "Glow") == kInvalidIndex);
    // A Warcraft III light states no LITE flags; it lights opaque geometry the
    // way every shipped light says it does.
    CHECK(off.model.lights[0].flags == m3::LightFlag::LightOpaque);

    const Written on = writeAndRead(document, true);
    const u32 bone = boneNamed(on.model, "Glow");
    REQUIRE(bone != kInvalidIndex);
    REQUIRE(on.model.lights.size() == 1);
    CHECK(on.model.lights[0].boneIndex == bone);
    const m3::Bone& carrier = on.model.bones[bone];
    REQUIRE(carrier.position.animId != 0u);
    const u32 stand = sequenceNamed(on.model, "Stand");
    const u32 stc = on.model.animationGroups.at(stand).subtrackIndices.at(0);
    const u32 ref = refFor(on.model, stc, carrier.position.animId);
    REQUIRE(ref != kInvalidIndex);
    CHECK((ref >> 16) == 2u);
    const auto& block = on.model.subTrackCollections[stc].sd3v.at(ref & 0xFFFF);
    REQUIRE(block.keys.size() == 2);
    // Keyed in WC3 units along +Z; the file says one StarCraft II unit up.
    CHECK(block.keys[1].z == Catch::Approx(1.0f));
}

TEST_CASE("wem m3 the export map joins clips to sequences and their first container",
          "[wem][m3][map]") {
    mdx::Model model = makeModel();
    model.particleEmitters2.push_back(makeEmitter("Smoke", 1, 0));
    setPivot(model, 1, Vector3f{0, 0, 0});
    const Document document = stage(model);
    const Written on = writeAndRead(document, true);

    REQUIRE(on.map.clipSequence.size() == document.clips.size());
    REQUIRE(on.map.sequenceStc.size() == on.model.sequences.size());
    for (std::size_t c = 0; c < document.clips.size(); ++c) {
        const u32 s = on.map.clipSequence[c];
        REQUIRE(s < on.model.sequences.size());
        CHECK(sequenceNamed(on.model, document.clips[c].name) == s);
        CHECK(on.map.sequenceStc[s] == on.model.animationGroups[s].subtrackIndices.at(0));
    }
    u32 highest = 0;
    for (const m3::SubTrackContainer& stc : on.model.subTrackCollections) {
        for (const u32 id : stc.animIds) {
            highest = std::max(highest, id);
        }
    }
    CHECK(on.map.nextAnimId > highest);
    const u32 node = nodeWithObjectId(document, 1);
    CHECK(on.map.nodeBone.at(node) == boneNamed(on.model, "Smoke"));
}

// ============================================================================
// C4 -- billboards
// ============================================================================

TEST_CASE("wem m3 a Warcraft III billboard flag is a BBSC record on the node's bone",
          "[wem][m3][billboard]") {
    mdx::Model model = makeModel();
    const auto helper = [&](const std::string& name, u32 objectId, u32 flags) {
        mdx::Helper node;
        node.node = makeNode(name, objectId, 0);
        node.node.flags = static_cast<mdx::Node::NodeFlag>(flags);
        model.helpers.push_back(node);
        setPivot(model, objectId, Vector3f{0, 0, static_cast<f32>(objectId)});
    };
    helper("Free", 1, 0x8);
    helper("LockX", 2, 0x10);
    helper("LockY", 3, 0x20);
    helper("LockZ", 4, 0x40);
    helper("Anchored", 5, 0x8 | 0x80);
    helper("AnchoredOnly", 6, 0x80);
    helper("Plain", 7, 0);
    const Written on = writeAndRead(stage(model), false);

    const auto recordOn = [&](const std::string& bone) -> const m3::BillboardBehavior* {
        const u32 index = boneNamed(on.model, bone);
        for (const m3::BillboardBehavior& record : on.model.billboardBehaviors) {
            if (record.boneIndex == index) {
                return &record;
            }
        }
        return nullptr;
    };
    REQUIRE(on.model.billboardBehaviors.size() == 5);
    const auto* free = recordOn("Free");
    const auto* lockX = recordOn("LockX");
    const auto* lockY = recordOn("LockY");
    const auto* lockZ = recordOn("LockZ");
    const auto* anchored = recordOn("Anchored");
    REQUIRE(free != nullptr);
    REQUIRE(lockX != nullptr);
    REQUIRE(lockY != nullptr);
    REQUIRE(lockZ != nullptr);
    REQUIRE(anchored != nullptr);
    CHECK(recordOn("AnchoredOnly") == nullptr);
    CHECK(recordOn("Plain") == nullptr);

    CHECK(free->billboardType == 6);
    CHECK(anchored->billboardType == 6);
    CHECK(lockZ->billboardType == 2);
    // The axes cross through the basis change (E1): Warcraft III's Y is the
    // model's X, and its X is the model's -Y.
    CHECK(lockY->billboardType == 0);
    CHECK(lockX->billboardType == 1);
    for (const auto* record : {free, lockX, lockY, lockZ, anchored}) {
        CHECK(record->cameraLookAt == 1);
        CHECK(record->forward.w == Catch::Approx(1.0f));
    }
    CHECK(lockY->up.w == Catch::Approx(1.0f));
    CHECK(lockX->up.w == Catch::Approx(1.0f));

    // Not for any other source: the `.m2` importer sets the same bits and
    // means something else by them.
    Document other = stage(model);
    other.defaultProfile = ProfileId::Wow;
    CHECK(writeAndRead(other, false).model.billboardBehaviors.empty());
}

// ============================================================================
// C5 -- particles, C7 -- ribbons
// ============================================================================

namespace {

/// `writeAndRead` plus the crossings, the way the export driver calls them:
/// the document prepared, staged, its emitters restated, converted with
/// carriers, then the cameras and hit tests.
struct Crossed {
    m3::Model model;
    std::vector<u8> bytes; ///< The file, as written.
    M3ExportMap map;
    Document staged; ///< What `toM3` was given.
    struct Report {
        u32 particleRecords = 0;
        u32 modelParticleRecords = 0;
        u32 ribbonRecords = 0;
        u32 cameraRecords = 0;
        u32 hitTests = 0;
        Diagnostics diagnostics; ///< Both crossings'.
    } report;
};

/// What the driver's texture plan does, and the rest of its choices: each
/// `.mdx` texture named @p textures gets that path, @p teamGlowMask is a
/// texture of its own, and @p spawned is which spawned models were written.
struct Plan {
    std::vector<std::string> textures;
    std::string teamGlowMask;
    std::map<std::string, std::string> spawned;
};

Crossed crossDocument(Document document, const mdx::Model& source, const Plan& plan) {
    for (std::size_t t = 0; t < plan.textures.size() && t < document.textures.size(); ++t) {
        document.textures[t].path = plan.textures[t];
    }
    cross::Wc3EmitterOptions emitters;
    if (!plan.teamGlowMask.empty()) {
        TextureRef mask;
        mask.path = plan.teamGlowMask;
        emitters.teamGlowMask = static_cast<u32>(document.textures.size());
        document.textures.push_back(mask);
    }
    emitters.spawnedModels = plan.spawned;
    emitters.lengthScale = 0.01f;
    Diagnostics prepared;
    cross::PrepareWc3Effects(document, prepared);
    document.declare(ProfileId::Sc2);
    REQUIRE(DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Sc2).ok);
    REQUIRE(RetargetSkeleton(document, ProfileId::Sc2).ok);
    REQUIRE(RescaleDocument(document, 0.01f).ok);
    const cross::Wc3EmitterReport crossed = cross::CrossWc3Emitters(document, emitters);

    M3Converter m3;
    M3ExportSettings settings;
    settings.effectNodeBones = true;
    Crossed out;
    Result<m3::Model> converted = m3.toM3(document, ProfileId::Sc2, 29, settings, &out.map);
    REQUIRE(converted.ok());
    cross::Wc3EffectOptions options;
    options.lengthScale = 0.01f;
    const cross::Wc3EffectReport effects =
        cross::CrossWc3Effects(source, document, out.map, options, *converted);
    out.report.particleRecords = crossed.particleRecords;
    out.report.modelParticleRecords = crossed.modelParticleRecords;
    out.report.ribbonRecords = crossed.ribbonRecords;
    out.report.cameraRecords = effects.cameraRecords;
    out.report.hitTests = effects.hitTests;
    out.report.diagnostics.append(crossed.diagnostics);
    out.report.diagnostics.append(effects.diagnostics);
    m3::Writer writer;
    out.bytes = writer.write(*converted);
    m3::Parser parser;
    out.model = parser.parse(std::span<const u8>(out.bytes));
    out.staged = std::move(document);
    return out;
}

Document importMdx(const mdx::Model& source) {
    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    return std::move(*imported.value);
}

Crossed crossAndRead(const mdx::Model& source, std::vector<std::string> textures = {},
                     std::string teamGlowMask = {},
                     std::map<std::string, std::string> spawned = {}) {
    return crossDocument(importMdx(source), source,
                         Plan{std::move(textures), std::move(teamGlowMask), std::move(spawned)});
}

/// The keys of the block @p ref names in the container of sequence @p name,
/// as (time, value) pairs; empty when that sequence does not key it.
template <class R, class K>
std::vector<std::pair<i32, K>> keysOf(const m3::Model& model, const std::string& name,
                                      const m3::AnimRef<R>& ref,
                                      std::vector<m3::AnimBlock<K>> m3::SubTrackContainer::*arr,
                                      u32 slot) {
    std::vector<std::pair<i32, K>> out;
    const u32 s = sequenceNamed(model, name);
    if (s == kInvalidIndex || ref.animId == 0) {
        return out;
    }
    const u32 stc = model.animationGroups.at(s).subtrackIndices.at(0);
    const u32 word = refFor(model, stc, ref.animId);
    if (word == kInvalidIndex || (word >> 16) != slot) {
        return out;
    }
    const m3::AnimBlock<K>& block = (model.subTrackCollections[stc].*arr).at(word & 0xFFFF);
    for (std::size_t k = 0; k < block.keys.size(); ++k) {
        out.emplace_back(block.timestamps[k], block.keys[k]);
    }
    return out;
}

const m3::StandardMaterial& materialOf(const m3::Model& model, u32 mapIndex) {
    const m3::MaterialMap& map = model.materialMaps.at(mapIndex);
    REQUIRE(map.materialType == m3::MaterialType::Standard);
    return model.standardMaterials.at(map.materialIndex);
}

} // namespace

TEST_CASE("wem m3 a Warcraft III particle emitter reads back as the PAR_ its rows name",
          "[wem][m3][particles]") {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 pe = makeEmitter("Smoke", 1, 0);
    pe.speed = 150.0f;
    pe.variation = 0.2f;
    pe.latitude = 30.0f;
    pe.gravity = 50.0f;
    pe.lifespan = 1.5f;
    pe.emissionRate = 12.0f;
    pe.width = 40.0f;
    pe.length = 20.0f;
    pe.segmentScaling = {10.0f, 20.0f, 5.0f};
    pe.segmentColor = {Vector3f{1.0f, 0.5f, 0.25f}, Vector3f{0, 1, 0}, Vector3f{0, 0, 1}};
    pe.segmentAlpha = {255, 128, 0};
    pe.time = 0.3f;
    pe.rows = 2;
    pe.columns = 4;
    pe.headInterval = {0, 3, 1};
    pe.headDecayInterval = {4, 7, 1};
    pe.filterMode = 1; // additive
    pe.textureId = 0;
    pe.priorityPlane = 2;
    pe.node.flags = static_cast<mdx::Node::NodeFlag>(0x8000 | 0x10000); // unshaded, sorted
    model.particleEmitters2.push_back(pe);
    setPivot(model, 1, Vector3f{10, 20, 30});

    const Crossed out = crossAndRead(model, {"Assets/Textures/smoke.dds"});
    REQUIRE(out.model.particleEmitters.size() == 1);
    const m3::ParticleEmitter& p = out.model.particleEmitters[0];
    CHECK(p.boneIndex == boneNamed(out.model, "Smoke"));

    // Rows 1-2: s(1-v) and s(1+v), in StarCraft II units, and the randomise bit.
    CHECK(p.initialSpeed.initValue == Catch::Approx(1.2f));
    CHECK(p.initialSpeedRandom.initValue == Catch::Approx(1.8f));
    CHECK(hasFlag(p.additionalFlags, m3::ParticleAdditionalFlag::EmitSpeedRandomize));
    CHECK(hasFlag(p.additionalFlags, m3::ParticleAdditionalFlag::WorldSpace));
    // Row 3: the square spread whose RMS polar angle is Warcraft III's.
    const f32 side = 30.0f * 3.14159265f / 180.0f / std::sqrt(2.0f);
    CHECK(p.velocityType == 0u);
    CHECK(p.initialHorizontal.initValue == Catch::Approx(side));
    CHECK(p.initialVertical.initValue == Catch::Approx(side));
    // Row 5: pulled down, in StarCraft II units.
    CHECK(p.gravity == Catch::Approx(-0.5f));
    CHECK_FALSE(hasFlag(p.flags, m3::ParticleFlag::MultiplyGravityByMass));
    // Rows 7-10.
    CHECK(p.lifetime.initValue == Catch::Approx(1.5f));
    CHECK(p.lifetimeRandom.initValue == Catch::Approx(1.5f));
    CHECK_FALSE(hasFlag(p.additionalFlags, m3::ParticleAdditionalFlag::LifespanRandomize));
    CHECK(p.emissionRate.initValue == Catch::Approx(12.0f));
    CHECK(p.maxParticles == 37u); // ceil(12 * 1.5 * 1.15) + 0 + 16
    // Row 11: the plane, width across and length along.
    CHECK(p.emitterShape == m3::EmitterShape::Plane);
    CHECK(p.shapeOuter.initValue.x == Catch::Approx(0.4f));
    CHECK(p.shapeOuter.initValue.y == Catch::Approx(0.2f));
    CHECK(p.shapeOuter.initValue.z == Catch::Approx(0.0f));
    // Row 12: the key is the full width.
    CHECK(p.sizeAnimation.initValue.x == Catch::Approx(0.2f));
    CHECK(p.sizeAnimation.initValue.y == Catch::Approx(0.4f));
    CHECK(p.sizeAnimation.initValue.z == Catch::Approx(0.1f));
    CHECK(p.sizeMidTime == Catch::Approx(0.3f));
    // Row 13: truncated bytes, the alpha kept.
    CHECK(p.colorStart.initValue.r == 255);
    CHECK(p.colorStart.initValue.g == 127);
    CHECK(p.colorStart.initValue.b == 63);
    CHECK(p.colorStart.initValue.a == 255);
    CHECK(p.colorMid.initValue.g == 255);
    CHECK(p.colorMid.initValue.a == 128);
    CHECK(p.colorEnd.initValue.b == 255);
    CHECK(p.colorEnd.initValue.a == 0);
    CHECK(p.colorMidTime == Catch::Approx(0.3f));
    CHECK(p.alphaMidTime == Catch::Approx(0.3f));
    // Rows 14, 17-18.
    CHECK(p.instanceType == m3::ParticleInstanceType::Billboard);
    CHECK(p.flipbookColumns == 4);
    CHECK(p.flipbookRows == 2);
    CHECK(p.flipbookColumnFraction == Catch::Approx(0.25f));
    CHECK(p.flipbookRowFraction == Catch::Approx(0.5f));
    CHECK(p.flipbookStartInitIndex == 0);
    CHECK(p.flipbookStartStopIndex == 4); // the decay's first cell (E2)
    CHECK(p.flipbookEndInitIndex == 7);
    CHECK(p.flipbookMidTime == Catch::Approx(0.3f));
    // Rows 23, 25: unshaded, so no lit bit; sorted, so the Euler step's inputs.
    CHECK_FALSE(hasFlag(p.flags, m3::ParticleFlag::LitParts));
    CHECK(hasFlag(p.flags, m3::ParticleFlag::Sort));
    CHECK(hasFlag(p.flags, m3::ParticleFlag::UseVertexAlpha));
    CHECK(p.mass == Catch::Approx(1.0f));
    CHECK(p.drag == Catch::Approx(0.0f));
    // A written AnimRef rests where a sequence that does not key it reads.
    CHECK(p.initialSpeed.nullValue == Catch::Approx(p.initialSpeed.initValue));
    CHECK(p.sizeAnimation.nullValue.y == Catch::Approx(p.sizeAnimation.initValue.y));
    CHECK(p.colorMid.nullValue.a == p.colorMid.initValue.a);
    // But not the emission pair: an unkeyed rate and squirt at their nulls read
    // as an emitter that emits nothing of its own, which the runtime retires.
    CHECK(p.emissionRate.nullValue == 0.0f);
    CHECK(p.squirtAmount.nullValue == 0);

    // Rows 20-21, 23-24, 26: the material.
    const m3::StandardMaterial& mat = materialOf(out.model, p.materialIndex);
    CHECK(mat.blendMode == m3::BlendMode::AlphaAdd);
    CHECK(hasFlag(mat.flags, m3::MaterialFlag::Unshaded));
    CHECK(hasFlag(mat.flags, m3::MaterialFlag::GeometryVisible));
    CHECK_FALSE(hasFlag(mat.flags, m3::MaterialFlag::NormalBlend));
    CHECK(mat.priority == 3);
    REQUIRE(mat.diffuseLayer.has_value());
    CHECK(mat.diffuseLayer->texturePath.rfind("Assets/Textures/smoke.dds", 0) == 0);
    CHECK(mat.diffuseLayer->uvMapping == m3::UVMappingMode::ParticleFlipbook);
    CHECK(hasFlag(mat.diffuseLayer->flags, m3::TextureLayerFlag::ParticleUVFlipbook));
    REQUIRE(mat.alphaLayer1.has_value());
    CHECK(mat.alphaLayer1->colorType == m3::ColorChannelSelect::Alpha);
    CHECK(mat.diffuseLayer->rgbMultiply.initValue == Catch::Approx(1.0f));
}

TEST_CASE("wem m3 an effect layer addresses its texture the way the source's TEXS word says",
          "[wem][m3][particles]") {
    // Warcraft III reads the address mode off the texture alone and a clear
    // bit is a clamp; a geoset layer already crosses that way, and an effect
    // drawing the same sheet must not tile it.
    mdx::Model model = makeModel();
    mdx::Texture tiled;
    tiled.fileName = "textures/tiled.blp";
    tiled.flags = mdx::Texture::Flag::WrapWidth;
    model.textures.push_back(tiled);
    mdx::ParticleEmitter2 clamped = makeEmitter("Clamped", 1, 0);
    clamped.textureId = 0;
    model.particleEmitters2.push_back(clamped);
    mdx::ParticleEmitter2 wraps = makeEmitter("Wraps", 2, 0);
    wraps.textureId = 1;
    model.particleEmitters2.push_back(wraps);
    mdx::Material ribbonMaterial;
    mdx::Layer ribbonLayer;
    ribbonLayer.textureId = 1;
    ribbonLayer.textureAnimationId = 0xFFFFFFFF;
    ribbonMaterial.layers.push_back(ribbonLayer);
    model.materials.push_back(ribbonMaterial);
    mdx::RibbonEmitter ribbon;
    ribbon.node = makeNode("Trail", 3, 0);
    ribbon.materialId = 1;
    ribbon.lifespan = 1.0f;
    ribbon.emissionRate = 10;
    ribbon.heightAbove = 10.0f;
    ribbon.heightBelow = 10.0f;
    ribbon.alpha = 1.0f;
    model.ribbonEmitters.push_back(ribbon);
    setPivot(model, 1, Vector3f{0, 0, 10});
    setPivot(model, 2, Vector3f{0, 0, 20});
    setPivot(model, 3, Vector3f{0, 0, 30});

    const Crossed out =
        crossAndRead(model, {"Assets/Textures/body.dds", "Assets/Textures/tiled.dds"});
    REQUIRE(out.model.particleEmitters.size() == 2);
    REQUIRE(out.model.ribbonEmitters.size() == 1);
    const auto wrapOf = [&out](u32 materialIndex) {
        const m3::StandardMaterial& mat = materialOf(out.model, materialIndex);
        REQUIRE(mat.diffuseLayer.has_value());
        return static_cast<u32>(mat.diffuseLayer->flags) &
               static_cast<u32>(m3::TextureLayerFlag::UVWrapX | m3::TextureLayerFlag::UVWrapY);
    };
    CHECK(wrapOf(out.model.particleEmitters[0].materialIndex) == 0u);
    CHECK(wrapOf(out.model.particleEmitters[1].materialIndex) ==
          static_cast<u32>(m3::TextureLayerFlag::UVWrapX));
    CHECK(wrapOf(out.model.ribbonEmitters[0].materialIndex) ==
          static_cast<u32>(m3::TextureLayerFlag::UVWrapX));
    // Two emitters on one texture share a material; a different word does not.
    CHECK(out.model.particleEmitters[0].materialIndex != out.model.particleEmitters[1].materialIndex);
}

TEST_CASE("wem m3 an emitter's keyed properties are the streams of the clips that play them",
          "[wem][m3][particles]") {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 pe = makeEmitter("Smoke", 1, 0);
    pe.variation = 0.5f;
    pe.emissionRateTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 1000}, std::vector<f32>{0.0f, 40.0f});
    pe.widthTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 1000}, std::vector<f32>{0.0f, 100.0f});
    pe.lengthTracks = makeTrack(mdx::InterpolationType::Linear, {500}, std::vector<f32>{50.0f});
    pe.speedTracks =
        makeTrack(mdx::InterpolationType::Linear, {2000, 3000}, std::vector<f32>{100.0f, 300.0f});
    model.particleEmitters2.push_back(pe);
    setPivot(model, 1, Vector3f{0, 0, 0});
    const Crossed out = crossAndRead(model);
    REQUIRE(out.model.particleEmitters.size() == 1);
    const m3::ParticleEmitter& p = out.model.particleEmitters[0];

    const auto rate = keysOf(out.model, "Stand", p.emissionRate, &m3::SubTrackContainer::sdr3, 5);
    REQUIRE(rate.size() == 2);
    CHECK(rate[1].first == 1000);
    CHECK(rate[1].second == Catch::Approx(40.0f));
    // A rate no key raised past its maximum would thin the effect.
    CHECK(p.maxParticles == static_cast<u32>(std::ceil(40.0f * 1.0f * 1.15f)) + 16u);

    // Width and length key apart; the plane keys once, on the union.
    const auto outer = keysOf(out.model, "Stand", p.shapeOuter, &m3::SubTrackContainer::sd3v, 2);
    REQUIRE(outer.size() == 3);
    CHECK(outer[1].first == 500);
    CHECK(outer[1].second.x == Catch::Approx(0.5f));
    CHECK(outer[1].second.y == Catch::Approx(0.5f));

    // Speed keys only Walk: its two endpoints are two streams of one track.
    CHECK(keysOf(out.model, "Stand", p.initialSpeed, &m3::SubTrackContainer::sdr3, 5).empty());
    const auto low = keysOf(out.model, "Walk", p.initialSpeed, &m3::SubTrackContainer::sdr3, 5);
    const auto high =
        keysOf(out.model, "Walk", p.initialSpeedRandom, &m3::SubTrackContainer::sdr3, 5);
    REQUIRE(low.size() == 2);
    REQUIRE(high.size() == 2);
    CHECK(low[1].second == Catch::Approx(1.5f));  // 300 * 0.5 * 0.01
    CHECK(high[1].second == Catch::Approx(4.5f)); // 300 * 1.5 * 0.01
    CHECK(p.initialSpeed.animId != p.initialSpeedRandom.animId);
    CHECK(p.initialSpeed.flags == 0x6);
}

TEST_CASE("wem m3 a squirt bursts on each rising edge of its rate and keeps no visibility",
          "[wem][m3][particles]") {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 pe = makeEmitter("Burst", 1, 0);
    pe.squirt = 1;
    pe.emissionRate = 0.0f;
    pe.emissionRateTracks = makeTrack(mdx::InterpolationType::None, {0, 200, 400, 600, 800},
                                      std::vector<f32>{0.0f, 20.0f, 0.0f, 30.5f, 35.0f});
    pe.visibilityTracks =
        makeTrack(mdx::InterpolationType::None, {0, 500}, std::vector<f32>{1.0f, 0.0f});
    model.particleEmitters2.push_back(pe);
    setPivot(model, 1, Vector3f{0, 0, 0});
    const Crossed out = crossAndRead(model);
    REQUIRE(out.model.particleEmitters.size() == 1);
    const m3::ParticleEmitter& p = out.model.particleEmitters[0];

    CHECK(p.emissionRate.initValue == Catch::Approx(0.0f));
    CHECK_FALSE(p.emissionRate.isAnimated());
    REQUIRE(p.squirtAmount.isAnimated());
    const auto bursts = keysOf(out.model, "Stand", p.squirtAmount, &m3::SubTrackContainer::sds6, 7);
    // 200 rises, 600 rises; 800 is still up and fires nothing.
    REQUIRE(bursts.size() == 2);
    CHECK(bursts[0].first == 200);
    CHECK(bursts[0].second == 20);
    CHECK(bursts[1].first == 600);
    CHECK(bursts[1].second == 30);
    CHECK(p.maxParticles == 30u + 16u);
    // §A9: its bursts ignore the track in Warcraft III, so no bone keys it.
    CHECK_FALSE(out.model.bones.at(p.boneIndex).visibility.isAnimated());
}

TEST_CASE("wem m3 a head and tail emitter is two records on one bone", "[wem][m3][particles]") {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 pe = makeEmitter("Streak", 1, 0);
    pe.headOrTail = 2;
    pe.tailLength = 0.5f;
    pe.segmentScaling = {10.0f, 25.0f, 10.0f};
    pe.node.flags = static_cast<mdx::Node::NodeFlag>(0x20000 | 0x80000); // line, model space
    pe.latitude = 40.0f;
    model.particleEmitters2.push_back(pe);
    setPivot(model, 1, Vector3f{0, 0, 0});
    const Crossed out = crossAndRead(model);
    REQUIRE(out.model.particleEmitters.size() == 2);
    const m3::ParticleEmitter& head = out.model.particleEmitters[0];
    const m3::ParticleEmitter& tail = out.model.particleEmitters[1];
    CHECK(head.boneIndex == tail.boneIndex);
    CHECK(head.instanceType == m3::ParticleInstanceType::Billboard);
    CHECK(tail.instanceType == m3::ParticleInstanceType::Trail);
    // The tail's length over the key it is measured against: T / (2 s L).
    CHECK(tail.tailLength == Catch::Approx(0.5f / (2.0f * 25.0f * 0.01f)));
    // A line emitter's fan: all the latitude one way, none the other.
    CHECK(head.initialHorizontal.initValue == Catch::Approx(40.0f * 3.14159265f / 180.0f));
    CHECK(head.initialVertical.initValue == Catch::Approx(0.0f));
    CHECK_FALSE(hasFlag(head.additionalFlags, m3::ParticleAdditionalFlag::WorldSpace));
    CHECK(out.report.particleRecords == 2u);
}

TEST_CASE("wem m3 a team-glow emitter draws the glow in the team's colour",
          "[wem][m3][particles]") {
    mdx::Model model = makeModel();
    mdx::Texture dust;
    dust.fileName = "textures/dust3.blp";
    model.textures.push_back(dust);
    mdx::ParticleEmitter2 pe = makeEmitter("Glow", 1, 0);
    pe.textureId = 1;
    pe.replaceableId = 2;
    pe.filterMode = 1; // additive
    model.particleEmitters2.push_back(pe);
    setPivot(model, 1, Vector3f{0, 0, 0});
    const std::vector<std::string> paths{"Assets/Textures/body.dds", "Assets/Textures/Dust3.dds"};
    const auto path = [](const std::optional<m3::TextureLayer>& layer) {
        std::string p = layer.has_value() ? layer->texturePath : std::string();
        while (!p.empty() && p.back() == '\0') {
            p.pop_back();
        }
        return p;
    };

    // The team's colour through the texture's alpha on the emissive, and the
    // same alpha as the mask the blend reads; no diffuse under it.
    const Crossed out = crossAndRead(model, paths);
    REQUIRE(out.model.particleEmitters.size() == 1);
    const m3::StandardMaterial& mat =
        materialOf(out.model, out.model.particleEmitters[0].materialIndex);
    CHECK(path(mat.diffuseLayer).empty());
    CHECK(path(mat.emissiveLayer1) == "Assets/Textures/Dust3.dds");
    REQUIRE(mat.emissiveLayer1.has_value());
    CHECK(mat.emissiveLayer1->colorType == m3::ColorChannelSelect::Alpha);
    CHECK(mat.emissiveBlendMode1 == m3::LayerBlendOp::TeamColorEmissiveAdd);
    CHECK(path(mat.alphaLayer1) == "Assets/Textures/Dust3.dds");
    REQUIRE(mat.alphaLayer1.has_value());
    CHECK(mat.alphaLayer1->colorType == m3::ColorChannelSelect::Alpha);
    CHECK(mat.blendMode == m3::BlendMode::AlphaAdd);

    // Given the glow's mask, the mask weights the team colour and is no
    // coverage: Warcraft III draws the glow with a flat alpha.
    const Crossed masked = crossAndRead(model, paths, "Assets/Textures/TeamGlowMask.dds");
    const m3::StandardMaterial& glow =
        materialOf(masked.model, masked.model.particleEmitters[0].materialIndex);
    CHECK(path(glow.emissiveLayer1) == "Assets/Textures/TeamGlowMask.dds");
    REQUIRE(glow.emissiveLayer1.has_value());
    CHECK(glow.emissiveLayer1->colorType == m3::ColorChannelSelect::Alpha);
    CHECK(glow.emissiveBlendMode1 == m3::LayerBlendOp::TeamColorEmissiveAdd);
    CHECK(path(glow.alphaLayer1).empty());
    CHECK(path(glow.diffuseLayer).empty());

    // A plain emitter on the same texture keeps its diffuse, on a material of
    // its own.
    model.particleEmitters2[0].replaceableId = 0;
    const Crossed plainOut = crossAndRead(model, paths);
    const m3::StandardMaterial& plain =
        materialOf(plainOut.model, plainOut.model.particleEmitters[0].materialIndex);
    CHECK(path(plain.diffuseLayer) == "Assets/Textures/Dust3.dds");
    CHECK(plain.emissiveBlendMode1 != m3::LayerBlendOp::TeamColorEmissiveAdd);
}

TEST_CASE("wem m3 each Warcraft III particle filter is its blend, and a keyed gravity says so",
          "[wem][m3][particles]") {
    // Row 20 over every filter, Modulate2x included (no shipped emitter uses
    // it), and row 6's diagnostic.
    const std::vector<std::pair<u32, m3::BlendMode>> filters = {
        {0, m3::BlendMode::AlphaBlend}, {1, m3::BlendMode::AlphaAdd}, {2, m3::BlendMode::Mod},
        {3, m3::BlendMode::Mod2x},      {4, m3::BlendMode::Opaque},
    };
    mdx::Model model = makeModel();
    for (u32 i = 0; i < filters.size(); ++i) {
        mdx::ParticleEmitter2 pe = makeEmitter("Filter" + std::to_string(i), i + 1, 0);
        pe.filterMode = filters[i].first;
        if (i == 0) {
            pe.gravityTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                         std::vector<f32>{0.0f, 100.0f});
        }
        model.particleEmitters2.push_back(pe);
        setPivot(model, i + 1, Vector3f{0, 0, 0});
    }
    const Crossed out = crossAndRead(model, {"Assets/Textures/body.dds"});
    REQUIRE(out.model.particleEmitters.size() == filters.size());
    for (u32 i = 0; i < filters.size(); ++i) {
        INFO("filter " << filters[i].first);
        const m3::StandardMaterial& mat =
            materialOf(out.model, out.model.particleEmitters[i].materialIndex);
        CHECK(mat.blendMode == filters[i].second);
        // Modulate multiplies the colour alone; the alpha-weighted filters
        // read the texture's alpha through a mask.
        const bool masked = filters[i].first == 0 || filters[i].first == 1 ||
                            filters[i].first == 4;
        CHECK((mat.alphaLayer1.has_value() && !mat.alphaLayer1->texturePath.empty()) == masked);
    }
    const m3::StandardMaterial& key =
        materialOf(out.model, out.model.particleEmitters[4].materialIndex);
    CHECK(key.alphaTestThreshold == 192);
    CHECK(out.report.diagnostics.countOf(DiagCode::AnimTrackDropped) >= 1u);
}

TEST_CASE("wem m3 a model-space emitter still rides the bone it was linked to",
          "[wem][m3][particles]") {
    mdx::Model model = makeModel();
    mdx::Bone hip;
    hip.node = makeNode("hip", 1, 0);
    hip.node.translationTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                           std::vector<Vector3f>{{0, 0, 0}, {0, 0, 10}});
    model.bones.push_back(hip);
    setPivot(model, 1, Vector3f{0, 0, 70});
    mdx::ParticleEmitter2 pe = makeEmitter("Spark", 2, 1);
    pe.node.flags = static_cast<mdx::Node::NodeFlag>(0x80000); // model space
    model.particleEmitters2.push_back(pe);
    setPivot(model, 2, Vector3f{0, 0, 69});

    // As the import states the bit -- the node's local is its world -- `toM3`
    // keeps the file consistent with its own IREF: a root.
    const Written asImported = writeAndRead(stage(model), true);
    u32 spark = boneNamed(asImported.model, "Spark");
    REQUIRE(spark != kInvalidIndex);
    CHECK(asImported.model.bones[spark].parentIndex == 0xFFFFu);
    CHECK(asImported.model.bones[spark].position.initValue.z == Catch::Approx(0.69f));

    // Prepared for the crossing, it composes onto its parent the way Warcraft
    // III's hierarchy does, and its particles still live in its space.
    const Crossed out = crossAndRead(model);
    spark = boneNamed(out.model, "Spark");
    const u32 parent = boneNamed(out.model, "hip");
    REQUIRE(spark != kInvalidIndex);
    REQUIRE(parent != kInvalidIndex);
    CHECK(out.model.bones[spark].parentIndex == parent);
    CHECK(out.model.bones[spark].position.initValue.z == Catch::Approx(-0.01f).margin(1e-5));
    const Matrix44f bind = Matrix44f::inverse(out.model.initialReference.at(spark).matrix);
    CHECK(bind.data[3][2] == Catch::Approx(0.69f));
    REQUIRE(out.model.particleEmitters.size() == 1);
    CHECK(out.model.particleEmitters[0].boneIndex == spark);
    CHECK_FALSE(hasFlag(out.model.particleEmitters[0].additionalFlags,
                        m3::ParticleAdditionalFlag::WorldSpace));
}

TEST_CASE("wem m3 a model-space XY quad lies in its node's plane, spun at random",
          "[wem][m3][particles]") {
    // Warcraft III lays an XY quad in the plane of the frame its particle lives
    // in: the node's for a model-space emitter, the ground for a world-space one.
    // It turns the quad by the velocity's XY and faces the camera while that is
    // under 1e-3, so a slow, narrow cone draws a share of billboards.
    mdx::Model model = makeModel();
    const auto quads = [&](const std::string& name, u32 objectId, u32 flags, f32 latitude) {
        mdx::ParticleEmitter2 pe = makeEmitter(name, objectId, 0);
        pe.node.flags = static_cast<mdx::Node::NodeFlag>(flags);
        pe.latitude = latitude;
        model.particleEmitters2.push_back(pe);
    };
    quads("Rays", 1, 0x80000 | 0x100000, 30.0f);
    quads("Ground", 2, 0x100000, 30.0f);
    quads("Fan", 3, 0x80000 | 0x100000 | 0x20000, 30.0f);
    quads("Crown", 4, 0x80000 | 0x100000, 0.1f);
    model.particleEmitters2.back().speed = 2.0f;
    model.particleEmitters2.back().variation = 0.001f;
    quads("Still", 5, 0x80000 | 0x100000, 0.0f);
    setPivot(model, 5, Vector3f{0, 0, 0});

    const Crossed out = crossAndRead(model);
    REQUIRE(out.model.particleEmitters.size() == 6u);
    const m3::ParticleEmitter& rays = out.model.particleEmitters[0];
    CHECK(rays.instanceType == m3::ParticleInstanceType::EmitterOriented);
    CHECK_FALSE(hasFlag(rays.additionalFlags, m3::ParticleAdditionalFlag::WorldSpace));
    CHECK(rays.rotationRandomEnable == 1u);
    CHECK(rays.rotationAnimation.initValue.x == Catch::Approx(0.0f));
    CHECK(rays.rotationRandomAnimation.initValue.x == Catch::Approx(6.2831853f));
    CHECK(rays.emissionRate.initValue == Catch::Approx(10.0f));

    const m3::ParticleEmitter& flat = out.model.particleEmitters[1];
    CHECK(flat.instanceType == m3::ParticleInstanceType::TerrainDirOriented);
    CHECK(hasFlag(flat.additionalFlags, m3::ParticleAdditionalFlag::WorldSpace));
    CHECK(flat.rotationRandomEnable == 0u);

    // A line emitter's azimuth is one axis, so its quads keep the bone's.
    const m3::ParticleEmitter& fan = out.model.particleEmitters[2];
    CHECK(fan.instanceType == m3::ParticleInstanceType::EmitterOriented);
    CHECK(fan.rotationRandomEnable == 0u);

    // Speed 2 across 0.1 degrees keeps asin(5e-4) / 0.1 degrees = 28.6% of the
    // quads under the turning speed: two records on one bone, the rate split.
    // The second is a node of its own, so it follows the source's nodes.
    const m3::ParticleEmitter& turned = out.model.particleEmitters[3];
    const m3::ParticleEmitter& facing = out.model.particleEmitters[5];
    CHECK(turned.instanceType == m3::ParticleInstanceType::EmitterOriented);
    CHECK(facing.instanceType == m3::ParticleInstanceType::Billboard);
    CHECK(facing.boneIndex == turned.boneIndex);
    CHECK(facing.rotationRandomEnable == 0u);
    CHECK(facing.emissionRate.initValue == Catch::Approx(2.8648f).epsilon(1e-3));
    CHECK(turned.emissionRate.initValue + facing.emissionRate.initValue ==
          Catch::Approx(10.0f));
    CHECK(facing.emissionRate.nullValue == 0.0f);

    // No latitude, no XY: every quad faces the camera.
    const m3::ParticleEmitter& still = out.model.particleEmitters[4];
    CHECK(still.instanceType == m3::ParticleInstanceType::Billboard);
}

// ============================================================================
// C2.3-C2.4 -- geoset tints and static alpha
// ============================================================================

namespace {

/// `makeModel` with a second geoset on material @p material and a geoset
/// animation on geoset 0.
mdx::Model tintedModel(const mdx::GeosetAnimation& animation, u32 secondMaterial) {
    mdx::Model model = makeModel();
    mdx::Material other;
    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::None;
    layer.textureId = 0;
    layer.textureAnimationId = 0xFFFFFFFF;
    other.layers.push_back(layer);
    model.materials.push_back(other);
    mdx::Geoset second = model.geosets[0];
    second.lodName = "second";
    second.materialId = secondMaterial;
    model.geosets.push_back(second);
    model.geosetAnimations.push_back(animation);
    return model;
}

} // namespace

TEST_CASE("wem m3 a Warcraft III geoset tint is a Mod emissive carrier", "[wem][m3][tint]") {
    mdx::GeosetAnimation animation;
    animation.geosetId = 0;
    animation.flags = mdx::GeosetAnimation::Flag::Color;
    animation.color = Vector3f{1.0f, 0.5f, 0.0f}; // red first, as stored
    animation.alpha = 0.5f;
    const mdx::Model model = tintedModel(animation, 1);
    const Written on = writeAndRead(stage(model), false);

    REQUIRE(on.model.standardMaterials.size() >= 2);
    const m3::StandardMaterial& tinted = on.model.standardMaterials[0];
    REQUIRE(tinted.emissiveLayer1.has_value());
    CHECK(hasFlag(tinted.emissiveLayer1->flags, m3::TextureLayerFlag::Color));
    CHECK(tinted.emissiveBlendMode1 == m3::LayerBlendOp::Mod);
    CHECK(tinted.emissiveLayer1->color.initValue.r == 255);
    CHECK(tinted.emissiveLayer1->color.initValue.g == 128);
    CHECK(tinted.emissiveLayer1->color.initValue.b == 0);
    // The static half alpha rides an alpha carrier.
    REQUIRE(tinted.alphaLayer1.has_value());
    CHECK(hasFlag(tinted.alphaLayer1->flags, m3::TextureLayerFlag::Color));
    CHECK(tinted.alphaLayer1->mapAlpha.initValue == Catch::Approx(0.5f));
    // Canonical nulls: the engine writes a still layer constant only when its
    // init differs from its null, so a restated null left both unwritten.
    CHECK(tinted.emissiveLayer1->color.nullValue.r == 0);
    CHECK(tinted.emissiveLayer1->color.nullValue.g == 0);
    CHECK(tinted.emissiveLayer1->color.nullValue.a == 0);
    CHECK(tinted.alphaLayer1->mapAlpha.nullValue == Catch::Approx(1.0f));
    // The other geoset's own material is untouched.
    const m3::StandardMaterial& plain = on.model.standardMaterials[1];
    CHECK_FALSE((plain.emissiveLayer1.has_value() &&
                 hasFlag(plain.emissiveLayer1->flags, m3::TextureLayerFlag::Color)));

    // Another profile's document writes none of it.
    Document other = stage(model);
    other.defaultProfile = ProfileId::Wow;
    const Written wow = writeAndRead(other, false);
    CHECK_FALSE((wow.model.standardMaterials[0].emissiveLayer1.has_value() &&
                 hasFlag(wow.model.standardMaterials[0].emissiveLayer1->flags,
                         m3::TextureLayerFlag::Color)));
}

TEST_CASE("wem m3 a second fade on one material rides its texture mask's map alpha",
          "[wem][m3][fade]") {
    // TinkerRocketMissile's shockwave: an additive layer, so its texture's
    // alpha is the first mask, keyed by a layer fade (KMTA) and a geoset fade
    // (KGAO). The geoset fade takes the free second slot as a Color carrier;
    // the layer fade then finds the mask's own map alpha, which multiplies the
    // sample in the game's layer shader.
    mdx::Model model = makeModel();
    model.materials[0].layers[0].filterMode = mdx::Layer::FilterMode::Additive;
    model.materials[0].layers[0].alphaTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 500}, std::vector<f32>{1.0f, 0.0f});
    mdx::GeosetAnimation animation;
    animation.geosetId = 0;
    animation.alpha = 1.0f;
    animation.alphaTracks = makeTrack(mdx::InterpolationType::Linear, {0, 250, 1000},
                                      std::vector<f32>{0.2f, 0.9f, 0.4f});
    model.geosetAnimations.push_back(animation);

    M3Converter converter;
    M3ExportSettings settings;
    settings.effectNodeBones = true;
    M3ExportMap map;
    Result<m3::Model> converted = converter.toM3(stage(model), ProfileId::Sc2, 29, settings, &map);
    REQUIRE(converted.ok());
    CHECK(converted.diagnostics.countOf(DiagCode::AnimTrackDropped) == 0u);
    m3::Writer writer;
    const std::vector<u8> bytes = writer.write(*converted);
    m3::Parser parser;
    const m3::Model out = parser.parse(std::span<const u8>(bytes));

    REQUIRE_FALSE(out.standardMaterials.empty());
    const m3::StandardMaterial& mat = out.standardMaterials[0];
    REQUIRE(mat.alphaLayer1.has_value());
    REQUIRE(mat.alphaLayer2.has_value());
    CHECK_FALSE(mat.alphaLayer1->texturePath.empty());
    CHECK(hasFlag(mat.alphaLayer2->flags, m3::TextureLayerFlag::Color));
    const auto mask = keysOf(out, "Stand", mat.alphaLayer1->mapAlpha, &m3::SubTrackContainer::sdr3, 5);
    const auto carrier =
        keysOf(out, "Stand", mat.alphaLayer2->mapAlpha, &m3::SubTrackContainer::sdr3, 5);
    // The layer fade's two keys (the sink adds its wrap key at the sequence end).
    REQUIRE(mask.size() >= 2u);
    CHECK(mask[0].second == Catch::Approx(1.0f));
    CHECK(mask[1].first == 500);
    CHECK(mask[1].second == Catch::Approx(0.0f));
    REQUIRE(carrier.size() >= 3u);
    CHECK(carrier[1].first == 250);
    CHECK(carrier[1].second == Catch::Approx(0.9f));
    // The mask still reads its texture's alpha and rests where the fade does.
    CHECK(mat.alphaLayer1->mapAlpha.initValue == Catch::Approx(1.0f));
    CHECK(mat.alphaLayer1->mapAlpha.nullValue == Catch::Approx(1.0f));
}

TEST_CASE("wem m3 a scrolled blended layer's texture mask scrolls with its colour",
          "[wem][m3][uv]") {
    // ChineseFairy's slashes: a Blend layer scrolls a clamped trail texture
    // across its mesh. The blend reads the mask the fold copies from the
    // diffuse, so a mask left where it rests drew no slash at all.
    mdx::Model model = makeModel();
    model.materials[0].layers[0].filterMode = mdx::Layer::FilterMode::Blend;
    model.materials[0].layers[0].textureAnimationId = 0;
    mdx::TextureAnimation scroll;
    scroll.translationTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                         std::vector<Vector3f>{{0, 0, 0}, {0, 2.5f, 20.0f}});
    model.textureAnimations.push_back(scroll);
    const Written on = writeAndRead(stage(model), false);

    REQUIRE_FALSE(on.model.standardMaterials.empty());
    const m3::StandardMaterial& mat = on.model.standardMaterials[0];
    REQUIRE(mat.diffuseLayer.has_value());
    REQUIRE(mat.alphaLayer1.has_value());
    CHECK(mat.alphaLayer1->colorType == m3::ColorChannelSelect::Alpha);
    // The same keys under an id of its own (`m3_sink::UnshareAnimIds`).
    CHECK(mat.alphaLayer1->uvOffset.animId != mat.diffuseLayer->uvOffset.animId);
    const auto colour =
        keysOf(on.model, "Stand", mat.diffuseLayer->uvOffset, &m3::SubTrackContainer::sd2v, 1);
    const auto mask =
        keysOf(on.model, "Stand", mat.alphaLayer1->uvOffset, &m3::SubTrackContainer::sd2v, 1);
    REQUIRE(colour.size() >= 2u);
    REQUIRE(mask.size() == colour.size());
    for (std::size_t k = 0; k < colour.size(); ++k) {
        CHECK(mask[k].first == colour[k].first);
        CHECK(mask[k].second.y == Catch::Approx(colour[k].second.y));
    }
    CHECK(mask.back().second.y == Catch::Approx(-2.5f));
    // Its own alpha stays the texture's: no fade crossed onto it.
    CHECK_FALSE(mat.alphaLayer1->mapAlpha.isAnimated());
}

namespace {

/// Every id a container drives that more than one AnimRef names.
std::vector<u32> sharedDrivenIds(m3::Model model) {
    std::set<u32> driven;
    for (const m3::SubTrackContainer& stc : model.subTrackCollections) {
        driven.insert(stc.animIds.begin(), stc.animIds.end());
    }
    std::vector<u32> readers;
    for (const u32* id : m3_sink::KeyableAnimIds(model)) {
        if (driven.count(*id) != 0) {
            readers.push_back(*id);
        }
    }
    std::sort(readers.begin(), readers.end());
    std::vector<u32> shared;
    for (std::size_t k = 1; k < readers.size(); ++k) {
        if (readers[k] == readers[k - 1] && (shared.empty() || shared.back() != readers[k])) {
            shared.push_back(readers[k]);
        }
    }
    return shared;
}

} // namespace

TEST_CASE("wem m3 every AnimRef a track drives names an id of its own", "[wem][m3][anim]") {
    // A fold's mask copy reads its diffuse's scroll, and an emitter's head and
    // tail records read one speed stream. No shipped file lets two AnimRefs name
    // one driven id (the game reads a ref's track through the row its load
    // binds), so each takes an id of its own over a copy of the same keys.
    mdx::Model model = makeModel();
    model.materials[0].layers[0].filterMode = mdx::Layer::FilterMode::Blend;
    model.materials[0].layers[0].textureAnimationId = 0;
    mdx::TextureAnimation scroll;
    scroll.translationTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                         std::vector<Vector3f>{{0, 0, 0}, {0, 2.5f, 20.0f}});
    model.textureAnimations.push_back(scroll);
    mdx::ParticleEmitter2 pe = makeEmitter("Smoke", 1, 0);
    pe.headOrTail = 2;
    pe.speedTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 1000}, std::vector<f32>{100.0f, 300.0f});
    model.particleEmitters2.push_back(pe);
    setPivot(model, 1, Vector3f{0, 0, 0});
    const Crossed out = crossAndRead(model);

    CHECK(sharedDrivenIds(out.model).empty());

    const m3::StandardMaterial& mat = out.model.standardMaterials.at(0);
    REQUIRE(mat.diffuseLayer.has_value());
    REQUIRE(mat.alphaLayer1.has_value());
    CHECK(mat.alphaLayer1->uvOffset.animId != mat.diffuseLayer->uvOffset.animId);

    REQUIRE(out.model.particleEmitters.size() == 2u);
    const m3::ParticleEmitter& head = out.model.particleEmitters[0];
    const m3::ParticleEmitter& tail = out.model.particleEmitters[1];
    CHECK(head.initialSpeed.animId != tail.initialSpeed.animId);
    const auto headKeys =
        keysOf(out.model, "Stand", head.initialSpeed, &m3::SubTrackContainer::sdr3, 5);
    const auto tailKeys =
        keysOf(out.model, "Stand", tail.initialSpeed, &m3::SubTrackContainer::sdr3, 5);
    REQUIRE(headKeys.size() == 2u);
    REQUIRE(tailKeys.size() == headKeys.size());
    for (std::size_t k = 0; k < headKeys.size(); ++k) {
        CHECK(tailKeys[k].first == headKeys[k].first);
        CHECK(tailKeys[k].second == Catch::Approx(headKeys[k].second));
    }
}

TEST_CASE("wem m3 a keyed geoset colour reads red first in the carrier's stream",
          "[wem][m3][tint]") {
    mdx::GeosetAnimation animation;
    animation.geosetId = 0;
    animation.flags = mdx::GeosetAnimation::Flag::Color;
    // KGAC is stored blue first: (0.2, 0.4, 0.8) is (r 0.8, g 0.4, b 0.2).
    animation.colorTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                      std::vector<Vector3f>{{0.2f, 0.4f, 0.8f}, {1, 1, 1}});
    const mdx::Model model = tintedModel(animation, 1);
    const Written on = writeAndRead(stage(model), false);

    const m3::StandardMaterial& tinted = on.model.standardMaterials[0];
    REQUIRE(tinted.emissiveLayer1.has_value());
    const m3::AnimRef<m3::ColorBGRA>& color = tinted.emissiveLayer1->color;
    REQUIRE(color.animId != 0u);
    const u32 stand = sequenceNamed(on.model, "Stand");
    const u32 stc = on.model.animationGroups.at(stand).subtrackIndices.at(0);
    const u32 ref = refFor(on.model, stc, color.animId);
    REQUIRE(ref != kInvalidIndex);
    CHECK((ref >> 16) == 4u);
    const auto& block = on.model.subTrackCollections[stc].sdcc.at(ref & 0xFFFF);
    REQUIRE(!block.keys.empty());
    CHECK(block.keys[0].r == 204);
    CHECK(block.keys[0].g == 102);
    CHECK(block.keys[0].b == 51);
    CHECK(block.keys[0].a == 255);

    // And the `.mdx` round trip puts it back blue first, bit for bit.
    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(model);
    REQUIRE(imported.ok());
    Result<mdx::Model> back = converter.toMdx(*imported.value, ProfileId::Wc3Classic);
    REQUIRE(back.ok());
    const mdx::GeosetAnimation* written = nullptr;
    for (const mdx::GeosetAnimation& a : back.value->geosetAnimations) {
        if (a.geosetId == 0) {
            written = &a;
        }
    }
    REQUIRE(written != nullptr);
    REQUIRE(written->colorTracks.keys_data.size() == 2);
    CHECK(written->colorTracks.keys_data[0].x == 0.2f);
    CHECK(written->colorTracks.keys_data[0].z == 0.8f);
}

TEST_CASE("wem mdx a keyed light and ribbon colour read red first and write back blue first",
          "[wem][mdx][tint]") {
    mdx::Model model = makeModel();
    mdx::Light light;
    light.node = makeNode("Sun", 1, 0);
    light.color = Vector3f{1, 1, 1};
    light.intensity = 1.0f;
    // Stored blue first, like every Warcraft III colour key: (0.2, 0.4, 0.8) is
    // r 0.8, g 0.4, b 0.2.
    light.colorTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                  std::vector<Vector3f>{{0.2f, 0.4f, 0.8f}, {1, 1, 1}});
    light.ambientColorTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                         std::vector<Vector3f>{{0.1f, 0.3f, 0.9f}, {1, 1, 1}});
    model.lights.push_back(light);
    setPivot(model, 1, Vector3f{0, 0, 0});
    mdx::RibbonEmitter rb;
    rb.node = makeNode("Trail", 2, 0);
    rb.heightAbove = 10.0f;
    rb.heightBelow = 10.0f;
    rb.lifespan = 0.5f;
    rb.emissionRate = 20;
    rb.color = Vector3f{1, 1, 1};
    rb.alpha = 1.0f;
    rb.colorTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                               std::vector<Vector3f>{{0.0f, 0.5f, 1.0f}, {1, 1, 1}});
    model.ribbonEmitters.push_back(rb);
    setPivot(model, 2, Vector3f{0, 0, 0});

    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(model);
    REQUIRE(imported.ok());
    const Document& document = *imported.value;
    const Model& wem = document.models.front();
    const auto firstKey = [&](u32 node, u32 sub) {
        for (const AnimChannel& channel : wem.animChannels.channels) {
            if (channel.target.kind != TrackTarget::Kind::Node || channel.target.node != node ||
                channel.target.channel != Channel::Color || channel.target.sub != sub) {
                continue;
            }
            for (const Clip& clip : document.clips) {
                for (const SubTrackContainer& container : clip.containers) {
                    for (const SubTrack& track : container.subTracks) {
                        if (track.channel == channel.id && track.values.size() >= 3 * sizeof(f32)) {
                            Vector3f key{};
                            std::memcpy(&key, track.values.data(), 3 * sizeof(f32));
                            return key;
                        }
                    }
                }
            }
        }
        return Vector3f{-1, -1, -1};
    };
    const Vector3f sun = firstKey(nodeWithObjectId(document, 1), 0);
    CHECK(sun.x == Catch::Approx(0.8f));
    CHECK(sun.z == Catch::Approx(0.2f));
    const Vector3f ambient = firstKey(nodeWithObjectId(document, 1), 1);
    CHECK(ambient.x == Catch::Approx(0.9f));
    CHECK(ambient.z == Catch::Approx(0.1f));
    const Vector3f trail = firstKey(nodeWithObjectId(document, 2), 0);
    CHECK(trail.x == Catch::Approx(1.0f));
    CHECK(trail.z == Catch::Approx(0.0f).margin(1e-6));

    // The round trip writes them back blue first, bit for bit.
    Result<mdx::Model> back = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(back.ok());
    REQUIRE(back.value->lights.size() == 1u);
    REQUIRE(back.value->lights[0].colorTracks.keys_data.size() == 2u);
    CHECK(back.value->lights[0].colorTracks.keys_data[0].x == 0.2f);
    CHECK(back.value->lights[0].colorTracks.keys_data[0].z == 0.8f);
    REQUIRE(back.value->lights[0].ambientColorTracks.keys_data.size() == 2u);
    CHECK(back.value->lights[0].ambientColorTracks.keys_data[0].x == 0.1f);
    REQUIRE(back.value->ribbonEmitters.size() == 1u);
    REQUIRE(back.value->ribbonEmitters[0].colorTracks.keys_data.size() == 2u);
    CHECK(back.value->ribbonEmitters[0].colorTracks.keys_data[0].x == 0.0f);
    CHECK(back.value->ribbonEmitters[0].colorTracks.keys_data[0].z == 1.0f);

    // And the crossed ribbon's colour stops key red first.
    const Crossed out = crossAndRead(model, {"Assets/Textures/body.dds"});
    REQUIRE(out.model.ribbonEmitters.size() == 1u);
    const m3::RibbonEmitter& r = out.model.ribbonEmitters[0];
    const u32 stand = sequenceNamed(out.model, "Stand");
    const u32 stc = out.model.animationGroups.at(stand).subtrackIndices.at(0);
    const u32 ref = refFor(out.model, stc, r.colorStart.animId);
    REQUIRE(ref != kInvalidIndex);
    const auto& block = out.model.subTrackCollections[stc].sdcc.at(ref & 0xFFFF);
    REQUIRE_FALSE(block.keys.empty());
    CHECK(block.keys[0].r == 255);
    CHECK(block.keys[0].b == 0);
}

TEST_CASE("wem mdx a static geoset tint and partial alpha survive the round trip",
          "[wem][mdx][tint]") {
    mdx::GeosetAnimation animation;
    animation.geosetId = 0;
    animation.flags = mdx::GeosetAnimation::Flag::Color;
    animation.color = Vector3f{0.25f, 0.5f, 0.75f};
    animation.alpha = 0.3f;
    const mdx::Model model = tintedModel(animation, 1);
    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(model);
    REQUIRE(imported.ok());
    Result<mdx::Model> back = converter.toMdx(*imported.value, ProfileId::Wc3Classic);
    REQUIRE(back.ok());
    const mdx::GeosetAnimation* written = nullptr;
    for (const mdx::GeosetAnimation& a : back.value->geosetAnimations) {
        if (a.geosetId == 0) {
            written = &a;
        }
    }
    REQUIRE(written != nullptr);
    CHECK(written->color.x == 0.25f);
    CHECK(written->color.y == 0.5f);
    CHECK(written->color.z == 0.75f);
    CHECK(written->alpha == 0.3f);
}

TEST_CASE("wem m3 a tint on a material another geoset draws untinted draws a copy of it",
          "[wem][m3][tint]") {
    mdx::GeosetAnimation animation;
    animation.geosetId = 0;
    animation.flags = mdx::GeosetAnimation::Flag::Color;
    animation.color = Vector3f{1.0f, 0.0f, 0.0f};
    const mdx::Model model = tintedModel(animation, 0); // both geosets on material 0
    M3Converter converter;
    const Document document = stage(model);
    Result<m3::Model> converted = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(converted.ok());
    CHECK(converted.diagnostics.countOf(DiagCode::FeatureDropped) == 0u);
    const m3::Model& out = *converted.value;
    REQUIRE(out.divisions.size() == 1u);
    REQUIRE(out.divisions[0].batches.size() == 2u);
    const auto standardOf = [&out](const m3::Batch& batch) -> const m3::StandardMaterial& {
        const m3::MaterialMap& map = out.materialMaps.at(batch.materialIndex);
        REQUIRE(map.materialType == m3::MaterialType::Standard);
        return out.standardMaterials.at(map.materialIndex);
    };
    const m3::Batch& first = out.divisions[0].batches[0];
    const m3::Batch& second = out.divisions[0].batches[1];
    // The tinted geoset keeps the slot's entry; the plain one draws a copy.
    CHECK(first.materialIndex == 0u);
    CHECK(second.materialIndex != first.materialIndex);
    const m3::StandardMaterial& tinted = standardOf(first);
    const m3::StandardMaterial& plain = standardOf(second);
    REQUIRE(tinted.emissiveLayer1.has_value());
    CHECK(hasFlag(tinted.emissiveLayer1->flags, m3::TextureLayerFlag::Color));
    CHECK(tinted.emissiveLayer1->color.initValue.r == 255);
    CHECK(tinted.emissiveLayer1->color.initValue.g == 0);
    CHECK_FALSE((plain.emissiveLayer1.has_value() &&
                 hasFlag(plain.emissiveLayer1->flags, m3::TextureLayerFlag::Color)));
    // The copy is the same material otherwise.
    REQUIRE(plain.diffuseLayer.has_value());
    REQUIRE(tinted.diffuseLayer.has_value());
    CHECK(plain.diffuseLayer->texturePath == tinted.diffuseLayer->texturePath);
    CHECK(plain.blendMode == tinted.blendMode);

    // Another profile's document copies nothing.
    Document other = stage(model);
    other.defaultProfile = ProfileId::Wow;
    Result<m3::Model> wow = converter.toM3(other, ProfileId::Sc2, 29);
    REQUIRE(wow.ok());
    CHECK(wow.value->materialMaps.size() == out.materialMaps.size() - 1u);

    // Tinting the second drawer instead: the plain geoset keeps the slot, and
    // the tint lands on the copy.
    animation.geosetId = 1;
    Result<m3::Model> late = converter.toM3(stage(tintedModel(animation, 0)), ProfileId::Sc2, 29);
    REQUIRE(late.ok());
    const m3::Model& lateOut = *late.value;
    REQUIRE(lateOut.divisions[0].batches.size() == 2u);
    const auto lateStandard = [&lateOut](u32 batch) -> const m3::StandardMaterial& {
        const m3::MaterialMap& map =
            lateOut.materialMaps.at(lateOut.divisions[0].batches.at(batch).materialIndex);
        return lateOut.standardMaterials.at(map.materialIndex);
    };
    CHECK(lateOut.divisions[0].batches[0].materialIndex == 0u);
    CHECK_FALSE((lateStandard(0).emissiveLayer1.has_value() &&
                 hasFlag(lateStandard(0).emissiveLayer1->flags, m3::TextureLayerFlag::Color)));
    REQUIRE(lateStandard(1).emissiveLayer1.has_value());
    CHECK(hasFlag(lateStandard(1).emissiveLayer1->flags, m3::TextureLayerFlag::Color));
}

TEST_CASE("wem m3 two geosets on one material fade on copies of their own",
          "[wem][m3][fade]") {
    // GryphonRider's body: one of the geosets drawing a material fades, the
    // other does not. A carrier on the shared material would fade both.
    mdx::Model model = makeModel();
    mdx::Geoset second = model.geosets[0];
    second.lodName = "second";
    model.geosets.push_back(second);
    mdx::Geoset third = model.geosets[0];
    third.lodName = "third";
    model.geosets.push_back(third);
    // Geosets 0 and 2 fade on different keys; geoset 1 does not fade.
    mdx::GeosetAnimation fadeA;
    fadeA.geosetId = 0;
    fadeA.alpha = 1.0f;
    fadeA.alphaTracks = makeTrack(mdx::InterpolationType::Linear, {0, 500},
                                  std::vector<f32>{1.0f, 0.25f});
    model.geosetAnimations.push_back(fadeA);
    mdx::GeosetAnimation fadeB = fadeA;
    fadeB.geosetId = 2;
    fadeB.alphaTracks = makeTrack(mdx::InterpolationType::Linear, {0, 800},
                                  std::vector<f32>{0.5f, 1.0f});
    model.geosetAnimations.push_back(fadeB);
    // The material's own layer fade reaches every copy.
    model.materials[0].layers[0].alphaTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 300}, std::vector<f32>{1.0f, 0.5f});

    M3Converter converter;
    Result<m3::Model> converted = converter.toM3(stage(model), ProfileId::Sc2, 29);
    REQUIRE(converted.ok());
    CHECK(converted.diagnostics.countOf(DiagCode::AnimTrackDropped) == 0u);
    m3::Writer writer;
    const std::vector<u8> bytes = writer.write(*converted);
    m3::Parser parser;
    const m3::Model out = parser.parse(std::span<const u8>(bytes));
    REQUIRE(out.divisions[0].batches.size() == 3u);
    std::set<u32> entries;
    for (const m3::Batch& batch : out.divisions[0].batches) {
        entries.insert(batch.materialIndex);
    }
    CHECK(entries.size() == 3u);

    const auto fadeKeys = [&out](const m3::Batch& batch) {
        const m3::StandardMaterial& mat =
            out.standardMaterials.at(out.materialMaps.at(batch.materialIndex).materialIndex);
        std::vector<std::vector<std::pair<i32, f32>>> keyed;
        for (const auto* layer : {&mat.alphaLayer1, &mat.alphaLayer2}) {
            if (layer->has_value() && (*layer)->mapAlpha.isAnimated()) {
                keyed.push_back(
                    keysOf(out, "Stand", (*layer)->mapAlpha, &m3::SubTrackContainer::sdr3, 5));
            }
        }
        return keyed;
    };
    const auto a = fadeKeys(out.divisions[0].batches[0]);
    const auto plain = fadeKeys(out.divisions[0].batches[1]);
    const auto b = fadeKeys(out.divisions[0].batches[2]);
    // The layer fade on every copy; each geoset fade on its own copy alone.
    REQUIRE(plain.size() == 1u);
    REQUIRE(a.size() == 2u);
    REQUIRE(b.size() == 2u);
    const auto holds = [](const std::vector<std::vector<std::pair<i32, f32>>>& streams, i32 time,
                          f32 value) {
        for (const auto& stream : streams) {
            for (const auto& [t, v] : stream) {
                if (t == time && std::fabs(v - value) < 1e-4f) {
                    return true;
                }
            }
        }
        return false;
    };
    CHECK(holds(plain, 300, 0.5f));
    CHECK(holds(a, 300, 0.5f));
    CHECK(holds(b, 300, 0.5f));
    CHECK(holds(a, 500, 0.25f));
    CHECK_FALSE(holds(b, 500, 0.25f));
    CHECK(holds(b, 800, 1.0f));
    CHECK_FALSE(holds(a, 800, 1.0f));
    CHECK_FALSE(holds(plain, 500, 0.25f));
}

// ============================================================================
// C8.1 -- model-spawning emitters
// ============================================================================

TEST_CASE("wem m3 a model-spawning emitter is a model particle naming the written model",
          "[wem][m3][particles]") {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter bones;
    bones.node = makeNode("SuperSpray05Bones", 1, 0);
    bones.spawnModelFileName = "SharedModels\\Bones1.MDL";
    bones.emissionRate = 24.0f;
    bones.lifespan = 2.5f;
    bones.initialVelocity = 560.0f;
    bones.gravity = 2155.83f;
    bones.latitude = 0.506145f;
    bones.longitude = 3.14159265f;
    bones.visibilityTracks =
        makeTrack(mdx::InterpolationType::None, {0, 500}, std::vector<f32>{0.0f, 1.0f});
    model.particleEmitters.push_back(bones);
    setPivot(model, 1, Vector3f{0, 0, 60});
    // Wide and over part of a circle.
    mdx::ParticleEmitter leaves = bones;
    leaves.node = makeNode("SuperSpray01", 2, 0);
    leaves.spawnModelFileName = "SharedModels\\leaves.mdl";
    leaves.latitude = 2.5f;
    leaves.longitude = 1.0f;
    leaves.visibilityTracks = {};
    model.particleEmitters.push_back(leaves);
    setPivot(model, 2, Vector3f{0, 0, 0});
    // Its model was not written.
    mdx::ParticleEmitter orphan = leaves;
    orphan.node = makeNode("SuperSpray02", 3, 0);
    orphan.spawnModelFileName = "SharedModels\\missing.mdl";
    model.particleEmitters.push_back(orphan);
    setPivot(model, 3, Vector3f{0, 0, 0});

    const Crossed out =
        crossAndRead(model, {}, {},
                     {{"SharedModels\\Bones1.MDL", "Assets/Models/Bones1.m3"},
                      {"SharedModels\\leaves.mdl", "Assets/Models/leaves.m3"}});
    REQUIRE(out.model.particleEmitters.size() == 2u);
    CHECK(out.report.modelParticleRecords == 2u);
    CHECK(out.report.diagnostics.countOf(DiagCode::FeatureDropped) >= 1u);
    CHECK(out.report.diagnostics.countOf(DiagCode::AnimTrackApproximated) >= 1u);

    const m3::ParticleEmitter& p = out.model.particleEmitters[0];
    REQUIRE(p.modelPaths.size() == 1u);
    std::string path = p.modelPaths[0];
    while (!path.empty() && path.back() == '\0') {
        path.pop_back();
    }
    CHECK(path == "Assets/Models/Bones1.m3");
    CHECK(hasFlag(p.flags, m3::ParticleFlag::ModelParticles));
    CHECK(hasFlag(p.additionalFlags, m3::ParticleAdditionalFlag::WorldSpace));
    CHECK_FALSE(hasFlag(p.additionalFlags, m3::ParticleAdditionalFlag::EmitSpeedRandomize));
    CHECK(p.instanceType == m3::ParticleInstanceType::FaceWorldDir);
    CHECK(p.instanceAngle.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(p.instanceAngle.y == Catch::Approx(-1.0f));
    CHECK(p.sizeAnimation.initValue.x == Catch::Approx(1.0f));
    CHECK(p.initialSpeed.initValue == Catch::Approx(5.6f));
    CHECK(p.initialSpeedRandom.initValue == Catch::Approx(5.6f));
    CHECK(p.velocityType == 0u);
    CHECK(p.initialHorizontal.initValue == Catch::Approx(0.506145f / std::sqrt(2.0f)));
    CHECK(p.initialVertical.initValue == Catch::Approx(0.506145f / std::sqrt(2.0f)));
    CHECK(p.gravity == Catch::Approx(-21.5583f));
    CHECK(p.lifetime.initValue == Catch::Approx(2.5f));
    CHECK(p.emissionRate.initValue == Catch::Approx(24.0f));
    CHECK(p.emissionRate.nullValue == 0.0f);
    CHECK(p.maxParticles == 85u);
    const u32 bone = boneNamed(out.model, "SuperSpray05Bones");
    REQUIRE(bone != kInvalidIndex);
    CHECK(p.boneIndex == bone);
    CHECK(out.model.bones[bone].visibility.isAnimated());

    const m3::ParticleEmitter& wide = out.model.particleEmitters[1];
    CHECK(wide.velocityType == 3u);
}

// ============================================================================
// C8.2 -- hit tests
// ============================================================================

TEST_CASE("wem m3 a collision shape is a fuzzy hit test on its node's bone",
          "[wem][m3][hittests]") {
    mdx::Model model = makeModel();
    // A sphere states its centre in model space; a box its corners about the
    // pivot; a plane has no StarCraft II shape.
    mdx::CollisionShape sphere;
    sphere.node = makeNode("Collision Sphere", 1, mdx::Node::NO_PARENT);
    sphere.type = mdx::CollisionShape::ShapeType::Sphere;
    sphere.vertices = {Vector3f{20, -10, 80}};
    sphere.radius = 40.0f;
    model.collisionShapes.push_back(sphere);
    setPivot(model, 1, Vector3f{20, -10, 80});
    mdx::CollisionShape box;
    box.node = makeNode("Collision Box", 2, 0);
    box.type = mdx::CollisionShape::ShapeType::Box;
    box.vertices = {Vector3f{-10, -20, 0}, Vector3f{10, 20, 50}};
    model.collisionShapes.push_back(box);
    setPivot(model, 2, Vector3f{30, 5, 0});
    mdx::CollisionShape plane;
    plane.node = makeNode("Collision Plane", 3, mdx::Node::NO_PARENT);
    plane.type = mdx::CollisionShape::ShapeType::Plane;
    plane.vertices = {Vector3f{-10, -10, 0}, Vector3f{10, 10, 0}};
    model.collisionShapes.push_back(plane);
    setPivot(model, 3, Vector3f{0, 0, 0});

    const Crossed out = crossAndRead(model);
    REQUIRE(out.model.fuzzyHitTestObjects.size() == 2u);
    CHECK(out.report.hitTests == 2u);
    CHECK(out.report.diagnostics.countOf(DiagCode::FeatureDropped) >= 1u);
    const auto restOf = [&out](u32 bone) {
        const Matrix44f inv = Matrix44f::inverse(out.model.initialReference.at(bone).matrix);
        return Vector3f{inv.data[3][0], inv.data[3][1], inv.data[3][2]};
    };

    const m3::HitTestShape& s = out.model.fuzzyHitTestObjects[0];
    CHECK(s.shapeType == m3::HitTestShapeType::Sphere);
    CHECK(s.sizeX == Catch::Approx(0.4f));
    const u32 sphereBone = boneNamed(out.model, "Collision Sphere");
    REQUIRE(sphereBone != kInvalidIndex);
    CHECK(s.boneIndex == sphereBone);
    CHECK(out.model.bones[sphereBone].parentIndex == 0xFFFFu);
    const Vector3f sphereRest = restOf(sphereBone);
    CHECK(sphereRest.x == Catch::Approx(-0.1f).margin(1e-4));
    CHECK(sphereRest.y == Catch::Approx(-0.2f).margin(1e-4));
    CHECK(sphereRest.z == Catch::Approx(0.8f).margin(1e-4));
    CHECK(s.transform.data[3][0] == Catch::Approx(0.0f).margin(1e-5));
    CHECK(s.transform.data[3][2] == Catch::Approx(0.0f).margin(1e-5));

    const m3::HitTestShape& b = out.model.fuzzyHitTestObjects[1];
    CHECK(b.shapeType == m3::HitTestShapeType::Box);
    // Half extents, the ground axes swapped by the basis change.
    CHECK(b.sizeX == Catch::Approx(0.2f));
    CHECK(b.sizeY == Catch::Approx(0.1f));
    CHECK(b.sizeZ == Catch::Approx(0.25f));
    const u32 boxBone = boneNamed(out.model, "Collision Box");
    REQUIRE(boxBone != kInvalidIndex);
    CHECK(b.boneIndex == boxBone);
    CHECK(out.model.bones[boxBone].parentIndex == boneNamed(out.model, "root"));
    const Vector3f boxRest = restOf(boxBone);
    CHECK(boxRest.x == Catch::Approx(0.05f).margin(1e-4));
    CHECK(boxRest.y == Catch::Approx(-0.3f).margin(1e-4));
    CHECK(b.transform.data[3][0] == Catch::Approx(0.0f).margin(1e-5));
    CHECK(b.transform.data[3][1] == Catch::Approx(0.0f).margin(1e-5));
    CHECK(b.transform.data[3][2] == Catch::Approx(0.25f));
}

// ============================================================================
// C2.6 -- cameras
// ============================================================================

namespace {

Vector3f sub3(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.x - b.x, a.y - b.y, a.z - b.z};
}

f32 dot3(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3f cross3(const Vector3f& a, const Vector3f& b) {
    return Vector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vector3f unit3(const Vector3f& v) {
    const f32 l = std::sqrt(dot3(v, v));
    return Vector3f{v.x / l, v.y / l, v.z / l};
}

/// Warcraft III's (x, y, z) in StarCraft II units: (y, -x, z) / 100.
Vector3f sc2Point(const Vector3f& v) {
    return Vector3f{v.y * 0.01f, -v.x * 0.01f, v.z * 0.01f};
}

/// Where a unit quaternion sends the X and Z axes, the way retail builds a
/// bone's rotation.
void axesOf(const Quaternion& q, Vector3f& x, Vector3f& z) {
    x = Vector3f{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.z * q.w),
                 2 * (q.x * q.z - q.y * q.w)};
    z = Vector3f{2 * (q.x * q.z + q.y * q.w), 2 * (q.y * q.z - q.x * q.w),
                 1 - 2 * (q.x * q.x + q.y * q.y)};
}

mdx::Camera makeCamera(const Vector3f& position, const Vector3f& target) {
    mdx::Camera camera;
    camera.name = "Portrait";
    camera.position = position;
    camera.targetPosition = target;
    camera.fieldOfView = 0.7854f;
    camera.nearClippingPlane = 8.0f;
    camera.farClippingPlane = 1000.0f;
    return camera;
}

} // namespace

TEST_CASE("wem m3 a Warcraft III camera stands where it did and looks at its target",
          "[wem][m3][cameras]") {
    mdx::Model model = makeModel();
    const Vector3f position{100, 200, 300};
    const Vector3f target{0, 0, 250};
    model.cameras.push_back(makeCamera(position, target));
    const Crossed out = crossAndRead(model);

    const u32 bone = boneNamed(out.model, "Portrait");
    REQUIRE(bone != kInvalidIndex);
    REQUIRE(out.model.cameras.size() == 1);
    const m3::Camera& camera = out.model.cameras[0];
    CHECK(camera.boneIndex == bone);
    const m3::Bone& carrier = out.model.bones[bone];

    // Where it stands, in the bone and in its IREF.
    const Vector3f at = sc2Point(position);
    CHECK(carrier.position.initValue.x == Catch::Approx(at.x));
    CHECK(carrier.position.initValue.y == Catch::Approx(at.y));
    CHECK(carrier.position.initValue.z == Catch::Approx(at.z));
    const Matrix44f bind = Matrix44f::inverse(out.model.initialReference.at(bone).matrix);
    CHECK(bind.data[3][0] == Catch::Approx(at.x));
    CHECK(bind.data[3][1] == Catch::Approx(at.y));
    CHECK(bind.data[3][2] == Catch::Approx(at.z));

    // What it looks at: down -Z, X level, Y = Z x X.
    const Vector3f forward = unit3(sub3(sc2Point(target), at));
    const Vector3f x{bind.data[0][0], bind.data[0][1], bind.data[0][2]};
    const Vector3f y{bind.data[1][0], bind.data[1][1], bind.data[1][2]};
    const Vector3f z{bind.data[2][0], bind.data[2][1], bind.data[2][2]};
    CHECK(dot3(z, forward) == Catch::Approx(-1.0f).margin(1e-5));
    CHECK(x.z == Catch::Approx(0.0f).margin(1e-5));
    CHECK(dot3(x, x) == Catch::Approx(1.0f).margin(1e-5));
    CHECK(dot3(y, cross3(z, x)) == Catch::Approx(1.0f).margin(1e-5));
    // The bone's own rest rotation is the same frame, and rests where it
    // starts.
    Vector3f qx, qz;
    axesOf(carrier.rotation.initValue, qx, qz);
    CHECK(dot3(qx, x) == Catch::Approx(1.0f).margin(1e-5));
    CHECK(dot3(qz, z) == Catch::Approx(1.0f).margin(1e-5));
    CHECK(carrier.rotation.nullValue.w == carrier.rotation.initValue.w);

    // Blizzard's vertical field of view over Warcraft III's horizontal one.
    CHECK(camera.fieldOfView.initValue ==
          Catch::Approx(2.0f * std::atan(0.75f * std::tan(0.7854f * 0.5f))));
    CHECK(camera.fieldOfView.nullValue == camera.fieldOfView.initValue);
    CHECK(camera.useVerticalFOV == 1u);
    CHECK(camera.nearClip.initValue == Catch::Approx(0.08f));
    CHECK(camera.farClip.initValue == Catch::Approx(10.0f));
    CHECK(out.report.cameraRecords == 1u);
}

TEST_CASE("wem m3 a camera keyed only by its target is solved as an animated bone",
          "[wem][m3][cameras]") {
    mdx::Model model = makeModel();
    mdx::Camera camera = makeCamera(Vector3f{100, 0, 100}, Vector3f{0, 0, 100});
    camera.targetPositionTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                  std::vector<Vector3f>{{0, 0, 0}, {0, 100, 0}});
    model.cameras.push_back(camera);
    const Crossed out = crossAndRead(model);
    const u32 bone = boneNamed(out.model, "Portrait");
    REQUIRE(bone != kInvalidIndex);
    const m3::Bone& carrier = out.model.bones[bone];
    CHECK(carrier.position.animId == 0u);
    REQUIRE(carrier.rotation.animId != 0u);
    // The file latches its bone flags as solved: this bone's must say it moves.
    CHECK((static_cast<u32>(carrier.flags) & 0x2200u) == 0x2200u);
}

TEST_CASE("wem m3 a camera's keyed position, target and roll turn its bone",
          "[wem][m3][cameras]") {
    mdx::Model model = makeModel();
    const Vector3f position{100, 0, 100};
    const Vector3f target{0, 0, 100};
    mdx::Camera camera = makeCamera(position, target);
    camera.positionTracks = makeTrack(mdx::InterpolationType::Linear, {0, 1000},
                                      std::vector<Vector3f>{{0, 0, 0}, {0, 0, 100}});
    camera.targetPositionTracks =
        makeTrack(mdx::InterpolationType::Linear, {500}, std::vector<Vector3f>{{0, 100, 0}});
    camera.targetRotationTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 1000}, std::vector<f32>{0.0f, 0.5f});
    model.cameras.push_back(camera);
    const Crossed out = crossAndRead(model);

    const u32 bone = boneNamed(out.model, "Portrait");
    REQUIRE(bone != kInvalidIndex);
    const m3::Bone& carrier = out.model.bones[bone];
    REQUIRE(carrier.position.animId != 0u);
    REQUIRE(carrier.rotation.animId != 0u);
    const u32 stand = sequenceNamed(out.model, "Stand");
    REQUIRE(stand != kInvalidIndex);
    const u32 stc = out.model.animationGroups.at(stand).subtrackIndices.at(0);
    const i32 end = static_cast<i32>(out.model.sequences[stand].startFrame) + 1000;

    // KCTR offsets the position: one unit up by the end.
    const u32 moved = refFor(out.model, stc, carrier.position.animId);
    REQUIRE(moved != kInvalidIndex);
    const auto& positions = out.model.subTrackCollections[stc].sd3v.at(moved & 0xFFFF);
    REQUIRE(!positions.keys.empty());
    CHECK(positions.keys.back().z == Catch::Approx(2.0f));

    // The rotation stream, at the end: from the raised camera to the moved
    // target, rolled half a radian.
    const u32 turned = refFor(out.model, stc, carrier.rotation.animId);
    REQUIRE(turned != kInvalidIndex);
    CHECK((turned >> 16) == 3u);
    const auto& rotations = out.model.subTrackCollections[stc].sd4q.at(turned & 0xFFFF);
    std::size_t last = rotations.timestamps.size();
    for (std::size_t k = 0; k < rotations.timestamps.size(); ++k) {
        if (rotations.timestamps[k] == end) {
            last = k;
        }
    }
    REQUIRE(last < rotations.keys.size());
    const Vector3f from = sc2Point(Vector3f{100, 0, 200});
    const Vector3f to = sc2Point(Vector3f{0, 100, 100});
    const Vector3f z = sub3(Vector3f{0, 0, 0}, unit3(sub3(to, from)));
    const Vector3f level = unit3(cross3(Vector3f{0, 0, 1}, z));
    const Vector3f up = cross3(z, level);
    const Vector3f rolled{level.x * std::cos(0.5f) + up.x * std::sin(0.5f),
                          level.y * std::cos(0.5f) + up.y * std::sin(0.5f),
                          level.z * std::cos(0.5f) + up.z * std::sin(0.5f)};
    Vector3f qx, qz;
    axesOf(rotations.keys[last], qx, qz);
    CHECK(dot3(qz, z) == Catch::Approx(1.0f).margin(1e-4));
    CHECK(dot3(qx, rolled) == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("wem m3 a Warcraft III ribbon reads back as the RIB_ its rows name",
          "[wem][m3][ribbons]") {
    mdx::Model model = makeModel();
    mdx::Material material;
    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::Blend;
    layer.textureId = 0;
    layer.shadingFlags = mdx::Layer::ShadingFlag::Unshaded | mdx::Layer::ShadingFlag::TwoSided;
    material.layers.push_back(layer);
    material.priorityPlane = 1;
    model.materials.push_back(material);

    mdx::RibbonEmitter rb;
    rb.node = makeNode("Trail", 1, 0);
    rb.heightAbove = 12.0f;
    rb.heightBelow = 8.0f;
    rb.color = Vector3f{1.0f, 0.5f, 0.0f};
    rb.alpha = 0.5f;
    rb.lifespan = 0.1f;
    rb.emissionRate = 40;
    rb.gravity = 30.0f;
    rb.materialId = 1;
    rb.visibilityTracks =
        makeTrack(mdx::InterpolationType::None, {0, 500}, std::vector<f32>{0.0f, 1.0f});
    model.ribbonEmitters.push_back(rb);
    setPivot(model, 1, Vector3f{0, 0, 40});

    const Crossed out = crossAndRead(model, {"Assets/Textures/trail.dds"});
    REQUIRE(out.model.ribbonEmitters.size() == 1);
    const m3::RibbonEmitter& r = out.model.ribbonEmitters[0];
    const u32 node = boneNamed(out.model, "Trail");
    const u32 bone = boneNamed(out.model, "Trail_Offset");
    REQUIRE(node != kInvalidIndex);
    REQUIRE(bone != kInvalidIndex);
    // Row 2: 12 above and 8 below, so the strip rides a helper 2 units along
    // the node's +Y, the bone's +X, which holds the visibility the node keyed.
    CHECK(r.boneIndex == bone);
    CHECK(out.model.bones[bone].parentIndex == node);
    CHECK(out.model.bones[bone].position.initValue.x == Catch::Approx(0.02f));
    CHECK(out.model.bones[bone].position.initValue.y == Catch::Approx(0.0f).margin(1e-6));
    CHECK(out.model.bones[bone].position.initValue.z == Catch::Approx(0.0f).margin(1e-6));
    CHECK(out.model.bones[bone].visibility.isAnimated());
    CHECK_FALSE(out.model.bones[node].visibility.isAnimated());
    // Rows 1, 4-6, 8.
    CHECK(r.sizeAnimation.initValue.x == Catch::Approx(0.4f));
    CHECK(r.lifetime.initValue == Catch::Approx(0.25f));
    CHECK(r.divisions == Catch::Approx(10.0f));
    CHECK(r.colorStart.initValue.r == 255);
    CHECK(r.colorStart.initValue.g == 127);
    CHECK(r.colorStart.initValue.a == 127);
    CHECK(r.colorEnd.initValue.a == 127);
    CHECK(r.gravity == Catch::Approx(-0.6f));
    // Row 13 and §4.6, and accurate tangents for a strip that falls (row 8).
    CHECK(r.emitterShape == 1u);
    CHECK(static_cast<u32>(r.flags) == 0xE800u);
    CHECK(hasFlag(r.additionalFlags, m3::RibbonAdditionalFlag::WorldSpace));
    CHECK(r.mass == Catch::Approx(1.0f));
    CHECK(r.active.initValue == 1u);
    // But its null stays 0, as the emission pair's does: an unkeyed ribbon
    // sitting at its `active` null reads as one that emits nothing of its own,
    // and the runtime retires it before it lays a segment.
    CHECK(r.active.nullValue == 0u);
    // Rows 10-11.
    const m3::StandardMaterial& mat = materialOf(out.model, r.materialIndex);
    CHECK(mat.blendMode == m3::BlendMode::AlphaBlend);
    CHECK(hasFlag(mat.flags, m3::MaterialFlag::Unshaded));
    CHECK(hasFlag(mat.flags, m3::MaterialFlag::TwoSided));
    CHECK(mat.priority == 2);
    REQUIRE(mat.diffuseLayer.has_value());
    CHECK(mat.diffuseLayer->uvMapping == m3::UVMappingMode::ExplicitUV0);
    CHECK(mat.diffuseLayer->uvAngle.initValue.z == Catch::Approx(-3.14159265f * 0.5f));
    REQUIRE(mat.alphaLayer1.has_value());
    CHECK(out.report.ribbonRecords == 1u);
}

TEST_CASE("wem m3 a centred ribbon that does not fall rides its own bone on the GPU technique",
          "[wem][m3][ribbons]") {
    mdx::Model model = makeModel();
    mdx::RibbonEmitter rb;
    rb.node = makeNode("Trail", 1, 0);
    rb.heightAbove = 10.0f;
    rb.heightBelow = 10.0f;
    rb.lifespan = 0.5f;
    rb.emissionRate = 20;
    rb.materialId = 0;
    rb.visibilityTracks =
        makeTrack(mdx::InterpolationType::None, {0, 500}, std::vector<f32>{0.0f, 1.0f});
    model.ribbonEmitters.push_back(rb);
    setPivot(model, 1, Vector3f{5, 0, 40});

    const Crossed out = crossAndRead(model, {"Assets/Textures/body.dds"});
    REQUIRE(out.model.ribbonEmitters.size() == 1);
    const m3::RibbonEmitter& r = out.model.ribbonEmitters[0];
    const u32 bone = boneNamed(out.model, "Trail");
    REQUIRE(bone != kInvalidIndex);
    CHECK(boneNamed(out.model, "Trail_Offset") == kInvalidIndex);
    CHECK(r.boneIndex == bone);
    CHECK(out.model.bones[bone].visibility.isAnimated());
    CHECK(static_cast<u32>(r.flags) == 0xC800u);
    CHECK(r.gravity == 0.0f);
    CHECK(out.report.diagnostics.countOf(DiagCode::AnimTrackApproximated) == 0u);
}

TEST_CASE("wem m3 a ribbon whose height difference moves keeps the rest offset and says so",
          "[wem][m3][ribbons]") {
    mdx::Model model = makeModel();
    mdx::RibbonEmitter rb;
    rb.node = makeNode("Trail", 1, 0);
    rb.heightAbove = 30.0f;
    rb.heightBelow = 10.0f;
    rb.lifespan = 0.5f;
    rb.emissionRate = 20;
    rb.materialId = 0;
    // Both heights grow together first (the difference holds), then only the
    // top does.
    rb.heightAboveTracks = makeTrack(mdx::InterpolationType::Linear, {0, 500, 1000},
                                     std::vector<f32>{30.0f, 40.0f, 60.0f});
    rb.heightBelowTracks = makeTrack(mdx::InterpolationType::Linear, {0, 500, 1000},
                                     std::vector<f32>{10.0f, 20.0f, 20.0f});
    model.ribbonEmitters.push_back(rb);
    setPivot(model, 1, Vector3f{0, 0, 0});

    const Crossed out = crossAndRead(model, {"Assets/Textures/body.dds"});
    REQUIRE(out.model.ribbonEmitters.size() == 1);
    const u32 bone = boneNamed(out.model, "Trail_Offset");
    REQUIRE(bone != kInvalidIndex);
    CHECK(out.model.ribbonEmitters[0].boneIndex == bone);
    CHECK(out.model.bones[bone].position.initValue.x == Catch::Approx(0.1f));
    CHECK(out.report.diagnostics.countOf(DiagCode::AnimTrackApproximated) == 1u);

    // Only the difference moving says so.
    rb.heightBelowTracks = makeTrack(mdx::InterpolationType::Linear, {0, 500, 1000},
                                     std::vector<f32>{10.0f, 20.0f, 40.0f});
    model.ribbonEmitters[0] = rb;
    CHECK(crossAndRead(model, {"Assets/Textures/body.dds"})
              .report.diagnostics.countOf(DiagCode::AnimTrackApproximated) == 0u);
}

TEST_CASE("wem m3 without effects an off-centre ribbon plants no helper", "[wem][m3][ribbons]") {
    mdx::Model model = makeModel();
    mdx::RibbonEmitter rb;
    rb.node = makeNode("Trail", 1, 0);
    rb.heightAbove = 12.0f;
    rb.heightBelow = 8.0f;
    model.ribbonEmitters.push_back(rb);
    setPivot(model, 1, Vector3f{0, 0, 0});
    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(model);
    REQUIRE(imported.ok());
    Document document = std::move(*imported.value);
    const u32 before = document.models.front().nodes.size();
    Diagnostics diagnostics;
    cross::PrepareWc3Effects(document, diagnostics, false);
    CHECK(document.models.front().nodes.size() == before);
    cross::PrepareWc3Effects(document, diagnostics, true);
    CHECK(document.models.front().nodes.size() == before + 1);
}

// ============================================================================
// The emitters cross inside WEM
// ============================================================================

namespace {

/// One of each system: a head-and-tail smoke over a keyed rate, an off-centre
/// ribbon keying a height, and a model spawner.
mdx::Model systemsModel() {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 smoke = makeEmitter("Smoke", 1, 0);
    smoke.headOrTail = 2;
    smoke.tailLength = 0.5f;
    smoke.emissionRateTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 1000}, std::vector<f32>{0.0f, 40.0f});
    model.particleEmitters2.push_back(smoke);
    setPivot(model, 1, Vector3f{0, 0, 30});
    mdx::RibbonEmitter trail;
    trail.node = makeNode("Trail", 2, 0);
    trail.heightAbove = 12.0f;
    trail.heightBelow = 8.0f;
    trail.lifespan = 0.5f;
    trail.emissionRate = 20;
    trail.color = Vector3f{1, 1, 1};
    trail.alpha = 1.0f;
    trail.materialId = 0;
    trail.heightAboveTracks =
        makeTrack(mdx::InterpolationType::Linear, {0, 1000}, std::vector<f32>{12.0f, 24.0f});
    model.ribbonEmitters.push_back(trail);
    setPivot(model, 2, Vector3f{0, 0, 40});
    mdx::ParticleEmitter bones;
    bones.node = makeNode("Bones", 3, 0);
    bones.spawnModelFileName = "SharedModels\\Bones1.mdl";
    bones.emissionRate = 5.0f;
    bones.lifespan = 1.0f;
    bones.initialVelocity = 100.0f;
    bones.latitude = 0.5f;
    bones.longitude = 3.14159265f;
    model.particleEmitters.push_back(bones);
    setPivot(model, 3, Vector3f{0, 0, 50});
    return model;
}

Plan systemsPlan() {
    return Plan{{"Assets/Textures/body.dds"},
                {},
                {{"SharedModels\\Bones1.mdl", "Assets/Models/Bones1.m3"}}};
}

} // namespace

TEST_CASE("wem m3 Warcraft III emitters cross as the StarCraft II nodes toM3 writes",
          "[wem][m3][particles]") {
    const mdx::Model model = systemsModel();
    const Crossed out = crossDocument(importMdx(model), model, systemsPlan());
    const Model& staged = out.staged.models.front();

    // No Warcraft III system is left; every record is a node of its own.
    u32 particles = 0;
    u32 ribbons = 0;
    for (const Node& node : staged.nodes.nodes) {
        CHECK_FALSE(node.kind == NodeKind::Wc3ParticleEmitter1);
        CHECK_FALSE(node.kind == NodeKind::Wc3ParticleEmitter2);
        CHECK_FALSE(node.kind == NodeKind::Wc3RibbonEmitter);
        particles += node.kind == NodeKind::Sc2ParticleEmitter ? 1u : 0u;
        ribbons += node.kind == NodeKind::Sc2RibbonEmitter ? 1u : 0u;
    }
    CHECK(particles == 3u); // the smoke's head and tail, and the spawner
    CHECK(ribbons == 1u);
    CHECK(out.report.particleRecords == 3u);
    CHECK(out.report.modelParticleRecords == 1u);
    CHECK(out.report.ribbonRecords == 1u);
    REQUIRE(out.model.particleEmitters.size() == 3u);
    REQUIRE(out.model.ribbonEmitters.size() == 1u);

    // Every keyed property is a StarCraft II property of its record's kind.
    u32 keyed = 0;
    for (const AnimChannel& channel : staged.animChannels.channels) {
        if (channel.target.channel != Channel::EmitterProperty) {
            continue;
        }
        const NodeKind kind = staged.nodes.nodes.at(channel.target.node).kind;
        CHECK((kind == NodeKind::Sc2ParticleEmitter || kind == NodeKind::Sc2RibbonEmitter));
        const EmitterPropertyDesc* desc = FindEmitterProperty(kind, channel.target.sub);
        REQUIRE(desc != nullptr);
        CHECK(desc->type == channel.valueType);
        ++keyed;
    }
    CHECK(keyed == 3u); // the rate on the head and on the tail, the ribbon's size
    CHECK_FALSE(Validate(out.staged, ValidateLevel::Structural).hasErrors());

    // The tail rides the smoke's bone and keys the same rate under its own id.
    const u32 smoke = boneNamed(out.model, "Smoke");
    REQUIRE(smoke != kInvalidIndex);
    CHECK(boneNamed(out.model, "Smoke_Tail") == kInvalidIndex);
    std::vector<const m3::ParticleEmitter*> onSmoke;
    for (const m3::ParticleEmitter& p : out.model.particleEmitters) {
        if (p.boneIndex == smoke) {
            onSmoke.push_back(&p);
        }
    }
    REQUIRE(onSmoke.size() == 2u);
    CHECK(onSmoke[0]->emissionRate.animId != onSmoke[1]->emissionRate.animId);
    const auto head = keysOf(out.model, "Stand", onSmoke[0]->emissionRate,
                             &m3::SubTrackContainer::sdr3, 5);
    const auto tail = keysOf(out.model, "Stand", onSmoke[1]->emissionRate,
                             &m3::SubTrackContainer::sdr3, 5);
    REQUIRE(head.size() == 2u);
    CHECK(tail == head);

    // Each record's material is a slot of the model, bound in StarCraft II's set.
    for (const m3::ParticleEmitter& p : out.model.particleEmitters) {
        CHECK(Resolve(staged, p.materialIndex, ProfileId::Sc2) != nullptr);
    }
    CHECK(Resolve(staged, out.model.ribbonEmitters[0].materialIndex, ProfileId::Sc2) != nullptr);
}

TEST_CASE("wem m3 a crossed emitter follows its WEM payload, not the .mdx",
          "[wem][m3][particles]") {
    mdx::Model model = makeModel();
    model.particleEmitters2.push_back(makeEmitter("Smoke", 1, 0));
    setPivot(model, 1, Vector3f{0, 0, 0});
    Document document = importMdx(model);
    const u32 node = nodeWithObjectId(document, 1);
    REQUIRE(node != kInvalidIndex);
    auto& payload =
        std::get<Wc3ParticleEmitter2Payload>(document.models[0].nodes.nodes[node].payload);
    payload.speed = 300.0f;
    payload.filter = Wc3ParticleFilter::Additive;

    const Crossed out =
        crossDocument(std::move(document), model, Plan{{"Assets/Textures/body.dds"}, {}, {}});
    REQUIRE(out.model.particleEmitters.size() == 1u);
    const m3::ParticleEmitter& p = out.model.particleEmitters[0];
    CHECK(p.initialSpeed.initValue == Catch::Approx(3.0f));
    CHECK(materialOf(out.model, p.materialIndex).blendMode == m3::BlendMode::AlphaAdd);
}

TEST_CASE("wem m3 a .wem of a Warcraft III model crosses to the same file",
          "[wem][m3][particles]") {
    const mdx::Model model = systemsModel();
    const Crossed direct = crossDocument(importMdx(model), model, systemsPlan());

    Writer writer;
    const std::vector<u8> bytes = writer.write(importMdx(model));
    Parser parser;
    std::optional<Document> reread = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(reread.has_value());
    const Crossed fromWem = crossDocument(std::move(*reread), model, systemsPlan());
    CHECK(fromWem.model.particleEmitters.size() == 3u);
    CHECK(fromWem.bytes == direct.bytes);
}

TEST_CASE("wem m3 a second record hides with its emitter, not with its emitter's children",
          "[wem][m3][carriers]") {
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 streak = makeEmitter("Streak", 1, 0);
    streak.headOrTail = 2;
    streak.visibilityTracks =
        makeTrack(mdx::InterpolationType::None, {0, 500}, std::vector<f32>{1.0f, 0.0f});
    model.particleEmitters2.push_back(streak);
    setPivot(model, 1, Vector3f{0, 0, 30});
    model.particleEmitters2.push_back(makeEmitter("Spark", 2, 1));
    setPivot(model, 2, Vector3f{0, 0, 40});

    const Crossed out = crossAndRead(model);
    const u32 owner = boneNamed(out.model, "Streak");
    const u32 leaf = boneNamed(out.model, "Streak_Vis");
    const u32 child = boneNamed(out.model, "Spark");
    REQUIRE(owner != kInvalidIndex);
    REQUIRE(leaf != kInvalidIndex);
    REQUIRE(child != kInvalidIndex);
    // Node order: the head is Streak itself, then Spark, then the tail's node.
    REQUIRE(out.model.particleEmitters.size() == 3u);
    CHECK(out.model.particleEmitters[0].boneIndex == leaf);
    CHECK(out.model.particleEmitters[1].boneIndex == child);
    CHECK(out.model.particleEmitters[2].boneIndex == leaf);
    CHECK(out.model.bones[leaf].visibility.isAnimated());
    CHECK_FALSE(out.model.bones[owner].visibility.isAnimated());
    CHECK(out.model.bones[child].parentIndex == owner);
}

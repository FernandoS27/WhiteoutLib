// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P7 — the WC3 animation import.
///
/// MDX is the format whose animation model is furthest from WEM's: one global
/// timeline against N clips, no containers, and a `GeosetAnimation` that keys
/// something neither a node nor a material. Each case below is one of those
/// distances, plus the corpus arm that says the mapping survives shipped files
/// with **zero validation errors** — because a green conversion proves a track
/// parsed, never that its keys landed on the right channel.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/mdx/writer.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/validate.h>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;
namespace fs = std::filesystem;

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

/// Two sequences over one timeline, so slicing has something to slice.
mdx::Model makeModel() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "anim";

    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 1000;
    model.sequences.push_back(stand);

    mdx::Sequence walk;
    walk.name = "Walk";
    walk.intervalStart = 2000;
    walk.intervalEnd = 3000;
    walk.flags = mdx::Sequence::Flag::NonLooping;
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
    model.geosets.push_back(geoset);
    return model;
}

Document convert(const mdx::Model& model) {
    MdxConverter converter;
    Result<Document> result = converter.fromMdx(model);
    REQUIRE(result.ok());
    return std::move(*result.value);
}

const Clip* clipNamed(const Document& document, const std::string& name) {
    for (const Clip& clip : document.clips) {
        if (clip.name == name) {
            return &clip;
        }
    }
    return nullptr;
}

/// The one sub-track in @p clip driving a channel with @p channel semantics.
const SubTrack* trackFor(const Document& document, const Clip& clip, Channel channel) {
    const AnimChannelTable& table = document.models[clip.model].animChannels;
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            const AnimChannel* declared = table.find(track.channel);
            if (declared != nullptr && declared->target.channel == channel) {
                return &track;
            }
        }
    }
    return nullptr;
}

f32 firstFloat(const SubTrack& track) {
    f32 value = 0;
    std::memcpy(&value, track.values.data(), sizeof(f32));
    return value;
}

} // namespace

// ============================================================================
// Clips and slicing
// ============================================================================

TEST_CASE("wem mdx a sequence becomes a clip with one opaque container", "[wem][anim][mdx]") {
    mdx::Model model = makeModel();
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});

    const Document document = convert(model);
    REQUIRE(document.clips.size() == 2u);

    const Clip* stand = clipNamed(document, "Stand");
    REQUIRE(stand != nullptr);
    CHECK(stand->model == 0u);
    CHECK(stand->duration == 1.0f);
    CHECK(stand->looping);
    REQUIRE(stand->containers.size() == 1u);
    CHECK_FALSE(stand->containers[0].concurrent);
    CHECK(stand->containers[0].priority == 0);

    const Clip* walk = clipNamed(document, "Walk");
    REQUIRE(walk != nullptr);
    CHECK_FALSE(walk->looping);

    // The channel is declared once for the whole model, not once per clip.
    CHECK(document.models[0].animChannels.channels.size() == 1u);
}

TEST_CASE("wem mdx a slice keeps the keys that bracket its window", "[wem][anim][mdx]") {
    // Keys at 0 and 1500: the "Stand" window [0,1000] contains only the first,
    // and a slice that dropped the second would hold instead of moving.
    mdx::Model model = makeModel();
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {0, 1500}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 15}});

    const Document document = convert(model);
    const Clip* stand = clipNamed(document, "Stand");
    REQUIRE(stand != nullptr);
    const SubTrack* track = trackFor(document, *stand, Channel::Translation);
    REQUIRE(track != nullptr);

    REQUIRE(track->times.size() == 2u);
    CHECK(track->times[0] == 0.0f);
    // Past the clip's own duration, which §10.8.2 allows precisely for this.
    CHECK(track->times[1] == 1.5f);

    // And "Walk" [2000,3000] holds the last key, so the pose is right there too.
    const Clip* walk = clipNamed(document, "Walk");
    REQUIRE(walk != nullptr);
    const SubTrack* walkTrack = trackFor(document, *walk, Channel::Translation);
    REQUIRE(walkTrack != nullptr);
    REQUIRE(walkTrack->times.size() == 1u);
    CHECK(walkTrack->times[0] == -0.5f);
}

TEST_CASE("wem mdx hermite keys keep their tangents", "[wem][anim][mdx]") {
    mdx::Model model = makeModel();
    // One key, three values: value, inTangent, outTangent — MDX's own layout.
    model.bones[0].node.translationTracks =
        makeTrack<Vector3f>(mdx::InterpolationType::Hermite, {0},
                            {Vector3f{0, 0, 1}, Vector3f{0, 0, 2}, Vector3f{0, 0, 3}});

    const Document document = convert(model);
    const Clip* stand = clipNamed(document, "Stand");
    REQUIRE(stand != nullptr);
    const SubTrack* track = trackFor(document, *stand, Channel::Translation);
    REQUIRE(track != nullptr);
    CHECK(track->interp == Interpolation::Hermite);
    CHECK(track->times.size() == 1u);
    CHECK(track->values.size() == 3 * sizeof(Vector3f));
}

TEST_CASE("wem mdx a rotation track says slerp, not linear", "[wem][anim][mdx]") {
    // MDX's `Linear` over a quaternion *is* a shortest-arc slerp in the engine.
    mdx::Model model = makeModel();
    model.bones[0].node.rotationTracks =
        makeTrack<Quaternion>(mdx::InterpolationType::Linear, {0}, {Quaternion{0, 0, 0, 1}});

    const Document document = convert(model);
    const Clip* stand = clipNamed(document, "Stand");
    REQUIRE(stand != nullptr);
    const SubTrack* track = trackFor(document, *stand, Channel::Rotation);
    REQUIRE(track != nullptr);
    CHECK(track->interp == Interpolation::Slerp);
}

TEST_CASE("wem mdx a global sequence becomes an auto-play clip", "[wem][anim][mdx]") {
    mdx::Model model = makeModel();
    model.globalSequences.push_back(4000);
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {0, 4000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});
    model.bones[0].node.translationTracks.globalSequenceId = 0;

    const Document document = convert(model);
    // Two sequences plus the global one, and the track is in the global clip
    // alone — it runs on its own clock, so slicing it into the sequences would
    // be claiming it restarts with them.
    REQUIRE(document.clips.size() == 3u);
    const Clip* global = clipNamed(document, "globalSequence_0");
    REQUIRE(global != nullptr);
    CHECK(global->duration == 4.0f);
    CHECK(global->looping);
    CHECK(hasFlag(global->flags, ClipFlags::AutoPlay));
    CHECK(hasFlag(global->flags, ClipFlags::WorldClocked));
    CHECK(global->containers[0].subTracks.size() == 1u);
    CHECK(clipNamed(document, "Stand")->containers[0].subTracks.empty());
}

// ============================================================================
// What a track lands on
// ============================================================================

TEST_CASE("wem mdx a geoset animation drives a section, not a material", "[wem][anim][mdx]") {
    // The distinction that matters: two geosets can share a material and be
    // hidden separately, which is how a unit sheathes a weapon.
    mdx::Model model = makeModel();
    mdx::GeosetAnimation animation;
    animation.geosetId = 0;
    animation.alphaTracks = makeTrack<f32>(mdx::InterpolationType::Linear, {0, 1000}, {1.0f, 0.0f});
    model.geosetAnimations.push_back(animation);

    const Document document = convert(model);
    const AnimChannelTable& table = document.models[0].animChannels;
    REQUIRE(table.channels.size() == 1u);
    CHECK(table.channels[0].target.kind == TrackTarget::Kind::Section);
    CHECK(table.channels[0].target.mesh == 0u);
    CHECK(table.channels[0].target.sub == 0u);
    CHECK(table.channels[0].target.channel == Channel::Alpha);
    CHECK(table.channels[0].valueType == geom::AttrType::F32);

    const Clip* stand = clipNamed(document, "Stand");
    REQUIRE(stand != nullptr);
    const SubTrack* track = trackFor(document, *stand, Channel::Alpha);
    REQUIRE(track != nullptr);
    CHECK(firstFloat(*track) == 1.0f);
}

TEST_CASE("wem mdx a layer alpha track names an ordinal in the filtered stack",
          "[wem][anim][mdx]") {
    // Two layers, the first HD: the classic set keeps only the second, so its
    // ordinal is 0 even though it is layer 1 in the file.
    mdx::Model model = makeModel();
    model.version = 1200;
    mdx::Layer hd;
    hd.filterMode = mdx::Layer::FilterMode::None;
    hd.shader = mdx::Layer::ShaderType::HD;
    hd.is_hd = true;
    hd.textureId = 0;
    hd.textureAnimationId = 0xFFFFFFFF;
    model.materials[0].layers.insert(model.materials[0].layers.begin(), hd);
    model.materials[0].layers[1].alphaTracks =
        makeTrack<f32>(mdx::InterpolationType::Linear, {0, 1000}, {1.0f, 0.5f});

    const Document document = convert(model);
    const AnimChannelTable& table = document.models[0].animChannels;
    REQUIRE(table.channels.size() == 1u);
    const TrackTarget& target = table.channels[0].target;
    CHECK(target.kind == TrackTarget::Kind::MaterialLayer);
    CHECK(target.material.profile == ProfileId::Wc3Classic);
    CHECK(target.material.slot == 0u);
    CHECK(target.sub == 0u);
    CHECK(target.channel == Channel::Alpha);
}

TEST_CASE("wem mdx a texture animation becomes a feature and its channels", "[wem][anim][mdx]") {
    // §7.2.5: a keyed UV source leaves the rates zero and puts the motion on the
    // feature's channels — so the feature has to exist for them to name.
    mdx::Model model = makeModel();
    mdx::TextureAnimation animation;
    animation.translationTracks = makeTrack<Vector3f>(mdx::InterpolationType::Linear, {0, 1000},
                                                      {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}});
    model.textureAnimations.push_back(animation);
    model.materials[0].layers[0].textureAnimationId = 0;

    const Document document = convert(model);
    const ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Classic);
    REQUIRE(set != nullptr);
    REQUIRE(set->materials.size() == 1u);
    const std::vector<MaterialFeature>& features = set->materials[0].Common().features;
    REQUIRE(features.size() == 1u);
    CHECK(features[0].kind() == FeatureKind::UvAnimation);
    CHECK(features[0].layer == 0u);
    // The rates stay zero: the keys are the animation, not a constant rate.
    CHECK_FALSE(std::get<UvAnimationFeature>(features[0].payload).isConstantRate());

    // Adding the feature must not make the native block stale — the import is
    // still deriving common from native (§7.1).
    CHECK(set->materials[0].sync() == NativeSync::InSync);

    const AnimChannelTable& table = document.models[0].animChannels;
    REQUIRE(table.channels.size() == 1u);
    CHECK(table.channels[0].target.kind == TrackTarget::Kind::MaterialFeature);
    CHECK(table.channels[0].target.sub == features[0].id);
    CHECK(table.channels[0].target.channel == Channel::UvTranslate);
}

TEST_CASE("wem mdx an event object's times become clip events", "[wem][anim][mdx]") {
    mdx::Model model = makeModel();
    mdx::EventObject event;
    event.node = makeNode("SNDxFOOT", 1, 0);
    event.eventTrackTimes = {500, 2500, 9000};
    model.eventObjects.push_back(event);

    const Document document = convert(model);
    const Clip* stand = clipNamed(document, "Stand");
    const Clip* walk = clipNamed(document, "Walk");
    REQUIRE(stand != nullptr);
    REQUIRE(walk != nullptr);

    REQUIRE(stand->events.size() == 1u);
    CHECK(stand->events[0].time == 0.5f);
    CHECK(stand->events[0].name == "SNDxFOOT");
    CHECK(document.models[0].nodes.nodes[stand->events[0].node].kind == NodeKind::Event);

    REQUIRE(walk->events.size() == 1u);
    CHECK(walk->events[0].time == 0.5f);
    // 9000 is in no sequence's window, so it fires in no clip. Not an error —
    // shipped models carry keys past the last sequence.
    CHECK(stand->events.size() + walk->events.size() == 2u);
}

TEST_CASE("wem mdx an emitter keys its own properties beside its visibility",
          "[wem][anim][mdx]") {
    // §10.9: a PRE2 is a node of its own kind whose payload is the system, so
    // its emission rate is a property WEM holds and a track can drive -- an
    // `EmitterProperty` channel naming the property -- while the visibility
    // stays the shared channel every node kind keys.
    mdx::Model model = makeModel();
    mdx::ParticleEmitter2 emitter;
    emitter.node = makeNode("emitter", 1, 0);
    emitter.emissionRateTracks =
        makeTrack<f32>(mdx::InterpolationType::Linear, {0, 1000}, {0.0f, 40.0f});
    emitter.visibilityTracks =
        makeTrack<f32>(mdx::InterpolationType::None, {0, 1000}, {1.0f, 0.0f});
    model.particleEmitters2.push_back(emitter);

    const Document document = convert(model);
    const AnimChannelTable& table = document.models[0].animChannels;
    REQUIRE(table.channels.size() == 2u);
    CHECK(table.channels[0].target.channel == Channel::Visibility);
    CHECK(table.channels[1].target.channel == Channel::EmitterProperty);
    CHECK(table.channels[1].target.sub ==
          EmitterPropertySub(static_cast<u32>(Wc3Particle2Property::EmissionRate)));
    CHECK(table.channels[1].valueType == geom::AttrType::F32);
    CHECK(document.models[0].nodes.nodes[table.channels[1].target.node].kind ==
          NodeKind::Wc3ParticleEmitter2);
}

TEST_CASE("wem mdx a light keys its ambient term beside its diffuse one", "[wem][anim][mdx]") {
    mdx::Model model = makeModel();
    mdx::Light light;
    light.node = makeNode("lamp", 1, 0);
    light.colorTracks =
        makeTrack<Vector3f>(mdx::InterpolationType::Linear, {0}, {Vector3f{1, 0, 0}});
    light.ambientColorTracks =
        makeTrack<Vector3f>(mdx::InterpolationType::Linear, {0}, {Vector3f{0, 0, 1}});
    model.lights.push_back(light);

    const Document document = convert(model);
    const AnimChannelTable& table = document.models[0].animChannels;
    REQUIRE(table.channels.size() == 2u);
    CHECK(table.channels[0].target.channel == Channel::Color);
    CHECK(table.channels[0].target.sub == 0u);
    CHECK(table.channels[1].target.channel == Channel::Color);
    CHECK(table.channels[1].target.sub == 1u);
    CHECK(table.channels[0].target.node == table.channels[1].target.node);
}

TEST_CASE("wem mdx export writes the clips back onto the timeline", "[wem][anim][mdx]") {
    // The inverse of the slice above: import cut one global timeline into a
    // clip per sequence and kept the bracketing keys, so export merges them
    // back and a key two clips share is written once.
    mdx::Model model = makeModel();
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});

    const Document document = convert(model);
    REQUIRE_FALSE(document.clips.empty());

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());

    // The sequence table comes back with its own windows — they rode
    // `Clip::native`, which is what makes the merge exact rather than a
    // re-timing, so nothing reports one.
    REQUIRE(exported->sequences.size() == model.sequences.size());
    CHECK(exported->sequences[0].name == model.sequences[0].name);
    CHECK(exported->sequences[0].intervalStart == model.sequences[0].intervalStart);
    CHECK(exported->sequences[0].intervalEnd == model.sequences[0].intervalEnd);
    CHECK(exported.diagnostics.countOf(DiagCode::AnimClipRetimed) == 0u);

    // …and so does the track, on the bone it was keyed on, with both keys and
    // neither of them duplicated.
    REQUIRE_FALSE(exported->bones.empty());
    const mdx::Track<Vector3f>& track = exported->bones[0].node.translationTracks;
    REQUIRE(track.isUsed);
    REQUIRE(track.timestamps.size() == 2u);
    CHECK(track.timestamps[0] == 0u);
    CHECK(track.timestamps[1] == 1000u);
    REQUIRE(track.keys_data.size() == 2u);
    CHECK(track.keys_data[1].z == Catch::Approx(5.0f));
    CHECK(track.interpolationType == mdx::InterpolationType::Linear);
}

// ============================================================================
// UV state, the way StarCraft II says it
// ============================================================================

namespace {

/// @p document's material 0 with a `UvAnimation` feature on layer 0, and one
/// channel of @p kind targeting it in the `.m3` spelling: a two-float offset, a
/// three-float euler angle, a two-float tiling.
u32 addM3UvChannel(Document& document, Channel kind, geom::AttrType type,
                   std::vector<f32> times, std::vector<f32> values) {
    Model& model = document.models[0];
    ProfileMaterialSet* set = model.setFor(ProfileId::Wc3Classic);
    REQUIRE(set != nullptr);
    REQUIRE_FALSE(set->materials.empty());
    CommonMaterial& common = set->materials[0].MutableCommon();

    u32 featureId = kInvalidIndex;
    for (const MaterialFeature& feature : common.features) {
        if (feature.kind() == FeatureKind::UvAnimation) {
            featureId = feature.id;
        }
    }
    if (featureId == kInvalidIndex) {
        MaterialFeature feature;
        feature.id = NextFeatureId(common.features);
        feature.layer = 0;
        feature.payload = UvAnimationFeature{};
        featureId = feature.id;
        common.features.push_back(feature);
    }

    AnimChannel channel;
    channel.id = (std::max)(1u, model.animChannels.nextFreeId());
    channel.target.kind = TrackTarget::Kind::MaterialFeature;
    channel.target.material.profile = ProfileId::Wc3Classic;
    channel.target.material.slot = 0;
    channel.target.material.look = 0;
    channel.target.sub = featureId;
    channel.target.channel = kind;
    channel.valueType = type;
    model.animChannels.add(channel);

    SubTrack track;
    track.channel = channel.id;
    track.interp = Interpolation::Linear;
    track.times = std::move(times);
    track.values.resize(values.size() * sizeof(f32));
    std::memcpy(track.values.data(), values.data(), track.values.size());
    document.clips[0].containers[0].subTracks.push_back(std::move(track));
    return channel.id;
}

} // namespace

TEST_CASE("wem mdx a StarCraft II UV channel is restated, not reinterpreted",
          "[wem][anim][mdx][uv]") {
    // An `.m3` layer keys a Vector2 offset, a Vector3 of euler angles and a
    // Vector2 tiling; a `TextureAnimation` keys a Vector3, a quaternion and a
    // Vector3. Written straight through, `Emit` decoded each key as whatever
    // the destination holds and read four bytes past its end -- a Heroes
    // crystal exported the quaternion (0, 6.28, 0, -7.9e11).
    mdx::Model source = makeModel();
    source.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});
    Document document = convert(source);
    REQUIRE_FALSE(document.clips.empty());
    REQUIRE_FALSE(document.clips[0].containers.empty());

    addM3UvChannel(document, Channel::UvTranslate, geom::AttrType::F32x2, {0.0f, 1.0f},
                   {0.0f, 0.0f, 0.25f, -0.5f});
    addM3UvChannel(document, Channel::UvRotate, geom::AttrType::F32x3, {0.0f},
                   {0.0f, 0.0f, 1.5707963f});
    addM3UvChannel(document, Channel::UvScale, geom::AttrType::F32x2, {0.0f}, {2.0f, 3.0f});

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->textureAnimations.size() == 1u);
    const mdx::TextureAnimation& animation = exported->textureAnimations[0];

    // The translation: both engines apply it in source space inside the
    // (0.5, 0.5) pivot, StarCraft II subtracting where Warcraft III adds, so it
    // is the offset negated -- and its third component is a real zero rather
    // than whatever followed the key in memory.
    REQUIRE(animation.translationTracks.isUsed);
    REQUIRE(animation.translationTracks.keys_data.size() == 2u);
    CHECK(animation.translationTracks.keys_data[1].x == Catch::Approx(-0.25f));
    CHECK(animation.translationTracks.keys_data[1].y == Catch::Approx(0.5f));
    CHECK(animation.translationTracks.keys_data[1].z == 0.0f);

    // The rotation: `uvAngle.z` radians as a unit quaternion about z.
    REQUIRE(animation.rotationTracks.isUsed);
    REQUIRE(animation.rotationTracks.keys_data.size() == 1u);
    const Quaternion& turn = animation.rotationTracks.keys_data[0];
    CHECK(turn.x == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(turn.y == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(turn.z == Catch::Approx(0.70710678f));
    CHECK(turn.w == Catch::Approx(0.70710678f));

    // The tiling: widened, with the z MDX ignores left at one.
    REQUIRE(animation.scalingTracks.isUsed);
    REQUIRE(animation.scalingTracks.keys_data.size() == 1u);
    CHECK(animation.scalingTracks.keys_data[0].x == Catch::Approx(2.0f));
    CHECK(animation.scalingTracks.keys_data[0].y == Catch::Approx(3.0f));
    CHECK(animation.scalingTracks.keys_data[0].z == Catch::Approx(1.0f));
}

TEST_CASE("wem mdx a standing UV transform keeps its turn", "[wem][anim][mdx][uv]") {
    // The static half of the same crossing: 5981 StarCraft II layers and 15218
    // Heroes ones set a `uvAngle` and nothing else, and it reaches MDX only as
    // a one-key rotation track.
    Document document = convert(makeModel());
    ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Classic);
    REQUIRE(set != nullptr);
    TextureInput* input = set->materials[0].MutableCommon().inputAt(0);
    REQUIRE(input != nullptr);
    // A quarter turn about the texture centre, and the translation column that
    // pivot leaves behind: R * (uv - 0.5) + 0.5.
    input->uvTransform.m[0][0] = 0.0f;
    input->uvTransform.m[0][1] = -1.0f;
    input->uvTransform.m[1][0] = 1.0f;
    input->uvTransform.m[1][1] = 0.0f;
    input->uvTransform.m[0][2] = 1.0f;
    input->uvTransform.m[1][2] = 0.0f;

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->textureAnimations.size() == 1u);
    const mdx::TextureAnimation& animation = exported->textureAnimations[0];

    REQUIRE(animation.rotationTracks.isUsed);
    REQUIRE(animation.rotationTracks.keys_data.size() == 1u);
    CHECK(animation.rotationTracks.keys_data[0].z == Catch::Approx(0.70710678f));
    CHECK(animation.rotationTracks.keys_data[0].w == Catch::Approx(0.70710678f));
    // The pivot is the engine's own, so the translation stays at rest: a turn
    // about the centre needs no help from the KTAT.
    CHECK_FALSE(animation.translationTracks.isUsed);
    CHECK_FALSE(animation.scalingTracks.isUsed);
}

// ============================================================================
// Visibility gates
// ============================================================================

namespace {

/// A document from `makeModel`, with its one section gated on the root bone and
/// that bone's visibility declared at @p rest.
Document makeGatedDocument(f32 rest, u32* channelOut) {
    Document document = convert(makeModel());
    REQUIRE(document.models.size() == 1u);
    Model& model = document.models[0];
    REQUIRE(model.meshes.size() == 1u);
    REQUIRE_FALSE(model.meshes[0].sections.empty());
    REQUIRE_FALSE(model.nodes.empty());

    // Exactly what an M3 import leaves behind on a gated batch.
    model.meshes[0].sections[0].native.set(kSectionVisibilityNode, 0);

    AnimChannel gate;
    gate.id = 900;
    gate.target.kind = TrackTarget::Kind::Node;
    gate.target.node = 0;
    gate.target.channel = Channel::Visibility;
    gate.valueType = geom::AttrType::F32;
    gate.initValue.resize(sizeof(f32));
    std::memcpy(gate.initValue.data(), &rest, sizeof(f32));
    model.animChannels.add(gate);
    *channelOut = gate.id;
    return document;
}

} // namespace

TEST_CASE("wem mdx a section's visibility gate becomes a geoset animation",
          "[wem][anim][mdx]") {
    // M3 hides a BONE, not a geoset: a batch names the bone whose visibility
    // flag gates its draw. Warcraft III's only per-geoset visibility is a geoset
    // animation's alpha, so the gate has to be resolved on the way out or the
    // geoset draws unconditionally -- which is a Murky exported holding a shark
    // he only holds while riding one.
    u32 channel = 0;
    Document document = makeGatedDocument(0.0f, &channel);

    // "Stand" says visible; "Walk" says nothing at all.
    bool keyed = false;
    for (Clip& clip : document.clips) {
        if (clip.name != "Stand") {
            continue;
        }
        SubTrackContainer container;
        SubTrack track;
        track.channel = channel;
        // Deliberately linear: the source says so on 18,778 of StarCraft II's
        // 18,778 gates, and the export is expected to overrule it.
        track.interp = Interpolation::Linear;
        track.times = {0.0f};
        track.values.resize(sizeof(f32));
        const f32 visible = 1.0f;
        std::memcpy(track.values.data(), &visible, sizeof(f32));
        container.subTracks.push_back(std::move(track));
        clip.containers.push_back(std::move(container));
        keyed = true;
    }
    REQUIRE(keyed);

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->geosetAnimations.size() == 1u);
    const mdx::GeosetAnimation& animation = exported->geosetAnimations[0];
    CHECK(animation.geosetId == 0u);
    REQUIRE(animation.alphaTracks.isUsed);

    // A flag crosses as a flag. Linear between 0 and 1 is a fade, and the
    // engine that wrote this has no midpoint to fade through.
    CHECK(animation.alphaTracks.interpolationType == mdx::InterpolationType::None);

    // Two keys: the one "Stand" carries, and one at "Walk"'s start holding the
    // rest value -- MDX has one timeline where M3 had a clock per sequence, so
    // without it "Stand"'s answer would still be in force during "Walk".
    REQUIRE(animation.alphaTracks.timestamps.size() == 2u);
    CHECK(animation.alphaTracks.timestamps[0] == 0u);
    CHECK(animation.alphaTracks.timestamps[1] == 2000u);
    REQUIRE(animation.alphaTracks.keys_data.size() == 2u);
    CHECK(animation.alphaTracks.keys_data[0] == Catch::Approx(1.0f));
    CHECK(animation.alphaTracks.keys_data[1] == Catch::Approx(0.0f));
}

TEST_CASE("wem mdx a gate no clip drives is the rest value, once", "[wem][anim][mdx]") {
    // 2,813 of StarCraft II's 18,199 animated gates are keyed by no sequence at
    // all: the AnimRef states the answer and nothing ever changes it. A static
    // alpha of zero is the same thing a hidden section becomes.
    u32 channel = 0;
    const Document document = makeGatedDocument(0.0f, &channel);

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->geosetAnimations.size() == 1u);
    CHECK(exported->geosetAnimations[0].geosetId == 0u);
    CHECK(exported->geosetAnimations[0].alpha == Catch::Approx(0.0f));
    CHECK_FALSE(exported->geosetAnimations[0].alphaTracks.isUsed);
}

TEST_CASE("wem mdx a gate that rests visible writes nothing", "[wem][anim][mdx]") {
    // The other two thirds. A geoset that draws is what a geoset does, and a
    // record saying so is a record for nothing to read.
    u32 channel = 0;
    const Document document = makeGatedDocument(1.0f, &channel);

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    CHECK(exported->geosetAnimations.empty());
}

TEST_CASE("wem mdx a sequence's own extent survives the round trip", "[wem][anim][mdx]") {
    // A per-sequence bound is the one bound WEM stores rather than recomputes:
    // it is the union over the *posed* model across the clip, so recovering it
    // means evaluating the skeleton at a sampling the file never recorded. A
    // host frames its camera on it, and substituting the model's own bounds
    // frames every clip as though it were the widest one.
    mdx::Model model = makeModel();
    model.modelExtent.minimum = Vector3f{-100, -100, -100};
    model.modelExtent.maximum = Vector3f{100, 100, 100};
    model.modelExtent.boundsRadius = 173.0f;
    model.sequences[0].extent.minimum = Vector3f{-4, -5, 0};
    model.sequences[0].extent.maximum = Vector3f{4, 5, 12};
    model.sequences[0].extent.boundsRadius = 9.5f;

    const Document document = convert(model);
    REQUIRE(document.clips.size() >= 2u);
    CHECK(document.clips[0].bounds.maximum.z == Catch::Approx(12.0f));
    CHECK(document.clips[0].bounds.sphereRadius == Catch::Approx(9.5f));

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->sequences.size() == model.sequences.size());
    CHECK(exported->sequences[0].extent.minimum.y == Catch::Approx(-5.0f));
    CHECK(exported->sequences[0].extent.maximum.z == Catch::Approx(12.0f));
    CHECK(exported->sequences[0].extent.boundsRadius == Catch::Approx(9.5f));

    // The second sequence carried none, so it falls back to the model's — a
    // bound may err wide and must never err narrow.
    CHECK(exported->sequences[1].extent.maximum.z == Catch::Approx(100.0f));
}

TEST_CASE("wem mdx a fractional rarity survives the round trip", "[wem][anim][mdx]") {
    mdx::Model model = makeModel();
    model.sequences[0].rarity = 0.5f;
    model.sequences[1].rarity = 3.25f;

    const Document document = convert(model);
    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->sequences.size() == model.sequences.size());
    CHECK(exported->sequences[0].rarity == Catch::Approx(0.5f));
    CHECK(exported->sequences[1].rarity == Catch::Approx(3.25f));
}

TEST_CASE("wem mdx a whole-unit rarity from an older document still exports", "[wem][anim][mdx]") {
    Document document = convert(makeModel());
    for (Clip& clip : document.clips) {
        std::erase_if(clip.native.entries,
                      [](const NativeBag::Entry& entry) { return entry.name == "rarityMilli"; });
        clip.native.set("rarity", 2);
    }

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE_FALSE(exported->sequences.empty());
    CHECK(exported->sequences[0].rarity == Catch::Approx(2.0f));
}

TEST_CASE("wem mdx export re-times a clip that never had a window", "[wem][anim][mdx]") {
    // A clip from another format — or from an editor — carries no
    // `intervalStart`, and MDX's one timeline is the only clock it has. That is
    // a real re-timing and the export says so rather than placing it silently.
    mdx::Model model = makeModel();
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});

    Document document = convert(model);
    REQUIRE_FALSE(document.clips.empty());
    document.clips[0].native = NativeBag{};

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    // Every clip still becomes a sequence — the one whose window was taken away
    // is placed rather than dropped, and it is the only one reported.
    CHECK(exported->sequences.size() == document.clips.size());
    CHECK(exported.diagnostics.countOf(DiagCode::AnimClipRetimed) == 1u);
}

TEST_CASE("wem mdx export never gives a new global loop a stored loop's id", "[wem][anim][mdx]") {
    // A global loop that carries no id (an editor's, or another format's) comes
    // first in the document, and a stored loop after it owns id 0. Handing out
    // the next slot in document order would put both on sequence 0: one clock,
    // one duration, and two loops' keys merged onto it.
    mdx::Model model = makeModel();
    model.globalSequences = {4000, 2000};
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {0, 4000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});
    model.bones[0].node.translationTracks.globalSequenceId = 0;
    model.bones[0].node.rotationTracks = makeTrack<Quaternion>(
        mdx::InterpolationType::Linear, {0, 2000},
        {Quaternion{0, 0, 0, 1}, Quaternion{0, 0, 0.70710678f, 0.70710678f}});
    model.bones[0].node.rotationTracks.globalSequenceId = 1;

    Document document = convert(model);
    auto unnumbered = std::find_if(document.clips.begin(), document.clips.end(),
                                   [](const Clip& clip) { return clip.name == "globalSequence_1"; });
    REQUIRE(unnumbered != document.clips.end());
    unnumbered->native = NativeBag{};
    std::rotate(document.clips.begin(), unnumbered, unnumbered + 1);

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->globalSequences.size() == 2u);
    const mdx::Node& node = exported->bones[0].node;
    CHECK(node.translationTracks.globalSequenceId == 0u);
    CHECK(node.rotationTracks.globalSequenceId == 1u);
    CHECK(exported->globalSequences[0] == 4000u);
    CHECK(exported->globalSequences[1] == 2000u);
}

TEST_CASE("wem mdx export keys both edges of a window its track does not reach",
          "[wem][anim][mdx]") {
    // Warcraft III has no default value: a window plays its first key from its
    // start, and past the last key the engine wraps back toward the first.
    // Every other format holds outside its keys, so a clip that did not come
    // from an .mdx gets its first and last key restated at the two edges.
    mdx::Model model = makeModel();
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {200, 800}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});
    Document document = convert(model);
    REQUIRE_FALSE(document.clips.empty());
    const f32 duration = document.clips[0].duration;
    document.clips[0].native = NativeBag{};

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE_FALSE(exported->sequences.empty());
    const u32 start = exported->sequences[0].intervalStart;
    const u32 end = exported->sequences[0].intervalEnd;
    CHECK(end - start == static_cast<u32>(duration * 1000.0f + 0.5f));

    // The window lands after Walk's, and Walk still carries its bracket copy
    // of the 800 ms key at its old place, where no window plays it. Only the
    // keys inside the window are this clip's.
    const mdx::Track<Vector3f>& track = exported->bones[0].node.translationTracks;
    std::vector<u32> times;
    std::vector<f32> z;
    for (std::size_t k = 0; k < track.timestamps.size(); ++k) {
        if (track.timestamps[k] >= start && track.timestamps[k] <= end) {
            times.push_back(track.timestamps[k]);
            z.push_back(track.keys_data[k].z);
        }
    }
    REQUIRE(times.size() == 4u);
    CHECK(times[0] == start);
    CHECK(times[1] == start + 200u);
    CHECK(times[2] == start + 800u);
    CHECK(times[3] == end);
    CHECK(z[0] == Catch::Approx(0.0f));
    CHECK(z[3] == Catch::Approx(5.0f));
}

TEST_CASE("wem mdx export drops a clip's keys past its window and samples the edge",
          "[wem][anim][mdx]") {
    // A source that is not an .mdx plays nothing past its clip, and on the one
    // timeline a key past the window lands inside the next sequence and plays
    // there. What the window's end gets instead is the value the source shows
    // at it.
    mdx::Model model = makeModel();
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {500, 1500}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 10}});
    Document document = convert(model);
    REQUIRE_FALSE(document.clips.empty());
    document.clips.resize(1);
    document.clips[0].native = NativeBag{};
    // The slicer kept the key past the window as a bracket; the clip is 1 s.
    REQUIRE(document.clips[0].duration == Catch::Approx(1.0f));

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    const u32 start = exported->sequences[0].intervalStart;
    const u32 end = exported->sequences[0].intervalEnd;
    const mdx::Track<Vector3f>& track = exported->bones[0].node.translationTracks;
    REQUIRE(track.timestamps.size() == 3u);
    CHECK(track.timestamps[0] == start);
    CHECK(track.timestamps[1] == start + 500u);
    CHECK(track.timestamps[2] == end);
    // Before its first key the source holds it; at the end it is half way to
    // the key the window never reaches.
    CHECK(track.keys_data[0].z == Catch::Approx(0.0f));
    CHECK(track.keys_data[2].z == Catch::Approx(5.0f));
}

TEST_CASE("wem mdx export lets the moving clip name a shared track's interpolation",
          "[wem][anim][mdx]") {
    // One MDX track has one interpolation. A step written over a ramp turns
    // every key into a jolt; a ramp written over a step only softens a hold.
    mdx::Model model = makeModel();
    model.bones[0].node.translationTracks =
        makeTrack<Vector3f>(mdx::InterpolationType::Linear, {0, 1000, 2000, 3000},
                            {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}, Vector3f{0, 0, 2},
                             Vector3f{0, 0, 3}});
    Document document = convert(model);
    REQUIRE(document.clips.size() >= 2u);
    for (SubTrack& track : document.clips[0].containers[0].subTracks) {
        track.interp = Interpolation::Step;
    }

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    CHECK(exported->bones[0].node.translationTracks.interpolationType ==
          mdx::InterpolationType::Linear);
    CHECK(exported.diagnostics.countOf(DiagCode::AnimTrackApproximated) == 1u);
}

// ============================================================================
// Where the export puts things, and what it writes for an edited clip
// ============================================================================

namespace {

/// Every node record @p model holds, by object id; null for an id nothing
/// holds. @p duplicates counts ids two records claim.
std::vector<const mdx::Node*> nodesById(const mdx::Model& model, u32& duplicates) {
    std::vector<const mdx::Node*> byId;
    const auto put = [&](const mdx::Node& node) {
        if (byId.size() <= node.objectId) {
            byId.resize(node.objectId + 1u, nullptr);
        }
        duplicates += byId[node.objectId] != nullptr ? 1u : 0u;
        byId[node.objectId] = &node;
    };
    for (const auto& record : model.bones) put(record.node);
    for (const auto& record : model.lights) put(record.node);
    for (const auto& record : model.helpers) put(record.node);
    for (const auto& record : model.attachments) put(record.node);
    for (const auto& record : model.particleEmitters) put(record.node);
    for (const auto& record : model.particleEmitters2) put(record.node);
    for (const auto& record : model.ribbonEmitters) put(record.node);
    for (const auto& record : model.cornEmitters) put(record.node);
    for (const auto& record : model.eventObjects) put(record.node);
    for (const auto& record : model.collisionShapes) put(record.node);
    return byId;
}

/// How many of @p document's nodes and clips @p map puts somewhere other than
/// where @p exported has them.
u32 mapMismatches(const Document& document, const MdxExportMap& map, const mdx::Model& exported) {
    u32 wrong = 0;
    const Model& model = document.models[0];
    const std::vector<const mdx::Node*> byId = nodesById(exported, wrong);
    if (map.nodeObjectId.size() != model.nodes.size() ||
        map.clipSequence.size() != document.clips.size()) {
        return wrong + 1u;
    }
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const u32 id = map.nodeObjectId[i];
        if (model.nodes.nodes[i].kind == NodeKind::Camera) {
            wrong += id != kInvalidIndex ? 1u : 0u;
        } else if (id >= byId.size() || byId[id] == nullptr ||
                   byId[id]->name != model.nodes.nodes[i].name) {
            ++wrong;
        }
    }
    for (std::size_t c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        const u32 sequence = map.clipSequence[c];
        const bool global = hasFlag(clip.flags, ClipFlags::AutoPlay) &&
                            hasFlag(clip.flags, ClipFlags::WorldClocked);
        if (clip.model != 0u || global) {
            wrong += sequence != kInvalidIndex ? 1u : 0u;
        } else if (sequence >= exported.sequences.size() ||
                   exported.sequences[sequence].name != clip.name) {
            ++wrong;
        }
    }
    return wrong;
}

} // namespace

TEST_CASE("wem mdx the export map says where toMdx puts nodes and clips", "[wem][anim][mdx]") {
    // A global loop between two animations: it is a clip in the document but no
    // sequence in the file, so the animation after it is sequence 1, not 2.
    mdx::Model model = makeModel();
    model.globalSequences = {500};
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {0, 500}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});
    model.bones[0].node.translationTracks.globalSequenceId = 0;
    mdx::Helper helper;
    helper.node = makeNode("helper", 1, 0);
    model.helpers.push_back(helper);
    model.pivotPoints.push_back(Vector3f{0, 0, 1});

    Document document = convert(model);
    auto loop = std::find_if(document.clips.begin(), document.clips.end(), [](const Clip& clip) {
        return hasFlag(clip.flags, ClipFlags::WorldClocked);
    });
    REQUIRE(loop != document.clips.end());
    const Clip moved = *loop;
    document.clips.erase(loop);
    document.clips.insert(document.clips.begin() + 1, moved);
    REQUIRE(document.clips[0].name == "Stand");
    REQUIRE(document.clips[2].name == "Walk");

    const MdxExportMap map = MdxExportMapOf(document, 0, ProfileId::Wc3Classic);
    REQUIRE(map.clipSequence.size() == 3u);
    CHECK(map.clipSequence[0] == 0u);
    CHECK(map.clipSequence[1] == kInvalidIndex);
    CHECK(map.clipSequence[2] == 1u);

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->sequences.size() == 2u);
    CHECK(exported->sequences[1].name == "Walk");
    CHECK(mapMismatches(document, map, *exported) == 0u);
    CHECK(MdxExportMapOf(document, 1, ProfileId::Wc3Classic).nodeObjectId.empty());
}

TEST_CASE("wem mdx a clip without a window lands after every stored window",
          "[wem][anim][mdx]") {
    // An editor takes a clip's window away, and that clip can be anywhere in the
    // list. Placed after only the windows ahead of it, the middle one here would
    // land on top of the last.
    mdx::Model model = makeModel();
    mdx::Sequence fly;
    fly.name = "Fly";
    fly.intervalStart = 4000;
    fly.intervalEnd = 5000;
    model.sequences.push_back(fly);
    model.bones[0].node.translationTracks = makeTrack<Vector3f>(
        mdx::InterpolationType::Linear, {2000, 3000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 5}});

    Document document = convert(model);
    REQUIRE(document.clips.size() == 3u);
    REQUIRE(document.clips[1].name == "Walk");
    document.clips[1].native = NativeBag{};

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    const std::vector<mdx::Sequence>& sequences = exported->sequences;
    REQUIRE(sequences.size() == 3u);
    CHECK(sequences[1].name == "Walk");
    CHECK(sequences[1].intervalStart == 6000u);
    CHECK(sequences[1].intervalEnd == 7000u);
    for (std::size_t a = 0; a < sequences.size(); ++a) {
        for (std::size_t b = a + 1; b < sequences.size(); ++b) {
            CHECK((sequences[a].intervalEnd < sequences[b].intervalStart ||
                   sequences[b].intervalEnd < sequences[a].intervalStart));
        }
    }
    CHECK(exported.diagnostics.countOf(DiagCode::AnimClipRetimed) == 1u);

    // Its keys went with it, and the key at 3000 is at the new window's end.
    const mdx::Track<Vector3f>& track = exported->bones[0].node.translationTracks;
    CHECK(std::find(track.timestamps.begin(), track.timestamps.end(), 7000u) !=
          track.timestamps.end());
}

namespace {

/// The first clip of @p model's document, alone and without a window, exported:
/// what an editor's clip looks like on its way out.
mdx::Model exportWindowless(const mdx::Model& model) {
    Document document = convert(model);
    REQUIRE_FALSE(document.clips.empty());
    document.clips.resize(1);
    document.clips[0].native = NativeBag{};
    MdxConverter converter;
    Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->sequences.size() == 1u);
    REQUIRE(exported->sequences[0].intervalStart == 0u);
    return std::move(*exported.value);
}

template <class T>
bool same(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

} // namespace

TEST_CASE("wem mdx a made-up edge of a tangent stream is written as a hold",
          "[wem][anim][mdx]") {
    // Keys at 300 and 700 in a 1 s clip: the export makes up the keys at 0 and
    // 1000. Each is the hold the source plays there, so its tangents are flat,
    // and so is the tangent of the real key that faces it. The two tangents
    // between the real keys are untouched.
    mdx::Model model = makeModel();

    SECTION("a Hermite vector: flat is zero") {
        const Vector3f v0{0, 0, 1}, in0{1, 2, 3}, out0{4, 5, 6};
        const Vector3f v1{0, 0, 2}, in1{7, 8, 9}, out1{10, 11, 12};
        model.bones[0].node.translationTracks = makeTrack<Vector3f>(
            mdx::InterpolationType::Hermite, {300, 700}, {v0, in0, out0, v1, in1, out1});
        const mdx::Model exported = exportWindowless(model);
        const mdx::Track<Vector3f>& track = exported.bones[0].node.translationTracks;
        CHECK(track.interpolationType == mdx::InterpolationType::Hermite);
        REQUIRE(track.timestamps == std::vector<u32>{0, 300, 700, 1000});
        const auto keys = track.tangentKeys();
        const Vector3f zero{0, 0, 0};
        CHECK(same(keys[0].value, v0));
        CHECK(same(keys[0].inTan, zero));
        CHECK(same(keys[0].outTan, zero));
        CHECK(same(keys[1].inTan, zero));
        CHECK(same(keys[1].outTan, out0));
        CHECK(same(keys[2].inTan, in1));
        CHECK(same(keys[2].outTan, zero));
        CHECK(same(keys[3].value, v1));
        CHECK(same(keys[3].inTan, zero));
        CHECK(same(keys[3].outTan, zero));
    }

    SECTION("a Bezier vector: flat is the key's own value") {
        const Vector3f v0{0, 0, 1}, in0{1, 2, 3}, out0{4, 5, 6};
        const Vector3f v1{0, 0, 2}, in1{7, 8, 9}, out1{10, 11, 12};
        model.bones[0].node.translationTracks = makeTrack<Vector3f>(
            mdx::InterpolationType::Bezier, {300, 700}, {v0, in0, out0, v1, in1, out1});
        const mdx::Model exported = exportWindowless(model);
        const mdx::Track<Vector3f>& track = exported.bones[0].node.translationTracks;
        CHECK(track.interpolationType == mdx::InterpolationType::Bezier);
        REQUIRE(track.timestamps == std::vector<u32>{0, 300, 700, 1000});
        const auto keys = track.tangentKeys();
        CHECK(same(keys[0].inTan, v0));
        CHECK(same(keys[0].outTan, v0));
        CHECK(same(keys[1].inTan, v0));
        CHECK(same(keys[1].outTan, out0));
        CHECK(same(keys[2].inTan, in1));
        CHECK(same(keys[2].outTan, v1));
        CHECK(same(keys[3].inTan, v1));
        CHECK(same(keys[3].outTan, v1));
    }

    SECTION("a Hermite quaternion: flat is the key's own value, squad's control point") {
        const Quaternion q0{0, 0, 0, 1}, a0{0.1f, 0, 0, 0.995f}, b0{0.2f, 0, 0, 0.98f};
        const Quaternion q1{0, 0, 0.70710678f, 0.70710678f}, a1{0, 0.1f, 0, 0.995f},
            b1{0, 0.2f, 0, 0.98f};
        model.bones[0].node.rotationTracks = makeTrack<Quaternion>(
            mdx::InterpolationType::Hermite, {300, 700}, {q0, a0, b0, q1, a1, b1});
        const mdx::Model exported = exportWindowless(model);
        const mdx::Track<Quaternion>& track = exported.bones[0].node.rotationTracks;
        CHECK(track.interpolationType == mdx::InterpolationType::Hermite);
        REQUIRE(track.timestamps == std::vector<u32>{0, 300, 700, 1000});
        const auto keys = track.tangentKeys();
        CHECK(same(keys[0].inTan, q0));
        CHECK(same(keys[0].outTan, q0));
        CHECK(same(keys[1].inTan, q0));
        CHECK(same(keys[1].outTan, b0));
        CHECK(same(keys[2].inTan, a1));
        CHECK(same(keys[2].outTan, q1));
        CHECK(same(keys[3].inTan, q1));
        CHECK(same(keys[3].outTan, q1));
    }

    SECTION("a lone key is flat on both sides") {
        const Vector3f v{0, 0, 3}, in{1, 2, 3}, out{4, 5, 6};
        model.bones[0].node.translationTracks =
            makeTrack<Vector3f>(mdx::InterpolationType::Hermite, {500}, {v, in, out});
        const mdx::Model exported = exportWindowless(model);
        const mdx::Track<Vector3f>& track = exported.bones[0].node.translationTracks;
        REQUIRE(track.timestamps == std::vector<u32>{0, 500, 1000});
        const auto keys = track.tangentKeys();
        const Vector3f zero{0, 0, 0};
        for (const auto& key : keys) {
            CHECK(same(key.value, v));
            CHECK(same(key.inTan, zero));
            CHECK(same(key.outTan, zero));
        }
    }
}

// ============================================================================
// The corpus arm
// ============================================================================

TEST_CASE("wem mdx a second HD layer keeps its tracks through the native block",
          "[wem][anim][mdx]") {
    // Two HD layers that each set all six slots: the second re-sets the first
    // one's, so where its first slot landed was one past the body's end. Its
    // ordinal is its position, which is what the export — writing the native
    // block layer by layer — reads it back by.
    mdx::Model model = makeModel();
    model.version = 1200;
    for (u32 t = 1; t < 6; ++t) {
        mdx::Texture texture;
        texture.fileName = "textures/slot" + std::to_string(t) + ".dds";
        model.textures.push_back(texture);
    }
    mdx::Material material;
    for (u32 l = 0; l < 2; ++l) {
        mdx::Layer layer;
        layer.filterMode = l == 0 ? mdx::Layer::FilterMode::None : mdx::Layer::FilterMode::Additive;
        layer.shader = mdx::Layer::ShaderType::HD;
        layer.is_hd = true;
        layer.textureAnimationId = 0xFFFFFFFF;
        for (u32 s = 0; s < 6; ++s) {
            mdx::Layer::SubTexture sub;
            sub.textureId = s;
            sub.slot = static_cast<mdx::Layer::SlotType>(s);
            layer.subTextures.push_back(sub);
        }
        material.layers.push_back(layer);
    }
    material.layers[1].alphaTracks =
        makeTrack<f32>(mdx::InterpolationType::Linear, {0, 1000}, {1.0f, 0.25f});
    model.materials[0] = material;

    const Document document = convert(model);
    const AnimChannelTable& table = document.models[0].animChannels;
    REQUIRE(table.channels.size() == 1u);
    CHECK(table.channels[0].target.material.profile == ProfileId::Wc3Reforged);
    CHECK(table.channels[0].target.sub == 1u);
    CHECK(Validate(document, ValidateLevel::Profile).countOf(DiagCode::IndexOutOfRange) == 0u);

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Reforged, 1200);
    REQUIRE(exported.ok());
    REQUIRE(exported->materials.size() == 1u);
    REQUIRE(exported->materials[0].layers.size() == 2u);
    CHECK_FALSE(exported->materials[0].layers[0].alphaTracks.isUsed);
    const mdx::Track<f32>& alpha = exported->materials[0].layers[1].alphaTracks;
    REQUIRE(alpha.isUsed);
    REQUIRE(alpha.keys_data.size() == 2u);
    CHECK(alpha.keys_data[1] == Catch::Approx(0.25f));
}

namespace {

/// A two-frame flipbook, stepped.
mdx::Track<u32> flipbookTrack() {
    return makeTrack<u32>(mdx::InterpolationType::None, {0, 500}, {0u, 1u});
}

/// Where the writer reads a layer's flipbook at @p version (`writer.cpp`).
const mdx::Track<u32>& writtenFlipbook(const mdx::Layer& layer, u32 version) {
    return version >= 1100 && !layer.subTextures.empty() ? layer.subTextures[0].tracks
                                                         : layer.textureIdTracks;
}

u32 countChannels(const Document& document, Channel channel) {
    u32 count = 0;
    for (const AnimChannel& declared : document.models[0].animChannels.channels) {
        count += declared.target.channel == channel ? 1u : 0u;
    }
    return count;
}

} // namespace

TEST_CASE("wem mdx a flipbook crosses from where each version keeps it", "[wem][anim][mdx]") {
    // The parser leaves a v800 layer's KMTF on the layer, moves a v900-1000
    // one onto sub-texture 0 and reads a v1100 one there. The import read only
    // the layer's own, so every Reforged-era flipbook became no channel, and
    // the export wrote the track where the writer drops it from v1100 on.
    for (const u32 version : {800u, 1200u}) {
        CAPTURE(version);
        mdx::Model model = makeModel();
        model.version = version;
        mdx::Texture second;
        second.fileName = "textures/frame1.blp";
        model.textures.push_back(second);
        mdx::Layer& layer = model.materials[0].layers[0];
        if (version >= 1100) {
            mdx::Layer::SubTexture sub;
            sub.textureId = 0;
            sub.slot = mdx::Layer::SlotType::DiffuseMap;
            sub.tracks = flipbookTrack();
            layer.subTextures.push_back(sub);
        } else {
            layer.textureIdTracks = flipbookTrack();
        }

        const Document document = convert(model);
        const AnimChannelTable& table = document.models[0].animChannels;
        REQUIRE(countChannels(document, Channel::TextureIndex) >= 1u);
        ProfileId profile = ProfileId::Wc3Classic;
        for (const AnimChannel& declared : table.channels) {
            if (declared.target.channel == Channel::TextureIndex) {
                CHECK(declared.target.kind == TrackTarget::Kind::MaterialLayer);
                CHECK(declared.target.sub == 0u);
                CHECK(declared.valueType == geom::AttrType::U32);
                profile = declared.target.material.profile;
            }
        }

        MdxConverter converter;
        const Result<mdx::Model> exported = converter.toMdx(document, profile, version);
        REQUIRE(exported.ok());
        REQUIRE_FALSE(exported->materials.empty());
        REQUIRE_FALSE(exported->materials[0].layers.empty());
        const mdx::Layer& out = exported->materials[0].layers[0];
        const mdx::Track<u32>& written = writtenFlipbook(out, version);
        REQUIRE(written.isUsed);
        CHECK(written.timestamps == std::vector<u32>{0u, 500u});
        CHECK(written.keys_data == std::vector<u32>{0u, 1u});

        // And through the bytes: what the writer writes, the parser reads back.
        mdx::Writer writer;
        const std::vector<u8> bytes = writer.write(*exported);
        mdx::Parser parser;
        const mdx::Model reread = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        REQUIRE_FALSE(reread.materials.empty());
        REQUIRE_FALSE(reread.materials[0].layers.empty());
        const mdx::Track<u32>& back = writtenFlipbook(reread.materials[0].layers[0], version);
        REQUIRE(back.isUsed);
        CHECK(back.timestamps == std::vector<u32>{0u, 500u});
        CHECK(back.keys_data == std::vector<u32>{0u, 1u});
    }
}

TEST_CASE("wem mdx a flipbook on another HD slot is reported, not silently dropped",
          "[wem][anim][mdx]") {
    // A `MaterialLayer` target names a layer and has no field for a slot, so an
    // HD layer's normal-map flipbook has nowhere to go yet. Slot 0 still does.
    mdx::Model model = makeModel();
    model.version = 1200;
    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::None;
    layer.shader = mdx::Layer::ShaderType::HD;
    layer.is_hd = true;
    layer.textureAnimationId = 0xFFFFFFFF;
    for (u32 s = 0; s < 6; ++s) {
        mdx::Layer::SubTexture sub;
        sub.textureId = 0;
        sub.slot = static_cast<mdx::Layer::SlotType>(s);
        layer.subTextures.push_back(sub);
    }
    layer.subTextures[0].tracks = flipbookTrack();
    layer.subTextures[2].tracks = flipbookTrack();
    model.materials[0].layers[0] = layer;

    MdxConverter converter;
    const Result<Document> result = converter.fromMdx(model);
    REQUIRE(result.ok());
    CHECK(countChannels(*result.value, Channel::TextureIndex) == 1u);
    CHECK(result.diagnostics.countOf(DiagCode::AnimTrackDropped) == 1u);
}

TEST_CASE("wem mdx export gives each layer back its own texture animation",
          "[wem][anim][mdx][uv]") {
    // The file's TXAN table is not kept: export rebuilds it from the UV
    // features, handing out entries in channel order and reading an id below
    // the table's size as one already handed out. A layer that kept the id it
    // was read with therefore pointed into the new table at whatever landed
    // there.
    mdx::Model model = makeModel();
    model.materials.push_back(model.materials[0]);
    mdx::Geoset second = model.geosets[0];
    second.materialId = 1;
    model.geosets.push_back(second);
    const auto scroll = [](Vector3f to) {
        mdx::TextureAnimation animation;
        animation.translationTracks = makeTrack<Vector3f>(mdx::InterpolationType::Linear,
                                                          {0, 1000}, {Vector3f{0, 0, 0}, to});
        return animation;
    };

    const auto exportedScroll = [](const mdx::Model& exported, u32 material) {
        const u32 id = exported.materials[material].layers[0].textureAnimationId;
        if (id >= exported.textureAnimations.size()) {
            return std::optional<Vector3f>();
        }
        const mdx::Track<Vector3f>& track = exported.textureAnimations[id].translationTracks;
        return track.isUsed && !track.keys_data.empty() ? std::optional(track.keys_data.back())
                                                        : std::optional<Vector3f>();
    };

    MdxConverter converter;
    SECTION("a later layer's kept id is another layer's new one") {
        // Material 0 is exported first and handed entry 0 — the id material 1
        // was read with, so its scroll went into material 0's TXAN.
        model.textureAnimations = {scroll(Vector3f{0, 2, 0}), scroll(Vector3f{1, 0, 0})};
        model.materials[0].layers[0].textureAnimationId = 1;
        model.materials[1].layers[0].textureAnimationId = 0;

        const Result<mdx::Model> exported =
            converter.toMdx(convert(model), ProfileId::Wc3Classic);
        REQUIRE(exported.ok());
        REQUIRE(exported->materials.size() == 2u);
        const std::optional<Vector3f> first = exportedScroll(*exported, 0);
        const std::optional<Vector3f> other = exportedScroll(*exported, 1);
        REQUIRE(first.has_value());
        REQUIRE(other.has_value());
        CHECK(first->x == Catch::Approx(1.0f));
        CHECK(first->y == Catch::Approx(0.0f));
        CHECK(other->x == Catch::Approx(0.0f));
        CHECK(other->y == Catch::Approx(2.0f));
        CHECK(exported->textureAnimations.size() == 2u);
    }
    SECTION("a kept id to a TXAN that moves nothing is another layer's scroll") {
        // Material 1's TXAN has no keys, so it has no feature and no channel —
        // and its kept id 0 is the entry material 0's scroll is handed.
        model.textureAnimations = {mdx::TextureAnimation{}, scroll(Vector3f{1, 0, 0})};
        model.materials[0].layers[0].textureAnimationId = 1;
        model.materials[1].layers[0].textureAnimationId = 0;

        const Result<mdx::Model> exported =
            converter.toMdx(convert(model), ProfileId::Wc3Classic);
        REQUIRE(exported.ok());
        REQUIRE(exported->materials.size() == 2u);
        REQUIRE(exportedScroll(*exported, 0).has_value());
        CHECK(exportedScroll(*exported, 0)->x == Catch::Approx(1.0f));
        CHECK_FALSE(exportedScroll(*exported, 1).has_value());
    }
}

TEST_CASE("wem mdx animation survives the corpus", "[wem][anim][mdx][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        WARN("no .mdx corpus found; set WEM_MDX_CORPUS_DIR");
        return;
    }

    const std::size_t limit = test::sweepLimit(files.size(), 200);
    u32 imported = 0;
    u32 withSequences = 0;
    u32 animated = 0;
    u32 clips = 0;
    u32 autoPlay = 0;
    u32 channels = 0;
    u32 subTracks = 0;
    u32 events = 0;
    u32 sectionChannels = 0;
    u32 materialChannels = 0;
    u32 validationErrors = 0;
    std::vector<std::string> failing;

    MdxConverter converter;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        {
            mdx::Parser probe;
            const mdx::Model parsed = probe.parse(std::span<const u8>(bytes.data(), bytes.size()));
            if (!parsed.sequences.empty()) {
                ++withSequences;
            }
        }
        Result<Document> converted =
            converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!converted.ok()) {
            continue;
        }
        ++imported;
        const Document& document = *converted.value;
        if (!document.clips.empty()) {
            ++animated;
        }
        clips += static_cast<u32>(document.clips.size());
        for (const Clip& clip : document.clips) {
            if (hasFlag(clip.flags, ClipFlags::AutoPlay)) {
                ++autoPlay;
            }
            events += static_cast<u32>(clip.events.size());
            for (const SubTrackContainer& container : clip.containers) {
                subTracks += static_cast<u32>(container.subTracks.size());
            }
        }
        for (const Model& model : document.models) {
            channels += static_cast<u32>(model.animChannels.channels.size());
            for (const AnimChannel& channel : model.animChannels.channels) {
                if (channel.target.kind == TrackTarget::Kind::Section) {
                    ++sectionChannels;
                } else if (IsMaterialTarget(channel.target.kind)) {
                    ++materialChannels;
                }
            }
        }

        const Diagnostics report = Validate(document, ValidateLevel::Profile);
        for (const Diagnostic& issue : report.all()) {
            if (issue.severity != Severity::Error) {
                continue;
            }
            ++validationErrors;
            if (failing.size() < 8) {
                failing.push_back(test::pathText(files[i].filename()) + ": " + issue.message);
            }
        }
    }

    std::cout << "mdx animation: " << imported << " imported, " << withSequences
              << " with sequences, " << animated << " animated, " << clips << " clips (" << autoPlay
              << " auto-play), " << channels << " channels (" << sectionChannels << " section, "
              << materialChannels << " material), " << subTracks << " sub-tracks, " << events
              << " events" << std::endl;
    for (const std::string& line : failing) {
        std::cout << "  " << line << std::endl;
    }

    REQUIRE(imported > 0);
    // The gate that matters, and the reason `withSequences` is counted at all:
    // most of this corpus is doodads and tiles with no `SEQS` chunk, so "75 of
    // 198 animated" looks like a dropped import until the parse says the other
    // 123 carry no sequences. Every file that has them gets clips.
    CHECK(animated == withSequences);
    CHECK(clips > 0);
    CHECK(channels > 0);
    CHECK(subTracks > 0);
    // The one that is not a count: a channel table nothing can resolve is what a
    // green conversion would hide (§16's testing note).
    CHECK(validationErrors == 0u);
}

TEST_CASE("wem mdx the export map agrees with toMdx across the corpus",
          "[wem][anim][mdx][corpus]") {
    // A host finds a document node in the renderer through this map, so a node
    // the map sends to another record is a marker on the wrong bone.
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        WARN("no .mdx corpus found; set WEM_MDX_CORPUS_DIR");
        return;
    }
    const std::size_t limit = test::sweepLimit(files.size(), 200);
    u32 exports = 0;
    u32 nodes = 0;
    u32 clips = 0;
    u32 mismatches = 0;
    std::vector<std::string> failing;
    MdxConverter converter;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        const Result<Document> converted =
            converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!converted.ok() || converted->models.empty()) {
            continue;
        }
        for (const ProfileId profile : {ProfileId::Wc3Classic, ProfileId::Wc3Reforged}) {
            const Result<mdx::Model> exported = converter.toMdx(*converted.value, profile);
            if (!exported.ok()) {
                continue;
            }
            ++exports;
            nodes += static_cast<u32>(converted->models[0].nodes.size());
            clips += static_cast<u32>(converted->clips.size());
            const u32 wrong = mapMismatches(
                *converted.value, MdxExportMapOf(*converted.value, 0, profile), *exported);
            mismatches += wrong;
            if (wrong != 0 && failing.size() < 8) {
                failing.push_back(test::pathText(files[i].filename()) + ": " +
                                  std::to_string(wrong));
            }
        }
    }
    std::cout << "mdx export map: " << exports << " exports, " << nodes << " nodes, " << clips
              << " clips, " << mismatches << " misplaced" << std::endl;
    for (const std::string& line : failing) {
        std::cout << "  " << line << std::endl;
    }
    REQUIRE(exports > 0u);
    CHECK(mismatches == 0u);
}

TEST_CASE("wem mdx bone gates and geoset flags survive the corpus round trip",
          "[wem][anim][mdx][corpus][gate]") {
    // G19 (EDIT_MODE_MESH_PLAN.md L1). A bone's link to its geoset hides the
    // parts of the rig that hang there while the geoset is invisible, and the
    // export rebuilds the geoset-animation table the file's raw indices named:
    // carried raw, 1,252 of 3,627 gates moved to another geoset and 160 fell off
    // the table. Every gate is resolved the way the renderer and the game
    // resolve it, before and after, at the profile the file is written for.
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        WARN("no .mdx corpus found; set WEM_MDX_CORPUS_DIR");
        return;
    }
    const std::size_t limit = test::sweepLimit(files.size(), 200);
    constexpr u32 kSentinel = mdx::Bone::MULTIPLE_GEOSETS;
    const auto gateOf = [](const mdx::Model& model, const mdx::Bone& bone) {
        if (bone.geosetId == kSentinel || bone.geosetAnimationId >= model.geosetAnimations.size()) {
            return kInvalidIndex;
        }
        const u32 geoset = model.geosetAnimations[bone.geosetAnimationId].geosetId;
        return geoset < model.geosets.size() ? geoset : kInvalidIndex;
    };
    const auto firstRecord = [](const mdx::Model& model, u32 geoset) -> const mdx::GeosetAnimation* {
        for (const mdx::GeosetAnimation& record : model.geosetAnimations) {
            if (record.geosetId == geoset) {
                return &record;
            }
        }
        return nullptr;
    };

    struct Counts {
        u32 exports = 0;
        u32 gated = 0;
        u32 same = 0;
        u32 moved = 0;
        u32 lost = 0;
        u32 dropShadow = 0;
        u32 dropShadowKept = 0;
        u32 unnamedBits = 0; ///< Records whose word carries bits beyond 0x3.
        u32 unnamedKept = 0;
    };
    Counts atDefault;
    Counts atOther;
    std::vector<std::string> failing;
    const MdxConverter converter;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Model source;
        try {
            mdx::Parser parser;
            source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        } catch (const std::exception&) {
            continue; // the parser suite's business
        }
        const Result<Document> converted = converter.fromMdx(source);
        if (!converted.ok() || converted->models.empty()) {
            continue;
        }
        const Document& document = *converted.value;
        for (const ProfileId profile : document.profiles) {
            // Reforged is written at v1000 by the host (`MdxVersionForWemProfile`).
            const u32 version = profile == ProfileId::Wc3Reforged ? 1000u : 800u;
            const Result<mdx::Model> exported = converter.toMdx(document, profile, version);
            if (!exported.ok()) {
                continue;
            }
            Counts& counts = profile == document.defaultProfile ? atDefault : atOther;
            ++counts.exports;
            // Bones are written in node order, which `fromMdx` took from the
            // object ids, so the i-th bone out is the i-th in.
            const bool paired = exported->bones.size() == source.bones.size();
            for (std::size_t b = 0; paired && b < source.bones.size(); ++b) {
                const u32 before = gateOf(source, source.bones[b]);
                if (before == kInvalidIndex) {
                    continue;
                }
                ++counts.gated;
                const u32 after = gateOf(*exported, exported->bones[b]);
                if (after == before && exported->bones[b].node.name == source.bones[b].node.name) {
                    ++counts.same;
                } else if (after == kInvalidIndex) {
                    ++counts.lost;
                } else {
                    ++counts.moved;
                }
                if (after != before && failing.size() < 8) {
                    failing.push_back(test::pathText(files[i].filename()) + " '" +
                                      source.bones[b].node.name + "': geoset " +
                                      std::to_string(before) + " -> " +
                                      (after == kInvalidIndex ? std::string("none")
                                                              : std::to_string(after)));
                }
            }
            for (u32 g = 0; g < source.geosets.size(); ++g) {
                const mdx::GeosetAnimation* was = firstRecord(source, g);
                if (was == nullptr) {
                    continue;
                }
                const u32 word = static_cast<u32>(was->flags);
                const mdx::GeosetAnimation* now = firstRecord(*exported, g);
                const u32 wrote = now != nullptr ? static_cast<u32>(now->flags) : 0x2u;
                if ((word & 0x1u) != 0) {
                    ++counts.dropShadow;
                    counts.dropShadowKept += (wrote & 0x1u) != 0 ? 1u : 0u;
                }
                if ((word & ~0x3u) != 0) {
                    ++counts.unnamedBits;
                    counts.unnamedKept += now != nullptr && wrote == word ? 1u : 0u;
                }
            }
        }
    }
    const auto report = [](const char* label, const Counts& c) {
        std::cout << "mdx bone gates " << label << ": " << c.exports << " exports, " << c.gated
                  << " gated bones, " << c.same << " unchanged, " << c.moved << " moved, "
                  << c.lost << " unresolvable; DropShadow " << c.dropShadowKept << " of "
                  << c.dropShadow << " kept; unnamed flag words " << c.unnamedKept << " of "
                  << c.unnamedBits << " kept" << std::endl;
    };
    report("at the default profile", atDefault);
    report("at the other profile", atOther);
    for (const std::string& line : failing) {
        std::cout << "  " << line << std::endl;
    }
    REQUIRE(atDefault.exports > 0u);
    for (const Counts* c : {&atDefault, &atOther}) {
        CHECK(c->moved == 0u);
        CHECK(c->lost == 0u);
        CHECK(c->dropShadowKept == c->dropShadow);
        CHECK(c->unnamedKept == c->unnamedBits);
    }
}

TEST_CASE("wem mdx flipbooks and light ambients cross the corpus", "[wem][anim][mdx][corpus]") {
    // A flipbook lives on the layer before v1100 and on each sub-texture from
    // it; a light's ambient colour and its two intensities have no payload
    // field. Both used to be lost on every Reforged-era file.
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        WARN("no .mdx corpus found; set WEM_MDX_CORPUS_DIR");
        return;
    }
    const std::size_t limit = test::sweepLimit(files.size(), 200);
    u32 bySlot[7] = {}; // [0] the layer's own KMTF, [1 + s] sub-texture s
    u32 flipbookMaterials = 0;
    u32 missedMaterials = 0;
    u32 channels = 0;
    u32 written = 0;
    u32 lights = 0;
    u32 lightMisses = 0;
    u32 fractionalIntensities = 0;
    std::vector<std::string> failing;

    MdxConverter converter;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        mdx::Parser parser;
        const mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        Result<Document> converted = converter.fromMdx(source);
        if (!converted.ok() || converted->models.empty()) {
            continue;
        }
        const Document& document = *converted.value;
        const std::string name = test::pathText(files[i].filename());

        // Every material with a slot-0 flipbook gets a channel on some layer.
        std::vector<bool> keyed(source.materials.size(), false);
        for (const AnimChannel& declared : document.models[0].animChannels.channels) {
            if (declared.target.kind == TrackTarget::Kind::MaterialLayer &&
                declared.target.channel == Channel::TextureIndex) {
                ++channels;
                if (declared.target.material.slot < keyed.size()) {
                    keyed[declared.target.material.slot] = true;
                }
            }
        }
        for (std::size_t m = 0; m < source.materials.size(); ++m) {
            bool flipbook = false;
            for (const mdx::Layer& layer : source.materials[m].layers) {
                bySlot[0] += layer.textureIdTracks.isUsed ? 1u : 0u;
                flipbook = flipbook || layer.textureIdTracks.isUsed;
                for (std::size_t s = 0; s < layer.subTextures.size() && s < 6; ++s) {
                    bySlot[1 + s] += layer.subTextures[s].tracks.isUsed ? 1u : 0u;
                }
                flipbook = flipbook ||
                           (!layer.subTextures.empty() && layer.subTextures[0].tracks.isUsed);
            }
            if (flipbook) {
                ++flipbookMaterials;
                if (!keyed[m]) {
                    ++missedMaterials;
                    if (failing.size() < 8) {
                        failing.push_back(name + ": material " + std::to_string(m) +
                                          " flipbooks and has no channel");
                    }
                }
            }
        }

        // The export writes every one of them back, where the writer reads it.
        for (const ProfileMaterialSet& set : document.models[0].profileSets) {
            const Result<mdx::Model> exported = converter.toMdx(document, set.profile, source.version);
            if (!exported.ok()) {
                continue;
            }
            for (const mdx::Material& material : exported->materials) {
                for (const mdx::Layer& layer : material.layers) {
                    written += writtenFlipbook(layer, source.version).isUsed ? 1u : 0u;
                }
            }
            if (set.profile != document.models[0].profileSets.front().profile) {
                continue;
            }
            // A light's static ambient, by its node's name.
            for (const mdx::Light& light : source.lights) {
                ++lights;
                const f32 intensities[] = {light.ambientIntensity, light.shadowIntensity};
                for (const f32 value : intensities) {
                    const f32 milli = value * 1000.0f;
                    fractionalIntensities += std::abs(milli - std::round(milli)) > 1e-3f ? 1u : 0u;
                }
                const mdx::Light* back = nullptr;
                for (const mdx::Light& candidate : exported->lights) {
                    if (candidate.node.name == light.node.name) {
                        back = &candidate;
                    }
                }
                const bool same = back != nullptr && back->ambientColor == light.ambientColor &&
                                  std::abs(back->ambientIntensity - light.ambientIntensity) <= 1e-3f &&
                                  std::abs(back->shadowIntensity - light.shadowIntensity) <= 1e-3f;
                if (!same) {
                    ++lightMisses;
                    if (failing.size() < 8) {
                        failing.push_back(name + ": light " + light.node.name +
                                          " lost its ambient or shadow");
                    }
                }
            }
        }
    }

    std::cout << "mdx flipbooks: layer KMTF " << bySlot[0] << ", sub-texture KMTF by slot";
    for (u32 s = 1; s < 7; ++s) {
        std::cout << " " << bySlot[s];
    }
    std::cout << "; " << flipbookMaterials << " materials flipbook, " << missedMaterials
              << " without a channel; " << channels << " channels, " << written
              << " written back" << std::endl;
    std::cout << "mdx light ambients: " << lights << " lights, " << lightMisses << " lost, "
              << fractionalIntensities << " intensities not whole thousandths" << std::endl;
    for (const std::string& line : failing) {
        std::cout << "  " << line << std::endl;
    }
    CHECK(missedMaterials == 0u);
    CHECK(written == channels);
    CHECK(lightMisses == 0u);
}

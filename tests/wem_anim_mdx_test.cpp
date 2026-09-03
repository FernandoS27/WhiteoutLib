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

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
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

TEST_CASE("wem mdx an emitter's property tracks are out of scope, its visibility is not",
          "[wem][anim][mdx]") {
    // §18: WEM stores an emitter's placement and an `AssetKey`, so animating an
    // emission rate would be storing the motion of something absent.
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
    REQUIRE(table.channels.size() == 1u);
    CHECK(table.channels[0].target.channel == Channel::Visibility);
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

// ============================================================================
// The corpus arm
// ============================================================================

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

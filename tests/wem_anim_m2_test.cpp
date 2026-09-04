// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P7 — the WoW animation import.
///
/// `.m2` tracks are already per sequence, so there is no slicing to get wrong.
/// What there is to get wrong is everything around that: fixed-point decoding,
/// which ordinal a texture track drives, and the difference between a colour
/// that multiplies the whole batch and a weight that multiplies one stage.
///
/// The corpus arm is tagged `[.m2slow]` for the reason the P5 sweep's is: 56
/// known corpus files drive the M2 parser into multi-GB zero-filled resizes, so
/// a full run needs a process commit cap set outside the test.

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m2/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/validate.h>
#include <whiteout/utils/os_file_system.h>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

template <class T>
m2::AnimationTrack<T> makeTrack(m2::InterpolationType interp, std::vector<std::vector<u32>> times,
                                std::vector<std::vector<T>> values) {
    m2::AnimationTrack<T> track;
    track.interpolationType = interp;
    track.timestamps = std::move(times);
    track.values = std::move(values);
    return track;
}

/// One bone, one submesh, one batch, two sequences.
m2::Model makeModel() {
    m2::Model model;
    model.modelName = "anim";

    m2::Sequence stand;
    stand.id = 0;
    stand.duration = 1000;
    stand.flags = m2::SequenceFlag::Looping;
    model.sequences.push_back(stand);

    m2::Sequence walk;
    walk.id = 4;
    walk.duration = 800;
    model.sequences.push_back(walk);

    m2::Bone bone;
    bone.parentBoneId = -1;
    bone.pivot = Vector3f{0, 0, 0};
    model.bones.push_back(bone);

    m2::Texture texture;
    texture.filename = "world/body.blp";
    model.textures.push_back(texture);
    model.textureCombos = {0};
    model.textureCoordCombos = {0};
    model.textureWeightCombos = {0};
    model.textureTransformCombos = {0xFFFF};
    model.materials.push_back(m2::Material{});

    model.vertices.resize(3);
    for (std::size_t v = 0; v < 3; ++v) {
        model.vertices[v].position = Vector3f{static_cast<f32>(v), 0, 0};
        model.vertices[v].boneWeights = {255, 0, 0, 0};
        model.vertices[v].boneIndices = {0, 0, 0, 0};
    }

    m2::SkinProfile skin;
    skin.vertices = {0, 1, 2};
    skin.indices = {0, 1, 2};
    m2::SkinSection submesh;
    submesh.indexStart = 0;
    submesh.indexCount = 3;
    submesh.vertexStart = 0;
    submesh.vertexCount = 3;
    skin.submeshes.push_back(submesh);
    m2::Batch batch;
    batch.skinSectionIndex = 0;
    batch.textureCount = 1;
    batch.textureComboIndex = 0;
    batch.textureCoordComboIndex = 0;
    batch.textureWeightComboIndex = 0;
    batch.textureTransformComboIndex = 0;
    skin.batches.push_back(batch);
    model.skinProfiles.push_back(std::move(skin));
    return model;
}

Document convert(const m2::Model& model) {
    M2Converter converter;
    Result<Document> result = converter.fromM2(model, 272);
    REQUIRE(result.ok());
    return std::move(*result.value);
}

const AnimChannel* channelWith(const Document& document, Channel channel) {
    for (const AnimChannel& entry : document.models[0].animChannels.channels) {
        if (entry.target.channel == channel) {
            return &entry;
        }
    }
    return nullptr;
}

const SubTrack* trackIn(const Clip& clip, u32 channelId) {
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            if (track.channel == channelId) {
                return &track;
            }
        }
    }
    return nullptr;
}

f32 floatAt(const SubTrack& track, std::size_t key) {
    f32 value = 0;
    std::memcpy(&value, track.values.data() + key * sizeof(f32), sizeof(f32));
    return value;
}

} // namespace

// ============================================================================

TEST_CASE("wem m2 a sequence is a clip and its inner array is the sub-track", "[wem][anim][m2]") {
    m2::Model model = makeModel();
    model.bones[0].translation =
        makeTrack<Vector3f>(m2::InterpolationType::Linear, {{0, 1000}, {0, 400, 800}},
                            {{Vector3f{0, 0, 0}, Vector3f{0, 0, 1}},
                             {Vector3f{0, 0, 0}, Vector3f{0, 0, 2}, Vector3f{0, 0, 0}}});

    const Document document = convert(model);
    REQUIRE(document.clips.size() == 2u);
    CHECK(document.clips[0].duration == 1.0f);
    CHECK(document.clips[0].looping);
    CHECK_FALSE(document.clips[1].looping);

    const AnimChannel* channel = channelWith(document, Channel::Translation);
    REQUIRE(channel != nullptr);
    // One declaration, two sub-tracks — the whole point of the id indirection.
    REQUIRE(trackIn(document.clips[0], channel->id) != nullptr);
    REQUIRE(trackIn(document.clips[1], channel->id) != nullptr);
    CHECK(trackIn(document.clips[0], channel->id)->times.size() == 2u);
    CHECK(trackIn(document.clips[1], channel->id)->times.size() == 3u);
    CHECK(trackIn(document.clips[1], channel->id)->times[1] == 0.4f);
}

TEST_CASE("wem m2 a global loop becomes an auto-play clip", "[wem][anim][m2]") {
    m2::Model model = makeModel();
    model.globalLoops.push_back(m2::GlobalSequence{3000});
    model.bones[0].translation = makeTrack<Vector3f>(m2::InterpolationType::Linear, {{0, 3000}},
                                                     {{Vector3f{0, 0, 0}, Vector3f{0, 0, 1}}});
    model.bones[0].translation.globalSequenceId = 0;

    const Document document = convert(model);
    REQUIRE(document.clips.size() == 3u);
    const Clip& global = document.clips[2];
    CHECK(global.name == "globalLoop_0");
    CHECK(global.duration == 3.0f);
    CHECK(hasFlag(global.flags, ClipFlags::AutoPlay));
    CHECK(hasFlag(global.flags, ClipFlags::WorldClocked));
    CHECK(global.containers[0].subTracks.size() == 1u);
    // And the sequences got nothing: it runs on its own clock.
    CHECK(document.clips[0].containers[0].subTracks.empty());
}

TEST_CASE("wem m2 fixed-point weights decode, and a visibility byte does not scale",
          "[wem][anim][m2]") {
    m2::Model model = makeModel();
    m2::TextureWeight weight;
    weight.weight =
        makeTrack<i16>(m2::InterpolationType::Linear, {{0, 1000}, {}}, {{32767, 16384}, {}});
    model.textureWeights.push_back(weight);
    model.textureWeightCombos = {0};

    m2::Attachment attachment;
    attachment.id = 1;
    attachment.boneId = 0;
    attachment.animate = makeTrack<u8>(m2::InterpolationType::None, {{0, 1000}, {}}, {{1, 0}, {}});
    model.attachments.push_back(attachment);

    const Document document = convert(model);

    const AnimChannel* alpha = channelWith(document, Channel::Alpha);
    REQUIRE(alpha != nullptr);
    CHECK(alpha->valueType == geom::AttrType::F32);
    const SubTrack* alphaTrack = trackIn(document.clips[0], alpha->id);
    REQUIRE(alphaTrack != nullptr);
    CHECK(floatAt(*alphaTrack, 0) == 1.0f);
    CHECK(std::abs(floatAt(*alphaTrack, 1) - 0.5f) < 0.001f);

    const AnimChannel* visibility = channelWith(document, Channel::Visibility);
    REQUIRE(visibility != nullptr);
    const SubTrack* visibilityTrack = trackIn(document.clips[0], visibility->id);
    REQUIRE(visibilityTrack != nullptr);
    // A flag, not a byte to divide by 255 — that mistake draws the model at
    // 1/255 opacity and looks invisible rather than broken.
    CHECK(floatAt(*visibilityTrack, 0) == 1.0f);
    CHECK(floatAt(*visibilityTrack, 1) == 0.0f);
}

TEST_CASE("wem m2 a batch colour drives the whole material, a weight drives a stage",
          "[wem][anim][m2]") {
    m2::Model model = makeModel();
    m2::ColorAnimation color;
    color.color =
        makeTrack<Vector3f>(m2::InterpolationType::Linear, {{0}, {}}, {{Vector3f{1, 0, 0}}, {}});
    color.alpha = makeTrack<i16>(m2::InterpolationType::Linear, {{0}, {}}, {{32767}, {}});
    model.colors.push_back(color);
    model.skinProfiles[0].batches[0].colorIndex = 0;

    m2::TextureWeight weight;
    weight.weight = makeTrack<i16>(m2::InterpolationType::Linear, {{0}, {}}, {{32767}, {}});
    model.textureWeights.push_back(weight);

    const Document document = convert(model);
    const AnimChannelTable& table = document.models[0].animChannels;

    u32 wholeMaterial = 0;
    u32 stageZero = 0;
    for (const AnimChannel& channel : table.channels) {
        REQUIRE(channel.target.kind == TrackTarget::Kind::MaterialLayer);
        CHECK(channel.target.material.profile == ProfileId::Wow);
        if (channel.target.sub == kWholeMaterial) {
            ++wholeMaterial;
        } else if (channel.target.sub == 0u) {
            ++stageZero;
        }
    }
    CHECK(wholeMaterial == 2u); // colour and alpha
    CHECK(stageZero == 1u);     // the unit-0 weight

    // And the document still validates: `kWholeMaterial` is a legal ordinal.
    CHECK(Validate(document, ValidateLevel::Profile).hasErrors() == false);
}

TEST_CASE("wem m2 a texture transform becomes a feature and its channels", "[wem][anim][m2]") {
    m2::Model model = makeModel();
    m2::TextureTransform transform;
    transform.translation = makeTrack<Vector3f>(m2::InterpolationType::Linear, {{0, 1000}, {}},
                                                {{Vector3f{0, 0, 0}, Vector3f{1, 0, 0}}, {}});
    transform.rotation = makeTrack<Quaternion>(m2::InterpolationType::Linear, {{0}, {}},
                                               {{Quaternion{0, 0, 0, 1}}, {}});
    model.textureTransforms.push_back(transform);
    model.textureTransformCombos = {0};

    const Document document = convert(model);
    const ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wow);
    REQUIRE(set != nullptr);
    REQUIRE(set->materials.size() == 1u);
    const std::vector<MaterialFeature>& features = set->materials[0].Common().features;
    REQUIRE(features.size() == 1u);
    CHECK(features[0].kind() == FeatureKind::UvAnimation);
    CHECK(features[0].layer == 0u);
    // The block stays in sync: adding the feature is the import deriving common
    // from native, not an edit to it.
    CHECK(set->materials[0].sync() == NativeSync::InSync);

    const AnimChannel* translate = channelWith(document, Channel::UvTranslate);
    REQUIRE(translate != nullptr);
    CHECK(translate->target.kind == TrackTarget::Kind::MaterialFeature);
    CHECK(translate->target.sub == features[0].id);

    const AnimChannel* rotate = channelWith(document, Channel::UvRotate);
    REQUIRE(rotate != nullptr);
    CHECK(rotate->valueType == geom::AttrType::Quat);
    // A `.m2` texture transform's rotation is float quaternions, unlike a bone's
    // compressed ones, so it slerps rather than stepping.
    CHECK(trackIn(document.clips[0], rotate->id)->interp == Interpolation::Slerp);
}

TEST_CASE("wem m2 an event's timestamps become clip events", "[wem][anim][m2]") {
    m2::Model model = makeModel();
    m2::Event event;
    event.identifier = 0x44485724; // '$DHW'
    event.boneId = 0;
    event.enabled.timestamps = {{250}, {100, 500}};
    model.events.push_back(event);

    const Document document = convert(model);
    REQUIRE(document.clips.size() == 2u);
    REQUIRE(document.clips[0].events.size() == 1u);
    CHECK(document.clips[0].events[0].time == 0.25f);
    CHECK(document.clips[0].events[0].value == 0x44485724u);
    CHECK(document.models[0].nodes.nodes[document.clips[0].events[0].node].kind == NodeKind::Event);
    CHECK(document.clips[1].events.size() == 2u);
}

TEST_CASE("wem m2 an alias sequence keeps its chain", "[wem][anim][m2]") {
    // §10.8.3: following the chain is playback policy, so it rides `ClipNative`
    // rather than becoming structure WEM would then have to define.
    m2::Model model = makeModel();
    model.sequences[1].flags = m2::SequenceFlag::IsAlias;
    model.sequences[1].aliasNext = 0;

    const Document document = convert(model);
    REQUIRE(document.clips.size() == 2u);
    const NativeBag::Entry* alias = document.clips[1].native.find("aliasNext");
    REQUIRE(alias != nullptr);
    CHECK(alias->value == 0);
    CHECK(document.clips[1].containers[0].subTracks.empty());
}

// ============================================================================

TEST_CASE("wem m2 animation survives the corpus", "[wem][anim][m2][.m2slow]") {
    const auto files = test::gather("WEM_M2_CORPUS_DIR", ".m2", {"WoW", "WowM2"});
    if (files.empty()) {
        SKIP("M2 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 100);

    u32 imported = 0;
    u32 withSequences = 0;
    u32 animated = 0;
    u32 clips = 0;
    u32 autoPlay = 0;
    u32 channels = 0;
    u32 subTracks = 0;
    u32 events = 0;
    u32 validationErrors = 0;
    std::vector<std::string> failing;

    const M2Converter converter;
    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        utils::OsFileSystem vfs(test::pathText(files[i].parent_path()));
        m2::Parser parser;
        const m2::Model source = parser.parse(vfs, test::pathText(files[i]));
        if (source.skinProfiles.empty()) {
            continue;
        }
        ++imported;
        if (!source.sequences.empty()) {
            ++withSequences;
        }

        Result<Document> converted = converter.fromM2(source, 272);
        if (!converted.ok()) {
            continue;
        }
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

    std::cout << "m2 animation: " << imported << " imported, " << withSequences
              << " with sequences, " << animated << " animated, " << clips << " clips (" << autoPlay
              << " auto-play), " << channels << " channels, " << subTracks << " sub-tracks, "
              << events << " events" << std::endl;
    for (const std::string& line : failing) {
        std::cout << "  " << line << std::endl;
    }

    REQUIRE(imported > 0);
    CHECK(animated == withSequences);
    CHECK(clips > 0);
    CHECK(channels > 0);
    CHECK(subTracks > 0);
    CHECK(validationErrors == 0u);
}

// ============================================================================
// What the WoW material animation becomes in Warcraft III.
//
// Both halves of §7's material axis cross a profile boundary here, and both of
// them were losing everything: `DeriveProfile` builds a second material set and
// §10.8 keys a channel by `(profile, slot, look)`, so until the derive twinned
// them the derived set had no animation at all.
// ============================================================================

TEST_CASE("wem m2 a batch colour hides its batch in Warcraft III too", "[wem][anim][m2][mdx]") {
    // The shipped idiom for "this batch is not on right now": an `M2Color` whose
    // alpha is a single key of zero. MDX has no per-material tint, so the only
    // record with the right reach is `GeosetAnimation`, which keys a geoset.
    m2::Model model = makeModel();
    m2::ColorAnimation color;
    color.color = makeTrack<Vector3f>(m2::InterpolationType::Linear, {{0}, {0}},
                                      {{Vector3f{1, 1, 1}}, {Vector3f{1, 1, 1}}});
    color.alpha = makeTrack<i16>(m2::InterpolationType::Linear, {{0}, {0}}, {{0}, {0}});
    model.colors.push_back(color);
    model.skinProfiles[0].batches[0].colorIndex = 0;

    Document document = convert(model);
    const DeriveResult derived = DeriveProfile(document, ProfileId::Wow, ProfileId::Wc3Classic);
    REQUIRE(derived.ok);

    // The twin: same target, the new profile, a new id, and the sub-tracks came
    // with it — a channel is a join key and the curve lives in the clip.
    u32 twins = 0;
    for (const AnimChannel& channel : document.models[0].animChannels.channels) {
        if (channel.target.material.profile != ProfileId::Wc3Classic) {
            continue;
        }
        ++twins;
        CHECK(trackIn(document.clips[0], channel.id) != nullptr);
    }
    CHECK(twins == 2u); // the colour and its alpha; this model keys no unit weight

    MdxConverter mdx;
    Result<mdx::Model> out = mdx.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(out.ok());
    REQUIRE(out.value->geosets.size() == 1u);
    REQUIRE(out.value->geosetAnimations.size() == 1u);
    const mdx::GeosetAnimation& animation = out.value->geosetAnimations[0];
    CHECK(animation.geosetId == 0u);
    CHECK(hasFlag(animation.flags, mdx::GeosetAnimation::Flag::Color));
    REQUIRE_FALSE(animation.alphaTracks.keys().empty());
    CHECK(animation.alphaTracks.keys().front() == 0.0f);
    CHECK_FALSE(animation.colorTracks.keys().empty());
}

TEST_CASE("wem m2 each animated layer gets a texture animation of its own",
          "[wem][anim][m2][mdx]") {
    // `Layer::textureAnimationId` has no presence bit and defaults to 0, which
    // is a perfectly good TXAN index — so every layer this export wrote used to
    // claim entry 0, and the second scrolling layer overwrote the first.
    m2::Model model = makeModel();
    m2::Texture second;
    second.filename = "world/detail.blp";
    model.textures.push_back(second);
    model.textureCombos = {0, 1};
    model.textureCoordCombos = {0, 0};
    model.textureWeightCombos = {0, 0};
    model.textureTransformCombos = {0, 1};
    // Two units, and `shaderId` 0 over two of them is the bit-field path's
    // `Combiners_Opaque_Opaque` — two stages, so two MDX layers.
    model.skinProfiles[0].batches[0].textureCount = 2;

    for (int i = 0; i < 2; ++i) {
        m2::TextureTransform transform;
        transform.translation =
            makeTrack<Vector3f>(m2::InterpolationType::Linear, {{0, 1000}, {}},
                                {{Vector3f{0, 0, 0}, Vector3f{static_cast<f32>(i + 1), 0, 0}}, {}});
        model.textureTransforms.push_back(transform);
    }

    Document document = convert(model);
    REQUIRE(DeriveProfile(document, ProfileId::Wow, ProfileId::Wc3Classic).ok);

    MdxConverter mdx;
    Result<mdx::Model> out = mdx.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(out.ok());
    REQUIRE(out.value->materials.size() == 1u);
    const std::vector<mdx::Layer>& layers = out.value->materials[0].layers;
    REQUIRE(layers.size() == 2u);
    CHECK(out.value->textureAnimations.size() == 2u);
    CHECK(layers[0].textureAnimationId != layers[1].textureAnimationId);
    CHECK(layers[0].textureAnimationId < out.value->textureAnimations.size());
    CHECK(layers[1].textureAnimationId < out.value->textureAnimations.size());
}

TEST_CASE("wem m2 a layer with no texture animation says so", "[wem][anim][m2][mdx]") {
    // The other half of the same sentinel: a layer nothing animates must not
    // name TXAN 0 either, or a model with one animated layer animates all of
    // them.
    Document document = convert(makeModel());
    REQUIRE(DeriveProfile(document, ProfileId::Wow, ProfileId::Wc3Classic).ok);
    MdxConverter mdx;
    Result<mdx::Model> out = mdx.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(out.ok());
    REQUIRE(out.value->materials.size() == 1u);
    REQUIRE(out.value->materials[0].layers.size() == 1u);
    CHECK(out.value->textureAnimations.empty());
    CHECK(out.value->materials[0].layers[0].textureAnimationId == 0xFFFFFFFFu);
}

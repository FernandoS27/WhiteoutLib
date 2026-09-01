// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P7 — the Diablo III animation import.
///
/// D3 is the only one of the four that joins its animation to its skeleton by
/// **name**, and the only one whose frame rate is per clip rather than per file.
/// Both are here, together with the anim set — which is where the "there is no
/// `Actor` type" decision finally has to answer for itself: an `.acr`'s
/// `snoAnimSet` has to land somewhere, and it lands on the `Model`.

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/d3_converter.h>
#include <whiteout/models/wem/validate.h>

#include "whiteout/models/wem/converters/d3_anim.h"

#include "wem_d3_corpus.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using whiteout::test::d3::CorpusProvider;
using whiteout::test::d3::corpusRoot;
using whiteout::test::d3::readWhole;

namespace fs = std::filesystem;
namespace d3n = whiteout::sno::d3::native;

namespace {

/// Two bones and one hardpoint, so a name join has something to hit and miss.
d3n::Appearances makeAppearance() {
    d3n::Appearances appearance;
    appearance.dwSnoId = 1000;

    d3n::BoneStructure root;
    root.szName = "Root";
    root.nParentIndex = -1;
    root.tTransform2.flScale = 1.0f;
    root.tTransform2.qRotation = Vector4f{0, 0, 0, 1};
    appearance.arBones.push_back(root);

    d3n::BoneStructure spine;
    spine.szName = "Spine";
    spine.nParentIndex = 0;
    spine.tTransform2.flScale = 1.0f;
    spine.tTransform2.qRotation = Vector4f{0, 0, 0, 1};
    appearance.arBones.push_back(spine);

    d3n::Hardpoint hardpoint;
    hardpoint.szName = "HP_Right_Hand";
    hardpoint.nBoneIndex = 1;
    hardpoint.tTransform.qRotation = Vector4f{0, 0, 0, 1};
    appearance.dwHardpointCount = 1;
    appearance.arHardpoints.push_back(hardpoint);

    // No geometry: the subject here is the skeleton join, and an appearance
    // with bones and no sub-objects is a legal thing to convert.
    appearance.arLooks.resize(1);
    appearance.arLooks[0].szName = "A";
    appearance.dwLookCount = 1;
    return appearance;
}

/// One permutation, `flFramesPerTick` chosen so fps is exactly 30.
d3n::Anim makeAnim() {
    d3n::Anim anim;
    anim.dwSnoId = 2000;

    d3n::AnimPermutation permutation;
    permutation.szName = "Idle_01";
    permutation.flFramesPerTick = 0.5f; // 0.5 * 60 = 30 fps
    permutation.dwFrameCount = 31;      // (31 - 1) / 30 = 1 second

    permutation.arBoneNames.push_back(d3n::BoneName{"root"}); // lower case on purpose
    permutation.arBoneNames.push_back(d3n::BoneName{"NotInThisModel"});

    d3n::TranslationCurve translation;
    translation.arKeys.push_back(d3n::TranslationKey{0, Vector3f{0, 0, 0}});
    translation.arKeys.push_back(d3n::TranslationKey{30, Vector3f{0, 0, 5}});
    translation.dwKeyCount = 2;
    permutation.arTranslationCurves.push_back(translation);
    permutation.arTranslationCurves.push_back(d3n::TranslationCurve{});

    d3n::RotationCurve rotation;
    // 32767 in w, zero elsewhere: the identity, and the value that proves the
    // components are read as signed 16-bit rather than unsigned.
    rotation.arKeys.push_back(d3n::RotationKey{0, d3n::Quaternion16{0, 0, 0, 32767}});
    rotation.dwKeyCount = 1;
    permutation.arRotationCurves.push_back(rotation);
    permutation.arRotationCurves.push_back(d3n::RotationCurve{});

    d3n::ScaleCurve scale;
    scale.arKeys.push_back(d3n::ScaleKey{0, 2.0f});
    scale.dwKeyCount = 1;
    permutation.arScaleCurves.push_back(scale);
    permutation.arScaleCurves.push_back(d3n::ScaleCurve{});

    anim.arPermutations.push_back(std::move(permutation));
    anim.dwPermutationCount = 1;
    return anim;
}

Document convertAppearance(const d3n::Appearances& appearance) {
    D3Converter converter;
    Result<Document> result = converter.fromAppearance(appearance);
    REQUIRE(result.ok());
    return std::move(*result.value);
}

const SubTrack* trackFor(const Document& document, const Clip& clip, u32 node, Channel channel) {
    const AnimChannelTable& table = document.models[clip.model].animChannels;
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            const AnimChannel* declared = table.find(track.channel);
            if (declared != nullptr && declared->target.node == node &&
                declared->target.channel == channel) {
                return &track;
            }
        }
    }
    return nullptr;
}

} // namespace

// ============================================================================

TEST_CASE("wem d3 one permutation is one clip at its own frame rate", "[wem][anim][d3]") {
    Document document = convertAppearance(makeAppearance());
    Diagnostics report;
    const std::vector<u32> clips = d3_anim::ImportAnim(makeAnim(), document, 0, report);

    REQUIRE(clips.size() == 1u);
    REQUIRE(clips[0] != kInvalidIndex);
    const Clip& clip = document.clips[clips[0]];
    CHECK(clip.name == "Idle_01");
    CHECK(clip.model == 0u);
    // fps = flFramesPerTick * 60 = 30, and the duration counts spans, not
    // frames: (31 - 1) / 30.
    CHECK(clip.duration == 1.0f);
    REQUIRE(clip.containers.size() == 1u);

    const SubTrack* translation = trackFor(document, clip, 0, Channel::Translation);
    REQUIRE(translation != nullptr);
    REQUIRE(translation->times.size() == 2u);
    CHECK(translation->times[0] == 0.0f);
    CHECK(translation->times[1] == 1.0f);
}

TEST_CASE("wem d3 bones bind by name, case-insensitively, and a miss is skipped",
          "[wem][anim][d3]") {
    // The permutation names "root" and the appearance spells it "Root"; a byte
    // compare loses the track, and the second name is a bone this appearance
    // does not have at all.
    Document document = convertAppearance(makeAppearance());
    Diagnostics report;
    const std::vector<u32> clips = d3_anim::ImportAnim(makeAnim(), document, 0, report);
    REQUIRE(clips.size() == 1u);

    const AnimChannelTable& table = document.models[0].animChannels;
    // Translation, rotation and scale for bone 0 — and nothing for the bone the
    // model lacks.
    CHECK(table.channels.size() == 3u);
    for (const AnimChannel& channel : table.channels) {
        CHECK(channel.target.kind == TrackTarget::Kind::Node);
        CHECK(channel.target.node == 0u);
    }
    CHECK(trackFor(document, document.clips[clips[0]], 1, Channel::Translation) == nullptr);
}

TEST_CASE("wem d3 a rotation key is signed 16-bit, w last", "[wem][anim][d3]") {
    Document document = convertAppearance(makeAppearance());
    Diagnostics report;
    const std::vector<u32> clips = d3_anim::ImportAnim(makeAnim(), document, 0, report);
    REQUIRE(clips.size() == 1u);

    const SubTrack* rotation = trackFor(document, document.clips[clips[0]], 0, Channel::Rotation);
    REQUIRE(rotation != nullptr);
    CHECK(rotation->interp == Interpolation::Slerp);
    REQUIRE(rotation->values.size() == sizeof(Quaternion));
    Quaternion decoded{};
    std::memcpy(&decoded, rotation->values.data(), sizeof(Quaternion));
    CHECK(decoded.w == 1.0f);
    CHECK(decoded.x == 0.0f);
}

TEST_CASE("wem d3 a scale key stays one float", "[wem][anim][d3]") {
    // D3 ships one scale per bone, which is why the node import sets
    // `uniformScaleOnly`; widening the key here would invent two components.
    Document document = convertAppearance(makeAppearance());
    Diagnostics report;
    const std::vector<u32> clips = d3_anim::ImportAnim(makeAnim(), document, 0, report);
    REQUIRE(clips.size() == 1u);

    const SubTrack* scale = trackFor(document, document.clips[clips[0]], 0, Channel::Scale);
    REQUIRE(scale != nullptr);
    const AnimChannel* channel = document.models[0].animChannels.find(scale->channel);
    REQUIRE(channel != nullptr);
    CHECK(channel->valueType == geom::AttrType::F32);
    CHECK(scale->values.size() == sizeof(f32));
    f32 value = 0;
    std::memcpy(&value, scale->values.data(), sizeof(f32));
    CHECK(value == 2.0f);

    CHECK_FALSE(Validate(document, ValidateLevel::Structural).hasErrors());
}

TEST_CASE("wem d3 a keyframed attachment fires at its hardpoint", "[wem][anim][d3]") {
    d3n::Anim anim = makeAnim();
    d3n::KeyframedAttachment attachment;
    attachment.flFrame = 15.0f;
    attachment.tEvent.eTriggerType = 3;
    attachment.tEvent.tHardpoint0.szName = "hp_right_hand"; // the model spells it differently
    attachment.tEvent.tPayload.dwNameHandle = 4242;
    anim.arPermutations[0].arAttachments.push_back(attachment);
    anim.arPermutations[0].dwAttachmentCount = 1;

    Document document = convertAppearance(makeAppearance());
    Diagnostics report;
    const std::vector<u32> clips = d3_anim::ImportAnim(anim, document, 0, report);
    REQUIRE(clips.size() == 1u);

    const Clip& clip = document.clips[clips[0]];
    REQUIRE(clip.events.size() == 1u);
    CHECK(clip.events[0].time == 0.5f);
    CHECK(clip.events[0].value == 4242u);
    REQUIRE(clip.events[0].node != kInvalidNode);
    CHECK(document.models[0].nodes.nodes[clip.events[0].node].kind == NodeKind::Attachment);
}

TEST_CASE("wem d3 a permutation with no frame rate is reported, not divided by",
          "[wem][anim][d3]") {
    d3n::Anim anim = makeAnim();
    anim.arPermutations[0].flFramesPerTick = 0.0f;

    Document document = convertAppearance(makeAppearance());
    Diagnostics report;
    const std::vector<u32> clips = d3_anim::ImportAnim(anim, document, 0, report);
    REQUIRE(clips.size() == 1u);
    CHECK(clips[0] == kInvalidIndex);
    CHECK(document.clips.empty());
    CHECK(report.countOf(DiagCode::AnimTrackDropped) == 1u);
}

TEST_CASE("wem d3 an anim set becomes one set per tag map with core as the fallback",
          "[wem][anim][d3][corpus]") {
    const fs::path root = corpusRoot();
    if (!fs::is_directory(root / "AnimSet")) {
        WARN("no D3 corpus; skipping");
        return;
    }
    CorpusProvider provider(root);
    AssetSource assets(provider);
    D3Converter converter;

    // The first actor in the corpus that names both an appearance and an anim
    // set — found, not assumed.
    const d3n::Actor* actor = nullptr;
    d3n::Actor storage;
    std::size_t scanned = 0;
    for (fs::directory_iterator it(root / "Actor"); it != fs::directory_iterator(); ++it) {
        if (scanned++ >= 600) {
            break;
        }
        auto parsed = d3n::parseActor(readWhole(it->path()));
        if (!parsed.has_value() || !parsed->snoAppearance.valid() || !parsed->snoAnimSet.valid()) {
            continue;
        }
        if (assets.animSet(parsed->snoAnimSet.id) == nullptr ||
            assets.appearance(parsed->snoAppearance.id) == nullptr) {
            continue;
        }
        storage = std::move(*parsed);
        actor = &storage;
        break;
    }
    if (actor == nullptr) {
        WARN("no corpus actor with both an appearance and an anim set");
        return;
    }

    Document document;
    document.declare(ProfileId::Diablo3);
    document.defaultProfile = ProfileId::Diablo3;
    const Result<u32> model = converter.appendActor(document, *actor, assets);
    REQUIRE(model.ok());

    REQUIRE_FALSE(document.animSets.empty());
    CHECK(document.animSets[0].name == "core");
    CHECK(document.animSets[0].baseAnimSet == kInvalidIndex);
    // Every weapon map falls back to core — the field spelling the fallback.
    for (std::size_t a = 1; a < document.animSets.size(); ++a) {
        CHECK(document.animSets[a].baseAnimSet == 0u);
    }
    // The `.acr`'s `snoAnimSet` had nowhere to go but the model, and this is it.
    CHECK(document.models[*model.value].animSet == 0u);

    CHECK_FALSE(Validate(document, ValidateLevel::Profile).hasErrors());

    std::cout << "d3 anim set: actor " << actor->dwSnoId << " -> " << document.animSets.size()
              << " sets, " << document.clips.size() << " clips" << std::endl;
}

TEST_CASE("wem d3 animation survives the corpus", "[wem][anim][d3][corpus]") {
    const fs::path root = corpusRoot();
    if (!fs::is_directory(root / "Anim")) {
        WARN("no D3 corpus; skipping");
        return;
    }
    CorpusProvider provider(root);
    AssetSource assets(provider);
    D3Converter converter;

    // Actors with an anim set, so every arm of the join is exercised: the
    // appearance builds the model, the set names the animations, and the
    // animations bind to bones by name.
    u32 actors = 0;
    u32 withAnimSet = 0;
    u32 imported = 0;
    u32 sets = 0;
    u32 clips = 0;
    u32 channels = 0;
    u32 subTracks = 0;
    u32 events = 0;
    u32 boundBones = 0;
    u32 validationErrors = 0;
    std::vector<std::string> failing;

    std::size_t scanned = 0;
    for (fs::directory_iterator it(root / "Actor"); it != fs::directory_iterator(); ++it) {
        if (scanned++ >= 400 || imported >= 24) {
            break;
        }
        auto parsed = d3n::parseActor(readWhole(it->path()));
        if (!parsed.has_value() || !parsed->snoAppearance.valid()) {
            continue;
        }
        ++actors;
        if (!parsed->snoAnimSet.valid()) {
            continue;
        }
        ++withAnimSet;

        Document document;
        document.declare(ProfileId::Diablo3);
        document.defaultProfile = ProfileId::Diablo3;
        D3ImportOptions options;
        options.attachmentDepth = 0; // The animation is the subject here.
        const Result<u32> model = converter.appendActor(document, *parsed, assets, options);
        if (!model.ok() || document.clips.empty()) {
            continue;
        }
        ++imported;

        sets += static_cast<u32>(document.animSets.size());
        clips += static_cast<u32>(document.clips.size());
        for (const Clip& clip : document.clips) {
            events += static_cast<u32>(clip.events.size());
            for (const SubTrackContainer& container : clip.containers) {
                subTracks += static_cast<u32>(container.subTracks.size());
            }
        }
        for (const Model& entry : document.models) {
            channels += static_cast<u32>(entry.animChannels.channels.size());
            for (const AnimChannel& channel : entry.animChannels.channels) {
                if (channel.target.kind == TrackTarget::Kind::Node &&
                    channel.target.node < entry.nodes.size()) {
                    ++boundBones;
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
                failing.push_back(it->path().filename().string() + ": " + issue.message);
            }
        }
    }

    std::cout << "d3 animation: " << actors << " actors scanned, " << withAnimSet
              << " with an anim set, " << imported << " imported, " << sets << " sets, " << clips
              << " clips, " << channels << " channels (" << boundBones << " on a live node), "
              << subTracks << " sub-tracks, " << events << " events" << std::endl;
    for (const std::string& line : failing) {
        std::cout << "  " << line << std::endl;
    }

    REQUIRE(imported > 0);
    CHECK(clips > 0);
    CHECK(channels > 0);
    CHECK(subTracks > 0);
    // Every channel D3 declares is a bone track, and a bone track exists only
    // because the name matched — so a channel pointing off the tree would mean
    // the name join produced an index rather than a match.
    CHECK(boundBones == channels);
    // And the id indirection is doing its job: a bone's translation is *one*
    // property however many of the actor's animations drive it. One channel per
    // sub-track means the importer declared per permutation instead of per
    // property — measured at 193,584 : 193,584 before this held, against
    // 2,340 : 193,584 after.
    CHECK(channels * 4 < subTracks);
    CHECK(validationErrors == 0u);
}

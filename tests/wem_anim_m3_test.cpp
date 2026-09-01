// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 P7 — the SC2 / Heroes animation import.
///
/// This is the 1:1 one, so the cases are not "did the shape survive" but the
/// four things measured elsewhere in this tree that a green parse cannot see:
/// interpolation from **AnimRef flags bit 4** and never the track-table row, a
/// keyed discrete channel in the **SDFG** slot and never slot 10, the basis
/// change applied to a key the same way it was applied to the rest pose, and an
/// `.m3a` merge that joins on **animId** with no name matching at all.

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/validate.h>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

/// STC `animRefs` packs the slot in the high half.
constexpr u32 Ref(u32 slot, u32 block) {
    return (slot << 16) | block;
}

template <class T>
m3::AnimRef<T> animated(u32 animId, const T& initValue, u16 flags = 0) {
    m3::AnimRef<T> ref;
    ref.animId = animId;
    ref.initValue = initValue;
    ref.flags = flags;
    return ref;
}

/// One bone, one sequence, one STG_ naming one STC_.
m3::Model makeModel() {
    m3::Model model;
    model.name = "anim.m3";

    m3::Sequence sequence;
    sequence.id = 0;
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
    stc.animPriority = 3;
    stc.runsConcurrent = 1;
    model.subTrackCollections.push_back(stc);

    m3::Bone bone;
    bone.name = std::string("Bone_Root");
    bone.parentIndex = 0xFFFF;
    bone.position = animated<Vector3f>(0, Vector3f{0, 0, 0});
    bone.rotation = animated<Quaternion>(0, Quaternion{0, 0, 0, 1});
    bone.scale = animated<Vector3f>(0, Vector3f{1, 1, 1});
    bone.visibility = animated<u32>(0, 1u);
    model.bones.push_back(bone);
    return model;
}

/// Gives the model's bone a keyed translation under animId @p id.
void keyTranslation(m3::Model& model, u32 id, std::vector<i32> times, std::vector<Vector3f> keys,
                    u16 flags = 0) {
    model.bones[0].position = animated<Vector3f>(id, Vector3f{0, 0, 0}, flags);
    m3::AnimBlock<Vector3f> block;
    block.timestamps = std::move(times);
    block.keys = std::move(keys);
    model.subTrackCollections[0].sd3v.push_back(std::move(block));
    model.subTrackCollections[0].animIds.push_back(id);
    model.subTrackCollections[0].animRefs.push_back(
        Ref(2, static_cast<u32>(model.subTrackCollections[0].sd3v.size() - 1)));
}

Document convert(const m3::Model& model) {
    M3Converter converter;
    Result<Document> result = converter.fromM3(model, ProfileId::Sc2);
    REQUIRE(result.ok());
    return std::move(*result.value);
}

const SubTrack* trackFor(const Document& document, std::size_t clip, u32 channelId) {
    for (const SubTrackContainer& container : document.clips[clip].containers) {
        if (const SubTrack* found = container.find(channelId)) {
            return found;
        }
    }
    return nullptr;
}

Vector3f vectorAt(const SubTrack& track, std::size_t key) {
    Vector3f value{0, 0, 0};
    std::memcpy(&value, track.values.data() + key * sizeof(Vector3f), sizeof(Vector3f));
    return value;
}

} // namespace

// ============================================================================

TEST_CASE("wem m3 a sequence, its group and its containers map one to one", "[wem][anim][m3]") {
    m3::Model model = makeModel();
    keyTranslation(model, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    const Document document = convert(model);
    REQUIRE(document.clips.size() == 1u);
    const Clip& clip = document.clips[0];
    CHECK(clip.name == "Stand");
    CHECK(clip.duration == 1.0f);
    CHECK(clip.looping);
    REQUIRE(clip.containers.size() == 1u);
    // The STC's own layering data, copied rather than collapsed — the level
    // exists because M3's split-body playback is stated in it.
    CHECK(clip.containers[0].name == "Stand");
    CHECK(clip.containers[0].priority == 3);
    CHECK(clip.containers[0].concurrent);

    // The animId is the channel id, verbatim.
    REQUIRE(document.models[0].animChannels.find(7) != nullptr);
    CHECK(trackFor(document, 0, 7) != nullptr);
}

TEST_CASE("wem m3 a track steps on AnimRef flags bit 4", "[wem][anim][m3]") {
    // The whole point: `interpType` is a track-table row that lies at runtime,
    // and reading it juddered every `.m3a`-driven hero.
    m3::Model smooth = makeModel();
    keyTranslation(smooth, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}}, 0);
    CHECK(trackFor(convert(smooth), 0, 7)->interp == Interpolation::Linear);

    m3::Model stepped = makeModel();
    keyTranslation(stepped, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}}, 0x10);
    CHECK(trackFor(convert(stepped), 0, 7)->interp == Interpolation::Step);
}

TEST_CASE("wem m3 a keyed visibility is the SDFG slot", "[wem][anim][m3]") {
    // Slot 11, never 10. The wrong slot silently returns `initValue` forever,
    // which is a model that never blinks and never fails.
    m3::Model model = makeModel();
    model.bones[0].visibility = animated<u32>(9, 1u);
    m3::AnimBlock<m3::Flag> block;
    block.timestamps = {0, 500};
    block.keys = {m3::Flag{1}, m3::Flag{0}};
    model.subTrackCollections[0].sdfg.push_back(std::move(block));
    model.subTrackCollections[0].animIds.push_back(9);
    model.subTrackCollections[0].animRefs.push_back(Ref(11, 0));

    const Document document = convert(model);
    const AnimChannel* channel = document.models[0].animChannels.find(9);
    REQUIRE(channel != nullptr);
    CHECK(channel->target.channel == Channel::Visibility);
    CHECK(channel->valueType == geom::AttrType::F32);

    const SubTrack* track = trackFor(document, 0, 9);
    REQUIRE(track != nullptr);
    REQUIRE(track->times.size() == 2u);
    f32 first = 0;
    std::memcpy(&first, track->values.data(), sizeof(f32));
    CHECK(first == 1.0f);
}

TEST_CASE("wem m3 a key is rebased the way the rest pose was", "[wem][anim][m3]") {
    // §6.4 canonicalises geometry, and a translation key is a vector in the
    // basis being changed. Skipping this plays SC2's animation over WEM's mesh.
    m3::Model model = makeModel();
    keyTranslation(model, 7, {0}, {Vector3f{1, 2, 3}});

    const Document document = convert(model);
    const SubTrack* track = trackFor(document, 0, 7);
    REQUIRE(track != nullptr);
    const Vector3f key = vectorAt(*track, 0);
    CHECK(key.x == -2.0f);
    CHECK(key.y == 1.0f);
    CHECK(key.z == 3.0f);
}

TEST_CASE("wem m3 an AnimRef's initValue becomes the channel's rest value", "[wem][anim][m3]") {
    // The asymmetry §10.8.1 is built on: an opaque container with no sub-track
    // for a channel contributes this, which is unstatable on a track.
    m3::Model model = makeModel();
    model.bones[0].position = animated<Vector3f>(7, Vector3f{0, 4, 0});
    keyTranslation(model, 7, {0}, {Vector3f{0, 0, 0}});
    model.bones[0].position.initValue = Vector3f{0, 4, 0};

    const Document document = convert(model);
    const AnimChannel* channel = document.models[0].animChannels.find(7);
    REQUIRE(channel != nullptr);
    REQUIRE(channel->hasInitValue());
    Vector3f rest{0, 0, 0};
    std::memcpy(&rest, channel->initValue.data(), sizeof(Vector3f));
    // Rebased, like the keys: -y, x, z.
    CHECK(rest.x == -4.0f);
    CHECK(rest.y == 0.0f);
}

TEST_CASE("wem m3 SEQS flag 0x2 is the auto-play loop", "[wem][anim][m3]") {
    m3::Model model = makeModel();
    model.sequences[0].flags = m3::SequenceFlag::AlwaysGlobal;
    keyTranslation(model, 7, {0}, {Vector3f{0, 0, 0}});

    const Document document = convert(model);
    REQUIRE(document.clips.size() == 1u);
    CHECK(hasFlag(document.clips[0].flags, ClipFlags::AutoPlay));
    CHECK(hasFlag(document.clips[0].flags, ClipFlags::Persistent));
    CHECK(hasFlag(document.clips[0].flags, ClipFlags::WorldClocked));
}

TEST_CASE("wem m3 an external animation merges on id, never on a name", "[wem][anim][m3]") {
    m3::Model base = makeModel();
    keyTranslation(base, 7, {0}, {Vector3f{0, 0, 0}});
    Document document = convert(base);
    REQUIRE(document.clips.size() == 1u);

    // The `.m3a`: different sequence name, different container name, different
    // bone array — nothing to match by name. Only the animId is shared, plus one
    // id the base model never declared.
    m3::Model external;
    m3::Sequence sequence;
    sequence.id = 41;
    sequence.name = "Attack";
    sequence.startFrame = 0;
    sequence.endFrame = 500;
    external.sequences.push_back(sequence);

    m3::AnimationGroup group;
    group.subtrackIndices = {0};
    external.animationGroups.push_back(group);

    m3::SubTrackContainer stc;
    stc.name = "SomeOtherName";
    stc.animPriority = 1;
    m3::AnimBlock<Vector3f> known;
    known.timestamps = {0, 500};
    known.keys = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}};
    stc.sd3v.push_back(std::move(known));
    stc.animIds.push_back(7);
    stc.animRefs.push_back(Ref(2, 0));

    m3::AnimBlock<Vector3f> stranger;
    stranger.timestamps = {0};
    stranger.keys = {Vector3f{9, 9, 9}};
    stc.sd3v.push_back(std::move(stranger));
    stc.animIds.push_back(999);
    stc.animRefs.push_back(Ref(2, 1));
    external.subTrackCollections.push_back(std::move(stc));

    M3Converter converter;
    const Result<u32> merged = converter.mergeAnimation(document, 0, external);
    REQUIRE(merged.ok());
    CHECK(*merged.value == 1u);
    REQUIRE(document.clips.size() == 2u);

    const Clip& added = document.clips[1];
    CHECK(added.name == "Attack");
    CHECK(added.model == 0u);
    // The known id landed; the unknown one did not become a channel.
    REQUIRE(added.containers.size() == 1u);
    CHECK(added.containers[0].subTracks.size() == 1u);
    CHECK(added.containers[0].subTracks[0].channel == 7u);
    CHECK(document.models[0].animChannels.channels.size() == 1u);
    CHECK(document.models[0].animChannels.find(999) == nullptr);
    // Keys arrive rebased, because the channel says what it drives.
    CHECK(vectorAt(added.containers[0].subTracks[0], 1).y == 1.0f);

    CHECK_FALSE(Validate(document, ValidateLevel::Profile).hasErrors());
}

TEST_CASE("wem m3 merging into a model the document lacks refuses", "[wem][anim][m3]") {
    Document document = convert(makeModel());
    M3Converter converter;
    const Result<u32> merged = converter.mergeAnimation(document, 3, m3::Model{});
    CHECK_FALSE(merged.ok());
}

// ============================================================================

TEST_CASE("wem m3 animation survives the corpus", "[wem][anim][m3][corpus]") {
    const auto files =
        test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        WARN("no .m3 corpus found; set WEM_M3_CORPUS_DIR");
        return;
    }
    const std::size_t limit = test::sweepLimit(files.size(), 150);

    u32 imported = 0;
    u32 withSequences = 0;
    u32 animated = 0;
    u32 clips = 0;
    u32 autoPlay = 0;
    u32 containers = 0;
    u32 concurrent = 0;
    u32 channels = 0;
    u32 stepped = 0;
    u32 subTracks = 0;
    u32 events = 0;
    u32 validationErrors = 0;
    u32 globalSequences = 0;
    u32 steppedRefs = 0;
    std::vector<std::string> failing;

    const M3Converter converter;
    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        m3::Parser parser;
        const m3::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        if (source.bones.empty() && source.sequences.empty()) {
            continue;
        }
        ++imported;
        if (!source.sequences.empty()) {
            ++withSequences;
        }
        // Counted from the *source*, so the two conditional gates below can ask
        // whether the import produced what the file actually contains — rather
        // than asserting a count that shipped content may legitimately not have.
        for (const m3::Sequence& sequence : source.sequences) {
            if (m3::hasFlag(sequence.flags, m3::SequenceFlag::AlwaysGlobal)) {
                ++globalSequences;
            }
        }
        for (const m3::Bone& bone : source.bones) {
            const u16 refs[] = {bone.position.flags, bone.rotation.flags, bone.scale.flags,
                                bone.visibility.flags};
            for (u16 flags : refs) {
                if ((flags & 0x10u) != 0) {
                    ++steppedRefs;
                }
            }
        }

        Result<Document> converted = converter.fromM3(source);
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
            containers += static_cast<u32>(clip.containers.size());
            for (const SubTrackContainer& container : clip.containers) {
                if (container.concurrent) {
                    ++concurrent;
                }
                subTracks += static_cast<u32>(container.subTracks.size());
                for (const SubTrack& track : container.subTracks) {
                    if (track.interp == Interpolation::Step) {
                        ++stepped;
                    }
                }
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

    std::cout << "m3 animation: " << imported << " imported, " << withSequences
              << " with sequences, " << animated << " animated, " << clips << " clips (" << autoPlay
              << " auto-play), " << containers << " containers (" << concurrent << " concurrent), "
              << channels << " channels, " << subTracks << " sub-tracks (" << stepped
              << " stepped), " << events << " events" << std::endl;
    std::cout << "  source: " << globalSequences << " SEQS 0x2, " << steppedRefs
              << " bone AnimRefs with the step bit" << std::endl;
    for (const std::string& line : failing) {
        std::cout << "  " << line << std::endl;
    }

    REQUIRE(imported > 0);
    CHECK(animated == withSequences);
    CHECK(clips > 0);
    CHECK(channels > 0);
    CHECK(subTracks > 0);
    // The layering level is not decoration: SC2 content actually uses it, and a
    // sweep where nothing is concurrent means `runsConcurrent` never arrived.
    CHECK(containers >= clips);
    CHECK(concurrent > 0u);
    CHECK(validationErrors == 0u);

    // Conditional, because both of these are genuinely rare in shipped content
    // and asserting a count would be asserting the corpus rather than the code.
    // The **step bit fires on 330 refs in 291,027** measured on the renderer
    // side, and the first several hundred `Sc2M3` files by path are props — so
    // "zero" is the honest expected reading here, and the gate is that the
    // import agrees with the file either way.
    CHECK((steppedRefs == 0u) == (stepped == 0u));
    CHECK((globalSequences == 0u) == (autoPlay == 0u));
}

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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/validate.h>

#include "wem_corpus_files.h"
#include "wem_material_fixture.h"

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

/// A second bone under the first, animated by nothing of its own.
void addChildBone(m3::Model& model) {
    m3::Bone bone;
    bone.name = std::string("Bone_Child");
    bone.parentIndex = 0;
    bone.position = animated<Vector3f>(0, Vector3f{0, 0, 1});
    bone.rotation = animated<Quaternion>(0, Quaternion{0, 0, 0, 1});
    bone.scale = animated<Vector3f>(0, Vector3f{1, 1, 1});
    bone.visibility = animated<u32>(0, 1u);
    model.bones.push_back(bone);
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

TEST_CASE("wem m3 a container's lookup table is one StarCraft II can search",
          "[wem][anim][m3]") {
    // `animIds` and `animRefs` are one table in two arrays, and the game
    // searches it by id: 1,221 of 1,221 shipped containers keep it sorted, and
    // ours came out in whatever order the channels were written (0 of the
    // banshee's 10). The `STS_` beside it is that same list read a second way —
    // every container named an `animationStateIndex` into an array this export
    // never wrote at all.
    // Three properties of the one bone, given ids in descending order so that
    // "sorted" cannot be true by accident.
    m3::Model model = makeModel();
    keyTranslation(model, 11, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    model.bones[0].scale = animated<Vector3f>(7, Vector3f{1, 1, 1});
    m3::AnimBlock<Vector3f> grow;
    grow.timestamps = {0, 1000};
    grow.keys = {Vector3f{1, 1, 1}, Vector3f{2, 2, 2}};
    model.subTrackCollections[0].sd3v.push_back(std::move(grow));
    model.subTrackCollections[0].animIds.push_back(7);
    model.subTrackCollections[0].animRefs.push_back(
        Ref(2, static_cast<u32>(model.subTrackCollections[0].sd3v.size() - 1)));

    model.bones[0].visibility = animated<u32>(3, 1u);
    m3::AnimBlock<m3::Flag> blink;
    blink.timestamps = {0, 500};
    blink.keys = {m3::Flag{1}, m3::Flag{0}};
    model.subTrackCollections[0].sdfg.push_back(std::move(blink));
    model.subTrackCollections[0].animIds.push_back(3);
    model.subTrackCollections[0].animRefs.push_back(Ref(11, 0));

    const Document document = convert(model);
    M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->subTrackCollections.empty());

    CHECK(written->animationStates.size() == written->subTrackCollections.size());
    // Sortedness says nothing about a one-entry table, and the three channels
    // above are keyed out of id order on purpose.
    std::size_t widest = 0;
    for (const m3::SubTrackContainer& stc : written->subTrackCollections) {
        widest = (std::max)(widest, stc.animIds.size());
    }
    CHECK(widest >= 3u);

    for (const m3::SubTrackContainer& stc : written->subTrackCollections) {
        INFO(stc.name);
        CHECK(stc.animIds.size() == stc.animRefs.size());
        CHECK(std::is_sorted(stc.animIds.begin(), stc.animIds.end()));

        REQUIRE(stc.animationStateIndex < written->animationStates.size());
        // The second u16 is not padding: 2,197 of 2,197 shipped containers
        // repeat the index there, and this export left it at zero.
        CHECK(stc.animationStateIndexCopy == stc.animationStateIndex);
        const m3::AnimationState& state = written->animationStates[stc.animationStateIndex];
        CHECK(state.animIds == stc.animIds);
        // No parent, no sibling, no child — §7.4, and every shipped record.
        CHECK(state.unknown[0] == 0xFFu);
        CHECK(state.unknown[13] == 0xFFu);
        CHECK(state.unknown[14] == 0x00u);
    }
}

TEST_CASE("wem m3 an event stream joins under an id the client knows", "[wem][anim][m3]") {
    // The slot-0 stream is reached by id like every other channel, and the id
    // is not ours to choose: across 2,044 shipped models every slot-0 stream
    // carries one of exactly two, and no model mixes them. This export wrote
    // `0`, which the format reserves for "not animated" — so the events were
    // in the file and unreachable, and every container's table began with an
    // id below the smallest any shipped model uses.
    m3::Model model = makeModel();
    keyTranslation(model, 11, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    Document document = convert(model);
    REQUIRE_FALSE(document.clips.empty());
    ClipEvent event;
    event.time = 0.25f;
    event.name = "Fire";
    document.clips[0].events.push_back(event);

    M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->subTrackCollections.empty());

    bool carried = false;
    for (const m3::SubTrackContainer& stc : written->subTrackCollections) {
        for (std::size_t j = 0; j < stc.animIds.size(); ++j) {
            // Nothing joins under the sentinel, whatever the slot.
            CHECK(stc.animIds[j] != 0u);
            if ((stc.animRefs[j] >> 16) == 0) {
                CHECK(stc.animIds[j] == 0x063F6D59u);
                carried = true;
            }
        }
    }
    CHECK(carried);
}

TEST_CASE("wem m3 every sequence ends with the event the engine waits for",
          "[wem][anim][m3]") {
    // All 1,209 StarCraft II and 671 Heroes models that carry sequences give
    // every one an `Evt_SeqEnd`, and it is the last key of all 2,378 slot-0
    // blocks measured -- always type 4, always bone 0xFFFF, keyed at the
    // sequence's end frame. It is how the engine learns a clip is over. This
    // export wrote none, and left the 18 of a footman's 27 sequences that
    // carried no events of their own with no slot-0 stream at all.
    m3::Model model = makeModel();
    keyTranslation(model, 11, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    Document document = convert(model);
    REQUIRE_FALSE(document.clips.empty());
    for (Clip& clip : document.clips) {
        clip.events.clear(); // a clip with nothing of its own still owes one
    }

    M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->subTrackCollections.size() == written->sequences.size());

    for (std::size_t i = 0; i < written->subTrackCollections.size(); ++i) {
        const m3::SubTrackContainer& stc = written->subTrackCollections[i];
        REQUIRE(stc.sdev.size() == 1);
        const m3::AnimBlock<m3::Event>& block = stc.sdev[0];
        REQUIRE_FALSE(block.keys.empty());
        CHECK(block.keys.back().name.rfind("Evt_SeqEnd", 0) == 0);
        CHECK(block.keys.back().eventType == 4u);
        CHECK(block.keys.back().boneIndex == 0xFFFFu);
        CHECK(block.timestamps.back() ==
              static_cast<i32>(written->sequences[i].endFrame));
        // One key is flagged and several are not: 2,317 of 2,317 single-key
        // blocks carry 1 and all 61 longer ones carry 0.
        CHECK(block.flags == (block.keys.size() == 1 ? 1u : 0u));
    }
}

TEST_CASE("wem m3 the end marker is replaced on a round trip, never doubled",
          "[wem][anim][m3]") {
    // The marker comes back as a clip event like any other, so an export that
    // appends its own would grow one key per trip -- and the block's flag,
    // which is 1 only while there is a single key, would flip with it.
    m3::Model model = makeModel();
    keyTranslation(model, 11, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    M3Converter converter;
    const Result<m3::Model> once = converter.toM3(convert(model), ProfileId::Sc2, 29);
    REQUIRE(once.ok());
    const Result<m3::Model> twice = converter.toM3(convert(*once), ProfileId::Sc2, 29);
    REQUIRE(twice.ok());
    REQUIRE(once->subTrackCollections.size() == twice->subTrackCollections.size());

    for (std::size_t i = 0; i < twice->subTrackCollections.size(); ++i) {
        REQUIRE(once->subTrackCollections[i].sdev.size() == 1);
        REQUIRE(twice->subTrackCollections[i].sdev.size() == 1);
        const m3::AnimBlock<m3::Event>& before = once->subTrackCollections[i].sdev[0];
        const m3::AnimBlock<m3::Event>& after = twice->subTrackCollections[i].sdev[0];
        CHECK(after.keys.size() == before.keys.size());
        CHECK(after.flags == before.flags);
        std::size_t markers = 0;
        for (const m3::Event& key : after.keys) {
            if (key.name.rfind("Evt_SeqEnd", 0) == 0) {
                ++markers;
            }
        }
        CHECK(markers == 1);
    }
}

TEST_CASE("wem m3 a container is named for the clip it plays under", "[wem][anim][m3]") {
    // Formats with no container of their own all name theirs `base`, so every
    // sequence exported a container by the same name; retail qualifies each
    // with its sequence and 784 of 794 shipped models keep them distinct.
    m3::Model model = makeModel();
    model.subTrackCollections[0].name = "base";
    keyTranslation(model, 11, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    Document document = convert(model);
    REQUIRE_FALSE(document.clips.empty());
    document.clips[0].containers[0].name = "base";

    M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->subTrackCollections.empty());
    CHECK(written->subTrackCollections[0].name == "Stand_base");
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

TEST_CASE("wem m3 an external clip keeps the record it was written from",
          "[wem][anim][m3]") {
    m3::Model base = makeModel();
    keyTranslation(base, 7, {0}, {Vector3f{0, 0, 0}});
    Document document = convert(base);

    m3::Model external;
    m3::Sequence sequence;
    sequence.id = 41;
    sequence.name = "Attack";
    sequence.startFrame = 0;
    sequence.endFrame = 500;
    sequence.frequency = 100;
    sequence.blendTime = 250;
    sequence.bounds.min = Vector3f{-1, -2, -3};
    sequence.bounds.max = Vector3f{4, 5, 6};
    sequence.bounds.radius = 7.0f;
    external.sequences.push_back(sequence);

    m3::AnimationGroup group;
    group.subtrackIndices = {0};
    external.animationGroups.push_back(group);

    m3::SubTrackContainer stc;
    m3::AnimBlock<Vector3f> known;
    known.timestamps = {0, 500};
    known.keys = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}};
    stc.sd3v.push_back(std::move(known));
    stc.animIds.push_back(7);
    stc.animRefs.push_back(Ref(2, 0));
    external.subTrackCollections.push_back(std::move(stc));

    M3Converter converter;
    REQUIRE(converter.mergeAnimation(document, 0, external).ok());
    REQUIRE(document.clips.size() == 2u);
    const Clip& added = document.clips[1];

    // The scalars an importing host reads back off the record, which an
    // external sequence has exactly as much as an authored one.
    CHECK(added.native.value("blendTime", -1) == 250);
    CHECK(added.native.value("m3Frequency", -1) == 100);
    CHECK(added.native.value("startFrame", -1) == 0);
    CHECK(added.native.value("external", 0) == 1);

    // The bound is the sequence's own, through the basis change: SC2's (x, y)
    // becomes (-y, x), so the x extent comes from the y one negated.
    CHECK(added.bounds.minimum.x == -5.0f);
    CHECK(added.bounds.maximum.x == 2.0f);
    CHECK(added.bounds.minimum.y == -1.0f);
    CHECK(added.bounds.maximum.y == 4.0f);
    CHECK(added.bounds.minimum.z == -3.0f);
    CHECK(added.bounds.maximum.z == 6.0f);
    CHECK(added.bounds.sphereRadius == 7.0f);
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

// ============================================================================
// The UV-animation crossing (WOW_TO_SC2_DESIGN.md): a source-convention
// feature channel (M2/MDX's three-float translate, quaternion rotate) becomes
// the layer's own offset/angle streams. Two things a green export cannot show:
// the values must land in the stream the layer's Vector2/Vector3 AnimRef
// reads (SD2V/SD3V, never SD3V/SD4Q), and the source's (0.5, 0.5) pivot must
// land in the OFFSET -- M3 composes about the origin.
// ============================================================================

namespace {

void pushFloats(std::vector<u8>& values, std::initializer_list<f32> parts) {
    for (const f32 part : parts) {
        const u8* bytes = reinterpret_cast<const u8*>(&part);
        values.insert(values.end(), bytes, bytes + sizeof(f32));
    }
}

/// A UvAnimation feature on layer 0 of slot @p slot's material, plus one
/// channel of @p kind targeting it, keyed in @p clip.
u32 addUvChannel(Document& document, u32 slot, u32 featureId, Channel kind,
                 geom::AttrType type, std::vector<f32> times, std::vector<u8> values) {
    Model& model = document.models[0];
    Material& material = model.profileSets[0].materials[slot];
    bool hasFeature = false;
    for (const MaterialFeature& feature : material.MutableCommon().features) {
        hasFeature = hasFeature || feature.id == featureId;
    }
    if (!hasFeature) {
        MaterialFeature feature;
        feature.id = featureId;
        feature.layer = 0;
        feature.payload = UvAnimationFeature{};
        material.MutableCommon().features.push_back(feature);
    }

    AnimChannel channel;
    // Never 0: id 0 is legal WEM but the exporter remaps it at the boundary
    // ("not animated" to every `.m3` consumer), which is not this case's story.
    channel.id = (std::max)(1u, model.animChannels.nextFreeId());
    channel.target.kind = TrackTarget::Kind::MaterialFeature;
    channel.target.material.profile = ProfileId::Sc2;
    channel.target.material.slot = slot;
    channel.target.material.look = 0;
    channel.target.sub = featureId;
    channel.target.channel = kind;
    channel.valueType = type;
    model.animChannels.add(channel);

    SubTrack track;
    track.channel = channel.id;
    track.interp = kind == Channel::UvRotate ? Interpolation::Slerp : Interpolation::Linear;
    track.times = std::move(times);
    track.values = std::move(values);
    document.clips[0].containers[0].subTracks.push_back(std::move(track));
    return channel.id;
}

/// The one STC entry for @p animId: REQUIREs it exists, returns its animRef.
u32 stcRefFor(const m3::Model& model, u32 animId) {
    for (const m3::SubTrackContainer& stc : model.subTrackCollections) {
        for (std::size_t i = 0; i < stc.animIds.size(); ++i) {
            if (stc.animIds[i] == animId && i < stc.animRefs.size()) {
                return stc.animRefs[i];
            }
        }
    }
    FAIL("animId " << animId << " is in no STC");
    return 0;
}

const m3::SubTrackContainer& stcHolding(const m3::Model& model, u32 animId) {
    for (const m3::SubTrackContainer& stc : model.subTrackCollections) {
        for (const u32 id : stc.animIds) {
            if (id == animId) {
                return stc;
            }
        }
    }
    FAIL("animId " << animId << " is in no STC");
    return model.subTrackCollections[0];
}

} // namespace

TEST_CASE("wem m3 a three-float UV translate becomes the offset stream, negated",
          "[wem][anim][m3][uv]") {
    Document document = wemfix::makeDocument(ProfileId::Sc2);
    Clip clip;
    clip.name = "Stand";
    clip.model = 0;
    clip.duration = 1.0f;
    clip.containers.push_back(SubTrackContainer{});
    document.clips.push_back(std::move(clip));

    std::vector<u8> keys;
    pushFloats(keys, {0.0f, 0.0f, 0.0f});
    pushFloats(keys, {0.25f, 0.5f, 0.0f});
    const u32 id = addUvChannel(document, 0, 7, Channel::UvTranslate, geom::AttrType::F32x3,
                                {0.0f, 1.0f}, std::move(keys));

    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());

    const auto& material =
        written->standardMaterials[written->materialMaps[0].materialIndex];
    REQUIRE(material.diffuseLayer.has_value());
    CHECK(material.diffuseLayer->uvOffset.animId == id);

    // SD2V -- slot 1. Landing in SD3V is one of the two defects this case
    // exists for: the layer's AnimRef is a Vector2 and a Vector3 stream never
    // joins it.
    const u32 ref = stcRefFor(*written, id);
    REQUIRE((ref >> 16) == 1u);
    const auto& stc = stcHolding(*written, id);
    const auto& block = stc.sd2v[ref & 0xFFFFu];
    REQUIRE(block.keys.size() == 2u);
    CHECK(block.keys[0].x == 0.0f);
    // The other: WoW and Warcraft III ADD the translation inside the (0.5,0.5)
    // pivot and StarCraft II SUBTRACTS it inside the same one, so the offset is
    // the source's own translation with its sign turned over. Written as it
    // stood, every crossed layer scrolled backwards.
    CHECK(block.keys[1].x == Catch::Approx(-0.25f));
    CHECK(block.keys[1].y == Catch::Approx(-0.5f));
}

TEST_CASE("wem m3 a UV rotation crosses as the angle and moves no offset",
          "[wem][anim][m3][uv]") {
    Document document = wemfix::makeDocument(ProfileId::Sc2);
    Clip clip;
    clip.name = "Stand";
    clip.model = 0;
    clip.duration = 1.0f;
    clip.containers.push_back(SubTrackContainer{});
    document.clips.push_back(std::move(clip));

    // Identity, then 90 degrees about z -- the only axis a UV plane has.
    const f32 half = 0.70710678f;
    std::vector<u8> keys;
    pushFloats(keys, {0.0f, 0.0f, 0.0f, 1.0f});
    pushFloats(keys, {0.0f, 0.0f, half, half});
    const u32 id = addUvChannel(document, 0, 7, Channel::UvRotate, geom::AttrType::Quat,
                                {0.0f, 1.0f}, std::move(keys));

    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());

    const auto& material =
        written->standardMaterials[written->materialMaps[0].materialIndex];
    REQUIRE(material.diffuseLayer.has_value());
    CHECK(material.diffuseLayer->uvAngle.animId == id);

    // The angle: SD3V, radians in z.
    const u32 angleRef = stcRefFor(*written, id);
    REQUIRE((angleRef >> 16) == 2u);
    const auto& angles = stcHolding(*written, id).sd3v[angleRef & 0xFFFFu];
    REQUIRE(angles.keys.size() == 2u);
    CHECK(angles.keys[0].z == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(angles.keys[1].z == Catch::Approx(1.5707963f));

    // The pivot: both engines turn about (0.5, 0.5), so a rotation with no
    // translation channel leaves the offset exactly where it was. It used to
    // synthesize an offset track carrying `(I - R) * (0.5, 0.5)` -- (1, 0) at a
    // quarter turn -- which double-applied a pivot the layer already has.
    CHECK(material.diffuseLayer->uvOffset.animId == 0u);

    // The quaternion itself must be gone: nothing in the file reads SD4Q here.
    for (const m3::SubTrackContainer& stc : written->subTrackCollections) {
        CHECK(stc.sd4q.empty());
    }
}

// ============================================================================
// What the retail sampler cannot absorb
// ============================================================================

namespace {

f32 dotOf(const Quaternion& a, const Quaternion& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

SubTrack& subTrackOf(Clip& clip, u32 channel) {
    for (SubTrack& track : clip.containers[0].subTracks) {
        if (track.channel == channel) {
            return track;
        }
    }
    FAIL("channel " << channel << " has no sub-track");
    return clip.containers[0].subTracks[0];
}

} // namespace

TEST_CASE("wem m3 a quaternion stream is written in one hemisphere", "[wem][anim][m3]") {
    // Retail lerps the four components as stored -- no slerp, no sign test --
    // so keys that alternate q, -q (every Warcraft III Max export) pass near
    // zero between each pair and the bone trembles at key rate. Written
    // continuous, and the first key on the rest value's side.
    const f32 c = 0.99619f;
    const f32 s = 0.08715f; // ten degrees about z, spelled both ways
    m3::Model model = makeModel();
    model.bones[0].rotation = animated<Quaternion>(8, Quaternion{0, 0, 0, 1});
    m3::AnimBlock<Quaternion> block;
    block.timestamps = {0, 333, 667, 1000};
    block.keys = {Quaternion{0, 0, -s, -c}, Quaternion{0, 0, s, c}, Quaternion{0, 0, -s, -c},
                  Quaternion{0, 0, s, c}};
    model.subTrackCollections[0].sd4q.push_back(std::move(block));
    model.subTrackCollections[0].animIds.push_back(8);
    model.subTrackCollections[0].animRefs.push_back(Ref(3, 0));

    const Document document = convert(model);
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());

    const u32 ref = stcRefFor(*written, 8);
    REQUIRE((ref >> 16) == 3u);
    const auto& keys = stcHolding(*written, 8).sd4q[ref & 0xFFFFu].keys;
    REQUIRE(keys.size() == 4u);
    CHECK(dotOf(written->bones[0].rotation.initValue, keys[0]) > 0.0f);
    for (std::size_t k = 1; k < keys.size(); ++k) {
        CHECK(dotOf(keys[k - 1], keys[k]) > 0.0f);
    }
    // The rotation itself is untouched: q and -q are the same turn.
    CHECK(std::fabs(keys[1].z) == Catch::Approx(s));
    CHECK(std::fabs(keys[1].w) == Catch::Approx(c));
}

TEST_CASE("wem m3 a rotation is written in spans the engine's lerp can carry",
          "[wem][anim][m3]") {
    // The engine lerps the four components raw and builds the basis from the
    // result with no normalise, so a wide span sags INSIDE the unit sphere and
    // the bone shrinks mid-span. Warcraft III slerps and spends keys
    // accordingly -- Zhao Yun's ground discs turn on four quarter-turn keys --
    // so the arc is sampled on the way out. The turn is unchanged; only the
    // number of keys stating it grows.
    const f32 h = 0.70710678f; // a quarter turn about z, and a half turn
    m3::Model model = makeModel();
    model.bones[0].rotation = animated<Quaternion>(8, Quaternion{0, 0, 0, 1});
    m3::AnimBlock<Quaternion> block;
    block.timestamps = {0, 1000};
    block.keys = {Quaternion{0, 0, 0, 1}, Quaternion{0, 0, h, h}};
    model.subTrackCollections[0].sd4q.push_back(std::move(block));
    model.subTrackCollections[0].animIds.push_back(8);
    model.subTrackCollections[0].animRefs.push_back(Ref(3, 0));

    const Document document = convert(model);
    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
    REQUIRE(written.ok());

    const u32 ref = stcRefFor(*written, 8);
    const auto& out = stcHolding(*written, 8).sd4q[ref & 0xFFFFu];
    REQUIRE(out.keys.size() >= 4u); // 90 degrees needs three spans of 30
    REQUIRE(out.keys.size() == out.timestamps.size());
    for (std::size_t k = 1; k < out.keys.size(); ++k) {
        CHECK(out.timestamps[k] > out.timestamps[k - 1]);
        // Every span inside 30 degrees of turn: cos(15 degrees) on the halves.
        CHECK(dotOf(out.keys[k - 1], out.keys[k]) > 0.9659f);
        // And every key still ON the arc -- a lerped one would sag.
        const Quaternion& q = out.keys[k];
        CHECK(std::sqrt(dotOf(q, q)) == Catch::Approx(1.0f).margin(1e-4f));
    }
    // The ends are the keys the source stated, and the turn between them the
    // one it meant.
    CHECK(out.timestamps.front() == 0);
    CHECK(out.timestamps.back() == 1000);
    CHECK(dotOf(out.keys.front(), Quaternion{0, 0, 0, 1}) == Catch::Approx(1.0f).margin(1e-4f));
    CHECK(dotOf(out.keys.back(), Quaternion{0, 0, h, h}) == Catch::Approx(1.0f).margin(1e-4f));
}

TEST_CASE("wem m3 a Warcraft window is keyed at both edges the way its engine plays them",
          "[wem][anim][m3]") {
    // Warcraft III reads only the keys inside a window and plays the span past
    // the last key -- and the one before the first -- as a single segment
    // back to the first key. M3 holds before its first key and loops a track
    // on its own last stamp, so each edge the track does not reach gets the
    // value the engine shows there; the slicer's bracket keys, which that
    // engine never reads, go.
    m3::Model model = makeModel();
    keyTranslation(model, 7, {200, 800}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 10}});
    Document document = convert(model);
    Clip& clip = document.clips[0];
    SubTrack& track = subTrackOf(clip, 7);
    // One bracket key past each edge, the way the .mdx slicer leaves them.
    // Everything rides z, the axis the basis change leaves alone.
    track.times = {-0.1f, 0.2f, 0.8f, 1.1f};
    track.values.clear();
    for (const f32 z : {-3.0f, 0.0f, 10.0f, 13.0f}) {
        const Vector3f v{0, 0, z};
        const u8* bytes = reinterpret_cast<const u8*>(&v);
        track.values.insert(track.values.end(), bytes, bytes + sizeof(v));
    }

    const M3Converter converter;
    SECTION("a Warcraft window") {
        clip.native.set("intervalStart", static_cast<i64>(0));
        clip.native.set("intervalEnd", static_cast<i64>(1000));
        Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
        REQUIRE(written.ok());
        const u32 ref = stcRefFor(*written, 7);
        const auto& block = stcHolding(*written, 7).sd3v[ref & 0xFFFFu];
        REQUIRE(block.timestamps == std::vector<i32>{0, 200, 800, 1000});
        // Half way from the last key back to the first: 200 ms into the
        // 400 ms the wrap segment spans -- at the end, and at the start.
        CHECK(block.keys[0].z == Catch::Approx(5.0f));
        CHECK(block.keys[1].z == Catch::Approx(0.0f));
        CHECK(block.keys[2].z == Catch::Approx(10.0f));
        CHECK(block.keys[3].z == Catch::Approx(5.0f));
    }
    SECTION("any other clip holds the bracket before it and drops the one after") {
        clip.native = NativeBag{};
        Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2, 29);
        REQUIRE(written.ok());
        const u32 ref = stcRefFor(*written, 7);
        const auto& block = stcHolding(*written, 7).sd3v[ref & 0xFFFFu];
        REQUIRE(block.timestamps == std::vector<i32>{0, 200, 800});
        CHECK(block.keys[0].z == Catch::Approx(-3.0f));
    }
}

TEST_CASE("wem m3 a smooth track states a non-zero interpType", "[wem][anim][m3]") {
    // The row lies at SAMPLE time -- the import is right to ignore it -- but
    // the loader reads the file's value once on the way in, and every chunk's
    // fixup folds it into the step bit the same way (BONE at 0x141EAB2B0, LAYR
    // at 0x141EABA80, ...):
    //
    //     if (interpType == 0) flags |= 0x10;   // then interpType = -1
    //
    // So writing 0 on everything, as this did, stepped every track of every
    // converted model however clear bit 4 was.
    const M3Converter converter;

    m3::Model source = makeModel();
    keyTranslation(source, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}}, 0);
    Result<m3::Model> written = converter.toM3(convert(source), ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->bones.empty());
    CHECK((written->bones[0].position.flags & 0x10u) == 0u);
    CHECK(written->bones[0].position.interpType != 0);

    m3::Model stepped = makeModel();
    keyTranslation(stepped, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}}, 0x10);
    written = converter.toM3(convert(stepped), ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->bones.empty());
    // A step may be spelt either way round -- the loader ORs them -- so this
    // asks only that the track still steps once the file has been read.
    const m3::AnimRef<Vector3f>& held = written->bones[0].position;
    CHECK(((held.flags & 0x10u) != 0u || held.interpType == 0));
}

TEST_CASE("wem m3 every bone states the rest a sequence falls back to", "[wem][anim][m3]") {
    // `nullValue` is what a property rests at in a sequence whose `STC_` does
    // not name its `animId`. It is a constant per property: all 736,433 bones
    // in the corpus state these four and no others. The struct's own default
    // is zero -- a null rotation and a zero scale, which collapse whatever the
    // bone skins the moment one clip leaves it out.
    m3::Model source = makeModel();
    keyTranslation(source, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(convert(source), ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE_FALSE(written->bones.empty());
    for (const m3::Bone& bone : written->bones) {
        CHECK(bone.position.nullValue.x == 0.0f);
        CHECK(bone.position.nullValue.y == 0.0f);
        CHECK(bone.position.nullValue.z == 0.0f);
        CHECK(bone.rotation.nullValue.x == 0.0f);
        CHECK(bone.rotation.nullValue.y == 0.0f);
        CHECK(bone.rotation.nullValue.z == 0.0f);
        CHECK(bone.rotation.nullValue.w == 1.0f);
        CHECK(bone.scale.nullValue.x == 1.0f);
        CHECK(bone.scale.nullValue.y == 1.0f);
        CHECK(bone.scale.nullValue.z == 1.0f);
        CHECK(bone.visibility.nullValue == 1u);
    }
}

TEST_CASE("wem m3 a bound AnimRef says it is bound", "[wem][anim][m3]") {
    // Bit 1 of `AnimRef.flags` is what tells the engine a track answers this
    // property and bit 2 latches that the track was this model's own. All
    // 15,633 bound bone AnimRefs measured over 1,169 shipped self-animated
    // models write 0x0006 and all 56,519 unbound ones write 0 -- no third
    // value exists. The converter wrote 0 on every ref it bound.
    m3::Model source = makeModel();
    addChildBone(source);
    keyTranslation(source, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(convert(source), ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->bones.size() >= 2);

    u32 boundRefs = 0;
    const auto states = [&](const auto& ref) {
        if (ref.animId != 0) {
            ++boundRefs;
            CHECK(ref.flags == 0x6u);
        } else {
            CHECK(ref.flags == 0u);
        }
    };
    for (const m3::Bone& bone : written->bones) {
        states(bone.position);
        states(bone.rotation);
        states(bone.scale);
        states(bone.visibility);
    }
    CHECK(boundRefs > 0);
}

TEST_CASE("wem m3 an animated bone is flagged animated, and so is what it carries",
          "[wem][anim][m3]") {
    // `sub_141EAD8F0`, the Galaxy editor's own solver: a bone with a bound SRT
    // ref takes 0x2200, and any bone under one takes 0x2000. Transcribed here
    // because the editor runs it BEFORE the pass that repairs `AnimRef.flags`
    // and then latches the result in `MODL.flags` -- so a file that states
    // neither leaves every bone marked un-animated for good and stands in its
    // bind pose, mesh and shadow alike, whatever its tracks say.
    m3::Model source = makeModel();
    addChildBone(source);
    keyTranslation(source, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(convert(source), ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->bones.size() >= 2);

    CHECK((static_cast<u32>(written->bones[0].flags) & 0x2200u) == 0x2200u);
    // The child animates only because its parent does, so it takes the
    // inherited bit and not the "a track names me" one.
    CHECK((static_cast<u32>(written->bones[1].flags) & 0x2000u) != 0u);
    CHECK((static_cast<u32>(written->bones[1].flags) & 0x200u) == 0u);
}

TEST_CASE("wem m3 a built model latches only the work it actually did",
          "[wem][anim][m3]") {
    // `MODL.flags` bits are "already done, do not redo it" latches, and only 21
    // of the corpus's 56,146 models leave the field at zero. The converter
    // earns exactly three of them, and each claim is checked here beside it.
    m3::Model source = makeModel();
    keyTranslation(source, 7, {0, 1000}, {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}});

    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(convert(source), ProfileId::Sc2, 29);
    REQUIRE(written.ok());

    const u32 flags = static_cast<u32>(written->flags);
    CHECK((flags & static_cast<u32>(m3::ModelFlag::TrackCollectionSorted)) != 0u);
    CHECK((flags & static_cast<u32>(m3::ModelFlag::TrackAnimatedBaseFlagValid)) != 0u);
    CHECK((flags & static_cast<u32>(m3::ModelFlag::BoneAnimatedFlagSolved)) != 0u);

    // TrackCollectionSorted is a promise the engine binary-searches on.
    for (const m3::SubTrackContainer& stc : written->subTrackCollections) {
        CHECK(std::is_sorted(stc.animIds.begin(), stc.animIds.end()));
    }
}

TEST_CASE("wem m3 every block ends where its sequence does", "[wem][anim][m3]") {
    // 61,344 of 61,344 shipped blocks end at their sequence end, the 1,550
    // single-key ones included. Ended at its last key instead, a geoset hidden
    // by one key of 0 at frame 0 ended at 0 -- and the Galaxy editor drew it.
    m3::Model model = makeModel();
    model.bones[0].visibility = animated<u32>(9, 1u);
    m3::AnimBlock<m3::Flag> hidden;
    hidden.timestamps = {0};
    hidden.keys = {m3::Flag{0}};
    model.subTrackCollections[0].sdfg.push_back(std::move(hidden));
    model.subTrackCollections[0].animIds.push_back(9);
    model.subTrackCollections[0].animRefs.push_back(Ref(11, 0));
    keyTranslation(model, 7, {0, 200, 800},
                   {Vector3f{0, 0, 0}, Vector3f{0, 0, 1}, Vector3f{0, 0, 2}});

    const M3Converter converter;
    Result<m3::Model> written = converter.toM3(convert(model), ProfileId::Sc2, 29);
    REQUIRE(written.ok());
    REQUIRE(written->sequences.size() == 1u);
    const u32 end = written->sequences[0].endFrame;
    CHECK(end == 1000u);

    const u32 flagRef = stcRefFor(*written, 9);
    const auto& flagBlock = stcHolding(*written, 9).sdfg[flagRef & 0xFFFFu];
    REQUIRE(flagBlock.timestamps.size() == 1u);
    CHECK(flagBlock.endFrame == end);

    const u32 moveRef = stcRefFor(*written, 7);
    const auto& moveBlock = stcHolding(*written, 7).sd3v[moveRef & 0xFFFFu];
    CHECK(moveBlock.timestamps.back() == 800);
    CHECK(moveBlock.endFrame == end);
}

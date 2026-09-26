// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// G-D (WEM_ANIMATION_RUNTIME_PLAN.md §5): the export's crossing pass does
/// nothing to a document that does not cross, and what it does to one that
/// does keeps the motion within tolerance.

#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>
#include <whiteout/models/m3/writer.h>
#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/mdx/writer.h>
#include <whiteout/models/wem/anim/crossing.h>
#include <whiteout/models/wem/anim/rests.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/reflect_bytes.h>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

Quaternion AboutZ(f32 degrees) {
    const f32 half = degrees * 3.14159265f / 360.0f;
    return Quaternion{0, 0, std::sin(half), std::cos(half)};
}

/// The turn between two quaternions, in degrees, and how much their lengths
/// part.
f32 Degrees(const Quaternion& a, const Quaternion& b) {
    const f32 la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    const f32 lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
    const f32 dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) / (la * lb);
    return 2.0f * std::acos(std::min(dot, 1.0f)) * 180.0f / 3.14159265f;
}

/// A model of one bone keyed by a rotation, in a clip of @p rule.
Document RotationDocument(ReadRule rule, std::vector<f32> times, std::vector<Quaternion> keys) {
    Document document;
    document.profiles = {ProfileId::Wc3Reforged, ProfileId::Sc2};
    document.defaultProfile = rule == ReadRule::Sc2 ? ProfileId::Sc2 : ProfileId::Wc3Reforged;
    document.models.emplace_back();
    Model& model = document.models.back();
    Node bone;
    bone.name = "bone";
    bone.kind = NodeKind::Bone;
    model.nodes.add(bone);
    AnimChannel channel;
    channel.id = 1;
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = 0;
    channel.target.channel = Channel::Rotation;
    channel.valueType = geom::AttrType::Quat;
    model.animChannels.add(channel);
    Clip clip;
    clip.name = "Stand";
    clip.model = 0;
    clip.duration = times.back();
    clip.looping = true;
    clip.readRule = rule;
    SubTrack track;
    track.channel = 1;
    track.interp = Interpolation::Slerp;
    track.times = std::move(times);
    for (const Quaternion& q : keys) {
        const u8* bytes = reinterpret_cast<const u8*>(&q);
        track.values.insert(track.values.end(), bytes, bytes + sizeof(q));
    }
    clip.containers.emplace_back();
    clip.containers[0].subTracks.push_back(std::move(track));
    document.clips.push_back(std::move(clip));
    return document;
}

} // namespace

TEST_CASE("wem crossing leaves a document that does not cross as it was",
          "[wem][anim][crossing][corpus]") {
    u32 compared = 0;
    {
        const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
        const std::size_t limit = test::sweepLimit(files.size(), 12);
        const MdxConverter converter;
        for (std::size_t i = 0; i < limit; ++i) {
            if (test::isKnownBad(files[i])) {
                continue;
            }
            const std::vector<u8> bytes = test::readCorpusFile(files[i]);
            if (bytes.empty()) {
                continue;
            }
            mdx::Parser parser;
            const mdx::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
            const Result<Document> converted = converter.fromMdx(source);
            if (!converted.ok() || converted->models.empty() || converted->clips.empty()) {
                continue;
            }
            Document staged = *converted.value;
            Diagnostics diagnostics;
            INFO(test::pathText(files[i]));
            CHECK(ResampleForTarget(staged, ProfileId::Wc3Reforged, Game::Warcraft, diagnostics) == 0);
            CHECK(FlattenContainers(staged, diagnostics) == 0);
            const bool same = ReflectBytes(staged) == ReflectBytes(*converted.value);
            CHECK(same);
            ++compared;
        }
    }
    {
        const auto files =
            test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
        const std::size_t limit = test::sweepLimit(files.size(), 12);
        const M3Converter converter;
        for (std::size_t i = 0; i < limit; ++i) {
            const std::vector<u8> bytes = test::readCorpusFile(files[i]);
            if (bytes.empty()) {
                continue;
            }
            m3::Parser parser;
            const m3::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
            const Result<Document> converted = converter.fromM3(source, ProfileId::Sc2);
            if (!converted.ok() || converted->models.empty() || converted->clips.empty()) {
                continue;
            }
            Document staged = *converted.value;
            Diagnostics diagnostics;
            INFO(test::pathText(files[i]));
            CHECK(ResampleForTarget(staged, ProfileId::Sc2, Game::StarCraft, diagnostics) == 0);
            const bool same = ReflectBytes(staged) == ReflectBytes(*converted.value);
            CHECK(same);
            // And written key for key: every block holds the clip's own keys.
            const Result<m3::Model> written = converter.toM3(staged, ProfileId::Sc2);
            REQUIRE(written.ok());
            ++compared;
        }
    }
    CHECK(compared > 0);
}

TEST_CASE("wem crossing a wide Warcraft III sweep to StarCraft II keeps it within 0.1 degree",
          "[wem][anim][crossing]") {
    Document document = RotationDocument(ReadRule::Wc3, {0.0f, 1.0f}, {AboutZ(0), AboutZ(170)});
    const Document before = document;
    Diagnostics diagnostics;
    CHECK(ResampleForTarget(document, ProfileId::Sc2, Game::Warcraft, diagnostics) == 1);
    CHECK(diagnostics.countOf(DiagCode::AnimTrackResampled) == 1);
    REQUIRE(document.clips[0].readRule == ReadRule::Sc2);
    const SubTrack& crossed = document.clips[0].containers[0].subTracks[0];
    CHECK(crossed.times.size() > 2);

    // Up to the loop's end: at 1000 a looping M3 track has wrapped to its first
    // key, which is the same instant of the loop as Warcraft III's last.
    f32 worst = 0;
    for (i32 ms = 0; ms < 1000; ++ms) {
        const Quaternion game = SampleQuat(before.clips[0], before.clips[0].containers[0].subTracks[0],
                                           ClipWindow(before.clips[0], static_cast<u32>(ms), -1),
                                           Quaternion{0, 0, 0, 1});
        const Quaternion file = SampleQuat(document.clips[0], crossed,
                                           ClipWindow(document.clips[0], static_cast<u32>(ms), -1),
                                           Quaternion{0, 0, 0, 1});
        worst = std::max(worst, Degrees(game, file));
    }
    CHECK(worst <= 0.11f);
}

TEST_CASE("wem an Sc2 rotation track is written key for key, hemispheres and all",
          "[wem][anim][crossing]") {
    // Two keys in opposite hemispheres: under the raw lerp that is a motion
    // (the bone passes through a short quaternion), and the file must keep it.
    const Quaternion a = AboutZ(20);
    const Quaternion b{-AboutZ(40).x, -AboutZ(40).y, -AboutZ(40).z, -AboutZ(40).w};
    Document document = RotationDocument(ReadRule::Sc2, {0.0f, 0.5f, 1.0f}, {a, b, a});
    Diagnostics diagnostics;
    CHECK(ResampleForTarget(document, ProfileId::Sc2, Game::StarCraft, diagnostics) == 0);
    const M3Converter converter;
    const Result<m3::Model> written = converter.toM3(document, ProfileId::Sc2);
    REQUIRE(written.ok());
    const m3::Model& m3 = *written.value;
    REQUIRE_FALSE(m3.subTrackCollections.empty());
    const auto& blocks = m3.subTrackCollections[0].sd4q;
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].keys.size() == 3);
    // The export un-rebases a rotation (x, y) -> (y, -x); about z that is
    // nothing, so each key is the document's own, sign included.
    CHECK(blocks[0].keys[1].w == b.w);
    CHECK(blocks[0].keys[1].z == b.z);
    CHECK(blocks[0].timestamps == std::vector<i32>{0, 500, 1000});
}

TEST_CASE("wem converting clips leaves a global loop's channels to the loop",
          "[wem][anim][crossing]") {
    // A light whose intensity only a global loop keys: Warcraft III rests it at
    // 0 in the clips, StarCraft II at its static 5, and neither clip may be
    // given a key for the other's channel.
    Document document = RotationDocument(ReadRule::Wc3, {0.0f, 1.0f}, {AboutZ(0), AboutZ(90)});
    Model& model = document.models[0];
    Node lamp;
    lamp.name = "lamp";
    lamp.kind = NodeKind::Light;
    LightPayload light;
    light.intensity = 5.0f;
    lamp.payload = light;
    model.nodes.add(lamp);
    AnimChannel intensity;
    intensity.id = 2;
    intensity.target.kind = TrackTarget::Kind::Node;
    intensity.target.node = 1;
    intensity.target.channel = Channel::Intensity;
    intensity.valueType = geom::AttrType::F32;
    model.animChannels.add(intensity);
    Clip loop;
    loop.name = "Flicker";
    loop.model = 0;
    loop.duration = 2.0f;
    loop.looping = true;
    loop.flags = ClipFlags::AutoPlay | ClipFlags::WorldClocked;
    loop.readRule = ReadRule::Wc3;
    SubTrack track;
    track.channel = 2;
    track.interp = Interpolation::Linear;
    track.times = {0.0f, 2.0f};
    for (const f32 v : {1.0f, 3.0f}) {
        const u8* bytes = reinterpret_cast<const u8*>(&v);
        track.values.insert(track.values.end(), bytes, bytes + sizeof(v));
    }
    loop.containers.emplace_back();
    loop.containers[0].subTracks.push_back(std::move(track));
    document.clips.push_back(std::move(loop));

    Diagnostics diagnostics;
    const std::vector<u32> clips{0, 1};
    ConvertClips(document, clips, ReadRule::Sc2, Game::Warcraft, Game::StarCraft, diagnostics);
    CHECK(FindSubTrack(document.clips[0], 2) == nullptr);
    CHECK(FindSubTrack(document.clips[1], 1) == nullptr);
    CHECK(document.clips[0].readRule == ReadRule::Sc2);
}

TEST_CASE("wem an empty bound block does not hide another layer's keys", "[wem][anim][crossing]") {
    Document document = RotationDocument(ReadRule::Sc2, {0.0f, 1.0f}, {AboutZ(0), AboutZ(90)});
    Clip& clip = document.clips[0];
    SubTrackContainer base;
    SubTrack empty;
    empty.channel = 1;
    empty.interp = Interpolation::Linear;
    base.subTracks.push_back(empty);
    clip.containers.insert(clip.containers.begin(), std::move(base));
    const SubTrack* found = FindSubTrack(clip, 1);
    REQUIRE(found != nullptr);
    CHECK(found->times.size() == 2);
}

TEST_CASE("wem a two-float track reads between its keys", "[wem][anim][crossing]") {
    Clip clip;
    clip.duration = 1.0f;
    clip.readRule = ReadRule::Sc2;
    SubTrack track;
    track.channel = 1;
    track.interp = Interpolation::Linear;
    track.times = {0.0f, 1.0f};
    for (const Vector2f v : {Vector2f{0, 0}, Vector2f{2, 4}}) {
        const u8* bytes = reinterpret_cast<const u8*>(&v);
        track.values.insert(track.values.end(), bytes, bytes + sizeof(v));
    }
    const std::vector<u8> fallback(sizeof(Vector2f), 0);
    const std::vector<u8> value =
        SampleSubTrack(clip, track, geom::AttrType::F32x2, ClipWindow(clip, 500, -1), fallback, false);
    REQUIRE(value.size() == sizeof(Vector2f));
    Vector2f half;
    std::memcpy(&half, value.data(), sizeof(half));
    CHECK(std::fabs(half.x - 1.0f) < 1e-4f);
    CHECK(std::fabs(half.y - 2.0f) < 1e-4f);
}

TEST_CASE("wem StarCraft II rests a channel at its own initValue", "[wem][anim][crossing]") {
    Document document = RotationDocument(ReadRule::Sc2, {0.0f, 1.0f}, {AboutZ(0), AboutZ(90)});
    AnimChannel alpha;
    alpha.id = 2;
    alpha.target.kind = TrackTarget::Kind::Node;
    alpha.target.node = 0;
    alpha.target.channel = Channel::Alpha;
    alpha.valueType = geom::AttrType::F32;
    const f32 rest = 0.25f;
    const u8* bytes = reinterpret_cast<const u8*>(&rest);
    alpha.initValue.assign(bytes, bytes + sizeof(rest));
    const TrackRests rests = RestsOf(document, 0, alpha, Game::StarCraft);
    CHECK(rests.unkeyed == alpha.initValue);
    CHECK(rests.keyedElsewhere == alpha.initValue);
}

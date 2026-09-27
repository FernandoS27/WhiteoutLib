// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// EDIT_MODE_KEY_OPTIMIZE_DESIGN.md §8: the key reducer's gates.
///
/// Tier 1 (`ReduceKeysExactly`) must leave every channel of every clip reading
/// what it read, at every millisecond, by the clip's own rule — and so must
/// the file the reduced document writes, read back, since a writer merges
/// clips in ways the reader of one sub-track cannot see. Each fixture also
/// says what the pass should have removed: a green that removed nothing proves
/// nothing.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m2/parser.h>
#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/anim/curves.h>
#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/anim/rests.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/nodes/emitters.h>
#include <whiteout/utils/os_file_system.h>

#include "wem_corpus_files.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;

namespace {

template <class T>
std::vector<u8> Bytes(const T& value) {
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

/// One pivot-rig model: a root bone and a child, a light, channels declared on
/// demand, and clips the caller keys.
struct Fixture {
    Document document;

    Fixture() {
        document.models.emplace_back();
        Model& model = document.models.back();
        Node root;
        root.name = "root";
        root.kind = NodeKind::Bone;
        model.nodes.add(root);
        Node child;
        child.name = "child";
        child.kind = NodeKind::Bone;
        child.parent = 0;
        child.pivot = Vector3f{0, 0, 50};
        model.nodes.add(child);
        Node light;
        light.name = "light";
        light.kind = NodeKind::Light;
        light.resetPayloadForKind();
        std::get<LightPayload>(light.payload).color = Vector3f{1, 0, 0};
        model.nodes.add(light);
    }

    Model& model() {
        return document.models[0];
    }

    u32 channel(u32 node, Channel what, geom::AttrType type) {
        AnimChannel entry;
        entry.id = model().animChannels.nextFreeId();
        entry.target.kind = TrackTarget::Kind::Node;
        entry.target.node = node;
        entry.target.channel = what;
        entry.valueType = type;
        return model().animChannels.add(entry);
    }

    u32 clip(ReadRule rule, f32 duration = 1.0f, bool transparent = false, bool looping = true) {
        Clip made;
        made.name = "clip" + std::to_string(document.clips.size());
        made.model = 0;
        made.duration = duration;
        made.looping = looping;
        made.readRule = rule;
        SubTrackContainer container;
        container.concurrent = transparent;
        made.containers.push_back(container);
        document.clips.push_back(made);
        return static_cast<u32>(document.clips.size() - 1);
    }

    /// Gives @p clip the MDX window `[start, end]` it would have been imported with.
    void window(u32 clip, i64 start, i64 end) {
        document.clips[clip].native.set("intervalStart", start);
        document.clips[clip].native.set("intervalEnd", end);
    }

    template <class T>
    SubTrack& key(u32 clip, u32 channel, std::vector<f32> times, std::vector<T> values,
                  Interpolation interp = Interpolation::Linear, std::size_t container = 0) {
        SubTrack track;
        track.channel = channel;
        track.interp = interp;
        track.times = std::move(times);
        for (const T& value : values) {
            const std::vector<u8> bytes = Bytes(value);
            track.values.insert(track.values.end(), bytes.begin(), bytes.end());
        }
        auto& tracks = document.clips[clip].containers[container].subTracks;
        tracks.push_back(std::move(track));
        return tracks.back();
    }

    const SubTrack* track(const Document& from, u32 clip, u32 channel) const {
        for (const SubTrackContainer& container : from.clips[clip].containers) {
            if (const SubTrack* found = container.find(channel)) {
                return found;
            }
        }
        return nullptr;
    }

    std::size_t keys(const Document& from, u32 clip, u32 channel) const {
        const SubTrack* found = track(from, clip, channel);
        return found != nullptr ? found->times.size() : 0;
    }
};

// ---- Reading a channel as the Animator reads a clip of one layer -------------------

i32 LastKeyMs(const Clip& clip) {
    i32 last = 0;
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            if (!track.times.empty()) {
                last = std::max(last, static_cast<i32>(std::lround(track.times.back() * 1000.0f)));
            }
        }
    }
    return last;
}

/// The milliseconds a clip is compared at: every one of a short clip; every
/// key time, the midpoints between them and a 7 ms stride of a long one. An
/// `Sc2` clip is read on past its last key, where a looping track wraps.
std::vector<i32> TimesOf(const Clip& clip) {
    i32 end = static_cast<i32>(ClipWindow(clip, 0, -1).endMs);
    if (end > 0x3FFFFFF) {
        end = static_cast<i32>(ClipMs(clip));
    }
    if (clip.readRule == ReadRule::Sc2) {
        end = 2 * std::max(end, LastKeyMs(clip)) + 1;
    }
    std::vector<i32> out;
    if (end <= 1500) {
        for (i32 t = 0; t <= end; ++t) {
            out.push_back(t);
        }
        return out;
    }
    std::vector<i32> keys;
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            for (const f32 t : track.times) {
                keys.push_back(static_cast<i32>(std::lround(t * 1000.0f)));
            }
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (std::size_t k = 0; k < keys.size(); ++k) {
        out.push_back(keys[k]);
        if (k + 1 < keys.size()) {
            out.push_back((keys[k] + keys[k + 1]) / 2);
        }
    }
    for (i32 t = 0; t <= end; t += 7) {
        out.push_back(t);
    }
    out.push_back(end);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    out.erase(std::remove_if(out.begin(), out.end(), [&](i32 t) { return t < 0 || t > end; }),
              out.end());
    return out;
}

/// @p channel in @p clip at @p times, one element each: the sub-track read by
/// its rule, or the rest an opaque layer fills with.
std::vector<u8> Series(const Document& document, u32 clipIndex, const AnimChannel& channel,
                       const std::vector<i32>& times) {
    const Clip& clip = document.clips[clipIndex];
    TrackRests rests = RestsPlayed(document, 0, channel, GameOf(document.defaultProfile));
    const std::vector<u8> rest = rests.differ() && KeyedAnywhere(document, 0, channel.id)
                                     ? rests.keyedElsewhere
                                     : rests.unkeyed;
    const SubTrack* track = FindSubTrack(clip, channel.id);
    if (track == nullptr || track->times.empty()) {
        std::vector<u8> out;
        for (std::size_t i = 0; i < times.size(); ++i) {
            out.insert(out.end(), rest.begin(), rest.end());
        }
        return out;
    }
    return SampleSubTrackBatch(clip, *track, channel.valueType, times, rest,
                               HeldByRenderer(document, 0, channel));
}

/// The largest finite component of @p series: the size float noise is judged
/// against, as the reducer judges it by its track's.
f32 SizeOf(geom::AttrType type, const std::vector<u8>& series) {
    f32 out = 0.0f;
    for (std::size_t at = 0; type != geom::AttrType::U32 && at + 4 <= series.size(); at += 4) {
        f32 v = 0;
        std::memcpy(&v, series.data() + at, 4);
        if (std::isfinite(v)) {
            out = std::max(out, std::fabs(v));
        }
    }
    return out;
}

/// Whether two reads of one element of @p type are the same within float
/// noise for a channel of @p size: a quaternion either sign.
bool Same(geom::AttrType type, const u8* a, const u8* b, f32 magnitude) {
    const std::size_t size = geom::AttrTypeSize(type);
    if (type == geom::AttrType::U32) {
        return std::memcmp(a, b, size) == 0;
    }
    const std::size_t n = size / sizeof(f32);
    const auto near = [&](f32 sign) {
        for (std::size_t i = 0; i < n; ++i) {
            f32 x = 0;
            f32 y = 0;
            std::memcpy(&x, a + i * 4, 4);
            std::memcpy(&y, b + i * 4, 4);
            if (std::isnan(x) && std::isnan(y)) {
                continue;
            }
            if (!(std::fabs(x - sign * y) <= 1e-5f * std::max(1.0f, magnitude))) {
                return false;
            }
        }
        return true;
    };
    return near(1.0f) || (type == geom::AttrType::Quat && near(-1.0f));
}

std::string Text(geom::AttrType type, const u8* at) {
    std::string out = "(";
    const std::size_t n = geom::AttrTypeSize(type) / 4;
    for (std::size_t i = 0; i < n; ++i) {
        if (type == geom::AttrType::U32) {
            u32 v = 0;
            std::memcpy(&v, at + i * 4, 4);
            out += std::to_string(v);
        } else {
            f32 v = 0;
            std::memcpy(&v, at + i * 4, 4);
            out += std::to_string(v);
        }
        out += i + 1 < n ? ", " : ")";
    }
    return out;
}

struct Reads {
    u64 compared = 0;
    u64 differing = 0;
    std::string first;
};

/// Every channel of every single-layer clip of model 0, read in @p before and
/// @p after at `TimesOf` — matched by id when the tables are the same one, and
/// by target after a round trip through a file.
Reads CompareReads(const Document& before, const Document& after, bool byTarget) {
    Reads out;
    const auto keyOf = [](const TrackTarget& t) {
        return std::make_tuple(static_cast<int>(t.kind), t.node, t.mesh, t.material.slot,
                               static_cast<int>(t.material.profile), t.material.look, t.sub,
                               static_cast<int>(t.channel));
    };
    std::map<decltype(keyOf(TrackTarget{})), const AnimChannel*> others;
    for (const AnimChannel& channel : after.models[0].animChannels.channels) {
        others[keyOf(channel.target)] = &channel;
    }
    const u32 clips = static_cast<u32>(std::min(before.clips.size(), after.clips.size()));
    for (u32 c = 0; c < clips; ++c) {
        if (before.clips[c].model != 0 || before.clips[c].containers.size() != 1 ||
            after.clips[c].containers.size() != 1) {
            continue;
        }
        const std::vector<i32> times = TimesOf(before.clips[c]);
        for (const AnimChannel& channel : before.models[0].animChannels.channels) {
            const AnimChannel* other = nullptr;
            if (byTarget) {
                const auto found = others.find(keyOf(channel.target));
                other = found != others.end() ? found->second : nullptr;
            } else {
                other = after.models[0].animChannels.find(channel.id);
            }
            const std::vector<u8> a = Series(before, c, channel, times);
            std::vector<u8> b;
            if (other != nullptr && other->valueType == channel.valueType) {
                b = Series(after, c, *other, times);
            } else {
                // Gone from the file: the reduced document left it unkeyed, so
                // it must have played its unkeyed rest everywhere.
                const TrackRests rests =
                    RestsPlayed(before, 0, channel, GameOf(before.defaultProfile));
                for (std::size_t i = 0; i < times.size(); ++i) {
                    b.insert(b.end(), rests.unkeyed.begin(), rests.unkeyed.end());
                }
            }
            const std::size_t size = geom::AttrTypeSize(channel.valueType);
            if (a.size() != times.size() * size || b.size() != a.size()) {
                ++out.differing;
                continue;
            }
            const f32 magnitude = SizeOf(channel.valueType, a);
            for (std::size_t i = 0; i < times.size(); ++i) {
                ++out.compared;
                if (!Same(channel.valueType, a.data() + i * size, b.data() + i * size, magnitude)) {
                    if (out.differing++ == 0) {
                        const SubTrack* was = FindSubTrack(before.clips[c], channel.id);
                        const SubTrack* now = other != nullptr ? FindSubTrack(after.clips[c], other->id) : nullptr;
                        out.first = "clip " + before.clips[c].name + " channel " +
                                    std::to_string(channel.id) + " (" + ToString(channel.target.channel) +
                                    ") at " + std::to_string(times[i]) + " ms: " +
                                    Text(channel.valueType, a.data() + i * size) + " vs " +
                                    Text(channel.valueType, b.data() + i * size) + "; keys " +
                                    std::to_string(was ? was->times.size() : 0) + " -> " +
                                    std::to_string(now ? now->times.size() : 0);
                    }
                    break;
                }
            }
        }
    }
    return out;
}

void RequireSameReads(const Document& before, const Document& after) {
    const Reads reads = CompareReads(before, after, false);
    INFO(reads.first);
    CHECK(reads.compared > 0);
    CHECK(reads.differing == 0);
}

Vector3f V(f32 x, f32 y, f32 z) {
    return Vector3f{x, y, z};
}

Quaternion Turn(f32 degrees) {
    const f32 half = degrees * 3.14159265f / 360.0f;
    return Quaternion(0.0f, 0.0f, std::sin(half), std::cos(half));
}

} // namespace

// ============================================================================
// Tier 1: exact
// ============================================================================

TEST_CASE("exact: a key a line reproduces goes, and the ends stay", "[wem][keys]") {
    Fixture f;
    const u32 t = f.channel(0, Channel::Translation, geom::AttrType::F32x3);
    const u32 clip = f.clip(ReadRule::Wc3);
    f.key<Vector3f>(clip, t, {0.0f, 0.25f, 0.5f, 1.0f}, {V(0, 0, 0), V(1, 0, 0), V(2, 0, 0), V(0, 0, 3)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.keysRemoved == 1); // 0.25 lies on the line from 0 to 0.5; 0.5 turns a corner.
    CHECK(f.keys(f.document, clip, t) == 3);
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: Warcraft III's slerp decides, not a textbook one", "[wem][keys]") {
    // 40 degrees apart, Warcraft III nlerps (the raw dot is over 0.9), which
    // does not turn at an even rate: a key on the arc at its own time is one
    // a textbook slerp reproduces and the game does not. A hold goes.
    const Quaternion a = Turn(0.0f);
    const Quaternion c = Turn(40.0f);
    for (const bool hold : {true, false}) {
        Fixture f;
        const u32 r = f.channel(0, Channel::Rotation, geom::AttrType::Quat);
        const u32 clip = f.clip(ReadRule::Wc3);
        if (hold) {
            f.key<Quaternion>(clip, r, {0.0f, 0.25f, 0.5f, 1.0f}, {a, a, a, c});
        } else {
            f.key<Quaternion>(clip, r, {0.0f, 0.25f, 1.0f}, {a, Turn(10.0f), c});
        }
        const Document before = f.document;
        const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
        CHECK(report.keysRemoved == (hold ? 1u : 0u));
        RequireSameReads(before, f.document);
    }
}

TEST_CASE("exact: keys outside a kept window go, and the window's first and last stay",
          "[wem][keys]") {
    Fixture f;
    const u32 t = f.channel(0, Channel::Translation, geom::AttrType::F32x3);
    const u32 clip = f.clip(ReadRule::Wc3);
    f.window(clip, 1000, 2000);
    // Bracket copies at -0.1 s and 1.1 s; inside, 0.2 .. 0.8 on one line. The
    // wrap before 0.2 and after 0.8 reads the first and the last.
    f.key<Vector3f>(clip, t, {-0.1f, 0.2f, 0.5f, 0.8f, 1.1f},
                    {V(9, 9, 9), V(2, 0, 0), V(5, 0, 0), V(8, 0, 0), V(-9, 0, 0)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.keysRemoved == 3);
    const SubTrack* left = f.track(f.document, clip, t);
    REQUIRE(left != nullptr);
    CHECK(left->times == std::vector<f32>{0.2f, 0.8f});
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a channel keyed only outside its windows stays keyed", "[wem][keys]") {
    // Every clip reads nothing of it, so every clip shows the keyed-elsewhere
    // black. Unkeyed, they would show the light's static red: the clip the
    // written track takes its clock from keeps its keys.
    Fixture f;
    const u32 colour = f.channel(2, Channel::Color, geom::AttrType::F32x3);
    const u32 first = f.clip(ReadRule::Wc3);
    const u32 second = f.clip(ReadRule::Wc3);
    f.window(first, 0, 1000);
    f.window(second, 2000, 3000);
    f.key<Vector3f>(first, colour, {1.5f}, {V(0, 1, 0)});
    f.key<Vector3f>(second, colour, {-0.5f}, {V(0, 1, 0)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.subTracksDropped == 1);
    CHECK(f.keys(f.document, first, colour) == 1);
    CHECK(f.track(f.document, second, colour) == nullptr);
    CHECK(KeyedAnywhere(f.document, 0, colour));
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a key that loses its millisecond goes", "[wem][keys]") {
    Fixture f;
    const u32 t = f.channel(0, Channel::Translation, geom::AttrType::F32x3);
    const u32 clip = f.clip(ReadRule::Wc3);
    f.window(clip, 0, 1000);
    f.key<Vector3f>(clip, t, {0.0f, 0.5f, 0.5002f, 1.0f},
                    {V(0, 0, 0), V(7, 0, 0), V(-7, 0, 0), V(0, 0, 1)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.keysRemoved == 1);
    CHECK(f.keys(f.document, clip, t) == 3);
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: StarCraft II's loop is as long as its last key", "[wem][keys]") {
    Fixture f;
    f.document.defaultProfile = ProfileId::Sc2;
    const u32 t = f.channel(0, Channel::Translation, geom::AttrType::F32x3);
    const u32 clip = f.clip(ReadRule::Sc2, 1.0f);
    // Past the clip's end, on one line: the inner keys go, and the last, which
    // sets where the loop wraps, stays.
    f.key<Vector3f>(clip, t, {0.0f, 0.5f, 1.0f, 1.5f}, {V(0, 0, 0), V(1, 0, 0), V(2, 0, 0), V(3, 0, 0)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.keysRemoved == 2);
    const SubTrack* left = f.track(f.document, clip, t);
    REQUIRE(left != nullptr);
    CHECK(left->times == std::vector<f32>{0.0f, 1.5f});
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a WoW clip holds its ends and lerps a turn without a sign fix", "[wem][keys]") {
    Fixture f;
    f.document.defaultProfile = ProfileId::Wow;
    const u32 r = f.channel(0, Channel::Rotation, geom::AttrType::Quat);
    const u32 clip = f.clip(ReadRule::Wow, 1.0f);
    const Quaternion a = Turn(0.0f);
    const Quaternion flipped(-a.x, -a.y, -a.z, -a.w);
    // The hold at 0.3 goes. -a is the same turn as a, but WoW lerps through
    // zero to reach it, so the keys around it are not a hold.
    f.key<Quaternion>(clip, r, {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}, {a, a, a, flipped, a});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.keysRemoved == 1);
    CHECK(f.keys(f.document, clip, r) == 4);
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a sub-track playing the rest goes, the clock owner's stays", "[wem][keys]") {
    Fixture f;
    const u32 t = f.channel(0, Channel::Translation, geom::AttrType::F32x3);
    const u32 first = f.clip(ReadRule::Wc3);
    const u32 second = f.clip(ReadRule::Wc3);
    const u32 third = f.clip(ReadRule::Wc3);
    f.key<Vector3f>(first, t, {0.0f, 0.5f, 1.0f}, {V(0, 0, 0), V(0, 0, 0), V(0, 0, 0)});
    f.key<Vector3f>(second, t, {0.0f, 1.0f}, {V(0, 0, 0), V(0, 0, 0)});
    f.key<Vector3f>(third, t, {0.0f, 1.0f}, {V(0, 0, 0), V(4, 0, 0)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.subTracksDropped == 1);
    CHECK(f.track(f.document, second, t) == nullptr);
    CHECK(f.keys(f.document, first, t) == 1); // Kept, as one key: it names the clock.
    CHECK(f.keys(f.document, third, t) == 2);
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a channel every clip keys at its unkeyed rest goes whole", "[wem][keys]") {
    Fixture f;
    const u32 colour = f.channel(2, Channel::Color, geom::AttrType::F32x3);
    const u32 first = f.clip(ReadRule::Wc3);
    const u32 second = f.clip(ReadRule::Wc3);
    f.key<Vector3f>(first, colour, {0.0f, 1.0f}, {V(1, 0, 0), V(1, 0, 0)});
    f.key<Vector3f>(second, colour, {0.0f, 1.0f}, {V(1, 0, 0), V(1, 0, 0)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.subTracksDropped == 2);
    CHECK_FALSE(KeyedAnywhere(f.document, 0, colour));
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a rest that differs keeps what a clip that never keys it shows",
          "[wem][keys]") {
    // A light's colour is its static red while nothing keys it, and black in a
    // clip that does not key it once another does.
    Fixture f;
    const u32 colour = f.channel(2, Channel::Color, geom::AttrType::F32x3);
    const u32 first = f.clip(ReadRule::Wc3);
    const u32 second = f.clip(ReadRule::Wc3);
    f.clip(ReadRule::Wc3); // Keys nothing: shows black.
    f.key<Vector3f>(first, colour, {0.0f, 1.0f}, {V(1, 0, 0), V(1, 0, 0)});
    f.key<Vector3f>(second, colour, {0.0f, 1.0f}, {V(1, 0, 0), V(1, 0, 0)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.subTracksDropped == 0);
    CHECK(report.tracksCollapsed == 2);
    CHECK(KeyedAnywhere(f.document, 0, colour));
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a transparent layer keeps its sub-track at the rest", "[wem][keys]") {
    Fixture f;
    f.document.defaultProfile = ProfileId::Sc2;
    const u32 t = f.channel(0, Channel::Translation, geom::AttrType::F32x3);
    const u32 clip = f.clip(ReadRule::Sc2, 1.0f, true);
    f.clip(ReadRule::Sc2);
    f.key<Vector3f>(clip, t, {0.0f, 1.0f}, {V(0, 0, 0), V(0, 0, 0)});
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.subTracksDropped == 0);
    CHECK(f.keys(f.document, clip, t) == 1);
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: squirt keys and TCB tracks are never touched", "[wem][keys]") {
    Fixture f;
    Node emitter;
    emitter.name = "squirt";
    emitter.kind = NodeKind::Wc3ParticleEmitter2;
    emitter.resetPayloadForKind();
    std::get<Wc3ParticleEmitter2Payload>(emitter.payload).squirt = true;
    const u32 node = f.model().nodes.add(emitter);
    AnimChannel rate;
    rate.id = f.model().animChannels.nextFreeId();
    rate.target.kind = TrackTarget::Kind::Node;
    rate.target.node = node;
    rate.target.channel = Channel::EmitterProperty;
    rate.target.sub = static_cast<u32>(Wc3Particle2Property::EmissionRate);
    rate.valueType = geom::AttrType::F32;
    const u32 rateId = f.model().animChannels.add(rate);
    const u32 t = f.channel(0, Channel::Translation, geom::AttrType::F32x3);
    const u32 clip = f.clip(ReadRule::Wc3);
    // Every key a burst, however equal.
    f.key<f32>(clip, rateId, {0.0f, 0.3f, 0.6f, 1.0f}, {5.0f, 5.0f, 5.0f, 5.0f}, Interpolation::Step);
    SubTrack& tcb = f.key<Vector3f>(clip, t, {0.0f, 0.5f, 1.0f}, {V(0, 0, 0), V(0, 0, 0), V(0, 0, 0),
                                                                  V(1, 0, 0), V(0, 0, 0), V(0, 0, 0),
                                                                  V(2, 0, 0), V(0, 0, 0), V(0, 0, 0)},
                                    Interpolation::Hermite);
    tcb.tcb.assign(9, 0.0f);
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.keysRemoved == 0);
    CHECK(f.keys(f.document, clip, rateId) == 4);
    CHECK(f.keys(f.document, clip, t) == 3);
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a channel whose clips disagree keeps its inner keys", "[wem][keys]") {
    Fixture f;
    const u32 t = f.channel(0, Channel::Translation, geom::AttrType::F32x3);
    const u32 lined = f.clip(ReadRule::Wc3);
    const u32 stepped = f.clip(ReadRule::Wc3);
    f.key<Vector3f>(lined, t, {0.0f, 0.5f, 1.0f}, {V(0, 0, 0), V(1, 0, 0), V(2, 0, 0)});
    f.key<Vector3f>(stepped, t, {0.0f, 0.5f, 1.0f}, {V(0, 0, 0), V(1, 0, 0), V(2, 0, 0)},
                    Interpolation::Step);
    const Document before = f.document;
    ReduceKeysExactly(f.document, 0);
    // One MDX track has one interpolation: the key the line reproduces in the
    // first clip is the step the second clip takes when the writer merges them.
    CHECK(f.keys(f.document, lined, t) == 3);
    RequireSameReads(before, f.document);
}

TEST_CASE("exact: a step equal to the one before it goes", "[wem][keys]") {
    Fixture f;
    const u32 v = f.channel(0, Channel::Visibility, geom::AttrType::F32);
    const u32 clip = f.clip(ReadRule::Wc3);
    f.key<f32>(clip, v, {0.0f, 0.2f, 0.4f, 0.6f, 1.0f}, {1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
               Interpolation::Step);
    const Document before = f.document;
    const ExactKeyReport report = ReduceKeysExactly(f.document, 0);
    CHECK(report.keysRemoved == 2);
    RequireSameReads(before, f.document);
}

// ============================================================================
// Tier 1 over the corpus, read directly and through the written file
// ============================================================================

namespace {

struct Sweep {
    u32 files = 0;
    u64 keysBefore = 0;
    u64 keysAfter = 0;
    u64 compared = 0;
    u64 comparedWritten = 0;
    u32 failures = 0;
    std::vector<std::string> failing;

    void fail(const std::string& why) {
        ++failures;
        if (failing.size() < 20) {
            failing.push_back(why);
        }
    }

    void report(const char* label) const {
        std::cout << "[keys exact " << label << "] " << files << " file(s); keys " << keysBefore << " -> "
                  << keysAfter << "; " << compared << " reads compared, " << comparedWritten
                  << " through the written file\n";
        for (const std::string& line : failing) {
            std::cout << "  " << line << "\n";
        }
    }
};

/// Tier 1 on @p before, read against it directly and through @p write, a
/// writer and reader round trip.
template <class Write>
void SweepOne(const Document& before, const std::string& label, Sweep& sweep, Write write) {
    Document after = before;
    for (u32 m = 0; m < after.models.size(); ++m) {
        ReduceKeysExactly(after, m);
    }
    ++sweep.files;
    sweep.keysBefore += CountKeys(before, 0);
    sweep.keysAfter += CountKeys(after, 0);
    const Reads direct = CompareReads(before, after, false);
    sweep.compared += direct.compared;
    if (direct.differing != 0) {
        sweep.fail(label + ": " + direct.first);
    }
    const std::optional<Document> writtenBefore = write(before);
    const std::optional<Document> writtenAfter = write(after);
    if (writtenBefore.has_value() != writtenAfter.has_value()) {
        sweep.fail(label + ": writes before or after only");
        return;
    }
    if (!writtenBefore) {
        return;
    }
    const Reads written = CompareReads(*writtenBefore, *writtenAfter, true);
    sweep.comparedWritten += written.compared;
    if (written.differing != 0) {
        sweep.fail(label + " (written): " + written.first);
    }
}

} // namespace

TEST_CASE("exact sweep: every corpus mdx reads the same, and so does its file",
          "[wem][keys][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 80);
    const MdxConverter converter;
    Sweep sweep;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        Result<Document> imported = converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!imported.ok() || imported->models.empty() || imported->clips.empty()) {
            continue;
        }
        SweepOne(*imported, test::pathText(files[i].filename()), sweep,
                 [&](const Document& document) -> std::optional<Document> {
                     const ProfileId profile = document.defaultProfile;
                     Result<mdx::Model> written = converter.toMdx(document, profile, MdxFileVersion(profile));
                     if (!written.ok()) {
                         return std::nullopt;
                     }
                     Result<Document> back = converter.fromMdx(*written);
                     return back.ok() ? std::optional<Document>(std::move(*back)) : std::nullopt;
                 });
    }
    sweep.report("mdx");
    CHECK(sweep.files > 0);
    CHECK(sweep.keysAfter < sweep.keysBefore);
    CHECK(sweep.failures == 0);
}

TEST_CASE("exact sweep: every corpus m3 reads the same, and so does its file",
          "[wem][keys][corpus]") {
    const auto files = test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 60);
    const M3Converter converter;
    Sweep sweep;
    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        Result<Document> imported = converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!imported.ok() || imported->models.empty() || imported->clips.empty()) {
            continue;
        }
        SweepOne(*imported, test::pathText(files[i].filename()), sweep,
                 [&](const Document& document) -> std::optional<Document> {
                     Result<m3::Model> written = converter.toM3(document, document.defaultProfile);
                     if (!written.ok()) {
                         return std::nullopt;
                     }
                     Result<Document> back = converter.fromM3(*written);
                     return back.ok() ? std::optional<Document>(std::move(*back)) : std::nullopt;
                 });
    }
    sweep.report("m3");
    CHECK(sweep.files > 0);
    CHECK(sweep.keysAfter < sweep.keysBefore);
    CHECK(sweep.failures == 0);
}

TEST_CASE("exact sweep: every corpus m2 reads the same, and so does its file",
          "[wem][keys][corpus][.m2slow]") {
    const auto files = test::gather("WEM_M2_CORPUS_DIR", ".m2", {"WoW", "WowM2"});
    if (files.empty()) {
        SKIP("M2 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 20);
    const M2Converter converter;
    Sweep sweep;
    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        utils::OsFileSystem vfs(test::pathText(files[i].parent_path()));
        m2::Parser parser;
        const m2::Model model = parser.parse(vfs, test::pathText(files[i]));
        if (model.skinProfiles.empty()) {
            continue;
        }
        Result<Document> imported = converter.fromM2(model);
        if (!imported.ok() || imported->models.empty() || imported->clips.empty()) {
            continue;
        }
        SweepOne(*imported, test::pathText(files[i].filename()), sweep,
                 [&](const Document& document) -> std::optional<Document> {
                     Result<m2::Model> written = converter.toM2(document, document.defaultProfile);
                     if (!written.ok()) {
                         return std::nullopt;
                     }
                     Result<Document> back = converter.fromM2(*written);
                     return back.ok() ? std::optional<Document>(std::move(*back)) : std::nullopt;
                 });
    }
    sweep.report("m2");
    CHECK(sweep.files > 0);
    CHECK(sweep.failures == 0);
}


// ============================================================================
// Tier 2: within a tolerance
// ============================================================================

namespace {

/// A hip and a finger: a long bone whose vertices reach 100 units from its
/// pivot, and a short one at its tip whose vertices reach 2. Height 102.
struct Arm {
    Document document;
    u32 hip = 0;
    u32 finger = 0;

    Arm() {
        document.models.emplace_back();
        Model& model = document.models.back();
        model.nodes.rig = RigConvention::PivotRelative;
        Node a;
        a.name = "hip";
        a.kind = NodeKind::Bone;
        hip = model.nodes.add(a);
        Node b;
        b.name = "finger";
        b.kind = NodeKind::Bone;
        b.parent = hip;
        b.pivot = Vector3f{0, 0, 100};
        finger = model.nodes.add(b);

        const std::vector<Vector3f> points = {
            {-5, 0, 0}, {5, 0, 0}, {-5, 0, 100}, {5, 0, 98}, {-1, 0, 100}, {1, 0, 100}, {0, 0, 102}};
        const std::vector<u32> bones = {hip, hip, hip, hip, finger, finger, finger};
        geom::FaceSet faces;
        faces.vertexCount = static_cast<u32>(points.size());
        faces.addTriangle(0, 1, 2);
        faces.addTriangle(1, 3, 2);
        faces.addTriangle(4, 5, 6);
        Mesh mesh;
        mesh.setFaceSet(faces);
        const std::span<Vector3f> written = mesh.attributes.getOrCreate<Vector3f>(
            geom::names::kPosition, geom::Domain::Vertex, geom::AttrType::F32x3);
        for (std::size_t i = 0; i < points.size(); ++i) {
            written[i] = points[i];
        }
        mesh.attributes.getOrCreate<u32>(geom::names::kSection, geom::Domain::Face, geom::AttrType::U32);
        mesh.sections.emplace_back();
        mesh.skin.offsets.push_back(0);
        for (const u32 bone : bones) {
            mesh.skin.influences.push_back(geom::Influence{bone, 1.0f});
            mesh.skin.offsets.push_back(static_cast<u32>(mesh.skin.influences.size()));
        }
        model.meshes.push_back(std::move(mesh));
    }

    u32 channel(u32 node, Channel what, geom::AttrType type) {
        AnimChannel entry;
        entry.id = document.models[0].animChannels.nextFreeId();
        entry.target.kind = TrackTarget::Kind::Node;
        entry.target.node = node;
        entry.target.channel = what;
        entry.valueType = type;
        return document.models[0].animChannels.add(entry);
    }

    u32 clip(f32 duration = 1.0f) {
        Clip made;
        made.name = "clip" + std::to_string(document.clips.size());
        made.model = 0;
        made.duration = duration;
        made.readRule = ReadRule::Wc3;
        made.containers.emplace_back();
        document.clips.push_back(made);
        return static_cast<u32>(document.clips.size() - 1);
    }

    template <class T>
    void key(u32 clip, u32 channel, std::vector<f32> times, std::vector<T> values,
             Interpolation interp = Interpolation::Linear) {
        SubTrack track;
        track.channel = channel;
        track.interp = interp;
        track.times = std::move(times);
        for (const T& value : values) {
            const std::vector<u8> bytes = Bytes(value);
            track.values.insert(track.values.end(), bytes.begin(), bytes.end());
        }
        document.clips[clip].containers[0].subTracks.push_back(std::move(track));
    }

    std::size_t keys(u32 clip, u32 channel) const {
        const SubTrack* found = FindSubTrack(document.clips[clip], channel);
        return found != nullptr ? found->times.size() : 0;
    }
};

Quaternion About(f32 degrees, const Vector3f& axis) {
    const f32 half = degrees * 3.14159265f / 360.0f;
    return Quaternion(axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half),
                      std::cos(half));
}

/// A turn that wobbles: 40 keys over a second, a sweep of 30 degrees plus a
/// ripple of @p ripple degrees.
std::vector<Quaternion> Wobble(f32 ripple, std::vector<f32>& times) {
    std::vector<Quaternion> out;
    times.clear();
    for (int i = 0; i <= 40; ++i) {
        const f32 t = static_cast<f32>(i) / 40.0f;
        times.push_back(t);
        out.push_back(About(30.0f * t + ripple * std::sin(t * 37.0f), Vector3f{1, 0, 0}));
    }
    return out;
}

} // namespace

TEST_CASE("tolerance: a finger may drift where a hip may not", "[wem][keys]") {
    Arm arm;
    const u32 hipTurn = arm.channel(arm.hip, Channel::Rotation, geom::AttrType::Quat);
    const u32 fingerTurn = arm.channel(arm.finger, Channel::Rotation, geom::AttrType::Quat);
    const u32 clip = arm.clip();
    std::vector<f32> times;
    // The same ripple on both, a degree: at the hip's reach of 100 a key cut
    // from it moves a vertex about a unit, ten times ε; at the finger's reach
    // of 2, a fiftieth of that.
    const std::vector<Quaternion> turn = Wobble(1.0f, times);
    arm.key<Quaternion>(clip, hipTurn, times, turn);
    arm.key<Quaternion>(clip, fingerTurn, times, turn);
    const Document before = arm.document;
    const KeyReduceReport report = ReduceKeys(arm.document, 0);
    CHECK(report.tolerance > 0.09f);
    CHECK(report.tolerance < 0.11f);
    CHECK(arm.keys(clip, fingerTurn) < 10);
    CHECK(arm.keys(clip, hipTurn) > 30);
    CHECK(report.maxError <= report.tolerance * 1.001f);
    CHECK(report.repaired.empty()); // The bound alone held.
    const KeyReduceReport measured = MeasureKeyError(before, arm.document, 0);
    CHECK(measured.maxError <= report.tolerance * 1.001f);
    CHECK(measured.keysAfter < measured.keysBefore);
}

TEST_CASE("tolerance: an ancestor's error is spent before its descendants'", "[wem][keys]") {
    // A hip that may just drift within ε leaves the finger none of it: the
    // finger's vertices ride the hip's error, and the check sees them.
    Arm arm;
    const u32 hipTurn = arm.channel(arm.hip, Channel::Rotation, geom::AttrType::Quat);
    const u32 fingerTurn = arm.channel(arm.finger, Channel::Rotation, geom::AttrType::Quat);
    const u32 clip = arm.clip();
    std::vector<f32> times;
    arm.key<Quaternion>(clip, hipTurn, times, Wobble(0.03f, times));
    arm.key<Quaternion>(clip, fingerTurn, times, Wobble(0.3f, times));
    const Document before = arm.document;
    const KeyReduceReport report = ReduceKeys(arm.document, 0);
    const KeyReduceReport measured = MeasureKeyError(before, arm.document, 0);
    INFO("hip " << arm.keys(clip, hipTurn) << " finger " << arm.keys(clip, fingerTurn));
    CHECK(report.repaired.empty());
    CHECK(measured.maxError <= report.tolerance * 1.001f);
    CHECK(measured.keysAfter < measured.keysBefore);
}

TEST_CASE("tolerance: the search finds no more keys than it needs", "[wem][keys]") {
    for (const auto search : {KeyReduceOptions::Search::Balanced, KeyReduceOptions::Search::Maximum}) {
        Arm arm;
        const u32 fingerTurn = arm.channel(arm.finger, Channel::Rotation, geom::AttrType::Quat);
        const u32 clip = arm.clip();
        std::vector<f32> times;
        arm.key<Quaternion>(clip, fingerTurn, times, Wobble(0.3f, times));
        const Document before = arm.document;
        KeyReduceOptions options;
        options.search = search;
        const KeyReduceReport report = ReduceKeys(arm.document, 0, options);
        CHECK(MeasureKeyError(before, arm.document, 0).maxError <= report.tolerance * 1.001f);
        CHECK(arm.keys(clip, fingerTurn) < 20);
    }
    // Maximum keeps no more than Balanced on the same track.
    std::size_t kept[2] = {0, 0};
    int slot = 0;
    for (const auto search : {KeyReduceOptions::Search::Balanced, KeyReduceOptions::Search::Maximum}) {
        Arm arm;
        const u32 fingerTurn = arm.channel(arm.finger, Channel::Rotation, geom::AttrType::Quat);
        const u32 clip = arm.clip();
        std::vector<f32> times;
        arm.key<Quaternion>(clip, fingerTurn, times, Wobble(2.0f, times));
        KeyReduceOptions options;
        options.search = search;
        options.tolerance = 0.02f;
        ReduceKeys(arm.document, 0, options);
        kept[slot++] = arm.keys(clip, fingerTurn);
    }
    CHECK(kept[1] <= kept[0]);
}

TEST_CASE("tolerance: channels that move nothing keep their companions", "[wem][keys]") {
    Arm arm;
    AnimChannel alpha;
    alpha.id = arm.document.models[0].animChannels.nextFreeId();
    alpha.target.kind = TrackTarget::Kind::Node;
    alpha.target.node = arm.hip;
    alpha.target.channel = Channel::Alpha;
    alpha.valueType = geom::AttrType::F32;
    const u32 id = arm.document.models[0].animChannels.add(alpha);
    const u32 clip = arm.clip();
    // A ramp with a ripple under half an 8-bit step, then an exact zero that a
    // line from its neighbours would miss by less than that.
    arm.key<f32>(clip, id, {0.0f, 0.2f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 1.0f},
                 {0.0f, 0.201f, 0.4f, 0.001f, 0.001f, 0.0f, 0.001f, 0.001f});
    ReduceKeys(arm.document, 0);
    const SubTrack* track = FindSubTrack(arm.document.clips[clip], id);
    REQUIRE(track != nullptr);
    // The ripple goes; the zero at 0.7 stays.
    CHECK(std::find(track->times.begin(), track->times.end(), 0.2f) == track->times.end());
    CHECK(std::find(track->times.begin(), track->times.end(), 0.7f) != track->times.end());
}

TEST_CASE("tolerance: a Hermite track loses a key once its tangents are refit", "[wem][keys]") {
    // One cubic, keyed at its ends and its middle with the tangents each half
    // span has. Without the middle, the end tangents as they stand describe a
    // curve twice as flat; refit, they describe the cubic.
    const auto cubic = [](f32 t) { return Vector3f{0.0f, 0.0f, 40.0f * t * t * t - 60.0f * t * t + 30.0f * t}; };
    const auto slope = [](f32 t) { return Vector3f{0.0f, 0.0f, 120.0f * t * t - 120.0f * t + 30.0f}; };
    for (const bool refit : {true, false}) {
        Arm arm;
        const u32 move = arm.channel(arm.finger, Channel::Translation, geom::AttrType::F32x3);
        const u32 clip = arm.clip();
        // Warcraft III's Hermite reads a tangent per span, so each is the slope
        // times the half span it faces.
        std::vector<Vector3f> values;
        for (const f32 t : {0.0f, 0.5f, 1.0f}) {
            values.push_back(cubic(t));
            values.push_back(slope(t) * 0.5f);
            values.push_back(slope(t) * 0.5f);
        }
        arm.key<Vector3f>(clip, move, {0.0f, 0.5f, 1.0f}, values, Interpolation::Hermite);
        const Document before = arm.document;
        KeyReduceOptions options;
        options.refitTangents = refit;
        const KeyReduceReport report = ReduceKeys(arm.document, 0, options);
        CHECK(arm.keys(clip, move) == (refit ? 2u : 3u));
        CHECK(MeasureKeyError(before, arm.document, 0).maxError <= report.tolerance * 1.001f);
    }
}

TEST_CASE("tolerance: aligned poses keep one set of times", "[wem][keys]") {
    Arm arm;
    const u32 hipTurn = arm.channel(arm.hip, Channel::Rotation, geom::AttrType::Quat);
    const u32 fingerTurn = arm.channel(arm.finger, Channel::Rotation, geom::AttrType::Quat);
    const u32 clip = arm.clip();
    std::vector<f32> times;
    arm.key<Quaternion>(clip, hipTurn, times, Wobble(0.02f, times));
    arm.key<Quaternion>(clip, fingerTurn, times, Wobble(0.4f, times));
    const Document before = arm.document;
    KeyReduceOptions options;
    options.alignPoses = true;
    const KeyReduceReport report = ReduceKeys(arm.document, 0, options);
    CHECK(MeasureKeyError(before, arm.document, 0).maxError <= report.tolerance * 1.001f);
    const SubTrack* hip = FindSubTrack(arm.document.clips[clip], hipTurn);
    const SubTrack* finger = FindSubTrack(arm.document.clips[clip], fingerTurn);
    REQUIRE(hip != nullptr);
    REQUIRE(finger != nullptr);
    CHECK(hip->times.size() < times.size());
    // The finger keeps no time the hip dropped in the pose pass... and every
    // time either keeps is one of the original times.
    for (const f32 t : finger->times) {
        CHECK(std::find(times.begin(), times.end(), t) != times.end());
    }
}

namespace {

/// Tier 2 over corpus documents, each checked against the skinned mesh.
struct ToleranceSweep {
    u32 swept = 0;
    u64 before = 0;
    u64 after = 0;
    u32 repaired = 0;
    u32 kept = 0;
    f32 worstShare = 0.0f;
    std::vector<std::string> failing;

    void add(const Document& imported, const std::string& label) {
        Document reduced = imported;
        const KeyReduceReport report = ReduceKeys(reduced, 0);
        const KeyReduceReport measured = MeasureKeyError(imported, reduced, 0);
        ++swept;
        before += measured.keysBefore;
        after += measured.keysAfter;
        repaired += static_cast<u32>(report.repaired.size());
        kept += static_cast<u32>(report.kept.size());
        for (const u32 c : report.kept) {
            std::cout << "  kept: " << label << " clip " << imported.clips[c].name << " ("
                      << imported.clips[c].containers.size() << " layer(s))\n";
        }
        worstShare = std::max(worstShare, measured.maxError / report.tolerance);
        if (measured.maxError > report.tolerance * 1.001f + 1e-6f) {
            failing.push_back(label + ": " + std::to_string(measured.maxError) + " over " +
                              std::to_string(report.tolerance));
        }
    }

    void finish(const char* format) const {
        std::cout << "[keys tolerance " << format << "] " << swept << " file(s); keys " << before << " -> "
                  << after << "; worst " << worstShare << " of ε; " << repaired << " clip(s) repaired, "
                  << kept << " kept\n";
        for (const std::string& line : failing) {
            std::cout << "  " << line << "\n";
        }
        CHECK(swept > 0);
        CHECK(after < before);
        CHECK(failing.empty());
    }
};

} // namespace

TEST_CASE("tolerance sweep: every corpus mdx stays within ε, by its mesh",
          "[wem][keys][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 40);
    const MdxConverter converter;
    ToleranceSweep sweep;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        Result<Document> imported = converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (imported.ok() && !imported->models.empty() && !imported->clips.empty()) {
            sweep.add(*imported, test::pathText(files[i].filename()));
        }
    }
    sweep.finish("mdx");
}

TEST_CASE("tolerance sweep: every corpus m3 stays within ε, by its mesh",
          "[wem][keys][corpus]") {
    const auto files = test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        SKIP("M3 corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 30);
    const M3Converter converter;
    ToleranceSweep sweep;
    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        Result<Document> imported = converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (imported.ok() && !imported->models.empty() && !imported->clips.empty()) {
            sweep.add(*imported, test::pathText(files[i].filename()));
        }
    }
    sweep.finish("m3");
}

TEST_CASE("tolerance sweep: every corpus m2 stays within ε, by its mesh",
          "[wem][keys][corpus][.m2slow]") {
    const auto files = test::gather("WEM_M2_CORPUS_DIR", ".m2", {"WoW", "WowM2"});
    if (files.empty()) {
        SKIP("M2 corpus not found");
    }
    // Player characters: hundreds of clips each, dense keys.
    const std::size_t limit = test::sweepLimit(files.size(), 4);
    const M2Converter converter;
    ToleranceSweep sweep;
    for (std::size_t i = 0; i < limit; ++i) {
        test::trace(files[i]);
        utils::OsFileSystem vfs(test::pathText(files[i].parent_path()));
        m2::Parser parser;
        const m2::Model model = parser.parse(vfs, test::pathText(files[i]));
        if (model.skinProfiles.empty()) {
            continue;
        }
        Result<Document> imported = converter.fromM2(model);
        if (imported.ok() && !imported->models.empty() && !imported->clips.empty()) {
            sweep.add(*imported, test::pathText(files[i].filename()));
        }
    }
    sweep.finish("m2");
}

TEST_CASE("tolerance: a node answers for what its descendants carry", "[wem][keys]") {
    // A pelvis whose own vertices sit within 2 units of it, and a spine on it
    // reaching 100 units up: a degree lost at the pelvis moves the spine's tip
    // almost two units, twenty times ε, though the pelvis's own vertices barely
    // move. Only the check over the pelvis's descendants sees it.
    Document document;
    document.models.emplace_back();
    Model& model = document.models.back();
    Node pelvis;
    pelvis.name = "pelvis";
    pelvis.kind = NodeKind::Bone;
    const u32 root = model.nodes.add(pelvis);
    Node spine;
    spine.name = "spine";
    spine.kind = NodeKind::Bone;
    spine.parent = root;
    spine.pivot = Vector3f{0, 0, 2};
    const u32 child = model.nodes.add(spine);
    const std::vector<Vector3f> points = {{-2, 0, 0}, {2, 0, 0}, {0, 0, 2}, {-3, 0, 100}, {3, 0, 100}, {0, 0, 102}};
    const std::vector<u32> bones = {root, root, root, child, child, child};
    geom::FaceSet faces;
    faces.vertexCount = static_cast<u32>(points.size());
    faces.addTriangle(0, 1, 2);
    faces.addTriangle(3, 4, 5);
    Mesh mesh;
    mesh.setFaceSet(faces);
    const std::span<Vector3f> written = mesh.attributes.getOrCreate<Vector3f>(
        geom::names::kPosition, geom::Domain::Vertex, geom::AttrType::F32x3);
    for (std::size_t i = 0; i < points.size(); ++i) {
        written[i] = points[i];
    }
    mesh.attributes.getOrCreate<u32>(geom::names::kSection, geom::Domain::Face, geom::AttrType::U32);
    mesh.sections.emplace_back();
    mesh.skin.offsets.push_back(0);
    for (const u32 bone : bones) {
        mesh.skin.influences.push_back(geom::Influence{bone, 1.0f});
        mesh.skin.offsets.push_back(static_cast<u32>(mesh.skin.influences.size()));
    }
    model.meshes.push_back(std::move(mesh));
    AnimChannel turn;
    turn.id = model.animChannels.nextFreeId();
    turn.target.kind = TrackTarget::Kind::Node;
    turn.target.node = root;
    turn.target.channel = Channel::Rotation;
    turn.valueType = geom::AttrType::Quat;
    const u32 id = model.animChannels.add(turn);
    Clip clip;
    clip.name = "walk";
    clip.model = 0;
    clip.duration = 1.0f;
    clip.readRule = ReadRule::Wc3;
    clip.containers.emplace_back();
    std::vector<f32> times;
    const std::vector<Quaternion> values = Wobble(1.0f, times);
    SubTrack track;
    track.channel = id;
    track.times = times;
    for (const Quaternion& q : values) {
        const std::vector<u8> bytes = Bytes(q);
        track.values.insert(track.values.end(), bytes.begin(), bytes.end());
    }
    clip.containers[0].subTracks.push_back(std::move(track));
    document.clips.push_back(std::move(clip));

    const Document before = document;
    const KeyReduceReport report = ReduceKeys(document, 0);
    CHECK(report.repaired.empty());
    CHECK(report.kept.empty());
    CHECK(FindSubTrack(document.clips[0], id)->times.size() > 30);
    CHECK(MeasureKeyError(before, document, 0).maxError <= report.tolerance * 1.001f);
}

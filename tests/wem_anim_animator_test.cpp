// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The `Animator` (WEM_ANIMATION_RUNTIME_DESIGN.md §3): each track read by its
/// clip's rule, and layers and plays combined by StarCraft II's budget walk.
/// G-A2 is the last two groups: track-set layers leave a lone play as it was,
/// and `Validate` enforces the priority that makes that hold under Warcraft III.
/// The blend against `M3ModelAdapter` itself is G-A, in Flakes.

#include <cmath>
#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>
#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/validate.h>

#include "wem_corpus_files.h"

using namespace whiteout;
using namespace whiteout::models;
using namespace whiteout::models::wem;
using Catch::Approx;

namespace {

template <class T>
std::vector<u8> Bytes(const T& value) {
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

template <class T>
T Load(const std::vector<u8>& bytes) {
    T value{};
    REQUIRE(bytes.size() == sizeof(T));
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

/// One pivot-rig bone with a translation and a rotation channel, plus a u32
/// channel (a texture index on nothing), and no clips yet.
struct Fixture {
    Document document;
    u32 translation = 0;
    u32 rotation = 0;
    u32 texture = 0;

    Fixture() {
        document.models.emplace_back();
        Model& model = document.models.back();
        Node bone;
        bone.name = "bone";
        bone.kind = NodeKind::Bone;
        model.nodes.add(bone);
        const auto declare = [&](Channel channel, geom::AttrType type) {
            AnimChannel entry;
            entry.id = model.animChannels.nextFreeId();
            entry.target.kind = TrackTarget::Kind::Node;
            entry.target.node = 0;
            entry.target.channel = channel;
            entry.valueType = type;
            return model.animChannels.add(entry);
        };
        translation = declare(Channel::Translation, geom::AttrType::F32x3);
        rotation = declare(Channel::Rotation, geom::AttrType::Quat);
        texture = declare(Channel::TextureIndex, geom::AttrType::U32);
    }

    /// A clip of one container, which the caller keys.
    u32 clip(ReadRule rule, i32 priority = 0, bool transparent = false, f32 duration = 1.0f) {
        Clip made;
        made.name = "clip" + std::to_string(document.clips.size());
        made.model = 0;
        made.duration = duration;
        made.looping = true;
        made.readRule = rule;
        SubTrackContainer container;
        container.priority = priority;
        container.concurrent = transparent;
        made.containers.push_back(container);
        document.clips.push_back(made);
        return static_cast<u32>(document.clips.size() - 1);
    }

    template <class T>
    void key(u32 clip, u32 channel, std::vector<f32> times, std::vector<T> values,
             Interpolation interp = Interpolation::Linear, std::size_t container = 0) {
        SubTrack track;
        track.channel = channel;
        track.interp = interp;
        track.times = std::move(times);
        for (const T& value : values) {
            const std::vector<u8> bytes = Bytes(value);
            track.values.insert(track.values.end(), bytes.begin(), bytes.end());
        }
        document.clips[clip].containers[container].subTracks.push_back(std::move(track));
    }

    std::size_t indexOf(u32 channel) const {
        return document.models[0].animChannels.indexOf(channel);
    }

    Pose pose(std::vector<Play> plays, bool globals = true, f32 world = 0) const {
        Mix mix;
        mix.plays = std::move(plays);
        mix.globals = globals;
        mix.worldSeconds = world;
        Pose out;
        Animator(document, 0).evaluate(mix, out);
        return out;
    }

    Vector3f t(const Pose& pose) const {
        return Load<Vector3f>(pose.channelValues[indexOf(translation)]);
    }
};

Quaternion AboutZ(f32 degrees) {
    const f32 half = degrees * 3.14159265f / 360.0f;
    return Quaternion{0, 0, std::sin(half), std::cos(half)};
}

f32 Length(const Quaternion& q) {
    return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

} // namespace

// ---- Each track by its clip's rule ---------------------------------------------

TEST_CASE("wem animator reads a track by its clip's rule", "[wem][anim][animator]") {
    Fixture f;
    const u32 wc3 = f.clip(ReadRule::Wc3);
    const u32 sc2 = f.clip(ReadRule::Sc2);
    for (const u32 c : {wc3, sc2}) {
        f.key<Vector3f>(c, f.translation, {0.0f, 1.0f}, {Vector3f{0, 0, 0}, Vector3f{10, 0, 0}});
        f.key<Quaternion>(c, f.rotation, {0.0f, 1.0f}, {AboutZ(0), AboutZ(120)});
    }

    const Pose w = f.pose({Play{wc3, 0.5f}});
    const Pose s = f.pose({Play{sc2, 0.5f}});
    CHECK(f.t(w).x == Approx(5.0f));
    CHECK(f.t(s).x == Approx(5.0f));
    // Warcraft III slerps; StarCraft II lerps the raw components, which leaves
    // a wide span short of unit length.
    CHECK(Length(w.local[0].rotation) == Approx(1.0f));
    CHECK(Length(s.local[0].rotation) < 0.9f);
}

TEST_CASE("wem animator wraps an Sc2 track at its own last key and holds a Wc3 one",
          "[wem][anim][animator]") {
    Fixture f;
    const u32 wc3 = f.clip(ReadRule::Wc3, 0, false, 2.0f);
    const u32 sc2 = f.clip(ReadRule::Sc2, 0, false, 2.0f);
    for (const u32 c : {wc3, sc2}) {
        f.key<Vector3f>(c, f.translation, {0.0f, 1.0f}, {Vector3f{0, 0, 0}, Vector3f{10, 0, 0}});
    }
    // At 1.5 s the M3 track has wrapped to its own 0.5 s; `toMdx` keys the
    // windowless clip's end with the value it holds.
    CHECK(f.t(f.pose({Play{sc2, 1.5f}})).x == Approx(5.0f));
    CHECK(f.t(f.pose({Play{wc3, 1.5f}})).x == Approx(10.0f));
}

// ---- The budget walk ---------------------------------------------------------------

TEST_CASE("wem animator spends the budget and smoothsteps two plays", "[wem][anim][animator]") {
    Fixture f;
    const u32 a = f.clip(ReadRule::Sc2);
    const u32 b = f.clip(ReadRule::Sc2);
    f.key<Vector3f>(a, f.translation, {0.0f}, {Vector3f{10, 0, 0}});
    f.key<Vector3f>(b, f.translation, {0.0f}, {Vector3f{0, 0, 0}});

    // Equal halves: the newer play is combined last, at s(0.5 / 1.0) = 0.5.
    CHECK(f.t(f.pose({Play{a, 0, 0.5f}, Play{b, 0, 0.5f}})).x == Approx(5.0f));
    // 0.25 over 0.75: t = 0.25, s(t) = t^2 (3 - 2t) = 0.15625.
    CHECK(f.t(f.pose({Play{a, 0, 0.25f}, Play{b, 0, 0.75f}})).x == Approx(1.5625f));
    // A lone play at 0.3 is at full strength: weights are relative.
    CHECK(f.t(f.pose({Play{a, 0, 0.3f}})).x == Approx(10.0f));
}

TEST_CASE("wem animator lets a transparent layer abstain and an opaque one fill with rest",
          "[wem][anim][animator]") {
    Fixture f;
    const u32 below = f.clip(ReadRule::Sc2, 5);
    f.key<Vector3f>(below, f.translation, {0.0f}, {Vector3f{7, 0, 0}});

    const u32 transparent = f.clip(ReadRule::Sc2, 10, true);
    f.key<Quaternion>(transparent, f.rotation, {0.0f}, {AboutZ(90)});
    CHECK(f.t(f.pose({Play{transparent, 0}, Play{below, 0}})).x == Approx(7.0f));

    const u32 opaque = f.clip(ReadRule::Sc2, 10, false);
    f.key<Quaternion>(opaque, f.rotation, {0.0f}, {AboutZ(90)});
    CHECK(f.t(f.pose({Play{opaque, 0}, Play{below, 0}})).x == Approx(0.0f));

    // An empty track is not "no track": it falls to rest even when transparent.
    const u32 empty = f.clip(ReadRule::Sc2, 10, true);
    f.key<Vector3f>(empty, f.translation, {}, {});
    CHECK(f.t(f.pose({Play{empty, 0}, Play{below, 0}})).x == Approx(0.0f));
}

TEST_CASE("wem animator marks a play once and keeps its first 64", "[wem][anim][animator]") {
    Fixture f;
    // One play's two layers: the higher contributes, the lower is skipped.
    const u32 split = f.clip(ReadRule::Sc2, 10);
    f.document.clips[split].containers.push_back(SubTrackContainer{});
    f.document.clips[split].containers[1].priority = 20;
    f.key<Vector3f>(split, f.translation, {0.0f}, {Vector3f{1, 0, 0}}, Interpolation::Linear, 0);
    f.key<Vector3f>(split, f.translation, {0.0f}, {Vector3f{2, 0, 0}}, Interpolation::Linear, 1);
    CHECK(f.t(f.pose({Play{split, 0, 0.5f}})).x == Approx(2.0f));

    // 64 transparent plays keying nothing, and a 65th keying the channel: the
    // 65th is dropped, so the channel rests. StarCraft II's cap, and only its.
    Fixture g;
    g.document.defaultProfile = ProfileId::Sc2;
    std::vector<Play> plays;
    for (int p = 0; p < 64; ++p) {
        plays.push_back(Play{g.clip(ReadRule::Sc2, 1, true), 0, 0.01f});
    }
    const u32 last = g.clip(ReadRule::Sc2, 0);
    g.key<Vector3f>(last, g.translation, {0.0f}, {Vector3f{3, 0, 0}});
    plays.push_back(Play{last, 0});
    CHECK(g.t(g.pose(plays)).x == Approx(0.0f));
    plays.erase(plays.begin());
    CHECK(g.t(g.pose(plays)).x == Approx(3.0f));
    // Warcraft III runs every global loop there is: nothing is dropped.
    plays.insert(plays.begin(), Play{0, 0, 0.01f});
    g.document.defaultProfile = ProfileId::Wc3Reforged;
    CHECK(g.t(g.pose(plays)).x == Approx(3.0f));
}

TEST_CASE("wem animator keeps 16 contributions and spends past them", "[wem][anim][animator]") {
    const auto run = [](f32 seventeenth) {
        Fixture f;
        std::vector<Play> plays;
        for (int p = 0; p < 17; ++p) {
            const u32 c = f.clip(ReadRule::Sc2);
            f.key<Vector3f>(c, f.translation, {0.0f},
                            {Vector3f{p == 16 ? seventeenth : static_cast<f32>(p), 0, 0}});
            plays.push_back(Play{c, 0, 0.01f});
        }
        return f.t(f.pose(plays)).x;
    };
    CHECK(run(100.0f) == Approx(run(-100.0f)));
}

TEST_CASE("wem animator gives a discrete channel to its first contributor",
          "[wem][anim][animator]") {
    Fixture f;
    const u32 a = f.clip(ReadRule::Sc2);
    const u32 b = f.clip(ReadRule::Sc2);
    f.key<u32>(a, f.texture, {0.0f}, {4u});
    f.key<u32>(b, f.texture, {0.0f}, {9u});
    const Pose pose = f.pose({Play{a, 0, 0.1f}, Play{b, 0, 0.9f}});
    CHECK(Load<u32>(pose.channelValues[f.indexOf(f.texture)]) == 4u);

    // A visibility is discrete whatever its value type: the M3 import keys it
    // as a float.
    AnimChannel visibility;
    visibility.id = f.document.models[0].animChannels.nextFreeId();
    visibility.target.kind = TrackTarget::Kind::Node;
    visibility.target.node = 0;
    visibility.target.channel = Channel::Visibility;
    visibility.valueType = geom::AttrType::F32;
    const u32 vis = f.document.models[0].animChannels.add(visibility);
    f.key<f32>(a, vis, {0.0f}, {0.0f});
    f.key<f32>(b, vis, {0.0f}, {1.0f});
    const Pose shown = f.pose({Play{a, 0, 0.5f}, Play{b, 0, 0.5f}});
    CHECK(Load<f32>(shown.channelValues[f.indexOf(vis)]) == 0.0f);
}

TEST_CASE("wem animator puts a concurrent global ahead of a host play of its priority",
          "[wem][anim][animator]") {
    Fixture f;
    const u32 host = f.clip(ReadRule::Sc2);
    f.key<Vector3f>(host, f.translation, {0.0f}, {Vector3f{1, 0, 0}});
    const u32 global = f.clip(ReadRule::Sc2, 0, true);
    f.document.clips[global].flags = ClipFlags::AutoPlay | ClipFlags::WorldClocked;
    f.key<Vector3f>(global, f.translation, {0.0f}, {Vector3f{2, 0, 0}});
    CHECK(f.t(f.pose({Play{host, 0}})).x == Approx(2.0f));
    CHECK(f.t(f.pose({Play{host, 0}}, false)).x == Approx(1.0f));

    // A global that is not concurrent is the model's own animation: behind.
    f.document.clips[global].containers[0].concurrent = false;
    CHECK(f.t(f.pose({Play{host, 0}})).x == Approx(1.0f));
}

// ---- G-A2: layering is invisible to a lone play -----------------------------------

namespace {

/// Every channel value of @p document's clip @p clip, played alone at @p t.
std::vector<std::vector<u8>> Alone(const Document& document, u32 model, u32 clip, f32 t) {
    Mix mix;
    mix.plays.push_back(Play{clip, t});
    mix.globals = false;
    Pose pose;
    Animator(document, model).evaluate(mix, pose);
    return pose.channelValues;
}

/// A set of every other channel of @p model, used by every clip above all its
/// containers.
void SplitEveryOther(Document& document, u32 model) {
    Model& owner = document.models[model];
    TrackSet set;
    set.name = "Half";
    for (std::size_t c = 0; c < owner.animChannels.channels.size(); c += 2) {
        set.channels.push_back(owner.animChannels.channels[c].id);
    }
    owner.trackSets.push_back(set);
    for (Clip& clip : document.clips) {
        if (clip.model != model || clip.containers.empty()) {
            continue;
        }
        clip.trackSets.push_back(ClipTrackSet{static_cast<u32>(owner.trackSets.size() - 1),
                                              TrackSetFloor(clip, owner.trackSets.back()) + 1});
    }
}

} // namespace

TEST_CASE("wem track sets above their clip leave a lone play unchanged",
          "[wem][anim][animator][tracksets]") {
    Fixture f;
    const u32 c = f.clip(ReadRule::Wc3, 2);
    f.key<Vector3f>(c, f.translation, {0.0f, 1.0f}, {Vector3f{0, 0, 0}, Vector3f{4, 2, 1}});
    f.key<Quaternion>(c, f.rotation, {0.0f, 1.0f}, {AboutZ(10), AboutZ(80)});
    f.key<u32>(c, f.texture, {0.0f, 0.5f}, {1u, 2u});
    const auto before = Alone(f.document, 0, c, 0.4f);
    SplitEveryOther(f.document, 0);
    const bool same = Alone(f.document, 0, c, 0.4f) == before;
    CHECK(same);

    // Below the default the set's tracks snap to rest: why Warcraft III forbids it.
    f.document.clips[c].trackSets[0].priority = 1;
    const bool snapped = Alone(f.document, 0, c, 0.4f) != before;
    CHECK(snapped);
}

TEST_CASE("wem track sets above their clip leave corpus .m3 clips unchanged",
          "[wem][anim][animator][tracksets][corpus]") {
    const auto files =
        test::gather("WEM_M3_CORPUS_DIR", ".m3", {"Sc2M3", "Sc2BetaM3", "HotSM3", "StarM3"});
    if (files.empty()) {
        WARN("no .m3 corpus found; set WEM_M3_CORPUS_DIR");
        return;
    }
    const std::size_t limit = test::sweepLimit(files.size(), 20);
    const M3Converter converter;
    u32 compared = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        const std::vector<u8> bytes = test::readCorpusFile(files[i]);
        if (bytes.empty()) {
            continue;
        }
        m3::Parser parser;
        const m3::Model source = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
        if (source.sequences.empty()) {
            continue;
        }
        Result<Document> converted = converter.fromM3(source, ProfileId::Sc2);
        if (!converted.ok() || converted.value->models.empty()) {
            continue;
        }
        Document document = std::move(*converted.value);
        std::vector<std::vector<std::vector<u8>>> before;
        for (u32 c = 0; c < document.clips.size(); ++c) {
            before.push_back(Alone(document, 0, c, 0.25f * document.clips[c].duration));
        }
        SplitEveryOther(document, 0);
        for (u32 c = 0; c < document.clips.size(); ++c) {
            INFO(test::pathText(files[i]) << " clip " << c);
            const bool same = Alone(document, 0, c, 0.25f * document.clips[c].duration) == before[c];
            CHECK(same);
            ++compared;
        }
    }
    INFO("clips compared: " << compared);
    CHECK(compared > 0);
}

TEST_CASE("wem Validate holds a track set above its clip under Warcraft III only",
          "[wem][anim][animator][tracksets]") {
    Fixture f;
    const u32 c = f.clip(ReadRule::Wc3, 3);
    f.key<Vector3f>(c, f.translation, {0.0f}, {Vector3f{1, 0, 0}});
    TrackSet set;
    set.name = "Upper";
    set.channels = {f.translation};
    f.document.models[0].trackSets.push_back(set);
    f.document.clips[c].trackSets.push_back(ClipTrackSet{0, 3});

    f.document.profiles = {ProfileId::Wc3Reforged, ProfileId::Sc2};
    f.document.defaultProfile = ProfileId::Wc3Reforged;
    CHECK(Validate(f.document, ValidateLevel::Profile).countOf(DiagCode::TrackSetBelowDefault) == 1);
    f.document.clips[c].trackSets[0].priority = 4;
    CHECK(Validate(f.document, ValidateLevel::Profile).countOf(DiagCode::TrackSetBelowDefault) == 0);

    f.document.clips[c].trackSets[0].priority = 3;
    f.document.defaultProfile = ProfileId::Sc2;
    CHECK(Validate(f.document, ValidateLevel::Profile).countOf(DiagCode::TrackSetBelowDefault) == 0);
}

TEST_CASE("wem Validate forbids a channel on two clocks under Warcraft III",
          "[wem][anim][animator]") {
    Fixture f;
    const u32 host = f.clip(ReadRule::Wc3);
    f.key<Vector3f>(host, f.translation, {0.0f}, {Vector3f{1, 0, 0}});
    const u32 global = f.clip(ReadRule::Wc3, 0, true);
    f.document.clips[global].flags = ClipFlags::AutoPlay | ClipFlags::WorldClocked;
    f.key<Vector3f>(global, f.translation, {0.0f}, {Vector3f{2, 0, 0}});

    f.document.profiles = {ProfileId::Wc3Reforged, ProfileId::Sc2};
    f.document.defaultProfile = ProfileId::Wc3Reforged;
    const Diagnostics warcraft = Validate(f.document, ValidateLevel::Profile);
    CHECK(warcraft.countOf(DiagCode::GlobalAndClipChannel) == 1);
    CHECK(warcraft.hasErrors());

    // Legal under StarCraft II, and the concurrent global covers the keys.
    f.document.defaultProfile = ProfileId::Sc2;
    const Diagnostics starcraft = Validate(f.document, ValidateLevel::Profile);
    const auto found = starcraft.byCode(DiagCode::GlobalAndClipChannel);
    REQUIRE(found.size() == 1);
    CHECK(found[0].severity == Severity::Warning);
}

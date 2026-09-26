// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/crossing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>

#include "whiteout/models/wem/anim/animator.h"
#include "whiteout/models/wem/anim/curves.h"
#include "whiteout/models/wem/anim/rests.h"
#include "whiteout/models/wem/anim/track_read.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Rounds a refinement may take. Each halves the spans it touches, so this is
/// a span of a sixteenth of a millisecond's worth of the longest key gap; the
/// millisecond floor stops it long before.
constexpr int kMaxRounds = 12;

f32 Load(const u8* at) {
    f32 value = 0;
    std::memcpy(&value, at, sizeof(f32));
    return value;
}

/// Whether @p a and @p b, one element each of @p type, are the same motion.
bool Same(geom::AttrType type, const TrackTarget& target, const u8* a, const u8* b,
          const CrossingTolerance& tolerance) {
    if (type == geom::AttrType::Quat) {
        f32 dot = 0, la = 0, lb = 0;
        for (int c = 0; c < 4; ++c) {
            const f32 x = Load(a + c * 4);
            const f32 y = Load(b + c * 4);
            dot += x * y;
            la += x * x;
            lb += y * y;
        }
        la = std::sqrt(la);
        lb = std::sqrt(lb);
        if (la <= 0 || lb <= 0) {
            return la == lb;
        }
        const f32 cosine = std::min(std::fabs(dot) / (la * lb), 1.0f);
        return 2.0f * std::acos(cosine) <= tolerance.rotation &&
               std::fabs(la - lb) <= tolerance.length;
    }
    const bool floats = type == geom::AttrType::F32 || type == geom::AttrType::F32x2 ||
                        type == geom::AttrType::F32x3 || type == geom::AttrType::F32x4;
    const std::size_t size = geom::AttrTypeSize(type);
    if (!floats) {
        return std::memcmp(a, b, size) == 0;
    }
    const bool position = target.kind == TrackTarget::Kind::Node &&
                          target.channel == Channel::Translation;
    for (std::size_t at = 0; at < size; at += sizeof(f32)) {
        const f32 x = Load(a + at);
        const f32 y = Load(b + at);
        const f32 limit = position ? tolerance.translation
                                   : tolerance.relative * std::max(1.0f, std::fabs(x));
        if (std::fabs(x - y) > limit) {
            return false;
        }
    }
    return true;
}

/// Drops the MDX window markers from @p bag.
void DropWindow(NativeBag& bag) {
    std::vector<NativeBag::Entry> kept;
    for (NativeBag::Entry& entry : bag.entries) {
        if (entry.name != "intervalStart" && entry.name != "intervalEnd" &&
            entry.name != "globalSequenceId") {
            kept.push_back(std::move(entry));
        }
    }
    bag.entries = std::move(kept);
}

/// @p clip as a clip of rule @p to carries it: the rule, and no MDX window
/// outside Warcraft III.
Clip Restated(const Clip& clip, ReadRule to) {
    Clip view;
    view.name = clip.name;
    view.model = clip.model;
    view.duration = clip.duration;
    view.looping = clip.looping;
    view.flags = clip.flags;
    view.native = clip.native;
    view.readRule = to;
    if (to != ReadRule::Wc3) {
        DropWindow(view.native);
    }
    return view;
}

/// The whole milliseconds inside `[0, duration]` where @p track has a key, and
/// the millisecond before each: a Warcraft III window jumps to its first key's
/// value there, and any rule holds a step until its next key. Where one rule
/// wraps and the other does not (an M3 track shorter than its clip), the
/// refinement finds the span.
std::set<i32> KeyMs(const Clip& clip, const SubTrack& track) {
    std::set<i32> out;
    const i32 duration = static_cast<i32>(ClipMs(clip));
    for (const f32 t : track.times) {
        const i32 ms = static_cast<i32>(std::lround(t * 1000.0f));
        if (ms >= 0 && ms <= duration) {
            out.insert(ms);
            if (ms > 1) {
                out.insert(ms - 1);
            }
        }
    }
    return out;
}

std::vector<u8> ReadAt(const Clip& clip, const SubTrack& track, geom::AttrType type,
                       const std::vector<i32>& ms, std::span<const u8> rest) {
    return SampleSubTrackBatch(clip, track, type, std::span<const i32>(ms.data(), ms.size()), rest);
}

SubTrack FromSamples(const SubTrack& source, geom::AttrType type, const std::map<i32, std::vector<u8>>& keys,
                     bool step) {
    SubTrack out;
    out.channel = source.channel;
    out.interp = step ? Interpolation::Step
                      : type == geom::AttrType::Quat ? Interpolation::Slerp : Interpolation::Linear;
    for (const auto& [ms, value] : keys) {
        out.times.push_back(static_cast<f32>(ms) / 1000.0f);
        out.values.insert(out.values.end(), value.begin(), value.end());
    }
    return out;
}

/// The points a candidate is checked at: its spans' quarter points, on whole
/// milliseconds strictly inside each span.
std::vector<i32> CheckPoints(const std::map<i32, std::vector<u8>>& keys) {
    std::vector<i32> out;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const auto next = std::next(it);
        if (next == keys.end()) {
            break;
        }
        const i32 a = it->first;
        const i32 b = next->first;
        if (b - a < 2) {
            continue;
        }
        i32 last = a;
        for (const f32 q : {0.25f, 0.5f, 0.75f}) {
            const i32 at = a + static_cast<i32>(std::lround(q * static_cast<f32>(b - a)));
            if (at > last && at < b) {
                out.push_back(at);
                last = at;
            }
        }
    }
    return out;
}

bool Keys(const SubTrack* track) {
    return track != nullptr && !track->times.empty();
}

/// A constant track of @p value over @p clip.
SubTrack Constant(u32 channel, const std::vector<u8>& value, geom::AttrType type, f32 duration) {
    SubTrack out;
    out.channel = channel;
    out.interp = type == geom::AttrType::U32 ? Interpolation::Step
                 : type == geom::AttrType::Quat ? Interpolation::Slerp
                                                : Interpolation::Linear;
    out.times.push_back(0.0f);
    out.values = value;
    if (duration > 0.0f) {
        out.times.push_back(duration);
        out.values.insert(out.values.end(), value.begin(), value.end());
    }
    return out;
}

/// A step track as the lines that play it: each held value keyed again a
/// millisecond before the next key.
SubTrack StepAsLines(const SubTrack& track, geom::AttrType type) {
    const std::size_t size = geom::AttrTypeSize(type);
    SubTrack out;
    out.channel = track.channel;
    out.interp = type == geom::AttrType::Quat ? Interpolation::Slerp : Interpolation::Linear;
    for (std::size_t k = 0; k < track.times.size(); ++k) {
        const u8* value = track.values.data() + k * size;
        out.times.push_back(track.times[k]);
        out.values.insert(out.values.end(), value, value + size);
        if (k + 1 < track.times.size() && track.times[k + 1] - track.times[k] >= 0.002f) {
            out.times.push_back(track.times[k + 1] - 0.001f);
            out.values.insert(out.values.end(), value, value + size);
        }
    }
    return out;
}

/// Entering Warcraft III: MDX writes one controller per channel across every
/// clip, so a channel whose clips disagree is made linear everywhere.
void OneControllerPerChannel(Document& document, u32 model) {
    const Model& owner = document.models[model];
    for (const AnimChannel& channel : owner.animChannels.channels) {
        std::set<Interpolation> kinds;
        for (const Clip& clip : document.clips) {
            if (clip.model != model) {
                continue;
            }
            for (const SubTrackContainer& container : clip.containers) {
                const SubTrack* track = container.find(channel.id);
                if (Keys(track)) {
                    // Slerp is how a rotation spells linear.
                    kinds.insert(track->interp == Interpolation::Slerp ? Interpolation::Linear
                                                                       : track->interp);
                }
            }
        }
        if (kinds.size() < 2) {
            continue;
        }
        for (Clip& clip : document.clips) {
            if (clip.model != model) {
                continue;
            }
            for (SubTrackContainer& container : clip.containers) {
                for (SubTrack& track : container.subTracks) {
                    if (track.channel != channel.id || track.times.empty()) {
                        continue;
                    }
                    if (track.interp == Interpolation::Step) {
                        track = StepAsLines(track, channel.valueType);
                    } else if (ValuesPerKey(track.interp) > 1) {
                        track = LinearisedTrack(track, channel.valueType, 0.0f, clip.duration);
                    }
                }
            }
        }
    }
}

/// One play of @p clip alone at @p seconds.
std::vector<std::vector<u8>> Alone(const Animator& animator, u32 clip, f32 seconds) {
    Mix mix;
    mix.plays.push_back(Play{clip, seconds, 1.0f, false});
    mix.globals = false;
    Pose pose;
    animator.sample(mix, pose);
    return std::move(pose.channelValues);
}

} // namespace

CrossingTolerance TolerancesOf(const Model& model) {
    CrossingTolerance out;
    f32 height = model.bounds.maximum.z - model.bounds.minimum.z;
    if (!(height > 0.0f)) {
        f32 low = 0, high = 0;
        for (u32 n = 0; n < model.nodes.size(); ++n) {
            const f32 z = model.nodes.worldBind(n).translation.z;
            low = n == 0 ? z : std::min(low, z);
            high = n == 0 ? z : std::max(high, z);
        }
        height = high - low;
    }
    out.translation = 1e-3f * std::max(height, 1.0f);
    return out;
}

SubTrack RestateTrack(const Clip& clip, const SubTrack& track, geom::AttrType type,
                      const TrackTarget& target, ReadRule to, const CrossingTolerance& tolerance,
                      std::span<const u8> rest, bool* resampled) {
    if (resampled != nullptr) {
        *resampled = false;
    }
    const std::size_t size = geom::AttrTypeSize(type);
    if (track.times.empty() || !track.wellSized(type) || rest.size() != size) {
        return track;
    }
    const Clip view = Restated(clip, to);
    const i32 duration = static_cast<i32>(ClipMs(clip));
    const bool step = track.interp == Interpolation::Step || type == geom::AttrType::U32;

    // Read by its own rule at every key and both edges; a global loop of no
    // length is its first key, forever.
    std::set<i32> seed = duration > 0 ? KeyMs(clip, track) : std::set<i32>{};
    seed.insert(0);
    if (duration > 0) {
        seed.insert(duration);
    }
    std::vector<i32> at(seed.begin(), seed.end());
    const std::vector<u8> first = ReadAt(clip, track, type, at, rest);
    std::map<i32, std::vector<u8>> keys;
    for (std::size_t i = 0; i < at.size(); ++i) {
        keys[at[i]].assign(first.begin() + i * size, first.begin() + (i + 1) * size);
    }
    if (duration <= 0) {
        return FromSamples(track, type, keys, step);
    }

    // Then fill in every span the two reads disagree on.
    for (int round = 0; round < kMaxRounds; ++round) {
        const std::vector<i32> points = CheckPoints(keys);
        if (points.empty()) {
            break;
        }
        const SubTrack candidate = FromSamples(track, type, keys, step);
        const std::vector<u8> want = ReadAt(clip, track, type, points, rest);
        const std::vector<u8> have = ReadAt(view, candidate, type, points, rest);
        bool added = false;
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (!Same(type, target, want.data() + i * size, have.data() + i * size, tolerance)) {
                keys[points[i]].assign(want.begin() + i * size, want.begin() + (i + 1) * size);
                added = true;
            }
        }
        if (!added) {
            break;
        }
        if (resampled != nullptr) {
            *resampled = true;
        }
    }
    return FromSamples(track, type, keys, step);
}

u32 ConvertClips(Document& document, std::span<const u32> clips, ReadRule to, Game fromStorage,
                 Game toStorage, Diagnostics& diagnostics) {
    const Document before = document;
    std::set<u32> models;
    u32 converted = 0;
    for (const u32 c : clips) {
        if (c >= document.clips.size() || document.clips[c].model >= document.models.size() ||
            document.clips[c].readRule == to) {
            continue;
        }
        Clip& clip = document.clips[c];
        const Model& model = document.models[clip.model];
        const CrossingTolerance tolerance = TolerancesOf(model);
        u32 resampled = 0;
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const AnimChannel* channel = model.animChannels.find(track.channel);
                if (channel == nullptr || track.times.empty()) {
                    continue;
                }
                const TrackRests rests =
                    RestsOf(before, clip.model, *channel, fromStorage);
                const std::vector<u8>& rest =
                    KeyedAnywhere(before, clip.model, channel->id) ? rests.keyedElsewhere : rests.unkeyed;
                bool more = false;
                track = RestateTrack(before.clips[c], track, channel->valueType, channel->target, to,
                                     tolerance, rest, &more);
                if (more) {
                    ++resampled;
                    diagnostics.info(DiagCode::AnimTrackResampled,
                                     "clip '" + clip.name + "': channel " +
                                         std::to_string(channel->id) +
                                         " was given keys to play the same under its new rule",
                                     ElementRef(ElementKind::Track, channel->id));
                }
            }
        }
        const Clip restated = Restated(clip, to);
        clip.readRule = restated.readRule;
        clip.native = restated.native;
        models.insert(clip.model);
        ++converted;
        diagnostics.info(DiagCode::ClipRuleConverted,
                         "clip '" + clip.name + "' converted, " + std::to_string(resampled) +
                             " tracks resampled",
                         ElementRef(ElementKind::Clip, c));
    }
    if (fromStorage != toStorage) {
        for (u32 m = 0; m < document.models.size(); ++m) {
            models.insert(m);
        }
    }

    // What each clip showed where it keys nothing, under the storage it was
    // shown in, keyed wherever the new storage would show something else. A
    // key added to one clip can change another's Warcraft III rest, so this
    // runs until it settles. A global loop is left out both ways: it keys only
    // its own channels, and what it keys plays over every clip on its clock.
    for (const u32 m : models) {
        const std::vector<AnimChannel>& channels = document.models[m].animChannels.channels;
        std::vector<bool> global(channels.size(), false);
        for (std::size_t i = 0; i < channels.size(); ++i) {
            const Clip* owner = ClockOwner(before, m, channels[i].id);
            global[i] = owner != nullptr && IsGlobalLoop(*owner);
        }
        const Animator old(before, m, fromStorage);
        for (int pass = 0; pass < 4; ++pass) {
            const Animator now(document, m, toStorage);
            bool changed = false;
            for (u32 c = 0; c < document.clips.size(); ++c) {
                Clip& clip = document.clips[c];
                if (clip.model != m || clip.containers.empty() || IsGlobalLoop(clip)) {
                    continue;
                }
                const auto shown = Alone(old, c, 0.0f);
                const auto shows = Alone(now, c, 0.0f);
                for (std::size_t i = 0; i < channels.size(); ++i) {
                    const AnimChannel& channel = channels[i];
                    if (global[i] || Keys(FindSubTrack(clip, channel.id)) || shown[i] == shows[i] ||
                        shown[i].size() != geom::AttrTypeSize(channel.valueType)) {
                        continue;
                    }
                    if (Same(channel.valueType, channel.target, shown[i].data(), shows[i].data(),
                             TolerancesOf(document.models[m]))) {
                        continue;
                    }
                    SubTrackContainer& base = clip.containers.front();
                    base.subTracks.erase(std::remove_if(base.subTracks.begin(), base.subTracks.end(),
                                                        [&](const SubTrack& t) { return t.channel == channel.id; }),
                                         base.subTracks.end());
                    base.subTracks.push_back(Constant(channel.id, shown[i], channel.valueType, clip.duration));
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
        if (to == ReadRule::Wc3) {
            OneControllerPerChannel(document, m);
        }
    }
    return converted;
}

u32 ConvertClips(Document& document, std::span<const u32> clips, ReadRule to,
                 Diagnostics& diagnostics) {
    const Game from = GameOf(document.defaultProfile);
    const Game game = to == ReadRule::Sc2 ? Game::StarCraft : to == ReadRule::Wow ? Game::Wow : Game::Warcraft;
    return ConvertClips(document, clips, to, from, game, diagnostics);
}

u32 ResampleForTarget(Document& staged, ProfileId target, Game previewStorage,
                      Diagnostics& diagnostics) {
    const Game game = GameOf(target);
    if (game == Game::Diablo) {
        return 0;
    }
    const ReadRule rule = RuleOf(game);
    std::vector<u32> crossing;
    for (u32 c = 0; c < staged.clips.size(); ++c) {
        if (staged.clips[c].readRule != rule) {
            crossing.push_back(c);
        }
    }
    if (crossing.empty() && previewStorage == game) {
        return 0;
    }
    return ConvertClips(staged, crossing, rule, previewStorage, game, diagnostics);
}

namespace {

/// Keys that play @p poseAt: for every channel of @p seeds, the pose's value at
/// each of its seed times, then refined wherever the one track, read by
/// @p view's rule, parts from the pose at a span's quarter points. The pose at
/// a time is computed once for every channel.
SubTrackContainer BakePose(const Model& model, const Clip& view, std::map<u32, std::set<i32>> seeds,
                           const std::function<std::vector<std::vector<u8>>(i32)>& poseAt) {
    const CrossingTolerance tolerance = TolerancesOf(model);
    const i32 duration = static_cast<i32>(ClipMs(view));
    std::map<i32, std::vector<std::vector<u8>>> poses;
    const auto at = [&](i32 ms) -> const std::vector<std::vector<u8>>& {
        auto found = poses.find(ms);
        if (found == poses.end()) {
            found = poses.emplace(ms, poseAt(ms)).first;
        }
        return found->second;
    };
    SubTrackContainer out;
    for (auto& [channelId, seed] : seeds) {
        const u32 index = model.animChannels.indexOf(channelId);
        if (index == kInvalidIndex) {
            continue;
        }
        const AnimChannel& channel = model.animChannels.channels[index];
        const std::size_t size = geom::AttrTypeSize(channel.valueType);
        seed.insert(0);
        if (duration > 0) {
            seed.insert(duration);
        }
        const bool step = channel.valueType == geom::AttrType::U32 ||
                          channel.target.channel == Channel::Visibility;
        std::map<i32, std::vector<u8>> keys;
        for (const i32 ms : seed) {
            keys[ms] = at(ms)[index];
        }
        SubTrack like;
        like.channel = channelId;
        for (int round = 0; round < kMaxRounds; ++round) {
            const std::vector<i32> points = CheckPoints(keys);
            const SubTrack candidate = FromSamples(like, channel.valueType, keys, step);
            const std::vector<u8> have = ReadAt(view, candidate, channel.valueType, points, at(0)[index]);
            bool added = false;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const std::vector<u8>& want = at(points[i])[index];
                if (want.size() == size &&
                    !Same(channel.valueType, channel.target, want.data(), have.data() + i * size, tolerance)) {
                    keys[points[i]] = want;
                    added = true;
                }
            }
            if (!added) {
                break;
            }
        }
        out.subTracks.push_back(FromSamples(like, channel.valueType, keys, step));
    }
    return out;
}

} // namespace

u32 FlattenContainers(Document& document, Diagnostics& diagnostics) {
    u32 flattened = 0;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        if (document.clips[c].containers.size() < 2 || document.clips[c].model >= document.models.size()) {
            continue;
        }
        const Clip& clip = document.clips[c];
        const Model& model = document.models[clip.model];
        const Animator animator(document, clip.model);

        // Every channel a layer keys, and the times any of them keys it.
        std::map<u32, std::set<i32>> seeds;
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                const std::set<i32> ms = KeyMs(clip, track);
                seeds[track.channel].insert(ms.begin(), ms.end());
            }
        }
        SubTrackContainer merged = BakePose(model, clip, std::move(seeds), [&](i32 ms) {
            return Alone(animator, c, static_cast<f32>(ms) / 1000.0f);
        });
        merged.name = clip.containers.front().name;
        merged.priority = clip.containers.front().priority;
        merged.concurrent = clip.containers.front().concurrent;
        merged.native = clip.containers.front().native;

        Clip& target = document.clips[c];
        const std::string name = target.name;
        const std::size_t layers = target.containers.size();
        target.containers.clear();
        target.containers.push_back(std::move(merged));
        ++flattened;
        diagnostics.info(DiagCode::AnimLayersFlattened,
                         "clip '" + name + "': " + std::to_string(layers) +
                             " layers baked into one",
                         ElementRef(ElementKind::Clip, c));
    }
    return flattened;
}

Clip BakeMix(const Document& document, u32 model, const Mix& mix, f32 duration, ReadRule rule,
             const std::string& name, Diagnostics& diagnostics) {
    Clip made;
    made.name = name;
    made.model = model;
    made.duration = duration;
    made.looping = true;
    made.readRule = rule;
    if (model >= document.models.size()) {
        return made;
    }
    // The plays' own keys, where each play reaches them on the baked timeline.
    std::map<u32, std::set<i32>> seeds;
    const i32 length = static_cast<i32>(ClipMs(made));
    for (const Play& play : mix.plays) {
        if (play.clip >= document.clips.size() || document.clips[play.clip].model != model) {
            continue;
        }
        const Clip& clip = document.clips[play.clip];
        const i32 offset = static_cast<i32>(std::lround(play.seconds * 1000.0f));
        const i32 period = static_cast<i32>(ClipMs(clip));
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                std::set<i32>& seed = seeds[track.channel];
                for (const i32 ms : KeyMs(clip, track)) {
                    // Each time the play reaches the key inside the bake.
                    for (i32 at = ms - offset; at <= length; at += period > 0 ? period : length + 1) {
                        if (at >= 0) {
                            seed.insert(at);
                        }
                    }
                }
            }
        }
    }
    const Animator animator(document, model);
    Mix alone = mix;
    alone.globals = false; // They play under the baked clip in game.
    made.containers.push_back(BakePose(document.models[model], made, std::move(seeds), [&](i32 ms) {
        Mix at = alone;
        for (Play& play : at.plays) {
            play.seconds += static_cast<f32>(ms) / 1000.0f;
        }
        Pose pose;
        animator.sample(at, pose);
        return std::move(pose.channelValues);
    }));
    diagnostics.info(DiagCode::AnimLayersFlattened,
                     "clip '" + name + "': " + std::to_string(mix.plays.size()) +
                         " plays baked into one",
                     ElementRef(ElementKind::Clip, static_cast<u32>(document.clips.size())));
    return made;
}

} // namespace wem
} // namespace models
} // namespace whiteout

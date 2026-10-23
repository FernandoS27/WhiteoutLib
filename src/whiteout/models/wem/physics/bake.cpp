// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/bake.h>
#include <whiteout/models/wem/physics/cloth_bake.h>
#include <whiteout/models/wem/physics/crossing.h>
#include <whiteout/models/wem/physics/soft_step.h>

#include "soft_pass.h"

#include <whiteout/models/wem/anim/clip.h>
#include <whiteout/models/wem/anim/curve_keys.h>
#include <whiteout/models/wem/anim/detach.h>
#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/anim/transition.h>
#include <whiteout/models/wem/reflect_bytes.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr f32 kDegrees = 57.29577951308232f;

void Hash(u64& hash, std::span<const u8> bytes) {
    for (const u8 b : bytes) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
}

const ClipPhysics& SettingsOf(const Clip& clip) {
    static const ClipPhysics kNone;
    return clip.physics.has_value() ? *clip.physics : kNone;
}

/// One stretch of a clip a bake keyed on a channel (`PhysicsBake::stretchTimes`):
/// its keys run from @p from to @p to, the simulation's own from @p first
/// (before @p from where none of them is).
struct Stretch {
    f32 from = 0.0f;
    f32 first = 0.0f;
    f32 to = 0.0f;
};

/// A stretch's end at the clip's own: no key lies beyond.
constexpr f32 kEver = std::numeric_limits<f32>::max();
/// What a stretch reaches past its last key, so a key on its edge is inside.
constexpr f32 kStretchPad = 1e-5f;

/// @p channel's stretches in @p record and its place in the record's lists;
/// none for a channel the bake keys whole.
std::optional<std::size_t> StretchesOf(const PhysicsBake& record, u32 channel, std::vector<Stretch>& out) {
    out.clear();
    std::size_t at = 0;
    for (std::size_t i = 0; i < record.stretchChannels.size() && i < record.stretchCounts.size(); ++i) {
        const std::size_t count = record.stretchCounts[i];
        if (record.stretchChannels[i] != channel) {
            at += count;
            continue;
        }
        for (std::size_t k = at; k < at + count && k * 3 + 2 < record.stretchTimes.size(); ++k) {
            out.push_back(
                Stretch{record.stretchTimes[k * 3], record.stretchTimes[k * 3 + 1], record.stretchTimes[k * 3 + 2]});
        }
        return i;
    }
    return std::nullopt;
}

bool Within(const std::vector<Stretch>& stretches, f32 time) {
    return std::any_of(stretches.begin(), stretches.end(),
                       [&](const Stretch& s) { return time >= s.from && time <= s.to; });
}

/// @p track's keys inside @p stretches, or those outside, hashed with its
/// controller.
void HashKeys(u64& hash, const SubTrack& track, const std::vector<Stretch>& stretches, bool inside) {
    const u8 interp = static_cast<u8>(track.interp);
    Hash(hash, std::span<const u8>(&interp, 1));
    const std::size_t count = track.times.size();
    const std::size_t each = count != 0 ? track.values.size() / count : 0;
    for (std::size_t k = 0; k < count; ++k) {
        if (Within(stretches, track.times[k]) != inside) {
            continue;
        }
        Hash(hash, std::span<const u8>(reinterpret_cast<const u8*>(&track.times[k]), sizeof(f32)));
        Hash(hash, std::span<const u8>(track.values.data() + k * each, each));
    }
}

/// @p channel's keys in @p clip outside @p stretches, hashed: the clip's own
/// animation beside a bake's keys (`PhysicsBake::stretchOutside`).
u64 OutsideHash(const Clip& clip, u32 channel, const std::vector<Stretch>& stretches) {
    u64 hash = 1469598103934665603ull;
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            if (track.channel == channel) {
                HashKeys(hash, track, stretches, false);
            }
        }
    }
    return hash;
}

/// A baked channel's own animation as it now stands, on what the bake replaced
/// (§8.4): @p now's keys outside @p stretches, and @p source's inside them.
/// On a @p node's channel:
/// - a stretch the user emptied stays empty: what the bake replaced there is
///   not put back under keys taken away;
/// - where the key the animation leaves for a stretch from was changed, it
///   holds to where the simulation takes over. What the bake replaced goes on
///   from there, out of sight in the baked clip, and would pull the new pose
///   toward a key no one sees.
template <class T>
SubTrack OwnAnimation(const SubTrack* now, const SubTrack* source, const std::vector<Stretch>& stretches, bool node) {
    const std::vector<CurveKey<T>> ours = now != nullptr ? DecodeKeys<T>(*now) : std::vector<CurveKey<T>>{};
    const std::vector<CurveKey<T>> theirs = source != nullptr ? DecodeKeys<T>(*source) : std::vector<CurveKey<T>>{};
    SubTrack out;
    out.interp = now != nullptr ? now->interp : source->interp;
    const bool alike = source != nullptr && source->interp == out.interp;
    const auto same = [](const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; };
    std::vector<CurveKey<T>> keys;
    for (const CurveKey<T>& key : ours) {
        if (!Within(stretches, key.time)) {
            keys.push_back(key);
        }
    }
    const std::size_t outside = keys.size();
    // The source's own key at @p time, where the bake left it standing.
    const auto stood = [&](f32 time) -> const CurveKey<T>* {
        for (const CurveKey<T>& key : theirs) {
            if (key.time == time && !Within(stretches, key.time)) {
                return &key;
            }
        }
        return nullptr;
    };
    for (const Stretch& s : stretches) {
        const auto in = [&](const CurveKey<T>& key) { return key.time >= s.from && key.time <= s.to; };
        // The keys beside it: the last the animation has before, the first after.
        std::optional<std::size_t> before, after;
        for (std::size_t i = 0; i < outside; ++i) {
            if (keys[i].time < s.from && (!before || keys[i].time > keys[*before].time)) {
                before = i;
            }
            if (keys[i].time > s.to && (!after || keys[i].time < keys[*after].time)) {
                after = i;
            }
        }
        // A smooth neighbour got its half of the span the bake split: its own
        // tangent again, toward what is put back.
        if (alike && before) {
            if (const CurveKey<T>* was = stood(keys[*before].time); was != nullptr && same(was->value, keys[*before].value)) {
                keys[*before].out = was->out;
            }
        }
        if (alike && after) {
            if (const CurveKey<T>* was = stood(keys[*after].time); was != nullptr && same(was->value, keys[*after].value)) {
                keys[*after].in = was->in;
            }
        }
        if (node && std::none_of(ours.begin(), ours.end(), in)) {
            continue;
        }
        const std::size_t hidden = keys.size();
        for (const CurveKey<T>& key : theirs) {
            if (!in(key)) {
                continue;
            }
            CurveKey<T> kept = key;
            if (!alike) {
                kept.in = kept.out = FlatTangent(out.interp, kept.value);
                kept.tcb = Vector3f{0, 0, 0};
            }
            keys.push_back(kept);
        }
        if (!node || keys.size() == hidden || s.first < s.from || !before) {
            continue;
        }
        const CurveKey<T>* left = nullptr;
        for (const CurveKey<T>& key : theirs) {
            if (key.time < s.from && !Within(stretches, key.time) && (left == nullptr || key.time > left->time)) {
                left = &key;
            }
        }
        if (left != nullptr && left->time == keys[*before].time && same(left->value, keys[*before].value)) {
            continue;
        }
        CurveKey<T> hold;
        hold.time = s.first;
        hold.value = keys[*before].value;
        hold.in = hold.out = FlatTangent(out.interp, hold.value);
        keys.erase(std::remove_if(keys.begin() + static_cast<std::ptrdiff_t>(hidden), keys.end(),
                                  [&](const CurveKey<T>& key) {
                                      return Milliseconds(key.time) == Milliseconds(hold.time);
                                  }),
                   keys.end());
        keys.push_back(hold);
    }
    std::stable_sort(keys.begin(), keys.end(),
                     [](const CurveKey<T>& a, const CurveKey<T>& b) { return a.time < b.time; });
    EncodeKeys(out, keys);
    return out;
}

/// The clip's own keys on each channel a bake keyed in stretches, where they
/// are no longer what the bake left there (§8.4): the track an Unbake puts
/// back for it, by channel, with no key where the channel is left unkeyed.
std::map<u32, SubTrack> OwnAnimations(const Clip& clip, const PhysicsBake& baked) {
    std::map<u32, SubTrack> out;
    std::vector<Stretch> stretches;
    for (std::size_t i = 0; i < baked.stretchChannels.size() && i < baked.stretchOutside.size(); ++i) {
        const u32 channel = baked.stretchChannels[i];
        if (std::find(baked.channels.begin(), baked.channels.end(), channel) == baked.channels.end() ||
            StretchesOf(baked, channel, stretches) != i) {
            continue;
        }
        const SubTrack* now = nullptr;
        for (const SubTrackContainer& container : clip.containers) {
            if (const SubTrack* found = container.find(channel); found != nullptr && now == nullptr) {
                now = found;
            }
        }
        // One layer's keys: a channel the source keyed in several is put back whole.
        const SubTrack* source = nullptr;
        u32 layers = 0;
        for (const SubTrackContainer& container : baked.source) {
            if (const SubTrack* found = container.find(channel); found != nullptr) {
                source = found;
                ++layers;
            }
        }
        const SubTrack* sized = now != nullptr && !now->times.empty() ? now : source;
        if (layers > 1 || sized == nullptr || sized->times.empty()) {
            continue;
        }
        if (now != nullptr && now->times.empty()) {
            now = nullptr;
        }
        const std::size_t bytes = sized->values.size() / sized->times.size() / ValuesPerKey(sized->interp);
        // As the bake left them, with its own keys still in every stretch:
        // what it replaced goes back whole.
        const bool emptied = bytes != sizeof(f32) && std::any_of(stretches.begin(), stretches.end(), [&](const Stretch& s) {
            return now == nullptr ||
                   std::none_of(now->times.begin(), now->times.end(), [&](f32 t) { return t >= s.from && t <= s.to; });
        });
        if (!emptied && OutsideHash(clip, channel, stretches) == baked.stretchOutside[i]) {
            continue;
        }
        SubTrack made;
        if (bytes == sizeof(Quaternion)) {
            made = OwnAnimation<Quaternion>(now, source, stretches, true);
        } else if (bytes == sizeof(Vector3f)) {
            made = OwnAnimation<Vector3f>(now, source, stretches, true);
        } else if (bytes == sizeof(f32)) {
            made = OwnAnimation<f32>(now, source, stretches, false);
        } else {
            continue;
        }
        made.channel = channel;
        out.emplace(channel, std::move(made));
    }
    return out;
}

/// @p clip's baked channels' sub-tracks swapped back for what the bake
/// replaced, each in the place it had. A key the user set or changed outside
/// the stretch the bake keyed is the clip's own, and stays (`OwnAnimations`).
void RestoreSource(Clip& clip) {
    if (!clip.physics.has_value() || !clip.physics->baked.has_value()) {
        return;
    }
    const PhysicsBake& baked = *clip.physics->baked;
    const auto written = [&](u32 channel) {
        return std::find(baked.channels.begin(), baked.channels.end(), channel) != baked.channels.end();
    };
    std::map<u32, SubTrack> own = OwnAnimations(clip, baked);
    std::set<u32> placed;
    std::size_t at = 0;
    for (std::size_t c = 0; c < clip.containers.size(); ++c) {
        std::vector<SubTrack>& tracks = clip.containers[c].subTracks;
        std::erase_if(tracks, [&](const SubTrack& track) { return written(track.channel); });
        if (c >= baked.source.size()) {
            continue;
        }
        // In ascending place, so each lands where it was; one left unkeyed
        // moves those after it up.
        std::size_t gone = 0;
        for (const SubTrack& track : baked.source[c].subTracks) {
            const u32 place = at < baked.positions.size() ? baked.positions[at] : static_cast<u32>(tracks.size());
            ++at;
            const auto kept = own.find(track.channel);
            if (kept != own.end()) {
                placed.insert(track.channel);
                if (kept->second.times.empty()) {
                    ++gone;
                    continue;
                }
            }
            const std::size_t where = place >= gone ? place - gone : 0;
            tracks.insert(tracks.begin() + std::min<std::size_t>(where, tracks.size()),
                          kept != own.end() ? kept->second : track);
        }
    }
    // One the clip did not key before the bake: after its tracks, as a first key lands.
    for (auto& [channel, track] : own) {
        if (placed.count(channel) == 0 && !track.times.empty() && !clip.containers.empty()) {
            clip.containers.front().subTracks.push_back(std::move(track));
        }
    }
}

/// A driver's samples thinned to the keys that keep every step within
/// @p tolerance, every step when it is 0: its translations, or its rotations
/// measured as a turn in radians over the same share.
void ThinDriver(const std::vector<Vector3f>& moves, const std::vector<Quaternion>& turns, bool move, f32 tolerance,
                std::vector<u32>& kept) {
    kept.clear();
    const std::size_t count = move ? moves.size() : turns.size();
    std::vector<u8> keep(count, tolerance > 0.0f ? 0 : 1);
    if (count == 0) {
        return;
    }
    keep.front() = keep.back() = 1;
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    if (tolerance > 0.0f && count > 2) {
        spans.emplace_back(0, count - 1);
    }
    while (!spans.empty()) {
        const auto [a, b] = spans.back();
        spans.pop_back();
        f32 worst = 0.0f;
        std::size_t at = a;
        for (std::size_t i = a + 1; i < b; ++i) {
            const f32 u = static_cast<f32>(i - a) / static_cast<f32>(b - a);
            const f32 e = move ? (moves[a] + (moves[b] - moves[a]) * u - moves[i]).length()
                               : 2.0f * std::acos(std::min(1.0f, std::abs(Quaternion::slerp(turns[a], turns[b], u)
                                                                                .dot(turns[i]))));
            if (e > worst) {
                worst = e;
                at = i;
            }
        }
        if (worst > tolerance) {
            keep[at] = 1;
            spans.emplace_back(a, at);
            spans.emplace_back(at, b);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (keep[i] != 0) {
            kept.push_back(static_cast<u32>(i));
        }
    }
}

/// The node channel @p node's @p channel of @p model, or `kInvalidIndex`.
u32 FindNodeChannel(const Model& model, u32 node, Channel channel) {
    for (const AnimChannel& entry : model.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node &&
            entry.target.channel == channel && entry.target.sub == 0) {
            return entry.id;
        }
    }
    return kInvalidIndex;
}

/// The turn between two rotations, degrees: from their difference's axis
/// part, which a dot product near 1 loses to rounding.
f32 Angle(const Quaternion& a, const Quaternion& b) {
    const Quaternion d = a.normalized().conjugate() * b.normalized();
    const f32 axis = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return 2.0f * std::atan2(axis, std::abs(d.w)) * kDegrees;
}

Transform Blended(const Transform& a, const Transform& b, f32 w) {
    Transform out = a;
    out.translation = a.translation + (b.translation - a.translation) * w;
    out.rotation = Quaternion::slerp(a.rotation, b.rotation, w).normalized();
    return out;
}

/// The match's ease at @p u in [0, 1].
f32 Ease(const ClipPhysics& settings, f32 u) {
    u = std::clamp(u, 0.0f, 1.0f);
    switch (settings.ease) {
    case MatchEase::Linear:
        return u;
    case MatchEase::EaseIn:
        return u * u;
    case MatchEase::EaseOut:
        return 1.0f - (1.0f - u) * (1.0f - u);
    case MatchEase::Custom: {
        // Straight between the points, both ends pinned: a point on an end
        // would move it.
        std::vector<Vector2f> points{Vector2f{0.0f, 0.0f}, Vector2f{1.0f, 1.0f}};
        for (const Vector2f& p : settings.curve) {
            if (p.x > 0.0f && p.x < 1.0f) {
                points.push_back(p);
            }
        }
        std::sort(points.begin(), points.end(), [](const Vector2f& a, const Vector2f& b) { return a.x < b.x; });
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            if (u <= points[i + 1].x) {
                const f32 span = points[i + 1].x - points[i].x;
                const f32 f = span > 1e-6f ? (u - points[i].x) / span : 1.0f;
                return std::clamp(points[i].y + (points[i + 1].y - points[i].y) * f, 0.0f, 1.0f);
            }
        }
        return 1.0f;
    }
    default:
        return u * u * (3.0f - 2.0f * u);
    }
}

/// The world's forward: +X for Warcraft III's figures, -Y for StarCraft II's.
Vector3f Forward(const Document& document) {
    return GameOf(document.defaultProfile) == Game::StarCraft ? Vector3f{0, -1, 0} : Vector3f{1, 0, 0};
}

/// One stretch of a run: a clip from @p step for @p steps, each @p dt long.
struct Piece {
    u32 clip = kInvalidIndex;
    u32 step = 0;
    u32 steps = 0;
    enum class Kind : u8 { Loops, Once, Held, Baked, Post } kind = Kind::Once;
    f32 dt = 1.0f / 60.0f;
};

/// The clip's own time @p k steps into @p piece.
f32 LocalTime(const Document& document, const Piece& piece, u32 k, f32 dt) {
    const f32 length = document.clips[piece.clip].duration;
    const f32 t = static_cast<f32>(k) * dt;
    switch (piece.kind) {
    case Piece::Kind::Held:
        return 0.0f;
    case Piece::Kind::Loops:
        return length > 0.0f ? std::fmod(t, length) : 0.0f;
    case Piece::Kind::Post:
        // On from the clip's end, which the last step before it played.
        return length > 0.0f ? std::fmod(t + dt, length) : 0.0f;
    case Piece::Kind::Baked:
        return std::min(t, length);
    default:
        return std::min(t, length);
    }
}

/// Whether @p clip's first frame is held @p preheat seconds before it: never a
/// death's, which starts from its pose there; held, what it drops would have
/// fallen before its frame 0.
bool HoldsFirstFrame(const Clip& clip, u32 preheat) {
    return preheat > 0 && !IsDeathClipName(clip.name);
}

} // namespace

u32 BakeRun::stepAt(f32 seconds) const {
    if (times.empty()) {
        return 0;
    }
    const i64 at = static_cast<i64>(first) + std::llround(seconds / dt);
    return static_cast<u32>(std::clamp<i64>(at, 0, static_cast<i64>(times.size()) - 1));
}

void RestoreBakeSources(Document& document) {
    for (Clip& clip : document.clips) {
        RestoreSource(clip);
    }
}

namespace {

/// What a bake wrote into @p clip, hashed: the tracks of the channels @p record
/// lists, as they stand.
u64 WrittenHash(const Clip& clip, const PhysicsBake& record) {
    u64 hash = 1469598103934665603ull;
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            if (std::find(record.channels.begin(), record.channels.end(), track.channel) != record.channels.end()) {
                Hash(hash, ReflectBytes(track));
            }
        }
    }
    return hash;
}

/// What a bake wrote into @p clip and no one else keys, hashed as it stands
/// (`PhysicsBake::stretchInside`): a channel keyed in stretches by its keys
/// inside them, any other whole.
u64 InsideHash(const Clip& clip, const PhysicsBake& record) {
    u64 hash = 1469598103934665603ull;
    std::vector<Stretch> stretches;
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            if (std::find(record.channels.begin(), record.channels.end(), track.channel) == record.channels.end()) {
                continue;
            }
            if (StretchesOf(record, track.channel, stretches).has_value()) {
                HashKeys(hash, track, stretches, true);
            } else {
                Hash(hash, ReflectBytes(track));
            }
        }
    }
    return hash;
}

/// Where a record's bake keyed, and what it left (`PhysicsBake::stretchChannels`
/// on): nothing a bake reads, and a record made before them has none, so
/// never in a key.
constexpr std::string_view kStretchLists[] = {"stretchChannels", "stretchCounts", "stretchTimes", "stretchOutside",
                                              "stretchInside"};

/// The lists and the flag soft bodies added to the records a bake's inputs hold.
constexpr std::string_view kSoftLists[] = {"soft",        "softIds",  "softLumps", "softKeys",     "softWithin",
                                           "softFollows", "softPush", "softAtEnd", "softChannels", "softSource",
                                           "madeSettings"};

/// Every bound a document states is measured from what it holds, a baked
/// clip's keys among it (Recompute Extents, which every save runs), and a bake
/// reads none of them: in the key, a clip went out of date at its own save.
/// With them, the stretch lists.
constexpr std::string_view kMeasured[] = {"bounds",       "stretchChannels", "stretchCounts",
                                          "stretchTimes", "stretchOutside",  "stretchInside"};

/// `ReflectBytes` for a bake's key: the stretch lists left out, the bounds too
/// unless @p bounds, and
/// with @p lists the soft lists too while they are empty or off, so a model
/// with no soft body hashes as it did before there were any and a clip baked
/// then does not read out of date.
template <class T>
std::vector<u8> InputBytes(const T& value, bool bounds, bool lists = false) {
    ReflectBytesVisitor visitor;
    if (lists) {
        visitor.omitEmpty = kSoftLists;
    }
    if (bounds) {
        visitor.omit = kStretchLists;
    } else {
        visitor.omit = kMeasured;
    }
    visitor.field("", const_cast<T&>(value));
    return std::move(visitor.bytes);
}

/// `BakeInputs`, or with @p bounds the key as it was made while it held them.
u64 Inputs(const Document& document, u32 clip, bool bounds) {
    u64 hash = 1469598103934665603ull;
    if (clip >= document.clips.size() || document.clips[clip].model >= document.models.size()) {
        return hash;
    }
    const u32 owner = document.clips[clip].model;
    const Model& model = document.models[owner];
    Hash(hash, InputBytes(model.nodes, bounds));
    Hash(hash, InputBytes(model.physics, bounds, true));
    // A cloth's cage, pins, anchors and bindings are its mesh's (cloth design
    // §10.5).
    std::set<u32> clothMeshes;
    for (const Cloth& cloth : model.physics.cloths) {
        clothMeshes.insert(cloth.cage.mesh);
    }
    // A soft body's softness, lumps and lanes are its meshes' layers too (soft
    // design §11.4).
    for (const SoftBody& body : model.physics.soft) {
        clothMeshes.insert(body.meshes.begin(), body.meshes.end());
    }
    for (const u32 m : clothMeshes) {
        if (m < model.meshes.size()) {
            Hash(hash, InputBytes(model.meshes[m], bounds));
        }
    }
    // The channels some clip's source keys: one only a bake keys was written,
    // not read, and whether it is still declared after an Unbake reads nothing.
    std::set<u32> keyed;
    for (const Clip& each : document.clips) {
        if (each.model != owner) {
            continue;
        }
        const PhysicsBake* record =
            each.physics.has_value() && each.physics->baked.has_value() ? &*each.physics->baked : nullptr;
        for (const SubTrackContainer& container : each.containers) {
            for (const SubTrack& track : container.subTracks) {
                if (record == nullptr ||
                    std::find(record->channels.begin(), record->channels.end(), track.channel) == record->channels.end()) {
                    keyed.insert(track.channel);
                }
            }
        }
        for (std::size_t c = 0; record != nullptr && c < record->source.size(); ++c) {
            for (const SubTrack& track : record->source[c].subTracks) {
                keyed.insert(track.channel);
            }
        }
    }
    AnimChannelTable read = model.animChannels;
    std::erase_if(read.channels, [&](const AnimChannel& channel) { return keyed.count(channel.id) == 0; });
    Hash(hash, ReflectBytes(read));
    Hash(hash, ReflectBytes(model.poseStages));
    const u8 profile = static_cast<u8>(document.defaultProfile);
    Hash(hash, std::span<const u8>(&profile, 1));
    std::vector<u32> clips = BakeChain(document, clip);
    clips.push_back(clip);
    for (const u32 c : clips) {
        // Detached, as every bake and key edit leaves a clip: the same curves
        // either way, so a detach alone changes no input.
        Clip source = document.clips[c];
        RestoreSource(source);
        DetachClip(model.animChannels, source);
        // No settings read as the defaults, which a bake adds to hold its record.
        if (!source.physics.has_value()) {
            source.physics.emplace();
        }
        source.physics->baked.reset();
        Hash(hash, InputBytes(source, bounds));
    }
    // How a once-played clip of the lead-in runs is the library's: as its own
    // bake runs it, where it ran at this clip's step and Preheat. Said only of
    // a clip with one, so every other key reads as it did.
    if (std::any_of(clips.begin(), clips.end() - 1, [&](u32 c) { return !BakeLoops(document.clips[c]); })) {
        const u8 lead = 1; // 1: at its own step, after its own lead-in (2026-10-13).
        Hash(hash, std::span<const u8>(&lead, 1));
    }
    // A clip not set up carries no dropped rig over from what it comes from
    // (switches.cpp). Said only of one that is not, so a clip baked when every
    // baked clip was set up reads as it did.
    if (!IsSetUp(document.clips[clip])) {
        const u8 unset = 0x5u;
        Hash(hash, std::span<const u8>(&unset, 1));
    }
    // A death's first frame is no longer held, and a StarCraft II death model
    // drops every body (2026-10-23). Said only of a run either changes.
    const bool heldDeath = std::any_of(clips.begin(), clips.end(), [&](u32 c) {
        return IsDeathClipName(document.clips[c].name) && BakeComesFrom(document, c).kind == BakeFrom::FirstFrame &&
               SettingsOf(document.clips[c]).preheat > 0;
    });
    if (heldDeath || (GameOf(document.defaultProfile) == Game::StarCraft && IsDeathModel(document, owner))) {
        const u8 death = 1;
        Hash(hash, std::span<const u8>(&death, 1));
    }
    // How a soft body's lumps are fitted is the library's, and a clip baked by
    // a fit since changed holds keys the tool no longer shows (soft design §7).
    // Said only of a model with a soft body, so every other key reads as it did.
    if (!model.physics.soft.empty()) {
        const u8 fit = 1; // 1: the lumps of a point turn and scale alike (2026-10-12).
        Hash(hash, std::span<const u8>(&fit, 1));
    }
    // How a constraint reads the run is the library's as well: one made while
    // they read the animation alone holds keys a Rebake no longer writes. Said
    // only of a model with one, so every other key reads as it did.
    if (std::any_of(model.poseStages.begin(), model.poseStages.end(),
                    [](const PoseStage& stage) { return stage.enabled && !IsPhysicsStage(stage.kind); })) {
        const u8 reads = 1; // 1: the simulated pose (2026-10-12).
        Hash(hash, std::span<const u8>(&reads, 1));
    }
    // What a once-played clip's end is matched into, as it stands: a Rebake of
    // it makes this clip out of date (§7.4).
    if (const u32 to = BakeLoops(document.clips[clip]) ? kInvalidIndex : BakeGoesTo(document, clip);
        to < document.clips.size()) {
        Clip target = document.clips[to];
        DetachClip(model.animChannels, target);
        Hash(hash, InputBytes(target, bounds, true));
    }
    return hash;
}

} // namespace

u64 BakeInputs(const Document& document, u32 clip) {
    return Inputs(document, clip, false);
}

bool BakeInDate(const Document& document, u32 clip) {
    if (clip >= document.clips.size() || !document.clips[clip].physics.has_value() ||
        !document.clips[clip].physics->baked.has_value()) {
        return false;
    }
    const u64 held = document.clips[clip].physics->baked->inputs;
    // A key made while the bounds were in it stands for as long as they do.
    return held == Inputs(document, clip, false) || held == Inputs(document, clip, true);
}

BakeRun SimulateClip(const Document& given, u32 clip, const BakeHooks& hooks, const std::atomic<bool>* cancel,
                     BakeProgress* progress) {
    BakeRun run;
    run.clip = clip;
    if (clip >= given.clips.size() || given.clips[clip].model >= given.models.size()) {
        run.error = "no such clip";
        return run;
    }
    if (!hooks.step || !hooks.reset) {
        run.error = "no physics to run it";
        return run;
    }
    const u32 model = given.clips[clip].model;
    const Clip& baked = given.clips[clip];
    if (baked.duration <= 0.0f) {
        run.error = "the clip has no length";
        return run;
    }
    run.inputs = BakeInputs(given, clip);
    // Every clip of the run from its bake's source (§7.2).
    const bool anyBaked = std::any_of(given.clips.begin(), given.clips.end(), [](const Clip& c) {
        return c.physics.has_value() && c.physics->baked.has_value();
    });
    Document restored;
    if (anyBaked) {
        restored = given;
        RestoreBakeSources(restored);
    }
    const Document& document = anyBaked ? restored : given;
    const Model& owner = document.models[model];
    const ClipPhysics& settings = SettingsOf(baked);
    run.steps = BakeStepsOf(document, clip, run.dt);
    const u32 preheat = std::min<u32>(settings.preheat, 20);
    const f32 dt = run.dt;
    const auto stepsOf = [](f32 seconds, f32 step) {
        return std::max<u32>(1, static_cast<u32>(std::lround(seconds / step)));
    };

    // The pieces: what runs before frame 0, the clip, and any post-roll.
    std::vector<Piece> pieces;
    u32 at = 0;
    const auto add = [&](u32 c, u32 steps, Piece::Kind kind, f32 step) {
        pieces.push_back(Piece{c, at, steps, kind, step});
        at += steps;
    };
    const bool loops = BakeLoops(baked);
    const BakeLeadIn from = BakeComesFrom(document, clip);
    // The lead-in's loops, for the settling below.
    u32 leadLoops = preheat;
    if (from.kind == BakeFrom::Itself) {
        if (preheat > 0) {
            add(clip, preheat * run.steps, Piece::Kind::Loops, dt);
        }
    } else if (from.kind == BakeFrom::FirstFrame) {
        if (HoldsFirstFrame(baked, preheat)) {
            add(clip, stepsOf(static_cast<f32>(preheat), dt), Piece::Kind::Held, dt);
        }
    } else {
        // A once-played clip of the chain runs as its own bake runs it: at its
        // own step and after its own lead-in, so what continues it (a Decay, a
        // Dissipate) starts where that bake ended. A loop at the head is the
        // lead-in of the clip after it, and takes that clip's Preheat and step.
        const std::vector<u32> chain = BakeChain(document, clip);
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const Clip& lead = document.clips[chain[i]];
            if (i == 0 && BakeLoops(lead)) {
                const u32 next = i + 1 < chain.size() ? chain[i + 1] : clip;
                f32 step = dt;
                BakeStepsOf(document, next, step);
                leadLoops = std::max<u32>(1, std::min<u32>(SettingsOf(document.clips[next]).preheat, 20));
                add(chain[i], leadLoops * stepsOf(lead.duration, step), Piece::Kind::Loops, step);
                continue;
            }
            f32 step = dt;
            const u32 steps = std::max<u32>(1, BakeStepsOf(document, chain[i], step));
            if (const u32 own = std::min<u32>(SettingsOf(lead).preheat, 20); i == 0 && own > 0) {
                if (BakeComesFrom(document, chain[i]).kind == BakeFrom::Itself) {
                    leadLoops = own;
                    add(chain[i], own * steps, Piece::Kind::Loops, step);
                } else if (HoldsFirstFrame(lead, own)) {
                    add(chain[i], stepsOf(static_cast<f32>(own), step), Piece::Kind::Held, step);
                }
            }
            add(chain[i], steps, Piece::Kind::Once, step);
        }
    }
    run.first = at;
    const std::size_t baking = pieces.size();
    add(clip, run.steps + 1, Piece::Kind::Baked, dt);
    // A loop matched at its start reads the cycle after it (§7.3).
    if (loops && settings.match == LoopMatch::Crossfade &&
        (settings.where == MatchWhere::Start || settings.where == MatchWhere::Both)) {
        const u32 window = std::max<u32>(1, static_cast<u32>(std::lround(settings.window * run.steps)));
        add(clip, window, Piece::Kind::Post, dt);
    }
    // The run's clock where each piece starts.
    std::vector<f32> starts(pieces.size(), 0.0f);
    for (std::size_t p = 1; p < pieces.size(); ++p) {
        starts[p] = starts[p - 1] + static_cast<f32>(pieces[p - 1].steps) * pieces[p - 1].dt;
    }
    for (const Piece& piece : pieces) {
        run.pieces.push_back(BakeRun::Piece{piece.clip, piece.step, piece.kind == Piece::Kind::Held});
    }

    // Who moves, and where they are recorded.
    std::vector<SwitchReader> readers;
    readers.reserve(pieces.size());
    for (const Piece& piece : pieces) {
        readers.emplace_back(document, model, piece.clip, SwitchScope::Simulate, piece.clip != clip);
    }
    const SwitchReader& own = readers[pieces.size() - (pieces.back().kind == Piece::Kind::Post ? 2 : 1)];
    run.bodies = static_cast<u32>(owner.physics.bodies.size());
    std::vector<u8> movable(run.bodies, 0);
    std::set<u32> nodes;
    for (u32 b = 0; b < run.bodies; ++b) {
        // Movable in any clip of the model, so every run builds the same.
        bool can = own.movable(b);
        for (const SwitchReader& reader : readers) {
            can = can || reader.movable(b);
        }
        movable[b] = can ? 1 : 0;
        if (can && owner.physics.bodies[b].node < owner.nodes.size()) {
            nodes.insert(owner.physics.bodies[b].node);
        }
    }
    // A cloth that bakes into its bones records them as a body's node is
    // (cloth design §10.2).
    for (const Cloth& cloth : owner.physics.cloths) {
        if (hooks.cloth && cloth.recipe && cloth.recipe->bakeInto == ClothBakeInto::Bones) {
            for (const u32 bone : cloth.recipe->bones) {
                if (bone < owner.nodes.size()) {
                    nodes.insert(bone);
                }
            }
        }
    }
    // What a blast is placed and sized by: the bodied nodes alone.
    const std::vector<u32> bodied(nodes.begin(), nodes.end());
    // A node a constraint or IK stage drives is recorded too: where it stands
    // while the run moves what it reads, and what a stage's other bones do
    // while it moves one of them.
    for (const PoseStage& stage : owner.poseStages) {
        if (!stage.enabled || IsPhysicsStage(stage.kind)) {
            continue;
        }
        for (const u32 node : stage.driven) {
            if (node < owner.nodes.size()) {
                nodes.insert(node);
            }
        }
    }
    run.nodes.assign(nodes.begin(), nodes.end());
    hooks.reset(document, model, movable);
    std::vector<std::unique_ptr<ClothBoneFit>> fits;

    Animator animator(document, model);
    animator.keepTracksPrepared();
    // The constraints read the bodies where each step leaves them.
    SimulatedSteps kept;
    SimulatedStages around(document, model, kept);
    std::vector<u8> simulated(owner.nodes.size(), 0);
    std::vector<u8> wrote(owner.nodes.size(), 0);
    // The root, whose animated height a floor that follows it takes.
    u32 root = 0;
    while (root < owner.nodes.size() && owner.nodes.nodes[root].parent < owner.nodes.size()) {
        ++root;
    }
    f32 restHeight = 0.0f;
    {
        Mix rest;
        rest.globals = false;
        Pose pose;
        animator.evaluate(rest, pose);
        if (root < pose.frame.size()) {
            restHeight = pose.frame[root].data[3][2];
        }
    }
    const Vector3f forward = Forward(document);
    Vector3f travel{0, 0, 0};
    Vector3f lower{0, 0, 0}, upper{0, 0, 0};
    std::vector<BodySwitch> previous, now;
    BakeClothStep cloth;
    if (progress != nullptr) {
        progress->steps = at;
        progress->step = 0;
    }
    run.locals.reserve(static_cast<std::size_t>(at) * run.nodes.size());
    run.switches.reserve(static_cast<std::size_t>(at) * run.bodies);
    for (std::size_t p = 0; p < pieces.size(); ++p) {
        const Piece& piece = pieces[p];
        const Clip& playing = document.clips[piece.clip];
        const ClipPhysics& own = SettingsOf(playing);
        // The game's switch into a clip other than the one before.
        const bool switching = p > 0 && pieces[p - 1].clip != piece.clip;
        const ClipTransition into = switching ? ClipTransitionInto(document, piece.clip) : ClipTransition{0.0f};
        // A snapshot freezes the pose it finds, and a body the simulation still
        // moved there stands where that left it, as its baked keys play it:
        // not where the clip switched from keys it. By body, its node's local.
        std::vector<std::pair<u32, Transform>> left;
        if (const Transform* last = run.localsAt(run.recorded() - 1);
            switching && into.curve == ClipTransition::Curve::Snapshot && last != nullptr) {
            for (u32 b = 0; b < run.bodies && b < previous.size(); ++b) {
                const auto node = std::lower_bound(run.nodes.begin(), run.nodes.end(), owner.physics.bodies[b].node);
                if (previous[b].active && node != run.nodes.end() && *node == owner.physics.bodies[b].node) {
                    left.emplace_back(b, last[node - run.nodes.begin()]);
                }
            }
        }
        const f32 speed = own.travel ? ClipMoveSpeed(playing) : 0.0f;
        for (u32 k = 0; k < piece.steps; ++k) {
            if (cancel != nullptr && cancel->load()) {
                return run;
            }
            const f32 local = LocalTime(document, piece, k, piece.dt);
            Mix mix;
            mix.worldSeconds = starts[p] + static_cast<f32>(k) * piece.dt;
            const f32 into_t = static_cast<f32>(k) * piece.dt;
            const f32 w = switching ? TransitionWeight(into, into_t) : 1.0f;
            mix.plays.push_back(Play{piece.clip, local, w, false});
            if (switching && w < 1.0f) {
                // The clip switched from: frozen where the switch found it
                // (Warcraft III), or playing on, from the step after its last.
                const Piece& before = pieces[p - 1];
                const u32 carried = into.curve == ClipTransition::Curve::Snapshot ? before.steps : before.steps + k;
                // Its own steps at its own length, those since at this piece's.
                const f32 on = before.dt == piece.dt
                                   ? static_cast<f32>(carried) * piece.dt
                                   : static_cast<f32>(before.steps) * before.dt +
                                         static_cast<f32>(carried - before.steps) * piece.dt;
                const f32 then = before.kind == Piece::Kind::Loops
                                     ? std::fmod(on, std::max(document.clips[before.clip].duration, 1e-6f))
                                     : std::min(on, document.clips[before.clip].duration);
                mix.plays.push_back(Play{before.clip, before.kind == Piece::Kind::Held ? 0.0f : then, 1.0f, false});
            }
            Pose pose;
            animator.evaluate(mix, pose);
            readers[p].read(local, previous, now);
            if (w < 1.0f && !left.empty()) {
                Mix alone;
                alone.worldSeconds = mix.worldSeconds;
                alone.plays.push_back(Play{piece.clip, local, 1.0f, false});
                Pose incoming;
                animator.sample(alone, incoming, true);
                bool handed = false;
                for (const auto& [body, was] : left) {
                    // One the new clip simulates on is the simulation's still.
                    const u32 node = owner.physics.bodies[body].node;
                    if ((body < now.size() && now[body].active) || node >= incoming.local.size()) {
                        continue;
                    }
                    const Transform blended = Blended(was, incoming.local[node], w);
                    pose.local[node].translation = blended.translation;
                    pose.local[node].rotation = blended.rotation;
                    handed = true;
                }
                if (handed) {
                    animator.compose(pose);
                }
            }
            if (!around.empty()) {
                // By node: what this step writes, and what of it is still the
                // simulation's own (a body being handed back is not).
                std::fill(simulated.begin(), simulated.end(), u8{0});
                std::fill(wrote.begin(), wrote.end(), u8{0});
                for (u32 b = 0; b < run.bodies && b < now.size(); ++b) {
                    const u32 node = owner.physics.bodies[b].node;
                    if (node < wrote.size() && now[b].active) {
                        wrote[node] = 1;
                        simulated[node] = simulated[node] | static_cast<u8>(now[b].dynamic() ? 1 : 0);
                    }
                }
                around.before(animator, mix, mix.worldSeconds, pose, simulated);
            }
            if (piece.step + k == 0) {
                bool any = false;
                for (const u32 n : bodied) {
                    const Vector3f o{pose.frame[n].data[3][0], pose.frame[n].data[3][1], pose.frame[n].data[3][2]};
                    lower = any ? Vector3f{std::min(lower.x, o.x), std::min(lower.y, o.y), std::min(lower.z, o.z)} : o;
                    upper = any ? Vector3f{std::max(upper.x, o.x), std::max(upper.y, o.y), std::max(upper.z, o.z)} : o;
                    any = true;
                }
            }
            BakeStep step;
            step.switches = now;
            step.world = own.world;
            step.time = local;
            step.floor = own.floor;
            step.floorHeight = root < pose.frame.size() ? pose.frame[root].data[3][2] - restHeight : 0.0f;
            step.travel = travel;
            step.dt = piece.dt;
            step.lower = lower;
            step.upper = upper;
            hooks.step(document, model, step, pose);
            around.after(animator, mix, mix.worldSeconds, pose, wrote);
            if (hooks.cloth) {
                cloth.cloths.clear();
                cloth.particles.clear();
                cloth.vertices.clear();
                cloth.frames.clear();
                hooks.cloth(document, model, step, pose, cloth);
                // The first step sets what is recorded; a step that answers
                // otherwise records every particle at rest.
                if (run.times.empty()) {
                    run.cloths = cloth.cloths;
                    run.clothParticles = cloth.particles;
                    run.particles = static_cast<u32>(cloth.frames.size());
                    run.clothVertices = cloth.vertices;
                    run.clothBones.assign(run.cloths.size(), {});
                    run.clothFit.assign(run.cloths.size(), 0.0f);
                    run.clothSize.assign(run.cloths.size(), 0.0f);
                    fits.resize(run.cloths.size());
                    for (std::size_t c = 0, first = 0; c < run.cloths.size(); first += run.clothParticles[c], ++c) {
                        const Cloth* record = owner.physics.cloth(run.cloths[c]);
                        if (record == nullptr || !record->recipe || record->recipe->bakeInto != ClothBakeInto::Bones ||
                            first + run.clothParticles[c] > run.clothVertices.size()) {
                            continue;
                        }
                        fits[c] = std::make_unique<ClothBoneFit>(
                            document, model, *record,
                            std::span<const u32>(run.clothVertices).subspan(first, run.clothParticles[c]));
                        run.clothBones[c].assign(fits[c]->bones().begin(), fits[c]->bones().end());
                        run.clothSize[c] = fits[c]->size();
                    }
                }
                if (cloth.frames.size() == run.particles && cloth.cloths == run.cloths) {
                    run.clothFrames.insert(run.clothFrames.end(), cloth.frames.begin(), cloth.frames.end());
                } else {
                    run.clothFrames.resize(run.clothFrames.size() + run.particles);
                }
                // Each cloth's *Active*, and its bones fitted where it draws.
                const ClothParticleFrame* frames = run.clothFrames.data() + run.clothFrames.size() - run.particles;
                for (std::size_t c = 0, first = 0; c < run.cloths.size(); first += run.clothParticles[c], ++c) {
                    u32 index = 0;
                    while (index < owner.physics.cloths.size() && owner.physics.cloths[index].id != run.cloths[c]) {
                        ++index;
                    }
                    const bool active = readers[p].clothActive(index, local);
                    run.clothActive.push_back(active ? 1 : 0);
                    if (!active || !fits[c] || !fits[c]->fits()) {
                        continue;
                    }
                    const f32 missed =
                        fits[c]->fit(std::span<const ClothParticleFrame>(frames + first, run.clothParticles[c]), pose);
                    if (piece.step + k >= run.first && piece.step + k <= run.first + run.steps) {
                        run.clothFit[c] = std::max(run.clothFit[c], missed);
                    }
                }
            }
            for (const u32 n : run.nodes) {
                run.locals.push_back(n < pose.local.size() ? pose.local[n] : Transform{});
            }
            run.switches.insert(run.switches.end(), now.begin(), now.end());
            // The clip's own steps to the step; a lead-in's by the run's clock.
            run.times.push_back(piece.step >= run.first
                                    ? static_cast<f32>(piece.step + k - run.first) * dt
                                    : mix.worldSeconds - starts[baking]);
            run.localTimes.push_back(local);
            previous = now;
            travel = travel + forward * (speed * piece.dt);
            if (progress != nullptr) {
                ++progress->step;
            }
        }
    }

    // How settled: each preheat loop against the one before, at the same time.
    const Piece& lead = pieces.front();
    if (lead.kind == Piece::Kind::Loops && !run.nodes.empty()) {
        const u32 cycle = lead.clip == clip ? run.steps : stepsOf(document.clips[lead.clip].duration, lead.dt);
        const u32 loopsRun = lead.clip == clip ? preheat + 1 : leadLoops;
        for (u32 l = 1; l < loopsRun; ++l) {
            f32 worst = 0.0f;
            for (u32 k = 0; k < cycle; ++k) {
                const Transform* a = run.localsAt((l - 1) * cycle + k);
                const Transform* b = run.localsAt(l * cycle + k);
                if (a == nullptr || b == nullptr) {
                    continue;
                }
                for (std::size_t n = 0; n < run.nodes.size(); ++n) {
                    worst = std::max(worst, Angle(a[n].rotation, b[n].rotation));
                }
            }
            run.settling.push_back(worst);
        }
    }
    run.complete = true;
    return run;
}

void RecordedPose(const Document& document, const BakeRun& run, u32 step, Pose& pose) {
    const Transform* locals = run.localsAt(step);
    if (locals == nullptr || run.clip >= document.clips.size()) {
        return;
    }
    for (std::size_t n = 0; n < run.nodes.size(); ++n) {
        const u32 node = run.nodes[n];
        if (node < pose.local.size()) {
            pose.local[node].translation = locals[n].translation;
            pose.local[node].rotation = locals[n].rotation;
        }
    }
    Animator(document, document.clips[run.clip].model).compose(pose);
}

// ---- The bake (§8) ------------------------------------------------------------

namespace {

/// What one written node's samples are over the clip, step by step.
struct Written {
    u32 node = kInvalidNode;
    std::vector<Transform> samples; ///< Steps 0 .. steps.
    std::vector<u8> active;         ///< Per step, whether a body on it was simulated.
    /// Per step, whether the recording is written though none was: a stage it
    /// shares moved another of its nodes, or the match moved it.
    std::vector<u8> taken;
};

/// The loop matched (§7.3): its end or start blended with the cycle beside it,
/// or offset to close. @p s reads the recording at a clip step (negative in
/// the lead-in, past the end in the post-roll).
template <class Sample>
void MatchLoop(const ClipPhysics& settings, u32 steps, bool preheated, Sample s, std::vector<Transform>& out,
               bool& offsetInstead) {
    const u32 window = std::clamp<u32>(static_cast<u32>(std::lround(settings.window * static_cast<f32>(steps))), 1, steps);
    LoopMatch match = settings.match;
    const bool atEnd = settings.where != MatchWhere::Start;
    const bool atStart = settings.where != MatchWhere::End;
    const bool both = settings.where == MatchWhere::Both;
    // No cycle before it to blend with: the end is offset instead.
    if (match == LoopMatch::Crossfade && atEnd && !preheated) {
        match = LoopMatch::Offset;
        offsetInstead = true;
    }
    if (match == LoopMatch::Off) {
        return;
    }
    const u32 half = both ? std::max<u32>(1, window / 2) : window;
    const f32 share = both ? 0.5f : 1.0f;
    const Transform first = s(0);
    const Transform last = s(static_cast<i32>(steps));
    for (u32 i = 0; i <= steps; ++i) {
        Transform b = out[i];
        if (atEnd && i + half >= steps) {
            const f32 u = static_cast<f32>(i - (steps - half)) / static_cast<f32>(half);
            const f32 e = Ease(settings, u) * share;
            if (match == LoopMatch::Crossfade) {
                b = Blended(b, s(static_cast<i32>(i) - static_cast<i32>(steps)), e);
            } else {
                // Δ = S(0)·S(T)⁻¹, on the left.
                const Quaternion delta = (first.rotation * last.rotation.conjugate()).normalized();
                b.rotation = (Quaternion::slerp(Quaternion{0, 0, 0, 1}, delta, e) * b.rotation).normalized();
                b.translation = b.translation + (first.translation - last.translation) * e;
            }
        }
        if (atStart && i <= half) {
            const f32 u = static_cast<f32>(half - i) / static_cast<f32>(half);
            const f32 e = Ease(settings, u) * share;
            if (match == LoopMatch::Crossfade) {
                b = Blended(b, s(static_cast<i32>(i + steps)), e);
            } else {
                const Quaternion delta = (last.rotation * first.rotation.conjugate()).normalized();
                b.rotation = (Quaternion::slerp(Quaternion{0, 0, 0, 1}, delta, e) * b.rotation).normalized();
                b.translation = b.translation + (last.translation - first.translation) * e;
            }
        }
        out[i] = b;
    }
}

/// @p channel's controller in @p model's other clips, the first that keys it.
std::optional<Interpolation> ControllerElsewhere(const Document& document, u32 model, u32 clip, u32 channel) {
    for (u32 c = 0; c < document.clips.size(); ++c) {
        if (c == clip || document.clips[c].model != model) {
            continue;
        }
        for (const SubTrackContainer& container : document.clips[c].containers) {
            if (const SubTrack* track = container.find(channel); track != nullptr && !track->times.empty()) {
                return track->interp;
            }
        }
    }
    return std::nullopt;
}

template <class T>
T Part(const Transform& t);
template <>
Vector3f Part<Vector3f>(const Transform& t) {
    return t.translation;
}
template <>
Quaternion Part<Quaternion>(const Transform& t) {
    return t.rotation;
}

/// How far @p q is from the line @p u of the way from @p a to @p b, as a share
/// of @p height: a turn by what it moves a point a height away.
f32 OffLine(const Vector3f& a, const Vector3f& b, f32 u, const Vector3f& q, f32 height) {
    return (a + (b - a) * u - q).length() / height;
}
f32 OffLine(const Quaternion& a, const Quaternion& b, f32 u, const Quaternion& q, f32) {
    return Angle(Quaternion::slerp(a, b, u), q) / kDegrees;
}

/// One channel's keys spliced (§8.2): the source's outside @p spans, a key one
/// step outside each span on the source's curve, split exactly, and the baked
/// samples inside, those within @p noise (a share of @p height) of a line
/// left out. @p kept gets the times a reduction must keep.
///
/// A @p staged node's keys stand before its constraint and its samples after
/// it, and the stage is on up to the span and off inside: the edge keys sit a
/// millisecond out, the nearest the tracks are read, so no moment plays one
/// blended into the other. Its span's ends are kept and its slope is its own.
/// @p own gets the stretches it keyed (`PhysicsBake::stretchTimes`).
template <class T>
void Splice(const SubTrack* source, Interpolation interp, const std::vector<Transform>& samples,
            const std::vector<std::pair<u32, u32>>& spans, f32 dt, u32 stride, f32 noise, f32 height,
            const std::vector<T>& animation, bool staged, SubTrack& out, std::vector<f32>& kept,
            std::vector<Stretch>& own) {
    constexpr bool kQuat = std::is_same_v<T, Quaternion>;
    const u32 steps = static_cast<u32>(samples.size()) - 1;
    std::vector<CurveKey<T>> src = source != nullptr ? DecodeKeys<T>(*source) : std::vector<CurveKey<T>>{};
    const auto inside = [&](f32 t, f32 margin) {
        for (const auto& [a, b] : spans) {
            if (t >= static_cast<f32>(a) * dt - margin && t <= static_cast<f32>(b) * dt + margin) {
                return true;
            }
        }
        return false;
    };
    const f32 slop = dt * 1e-3f;
    std::vector<CurveKey<T>> keys;
    // The source's own keys, where the bake leaves the curve.
    for (const CurveKey<T>& key : src) {
        if (!inside(key.time, (staged ? 0.001f : dt) - slop)) {
            keys.push_back(key);
        }
    }
    // A key at each span's edge, one step out, on the source's curve: on a
    // whole millisecond, and split where the game reads it, which is keys at
    // whole milliseconds too.
    // @p before: the edge before its span, the source kept on its left.
    // @p step: the step nearest it, for a clip with no curve there.
    const auto onSource = [&](u32 ms, u32 step, bool before) {
        CurveKey<T> key;
        key.time = static_cast<f32>(ms) / 1000.0f;
        if (src.size() >= 2 && key.time >= src.front().time && key.time <= src.back().time) {
            for (std::size_t i = 0; i + 1 < src.size(); ++i) {
                if (key.time > src[i + 1].time) {
                    continue;
                }
                CurveKey<T> a = src[i];
                CurveKey<T> b = src[i + 1];
                const f32 span = static_cast<f32>(Milliseconds(b.time)) - static_cast<f32>(Milliseconds(a.time));
                const f32 u = span > 0.0f ? (static_cast<f32>(ms) - static_cast<f32>(Milliseconds(a.time))) / span : 0.0f;
                // On a source key: that key, as it is.
                if (u <= 0.0f || u >= 1.0f) {
                    return u <= 0.0f ? a : b;
                }
                CurveKey<T> cut = SplitSpan(interp, a, b, u);
                cut.time = key.time;
                // The kept neighbour takes its half's tangent: the other side
                // is the span's, or another edge's to split.
                for (CurveKey<T>& k : keys) {
                    if (before && k.time == a.time) {
                        k.out = a.out;
                    }
                    if (!before && k.time == b.time) {
                        k.in = b.in;
                    }
                }
                return cut;
            }
        }
        // No curve to split: what the clip plays there.
        key.value = key.in = key.out = animation[std::min<u32>(step, steps)];
        key.in = key.out = FlatTangent(interp, key.value);
        return key;
    };
    const auto keyed = [&](u32 ms) {
        return std::any_of(keys.begin(), keys.end(), [&](const CurveKey<T>& k) { return Milliseconds(k.time) == ms; });
    };
    for (const auto& [a, b] : spans) {
        // The stretch is its edges' and the source keys' it leaves out.
        const f32 margin = (staged ? 0.001f : dt) - slop;
        const u32 first = Milliseconds(static_cast<f32>(a) * dt);
        const u32 last = Milliseconds(static_cast<f32>(b) * dt);
        const u32 before = staged ? (first > 0 ? first - 1 : 0) : Milliseconds(static_cast<f32>(a > 0 ? a - 1 : 0) * dt);
        const u32 after = staged ? last + 1 : Milliseconds(static_cast<f32>(b + 1) * dt);
        Stretch stretch{-kEver, static_cast<f32>(a) * dt, kEver};
        if (a > 0) {
            stretch.from = std::min(static_cast<f32>(before) / 1000.0f, stretch.first - margin) - kStretchPad;
        }
        if (b < steps) {
            stretch.to = std::max(static_cast<f32>(after) / 1000.0f, static_cast<f32>(b) * dt + margin) + kStretchPad;
        }
        own.push_back(stretch);
        if (staged) {
            if (a > 0 && first > 0 && !keyed(before)) {
                keys.push_back(onSource(before, a, true));
            }
            if (b < steps && !keyed(after)) {
                keys.push_back(onSource(after, b, false));
            }
            continue;
        }
        if (a > 0) {
            keys.push_back(onSource(before, a - 1, true));
        }
        if (b < steps) {
            keys.push_back(onSource(after, b + 1, false));
        }
    }
    // With no source track, the clip's ends hold what it played before.
    if (source == nullptr) {
        for (const u32 end : {0u, steps}) {
            if (!inside(static_cast<f32>(end) * dt, slop) &&
                std::none_of(keys.begin(), keys.end(), [&](const CurveKey<T>& k) {
                    return std::abs(k.time - static_cast<f32>(end) * dt) < slop;
                })) {
                CurveKey<T> hold;
                hold.time = static_cast<f32>(end) * dt;
                hold.value = animation[end];
                hold.in = hold.out = FlatTangent(interp, hold.value);
                keys.push_back(hold);
                // The bake's own key too, with no simulation at it.
                own.push_back(Stretch{hold.time - kStretchPad, hold.time - 1.0f, hold.time + kStretchPad});
            }
        }
    }
    for (const CurveKey<T>& key : keys) {
        kept.push_back(key.time);
    }
    if (staged) {
        for (const auto& [a, b] : spans) {
            kept.push_back(static_cast<f32>(a) * dt);
            kept.push_back(static_cast<f32>(b) * dt);
        }
    }
    // The samples, every stride-th step and each span's ends. The reduction
    // reads every key it is given at every key, so a minute of Decay keyed
    // every step would cost it the square of its steps: a sample within
    // @p noise of the line between the samples kept around it (a body asleep)
    // goes before it, the farthest from each line kept first.
    std::vector<u8> onLine(steps + 1, 0);
    if (noise > 0.0f && stride == 1) {
        std::vector<std::pair<u32, u32>> lines(spans.begin(), spans.end());
        while (!lines.empty()) {
            const auto [a, b] = lines.back();
            lines.pop_back();
            u32 farthest = a;
            f32 far = 0.0f;
            for (u32 m = a + 1; m < b; ++m) {
                const f32 u = static_cast<f32>(m - a) / static_cast<f32>(b - a);
                const f32 off = OffLine(Part<T>(samples[a]), Part<T>(samples[b]), u, Part<T>(samples[m]), height);
                if (!(off < far)) {
                    far = off;
                    farthest = m;
                }
            }
            if (farthest == a) {
                continue;
            }
            if (far < noise) {
                std::fill(onLine.begin() + a + 1, onLine.begin() + b, u8{1});
            } else {
                lines.emplace_back(a, farthest);
                lines.emplace_back(farthest, b);
            }
        }
    }
    std::vector<std::size_t> baked;
    for (const auto& [a, b] : spans) {
        for (u32 i = a; i <= b; ++i) {
            if (i != a && i != b && ((i - a) % stride != 0 || onLine[i] != 0)) {
                continue;
            }
            CurveKey<T> key;
            key.time = static_cast<f32>(i) * dt;
            key.value = Part<T>(samples[i]);
            if constexpr (kQuat) {
                key.value = key.value.normalized();
            }
            key.in = key.out = key.value;
            baked.push_back(keys.size());
            keys.push_back(key);
        }
    }
    std::vector<std::size_t> order(keys.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a].time < keys[b].time; });
    std::vector<CurveKey<T>> sorted;
    std::vector<u8> isBaked;
    for (const std::size_t i : order) {
        if (!sorted.empty() && std::abs(sorted.back().time - keys[i].time) < slop) {
            continue;
        }
        sorted.push_back(keys[i]);
        isBaked.push_back(std::find(baked.begin(), baked.end(), i) != baked.end() ? 1 : 0);
    }
    if constexpr (kQuat) {
        // The short way round from key to key.
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            if (sorted[i].value.dot(sorted[i - 1].value) < 0.0f && isBaked[i] != 0) {
                sorted[i].value = Quaternion{-sorted[i].value.x, -sorted[i].value.y, -sorted[i].value.z, -sorted[i].value.w};
            }
        }
    }
    // A smooth controller's tangents from the sampled slope.
    if (interp == Interpolation::Hermite || interp == Interpolation::Bezier) {
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (isBaked[i] == 0) {
                continue;
            }
            if (!staged) {
                SmoothTangents(interp, sorted, i);
                continue;
            }
            std::size_t end = i;
            while (end < sorted.size() && isBaked[end] != 0) {
                ++end;
            }
            std::vector<CurveKey<T>> span(sorted.begin() + static_cast<std::ptrdiff_t>(i),
                                          sorted.begin() + static_cast<std::ptrdiff_t>(end));
            for (std::size_t k = 0; k < span.size(); ++k) {
                SmoothTangents(interp, span, k);
            }
            std::copy(span.begin(), span.end(), sorted.begin() + static_cast<std::ptrdiff_t>(i));
            i = end - 1;
        }
    }
    out.interp = interp;
    out.tcb.clear();
    EncodeKeys(out, sorted);
}

} // namespace

u32 BakeStepsOf(const Document& document, u32 clip, f32& dt) {
    dt = 1.0f / 60.0f;
    if (clip >= document.clips.size() || !(document.clips[clip].duration > 0.0f)) {
        return 0;
    }
    const Clip& baked = document.clips[clip];
    const u32 rate = std::max<u32>(SettingsOf(baked).stepRate, 15);
    const u32 steps = std::max<u32>(1, static_cast<u32>(std::ceil(baked.duration * static_cast<f32>(rate) - 1e-3f)));
    dt = baked.duration / static_cast<f32>(steps);
    return steps;
}

namespace {

/// A bake's key reduction, left to its caller (`BakeForExport`): what
/// `ReduceKeys` is to be asked for the clip.
struct DeferredReduction {
    u32 clip = kInvalidIndex;
    std::vector<u32> channels;
    std::vector<KeptKeys> kept;
    f32 tolerance = 0.0f;
    bool mesh = false;
};

/// `BakeClip`, its reduction handed to @p deferred when it is given: the
/// clip's record is then stamped as the keys stand unreduced, and
/// `StampReduced` stamps it again once they are.
BakeReport BakeClipInto(Document& document, const BakeRun& run, const BakeRun* goesTo, SoftModesCache* softModes,
                        DeferredReduction* deferred) {
    BakeReport report;
    if (run.clip >= document.clips.size() || !run.complete || run.steps == 0) {
        report.error = run.error.empty() ? "the run is not complete" : run.error;
        return report;
    }
    const u32 clipIndex = run.clip;
    const u32 model = document.clips[clipIndex].model;
    if (model >= document.models.size()) {
        report.error = "no such model";
        return report;
    }
    // The channels a bake declared: one found again is still owned, so the
    // last Unbake drops it.
    std::vector<u32> owned;
    for (const Clip& each : document.clips) {
        if (each.model == model && each.physics.has_value() && each.physics->baked.has_value()) {
            owned.insert(owned.end(), each.physics->baked->appended.begin(), each.physics->baked->appended.end());
        }
    }
    const Clip& was = document.clips[clipIndex];
    std::vector<u32> appended =
        was.physics.has_value() && was.physics->baked.has_value() ? was.physics->baked->appended : std::vector<u32>{};
    // Baked before: from the source again, as Rebake is. Its channels stay, so
    // the bake keys the same ids and the table only ever grows, as a journal of
    // appends needs.
    UnbakeClip(document, clipIndex, false);
    DetachClip(document, model, clipIndex);
    Clip& clip = document.clips[clipIndex];
    const ClipPhysics settings = SettingsOf(clip);
    const u32 steps = run.steps;
    const f32 dt = run.dt;
    Model& owner = document.models[model];

    // What the clip plays on its own, step by step: the source the bake
    // compares with and splices into.
    std::vector<std::vector<Transform>> animation(steps + 1);
    {
        Animator animator(document, model);
        animator.keepTracksPrepared();
        for (u32 i = 0; i <= steps; ++i) {
            Mix mix;
            mix.plays.push_back(Play{clipIndex, static_cast<f32>(i) * dt, 1.0f, false});
            Pose pose;
            animator.sample(mix, pose, true);
            animation[i] = pose.local;
        }
    }
    // A node on a body the bake cannot key: one a global loop keys.
    std::set<u32> global;
    for (const Clip& other : document.clips) {
        if (other.model != model || !IsGlobalLoop(other)) {
            continue;
        }
        for (const SubTrackContainer& container : other.containers) {
            for (const SubTrack& track : container.subTracks) {
                const AnimChannel* channel = owner.animChannels.find(track.channel);
                if (channel != nullptr && channel->target.kind == TrackTarget::Kind::Node && !track.times.empty()) {
                    global.insert(channel->target.node);
                }
            }
        }
    }

    // Each recorded node's samples, and when a body on it was simulated.
    std::vector<Written> written;
    const auto recorded = [&](std::size_t n, i32 step) {
        const i64 at = std::clamp<i64>(static_cast<i64>(run.first) + step, 0, static_cast<i64>(run.recorded()) - 1);
        return run.localsAt(static_cast<u32>(at))[n];
    };
    // The recorded cloths that bake into each node, by the run's cloth index.
    std::map<u32, std::vector<u32>> clothsOn;
    for (u32 c = 0; c < run.clothBones.size(); ++c) {
        for (const u32 bone : run.clothBones[c]) {
            clothsOn[bone].push_back(c);
        }
    }
    for (std::size_t n = 0; n < run.nodes.size(); ++n) {
        Written w;
        w.node = run.nodes[n];
        w.samples.resize(steps + 1);
        w.active.assign(steps + 1, 0);
        w.taken.assign(steps + 1, 0);
        const auto on = clothsOn.find(w.node);
        for (u32 i = 0; i <= steps; ++i) {
            const BodySwitch* switches = run.switchesAt(run.first + i);
            for (u32 b = 0; b < run.bodies && switches != nullptr; ++b) {
                if (b < owner.physics.bodies.size() && owner.physics.bodies[b].node == w.node && switches[b].active) {
                    w.active[i] = 1;
                }
            }
            // A cloth bone, where its cloth draws (cloth design §10.2).
            for (std::size_t c = 0; on != clothsOn.end() && c < on->second.size(); ++c) {
                if (run.clothActiveAt(run.first + i, on->second[c])) {
                    w.active[i] = 1;
                }
            }
            // A node no body simulates plays the clip: the run's switch into
            // it from the lead-in is the game's to blend, never the keys' (§7.2).
            w.samples[i] = w.active[i] != 0 ? recorded(n, static_cast<i32>(i)) : animation[i][w.node];
        }
        written.push_back(std::move(w));
    }

    // The nodes a constraint or IK stage drives. Their keys stand before the
    // stage and the recording after it, so the two are never compared or
    // blended: such a node is written where a body simulated it, where a
    // stage it shares moved another of its nodes, or where the match moved it.
    std::vector<const PoseStage*> constraints;
    std::vector<u8> staged(owner.nodes.size(), 0);
    for (const PoseStage& stage : owner.poseStages) {
        if (!stage.enabled || IsPhysicsStage(stage.kind)) {
            continue;
        }
        constraints.push_back(&stage);
        for (const u32 node : stage.driven) {
            if (node < staged.size()) {
                staged[node] = 1;
            }
        }
    }
    std::map<u32, std::size_t> recordOf;
    for (std::size_t n = 0; n < written.size(); ++n) {
        recordOf[written[n].node] = n;
    }
    const auto owns = [](const Written& w, u32 i) { return w.active[i] != 0 || w.taken[i] != 0; };
    // A stage moves its nodes together: where the bake has one, it takes them
    // all, as recorded.
    const auto together = [&] {
        for (bool again = true; again;) {
            again = false;
            for (const PoseStage* stage : constraints) {
                for (u32 i = 0; stage->driven.size() > 1 && i <= steps; ++i) {
                    bool any = false;
                    for (const u32 node : stage->driven) {
                        const auto found = recordOf.find(node);
                        any = any || (found != recordOf.end() && owns(written[found->second], i));
                    }
                    for (const u32 node : stage->driven) {
                        const auto found = recordOf.find(node);
                        if (any && found != recordOf.end() && !owns(written[found->second], i)) {
                            written[found->second].taken[i] = 1;
                            written[found->second].samples[i] = recorded(found->second, static_cast<i32>(i));
                            again = true;
                        }
                    }
                }
            }
        }
    };
    together();
    const f32 height = std::max(ModelHeight(owner), 1.0f);
    const f32 still = 1e-5f * height;
    const auto departs = [&](const Transform& a, const Transform& b) {
        return Angle(a.rotation, b.rotation) > 1e-3f || (a.translation - b.translation).length() > still;
    };
    // Only what the run moved in this clip is matched (§8.2).
    std::vector<u8> simulates(written.size(), 0);
    for (std::size_t n = 0; n < written.size(); ++n) {
        for (u32 i = 0; i <= steps; ++i) {
            simulates[n] = simulates[n] | static_cast<u8>(owns(written[n], i) ? 1 : 0);
        }
    }

    // The seam before matching, then the match (§7.3, §7.4). A loop's seam
    // is its end against its start; a once-played clip's, its end against
    // the frame of its *Goes to* it meets, and none when it goes nowhere.
    const bool loops = BakeLoops(clip);
    const u32 target = loops ? kInvalidIndex : BakeGoesTo(document, clipIndex);
    // A looping target is met in its tail, which runs into its frame 0; any
    // other at its frame 0, held (§7.4).
    const bool intoLoop = target != kInvalidIndex && BakeLoops(document.clips[target]);
    Pose meets;
    if (target != kInvalidIndex) {
        Mix mix;
        mix.plays.push_back(Play{target, intoLoop ? document.clips[target].duration : 0.0f, 1.0f, false});
        Animator(document, model).sample(mix, meets, true);
    }
    const auto seam = [&](u32 node, const Quaternion& first, const Quaternion& last) {
        if (loops) {
            return Angle(first, last);
        }
        return node < meets.local.size() ? Angle(last, meets.local[node].rotation) : 0.0f;
    };
    f32 seamBefore = 0.0f;
    for (std::size_t n = 0; n < written.size(); ++n) {
        if (std::find(written[n].active.begin(), written[n].active.end(), u8{1}) != written[n].active.end()) {
            seamBefore = std::max(seamBefore, seam(written[n].node, written[n].samples.front().rotation,
                                                   written[n].samples.back().rotation));
        }
    }
    if (loops) {
        // A cycle of its own before it, to cross-fade with; a lead-in of its
        // first frame or of another clip is not one.
        bool preheated = false;
        for (std::size_t p = 1; p < run.pieces.size(); ++p) {
            if (run.pieces[p].step == run.first) {
                const BakeRun::Piece& lead = run.pieces[p - 1];
                preheated = lead.clip == run.clip && !lead.held && run.first - lead.step >= steps;
            }
        }
        for (std::size_t n = 0; n < written.size(); ++n) {
            Written& w = written[n];
            if (simulates[n] == 0) {
                continue;
            }
            const auto sample = [&](i32 step) { return recorded(n, step); };
            if (staged[w.node] == 0) {
                MatchLoop(settings, steps, preheated, sample, w.samples, report.offsetInstead);
                continue;
            }
            // Matched as recorded, after its stage: a step the match moves is
            // the bake's from then on.
            std::vector<Transform> made(steps + 1);
            for (u32 i = 0; i <= steps; ++i) {
                made[i] = recorded(n, static_cast<i32>(i));
            }
            MatchLoop(settings, steps, preheated, sample, made, report.offsetInstead);
            for (u32 i = 0; i <= steps; ++i) {
                if (owns(w, i)) {
                    w.samples[i] = made[i];
                } else if (departs(recorded(n, static_cast<i32>(i)), made[i])) {
                    w.taken[i] = 1;
                    w.samples[i] = made[i];
                }
            }
        }
    } else if (target != kInvalidIndex && settings.match != LoopMatch::Off) {
        // The end crossfaded into what a looping target plays from its end
        // back, or another's frame 0: its run, else its keys as they stand.
        const Clip& into = document.clips[target];
        const u32 window =
            std::clamp<u32>(static_cast<u32>(std::lround(settings.window * static_cast<f32>(steps))), 1, steps);
        const Animator animator(document, model);
        const StageRunner stages(document, model);
        bool anyStaged = false;
        for (std::size_t n = 0; n < written.size(); ++n) {
            anyStaged = anyStaged || (simulates[n] != 0 && staged[written[n].node] != 0);
        }
        for (u32 i = steps - window; i <= steps; ++i) {
            const f32 u = static_cast<f32>(i - (steps - window)) / static_cast<f32>(window);
            const f32 e = Ease(settings, u);
            const f32 there = intoLoop ? std::max(into.duration - static_cast<f32>(steps - i) * dt, 0.0f) : 0.0f;
            // Its bake is its matched cycle; else its run; else its keys, and
            // for a constrained node what its stage makes of them.
            const bool intoBaked = into.physics.has_value() && into.physics->baked.has_value();
            const bool useRun = !intoBaked && goesTo != nullptr && goesTo->clip == target && goesTo->complete;
            Pose pose;
            Pose played;
            if (!useRun) {
                Mix mix;
                mix.plays.push_back(Play{target, there, 1.0f, false});
                animator.sample(mix, pose, true);
                if (anyStaged) {
                    animator.evaluate(mix, played);
                    stages.constrain(animator, mix, played);
                }
            }
            for (std::size_t n = 0; n < written.size(); ++n) {
                Written& w = written[n];
                if (simulates[n] == 0) {
                    continue;
                }
                const bool after = staged[w.node] != 0;
                const Pose& keys = after ? played : pose;
                Transform partner;
                if (useRun) {
                    const auto found = std::find(goesTo->nodes.begin(), goesTo->nodes.end(), w.node);
                    if (found == goesTo->nodes.end()) {
                        continue;
                    }
                    partner = goesTo->localsAt(goesTo->stepAt(there))[found - goesTo->nodes.begin()];
                } else if (w.node < keys.local.size()) {
                    partner = keys.local[w.node];
                } else {
                    continue;
                }
                if (!after || owns(w, i)) {
                    w.samples[i] = Blended(w.samples[i], partner, e);
                    continue;
                }
                const Transform from = recorded(n, static_cast<i32>(i));
                const Transform made = Blended(from, partner, e);
                if (departs(from, made)) {
                    w.taken[i] = 1;
                    w.samples[i] = made;
                }
            }
        }
    }
    together();

    // Which nodes are written, and where: wherever the bake's samples leave
    // the clip's own animation, a body on them simulated or the match moved.
    PhysicsBake record;
    record.inputs = run.inputs;
    record.settling = run.settling;
    record.source.resize(clip.containers.size());
    // Each container's channels as they stand before the bake: where each
    // replaced sub-track goes back to.
    std::vector<std::vector<u32>> before(clip.containers.size());
    for (u32 c = 0; c < clip.containers.size(); ++c) {
        for (const SubTrack& track : clip.containers[c].subTracks) {
            before[c].push_back(track.channel);
        }
    }
    std::vector<std::vector<std::pair<u32, SubTrack>>> replaced(clip.containers.size());
    std::vector<KeptKeys> keep;
    // A hundredth of the accuracy's error: what thinning a still stretch may
    // add to it, unmeasured.
    const f32 noise = settings.tolerance * 0.01f;
    const u32 stride = settings.tolerance > 0.0f
                           ? 1
                           : std::max<u32>(1, static_cast<u32>(std::lround(1.0f / (dt * std::max<f32>(settings.keyRate, 1)))));
    // @p made in place of @p channel's sub-track in @p home, or after its
    // tracks when the clip keys none; every one it replaces kept, and where it was.
    const auto place = [&](u32 channel, SubTrack made, u32 home, bool sourced) {
        for (u32 c = 0; c < clip.containers.size(); ++c) {
            std::vector<SubTrack>& tracks = clip.containers[c].subTracks;
            u32 at = 0;
            for (const SubTrack& track : tracks) {
                if (track.channel != channel) {
                    continue;
                }
                while (at < before[c].size() && before[c][at] != channel) {
                    ++at;
                }
                replaced[c].emplace_back(at, track);
                ++at;
            }
            if (c == home && sourced) {
                for (SubTrack& track : tracks) {
                    if (track.channel == channel) {
                        track = made;
                        break;
                    }
                }
            } else {
                std::erase_if(tracks, [&](const SubTrack& track) { return track.channel == channel; });
            }
        }
        if (!sourced) {
            clip.containers[home].subTracks.push_back(std::move(made));
        }
    };
    // A channel an earlier bake declared, here or in another clip, stays the
    // bakes' own, so the last Unbake drops it.
    const auto own = [&](u32 channel) {
        if (std::find(owned.begin(), owned.end(), channel) != owned.end() &&
            std::find(appended.begin(), appended.end(), channel) == appended.end()) {
            appended.push_back(channel);
        }
    };
    // Per node the bake writes, the steps it writes it at; and per channel it
    // keys in stretches, those (`PhysicsBake::stretchTimes`).
    std::map<u32, std::vector<u8>> writes;
    std::map<u32, std::vector<Stretch>> stretched;
    for (const Written& w : written) {
        if (global.count(w.node) != 0) {
            report.globalLoops.push_back(w.node);
            continue;
        }
        bool turns = false, moves = false;
        std::vector<u8> differs(steps + 1, 0);
        for (u32 i = 0; i <= steps; ++i) {
            const Transform& a = animation[i][w.node];
            const Transform& b = w.samples[i];
            const bool turned = Angle(a.rotation, b.rotation) > 1e-3f;
            const bool moved = (a.translation - b.translation).length() > still;
            turns = turns || turned;
            moves = moves || moved;
            differs[i] = owns(w, i) || turned || moved ? 1 : 0;
        }
        if (!turns && !moves) {
            continue;
        }
        std::vector<std::pair<u32, u32>> spans;
        for (u32 i = 0; i <= steps; ++i) {
            if (differs[i] == 0) {
                continue;
            }
            if (!spans.empty() && spans.back().second + 1 >= i) {
                spans.back().second = i;
            } else {
                spans.emplace_back(i, i);
            }
        }
        ++report.nodes;
        writes[w.node] = differs;
        for (const Channel which : {Channel::Rotation, Channel::Translation}) {
            if (which == Channel::Translation && !moves) {
                continue;
            }
            u32 channel = FindNodeChannel(owner, w.node, which);
            if (channel == kInvalidIndex) {
                AnimChannel made;
                made.id = owner.animChannels.nextFreeId();
                made.target.kind = TrackTarget::Kind::Node;
                made.target.node = w.node;
                made.target.channel = which;
                made.valueType = which == Channel::Rotation ? geom::AttrType::Quat : geom::AttrType::F32x3;
                channel = owner.animChannels.add(made);
                appended.push_back(channel);
            } else {
                own(channel);
            }
            const AnimChannel* declared = owner.animChannels.find(channel);
            const bool quat = which == Channel::Rotation && declared->valueType == geom::AttrType::Quat;
            if (which == Channel::Rotation && !quat) {
                continue; // an Euler or scalar rotation is not the bake's to rewrite
            }
            record.channels.push_back(channel);
            // The container holding the node's track, else the first; the
            // same channel cleared from the others, which the export would
            // flatten over the keys.
            u32 home = 0;
            const SubTrack* source = nullptr;
            for (u32 c = 0; c < clip.containers.size(); ++c) {
                if (const SubTrack* found = clip.containers[c].find(channel); found != nullptr && source == nullptr) {
                    home = c;
                    source = found;
                }
            }
            SubTrack original;
            if (source != nullptr) {
                original = *source;
                if (!original.tcb.empty()) {
                    ++report.restated;
                }
            }
            Interpolation interp = which == Channel::Rotation ? Interpolation::Slerp : Interpolation::Linear;
            if (source != nullptr) {
                interp = source->interp;
            } else if (const std::optional<Interpolation> elsewhere =
                           ControllerElsewhere(document, model, clipIndex, channel)) {
                interp = *elsewhere;
            }
            SubTrack made;
            made.channel = channel;
            std::vector<f32> keptTimes;
            if (quat) {
                std::vector<Quaternion> anim(steps + 1);
                for (u32 i = 0; i <= steps; ++i) {
                    anim[i] = animation[i][w.node].rotation;
                }
                Splice<Quaternion>(source != nullptr ? &original : nullptr, interp, w.samples, spans, dt, stride, noise,
                                   height, anim, staged[w.node] != 0, made, keptTimes, stretched[channel]);
            } else {
                std::vector<Vector3f> anim(steps + 1);
                for (u32 i = 0; i <= steps; ++i) {
                    anim[i] = animation[i][w.node].translation;
                }
                Splice<Vector3f>(source != nullptr ? &original : nullptr, interp, w.samples, spans, dt, stride, noise,
                                 height, anim, staged[w.node] != 0, made, keptTimes, stretched[channel]);
            }
            report.keys += static_cast<u32>(made.times.size());
            keep.push_back(KeptKeys{clipIndex, channel, std::move(keptTimes)});
            place(channel, std::move(made), home, source != nullptr);
        }
    }
    // A constraint or IK stage over a node the bake writes runs again over its
    // keys at play, and a Link carries them a second time: off wherever the
    // bake writes one of its nodes, as it was everywhere else, stepped. On
    // again a millisecond after the last key written, where `Splice` put the
    // source's own.
    std::vector<u32> weights;
    for (const PoseStage& stage : owner.poseStages) {
        if (!stage.enabled || IsPhysicsStage(stage.kind) || stage.driven.empty()) {
            continue;
        }
        std::vector<u8> off(steps + 1, 0);
        bool any = false;
        for (const u32 node : stage.driven) {
            if (const auto found = writes.find(node); found != writes.end()) {
                for (u32 i = 0; i <= steps; ++i) {
                    off[i] = off[i] | found->second[i];
                    any = any || found->second[i] != 0;
                }
            }
        }
        if (!any) {
            continue;
        }
        u32 channel = kInvalidIndex;
        if (const AnimChannel* weight = StageWeightChannel(owner, stage)) {
            channel = weight->id;
            own(channel);
        } else {
            AnimChannel made;
            made.id = owner.animChannels.nextFreeId();
            made.target.kind = TrackTarget::Kind::Node;
            made.target.node = stage.driven.front();
            made.target.channel = Channel::StageWeight;
            made.target.sub = stage.id;
            made.valueType = geom::AttrType::F32;
            channel = owner.animChannels.add(made);
            appended.push_back(channel);
        }
        u32 home = 0;
        const SubTrack* source = nullptr;
        for (u32 c = 0; c < clip.containers.size() && source == nullptr; ++c) {
            if (const SubTrack* found = clip.containers[c].find(channel); found != nullptr) {
                home = c;
                source = found;
            }
        }
        std::vector<i32> timesMs(steps + 1);
        for (u32 i = 0; i <= steps; ++i) {
            timesMs[i] = static_cast<i32>(Milliseconds(static_cast<f32>(i) * dt));
        }
        const f32 one = 1.0f;
        const std::vector<u8> was =
            source != nullptr ? SampleSubTrackBatch(clip, *source, geom::AttrType::F32, timesMs,
                                                    std::span<const u8>(reinterpret_cast<const u8*>(&one), sizeof one))
                              : std::vector<u8>{};
        SubTrack made;
        made.channel = channel;
        made.interp = Interpolation::Step;
        f32 last = -1.0f;
        for (u32 i = 0; i <= steps; ++i) {
            f32 w = one;
            if (off[i] != 0) {
                w = 0.0f;
            } else if ((i + 1) * sizeof(f32) <= was.size()) {
                std::memcpy(&w, was.data() + i * sizeof(f32), sizeof(f32));
            }
            if (i == 0 || w != last) {
                const bool resumes = i > 0 && off[i] == 0 && off[i - 1] != 0;
                made.times.push_back(resumes ? static_cast<f32>(Milliseconds(static_cast<f32>(i - 1) * dt) + 1) / 1000.0f
                                             : static_cast<f32>(i) * dt);
                const u8* bytes = reinterpret_cast<const u8*>(&w);
                made.values.insert(made.values.end(), bytes, bytes + sizeof(f32));
                last = w;
            }
        }
        place(channel, std::move(made), home, source != nullptr);
        weights.push_back(channel);
        // Off is the bake's, and the key that turns it on again.
        std::vector<Stretch>& own = stretched[channel];
        for (u32 i = 0; i <= steps; ++i) {
            if (off[i] == 0) {
                continue;
            }
            u32 b = i;
            while (b < steps && off[b + 1] != 0) {
                ++b;
            }
            Stretch stretch{static_cast<f32>(i) * dt - kStretchPad, static_cast<f32>(i) * dt, kEver};
            if (i == 0) {
                stretch.from = -kEver;
            }
            if (b < steps) {
                stretch.to = static_cast<f32>(Milliseconds(static_cast<f32>(b) * dt) + 1) / 1000.0f + kStretchPad;
            }
            own.push_back(stretch);
            i = b;
        }
    }
    // A *Full detail* cloth's drivers (cloth design §10.3): each free
    // particle's frame relative to the cloth's holder, step by step, the
    // recording's where the cloth is on and its anchors' where it is off, on
    // the cloth's own channels, which an Unbake takes away.
    for (u32 c = 0; c < run.cloths.size() && c < run.clothParticles.size(); ++c) {
        const Cloth* cloth = owner.physics.cloth(run.cloths[c]);
        if (cloth == nullptr || !BakesFullDetail(*cloth) || cloth->cage.mesh >= owner.meshes.size()) {
            continue;
        }
        const ClothDrivers drivers = ClothDriversOf(owner, *cloth);
        u32 first = 0;
        for (u32 k = 0; k < c; ++k) {
            first += run.clothParticles[k];
        }
        std::vector<u32> particle(drivers.vertices.size(), kInvalidIndex);
        for (u32 k = 0; k < run.clothParticles[c] && first + k < run.clothVertices.size(); ++k) {
            const auto at =
                std::lower_bound(drivers.vertices.begin(), drivers.vertices.end(), run.clothVertices[first + k]);
            if (at != drivers.vertices.end() && *at == run.clothVertices[first + k]) {
                particle[static_cast<std::size_t>(at - drivers.vertices.begin())] = first + k;
            }
        }
        const Mesh& mesh = owner.meshes[cloth->cage.mesh];
        std::vector<std::vector<Vector3f>> moves(drivers.vertices.size());
        std::vector<std::vector<Quaternion>> turns(drivers.vertices.size());
        // The bones alone, each track prepared once: a cloth baked before
        // this one left two tracks a driver in the clip.
        Animator animator(document, model);
        animator.keepTracksPrepared();
        for (u32 i = 0; i <= steps; ++i) {
            const u32 at = run.first + i;
            Mix mix;
            mix.plays.push_back(Play{clipIndex, static_cast<f32>(i) * dt, 1.0f, false});
            mix.globals = false;
            Pose pose;
            animator.sample(mix, pose, true);
            animator.compose(pose);
            RecordedPose(document, run, at, pose);
            const Matrix44f holder =
                drivers.holder < pose.frame.size() ? pose.frame[drivers.holder] : Matrix44f::identity();
            const Matrix44f back = Matrix44f::inverse(holder);
            const ClothParticleFrame* recorded = run.clothFramesAt(at);
            const bool active = run.clothActiveAt(at, c);
            for (u32 d = 0; d < drivers.vertices.size(); ++d) {
                Matrix44f frame;
                if (active && recorded != nullptr && particle[d] != kInvalidIndex) {
                    const ClothParticleFrame& p = recorded[particle[d]];
                    frame = ToMatrix(Transform{p.position, p.rotation, Vector3f{1, 1, 1}});
                } else {
                    frame = AnchoredFrame(mesh, drivers.vertices[d], pose);
                }
                const Transform local = FromMatrix(frame * back);
                Quaternion q = local.rotation;
                if (!turns[d].empty() && q.dot(turns[d].back()) < 0.0f) {
                    q = q * -1.0f;
                }
                moves[d].push_back(local.translation);
                turns[d].push_back(q);
            }
        }
        if (clip.containers.empty()) {
            clip.containers.emplace_back();
        }
        const f32 tolerance = settings.tolerance * height;
        std::vector<u32> keys;
        for (u32 d = 0; d < drivers.vertices.size(); ++d) {
            for (const Channel which : {Channel::ClothDriverTranslation, Channel::ClothDriverRotation}) {
                const bool move = which == Channel::ClothDriverTranslation;
                if (FindClothDriverChannel(owner, cloth->id, d, which) == kInvalidIndex) {
                    appended.push_back(ClothDriverChannel(owner, cloth->id, d, which));
                }
                const u32 channel = ClothDriverChannel(owner, cloth->id, d, which);
                record.channels.push_back(channel);
                ThinDriver(moves[d], turns[d], move, tolerance, keys);
                SubTrack made;
                made.channel = channel;
                made.interp = move ? Interpolation::Linear : Interpolation::Slerp;
                for (const u32 i : keys) {
                    made.times.push_back(static_cast<f32>(i) * dt);
                    const u8* bytes = move ? reinterpret_cast<const u8*>(&moves[d][i])
                                           : reinterpret_cast<const u8*>(&turns[d][i]);
                    made.values.insert(made.values.end(), bytes,
                                       bytes + (move ? sizeof(Vector3f) : sizeof(Quaternion)));
                }
                report.keys += static_cast<u32>(made.times.size());
                clip.containers.front().subTracks.push_back(std::move(made));
            }
        }
        report.cloths.push_back({cloth->id, static_cast<u32>(drivers.vertices.size()), 0.0f,
                                 c < run.clothSize.size() ? run.clothSize[c] : 0.0f});
    }
    // Fewer keys, only among those the bake wrote (§8.3). A bake that wrote
    // none is still recorded, so the clip reads baked.
    if (settings.tolerance > 0.0f && !record.channels.empty() && deferred != nullptr) {
        deferred->clip = clipIndex;
        deferred->channels = record.channels;
        deferred->kept = std::move(keep);
        deferred->tolerance = settings.tolerance;
        deferred->mesh = settings.meshMeasure;
    } else if (settings.tolerance > 0.0f && !record.channels.empty()) {
        KeyReduceOptions options;
        options.heightShare = settings.tolerance;
        options.measure = settings.meshMeasure ? KeyReduceOptions::Measure::Mesh : KeyReduceOptions::Measure::Skeleton;
        options.clips = {clipIndex};
        options.channels = record.channels;
        options.kept = std::move(keep);
        options.threads = 1;
        KeyReduceReport reduced = ReduceKeys(document, model, options);
        report.diagnostics.append(reduced.diagnostics);
        report.keys = 0;
        for (const SubTrackContainer& container : document.clips[clipIndex].containers) {
            for (const SubTrack& track : container.subTracks) {
                if (std::find(record.channels.begin(), record.channels.end(), track.channel) != record.channels.end()) {
                    report.keys += static_cast<u32>(track.times.size());
                }
            }
        }
        // As the reduction measured it, against the keys it was given: sampling
        // them again costs as much as the reduction.
        report.maxError = reduced.maxError;
    }

    // The soft bodies, last (soft design §11.1): stepped on the clip as the
    // keys above now play it. A copy's and a surface lump's tracks are the
    // body's own; a bone used as it is is keyed in place, scale included, and
    // only where its lump departs.
    std::vector<u32> lumpChannels;
    {
        softdetail::SoftPass pass =
            softdetail::RunSoftPass(document, clipIndex, steps, dt, settings.tolerance * height, stride, softModes);
        report.soft = std::move(pass.lines);
        appended.insert(appended.end(), pass.declared.begin(), pass.declared.end());
        if (!pass.tracks.empty() && clip.containers.empty()) {
            clip.containers.emplace_back();
            replaced.emplace_back();
            before.emplace_back();
            record.source.emplace_back();
        }
        for (SubTrack& track : pass.tracks) {
            own(track.channel);
            lumpChannels.push_back(track.channel);
            clip.containers.front().subTracks.push_back(std::move(track));
        }
        // A third of the accuracy for each of a bone's three tracks, as a
        // lump's own get.
        const f32 softNoise = settings.tolerance / 3.0f;
        for (const softdetail::SoftBoneKeys& bone : pass.bones) {
            if (global.count(bone.node) != 0) {
                report.globalLoops.push_back(bone.node);
                continue;
            }
            std::vector<std::pair<u32, u32>> spans;
            for (u32 i = 0; i <= steps; ++i) {
                if (bone.departs[i] == 0) {
                    continue;
                }
                if (!spans.empty() && spans.back().second + 1 >= i) {
                    spans.back().second = i;
                } else {
                    spans.emplace_back(i, i);
                }
            }
            u32 keys = 0;
            for (const Channel which : {Channel::Rotation, Channel::Translation, Channel::Scale}) {
                if ((which == Channel::Rotation && !bone.turns) || (which == Channel::Translation && !bone.moves) ||
                    (which == Channel::Scale && !bone.scales)) {
                    continue;
                }
                const bool quat = which == Channel::Rotation;
                u32 channel = FindNodeChannel(owner, bone.node, which);
                if (channel == kInvalidIndex) {
                    AnimChannel made;
                    made.id = owner.animChannels.nextFreeId();
                    made.target.kind = TrackTarget::Kind::Node;
                    made.target.node = bone.node;
                    made.target.channel = which;
                    made.valueType = quat ? geom::AttrType::Quat : geom::AttrType::F32x3;
                    channel = owner.animChannels.add(made);
                    appended.push_back(channel);
                } else {
                    own(channel);
                }
                const AnimChannel* declared = owner.animChannels.find(channel);
                if (declared->valueType != (quat ? geom::AttrType::Quat : geom::AttrType::F32x3)) {
                    continue; // an Euler turn or a single-float scale is not the bake's to rewrite
                }
                // Written above for a body on the same bone: its keys are the
                // source here, and its own source is already kept.
                const bool again =
                    std::find(record.channels.begin(), record.channels.end(), channel) != record.channels.end();
                u32 home = 0;
                const SubTrack* source = nullptr;
                for (u32 c = 0; c < clip.containers.size(); ++c) {
                    if (const SubTrack* found = clip.containers[c].find(channel); found != nullptr && source == nullptr) {
                        home = c;
                        source = found;
                    }
                }
                SubTrack original;
                if (source != nullptr) {
                    original = *source;
                    if (!original.tcb.empty()) {
                        ++report.restated;
                    }
                }
                Interpolation interp = quat ? Interpolation::Slerp : Interpolation::Linear;
                if (source != nullptr) {
                    interp = source->interp;
                } else if (const std::optional<Interpolation> elsewhere =
                               ControllerElsewhere(document, model, clipIndex, channel)) {
                    interp = *elsewhere;
                }
                SubTrack made;
                made.channel = channel;
                std::vector<f32> keptTimes;
                // A lump's keys are the channel's whole: no stretch of it is kept.
                std::vector<Stretch> whole;
                stretched.erase(channel);
                if (quat) {
                    std::vector<Quaternion> plays(steps + 1);
                    for (u32 i = 0; i <= steps; ++i) {
                        plays[i] = bone.plays[i].rotation;
                    }
                    Splice<Quaternion>(source != nullptr ? &original : nullptr, interp, bone.samples, spans, dt, stride,
                                       softNoise, height, plays, false, made, keptTimes, whole);
                } else {
                    // `Splice` reads a translation: a scale goes through it as
                    // one, measured by what it moves a point a reach away.
                    const bool scale = which == Channel::Scale;
                    std::vector<Transform> samples = bone.samples;
                    std::vector<Vector3f> plays(steps + 1);
                    for (u32 i = 0; i <= steps; ++i) {
                        if (scale) {
                            samples[i].translation = bone.samples[i].scale;
                        }
                        plays[i] = scale ? bone.plays[i].scale : bone.plays[i].translation;
                    }
                    Splice<Vector3f>(source != nullptr ? &original : nullptr, interp, samples, spans, dt, stride, softNoise,
                                     scale ? height / std::max(bone.reach, 1e-6f) : height, plays, false, made, keptTimes,
                                     whole);
                }
                keys += static_cast<u32>(made.times.size());
                if (again) {
                    // The keys a body's bake left here are what the soft bodies
                    // were stepped on, and nothing else holds them.
                    if (source != nullptr && std::find(record.softChannels.begin(), record.softChannels.end(),
                                                       channel) == record.softChannels.end()) {
                        if (record.softSource.empty()) {
                            record.softSource.emplace_back();
                        }
                        record.softSource.front().subTracks.push_back(original);
                        record.softChannels.push_back(channel);
                    }
                    for (SubTrack& track : clip.containers[home].subTracks) {
                        if (track.channel == channel) {
                            track = std::move(made);
                            break;
                        }
                    }
                } else {
                    record.channels.push_back(channel);
                    record.softChannels.push_back(channel);
                    place(channel, std::move(made), home, source != nullptr);
                }
            }
            for (BakeReport::SoftLine& line : report.soft) {
                if (line.body == bone.body) {
                    line.keys += keys;
                }
            }
        }
    }
    // Container by container, in the order they stood: an Unbake puts each
    // back in turn.
    for (u32 c = 0; c < replaced.size(); ++c) {
        std::stable_sort(replaced[c].begin(), replaced[c].end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& [place, track] : replaced[c]) {
            record.positions.push_back(place);
            record.source[c].subTracks.push_back(std::move(track));
        }
    }

    // The seam after, as the keys now play it.
    {
        const Animator animator(document, model);
        Pose start, end;
        Mix a, b;
        a.plays.push_back(Play{clipIndex, 0.0f, 1.0f, false});
        b.plays.push_back(Play{clipIndex, document.clips[clipIndex].duration, 1.0f, false});
        animator.sample(a, start, true);
        animator.sample(b, end, true);
        f32 seamAfter = 0.0f;
        for (const Written& w : written) {
            if (w.node < start.local.size() &&
                std::find(w.active.begin(), w.active.end(), u8{1}) != w.active.end()) {
                seamAfter = std::max(seamAfter, seam(w.node, start.local[w.node].rotation, end.local[w.node].rotation));
            }
        }
        report.seamAfter = seamAfter;
    }
    report.seamBefore = seamBefore;
    // After the reduction: a stepped weight is not its to thin, and a lump's
    // tracks were thinned by their own measure.
    record.channels.insert(record.channels.end(), weights.begin(), weights.end());
    record.channels.insert(record.channels.end(), lumpChannels.begin(), lumpChannels.end());
    for (const BakeReport::SoftLine& line : report.soft) {
        record.softIds.push_back(line.body);
        record.softLumps.push_back(line.lumps);
        record.softKeys.push_back(line.keys);
        record.softWithin.push_back(line.within);
        record.softFollows.push_back(line.follows);
        record.softPush.push_back(line.push);
        record.softAtEnd.push_back(line.atEnd);
    }
    record.nodes = report.nodes;
    record.keys = report.keys;
    record.restated = report.restated;
    record.maxError = report.maxError;
    record.seamBefore = report.seamBefore;
    record.seamAfter = report.seamAfter;
    record.appended = std::move(appended);
    for (u32 c = 0; c < run.clothBones.size() && c < run.cloths.size(); ++c) {
        if (!run.clothBones[c].empty()) {
            report.cloths.push_back({run.cloths[c], static_cast<u32>(run.clothBones[c].size()),
                                     c < run.clothFit.size() ? run.clothFit[c] : 0.0f,
                                     c < run.clothSize.size() ? run.clothSize[c] : 0.0f});
        }
    }
    for (const BakeReport::ClothLine& line : report.cloths) {
        record.clothIds.push_back(line.cloth);
        record.clothFits.push_back(line.fit);
        record.clothSizes.push_back(line.size);
    }
    // Where it keyed each node's channel and each weight, and the clip's own
    // keys beside them (§8.4).
    for (const auto& [channel, stretches] : stretched) {
        if (stretches.empty() ||
            std::find(record.channels.begin(), record.channels.end(), channel) == record.channels.end()) {
            continue;
        }
        record.stretchChannels.push_back(channel);
        record.stretchCounts.push_back(static_cast<u32>(stretches.size()));
        for (const Stretch& stretch : stretches) {
            record.stretchTimes.insert(record.stretchTimes.end(), {stretch.from, stretch.first, stretch.to});
        }
        record.stretchOutside.push_back(OutsideHash(document.clips[clipIndex], channel, stretches));
    }
    record.written = WrittenHash(document.clips[clipIndex], record);
    record.stretchInside = InsideHash(document.clips[clipIndex], record);
    Clip& out = document.clips[clipIndex];
    // Settings added here only hold the record: the clip is no more set up
    // than it was, and an Unbake takes them with it.
    record.madeSettings = !out.physics.has_value();
    if (!out.physics.has_value()) {
        out.physics.emplace();
    }
    out.physics->baked = std::move(record);
    report.ok = true;
    return report;
}

/// @p clip's record after its deferred reduction: what it keyed, hashed again
/// as it now stands, and the reduction's count and error.
void StampReduced(Document& document, u32 clip, f32 maxError) {
    Clip& out = document.clips[clip];
    if (!out.physics.has_value() || !out.physics->baked.has_value()) {
        return;
    }
    PhysicsBake& record = *out.physics->baked;
    record.keys = 0;
    for (const SubTrackContainer& container : out.containers) {
        for (const SubTrack& track : container.subTracks) {
            if (std::find(record.channels.begin(), record.channels.end(), track.channel) != record.channels.end()) {
                record.keys += static_cast<u32>(track.times.size());
            }
        }
    }
    record.maxError = maxError;
    std::vector<Stretch> stretches;
    for (std::size_t k = 0; k < record.stretchChannels.size() && k < record.stretchOutside.size(); ++k) {
        stretches.clear();
        if (StretchesOf(record, record.stretchChannels[k], stretches).has_value() && !stretches.empty()) {
            record.stretchOutside[k] = OutsideHash(out, record.stretchChannels[k], stretches);
        }
    }
    record.written = WrittenHash(out, record);
    record.stretchInside = InsideHash(out, record);
}

} // namespace

BakeReport BakeClip(Document& document, const BakeRun& run, const BakeRun* goesTo, SoftModesCache* softModes) {
    return BakeClipInto(document, run, goesTo, softModes, nullptr);
}

std::vector<u32> BakeOrder(const Document& document, std::vector<u32> clips) {
    std::vector<u32> out;
    std::set<u32> placed;
    const auto wanted = [&](u32 c) { return std::find(clips.begin(), clips.end(), c) != clips.end(); };
    // Depth first: what a clip reads goes before it; a cycle is cut where it
    // comes back.
    std::set<u32> visiting;
    const auto visit = [&](auto&& self, u32 c) -> void {
        if (placed.count(c) != 0 || visiting.count(c) != 0) {
            return;
        }
        visiting.insert(c);
        for (const u32 before : BakeChain(document, c)) {
            if (wanted(before)) {
                self(self, before);
            }
        }
        const u32 to = BakeGoesTo(document, c);
        if (to != kInvalidIndex && wanted(to)) {
            self(self, to);
        }
        visiting.erase(c);
        placed.insert(c);
        out.push_back(c);
    };
    for (const u32 c : clips) {
        visit(visit, c);
    }
    return out;
}

void UnbakeClip(Document& document, u32 clip, bool dropChannels) {
    if (clip >= document.clips.size()) {
        return;
    }
    Clip& target = document.clips[clip];
    if (!target.physics.has_value() || !target.physics->baked.has_value()) {
        return;
    }
    const std::vector<u32> appended = target.physics->baked->appended;
    const bool madeSettings = target.physics->baked->madeSettings;
    RestoreSource(target);
    target.physics->baked.reset();
    if (madeSettings) {
        target.physics.reset();
    }
    // A channel the bake declared goes once nothing keys it: another clip's
    // bake may have keyed it since.
    const u32 model = target.model;
    if (!dropChannels || model >= document.models.size()) {
        return;
    }
    AnimChannelTable& table = document.models[model].animChannels;
    for (const u32 id : appended) {
        const bool used = std::any_of(document.clips.begin(), document.clips.end(), [&](const Clip& c) {
            return c.model == model && std::any_of(c.containers.begin(), c.containers.end(), [&](const SubTrackContainer& k) {
                       return k.find(id) != nullptr;
                   });
        });
        if (!used) {
            std::erase_if(table.channels, [&](const AnimChannel& channel) { return channel.id == id; });
        }
    }
}

std::vector<u32> UnbakedSetUpClips(const Document& document) {
    std::vector<u32> out;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        const bool baked = clip.physics.has_value() && clip.physics->baked.has_value();
        if (!baked && !IsGlobalLoop(clip) && (clip.physics.has_value() || SoftActsIn(document, c))) {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<u32> UnbakedSoftClips(const Document& document) {
    std::vector<u32> out;
    for (const u32 c : UnbakedSetUpClips(document)) {
        if (SoftActsIn(document, c)) {
            out.push_back(c);
        }
    }
    return out;
}

namespace {

/// Whether the rule hands a body with a shape to the simulation somewhere in
/// @p clip (`SwitchScope::Clip`): read at 0 and at each switch key, the only
/// times a state changes. A body with no shape follows its bone.
bool BodiesActIn(const Document& document, u32 clip) {
    const Clip& source = document.clips[clip];
    if (source.model >= document.models.size()) {
        return false;
    }
    const Model& model = document.models[source.model];
    const std::vector<PhysicsBody>& bodies = model.physics.bodies;
    if (std::all_of(bodies.begin(), bodies.end(), [](const PhysicsBody& body) { return body.shapes.empty(); })) {
        return false;
    }
    std::vector<f32> times{0.0f};
    for (const SubTrackContainer& container : source.containers) {
        for (const SubTrack& track : container.subTracks) {
            const AnimChannel* channel = model.animChannels.find(track.channel);
            if (channel == nullptr || channel->target.kind != TrackTarget::Kind::Physics ||
                (channel->target.channel != Channel::PhysicsDynamic && channel->target.channel != Channel::PhysicsRagdoll)) {
                continue;
            }
            for (const f32 t : track.times) {
                if (t > 0.0f && t <= source.duration) {
                    times.push_back(t);
                }
            }
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    const SwitchReader reader(document, source.model, clip, SwitchScope::Clip);
    std::vector<BodySwitch> previous;
    std::vector<BodySwitch> now;
    for (const f32 t : times) {
        reader.read(t, previous, now);
        for (std::size_t b = 0; b < now.size() && b < bodies.size(); ++b) {
            if (now[b].active && !bodies[b].shapes.empty()) {
                return true;
            }
        }
        previous.swap(now);
    }
    return false;
}

/// Whether a cloth draws somewhere in @p clip: read at 0 and at each of its
/// *Active* keys, the only times that changes.
bool ClothsActIn(const Document& document, u32 clip) {
    const Clip& source = document.clips[clip];
    if (source.model >= document.models.size() || document.models[source.model].physics.cloths.empty()) {
        return false;
    }
    const Model& model = document.models[source.model];
    std::vector<f32> times{0.0f};
    for (const SubTrackContainer& container : source.containers) {
        for (const SubTrack& track : container.subTracks) {
            const AnimChannel* channel = model.animChannels.find(track.channel);
            if (channel != nullptr && channel->target.kind == TrackTarget::Kind::Physics &&
                channel->target.channel == Channel::ClothActive) {
                times.insert(times.end(), track.times.begin(), track.times.end());
            }
        }
    }
    const SwitchReader reader(document, source.model, clip, SwitchScope::Clip);
    for (u32 c = 0; c < model.physics.cloths.size(); ++c) {
        for (const f32 t : times) {
            if (t >= 0.0f && t <= source.duration && reader.clothActive(c, t)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::vector<u32> UnbakedPhysicsClips(const Document& document) {
    std::vector<u32> out;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        const bool baked = clip.physics.has_value() && clip.physics->baked.has_value();
        if (!baked && !IsGlobalLoop(clip) &&
            (clip.physics.has_value() || SoftActsIn(document, c) || BodiesActIn(document, c) ||
             ClothsActIn(document, c))) {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<u32> OutOfDateBakes(const Document& document) {
    std::vector<u32> out;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.physics.has_value() && clip.physics->baked.has_value() && !BakeInDate(document, c)) {
            out.push_back(c);
        }
    }
    return out;
}

u64 BakeWrittenNow(const Document& document, u32 clip) {
    if (clip >= document.clips.size() || !document.clips[clip].physics || !document.clips[clip].physics->baked) {
        return 0;
    }
    return WrittenHash(document.clips[clip], *document.clips[clip].physics->baked);
}

bool BakedKeysEdited(const Document& document, u32 clip) {
    if (clip >= document.clips.size() || !document.clips[clip].physics || !document.clips[clip].physics->baked) {
        return false;
    }
    const PhysicsBake& record = *document.clips[clip].physics->baked;
    // Its own keys, where the record tells them from the clip's.
    if (record.stretchInside != 0) {
        return InsideHash(document.clips[clip], record) != record.stretchInside;
    }
    return WrittenHash(document.clips[clip], record) != record.written;
}

void StampBakeWritten(Document& document, u32 clip) {
    if (clip >= document.clips.size() || !document.clips[clip].physics || !document.clips[clip].physics->baked) {
        return;
    }
    PhysicsBake& record = *document.clips[clip].physics->baked;
    record.written = WrittenHash(document.clips[clip], record);
    if (record.stretchInside != 0) {
        record.stretchInside = InsideHash(document.clips[clip], record);
    }
}

namespace {

/// The stretches an older bake keyed on a node's channel, read off @p now's
/// keys: a key @p source does not hold is the bake's, with every key between
/// two of them, and a source key it left out is in the stretch nearest it.
template <class T>
std::vector<Stretch> ReadStretches(const SubTrack& now, const SubTrack* source, f32 duration) {
    const std::vector<CurveKey<T>> ours = DecodeKeys<T>(now);
    const std::vector<CurveKey<T>> theirs = source != nullptr ? DecodeKeys<T>(*source) : std::vector<CurveKey<T>>{};
    const auto held = [&](const CurveKey<T>& key) {
        return std::any_of(theirs.begin(), theirs.end(), [&](const CurveKey<T>& other) {
            return other.time == key.time && std::memcmp(&other.value, &key.value, sizeof(T)) == 0;
        });
    };
    std::vector<Stretch> out;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        if (held(ours[i])) {
            continue;
        }
        std::size_t last = i;
        while (last + 1 < ours.size() && !held(ours[last + 1])) {
            ++last;
        }
        Stretch stretch;
        stretch.from = i == 0 ? -kEver : ours[i].time - kStretchPad;
        stretch.to = last + 1 == ours.size() && ours[last].time >= duration - 0.001f ? kEver
                                                                                     : ours[last].time + kStretchPad;
        // An edge key leads the simulation's, where the clip's own go on before it.
        stretch.first = last == i ? stretch.from - 1.0f : ours[i == 0 ? i : i + 1].time;
        out.push_back(stretch);
        i = last;
    }
    for (const CurveKey<T>& key : theirs) {
        if (out.empty() || Within(out, key.time) ||
            std::any_of(ours.begin(), ours.end(), [&](const CurveKey<T>& other) { return other.time == key.time; })) {
            continue;
        }
        Stretch* nearest = &out.front();
        const auto apart = [&](const Stretch& s) { return std::max(s.from - key.time, key.time - s.to); };
        for (Stretch& s : out) {
            if (apart(s) < apart(*nearest)) {
                nearest = &s;
            }
        }
        nearest->from = std::min(nearest->from, key.time - kStretchPad);
        nearest->to = std::max(nearest->to, key.time + kStretchPad);
    }
    return out;
}

} // namespace

void MarkBakeStretches(Document& document) {
    std::vector<Stretch> stretches;
    for (Clip& clip : document.clips) {
        if (!clip.physics.has_value() || !clip.physics->baked.has_value() || clip.model >= document.models.size()) {
            continue;
        }
        PhysicsBake& record = *clip.physics->baked;
        if (record.stretchInside != 0) {
            continue;
        }
        // Keys edited since cannot be told from the bake's, channel by channel.
        const bool edited = WrittenHash(clip, record) != record.written;
        const AnimChannelTable& table = document.models[clip.model].animChannels;
        for (const u32 channel : record.channels) {
            const AnimChannel* declared = table.find(channel);
            if (declared == nullptr || declared->target.kind != TrackTarget::Kind::Node ||
                std::find(record.softChannels.begin(), record.softChannels.end(), channel) != record.softChannels.end() ||
                StretchesOf(record, channel, stretches).has_value()) {
                continue;
            }
            const bool turns =
                declared->target.channel == Channel::Rotation && declared->valueType == geom::AttrType::Quat;
            const bool moves =
                declared->target.channel == Channel::Translation && declared->valueType == geom::AttrType::F32x3;
            // One layer's keys, as `OwnAnimations` puts them back.
            const SubTrack* now = nullptr;
            const SubTrack* source = nullptr;
            u32 layers = 0;
            for (const SubTrackContainer& container : clip.containers) {
                if (const SubTrack* found = container.find(channel); found != nullptr && now == nullptr) {
                    now = found;
                }
            }
            for (const SubTrackContainer& container : record.source) {
                if (const SubTrack* found = container.find(channel); found != nullptr) {
                    source = found;
                    ++layers;
                }
            }
            // Every key of it taken away since: the bake's are gone, and what
            // it replaced is not put back under them. The track is the clip's
            // own from here on, with no stretch.
            const bool gone = edited && (now == nullptr || now->times.empty());
            if ((!turns && !moves) || layers > 1 || (!gone && (now == nullptr || now->times.empty())) ||
                (!gone && !now->wellSized(declared->valueType)) ||
                (source != nullptr && !source->wellSized(declared->valueType))) {
                continue;
            }
            stretches.clear();
            if (!gone) {
                stretches = turns ? ReadStretches<Quaternion>(*now, source, clip.duration)
                                  : ReadStretches<Vector3f>(*now, source, clip.duration);
                if (stretches.empty()) {
                    continue;
                }
            }
            record.stretchChannels.push_back(channel);
            record.stretchCounts.push_back(static_cast<u32>(stretches.size()));
            for (const Stretch& stretch : stretches) {
                record.stretchTimes.insert(record.stretchTimes.end(), {stretch.from, stretch.first, stretch.to});
            }
            // 0 is no track's keys: one with no stretch is never as the bake left it.
            record.stretchOutside.push_back(gone ? 0 : OutsideHash(clip, channel, stretches));
        }
        record.stretchInside = InsideHash(clip, record);
        // Edited before it was read: it goes on reading so.
        if (edited) {
            record.stretchInside = ~record.stretchInside != 0 ? ~record.stretchInside : 1;
        }
    }
}

namespace {

/// @p count jobs over @p threads threads, each job handed the index of the
/// thread that takes it.
void ForEachOn(std::size_t count, u32 threads, const std::function<void(std::size_t job, u32 thread)>& work) {
    if (count == 0) {
        return;
    }
    const u32 used = static_cast<u32>(std::min<std::size_t>(std::max<u32>(threads, 1), count));
    if (used == 1) {
        for (std::size_t job = 0; job < count; ++job) {
            work(job, 0);
        }
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (u32 t = 0; t < used; ++t) {
        pool.emplace_back([&, t] {
            for (std::size_t job = next++; job < count; job = next++) {
                work(job, t);
            }
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
}

u32 BakeForExportOn(Document& document, const std::function<BakeHooks()>& hooksFor, u32 runThreads,
                    u32 reduceThreads, std::span<const std::shared_ptr<const BakeRun>> runs, Diagnostics& out,
                    const std::vector<u32>* only, const BakeExportWatch* watch) {
    const std::vector<u32> order = BakeOrder(document, only != nullptr ? *only : UnbakedPhysicsClips(document));
    if (order.empty()) {
        return 0;
    }
    const auto stopped = [watch] { return watch != nullptr && watch->cancelled && watch->cancelled(); };
    std::mutex said;
    const auto say = [&](bool reducing, u32 done, u32 total) {
        if (watch != nullptr && watch->progress) {
            const std::lock_guard<std::mutex> lock(said);
            watch->progress(reducing, done, total);
        }
    };
    // A run reads every clip from its bake's source, which a bake here leaves
    // as it was: one copy with the sources put back serves every run, on any
    // thread. Settings a bake added go with their record, as `IsSetUp` reads
    // such a clip; the caches a first read fills are filled before the threads.
    Document sources = document;
    RestoreBakeSources(sources);
    for (Clip& clip : sources.clips) {
        if (clip.physics.has_value() && clip.physics->baked.has_value() && clip.physics->baked->madeSettings) {
            clip.physics.reset();
        } else if (clip.physics.has_value()) {
            clip.physics->baked.reset();
        }
    }
    for (const Model& model : sources.models) {
        model.nodes.ensureHierarchy();
        for (const Mesh& mesh : model.meshes) {
            (void)mesh.faceSet();
        }
    }
    // A bake reads the keys of the clip it is matched into as they stand
    // (§7.4), and a soft body's pass the clip's own: such a clip is reduced as
    // soon as it is baked, as a bake alone reduces it, and every other at the
    // end, side by side, each its own keys.
    std::set<u32> readLater;
    for (const u32 clip : order) {
        if (const u32 to = BakeLoops(document.clips[clip]) ? kInvalidIndex : BakeGoesTo(document, clip);
            to < document.clips.size()) {
            readLater.insert(to);
        }
    }
    const u32 threads = std::max<u32>(runThreads, 1);
    std::vector<BakeHooks> hooks;
    for (u32 t = 0; t < threads; ++t) {
        hooks.push_back(hooksFor());
    }
    // The editor's recording of @p clip, made from @p inputs; any complete
    // one with no inputs asked.
    const auto recorded = [&](u32 clip, std::optional<u64> inputs) -> const BakeRun* {
        const BakeRun* found = nullptr;
        for (const std::shared_ptr<const BakeRun>& run : runs) {
            if (run != nullptr && run->clip == clip && run->complete && (!inputs || run->inputs == *inputs)) {
                found = run.get();
            }
        }
        return found;
    };
    std::vector<DeferredReduction> deferred;
    u32 baked = 0;
    u32 done = 0;
    // A few runs a thread at a time, made side by side and then baked in order.
    const std::size_t batch = static_cast<std::size_t>(threads) * 2;
    for (std::size_t from = 0; from < order.size() && !stopped(); from += batch) {
        const std::size_t to = std::min(order.size(), from + batch);
        std::vector<std::optional<BakeRun>> fresh(to - from);
        std::vector<std::size_t> needed;
        for (std::size_t i = from; i < to; ++i) {
            if (recorded(order[i], std::nullopt) == nullptr) {
                needed.push_back(i - from);
            }
        }
        ForEachOn(needed.size(), threads, [&](std::size_t job, u32 thread) {
            if (stopped()) {
                return;
            }
            const std::size_t k = needed[job];
            fresh[k] = SimulateClip(sources, order[from + k], hooks[thread]);
        });
        for (std::size_t i = from; i < to && !stopped(); ++i) {
            say(false, done++, static_cast<u32>(order.size()));
            const u32 clip = order[i];
            const u64 inputs = BakeInputs(document, clip);
            const BakeRun* use = recorded(clip, inputs);
            if (use == nullptr) {
                std::optional<BakeRun>& run = fresh[i - from];
                if (!run.has_value()) {
                    run = SimulateClip(sources, clip, hooks.front());
                }
                run->inputs = inputs;
                use = &*run;
            }
            const ElementRef where(ElementKind::Clip, clip);
            if (!use->complete) {
                out.warn(DiagCode::AnimStageNotBaked,
                         "clip '" + document.clips[clip].name + "': its physics could not be run: " + use->error, where);
                continue;
            }
            DeferredReduction later;
            const bool now = readLater.count(clip) != 0 || SoftActsIn(document, clip);
            BakeReport report = BakeClipInto(document, *use, nullptr, nullptr, now ? nullptr : &later);
            out.append(report.diagnostics);
            if (!report.ok) {
                out.warn(DiagCode::AnimStageNotBaked,
                         "clip '" + document.clips[clip].name + "': its physics could not be baked: " + report.error, where);
                continue;
            }
            ++baked;
            if (later.clip != kInvalidIndex) {
                deferred.push_back(std::move(later));
            }
        }
    }
    say(false, done, static_cast<u32>(order.size()));
    // One reduction per model and setting, its clips side by side.
    const u32 reducing = static_cast<u32>(deferred.size());
    u32 reducedClips = 0;
    while (!deferred.empty() && !stopped()) {
        const u32 model = document.clips[deferred.front().clip].model;
        const f32 tolerance = deferred.front().tolerance;
        const bool mesh = deferred.front().mesh;
        KeyReduceOptions options;
        options.heightShare = tolerance;
        options.measure = mesh ? KeyReduceOptions::Measure::Mesh : KeyReduceOptions::Measure::Skeleton;
        options.threads = reduceThreads;
        options.cancelled = stopped;
        options.progress = [&, before = reducedClips](u32 d, u32) { say(true, before + d, reducing); };
        std::vector<DeferredReduction> rest;
        for (DeferredReduction& d : deferred) {
            if (document.clips[d.clip].model != model || d.tolerance != tolerance || d.mesh != mesh) {
                rest.push_back(std::move(d));
                continue;
            }
            options.clips.push_back(d.clip);
            options.clipChannels.push_back(ClipChannels{d.clip, std::move(d.channels)});
            options.kept.insert(options.kept.end(), std::make_move_iterator(d.kept.begin()),
                                std::make_move_iterator(d.kept.end()));
        }
        deferred = std::move(rest);
        reducedClips += static_cast<u32>(options.clips.size());
        KeyReduceReport reduced = ReduceKeys(document, model, options);
        out.append(reduced.diagnostics);
        for (const KeyErrorRow& row : reduced.clips) {
            if (row.index < document.clips.size()) {
                StampReduced(document, row.index, row.maxError);
            }
        }
    }
    return baked;
}

} // namespace

u32 BakeForExport(Document& document, const BakeHooks& hooks, std::span<const std::shared_ptr<const BakeRun>> runs,
                  Diagnostics& out, const std::vector<u32>* only) {
    return BakeForExportOn(document, [&] { return hooks; }, 1, 0, runs, out, only, nullptr);
}

u32 BakeForExport(Document& document, const std::function<BakeHooks()>& hooksFor,
                  std::span<const std::shared_ptr<const BakeRun>> runs, Diagnostics& out,
                  const std::vector<u32>* only, u32 threads, const BakeExportWatch* watch) {
    // Sampling allocates as it goes, and past the cores the threads wait on
    // the heap more than they work (371 clips: 35 s on 16 threads, 44 on 32).
    const u32 count = threads != 0 ? threads : std::max(1u, std::thread::hardware_concurrency() / 2);
    return BakeForExportOn(document, hooksFor, count, count, runs, out, only, watch);
}

} // namespace wem
} // namespace models
} // namespace whiteout

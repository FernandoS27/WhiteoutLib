// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/bake.h>
#include <whiteout/models/wem/physics/cloth_bake.h>

#include <whiteout/models/wem/anim/clip.h>
#include <whiteout/models/wem/anim/curve_keys.h>
#include <whiteout/models/wem/anim/detach.h>
#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/anim/stages.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/anim/transition.h>
#include <whiteout/models/wem/reflect_bytes.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>

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

/// @p clip's baked channels' sub-tracks swapped back for what the bake
/// replaced, each in the place it had.
void RestoreSource(Clip& clip) {
    if (!clip.physics.has_value() || !clip.physics->baked.has_value()) {
        return;
    }
    const PhysicsBake& baked = *clip.physics->baked;
    const auto written = [&](u32 channel) {
        return std::find(baked.channels.begin(), baked.channels.end(), channel) != baked.channels.end();
    };
    std::size_t at = 0;
    for (std::size_t c = 0; c < clip.containers.size(); ++c) {
        std::vector<SubTrack>& tracks = clip.containers[c].subTracks;
        std::erase_if(tracks, [&](const SubTrack& track) { return written(track.channel); });
        if (c >= baked.source.size()) {
            continue;
        }
        // In ascending place, so each lands where it was.
        for (const SubTrack& track : baked.source[c].subTracks) {
            const u32 place = at < baked.positions.size() ? baked.positions[at] : static_cast<u32>(tracks.size());
            ++at;
            tracks.insert(tracks.begin() + std::min<std::size_t>(place, tracks.size()), track);
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

/// One stretch of a run: a clip from @p step for @p steps.
struct Piece {
    u32 clip = kInvalidIndex;
    u32 step = 0;
    u32 steps = 0;
    enum class Kind : u8 { Loops, Once, Held, Baked, Post } kind = Kind::Once;
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

} // namespace

u64 BakeInputs(const Document& document, u32 clip) {
    u64 hash = 1469598103934665603ull;
    if (clip >= document.clips.size() || document.clips[clip].model >= document.models.size()) {
        return hash;
    }
    const u32 owner = document.clips[clip].model;
    const Model& model = document.models[owner];
    Hash(hash, ReflectBytes(model.nodes));
    Hash(hash, ReflectBytes(model.physics));
    // A cloth's cage, pins, anchors and bindings are its mesh's (cloth design
    // §10.5).
    std::set<u32> clothMeshes;
    for (const Cloth& cloth : model.physics.cloths) {
        clothMeshes.insert(cloth.cage.mesh);
    }
    for (const u32 m : clothMeshes) {
        if (m < model.meshes.size()) {
            Hash(hash, ReflectBytes(model.meshes[m]));
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
        Hash(hash, ReflectBytes(source));
    }
    // What a once-played clip's end is matched into, as it stands: a Rebake of
    // it makes this clip out of date (§7.4).
    if (const u32 to = BakeLoops(document.clips[clip]) ? kInvalidIndex : BakeGoesTo(document, clip);
        to < document.clips.size()) {
        Clip target = document.clips[to];
        DetachClip(model.animChannels, target);
        Hash(hash, ReflectBytes(target));
    }
    return hash;
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
    const u32 rate = std::max<u32>(settings.stepRate, 15);
    run.steps = std::max<u32>(1, static_cast<u32>(std::ceil(baked.duration * static_cast<f32>(rate) - 1e-3f)));
    run.dt = baked.duration / static_cast<f32>(run.steps);
    const u32 preheat = std::min<u32>(settings.preheat, 20);
    const f32 dt = run.dt;
    const auto stepsOf = [&](f32 seconds) { return std::max<u32>(1, static_cast<u32>(std::lround(seconds / dt))); };

    // The pieces: what runs before frame 0, the clip, and any post-roll.
    std::vector<Piece> pieces;
    u32 at = 0;
    const auto add = [&](u32 c, u32 steps, Piece::Kind kind) {
        pieces.push_back(Piece{c, at, steps, kind});
        at += steps;
    };
    const bool loops = BakeLoops(baked);
    const BakeLeadIn from = BakeComesFrom(document, clip);
    if (from.kind == BakeFrom::Itself) {
        if (preheat > 0) {
            add(clip, preheat * run.steps, Piece::Kind::Loops);
        }
    } else if (from.kind == BakeFrom::FirstFrame) {
        if (preheat > 0) {
            add(clip, stepsOf(static_cast<f32>(preheat)), Piece::Kind::Held);
        }
    } else {
        const std::vector<u32> chain = BakeChain(document, clip);
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const Clip& lead = document.clips[chain[i]];
            if (i == 0 && BakeLoops(lead)) {
                add(chain[i], std::max<u32>(1, preheat) * stepsOf(lead.duration), Piece::Kind::Loops);
                continue;
            }
            if (i == 0 && preheat > 0) {
                add(chain[i], stepsOf(static_cast<f32>(preheat)), Piece::Kind::Held);
            }
            add(chain[i], stepsOf(lead.duration), Piece::Kind::Once);
        }
    }
    run.first = at;
    add(clip, run.steps + 1, Piece::Kind::Baked);
    // A loop matched at its start reads the cycle after it (§7.3).
    if (loops && settings.match == LoopMatch::Crossfade &&
        (settings.where == MatchWhere::Start || settings.where == MatchWhere::Both)) {
        const u32 window = std::max<u32>(1, static_cast<u32>(std::lround(settings.window * run.steps)));
        pieces.push_back(Piece{clip, at, window, Piece::Kind::Post});
        at += window;
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
    run.nodes.assign(nodes.begin(), nodes.end());
    hooks.reset(document, model, movable);
    std::vector<std::unique_ptr<ClothBoneFit>> fits;

    const Animator animator(document, model);
    const StageRunner stages(document, model);
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
        const f32 speed = own.travel ? ClipMoveSpeed(playing) : 0.0f;
        for (u32 k = 0; k < piece.steps; ++k) {
            if (cancel != nullptr && cancel->load()) {
                return run;
            }
            const f32 local = LocalTime(document, piece, k, dt);
            Mix mix;
            mix.worldSeconds = static_cast<f32>(piece.step + k) * dt;
            const f32 into_t = static_cast<f32>(k) * dt;
            const f32 w = switching ? TransitionWeight(into, into_t) : 1.0f;
            mix.plays.push_back(Play{piece.clip, local, w, false});
            if (switching && w < 1.0f) {
                // The clip switched from: frozen where the switch found it
                // (Warcraft III), or playing on, from the step after its last.
                const Piece& before = pieces[p - 1];
                const u32 carried = into.curve == ClipTransition::Curve::Snapshot ? before.steps : before.steps + k;
                const f32 then = before.kind == Piece::Kind::Loops
                                     ? std::fmod(static_cast<f32>(carried) * dt,
                                                 std::max(document.clips[before.clip].duration, 1e-6f))
                                     : std::min(static_cast<f32>(carried) * dt, document.clips[before.clip].duration);
                mix.plays.push_back(Play{before.clip, before.kind == Piece::Kind::Held ? 0.0f : then, 1.0f, false});
            }
            Pose pose;
            animator.evaluate(mix, pose);
            if (!stages.empty()) {
                stages.constrain(animator, mix, pose);
            }
            if (piece.step + k == 0) {
                bool any = false;
                for (const u32 n : run.nodes) {
                    const Vector3f o{pose.frame[n].data[3][0], pose.frame[n].data[3][1], pose.frame[n].data[3][2]};
                    lower = any ? Vector3f{std::min(lower.x, o.x), std::min(lower.y, o.y), std::min(lower.z, o.z)} : o;
                    upper = any ? Vector3f{std::max(upper.x, o.x), std::max(upper.y, o.y), std::max(upper.z, o.z)} : o;
                    any = true;
                }
            }
            readers[p].read(local, previous, now);
            BakeStep step;
            step.switches = now;
            step.world = own.world;
            step.time = local;
            step.floor = own.floor;
            step.floorHeight = root < pose.frame.size() ? pose.frame[root].data[3][2] - restHeight : 0.0f;
            step.travel = travel;
            step.dt = dt;
            step.lower = lower;
            step.upper = upper;
            hooks.step(document, model, step, pose);
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
            run.times.push_back(static_cast<f32>(static_cast<i64>(piece.step + k) - static_cast<i64>(run.first)) * dt);
            run.localTimes.push_back(local);
            previous = now;
            travel = travel + forward * (speed * dt);
            if (progress != nullptr) {
                ++progress->step;
            }
        }
    }

    // How settled: each preheat loop against the one before, at the same time.
    const Piece& lead = pieces.front();
    if (lead.kind == Piece::Kind::Loops && !run.nodes.empty()) {
        const u32 cycle = lead.clip == clip ? run.steps : stepsOf(document.clips[lead.clip].duration);
        const u32 loopsRun = lead.clip == clip ? preheat + 1 : std::max<u32>(1, preheat);
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
template <class T>
void Splice(const SubTrack* source, Interpolation interp, const std::vector<Transform>& samples,
            const std::vector<std::pair<u32, u32>>& spans, f32 dt, u32 stride, f32 noise, f32 height,
            const std::vector<T>& animation, SubTrack& out, std::vector<f32>& kept) {
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
        if (!inside(key.time, dt - slop)) {
            keys.push_back(key);
        }
    }
    // A key at each span's edge, one step out, on the source's curve: on a
    // whole millisecond, and split where the game reads it, which is keys at
    // whole milliseconds too.
    // @p before: the edge before its span, the source kept on its left.
    const auto onSource = [&](u32 step, bool before) {
        CurveKey<T> key;
        const u32 ms = Milliseconds(static_cast<f32>(step) * dt);
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
    for (const auto& [a, b] : spans) {
        if (a > 0) {
            keys.push_back(onSource(a - 1, true));
        }
        if (b < steps) {
            keys.push_back(onSource(b + 1, false));
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
            }
        }
    }
    for (const CurveKey<T>& key : keys) {
        kept.push_back(key.time);
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
            if (isBaked[i] != 0) {
                SmoothTangents(interp, sorted, i);
            }
        }
    }
    out.interp = interp;
    out.tcb.clear();
    EncodeKeys(out, sorted);
}

} // namespace

BakeReport BakeClip(Document& document, const BakeRun& run, const BakeRun* goesTo) {
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
        const Animator animator(document, model);
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
            MatchLoop(settings, steps, preheated, [&](i32 step) { return recorded(n, step); }, written[n].samples,
                      report.offsetInstead);
        }
    } else if (target != kInvalidIndex && settings.match != LoopMatch::Off) {
        // The end crossfaded into what a looping target plays from its end
        // back, or another's frame 0: its run, else its keys as they stand.
        const Clip& into = document.clips[target];
        const u32 window =
            std::clamp<u32>(static_cast<u32>(std::lround(settings.window * static_cast<f32>(steps))), 1, steps);
        const Animator animator(document, model);
        for (u32 i = steps - window; i <= steps; ++i) {
            const f32 u = static_cast<f32>(i - (steps - window)) / static_cast<f32>(window);
            const f32 e = Ease(settings, u);
            const f32 there = intoLoop ? std::max(into.duration - static_cast<f32>(steps - i) * dt, 0.0f) : 0.0f;
            // Its bake is its matched cycle; else its run; else its keys.
            const bool intoBaked = into.physics.has_value() && into.physics->baked.has_value();
            const bool useRun = !intoBaked && goesTo != nullptr && goesTo->clip == target && goesTo->complete;
            Pose pose;
            if (!useRun) {
                Mix mix;
                mix.plays.push_back(Play{target, there, 1.0f, false});
                animator.sample(mix, pose, true);
            }
            for (Written& w : written) {
                Transform partner;
                if (useRun) {
                    const auto found = std::find(goesTo->nodes.begin(), goesTo->nodes.end(), w.node);
                    if (found == goesTo->nodes.end()) {
                        continue;
                    }
                    partner = goesTo->localsAt(goesTo->stepAt(there))[found - goesTo->nodes.begin()];
                } else if (w.node < pose.local.size()) {
                    partner = pose.local[w.node];
                } else {
                    continue;
                }
                w.samples[i] = Blended(w.samples[i], partner, e);
            }
        }
    }

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
    const f32 height = std::max(ModelHeight(owner), 1.0f);
    const f32 still = 1e-5f * height;
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
    // Per node the bake writes, the steps it writes it at.
    std::map<u32, std::vector<u8>> writes;
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
            differs[i] = w.active[i] != 0 || turned || moved ? 1 : 0;
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
                                   height, anim, made, keptTimes);
            } else {
                std::vector<Vector3f> anim(steps + 1);
                for (u32 i = 0; i <= steps; ++i) {
                    anim[i] = animation[i][w.node].translation;
                }
                Splice<Vector3f>(source != nullptr ? &original : nullptr, interp, w.samples, spans, dt, stride, noise,
                                 height, anim, made, keptTimes);
            }
            report.keys += static_cast<u32>(made.times.size());
            keep.push_back(KeptKeys{clipIndex, channel, std::move(keptTimes)});
            place(channel, std::move(made), home, source != nullptr);
        }
    }
    // A constraint or IK stage over a node the bake writes runs again over its
    // keys at play, and a Link carries them a second time: off wherever the
    // bake writes one of its nodes, as it was everywhere else, stepped.
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
                made.times.push_back(static_cast<f32>(i) * dt);
                const u8* bytes = reinterpret_cast<const u8*>(&w);
                made.values.insert(made.values.end(), bytes, bytes + sizeof(f32));
                last = w;
            }
        }
        place(channel, std::move(made), home, source != nullptr);
        weights.push_back(channel);
    }
    // A *Full detail* cloth's drivers (cloth design §10.3): each free
    // particle's frame relative to the cloth's holder, step by step, the
    // recording's where the cloth is on and its anchors' where it is off, on
    // the cloth's own channels, which an Unbake takes away.
    for (u32 c = 0; c < run.cloths.size() && c < run.clothParticles.size(); ++c) {
        const Cloth* cloth = owner.physics.cloth(run.cloths[c]);
        if (cloth == nullptr || !cloth->recipe || cloth->recipe->bakeInto != ClothBakeInto::FullDetail ||
            cloth->cage.mesh >= owner.meshes.size()) {
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
        const Animator animator(document, model);
        for (u32 i = 0; i <= steps; ++i) {
            const u32 at = run.first + i;
            Mix mix;
            mix.plays.push_back(Play{clipIndex, static_cast<f32>(i) * dt, 1.0f, false});
            mix.globals = false;
            Pose pose;
            animator.evaluate(mix, pose);
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
    // Fewer keys, only among those the bake wrote (§8.3). A bake that wrote
    // none is still recorded, so the clip reads baked.
    if (settings.tolerance > 0.0f && !record.channels.empty()) {
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
    // After the reduction: a stepped weight is not its to thin.
    record.channels.insert(record.channels.end(), weights.begin(), weights.end());
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
    record.written = WrittenHash(document.clips[clipIndex], record);
    Clip& out = document.clips[clipIndex];
    if (!out.physics.has_value()) {
        out.physics.emplace();
    }
    out.physics->baked = std::move(record);
    report.ok = true;
    return report;
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
    RestoreSource(target);
    target.physics->baked.reset();
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
        if (clip.physics.has_value() && !clip.physics->baked.has_value() && !IsGlobalLoop(clip)) {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<u32> OutOfDateBakes(const Document& document) {
    std::vector<u32> out;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.physics.has_value() && clip.physics->baked.has_value() &&
            clip.physics->baked->inputs != BakeInputs(document, c)) {
            out.push_back(c);
        }
    }
    return out;
}

bool BakedKeysEdited(const Document& document, u32 clip) {
    if (clip >= document.clips.size() || !document.clips[clip].physics || !document.clips[clip].physics->baked) {
        return false;
    }
    const PhysicsBake& record = *document.clips[clip].physics->baked;
    return WrittenHash(document.clips[clip], record) != record.written;
}

u32 BakeForExport(Document& document, const BakeHooks& hooks, std::span<const std::shared_ptr<const BakeRun>> runs,
                  Diagnostics& out) {
    u32 baked = 0;
    for (const u32 clip : BakeOrder(document, UnbakedSetUpClips(document))) {
        const u64 inputs = BakeInputs(document, clip);
        const BakeRun* use = nullptr;
        for (const std::shared_ptr<const BakeRun>& run : runs) {
            if (run != nullptr && run->clip == clip && run->complete && run->inputs == inputs) {
                use = run.get();
            }
        }
        BakeRun fresh;
        if (use == nullptr) {
            fresh = SimulateClip(document, clip, hooks);
            use = &fresh;
        }
        const ElementRef where(ElementKind::Clip, clip);
        if (!use->complete) {
            out.warn(DiagCode::AnimStageNotBaked,
                     "clip '" + document.clips[clip].name + "': its physics could not be run: " + use->error, where);
            continue;
        }
        BakeReport report = BakeClip(document, *use);
        out.append(report.diagnostics);
        if (!report.ok) {
            out.warn(DiagCode::AnimStageNotBaked,
                     "clip '" + document.clips[clip].name + "': its physics could not be baked: " + report.error, where);
            continue;
        }
        ++baked;
    }
    return baked;
}

} // namespace wem
} // namespace models
} // namespace whiteout

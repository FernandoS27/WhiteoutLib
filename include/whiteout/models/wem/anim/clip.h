// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file clip.h
 * @brief Sub-tracks, containers, clips and anim sets (WEM v3, design §10.8).
 *
 * Three levels, and the middle one is the one the other formats do not have:
 *
 * ```
 * Clip  (M3 SEQS + its STG_)      the group: a name, a duration, what plays
 *  └─ SubTrackContainer (STC_)    a layer: a priority, transparent or opaque
 *      └─ SubTrack (SD*)          one channel's keys
 * ```
 *
 * MDX, `.m2` and D3 all produce exactly one container per clip. The level
 * exists because M3's split-body playback is stated in it, and because
 * collapsing it would make an `.m3` import lossy in the one place SC2 content
 * actually uses.
 *
 * WEM plays the layers by StarCraft II's rules (`animator.h`,
 * WEM_ANIMATION_RUNTIME_DESIGN.md D1): a weight budget spent highest priority
 * first, a transparent container abstaining where it keys nothing and an opaque
 * one filling with rest, combined by the smoothstep. Each track is read by its
 * clip's own `ReadRule`. An export to a format that plays one layer flattens a
 * clip's containers into one (`FlattenContainers`); track sets over container 0
 * need nothing, since container 0 already holds the whole pose.
 */

#include <optional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/wem/anim/channel.h>
#include <whiteout/models/wem/bounds.h>
#include <whiteout/models/wem/native_bag.h>
#include <whiteout/models/wem/profile.h>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// SubTrack
// ============================================================================

enum class Interpolation : u8 {
    Step,    ///< Hold the previous key. M3's AnimRef flags **bit 4** (§10.8.2).
    Linear,  ///< Componentwise lerp.
    Hermite, ///< Two tangents per key; see `ValuesPerKey`.
    Bezier,  ///< Two control values per key; stored the same way.
    Slerp,   ///< Quaternion shortest-arc. The default for `Channel::Rotation`.
    Count
};

const char* ToString(Interpolation interp);

/**
 * @brief Value elements one key of @p interp occupies — 3 for `Hermite` and
 *        `Bezier`, 1 otherwise.
 *
 * The tangents share the key's storage rather than living in vectors of their
 * own, because that **is** MDX's layout: a `Track<T>`'s `keys_data` holds
 * `{value, inTan, outTan}` per key for the smooth modes and one value for the
 * rest, and reinterpreting the span is how the parser exposes it. Keeping the
 * shape means the WC3 import is a copy and the round trip is exact.
 */
constexpr u32 ValuesPerKey(Interpolation interp) {
    return (interp == Interpolation::Hermite || interp == Interpolation::Bezier) ? 3u : 1u;
}

/**
 * @brief One channel's keyframe stream. M3's SD entry.
 *
 * `times` are **seconds**, converted at import from whatever ticks the source
 * counts in, and are **not clamped or padded to `Clip::duration`**: M3 wraps the
 * playhead modulo the *track's* own length, so a sub-track outlasting or
 * undershooting its clip is data, not an error (§10.8.2).
 */
struct SubTrack {
    u32 channel = 0; ///< An `AnimChannel::id`, never an index into the table.
    Interpolation interp = Interpolation::Linear;

    std::vector<f32> times;

    /// `times.size() * ValuesPerKey(interp) * AttrTypeSize(channel's valueType)`
    /// bytes. The type is on the channel because every sub-track that drives one
    /// must agree about it, and storing it here would let two disagree.
    std::vector<u8> values;

    /// TCB parameters, three per key (tension, continuity, bias), or empty.
    /// Only meaningful on a `Hermite` sub-track: the tangents in `values` are
    /// the ones these produce, so a consumer that ignores this field plays the
    /// same curve. An editor keeps them to derive the tangents again when a key
    /// changes; a format without TCB drops them and keeps the curve.
    std::vector<f32> tcb;

    std::size_t keyCount() const {
        return times.size();
    }

    /// Whether `values` is sized for `times` given @p valueType — the channel's,
    /// which the caller has and this struct deliberately does not.
    bool wellSized(geom::AttrType valueType) const {
        return values.size() == times.size() * ValuesPerKey(interp) * geom::AttrTypeSize(valueType);
    }

    template <class V>
    void reflect(V& v) {
        v.field("channel", channel);
        v.field("interp", interp);
        v.field("times", times);
        v.field("values", values);
        // v2: a sub-track written before this field carries no TCB parameters,
        // which is what empty means.
        v.since(2).field("tcb", tcb);
    }
};

// ============================================================================
// SubTrackContainer
// ============================================================================

/**
 * @brief The STC track-table rows and STS_ state WEM does not interpret.
 *
 * The shared name/value bag; see `NodeNative`.
 */
using ContainerNative = NativeBag;

/**
 * @brief One layer of a clip. M3's STC_.
 *
 * `concurrent` is the asymmetry: a **transparent** container (true) holding no
 * sub-track for a channel is skipped and lower layers show through; an
 * **opaque** one contributes the channel's `initValue` at full weight, forcing
 * un-keyed channels back to rest. Every non-M3 importer writes one opaque
 * container at priority 0, which is the degenerate case of the same rule.
 */
struct SubTrackContainer {
    std::string name;
    i32 priority = 0;        ///< STC `animPriority`.
    bool concurrent = false; ///< STC `runsConcurrent`.
    std::vector<SubTrack> subTracks;
    ContainerNative native;

    /// The sub-track driving channel id @p channel, or null.
    const SubTrack* find(u32 channel) const;

    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("priority", priority);
        v.field("concurrent", concurrent);
        v.field("subTracks", subTracks);
        v.field("native", native);
    }
};

// ============================================================================
// Clip
// ============================================================================

/**
 * @brief What starts this clip, beyond a host asking for it by name.
 *
 * **`AutoPlay | WorldClocked` is one concept across three formats**: M3's SEQS
 * flag 0x2, M2's global sequences, MDX's `globalSeqId` tracks. All three are a
 * loop the model runs on its own, off a clock that is not the host play's — and
 * all three become this.
 */
enum class ClipFlags : u32 {
    None = 0,
    AutoPlay = 0x0001,     ///< Started at anim-state init, not by a play request.
    Persistent = 0x0002,   ///< Survives an anim-state change.
    WorldClocked = 0x0004, ///< Timed off the world clock, not the play's own bracket.
};

constexpr ClipFlags operator|(ClipFlags a, ClipFlags b) {
    return static_cast<ClipFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr ClipFlags operator&(ClipFlags a, ClipFlags b) {
    return static_cast<ClipFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
inline ClipFlags& operator|=(ClipFlags& a, ClipFlags b) {
    a = a | b;
    return a;
}
constexpr bool hasFlag(ClipFlags value, ClipFlags bit) {
    return (static_cast<u32>(value) & static_cast<u32>(bit)) != 0;
}

/**
 * @brief A discrete key firing at a node — the node is the *where*, the key is
 *        the *when*.
 *
 * MDX's `EventObject` plus its KEVT timestamps, M3's SDEV keys, D3's
 * `flEventFrame`. What the event *means* is the host's: WEM carries the name and
 * one integer because that is all three formats agree on.
 *
 * The node's **kind is not fixed**: MDX and `.m2` name a dedicated `Event` node,
 * `.m3` names the bone the SDEV key sits on, and D3 names the attachment its
 * hardpoint resolves to. All three are the place the event happens.
 */
struct ClipEvent {
    f32 time = 0;            ///< Seconds from the clip's start.
    u32 node = kInvalidNode; ///< The node it fires at, of any kind; a §10.6 referencer.
    std::string name;
    u32 value = 0;

    template <class V>
    void reflect(V& v) {
        v.field("time", time);
        v.field("node", node);
        v.field("name", name);
        v.field("value", value);
    }
};

using ClipNative = NativeBag;

/**
 * @brief How one of a clip's tracks is read: the interpolation, the window and
 *        the wrap of the game the clip was made for
 *        (WEM_ANIMATION_RUNTIME_DESIGN.md §3.1).
 *
 * Stored, because a conversion rewrites the keys for another game and the rule
 * has to say which arithmetic the keys now in the clip were written for.
 */
enum class ReadRule : u8 {
    Wc3, ///< Warcraft III: keys inside the window, the MDX curves, the window's wrap.
    Sc2, ///< StarCraft II: a raw quaternion lerp; a looping track wraps at its own last key.
    Wow, ///< World of Warcraft: a quaternion nlerp without a sign flip; the ends hold.
};

/// The rule a clip made for @p game is read by: the game's own, and Warcraft
/// III's for a game that has no editor of its own.
constexpr ReadRule RuleOf(Game game) {
    switch (game) {
    case Game::StarCraft:
        return ReadRule::Sc2;
    case Game::Wow:
        return ReadRule::Wow;
    default:
        return ReadRule::Wc3;
    }
}

/**
 * @brief One of the model's `TrackSet`s this clip plays on a layer of its own.
 *
 * Every clip also plays the "default" set — whatever no listed set claims — in
 * its own containers, at their priority.
 */
struct ClipTrackSet {
    u32 set = kInvalidIndex; ///< -> the clip's `Model::trackSets`.
    i32 priority = 0;        ///< The layer's STC `animPriority`.

    template <class V>
    void reflect(V& v) {
        v.field("set", set);
        v.field("priority", priority);
    }
};

// ============================================================================
// Physics baked into a clip (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §7-§9)
// ============================================================================

/// Whether the bake runs a clip as a loop: from the clip's flag and its name
/// (`BakeLoops`), or as the user set it.
enum class BakePlays : u8 { FromClip, Loops, Once };
/// What runs before a clip's frame 0 (§7.2).
enum class BakeFrom : u8 { Auto, Itself, FirstFrame, Clip };
/// What a once-played clip's end is matched into (§7.4).
enum class BakeTo : u8 { Auto, Nothing, Clip };
/// How a loop's end is matched to its start (§7.3).
enum class LoopMatch : u8 { Crossfade, Offset, Off };
enum class MatchWhere : u8 { End, Start, Both };
enum class MatchEase : u8 { Smooth, Linear, EaseIn, EaseOut, Custom };
/// The ground the bake's run stands on (§4.8).
enum class BakeFloor : u8 { Grid, Off, FollowsRoot };

/// A wind or a blast of one clip's bake (§4.7). Never exported: the bake writes
/// what it does into keys.
struct BakeWorldForce {
    enum class Kind : u8 { Wind, Blast };
    Kind kind = Kind::Wind;
    f32 start = 0.0f; ///< Seconds; a blast's moment.
    f32 end = 0.0f;   ///< Seconds; a wind's end.
    f32 rampIn = 0.0f;
    f32 rampOut = 0.0f;
    f32 strength = 0.15f; ///< In g.
    f32 heading = 0.0f;   ///< Wind: degrees about +Z from +X.
    f32 rise = 0.0f;      ///< Wind: degrees up from level.
    f32 gusts = 0.3f;     ///< Wind: the gusts' share of its strength.
    f32 gustSpeed = 1.0f; ///< Wind: gusts a second.
    f32 radius = 1.0f;    ///< Blast: model heights.
    f32 height = 0.2f;    ///< Blast: how high over the model's feet it goes off, model heights.
    /// Blast: where it goes off; the model's middle when invalid. A node
    /// referencer.
    u32 centreNode = kInvalidNode;
    /// The World channels it pushes on: Wind (1 << 16) or Explosion (1 << 17)
    /// by kind, never none.
    u32 channels = 0;

    template <class V>
    void reflect(V& v) {
        v.field("kind", kind);
        v.field("start", start);
        v.field("end", end);
        v.field("rampIn", rampIn);
        v.field("rampOut", rampOut);
        v.field("strength", strength);
        v.field("heading", heading);
        v.field("rise", rise);
        v.field("gusts", gusts);
        v.field("gustSpeed", gustSpeed);
        v.field("radius", radius);
        v.field("height", height);
        v.field("centreNode", centreNode);
        v.field("channels", channels);
    }
};

/// What a bake replaced and wrote, so it can be redone and undone (§8.4).
struct PhysicsBake {
    /// Per container of the clip, the sub-tracks the bake replaced or cleared,
    /// as they were (a TCB track in its TCB form); and each one's place in
    /// its container, container by container, so an Unbake puts it back there.
    std::vector<SubTrackContainer> source;
    std::vector<u32> positions;
    /// The channels the bake wrote; and those a bake declared that it keeps,
    /// its own before a Rebake and any it keyed again, which an Unbake drops
    /// where nothing keys them.
    std::vector<u32> channels;
    std::vector<u32> appended;
    /// What went in, to tell a clip out of date; and what came out, to tell
    /// baked keys edited since.
    u64 inputs = 0;
    u64 written = 0;
    u32 nodes = 0;       ///< Nodes written.
    u32 keys = 0;        ///< Keys written.
    u32 restated = 0;    ///< TCB tracks restated as Hermite.
    f32 maxError = 0.0f; ///< Against a key every step, model units.
    f32 seamBefore = 0.0f; ///< Degrees, the worst node's, before matching.
    f32 seamAfter = 0.0f;
    /// After each preheat loop, the largest change from the loop before, in
    /// degrees (§7.5).
    std::vector<f32> settling;
    /// Each cloth the bake carried (EDIT_MODE_PHYSICS_CLOTH_DESIGN.md §10.2):
    /// its id, how far its bones missed it (0 in full detail), and its size.
    std::vector<u32> clothIds;
    std::vector<f32> clothFits;
    std::vector<f32> clothSizes;

    template <class V>
    void reflect(V& v) {
        v.field("source", source);
        v.field("positions", positions);
        v.field("channels", channels);
        v.field("appended", appended);
        v.field("inputs", inputs);
        v.field("written", written);
        v.field("nodes", nodes);
        v.field("keys", keys);
        v.field("restated", restated);
        v.field("maxError", maxError);
        v.field("seamBefore", seamBefore);
        v.field("seamAfter", seamAfter);
        v.field("settling", settling);
        v.since(6).field("clothIds", clothIds);
        v.since(6).field("clothFits", clothFits);
        v.since(6).field("clothSizes", clothSizes);
    }
};

/// One clip's bake settings (§9.2): a clip is *set up* once it has them.
struct ClipPhysics {
    BakePlays plays = BakePlays::FromClip;
    BakeFrom comesFrom = BakeFrom::Auto;
    std::string comesFromClip; ///< `BakeFrom::Clip`: by name, as the games name sequences.
    u8 preheat = 2;       ///< Loops run before frame 0, 0-20.
    BakeTo goesTo = BakeTo::Auto;
    std::string goesToClip;
    LoopMatch match = LoopMatch::Crossfade;
    MatchWhere where = MatchWhere::End;
    f32 window = 0.25f; ///< The match's width, a share of the clip.
    MatchEase ease = MatchEase::Smooth;
    std::vector<Vector2f> curve; ///< `MatchEase::Custom`: 0 -> 1, both ends pinned.
    /// The visible error the keys may leave, a share of the model's height; 0
    /// keeps a key every step (§8.3).
    f32 tolerance = 0.001f;
    bool meshMeasure = false;
    u8 keyRate = 60;  ///< Keys a second at most, with no reduction.
    u8 stepRate = 60; ///< Simulation steps a second: 60, 120 or 240.
    BakeFloor floor = BakeFloor::Grid;
    bool travel = false; ///< Carried forward at the clip's move speed (§4.8).
    std::vector<BakeWorldForce> world;
    std::optional<PhysicsBake> baked;

    template <class V>
    void reflect(V& v) {
        v.field("plays", plays);
        v.field("comesFrom", comesFrom);
        v.field("comesFromClip", comesFromClip);
        v.field("preheat", preheat);
        v.field("goesTo", goesTo);
        v.field("goesToClip", goesToClip);
        v.field("match", match);
        v.field("where", where);
        v.field("window", window);
        v.field("ease", ease);
        v.field("curve", curve);
        v.field("tolerance", tolerance);
        v.field("meshMeasure", meshMeasure);
        v.field("keyRate", keyRate);
        v.field("stepRate", stepRate);
        v.field("floor", floor);
        v.field("travel", travel);
        v.field("world", world);
        v.optional("baked", baked);
    }
};

struct Clip;

/// Whether `toMdx` writes @p clip's keys as they stand, rather than keying its
/// edges and dropping what lies outside: it came from an `.mdx` and keeps its
/// window or its global sequence id.
bool KeepsWindow(const Clip& clip);

/// The rule a clip read from a file older than CLIP v4 was made for, from the
/// markers its import left in the bag: a kept MDX window is Warcraft III's,
/// an M3 sequence id or start frame StarCraft II's, an M2 animation id or
/// global loop World of Warcraft's, and anything else Warcraft III's.
ReadRule DerivedReadRule(const Clip& clip);

/**
 * @brief One playable animation. M3's SEQS plus its STG_.
 *
 * `model` exists because a document holds several models (§9.1) and a channel id
 * is only meaningful within one model's table — a clip that did not say whose
 * nodes its sub-tracks name would be ambiguous the moment a D3 actor brought a
 * second model along on a hardpoint.
 */
struct Clip {
    std::string name;
    u32 model = kInvalidIndex; ///< -> `Document::models[]`. Whose channel table this drives.

    f32 duration = 0; ///< Seconds.
    bool looping = false;
    ClipFlags flags = ClipFlags::None;

    std::vector<SubTrackContainer> containers; ///< >= 1; one is the common case.
    std::vector<ClipEvent> events;
    ClipNative native;

    /**
     * @brief What the model occupies while this clip plays, when the source
     *        said so. All zeros when it did not — which `valid()` calls
     *        well-formed, so test the extent for volume, not validity.
     *
     * The one place WEM stores a bound it does not recompute. All four formats
     * ship one per sequence — MDX's `Sequence::extent`, M2's `bounds`, M3's
     * SEQS extents — and it is not derivable from the geometry: it is the
     * union over the *posed* mesh across the clip, so recovering it means
     * evaluating the whole skeleton at a sampling the source never recorded.
     * A host reads it to frame a camera, and a conservative substitute (the
     * model's own bounds) frames every clip as though it were the widest.
     */
    Extent bounds;

    /// The track sets this clip plays split out of container 0; see
    /// `LayeredContainers`.
    std::vector<ClipTrackSet> trackSets;

    /// How its tracks are read. A clip written before v4 derives it once, as
    /// it is read (`DerivedReadRule`).
    ReadRule readRule = ReadRule::Wc3;

    /// How its physics is simulated and baked into it; none for a clip not
    /// set up (EDIT_MODE_PHYSICS_BAKE_DESIGN.md §9.2).
    std::optional<ClipPhysics> physics;

    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("model", model);
        v.field("duration", duration);
        v.field("looping", looping);
        v.field("flags", flags);
        v.field("containers", containers);
        v.field("events", events);
        v.field("native", native);
        // v2: a clip written before this field carries no bounds, and a
        // degenerate extent is exactly what "the source did not say" means.
        v.since(2).field("bounds", bounds);
        // v3: the track sets it plays; none before.
        v.since(3).field("trackSets", trackSets);
        // v4: the read rule; derived from the markers the import left before.
        v.since(4).fieldOr("readRule", readRule, [this] { readRule = DerivedReadRule(*this); });
        // v5: the physics bake's settings and record; none before.
        v.since(5).optional("physics", physics);
    }
};

/**
 * @brief @p clip's containers as a layering format writes them: container 0
 *        split by the clip's `trackSets`, and the rest unchanged.
 *
 * Each listed set becomes a **transparent** container named after it, holding
 * container 0's sub-tracks for the set's channels, at the listed priority —
 * transparent because an opaque one would force every channel it lacks to rest
 * and override the whole body. What no listed set claims stays in container 0,
 * which keeps its name, priority and opacity: StarCraft II's "Default Track
 * Set". Sets come first, highest priority first, and a channel two of them
 * share goes to the higher; a set with nothing keyed in this clip is left out.
 * A clip listing no set comes back as it is.
 */
std::vector<SubTrackContainer> LayeredContainers(const Clip& clip, const std::vector<TrackSet>& sets);

/// The priority a use of @p set in @p clip has to be above for its layer to
/// leave a lone play of the clip unchanged (WEM_ANIMATION_RUNTIME_DESIGN.md
/// §3.4): container 0's, and that of every later container keying one of the
/// set's channels.
i32 TrackSetFloor(const Clip& clip, const TrackSet& set);

/**
 * @brief One clip's travel speed, in document units per second — MDX's
 *        `MoveSpeed`, `.m2`'s `movespeed` and M3's `moveSpeed`, which mean the
 *        same thing and so share one native key.
 *
 * Held in thousandths because the bag stores `i64` and the games disagree on
 * how big a unit is: Warcraft III walks at 270 and World of Warcraft at 2.5,
 * and 2.5 kept as a whole number is 2. Zero when the source stated no travel.
 */
f32 ClipMoveSpeed(const Clip& clip);

/// @brief Stores @p speed on @p clip; see @ref ClipMoveSpeed.
void SetClipMoveSpeed(Clip& clip, f32 speed);

/**
 * @brief MDX's `Sequence::rarity`, which the M3 export also reads as a
 *        `frequency`. Held in thousandths for the same reason as
 *        @ref ClipMoveSpeed; a document written before that holds whole units.
 */
f32 ClipRarity(const Clip& clip);

/// @brief Stores @p rarity on @p clip; see @ref ClipRarity.
void SetClipRarity(Clip& clip, f32 rarity);

// ============================================================================
// AnimSet
// ============================================================================

/// One (tag -> clip) row. A struct rather than a `std::pair` because a pair has
/// no `reflect()` and naming the halves is worth more than the two lines.
struct AnimTag {
    u32 tagId = 0;            ///< The source's own tag id; D3 hashes a name into it.
    u32 clip = kInvalidIndex; ///< -> `Document::clips[]`.

    template <class V>
    void reflect(V& v) {
        v.field("tagId", tagId);
        v.field("clip", clip);
    }
};

/**
 * @brief A named (tag -> clip) map, with a fallback.
 *
 * D3's `.ans` is the shape this exists for: 30 tag maps in one asset, one core
 * and 29 keyed by what the character is holding, and the runtime falls back to
 * core when the weapon's map has no row for a tag. That is **one `AnimSet` per
 * map**, with `baseAnimSet` pointing at the core one — the fallback field
 * spelling the fallback, rather than a 30-wide struct nothing else could use.
 * The same field carries `.ans`'s own `snoBaseAnimSet` link on the core set,
 * because it means the same thing one level up.
 */
struct AnimSet {
    std::string name;
    std::vector<AnimTag> byTag;
    u32 baseAnimSet = kInvalidIndex; ///< -> `Document::animSets[]`.

    /// The clip @p tagId maps to **in this set only** — the fallback is a walk
    /// the caller does, because it needs the document to do it.
    u32 find(u32 tagId) const;

    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("byTag", byTag);
        v.field("baseAnimSet", baseAnimSet);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout

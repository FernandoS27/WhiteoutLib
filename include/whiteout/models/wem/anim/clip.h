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
 * MDX, `.m2` and D3 all produce exactly one container per clip, and a consumer
 * that ignores layering entirely plays container 0 — which loses nothing on
 * their content. The level exists because M3's split-body playback is stated in
 * it, and because collapsing it would make an `.m3` import lossy in the one
 * place SC2 content actually uses.
 *
 * WEM stores the layering **data** — priority, `concurrent`, the channel's
 * `initValue` — and deliberately does not define the blender. Weight budgets,
 * the smoothstep combine and per-play brackets are a runtime's business.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/wem/anim/channel.h>
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
    }
};

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

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// What a bake record says it wrote (`PhysicsBake`): its stretches and the
/// hashes it compares. The bake writes them; a `.wem` read reads them
/// (`MarkBakeStretches`).

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <whiteout/models/wem/anim/clip.h>
#include <whiteout/models/wem/reflect_bytes.h>

namespace whiteout {
namespace models {
namespace wem {
namespace bakedetail {

inline void Hash(u64& hash, std::span<const u8> bytes) {
    for (const u8 b : bytes) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
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
inline std::optional<std::size_t> StretchesOf(const PhysicsBake& record, u32 channel, std::vector<Stretch>& out) {
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

inline bool Within(const std::vector<Stretch>& stretches, f32 time) {
    return std::any_of(stretches.begin(), stretches.end(),
                       [&](const Stretch& s) { return time >= s.from && time <= s.to; });
}

/// @p track's keys inside @p stretches, or those outside, hashed with its
/// controller.
inline void HashKeys(u64& hash, const SubTrack& track, const std::vector<Stretch>& stretches, bool inside) {
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
inline u64 OutsideHash(const Clip& clip, u32 channel, const std::vector<Stretch>& stretches) {
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

/// What a bake wrote into @p clip, hashed: the tracks of the channels @p record
/// lists, as they stand.
inline u64 WrittenHash(const Clip& clip, const PhysicsBake& record) {
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
inline u64 InsideHash(const Clip& clip, const PhysicsBake& record) {
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

} // namespace bakedetail
} // namespace wem
} // namespace models
} // namespace whiteout

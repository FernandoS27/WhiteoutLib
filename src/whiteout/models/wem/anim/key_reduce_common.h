// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file key_reduce_common.h
 * @brief What both tiers of the key reducer share (EDIT_MODE_KEY_OPTIMIZE_DESIGN.md):
 *        the value types, the channels never touched, and float noise.
 *        Internal to `key_reduce.cpp` and `key_reduce_tolerance.cpp`.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <type_traits>
#include <vector>

#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/anim/rests.h>
#include <whiteout/models/wem/nodes/emitters.h>

#include "prepared_track.h"

namespace whiteout {
namespace models {
namespace wem {
namespace keys {

using prepared::Key;
using prepared::Pair;
using prepared::PairInterpolation;
using prepared::Prepare;
using prepared::PreparedTrack;
using prepared::SpanFraction;

/// Calls `f.template operator()<T>(quat)` with the C++ type of @p type; false
/// for a type no track is read as.
template <class F>
bool WithType(geom::AttrType type, F&& f) {
    switch (type) {
    case geom::AttrType::F32:
        f.template operator()<f32>(false);
        return true;
    case geom::AttrType::F32x2:
        f.template operator()<Vector2f>(false);
        return true;
    case geom::AttrType::F32x3:
        f.template operator()<Vector3f>(false);
        return true;
    case geom::AttrType::F32x4:
        f.template operator()<Vector4f>(false);
        return true;
    case geom::AttrType::Quat:
        f.template operator()<Quaternion>(true);
        return true;
    case geom::AttrType::U32:
        f.template operator()<u32>(false);
        return true;
    default:
        return false;
    }
}

template <class T>
constexpr std::size_t kFloats = std::is_same_v<T, u32> ? 0 : sizeof(T) / sizeof(f32);

template <class T>
std::array<f32, 4> Floats(const T& value) {
    std::array<f32, 4> out{};
    if constexpr (kFloats<T> > 0) {
        std::memcpy(out.data(), &value, sizeof(T));
    }
    return out;
}

/// The largest finite component over @p keys' values and tangents: what float
/// noise is relative to.
template <class T>
f64 Magnitude(const std::vector<Key<T>>& keys) {
    f64 out = 0.0;
    for (const Key<T>& key : keys) {
        for (const T* v : {&key.value, &key.in, &key.out}) {
            const std::array<f32, 4> f = Floats(*v);
            for (std::size_t i = 0; i < kFloats<T>; ++i) {
                if (std::isfinite(f[i])) {
                    out = std::max(out, static_cast<f64>(std::fabs(f[i])));
                }
            }
        }
    }
    return out;
}

/// What two reads of the same curve may differ by in float arithmetic: some
/// thirty times a single rounding at the track's magnitude.
inline f64 Noise(f64 magnitude) {
    return 4e-6 * magnitude + 1e-12;
}

/// Componentwise within @p noise; an integer exactly. NaN is never near.
template <class T>
bool Near(const T& a, const T& b, f64 noise) {
    if constexpr (std::is_same_v<T, u32>) {
        return a == b;
    } else {
        const std::array<f32, 4> x = Floats(a);
        const std::array<f32, 4> y = Floats(b);
        for (std::size_t i = 0; i < kFloats<T>; ++i) {
            if (!(std::fabs(static_cast<f64>(x[i]) - y[i]) <= noise)) {
                return false;
            }
        }
        return true;
    }
}

/// `Near`, and for a quaternion also its negation: the same turn.
template <class T>
bool NearTurn(const T& a, const T& b, f64 noise) {
    if constexpr (std::is_same_v<T, Quaternion>) {
        return Near(a, b, noise) || Near(a, Quaternion(-b.x, -b.y, -b.z, -b.w), noise);
    } else {
        return Near(a, b, noise);
    }
}

/// Whether a channel's keys are never touched (§3.2): a squirt's, whose every
/// key fires a burst, and a pose stage's, which the export bakes.
inline bool Untouchable(const Document& document, u32 model, const AnimChannel& channel) {
    const TrackTarget& target = channel.target;
    if (IsStageChannel(target) || HeldByRenderer(document, model, channel)) {
        return true;
    }
    const NodeTree& tree = document.models[model].nodes;
    if (target.kind == TrackTarget::Kind::Node && target.channel == Channel::EmitterProperty &&
        target.node < tree.size() && tree.nodes[target.node].kind == NodeKind::Sc2ParticleEmitter &&
        EmitterPropertyOf(target.sub) == static_cast<u32>(Sc2ParticleProperty::SquirtAmount)) {
        return true;
    }
    return false;
}

/// @p track's channel when the reducer may change it: keyed, well sized, with
/// no TCB parameters, and not `Untouchable`.
inline const AnimChannel* Eligible(const Document& document, u32 model, const SubTrack& track) {
    const AnimChannel* channel = document.models[model].animChannels.find(track.channel);
    if (channel == nullptr || track.times.empty() || !track.wellSized(channel->valueType) ||
        !track.tcb.empty() || Untouchable(document, model, *channel)) {
        return nullptr;
    }
    return channel;
}

/// Whether a rule reads @p key of a clip read in @p window: `Wc3` only inside
/// it, every other rule every key.
template <class T>
bool ReadAt(const Clip& clip, const Key<T>& key, const SampleWindow& window) {
    return clip.readRule != ReadRule::Wc3 || (key.ms >= window.startMs && key.ms <= window.endMs);
}

/// Keeps the keys of @p track whose @p keep is set.
inline void Filter(SubTrack& track, geom::AttrType type, const std::vector<u8>& keep) {
    const std::size_t stride = ValuesPerKey(track.interp) * geom::AttrTypeSize(type);
    const bool tcb = track.tcb.size() == track.times.size() * 3;
    std::size_t w = 0;
    for (std::size_t k = 0; k < track.times.size(); ++k) {
        if (keep[k] == 0) {
            continue;
        }
        if (w != k) {
            track.times[w] = track.times[k];
            std::memmove(track.values.data() + w * stride, track.values.data() + k * stride, stride);
            if (tcb) {
                std::memmove(track.tcb.data() + w * 3, track.tcb.data() + k * 3, 3 * sizeof(f32));
            }
        }
        ++w;
    }
    track.times.resize(w);
    track.values.resize(w * stride);
    if (tcb) {
        track.tcb.resize(w * 3);
    }
}

/// The one value @p p reads everywhere, or nothing when it moves or reads
/// nothing: every key the rule reads equal, and a smooth one flat.
template <class T>
std::optional<T> ConstantOf(const Clip& clip, const PreparedTrack<T>& p, bool quat) {
    if (!p.valid) {
        return std::nullopt;
    }
    const SampleWindow window = ClipWindow(clip, 0, -1);
    const f64 noise = Noise(Magnitude(p.keys));
    const Interpolation interp = PairInterpolation(p);
    const bool smooth = interp == Interpolation::Hermite || interp == Interpolation::Bezier;
    const bool derivative = interp == Interpolation::Hermite && !quat;
    const Key<T>* first = nullptr;
    for (const Key<T>& key : p.keys) {
        if (!ReadAt(clip, key, window)) {
            continue;
        }
        if (first == nullptr) {
            first = &key;
        }
        if (!Near(key.value, first->value, noise)) {
            return std::nullopt;
        }
        if (smooth) {
            const T flat = derivative ? T{} : key.value;
            if (!Near(key.in, flat, noise) || !Near(key.out, flat, noise)) {
                return std::nullopt;
            }
        }
    }
    if (first == nullptr) {
        return std::nullopt;
    }
    if constexpr (std::is_same_v<T, Quaternion>) {
        // Between two keys `Wc3Slerp` and WoW's nlerp normalise, and one key
        // alone is read raw: only a unit turn reads the same either way.
        const Quaternion& q = first->value;
        const f64 length = std::sqrt(static_cast<f64>(q.x) * q.x + static_cast<f64>(q.y) * q.y +
                                     static_cast<f64>(q.z) * q.z + static_cast<f64>(q.w) * q.w);
        if (p.rule != ReadRule::Sc2 && !(std::fabs(length - 1.0) <= Noise(1.0))) {
            return std::nullopt;
        }
    }
    return first->value;
}

/// Tier 1's step 1 (§3.1) over @p clips of @p model and @p channels' sub-tracks,
/// each all of them when empty: keys no rule reads.
void DropUnreadKeys(Document& document, u32 model, const std::vector<u32>& clips,
                    const std::vector<u32>& channels, ExactKeyReport& report);
/// Tier 1's step 3 over the same: constant sub-tracks cut to one key.
void CollapseConstantTracks(Document& document, u32 model, const std::vector<u32>& clips,
                            const std::vector<u32>& channels, ExactKeyReport& report);
/// The channels of @p model whose keyed sub-tracks share one interpolation.
std::set<u32> AgreeingChannels(Document& document, u32 model);

} // namespace keys
} // namespace wem
} // namespace models
} // namespace whiteout

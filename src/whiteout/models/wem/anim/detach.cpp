// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/anim/detach.h>

#include <whiteout/models/wem/anim/curve_keys.h>
#include <whiteout/models/wem/anim/track_read.h>

#include <algorithm>
#include <type_traits>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// A key's millisecond on its clip's timeline, as the export writes a clip
/// that keeps its window: rounded on the absolute timeline, then rebased.
i32 WindowedMs(f32 seconds, i64 start) {
    const f32 absolute = seconds * 1000.0f + static_cast<f32>(start);
    const i64 ms = absolute <= 0.0f ? 0 : static_cast<i64>(static_cast<u32>(absolute + 0.5f));
    return static_cast<i32>(ms - start);
}

f32 Seconds(i64 ms) {
    return static_cast<f32>(ms) / 1000.0f;
}

/// One sub-track taken off a window `[start, start + end]`. False when it had
/// no key inside, and goes.
template <class T>
bool DetachTrack(SubTrack& track, i64 start, i32 end) {
    std::vector<CurveKey<T>> keys = DecodeKeys<T>(track);
    if (keys.empty()) {
        return false;
    }
    std::vector<CurveKey<T>> inside;
    for (CurveKey<T> key : keys) {
        const i32 ms = WindowedMs(key.time, start);
        if (ms < 0 || ms > end) {
            continue;
        }
        key.time = Seconds(ms);
        inside.push_back(key);
    }
    if (inside.empty()) {
        return false;
    }

    const Interpolation interp = track.interp;
    constexpr bool kQuat = std::is_same_v<T, Quaternion>;
    const bool smooth = interp == Interpolation::Hermite || interp == Interpolation::Bezier;
    const i32 firstMs = static_cast<i32>(Milliseconds(inside.front().time));
    const i32 lastMs = static_cast<i32>(Milliseconds(inside.back().time));
    const bool keyStart = firstMs != 0;
    const bool keyEnd = lastMs != end;
    bool preFirst = false; ///< A key one millisecond short of the first went in.
    if (keyStart || keyEnd) {
        CurveKey<T> edgeStart;
        CurveKey<T> edgeEnd;
        if (inside.size() == 1) {
            // One key plays throughout: the edges hold it, and so does it.
            CurveKey<T>& lone = inside.front();
            const T flat = FlatTangent(interp, lone.value);
            lone.in = lone.out = flat;
            edgeStart = edgeEnd = lone;
        } else {
            // What the renderer shows at either edge is its wrap from the last
            // key round to the first (`FindBracket`). Past the last key it runs
            // from 0 at the last key to `u` at the end: split it there, and the
            // left half ends at the new end key.
            const CurveKey<T> last = inside.back();
            const CurveKey<T> first = inside.front();
            const i32 span = (firstMs - lastMs) + end;
            const f32 u = span > 0 ? std::clamp(static_cast<f32>(end - lastMs) / static_cast<f32>(span), 0.0f, 1.0f)
                                   : 0.0f;
            if (keyEnd) {
                CurveKey<T> left = last;
                CurveKey<T> right = first;
                const CurveKey<T> cut = SplitSpan(interp, left, right, u);
                edgeEnd = cut;
                edgeEnd.out = FlatTangent(interp, cut.value);
                inside.back().out = left.out;
            }
            // Before the first key the game plays the same span measured from
            // the first key, so from `u - 1` at 0 up to 0, and then jumps to the
            // first key's value when the time reaches it (§1.5). That is the
            // span over `[u - 1, -1 / span]` up to one millisecond short of the
            // first key, and the jump in the millisecond after: every whole
            // millisecond lands where it did.
            if (keyStart && span > 0) {
                const f32 f0 = -static_cast<f32>(firstMs) / static_cast<f32>(span);
                const f32 f1 = -1.0f / static_cast<f32>(span);
                const auto [from, to] = SubSpan(interp, last, first, f0, f1);
                edgeStart = from;
                edgeStart.in = FlatTangent(interp, from.value);
                if (firstMs > 1 && interp != Interpolation::Step) {
                    CurveKey<T> shortOf = to;
                    shortOf.time = Seconds(firstMs - 1);
                    shortOf.out = FlatTangent(interp, to.value);
                    inside.insert(inside.begin(), shortOf);
                    preFirst = true;
                } else {
                    edgeStart.out = FlatTangent(interp, from.value);
                }
                inside[preFirst ? 1 : 0].in = FlatTangent(interp, inside[preFirst ? 1 : 0].value);
            } else if (keyStart) {
                edgeStart = last;
                edgeStart.in = edgeStart.out = FlatTangent(interp, last.value);
            }
        }
        if (keyStart) {
            edgeStart.time = 0.0f;
            inside.insert(inside.begin(), edgeStart);
        }
        if (keyEnd) {
            edgeEnd.time = Seconds(end);
            inside.push_back(edgeEnd);
        }
        if constexpr (kQuat) {
            // Squad's control points are derived (§3.4): the new keys and their
            // neighbours re-derive; this split is not exact (§12).
            if (smooth) {
                const std::size_t n = inside.size();
                for (const std::size_t i : {std::size_t{0}, std::size_t{1}, preFirst ? std::size_t{2} : std::size_t{1},
                                            n - 2, n - 1}) {
                    if (i < n) {
                        DeriveSquadPoints(inside, i);
                    }
                }
            }
        }
    }
    EncodeKeys(track, inside);
    return true;
}

/// A held sub-track (an integer's), read the way `EvaluateTrackU32` reads one:
/// the last key inside holds before the first as well as after it, so both
/// edges are keyed with it (EDIT_MODE_CURVE_EDITOR_DESIGN.md §8). A held value
/// does not move before it jumps, so no key a millisecond short of the first.
bool DetachHeld(SubTrack& track, geom::AttrType type, i64 start, i32 end) {
    const std::size_t size = ValuesPerKey(track.interp) * geom::AttrTypeSize(type);
    if (size == 0 || track.values.size() != track.times.size() * size) {
        return false;
    }
    std::vector<f32> times;
    std::vector<u8> values;
    for (std::size_t k = 0; k < track.times.size(); ++k) {
        const i32 ms = WindowedMs(track.times[k], start);
        if (ms < 0 || ms > end) {
            continue;
        }
        times.push_back(Seconds(ms));
        values.insert(values.end(), track.values.begin() + static_cast<std::ptrdiff_t>(k * size),
                      track.values.begin() + static_cast<std::ptrdiff_t>((k + 1) * size));
    }
    if (times.empty()) {
        return false;
    }
    const std::vector<u8> last(values.end() - static_cast<std::ptrdiff_t>(size), values.end());
    if (Milliseconds(times.front()) != 0) {
        times.insert(times.begin(), 0.0f);
        values.insert(values.begin(), last.begin(), last.end());
    }
    if (static_cast<i32>(Milliseconds(times.back())) != end) {
        times.push_back(Seconds(end));
        values.insert(values.end(), last.begin(), last.end());
    }
    track.times = std::move(times);
    track.values = std::move(values);
    return true;
}

bool DetachAny(SubTrack& track, geom::AttrType type, i64 start, i32 end) {
    switch (type) {
    case geom::AttrType::F32:
        return DetachTrack<f32>(track, start, end);
    case geom::AttrType::F32x3:
        return DetachTrack<Vector3f>(track, start, end);
    case geom::AttrType::F32x4:
        return DetachTrack<Vector4f>(track, start, end);
    case geom::AttrType::Quat:
        return DetachTrack<Quaternion>(track, start, end);
    default:
        return DetachHeld(track, type, start, end);
    }
}

} // namespace

bool OnImportedTimeline(const Clip& clip) {
    return !IsGlobalLoop(clip) && clip.native.value("intervalStart", -1) >= 0;
}

void DetachClip(Document& document, u32 model, u32 clip) {
    if (model >= document.models.size() || clip >= document.clips.size() || document.clips[clip].model != model) {
        return;
    }
    DetachClip(document.models[model].animChannels, document.clips[clip]);
}

void DetachClip(const AnimChannelTable& table, Clip& target) {
    if (!OnImportedTimeline(target)) {
        return;
    }
    const i64 start = target.native.value("intervalStart", -1);
    const i64 stop = target.native.value("intervalEnd", -1);
    const i32 end = stop >= start ? static_cast<i32>(stop - start) : static_cast<i32>(ClipMs(target));
    for (SubTrackContainer& container : target.containers) {
        std::vector<SubTrack> kept;
        kept.reserve(container.subTracks.size());
        for (SubTrack& track : container.subTracks) {
            const AnimChannel* channel = table.find(track.channel);
            // A sub-track nothing declares is left as it is: the export drops
            // it either way, and it is not this edit's to judge.
            if (channel == nullptr || DetachAny(track, channel->valueType, start, end)) {
                kept.push_back(std::move(track));
            }
        }
        container.subTracks = std::move(kept);
    }
    std::erase_if(target.native.entries, [](const NativeBag::Entry& entry) {
        return entry.name == "intervalStart" || entry.name == "intervalEnd";
    });
}

} // namespace wem
} // namespace models
} // namespace whiteout

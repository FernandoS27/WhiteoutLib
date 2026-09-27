// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/anim/key_reduce.h>

#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "key_reduce_common.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using namespace keys;

struct Ref {
    u32 clip = 0;
    u32 container = 0;
    u32 track = 0;
};

SubTrack& At(Document& document, const Ref& ref) {
    return document.clips[ref.clip].containers[ref.container].subTracks[ref.track];
}

/// Erases the sub-tracks of @p container marked in @p drop.
u32 Erase(SubTrackContainer& container, const std::vector<u8>& drop) {
    u32 erased = 0;
    for (std::size_t i = drop.size(); i-- > 0;) {
        if (drop[i] != 0) {
            container.subTracks.erase(container.subTracks.begin() + static_cast<std::ptrdiff_t>(i));
            ++erased;
        }
    }
    return erased;
}

/// Every keyed sub-track of @p model, by channel id.
std::map<u32, std::vector<Ref>> RefsOf(const Document& document, u32 model) {
    std::map<u32, std::vector<Ref>> out;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != model) {
            continue;
        }
        for (u32 k = 0; k < clip.containers.size(); ++k) {
            const auto& tracks = clip.containers[k].subTracks;
            for (u32 i = 0; i < tracks.size(); ++i) {
                if (!tracks[i].times.empty()) {
                    out[tracks[i].channel].push_back(Ref{c, k, i});
                }
            }
        }
    }
    return out;
}

/// Whether every keyed sub-track of a channel has one interpolation, so a
/// writer that gives a track one (MDX, M3's step bit, M2) emits what each was
/// read with (§1.3).
bool Agree(Document& document, const std::vector<Ref>& refs) {
    for (const Ref& ref : refs) {
        if (At(document, ref).interp != At(document, refs.front()).interp) {
            return false;
        }
    }
    return true;
}

// ---- Step 1: keys no rule reads ------------------------------------------------

bool InScope(const std::vector<u32>& clips, u32 clip) {
    return clips.empty() || std::find(clips.begin(), clips.end(), clip) != clips.end();
}

void DropUnread(Document& document, u32 model, const std::vector<u32>& scope,
                const std::vector<u32>& channels, ExactKeyReport& report) {
    // Emptying the clip a channel's clock comes from would move a written track
    // onto another clock, or leave the channel unkeyed, which swaps the rest
    // every clip shows from the keyed-elsewhere one to the unkeyed one.
    std::map<u32, const Clip*> clocks;
    for (const auto& [id, refs] : RefsOf(document, model)) {
        clocks[id] = ClockOwner(document, model, id);
    }
    for (u32 c = 0; c < document.clips.size(); ++c) {
        Clip& clip = document.clips[c];
        if (clip.model != model || clip.readRule != ReadRule::Wc3 || !InScope(scope, c)) {
            continue;
        }
        const SampleWindow window = ClipWindow(clip, 0, -1);
        const bool kept = KeepsWindow(clip);
        for (SubTrackContainer& container : clip.containers) {
            std::vector<u8> drop(container.subTracks.size(), 0);
            for (std::size_t i = 0; i < container.subTracks.size(); ++i) {
                SubTrack& track = container.subTracks[i];
                const AnimChannel* channel = Eligible(document, model, track);
                if (channel == nullptr || !InScope(channels, track.channel)) {
                    continue;
                }
                std::vector<u8> keep(track.times.size(), 0);
                bool read = false;
                WithType(channel->valueType, [&]<class T>(bool quat) {
                    const PreparedTrack<T> p = Prepare<T>(clip, track, quat);
                    if (!p.valid) {
                        return;
                    }
                    read = true;
                    for (const Key<T>& key : p.keys) {
                        if (key.source >= 0 && ReadAt(clip, key, window)) {
                            keep[static_cast<std::size_t>(key.source)] = 1;
                        }
                    }
                    // A windowless clip's edges are made from its outside keys.
                    for (std::size_t k = 0; !kept && k < track.times.size(); ++k) {
                        if (track.times[k] < -1e-4f || track.times[k] > clip.duration + 1e-4f) {
                            keep[k] = 1;
                        }
                    }
                });
                const auto removed =
                    static_cast<u32>(std::count(keep.begin(), keep.end(), u8{0}));
                if (!read || removed == 0 ||
                    (removed == track.times.size() && clocks[track.channel] == &clip)) {
                    continue;
                }
                report.keysRemoved += removed;
                Filter(track, channel->valueType, keep);
                // It read nothing before either: absent, not empty, which a
                // transparent layer would read as the rest.
                drop[i] = track.times.empty() ? 1 : 0;
            }
            report.subTracksDropped += Erase(container, drop);
        }
    }
}

// ---- Step 2: sub-tracks that play the rest ------------------------------------------

/// The one value @p track reads, as bytes.
std::optional<std::vector<u8>> ConstantBytes(const Clip& clip, const SubTrack& track,
                                             geom::AttrType type) {
    std::optional<std::vector<u8>> out;
    WithType(type, [&]<class T>(bool quat) {
        const std::optional<T> value = ConstantOf(clip, Prepare<T>(clip, track, quat), quat);
        if (value) {
            out = prepared::Bytes(*value);
        }
    });
    return out;
}

bool SameAsRest(geom::AttrType type, const std::vector<u8>& value, const std::vector<u8>& rest) {
    if (value.size() != rest.size() || value.size() != geom::AttrTypeSize(type)) {
        return false;
    }
    bool same = false;
    WithType(type, [&]<class T>(bool) {
        const T a = prepared::Load<T>(value.data());
        const T b = prepared::Load<T>(rest.data());
        std::vector<Key<T>> both(2);
        both[0].value = both[0].in = both[0].out = a;
        both[1].value = both[1].in = both[1].out = b;
        same = NearTurn(a, b, Noise(Magnitude(both)));
    });
    return same;
}

void DropRests(Document& document, u32 model, Game storage, ExactKeyReport& report) {
    const Model& owner = document.models[model];
    u32 clipCount = 0;
    for (const Clip& clip : document.clips) {
        clipCount += clip.model == model ? 1u : 0u;
    }
    std::set<std::tuple<u32, u32, u32>> dropped;
    for (const auto& [id, refs] : RefsOf(document, model)) {
        const AnimChannel* channel = owner.animChannels.find(id);
        if (channel == nullptr || Untouchable(document, model, *channel) || !Agree(document, refs)) {
            continue;
        }
        const TrackRests rests = RestsPlayed(document, model, *channel, storage);
        std::vector<std::optional<std::vector<u8>>> constant(refs.size());
        std::vector<u8> droppable(refs.size(), 0);
        std::set<u32> keying;
        for (std::size_t j = 0; j < refs.size(); ++j) {
            const Clip& clip = document.clips[refs[j].clip];
            const SubTrack& track = At(document, refs[j]);
            keying.insert(refs[j].clip);
            // Only an opaque layer fills with the rest where it keys nothing.
            if (clip.containers[refs[j].container].concurrent ||
                Eligible(document, model, track) == nullptr) {
                continue;
            }
            constant[j] = ConstantBytes(clip, track, channel->valueType);
            droppable[j] = constant[j] ? 1 : 0;
        }
        bool all = true;
        for (std::size_t j = 0; j < refs.size() && all; ++j) {
            all = droppable[j] != 0 && SameAsRest(channel->valueType, *constant[j], rests.unkeyed);
        }
        // Dropping them all leaves the channel unkeyed, which moves every clip
        // that never keyed it from the keyed-elsewhere rest to the unkeyed one.
        if (all && (!rests.differ() || keying.size() == clipCount)) {
            for (const Ref& ref : refs) {
                dropped.insert({ref.clip, ref.container, ref.track});
            }
            continue;
        }
        // The channel stays keyed through the clip that sets its clock.
        const Clip* clock = ClockOwner(document, model, id);
        const std::vector<u8>& rest = rests.differ() ? rests.keyedElsewhere : rests.unkeyed;
        for (std::size_t j = 0; j < refs.size(); ++j) {
            if (&document.clips[refs[j].clip] != clock && droppable[j] != 0 &&
                SameAsRest(channel->valueType, *constant[j], rest)) {
                dropped.insert({refs[j].clip, refs[j].container, refs[j].track});
            }
        }
    }
    for (u32 c = 0; c < document.clips.size(); ++c) {
        Clip& clip = document.clips[c];
        for (u32 k = 0; k < clip.containers.size(); ++k) {
            SubTrackContainer& container = clip.containers[k];
            std::vector<u8> drop(container.subTracks.size(), 0);
            for (u32 i = 0; i < container.subTracks.size(); ++i) {
                if (dropped.count({c, k, i}) != 0) {
                    drop[i] = 1;
                    report.keysRemoved += static_cast<u32>(container.subTracks[i].times.size());
                }
            }
            report.subTracksDropped += Erase(container, drop);
        }
    }
}

// ---- Step 3: constant tracks ----------------------------------------------------------

void CollapseConstants(Document& document, u32 model, const std::vector<u32>& scope,
                       const std::vector<u32>& channels, ExactKeyReport& report) {
    for (u32 c = 0; c < document.clips.size(); ++c) {
        Clip& clip = document.clips[c];
        if (clip.model != model || !InScope(scope, c)) {
            continue;
        }
        const SampleWindow window = ClipWindow(clip, 0, -1);
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const AnimChannel* channel = Eligible(document, model, track);
                if (channel == nullptr || track.times.size() < 2 || !InScope(channels, track.channel)) {
                    continue;
                }
                std::optional<std::size_t> one;
                WithType(channel->valueType, [&]<class T>(bool quat) {
                    const PreparedTrack<T> p = Prepare<T>(clip, track, quat);
                    if (!ConstantOf(clip, p, quat)) {
                        return;
                    }
                    if (clip.readRule != ReadRule::Wc3) {
                        one = 0;
                    } else if (KeepsWindow(clip)) {
                        for (const Key<T>& key : p.keys) {
                            if (key.source >= 0 && ReadAt(clip, key, window)) {
                                one = static_cast<std::size_t>(key.source);
                                break;
                            }
                        }
                    } else {
                        // The export makes the edges from the one key left.
                        for (std::size_t k = 0; k < track.times.size(); ++k) {
                            if (track.times[k] >= -1e-4f && track.times[k] <= clip.duration + 1e-4f) {
                                one = k;
                                break;
                            }
                        }
                    }
                });
                if (!one) {
                    continue;
                }
                std::vector<u8> keep(track.times.size(), 0);
                keep[*one] = 1;
                report.keysRemoved += static_cast<u32>(track.times.size() - 1);
                ++report.tracksCollapsed;
                Filter(track, channel->valueType, keep);
            }
        }
    }
}

// ---- Step 4: keys the game reproduces --------------------------------------------------

/// Whether @p p read with keys @p a and @p c adjacent reads, every millisecond
/// between them, what it reads with all its keys. The keys between are checked
/// first; a curve is then checked at every millisecond, a line needing no more.
template <class T>
bool Reproduced(const PreparedTrack<T>& p, std::size_t a, std::size_t c, bool curved, f64 noise) {
    const Interpolation interp = PairInterpolation(p);
    const auto original = [&](i32 t, std::size_t j) {
        return Pair(p.rule, interp, p.keys[j], p.keys[j + 1],
                    SpanFraction(t, p.times[j], p.times[j + 1]), false);
    };
    const auto candidate = [&](i32 t) {
        return Pair(p.rule, interp, p.keys[a], p.keys[c], SpanFraction(t, p.times[a], p.times[c]),
                    false);
    };
    for (std::size_t j = a + 1; j < c; ++j) {
        if (!Near(original(p.times[j], j), candidate(p.times[j]), noise)) {
            return false;
        }
    }
    if (curved) {
        std::size_t j = a;
        for (i32 t = p.times[a]; t < p.times[c]; ++t) {
            while (j + 1 < c && p.times[j + 1] <= t) {
                ++j;
            }
            if (!Near(original(t, j), candidate(t), noise)) {
                return false;
            }
        }
    }
    return true;
}

/// Clears @p keep for every stored key the rule reproduces from its
/// neighbours; the first and last key read, and every key the export makes
/// up or does not read, stay.
template <class T>
void ReproducedKeys(const Clip& clip, const PreparedTrack<T>& p, bool quat, std::vector<u8>& keep) {
    const std::size_t n = p.keys.size();
    if (!p.valid || n < 3) {
        return;
    }
    for (std::size_t k = 1; k < n; ++k) {
        if (p.times[k] <= p.times[k - 1]) {
            return; // Two keys on one millisecond: no span to judge between them.
        }
    }
    const SampleWindow window = ClipWindow(clip, 0, -1);
    std::vector<u8> fixed(n, 0);
    std::size_t first = n;
    std::size_t last = n;
    for (std::size_t k = 0; k < n; ++k) {
        if (p.keys[k].source < 0 || !ReadAt(clip, p.keys[k], window)) {
            fixed[k] = 1;
        } else {
            first = first == n ? k : first;
            last = k;
        }
    }
    if (first == n) {
        return;
    }
    fixed[first] = fixed[last] = fixed[0] = fixed[n - 1] = 1;
    const Interpolation interp = PairInterpolation(p);
    const bool curved = (quat && p.rule != ReadRule::Sc2) || interp == Interpolation::Hermite ||
                        interp == Interpolation::Bezier;
    const f64 noise = Noise(Magnitude(p.keys));
    std::size_t a = 0;
    for (std::size_t k = 1; k + 1 < n; ++k) {
        if (fixed[k] == 0 && Reproduced(p, a, k + 1, curved, noise)) {
            keep[static_cast<std::size_t>(p.keys[k].source)] = 0;
        } else {
            a = k;
        }
    }
}

void DropReproduced(Document& document, u32 model, ExactKeyReport& report) {
    const std::set<u32> agreeing = keys::AgreeingChannels(document, model);
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            for (SubTrack& track : container.subTracks) {
                const AnimChannel* channel = Eligible(document, model, track);
                if (channel == nullptr || track.times.size() < 3 || agreeing.count(track.channel) == 0) {
                    continue;
                }
                std::vector<u8> keep(track.times.size(), 1);
                WithType(channel->valueType, [&]<class T>(bool quat) {
                    ReproducedKeys(clip, Prepare<T>(clip, track, quat), quat, keep);
                });
                const auto removed =
                    static_cast<u32>(std::count(keep.begin(), keep.end(), u8{0}));
                if (removed != 0) {
                    report.keysRemoved += removed;
                    Filter(track, channel->valueType, keep);
                }
            }
        }
    }
}

} // namespace

ExactKeyReport ReduceKeysExactly(Document& document, u32 model) {
    ExactKeyReport report;
    if (model >= document.models.size()) {
        return report;
    }
    DropUnread(document, model, {}, {}, report);
    // WoW's and Diablo III's writers fill a clip that keys nothing with rests
    // of their own, which the design has not audited.
    const Game storage = GameOf(document.defaultProfile);
    if (storage == Game::Warcraft || storage == Game::StarCraft) {
        DropRests(document, model, storage, report);
    }
    CollapseConstants(document, model, {}, {}, report);
    DropReproduced(document, model, report);
    return report;
}

namespace keys {

void DropUnreadKeys(Document& document, u32 model, const std::vector<u32>& clips,
                    const std::vector<u32>& channels, ExactKeyReport& report) {
    DropUnread(document, model, clips, channels, report);
}

void CollapseConstantTracks(Document& document, u32 model, const std::vector<u32>& clips,
                            const std::vector<u32>& channels, ExactKeyReport& report) {
    CollapseConstants(document, model, clips, channels, report);
}

std::set<u32> AgreeingChannels(Document& document, u32 model) {
    std::set<u32> out;
    for (const auto& [id, refs] : RefsOf(document, model)) {
        if (Agree(document, refs)) {
            out.insert(id);
        }
    }
    return out;
}

} // namespace keys

f32 ModelHeight(const Model& model) {
    bool any = false;
    f32 low = 0.0f;
    f32 high = 0.0f;
    const auto take = [&](f32 z) {
        if (!std::isfinite(z)) {
            return;
        }
        low = any ? std::min(low, z) : z;
        high = any ? std::max(high, z) : z;
        any = true;
    };
    for (const Mesh& mesh : model.meshes) {
        for (const Vector3f& p :
             mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex)) {
            take(p.z);
        }
    }
    // Attachments and emitters stand where the game hangs things, not where
    // the model is; they give the size only of a model with no mesh.
    if (!any) {
        for (u32 n = 0; n < model.nodes.size(); ++n) {
            take(model.nodes.worldBind(n).translation.z);
        }
    }
    return std::max(high - low, 1.0f);
}

u64 CountKeys(const Document& document, u32 model, const std::vector<u32>& clips) {
    u64 out = 0;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != model ||
            (!clips.empty() && std::find(clips.begin(), clips.end(), c) == clips.end())) {
            continue;
        }
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                out += track.times.size();
            }
        }
    }
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout

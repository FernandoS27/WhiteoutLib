// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/animator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "whiteout/models/mdx/structures.h"
#include "whiteout/models/wem/anim/m3_math.h"
#include "whiteout/models/wem/anim/mdx_math.h"
#include "whiteout/models/wem/anim/rests.h"
#include "whiteout/models/wem/anim/track_read.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using NF = ::whiteout::mdx::Node::NodeFlag;

/// StarCraft II's cap on plays: the M3 adapter builds layers for the first 64
/// clips of a request and drops the rest (`BuildLayers`). Warcraft III has no
/// such limit — a glue scene runs sixty global sequences under its animation —
/// so the cap is the StarCraft II storage's alone.
constexpr std::size_t kMaxPlays = 64;
/// Contributions kept per channel; budget is spent past them.
constexpr int kMaxContributions = 16;

template <class T>
T Load(const u8* at) {
    T value{};
    std::memcpy(&value, at, sizeof(T));
    return value;
}

template <class T>
std::vector<u8> Bytes(const T& value) {
    std::vector<u8> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

/// Two layers' values combined at @p t, as `MixLayers` does: rotations by
/// `M3SlerpQuat`, floats componentwise, anything else not at all.
std::vector<u8> MixValues(geom::AttrType type, const std::vector<u8>& a, const std::vector<u8>& b,
                          f32 t) {
    if (type == geom::AttrType::Quat && a.size() == sizeof(Quaternion) && b.size() == a.size()) {
        return Bytes(M3SlerpQuat(Load<Quaternion>(a.data()), Load<Quaternion>(b.data()), t));
    }
    const bool floats = type == geom::AttrType::F32 || type == geom::AttrType::F32x2 ||
                        type == geom::AttrType::F32x3 || type == geom::AttrType::F32x4;
    if (!floats || a.size() != b.size()) {
        return b;
    }
    std::vector<u8> out(a.size());
    for (std::size_t at = 0; at + sizeof(f32) <= a.size(); at += sizeof(f32)) {
        const f32 x = Load<f32>(a.data() + at);
        const f32 y = Load<f32>(b.data() + at);
        const f32 v = x + (y - x) * t;
        std::memcpy(out.data() + at, &v, sizeof(f32));
    }
    return out;
}

bool IsNodeTransform(const TrackTarget& target) {
    return target.kind == TrackTarget::Kind::Node &&
           (target.channel == Channel::Translation || target.channel == Channel::Rotation ||
            target.channel == Channel::Scale);
}

/// Whether every container of @p clip abstains where it keys nothing: a global
/// loop that overlays whatever plays, which a priority tie puts first.
bool Concurrent(const Clip& clip) {
    if (clip.containers.empty()) {
        return false;
    }
    for (const SubTrackContainer& container : clip.containers) {
        if (!container.concurrent) {
            return false;
        }
    }
    return true;
}

i32 WholeMs(f32 seconds) {
    return static_cast<i32>(std::lround(seconds * 1000.0f));
}

/// Where a host play of @p clip reads: its own time, which a looping `Sc2`
/// track wraps itself; a `Wc3` or `Wow` clip past its end wraps as the host's
/// playlist wraps it, or holds its last frame.
SampleWindow HostWindow(const Clip& clip, const Play& play) {
    const i32 duration = static_cast<i32>(ClipMs(clip));
    i32 ms = std::max(WholeMs(play.seconds), 0);
    const bool loop = play.loop && clip.looping;
    if (clip.readRule == ReadRule::Sc2) {
        SampleWindow window = ClipWindow(clip, static_cast<u32>(ms), -1);
        window.loop = loop;
        return window;
    }
    if (ms > duration) {
        ms = loop && duration > 0 ? ms % duration : duration;
    }
    return ClipWindow(clip, static_cast<u32>(ms), -1);
}

} // namespace

u32 MdxNodeFlags(const Node& node) {
    // `toMdx`'s rule (mdx_converter.cpp, `FromNodeFlags`): the raw word
    // carries the bits WEM has no name for, and the shared ones are rewritten
    // from `NodeFlags` so an edit to those survives.
    constexpr u32 kShared = static_cast<u32>(NF::DontInheritTranslation) |
                            static_cast<u32>(NF::DontInheritRotation) |
                            static_cast<u32>(NF::DontInheritScaling) |
                            static_cast<u32>(NF::Billboarded) |
                            static_cast<u32>(NF::BillboardedLockX) |
                            static_cast<u32>(NF::BillboardedLockY) |
                            static_cast<u32>(NF::BillboardedLockZ) |
                            static_cast<u32>(NF::ModelSpace);
    const auto* raw = node.native.find("mdxFlagBits");
    u32 bits = (raw ? static_cast<u32>(raw->value) : 0u) & ~kShared;
    const auto map = [&](NodeFlags from, NF to) {
        if (hasFlag(node.flags, from))
            bits |= static_cast<u32>(to);
    };
    map(NodeFlags::DontInheritTranslation, NF::DontInheritTranslation);
    map(NodeFlags::DontInheritRotation, NF::DontInheritRotation);
    map(NodeFlags::DontInheritScale, NF::DontInheritScaling);
    map(NodeFlags::Billboarded, NF::Billboarded);
    map(NodeFlags::BillboardLockX, NF::BillboardedLockX);
    map(NodeFlags::BillboardLockY, NF::BillboardedLockY);
    map(NodeFlags::BillboardLockZ, NF::BillboardedLockZ);
    map(NodeFlags::ModelSpace, NF::ModelSpace);
    return bits;
}

Animator::Animator(const Document& document, u32 model)
    : Animator(document, model, GameOf(document.defaultProfile)) {}

Animator::Animator(const Document& document, u32 model, Game storage) {
    if (model >= document.models.size()) {
        return;
    }
    document_ = &document;
    model_ = &document.models[model];
    modelIndex_ = model;
    clips_.resize(document.clips.size());

    storage_ = storage;
    const std::vector<AnimChannel>& channels = model_->animChannels.channels;
    rests_.resize(channels.size());
    restKnown_.resize(channels.size(), 0);
    held_.resize(channels.size(), 0);
    for (std::size_t c = 0; c < channels.size(); ++c) {
        held_[c] = HeldByRenderer(document, model, channels[c]) ? 1 : 0;
    }
    const NodeTree& tree = model_->nodes;
    if (tree.rig == RigConvention::ExplicitBind) {
        inverseBinds_.resize(tree.size());
        for (u32 n = 0; n < tree.size(); ++n) {
            inverseBinds_[n] = tree.inverseBindMatrix(n);
        }
    }
}

Animator::Animator(const Document& document, u32 model, Rests rests) : Animator(document, model) {
    if (rests.size() == rests_.size()) {
        rests_ = std::move(rests);
        restKnown_.assign(rests_.size(), 1);
    }
}

Animator::Rests Animator::resolveRests() const {
    for (std::size_t c = 0; c < rests_.size(); ++c) {
        restOf(c);
    }
    return rests_;
}

const std::vector<u8>& Animator::restOf(std::size_t c) const {
    if (restKnown_[c] != 0) {
        return rests_[c];
    }
    restKnown_[c] = 1;
    const AnimChannel& channel = model_->animChannels.channels[c];
    TrackRests rests = RestsPlayed(*document_, modelIndex_, channel, storage_);
    // Which of the two applies costs a walk of every clip; most channels have
    // one rest and need none.
    rests_[c] = rests.differ() && KeyedAnywhere(*document_, modelIndex_, channel.id)
                    ? std::move(rests.keyedElsewhere)
                    : std::move(rests.unkeyed);
    return rests_[c];
}

const Animator::ClipLayers& Animator::layersOf(u32 clip) const {
    ClipLayers& entry = clips_[clip];
    if (entry.resolved) {
        return entry;
    }
    entry.resolved = true;
    const Clip& source = document_->clips[clip];
    if (source.trackSets.empty()) {
        entry.containers = &source.containers;
    } else {
        entry.owned = LayeredContainers(source, model_->trackSets);
        entry.containers = &entry.owned;
    }
    const std::vector<AnimChannel>& channels = model_->animChannels.channels;
    std::unordered_map<u32, u32> index;
    index.reserve(channels.size());
    for (std::size_t c = 0; c < channels.size(); ++c) {
        index.emplace(channels[c].id, static_cast<u32>(c));
    }
    entry.tracks.resize(entry.containers->size());
    for (std::size_t k = 0; k < entry.containers->size(); ++k) {
        std::vector<const SubTrack*>& slots = entry.tracks[k];
        slots.assign(channels.size(), nullptr);
        for (const SubTrack& track : (*entry.containers)[k].subTracks) {
            const auto found = index.find(track.channel);
            // The first sub-track of a channel in a container is the one read,
            // as `SubTrackContainer::find` answers.
            if (found != index.end() && slots[found->second] == nullptr) {
                slots[found->second] = &track;
            }
        }
    }
    return entry;
}

std::vector<Animator::Layer> Animator::layersFor(const Mix& mix) const {
    struct Entry {
        u32 clip;
        SampleWindow window;
        f32 weight;
    };
    std::vector<Entry> overlays;
    std::vector<Entry> hosts;
    std::vector<Entry> under;
    const u32 worldMs = static_cast<u32>(std::max(WholeMs(mix.worldSeconds), 0));
    if (mix.globals) {
        for (u32 c = 0; c < document_->clips.size(); ++c) {
            const Clip& clip = document_->clips[c];
            if (clip.model != modelIndex_ || !IsGlobalLoop(clip)) {
                continue;
            }
            // A concurrent global starts at init and so sits ahead of every
            // host play of its priority; one that is not is the model's own
            // animation and sits behind them (`ClipPlaylist`).
            Entry entry{c, ClipWindow(clip, 0, static_cast<i32>(worldMs)), 1.0f};
            (Concurrent(clip) ? overlays : under).push_back(entry);
        }
    }
    for (const Play& play : mix.plays) {
        if (play.clip >= document_->clips.size() || document_->clips[play.clip].model != modelIndex_) {
            continue;
        }
        const Clip& clip = document_->clips[play.clip];
        if (mix.globals && IsGlobalLoop(clip)) {
            continue; // Already under every play, on its own clock.
        }
        hosts.push_back(Entry{play.clip, HostWindow(clip, play), play.weight});
    }

    std::vector<Entry> plays;
    plays.insert(plays.end(), overlays.begin(), overlays.end());
    plays.insert(plays.end(), hosts.begin(), hosts.end());
    plays.insert(plays.end(), under.begin(), under.end());
    if (storage_ == Game::StarCraft && plays.size() > kMaxPlays) {
        plays.resize(kMaxPlays);
    }

    std::vector<Layer> layers;
    for (u32 p = 0; p < plays.size(); ++p) {
        const ClipLayers& clip = layersOf(plays[p].clip);
        for (std::size_t k = 0; k < clip.containers->size(); ++k) {
            const SubTrackContainer& container = (*clip.containers)[k];
            Layer layer;
            layer.play = p;
            layer.priority = container.priority;
            layer.transparent = container.concurrent;
            layer.tracks = &clip.tracks[k];
            layer.clip = &document_->clips[plays[p].clip];
            layer.window = plays[p].window;
            layer.weight = plays[p].weight;
            layers.push_back(layer);
        }
    }
    // Priority first, and stable so that a tie keeps the play order above.
    std::stable_sort(layers.begin(), layers.end(),
                     [](const Layer& a, const Layer& b) { return a.priority > b.priority; });
    return layers;
}

void Animator::sample(const Mix& mix, Pose& out, bool nodesOnly) const {
    if (model_ == nullptr) {
        out = Pose{};
        return;
    }
    const std::vector<AnimChannel>& channels = model_->animChannels.channels;
    const std::vector<Layer> layers = layersFor(mix);
    out.channelValues.assign(channels.size(), {});

    struct Contribution {
        std::vector<u8> value;
        f32 weight = 0;
    };
    std::vector<Contribution> contributions;
    contributions.reserve(kMaxContributions);
    u32 plays = 0;
    for (const Layer& layer : layers) {
        plays = std::max(plays, layer.play + 1);
    }
    std::vector<u8> visited;
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const AnimChannel& channel = channels[c];
        // A stage's own channels ride with the skeleton: the stages read them.
        if (nodesOnly && !IsNodeTransform(channel.target) && !IsStageChannel(channel.target)) {
            continue;
        }
        const std::vector<u8>& rest = restOf(c);
        // Discrete channels have no midpoint — a bone is not 40% visible — so
        // they take `SampleRefOverride`'s rule.
        const bool discrete = channel.valueType == geom::AttrType::U32 ||
                              channel.target.channel == Channel::Visibility;
        contributions.clear();
        f32 budget = kM3StartBudget;
        visited.assign(plays, 0);
        std::optional<std::vector<u8>> first; // A discrete channel's winner.
        for (const Layer& layer : layers) {
            if (visited[layer.play] != 0) {
                continue; // This play already contributed.
            }
            const SubTrack* track = (*layer.tracks)[c];
            std::optional<std::vector<u8>> read;
            bool unusable = false;
            if (track != nullptr && !track->times.empty()) {
                if (!track->wellSized(channel.valueType)) {
                    unusable = true;
                } else {
                    read = ReadTrack(*layer.clip, *track, channel.valueType, layer.window,
                                     held_[c] != 0);
                    // Only keys the rule reads count: a `Wc3` track keyed
                    // only outside its window is no track at all.
                    if (!read) {
                        track = nullptr;
                    }
                }
            }
            // A transparent layer with no track abstains: no value, no budget,
            // and the play stays free for a lower layer. An empty track is
            // not "no track" — it falls to the rest, as an empty M3 block does.
            if (track == nullptr && layer.transparent) {
                continue;
            }
            visited[layer.play] = 1;
            if (discrete) {
                // Override mode: the first contributor wins outright.
                first = read ? std::move(*read) : rest;
                break;
            }
            if (unusable) {
                // Bound but unusable: spends its weight and gives nothing.
                budget -= layer.weight;
                if (budget <= kM3BudgetEpsilon) {
                    break;
                }
                continue;
            }
            if (static_cast<int>(contributions.size()) < kMaxContributions) {
                contributions.push_back(Contribution{read ? std::move(*read) : rest, layer.weight});
            }
            budget -= layer.weight;
            if (budget <= kM3BudgetEpsilon) {
                break;
            }
        }

        std::vector<u8>& value = out.channelValues[c];
        if (discrete) {
            value = first ? std::move(*first) : rest;
            continue;
        }
        if (contributions.empty()) {
            value = rest;
            continue;
        }
        // Weights are relative: one contribution, or a budget barely touched,
        // shows the first at full strength.
        if (contributions.size() == 1 || budget > kM3SettledBudget) {
            value = std::move(contributions.front().value);
            continue;
        }
        // The overspend is absorbed by the lowest-priority contributor so the
        // weights sum to one; then combine lowest first.
        if (budget < 0.0f) {
            contributions.back().weight += budget;
        }
        value = contributions.back().value;
        f32 accumulated = contributions.back().weight;
        for (std::size_t i = contributions.size() - 1; i-- > 0;) {
            value = MixValues(channel.valueType, value, contributions[i].value,
                              M3SmoothstepFactor(accumulated, contributions[i].weight));
            accumulated += contributions[i].weight;
        }
    }
    place(out);
}

void Animator::place(Pose& out) const {
    if (model_ == nullptr) {
        return;
    }
    const std::vector<AnimChannel>& channels = model_->animChannels.channels;
    const NodeTree& tree = model_->nodes;
    const bool pivoted = tree.rig == RigConvention::PivotRelative;
    out.local.resize(tree.size());
    for (u32 n = 0; n < tree.size(); ++n) {
        out.local[n] = pivoted ? Transform{} : tree.nodes[n].local;
    }
    for (std::size_t c = 0; c < channels.size() && c < out.channelValues.size(); ++c) {
        const TrackTarget& target = channels[c].target;
        const std::vector<u8>& value = out.channelValues[c];
        if (!IsNodeTransform(target) || target.node >= tree.size()) {
            continue;
        }
        Transform& local = out.local[target.node];
        if (target.channel == Channel::Rotation && value.size() == sizeof(Quaternion)) {
            local.rotation = Load<Quaternion>(value.data());
        } else if (target.channel == Channel::Translation && value.size() == sizeof(Vector3f)) {
            local.translation = Load<Vector3f>(value.data());
        } else if (target.channel == Channel::Scale && value.size() == sizeof(Vector3f)) {
            local.scale = Load<Vector3f>(value.data());
        } else if (target.channel == Channel::Scale && value.size() == sizeof(f32)) {
            // A source that keys one float scales uniformly -- D3 is the case.
            const f32 s = Load<f32>(value.data());
            local.scale = Vector3f{s, s, s};
        }
    }
}

Animator::Composition Animator::composition() const {
    Composition out;
    if (model_ == nullptr) {
        return out;
    }
    const NodeTree& tree = model_->nodes;
    const u32 count = tree.size();
    const bool pivoted = tree.rig == RigConvention::PivotRelative;
    out.order.reserve(count);
    out.parent.assign(count, kInvalidNode);
    // Parents first, whatever the storage order: an import can leave a parent
    // after its child (a Hive rig numbers `Bip001 Pelvis` ahead of the bone it
    // hangs from), and composing in index order would take such a node for a
    // root. A cycle, which no renderer can compose either, is cut where found.
    enum : u8 { kTodo, kBusy, kDone };
    std::vector<u8> state(count, kTodo);
    std::vector<u32> stack;
    for (u32 root = 0; root < count; ++root) {
        stack.push_back(root);
        while (!stack.empty()) {
            const u32 i = stack.back();
            if (state[i] == kDone) {
                stack.pop_back();
                continue;
            }
            const Node& node = tree.nodes[i];
            const u32 parent = node.parent;
            const bool camera = pivoted && node.kind == NodeKind::Camera;
            // An M3 bone in model space is a root (`toM3`).
            const bool detached = !pivoted && hasFlag(node.flags, NodeFlags::ModelSpace);
            const bool hasParent = parent < count && parent != i && !camera && !detached &&
                                   !(pivoted && tree.nodes[parent].kind == NodeKind::Camera);
            if (hasParent && state[parent] == kTodo && state[i] == kTodo) {
                state[i] = kBusy;
                stack.push_back(parent);
                continue;
            }
            stack.pop_back();
            if (!camera && hasParent && state[parent] == kDone) {
                out.parent[i] = parent;
            }
            out.order.push_back(i);
            state[i] = kDone;
        }
    }
    return out;
}

void Animator::composeNode(Pose& pose, u32 i, u32 parent) const {
    const NodeTree& tree = model_->nodes;
    const bool pivoted = tree.rig == RigConvention::PivotRelative;
    const Node& node = tree.nodes[i];
    const Transform& trs = pose.local[i];
    // The pivot exactly as `toMdx` writes it.
    const auto pivotOf = [&](u32 n) {
        return pivoted ? tree.nodes[n].pivot : tree.worldBind(n).translation;
    };
    const bool rooted = parent < tree.size();
    if (pivoted && node.kind == NodeKind::Camera) {
        // Not in the hierarchy: its position, plus the translation keys the
        // renderer adds to it.
        pose.frame[i] = ToMatrix(tree.worldBind(i));
        pose.frame[i].data[3][0] += trs.translation.x;
        pose.frame[i].data[3][1] += trs.translation.y;
        pose.frame[i].data[3][2] += trs.translation.z;
        pose.skinning[i] = Matrix44f::identity();
    } else if (pivoted) {
        const Matrix44f parentFrame = rooted ? pose.frame[parent] : Matrix44f::identity();
        const Vector3f parentPivot = rooted ? pivotOf(parent) : Vector3f{0, 0, 0};
        const Vector3f pivot = pivotOf(i);
        pose.frame[i] = ComposeNode(parentFrame, parentPivot, pivot, trs.translation, trs.rotation,
                                    trs.scale, MdxNodeFlags(node))
                            .frame;
        pose.skinning[i] = SkinFromFrame(pose.frame[i], pivot);
    } else {
        const Matrix44f own = ToMatrix(trs);
        pose.frame[i] = rooted ? own * pose.frame[parent] : own;
        pose.skinning[i] = inverseBinds_[i] * pose.frame[i];
    }
}

void Animator::compose(Pose& pose) const {
    if (model_ == nullptr) {
        return;
    }
    const u32 count = model_->nodes.size();
    if (pose.local.size() != count) {
        pose.local.resize(count, Transform{});
    }
    pose.skinning.assign(count, Matrix44f::identity());
    pose.frame.assign(count, Matrix44f::identity());
    const Composition order = composition();
    for (const u32 node : order.order) {
        composeNode(pose, node, order.parent[node]);
    }
}

void Animator::evaluate(const Mix& mix, Pose& out) const {
    sample(mix, out);
    compose(out);
}

} // namespace wem
} // namespace models
} // namespace whiteout

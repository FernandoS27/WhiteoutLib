// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/reduce.h>

#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/nodes/references.h>
#include <whiteout/models/wem/rigging/engine_names.h>
#include <whiteout/models/wem/skinning/fit.h>

#include "../geometry/fracture/parallel.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

namespace {

using geom::fracture::Parallel;

/// One skinned vertex: where it starts, how it was weighted, how it is now,
/// and how far the change puts it from where it went.
struct Point {
    u32 mesh = 0;
    u32 vertex = 0;
    Vector3f rest{0, 0, 0};
    std::vector<geom::Influence> truth;
    std::vector<geom::Influence> now;
    f64 error = 0.0; ///< Squared distance from where it went, summed over the frames.
    f32 worst = 0.0f;
};

/// What dropping one bone costs, and the weights it leaves.
struct Option {
    bool valid = false;
    f64 cost = 0.0;
    f32 worst = 0.0f;
    std::vector<std::vector<geom::Influence>> next; ///< Per point it moves, in `moves` order.
    std::vector<f64> error;
    std::vector<f32> worstOf;
};

std::vector<geom::Influence> Normalised(std::span<const geom::Influence> influences, u32 nodes) {
    std::vector<geom::Influence> out;
    f32 total = 0.0f;
    for (const geom::Influence& influence : influences) {
        if (influence.weight > 0.0f && influence.bone < nodes) {
            out.push_back(influence);
            total += influence.weight;
        }
    }
    for (geom::Influence& influence : out) {
        influence.weight /= total;
    }
    return out;
}

} // namespace

std::vector<u8> ReducibleBones(const Document& document, u32 model) {
    if (model >= document.models.size()) {
        return {};
    }
    const Model& owner = document.models[model];
    const NodeTree& tree = owner.nodes;
    const u32 nodes = tree.size();
    std::vector<u8> out(nodes, 0);
    // A billboard turns to the eye as it draws, which no sampled pose shows.
    constexpr NodeFlags kBillboard = NodeFlags::Billboarded | NodeFlags::BillboardLockX | NodeFlags::BillboardLockY |
                                     NodeFlags::BillboardLockZ;
    for (u32 n = 0; n < nodes; ++n) {
        const Node& node = tree.nodes[n];
        out[n] = !node.removed && node.kind == NodeKind::Bone && !IsWarcraftEngineBoneName(node.name) &&
                         !IsStarCraftEngineBoneName(node.name) && node.native.value("keyBoneId", -1) < 0 &&
                         (node.flags & kBillboard) == NodeFlags::None && node.sc2Billboards.empty() &&
                         !node.sc2ShadowBox.has_value()
                     ? 1
                     : 0;
    }
    // A stage, a body, a cloth, a region, a rigid section or an event holds it:
    // it stays.
    const auto held = [&](NodeReference row, const u32& node, u32, u32) {
        if (row != NodeReference::Influence && row != NodeReference::Channel && node < nodes) {
            out[node] = 0;
        }
    };
    ForEachNodeReference(tree, std::span<const Mesh>(owner.meshes), &owner.animChannels, &owner.poseStages,
                         &owner.physics, std::span<const Clip>(), held);
    for (u32 c = 0; c < document.clips.size(); ++c) {
        if (document.clips[c].model == model) {
            ForEachClipNodeReference(document.clips[c], c, held);
        }
    }
    return out;
}

BoneReducePlan PlanBoneReduction(const Document& document, u32 model, const BoneReduceOptions& options) {
    BoneReducePlan plan;
    if (model >= document.models.size()) {
        return plan;
    }
    const Model& owner = document.models[model];
    const NodeTree& tree = owner.nodes;
    const u32 nodes = tree.size();
    plan.weights.resize(owner.meshes.size());
    std::vector<u8> droppable = ReducibleBones(document, model);
    for (const u32 n : options.locked) {
        if (n < nodes) {
            droppable[n] = 0;
        }
    }
    std::vector<u32> children(nodes, 0);
    for (u32 n = 0; n < nodes; ++n) {
        const Node& node = tree.nodes[n];
        plan.bonesBefore += !node.removed && node.kind == NodeKind::Bone ? 1u : 0u;
        if (!node.removed && node.parent < nodes) {
            ++children[node.parent];
        }
    }
    plan.bonesAfter = plan.bonesBefore;
    const f32 height = ModelHeight(owner);
    const bool bounded = options.keep > 0 || options.rmsShare > 0.0f || options.worstShare > 0.0f;
    if (!bounded || nodes == 0) {
        return plan;
    }

    // Every clip sampled, and the rest: each frame's skinning of every node.
    std::vector<u32> clips;
    f32 seconds = 0.0f;
    for (u32 k = 0; k < document.clips.size(); ++k) {
        if (document.clips[k].model == model && document.clips[k].duration > 0.0f) {
            clips.push_back(k);
            seconds += document.clips[k].duration;
        }
    }
    // Some 3000 frames at most: a long model is measured more sparsely.
    const f32 rate = seconds * options.rate > 3000.0f ? 3000.0f / seconds : options.rate;
    owner.nodes.ensureHierarchy();
    const Animator::Rests rests = Animator(document, model).resolveRests();
    std::vector<std::vector<Matrix44f>> sampled(clips.size() + 1);
    Parallel(clips.size() + 1, options.threads, [&](std::size_t job) {
        Animator animator(document, model, rests);
        animator.keepTracksPrepared();
        const auto take = [&](const Mix& mix) {
            Pose pose;
            animator.sample(mix, pose, true);
            animator.compose(pose);
            for (u32 n = 0; n < nodes; ++n) {
                sampled[job].push_back(n < pose.skinning.size() ? pose.skinning[n] : Matrix44f::identity());
            }
        };
        if (job == 0) {
            take(Mix{});
            return;
        }
        const u32 k = clips[job - 1];
        const f32 duration = document.clips[k].duration;
        const u32 steps = std::max<u32>(1, static_cast<u32>(std::ceil(duration * rate - 1e-3f)));
        for (u32 i = 0; i <= steps; ++i) {
            Mix mix;
            mix.plays.push_back(Play{k, duration * static_cast<f32>(i) / static_cast<f32>(steps), 1.0f, false});
            mix.globals = false;
            take(mix);
        }
    });
    std::vector<Matrix44f> palette;
    for (const std::vector<Matrix44f>& frames : sampled) {
        palette.insert(palette.end(), frames.begin(), frames.end());
    }
    const u32 frames = static_cast<u32>(palette.size() / nodes);

    // Every skinned vertex, and which vertices each node moves.
    std::vector<Point> points;
    for (u32 m = 0; m < owner.meshes.size(); ++m) {
        const Mesh& mesh = owner.meshes[m];
        const auto positions = mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        for (u32 v = 0; v < mesh.skin.vertexCount() && v < positions.size(); ++v) {
            std::vector<geom::Influence> influences = Normalised(mesh.skin.forVertex(v), nodes);
            if (influences.empty()) {
                continue;
            }
            Point point;
            point.mesh = m;
            point.vertex = v;
            point.rest = positions[v];
            point.truth = influences;
            point.now = std::move(influences);
            points.push_back(std::move(point));
        }
    }
    std::vector<std::vector<u32>> moves(nodes);
    for (u32 p = 0; p < points.size(); ++p) {
        for (const geom::Influence& influence : points[p].now) {
            moves[influence.bone].push_back(p);
        }
    }

    // What dropping @p bone costs: its vertices weighted again over their other
    // bones and its parent.
    const auto evaluate = [&](u32 bone) {
        Option option;
        const u32 parent = tree.nodes[bone].parent;
        option.valid = true;
        for (const u32 p : moves[bone]) {
            const Point& point = points[p];
            std::vector<u32> candidates;
            for (const geom::Influence& influence : point.now) {
                if (influence.bone != bone) {
                    candidates.push_back(influence.bone);
                }
            }
            if (parent < nodes && std::find(candidates.begin(), candidates.end(), parent) == candidates.end()) {
                candidates.push_back(parent);
            }
            if (candidates.empty()) {
                option.valid = false;
                return option;
            }
            std::sort(candidates.begin(), candidates.end());
            const PointFit fit =
                FitPointWeights(point.rest, point.truth, palette, nodes, frames, candidates, options.maxInfluences);
            std::vector<geom::Influence> next;
            for (std::size_t c = 0; c < candidates.size(); ++c) {
                if (fit.weights[c] > 1e-6f) {
                    next.push_back(geom::Influence{candidates[c], fit.weights[c]});
                }
            }
            std::sort(next.begin(), next.end(),
                      [](const geom::Influence& a, const geom::Influence& b) { return a.weight > b.weight; });
            option.cost += fit.error - point.error;
            option.worst = std::max(option.worst, fit.worst);
            option.next.push_back(std::move(next));
            option.error.push_back(fit.error);
            option.worstOf.push_back(fit.worst);
        }
        return option;
    };

    std::vector<Option> costs(nodes);
    std::vector<u8> stale(nodes, 1), gone(nodes, 0);
    f64 total = 0.0;
    f32 worst = 0.0f;
    const f64 samples = std::max<f64>(1.0, static_cast<f64>(points.size()) * frames);
    for (;;) {
        if (options.keep > 0 && plan.bonesAfter <= options.keep) {
            break;
        }
        if (options.cancelled && options.cancelled()) {
            plan.cancelled = true;
            break;
        }
        std::vector<u32> open;
        for (u32 n = 0; n < nodes; ++n) {
            if (droppable[n] != 0 && gone[n] == 0 && children[n] == 0) {
                open.push_back(n);
            }
        }
        std::vector<u32> fresh;
        for (const u32 n : open) {
            if (stale[n] != 0) {
                fresh.push_back(n);
            }
        }
        Parallel(fresh.size(), options.threads, [&](std::size_t i) { costs[fresh[i]] = evaluate(fresh[i]); });
        for (const u32 n : fresh) {
            stale[n] = 0;
        }
        u32 best = nodes;
        for (const u32 n : open) {
            if (costs[n].valid && (best == nodes || costs[n].cost < costs[best].cost)) {
                best = n;
            }
        }
        if (best == nodes) {
            break;
        }
        const Option& option = costs[best];
        const f64 after = std::max(0.0, total + option.cost);
        const f32 rms = static_cast<f32>(std::sqrt(after / samples));
        const f32 farthest = std::max(worst, option.worst);
        if ((options.rmsShare > 0.0f && rms > options.rmsShare * height) ||
            (options.worstShare > 0.0f && farthest > options.worstShare * height)) {
            break;
        }
        // The drop: its vertices weighted anew, the bones they leave and join
        // judged again.
        const std::vector<u32> affected = moves[best];
        for (std::size_t i = 0; i < affected.size(); ++i) {
            Point& point = points[affected[i]];
            for (const geom::Influence& influence : point.now) {
                stale[influence.bone] = 1;
                std::erase(moves[influence.bone], affected[i]);
            }
            point.now = option.next[i];
            point.error = option.error[i];
            point.worst = option.worstOf[i];
            for (const geom::Influence& influence : point.now) {
                stale[influence.bone] = 1;
                moves[influence.bone].push_back(affected[i]);
            }
        }
        total = after;
        worst = farthest;
        gone[best] = 1;
        plan.dropped.push_back(best);
        --plan.bonesAfter;
        if (const u32 parent = tree.nodes[best].parent; parent < nodes) {
            --children[parent];
            stale[parent] = 1;
        }
    }
    for (Point& point : points) {
        bool same = point.now.size() == point.truth.size();
        for (std::size_t i = 0; same && i < point.now.size(); ++i) {
            same = point.now[i].bone == point.truth[i].bone && point.now[i].weight == point.truth[i].weight;
        }
        if (!same) {
            plan.weights[point.mesh].emplace_back(point.vertex, std::move(point.now));
        }
    }
    plan.rms = static_cast<f32>(std::sqrt(total / samples));
    plan.worst = worst;
    return plan;
}

BoneReduceOptions BoneReduceTier(u32 tier, u32 keep) {
    // The RMS and the farthest a vertex may stray, of the model's height.
    static constexpr f32 kTiers[4][2] = {{0.005f, 0.02f}, {0.0025f, 0.01f}, {0.001f, 0.005f}, {0.0005f, 0.002f}};
    const f32* chosen = kTiers[std::min(tier, 3u)];
    BoneReduceOptions options;
    options.rmsShare = chosen[0];
    options.worstShare = chosen[1];
    options.keep = keep;
    return options;
}

bool ApplyBoneReduction(Document& document, u32 model, const BoneReducePlan& plan) {
    if (model >= document.models.size() || plan.dropped.empty()) {
        return false;
    }
    Model& owner = document.models[model];
    for (std::size_t m = 0; m < plan.weights.size() && m < owner.meshes.size(); ++m) {
        for (const auto& [vertex, influences] : plan.weights[m]) {
            if (vertex < owner.meshes[m].skin.vertexCount()) {
                owner.meshes[m].skin.assignVertex(vertex, influences);
            }
        }
    }
    // A removed node's channels are left behind by the removal: they go first.
    std::vector<u8> dropped(owner.nodes.size(), 0);
    for (const u32 node : plan.dropped) {
        if (node < dropped.size()) {
            dropped[node] = 1;
        }
    }
    std::vector<u32> channels;
    for (const AnimChannel& channel : owner.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node < dropped.size() &&
            dropped[channel.target.node] != 0) {
            channels.push_back(channel.id);
        }
    }
    EraseChannels(document, model, channels);
    // Leaves only: nothing is reparented, so no survivor has a world to keep.
    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(owner.meshes);
    referencers.channels = &owner.animChannels;
    referencers.stages = &owner.poseStages;
    referencers.physics = &owner.physics;
    for (const u32 node : plan.dropped) {
        if (node < owner.nodes.size() && !IsNodeRemoved(owner.nodes, node)) {
            RemoveNode(owner.nodes, node, RemovePolicy::ReparentChildren, SkinPolicy::ReassignToParent,
                       /*preserveWorld=*/true, referencers);
        }
    }
    Diagnostics diagnostics;
    const NodeRemaps remaps = CompactNodes(owner.nodes, referencers, diagnostics);
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (ClipEvent& event : clip.events) {
            if (event.node != kInvalidNode) {
                event.node = event.node < remaps.nodes.size() ? remaps.nodes[event.node] : kInvalidNode;
            }
        }
        RemapClipWorldNodes(clip, remaps.nodes);
    }
    return true;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

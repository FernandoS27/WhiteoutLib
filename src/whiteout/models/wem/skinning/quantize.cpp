// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/skinning/quantize.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace whiteout {
namespace models {
namespace wem {
namespace skinning {

std::array<u8, 4> QuantizeWeights(const std::array<f32, 4>& weights, std::size_t count) {
    std::array<u8, 4> out{0, 0, 0, 0};
    count = std::min<std::size_t>(count, 4);
    f32 total = 0.0f;
    for (std::size_t k = 0; k < count; ++k) {
        total += weights[k];
    }
    if (count == 0 || !(total > 0.0f)) {
        return out;
    }
    std::array<f32, 4> remainder{};
    u32 assigned = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const f32 exact = std::max(weights[k], 0.0f) / total * 255.0f;
        out[k] = static_cast<u8>(std::clamp(std::floor(exact + 1e-3f), 0.0f, 255.0f));
        remainder[k] = exact - static_cast<f32>(out[k]);
        assigned += out[k];
    }
    // What rounding down left goes to the largest remainders, the earlier slot
    // on a tie. The epsilon can overshoot by at most count * 1e-3 of a unit, so
    // the sum cannot pass 255; the guard below is for a caller's NaN.
    while (assigned < 255) {
        std::size_t best = 0;
        for (std::size_t k = 1; k < count; ++k) {
            if (remainder[k] > remainder[best]) {
                best = k;
            }
        }
        if (out[best] == 255) {
            break;
        }
        ++out[best];
        remainder[best] -= 1.0f;
        ++assigned;
    }
    return out;
}

namespace {

constexpr u32 kNoNode = ~0u;

struct Share {
    u32 node = 0;
    f32 weight = 0.0f;
};

/// Step 1: a vertex's shares, merged, normalised, pruned and sorted heaviest
/// first. @p full receives them before the prune, normalised, for the
/// `snapped` count.
std::vector<Share> Gather(std::span<const geom::Influence> influences, f32 prune,
                          std::vector<Share>& full) {
    std::vector<Share> shares;
    for (const geom::Influence& influence : influences) {
        if (!(influence.weight > 0.0f) || !std::isfinite(influence.weight)) {
            continue;
        }
        const auto same = std::find_if(shares.begin(), shares.end(), [&](const Share& s) {
            return s.node == influence.bone;
        });
        if (same != shares.end()) {
            same->weight += influence.weight;
        } else {
            shares.push_back({influence.bone, influence.weight});
        }
    }
    const auto normalise = [](std::vector<Share>& list) {
        f32 total = 0.0f;
        for (const Share& s : list) {
            total += s.weight;
        }
        if (total > 1e-7f) {
            for (Share& s : list) {
                s.weight /= total;
            }
        }
    };
    normalise(shares);
    full = shares;
    // Even the smallest share a group holds is 1/8, far above the prune, so
    // this never drops a bone an imported classic group used.
    std::erase_if(shares, [prune](const Share& s) { return s.weight < prune; });
    normalise(shares);
    std::sort(shares.begin(), shares.end(), [](const Share& a, const Share& b) {
        if (a.weight != b.weight) {
            return a.weight > b.weight;
        }
        return a.node < b.node;
    });
    return shares;
}

/// Step 2: how many of the heaviest shares the vertex keeps.
u32 SnapCount(const std::vector<Share>& shares, u32 maxBones) {
    const u32 most = std::min<u32>(static_cast<u32>(shares.size()), maxBones);
    u32 best = 1;
    f32 bestCost = std::numeric_limits<f32>::max();
    f32 prefix = 0.0f;
    for (u32 k = 1; k <= most; ++k) {
        prefix += shares[k - 1].weight;
        const f32 cost = (1.0f - 2.0f * prefix) / static_cast<f32>(k);
        // Strict, so a tie keeps the smaller group.
        if (cost < bestCost - 1e-6f) {
            bestCost = cost;
            best = k;
        }
    }
    return best;
}

/// The squared error of @p group's equal split against @p shares, less the
/// `Σw²` every group shares: `1/k − 2·S/k`, where `S` is the weight the
/// vertex has on the group's bones.
f32 GroupCost(const std::vector<Share>& shares, const std::vector<u32>& group) {
    f32 inside = 0.0f;
    for (const Share& s : shares) {
        if (std::binary_search(group.begin(), group.end(), s.node)) {
            inside += s.weight;
        }
    }
    const f32 k = static_cast<f32>(group.size());
    return (1.0f - 2.0f * inside) / k;
}

} // namespace

ClassicSkin QuantizeClassic(std::span<const std::vector<geom::Influence>> vertices,
                            std::span<const u16> pins, const ClassicLimits& limits) {
    ClassicSkin result;
    const std::size_t count = vertices.size();
    const u32 maxBones = std::max<u32>(limits.maxBones, 1);

    std::vector<std::vector<Share>> gathered(count);
    std::vector<std::vector<Share>> full(count);
    u32 firstBone = kNoNode;
    for (std::size_t v = 0; v < count; ++v) {
        gathered[v] = Gather(vertices[v], limits.prune, full[v]);
        result.wide += gathered[v].size() > maxBones ? 1 : 0;
        if (firstBone == kNoNode && !gathered[v].empty()) {
            firstBone = gathered[v].front().node;
        }
    }
    if (firstBone == kNoNode) {
        return result;
    }

    // Step 2, and the distinct groups in first-use order.
    std::map<std::vector<u32>, u32> indexOf;
    std::vector<u32> usage;
    std::vector<bool> pinned;
    result.groupOf.assign(count, 0);
    for (std::size_t v = 0; v < count; ++v) {
        const std::vector<Share>& shares = gathered[v];
        const u16 pin = v < pins.size() ? pins[v] : u16{0};
        std::vector<u32> group;
        if (shares.empty()) {
            group.push_back(firstBone);
        } else {
            const u32 keep = pin != 0 ? std::min<u32>({pin, static_cast<u32>(shares.size()),
                                                       maxBones})
                                      : SnapCount(shares, maxBones);
            for (u32 k = 0; k < keep; ++k) {
                group.push_back(shares[k].node);
            }
            std::sort(group.begin(), group.end());
        }
        const auto [entry, added] = indexOf.try_emplace(group, static_cast<u32>(result.groups.size()));
        if (added) {
            result.groups.push_back(std::move(group));
            usage.push_back(0);
            pinned.push_back(false);
        }
        result.groupOf[v] = entry->second;
        ++usage[entry->second];
        if (pin != 0) {
            pinned[entry->second] = true;
        }
    }

    // Step 3. Every vertex keeps its own current group, so a vertex moved into
    // a group that is later merged away moves again on its own weights. The
    // survivors are numbered once, below: renumbering during the merges once
    // sent WhiteoutDex's vertices to an unrelated bone set.
    const std::size_t total = result.groups.size();
    std::vector<bool> alive(total, true);
    std::size_t aliveCount = total;
    if (aliveCount > limits.maxGroups) {
        std::vector<std::vector<u32>> members(total);
        for (std::size_t v = 0; v < count; ++v) {
            members[result.groupOf[v]].push_back(static_cast<u32>(v));
        }
        std::vector<bool> moved(count, false);
        while (aliveCount > limits.maxGroups) {
            std::size_t victim = total;
            for (std::size_t g = 0; g < total; ++g) {
                if (alive[g] && !pinned[g] && (victim == total || usage[g] < usage[victim])) {
                    victim = g;
                }
            }
            if (victim == total) {
                result.pinsOverflow = true;
                break;
            }
            alive[victim] = false;
            --aliveCount;
            ++result.mergedGroups;
            for (const u32 v : members[victim]) {
                std::size_t target = total;
                f32 best = std::numeric_limits<f32>::max();
                for (std::size_t g = 0; g < total; ++g) {
                    if (!alive[g]) {
                        continue;
                    }
                    const f32 cost = GroupCost(gathered[v], result.groups[g]);
                    if (cost < best - 1e-6f) {
                        best = cost;
                        target = g;
                    }
                }
                result.groupOf[v] = static_cast<u32>(target);
                members[target].push_back(v);
                ++usage[target];
                moved[v] = true;
            }
            members[victim].clear();
            usage[victim] = 0;
        }
        result.merged = static_cast<u32>(std::count(moved.begin(), moved.end(), true));

        std::vector<u32> compact(total, kNoNode);
        std::vector<std::vector<u32>> survivors;
        for (std::size_t g = 0; g < total; ++g) {
            if (alive[g]) {
                compact[g] = static_cast<u32>(survivors.size());
                survivors.push_back(std::move(result.groups[g]));
            }
        }
        result.groups = std::move(survivors);
        for (u32& group : result.groupOf) {
            group = compact[group];
        }
    }

    // A vertex is snapped when what the game draws for it is not its weights.
    constexpr f32 kByte = 2.0f / 255.0f;
    for (std::size_t v = 0; v < count; ++v) {
        const std::vector<u32>& group = result.groups[result.groupOf[v]];
        const f32 share = 1.0f / static_cast<f32>(group.size());
        bool differs = full[v].empty();
        for (const Share& s : full[v]) {
            const bool inside = std::binary_search(group.begin(), group.end(), s.node);
            differs = differs || std::abs(s.weight - (inside ? share : 0.0f)) > kByte;
        }
        for (const u32 node : group) {
            const bool held = std::any_of(full[v].begin(), full[v].end(),
                                          [node](const Share& s) { return s.node == node; });
            differs = differs || (!held && share > kByte);
        }
        result.snapped += differs ? 1 : 0;
    }
    return result;
}

} // namespace skinning
} // namespace wem
} // namespace models
} // namespace whiteout

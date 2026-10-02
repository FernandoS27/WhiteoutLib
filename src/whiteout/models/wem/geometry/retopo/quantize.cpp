// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/retopo/quantize.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

namespace {

constexpr f64 kInfinity = std::numeric_limits<f64>::infinity();

/// A directed arc of the cover, with a convex cost on its flow: quadratic about
/// `centre`, plus `penalty` for each unit under one.
struct CoverArc {
    u32 from = 0;
    u32 to = 0;
    f64 weight = 0.0; ///< 0 for the free node's pass-through.
    f64 centre = 0.0;
    f64 penalty = 0.0;
    i32 lower = 0;
    i32 upper = kUnbounded;
    i32 flow = 0;

    f64 cost(i32 f) const {
        const f64 off = f - centre;
        return weight * off * off + penalty * std::max(0.0, 1.0 - f);
    }
    /// The cost of one more unit, and of one less.
    f64 up() const {
        return cost(flow + 1) - cost(flow);
    }
    f64 down() const {
        return cost(flow - 1) - cost(flow);
    }
    /// The integer flow in the bounds the cost is least at.
    i32 best() const {
        if (weight <= 0.0 && penalty <= 0.0) {
            return std::clamp(0, lower, upper);
        }
        i32 f = static_cast<i32>(std::clamp(std::round(centre), static_cast<f64>(lower),
                                            static_cast<f64>(std::min(upper, 1 << 30))));
        while (f > lower && cost(f - 1) < cost(f)) {
            --f;
        }
        while (f < upper && cost(f + 1) < cost(f)) {
            ++f;
        }
        return f;
    }
};

/// Where an endpoint of a bidirected edge sends and takes flow in the cover: a
/// head takes it at the node's first copy, a tail gives it from there, and the
/// second copy mirrors both.
u32 Leaves(u32 node, bool head) {
    return 2 * node + (head ? 1 : 0);
}
u32 Enters(u32 node, bool head) {
    return 2 * node + (head ? 0 : 1);
}

} // namespace

QuantizeSolution SolveQuantizeDoubleCover(const QuantizeProblem& problem) {
    QuantizeSolution solution;
    const u32 nodes = problem.nodeCount * 2;
    std::vector<CoverArc> arcs;
    arcs.reserve(problem.arcs.size() * 2 + 2);
    for (const QuantizeArc& arc : problem.arcs) {
        // Each copy aims at half the target and costs twice as much per unit off
        // it, so a symmetric flow costs exactly what the edge does. The lower
        // bound and the penalty for short go to the first copy as far as its
        // upper bound lets them: `x >= 1` cannot be split into two integer
        // halves without forcing two.
        if (arc.lower > arc.upper) {
            return solution;
        }
        const f64 weight = 2.0 * arc.weight;
        const f64 centre = arc.target * 0.5;
        const i32 upperFirst = arc.upper == kUnbounded ? kUnbounded : arc.upper - arc.upper / 2;
        const i32 upperSecond = arc.upper == kUnbounded ? kUnbounded : arc.upper / 2;
        const i32 lowerFirst = std::min(arc.lower, upperFirst);
        arcs.push_back(CoverArc{Leaves(arc.nodeA, arc.headA), Enters(arc.nodeB, arc.headB), weight, centre,
                                arc.zeroPenalty, lowerFirst, upperFirst, 0});
        arcs.push_back(CoverArc{Leaves(arc.nodeB, arc.headB), Enters(arc.nodeA, arc.headA), weight, centre, 0.0,
                                arc.lower - lowerFirst, upperSecond, 0});
    }
    if (problem.freeNode < problem.nodeCount) {
        // A double-tail and a double-head self-loop: the node may give or take
        // any even amount, which in the cover is its two copies joined.
        const u32 plus = 2 * problem.freeNode;
        arcs.push_back(CoverArc{plus, plus + 1, 0.0, 0.0, 0.0, 0, kUnbounded, 0});
        arcs.push_back(CoverArc{plus + 1, plus, 0.0, 0.0, 0.0, 0, kUnbounded, 0});
    }

    // Every arc starts at its own optimum, so every residual move costs at least
    // nothing; what that leaves unbalanced is routed by shortest paths.
    std::vector<i64> excess(nodes, 0);
    for (CoverArc& arc : arcs) {
        arc.flow = arc.best();
        excess[arc.to] += arc.flow;
        excess[arc.from] -= arc.flow;
    }

    std::vector<std::vector<u32>> adjacency(nodes);
    for (u32 a = 0; a < arcs.size(); ++a) {
        adjacency[arcs[a].from].push_back(a * 2);
        adjacency[arcs[a].to].push_back(a * 2 + 1);
    }

    std::vector<f64> potential(nodes, 0.0);
    std::vector<f64> distance(nodes);
    std::vector<u32> via(nodes);
    using Entry = std::pair<f64, u32>;
    for (;;) {
        bool any = false;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        std::fill(distance.begin(), distance.end(), kInfinity);
        std::fill(via.begin(), via.end(), 0xFFFFFFFFu);
        for (u32 n = 0; n < nodes; ++n) {
            if (excess[n] > 0) {
                any = true;
                distance[n] = 0.0;
                open.push({0.0, n});
            }
        }
        if (!any) {
            break;
        }
        u32 sink = 0xFFFFFFFFu;
        f64 reach = kInfinity;
        while (!open.empty()) {
            const auto [d, n] = open.top();
            open.pop();
            if (d > distance[n]) {
                continue;
            }
            if (excess[n] < 0) {
                sink = n;
                reach = d;
                break;
            }
            for (u32 entry : adjacency[n]) {
                const CoverArc& arc = arcs[entry / 2];
                const bool forward = (entry & 1) == 0;
                if (forward ? arc.flow >= arc.upper : arc.flow <= arc.lower) {
                    continue;
                }
                const u32 other = forward ? arc.to : arc.from;
                const f64 cost = (forward ? arc.up() : arc.down()) + potential[n] - potential[other];
                const f64 next = d + std::max(cost, 0.0);
                if (next < distance[other]) {
                    distance[other] = next;
                    via[other] = entry;
                    open.push({next, other});
                }
            }
        }
        if (sink == 0xFFFFFFFFu) {
            return solution;
        }
        for (u32 n = 0; n < nodes; ++n) {
            potential[n] += std::min(distance[n], reach);
        }
        u32 at = sink;
        excess[sink] += 1;
        while (via[at] != 0xFFFFFFFFu) {
            const u32 entry = via[at];
            CoverArc& arc = arcs[entry / 2];
            if ((entry & 1) == 0) {
                arc.flow += 1;
                at = arc.from;
            } else {
                arc.flow -= 1;
                at = arc.to;
            }
        }
        excess[at] -= 1;
    }

    solution.lengths.resize(problem.arcs.size());
    for (u32 e = 0; e < problem.arcs.size(); ++e) {
        solution.lengths[e] = arcs[2 * e].flow + arcs[2 * e + 1].flow;
    }
    solution.solved = QuantizeFeasible(problem, solution.lengths);
    solution.cost = QuantizeCost(problem, solution.lengths);
    return solution;
}

f64 QuantizeCost(const QuantizeProblem& problem, const std::vector<i32>& lengths) {
    f64 cost = 0.0;
    for (u32 e = 0; e < problem.arcs.size() && e < lengths.size(); ++e) {
        const QuantizeArc& arc = problem.arcs[e];
        const f64 off = lengths[e] - arc.target;
        cost += arc.weight * off * off + arc.zeroPenalty * std::max(0.0, 1.0 - lengths[e]);
    }
    return cost;
}

bool QuantizeFeasible(const QuantizeProblem& problem, const std::vector<i32>& lengths) {
    if (lengths.size() != problem.arcs.size()) {
        return false;
    }
    std::vector<i64> balance(problem.nodeCount, 0);
    for (u32 e = 0; e < problem.arcs.size(); ++e) {
        const QuantizeArc& arc = problem.arcs[e];
        const i32 x = lengths[e];
        if (x < arc.lower || x > arc.upper || arc.nodeA >= problem.nodeCount ||
            arc.nodeB >= problem.nodeCount) {
            return false;
        }
        balance[arc.nodeA] += arc.headA ? x : -x;
        balance[arc.nodeB] += arc.headB ? x : -x;
    }
    for (u32 n = 0; n < problem.nodeCount; ++n) {
        if (n == problem.freeNode) {
            if (balance[n] % 2 != 0) {
                return false;
            }
        } else if (balance[n] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

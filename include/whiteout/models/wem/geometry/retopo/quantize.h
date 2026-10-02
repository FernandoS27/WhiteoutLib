// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file quantize.h
 * @brief The integer side lengths of a quad layout, as a min-deviation flow
 *        (EDIT_MODE_RETOPOLOGY_DESIGN.md §1.7).
 *
 * A layout's patches are rectangles in the field, so each one asks that its
 * opposite sides hold the same number of quads. With a node per patch and
 * direction and an edge per arc, that is flow conservation on a bidirected
 * graph: an arc on a patch's first or second side has a head at that patch's
 * node, one on its third or fourth a tail (Heistermann, Warnett & Bommes,
 * "Min-Deviation-Flow in Bi-directed Graphs for T-Mesh Quantization", 2023).
 *
 * The library states the problem and stays free of a solver: the caller passes
 * one (`QuantizeSolver`), and the viewer's is libSatsuma's exact one. With none,
 * `SolveQuantizeDoubleCover` answers -- a correct flow from the paper's
 * double-cover relaxation, coarser than the exact solve.
 */

#include <functional>
#include <limits>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace retopo {

/// No upper bound on an arc's length.
inline constexpr i32 kUnbounded = std::numeric_limits<i32>::max();

/// One arc: a bidirected edge between two nodes, and what its length should be.
struct QuantizeArc {
    u32 nodeA = 0;
    u32 nodeB = 0;
    bool headA = true; ///< The edge has its head at `nodeA`: its flow enters that node.
    bool headB = false;
    f64 target = 1.0;  ///< The ideal length in quads, from the arc's length over the target edge.
    f64 weight = 1.0;  ///< Cost is `weight * (length - target)^2`...
    i32 lower = 1;
    i32 upper = kUnbounded;
    /// ... plus `zeroPenalty * max(0, 1 - length)`: what collapsing the arc
    /// costs when `lower` lets it (§1.7). Convex, as every cost must be.
    f64 zeroPenalty = 0.0;
};

/**
 * @brief A bidirected flow problem: every node conserves -- the sum over the
 *        edges with a head at it equals the sum over those with a tail -- except
 *        `freeNode`, which may take any even imbalance (the mesh border, where a
 *        quad mesh always has an even number of edges).
 */
struct QuantizeProblem {
    u32 nodeCount = 0;
    u32 freeNode = 0xFFFFFFFFu; ///< None when every node conserves.
    std::vector<QuantizeArc> arcs;
};

struct QuantizeSolution {
    bool solved = false;
    std::vector<i32> lengths; ///< Per arc.
    f64 cost = 0.0;
};

/// A solver for `QuantizeProblem`. It must honour every bound and every node, or
/// return `solved = false`.
using QuantizeSolver = std::function<QuantizeSolution(const QuantizeProblem&)>;

/**
 * @brief The double-cover relaxation, solved exactly: each node becomes two,
 *        each edge two directed arcs aiming at half its target, and a min-cost
 *        flow over that cover (successive shortest paths on the arcs' marginal
 *        costs) projects back to an integer flow that conserves at every node.
 *
 * Optimal for the cover, not for the problem: the paper measures the exact
 * solve about a quarter lower in cost. Unsolved when no flow meets the bounds.
 */
QuantizeSolution SolveQuantizeDoubleCover(const QuantizeProblem& problem);

/// The problem's cost at @p lengths, and whether they meet every bound and node.
f64 QuantizeCost(const QuantizeProblem& problem, const std::vector<i32>& lengths);
bool QuantizeFeasible(const QuantizeProblem& problem, const std::vector<i32>& lengths);

} // namespace retopo
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout

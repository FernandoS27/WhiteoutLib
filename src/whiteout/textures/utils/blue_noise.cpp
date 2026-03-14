// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file blue_noise.cpp
 * @brief Void-and-Cluster blue noise threshold map — implementation
 *
 * The Void-and-Cluster algorithm builds a threshold map (dither array) with
 * excellent blue noise spectral properties.  The procedure:
 *
 *   1. Start with a small fraction of initial "minority" pixels placed at
 *      random, then iteratively move each to the tightest cluster (by removing
 *      from the densest spot and re-inserting at the largest void).
 *   2. Phase 1 — remove minority pixels one-by-one from the tightest cluster,
 *      assigning descending ranks.
 *   3. Phase 2 — insert new minority pixels one-by-one at the largest void,
 *      assigning ascending ranks, until half the array is filled.
 *   4. Phase 3 — switch to "majority" perspective and insert at the largest
 *      void among the *unranked* pixels, assigning ascending ranks until done.
 *
 * The energy function uses a Gaussian low-pass filter with toroidal wrapping.
 */

#include "blue_noise.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace whiteout::textures {

namespace {

constexpr u32 N = BLUE_NOISE_SIZE;
constexpr u32 TOTAL = BLUE_NOISE_TOTAL;

// Gaussian sigma as a fraction of the array side — controls the blue noise
// quality.  ~1.5 is the standard recommendation.
constexpr f64 SIGMA = 1.5;

// Pre-computed Gaussian kernel over the toroidal distance field.
// Only needs to cover half the array in each dimension (due to wrapping).
struct GaussianKernel {
    std::array<f64, TOTAL> weights{};

    GaussianKernel() {
        const f64 sigma2 = SIGMA * SIGMA;
        const i32 half = static_cast<i32>(N / 2);
        for (i32 dy = 0; dy < static_cast<i32>(N); ++dy) {
            const i32 wy = (dy <= half) ? dy : static_cast<i32>(N) - dy;
            for (i32 dx = 0; dx < static_cast<i32>(N); ++dx) {
                const i32 wx = (dx <= half) ? dx : static_cast<i32>(N) - dx;
                const f64 d2 = static_cast<f64>(wx * wx + wy * wy);
                weights[static_cast<u32>(dy) * N + static_cast<u32>(dx)] =
                    std::exp(-d2 / (2.0 * sigma2));
            }
        }
        // Zero out the (0,0) entry so a pixel doesn't contribute to its own energy.
        weights[0] = 0.0;
    }
};

/// Energy map: sum of Gaussian contributions from all "set" pixels.
/// Maintained incrementally as pixels are added/removed.
struct EnergyMap {
    std::array<f64, TOTAL> energy{};
    const GaussianKernel& kernel;

    explicit EnergyMap(const GaussianKernel& k) : kernel(k) { energy.fill(0.0); }

    void addPixel(u32 px, u32 py) {
        for (u32 dy = 0; dy < N; ++dy) {
            const u32 ry = (py + dy) % N;
            for (u32 dx = 0; dx < N; ++dx) {
                const u32 rx = (px + dx) % N;
                energy[ry * N + rx] += kernel.weights[dy * N + dx];
            }
        }
    }

    void removePixel(u32 px, u32 py) {
        for (u32 dy = 0; dy < N; ++dy) {
            const u32 ry = (py + dy) % N;
            for (u32 dx = 0; dx < N; ++dx) {
                const u32 rx = (px + dx) % N;
                energy[ry * N + rx] -= kernel.weights[dy * N + dx];
            }
        }
    }

    /// Find the set pixel with the highest energy (tightest cluster).
    u32 findTightestCluster(const std::vector<bool>& is_set) const {
        f64 best = -1.0;
        u32 best_idx = 0;
        for (u32 i = 0; i < TOTAL; ++i) {
            if (is_set[i] && energy[i] > best) {
                best = energy[i];
                best_idx = i;
            }
        }
        return best_idx;
    }

    /// Find the unset pixel with the lowest energy (largest void).
    u32 findLargestVoid(const std::vector<bool>& is_set) const {
        f64 best = std::numeric_limits<f64>::max();
        u32 best_idx = 0;
        for (u32 i = 0; i < TOTAL; ++i) {
            if (!is_set[i] && energy[i] < best) {
                best = energy[i];
                best_idx = i;
            }
        }
        return best_idx;
    }
};

/// Simple deterministic seed generator (good enough for initial placement).
/// Uses a basic LCG to avoid external dependencies.
struct Rng {
    u32 state = 0x12345678u;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
};

std::array<u16, TOTAL> generate() {
    GaussianKernel kernel;
    EnergyMap emap(kernel);
    std::vector<bool> is_set(TOTAL, false);
    std::array<u16, TOTAL> rank{};

    // Initial seed: ~10% of pixels placed at random.
    const u32 initial_count = TOTAL / 10;
    Rng rng;
    {
        // Shuffle indices and pick the first initial_count.
        std::array<u32, TOTAL> indices;
        std::iota(indices.begin(), indices.end(), 0u);
        for (u32 i = TOTAL - 1; i > 0; --i) {
            const u32 j = rng.next() % (i + 1);
            std::swap(indices[i], indices[j]);
        }
        for (u32 i = 0; i < initial_count; ++i) {
            const u32 idx = indices[i];
            is_set[idx] = true;
            emap.addPixel(idx % N, idx / N);
        }
    }

    // Phase 0 — tighten the initial pattern by iteratively moving the
    // tightest cluster pixel to the largest void.
    for (u32 iter = 0; iter < initial_count; ++iter) {
        const u32 cluster = emap.findTightestCluster(is_set);
        is_set[cluster] = false;
        emap.removePixel(cluster % N, cluster / N);

        const u32 voidIdx = emap.findLargestVoid(is_set);
        is_set[voidIdx] = true;
        emap.addPixel(voidIdx % N, voidIdx / N);
    }

    // Phase 1 — remove minority pixels from tightest clusters, assigning
    // descending ranks from initial_count-1 down to 0.
    {
        // Work on a copy so we can restore the set afterwards.
        auto set_copy = is_set;
        EnergyMap emap_copy(kernel);
        // Rebuild energy for the copy.
        for (u32 i = 0; i < TOTAL; ++i) {
            if (set_copy[i])
                emap_copy.addPixel(i % N, i / N);
        }

        for (u32 r = initial_count; r > 0;) {
            --r;
            const u32 cluster = emap_copy.findTightestCluster(set_copy);
            set_copy[cluster] = false;
            emap_copy.removePixel(cluster % N, cluster / N);
            rank[cluster] = static_cast<u16>(r);
        }
    }

    // Phase 2 — insert new pixels at the largest void, ascending rank from
    // initial_count upward until half the array is filled.
    const u32 half = TOTAL / 2;
    u32 current_rank = initial_count;
    while (current_rank < half) {
        const u32 voidIdx = emap.findLargestVoid(is_set);
        is_set[voidIdx] = true;
        emap.addPixel(voidIdx % N, voidIdx / N);
        rank[voidIdx] = static_cast<u16>(current_rank);
        ++current_rank;
    }

    // Phase 3 — switch perspective: among the *unranked* (still-unset) pixels,
    // find the one in the largest void and assign ascending ranks.
    // We rebuild the energy map to reflect only the unset pixels.
    EnergyMap emap3(kernel);
    std::vector<bool> unset(TOTAL, false);
    for (u32 i = 0; i < TOTAL; ++i) {
        if (!is_set[i]) {
            unset[i] = true;
            emap3.addPixel(i % N, i / N);
        }
    }

    while (current_rank < TOTAL) {
        const u32 cluster = emap3.findTightestCluster(unset);
        unset[cluster] = false;
        emap3.removePixel(cluster % N, cluster / N);
        rank[cluster] = static_cast<u16>(current_rank);
        ++current_rank;
    }

    return rank;
}

} // anonymous namespace

std::span<const u16, BLUE_NOISE_TOTAL> blueNoiseMap() {
    static const auto map = generate();
    return std::span<const u16, BLUE_NOISE_TOTAL>(map);
}

} // namespace whiteout::textures

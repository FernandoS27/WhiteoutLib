// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// This is needed as it's too expensive to generate using constexpr.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <vector>

using u16 = std::uint16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using f64 = double;

static constexpr u32 N = 64;
static constexpr u32 TOTAL = N * N; // 4096

// Gaussian sigma — must match the value in blue_noise.cpp
static constexpr f64 SIGMA = 1.5;

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
        weights[0] = 0.0;
    }
};

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

struct Rng {
    u32 state = 0x12345678u;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
};

static std::array<u16, TOTAL> generate() {
    GaussianKernel kernel;
    EnergyMap emap(kernel);
    std::vector<bool> is_set(TOTAL, false);
    std::array<u16, TOTAL> rank{};

    const u32 initial_count = TOTAL / 10;
    Rng rng;
    {
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

    // Phase 0 — tighten initial pattern
    std::cerr << "Phase 0: tightening initial pattern..." << std::endl;
    for (u32 iter = 0; iter < initial_count; ++iter) {
        const u32 cluster = emap.findTightestCluster(is_set);
        is_set[cluster] = false;
        emap.removePixel(cluster % N, cluster / N);

        const u32 voidIdx = emap.findLargestVoid(is_set);
        is_set[voidIdx] = true;
        emap.addPixel(voidIdx % N, voidIdx / N);
    }

    // Phase 1 — remove minority pixels, assign descending ranks
    std::cerr << "Phase 1: removing minority pixels..." << std::endl;
    {
        auto set_copy = is_set;
        EnergyMap emap_copy(kernel);
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

    // Phase 2 — fill to half with ascending ranks
    std::cerr << "Phase 2: filling to half..." << std::endl;
    const u32 half = TOTAL / 2;
    u32 current_rank = initial_count;
    while (current_rank < half) {
        const u32 voidIdx = emap.findLargestVoid(is_set);
        is_set[voidIdx] = true;
        emap.addPixel(voidIdx % N, voidIdx / N);
        rank[voidIdx] = static_cast<u16>(current_rank);
        ++current_rank;
    }

    // Phase 3 — fill remaining with inverted perspective
    std::cerr << "Phase 3: filling remaining..." << std::endl;
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

int main() {
    std::cerr << "Generating 64x64 blue noise threshold map..." << std::endl;

    const auto map = generate();

    std::cerr << "Done. Printing std::array initializer." << std::endl;

    // Print as a C++ std::array initializer
    std::cout << "// AUTO-GENERATED by examples/blue_noise_generator.cpp\n";
    std::cout << "// Void-and-Cluster 64x64 blue noise threshold map (sigma = " << SIGMA << ")\n";
    std::cout << "static constexpr std::array<u16, " << TOTAL << "> kBlueNoiseMap = {{\n";

    constexpr u32 COLS = 16; // values per line
    for (u32 i = 0; i < TOTAL; ++i) {
        if (i % COLS == 0)
            std::cout << "    ";
        std::cout << std::setw(4) << map[i];
        if (i + 1 < TOTAL)
            std::cout << ",";
        if ((i + 1) % COLS == 0)
            std::cout << "\n";
    }

    std::cout << "}};\n";

    return 0;
}

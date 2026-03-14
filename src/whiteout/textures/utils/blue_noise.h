// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file blue_noise.h
 * @brief Void-and-Cluster blue noise threshold map generation
 *
 * Implements the Void-and-Cluster algorithm described in:
 *   Robert Ulichney, "The Void-and-Cluster Method for Dither Array Generation",
 *   Proc. SPIE 1913, Human Vision, Visual Processing, and Digital Display IV, 1993.
 *
 * Produces a 64×64 blue noise threshold map suitable for ordered dithering.
 * The map is generated once (lazily) and cached for the lifetime of the process.
 */

#include <whiteout/common_types.h>

#include <array>
#include <span>

namespace whiteout::textures {

/// Side length of the blue noise threshold map.
static constexpr u32 BLUE_NOISE_SIZE = 64;

/// Total number of entries in the threshold map (64 × 64 = 4096).
static constexpr u32 BLUE_NOISE_TOTAL = BLUE_NOISE_SIZE * BLUE_NOISE_SIZE;

/// Returns a reference to a lazily-initialized 64×64 blue noise threshold map.
/// Each entry is in [0, 4095] and represents a rank in the dispersal order.
/// Thread-safe (uses function-local static).
std::span<const u16, BLUE_NOISE_TOTAL> blueNoiseMap();

} // namespace whiteout::textures

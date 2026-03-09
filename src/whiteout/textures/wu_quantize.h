// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file wu_quantize.h
 * @brief Wu's optimal color quantization for 256-color palette generation
 *
 * Implements the algorithm described in:
 *   Xiaolin Wu, "Efficient Statistical Computations for Optimal Color Quantization"
 *   Graphics Gems II, Academic Press, 1991, pp. 126–133.
 *
 * Given an RGBA8 image, this produces an optimal 256-color palette and maps
 * every pixel to its palette index.
 */

#include <whiteout/common_types.h>

#include <array>
#include <vector>

namespace whiteout::textures::wu {

/// Maximum number of palette entries produced.
static constexpr u32 MAX_COLORS = 256;

/// Result of quantization: the palette and a lookup table from quantized
/// color coordinates to palette indices.
struct QuantizeResult {
    /// BGRX palette entries (32-bit; [23:16]=R, [15:8]=G, [7:0]=B, [31:24]=0).
    /// Matches the BLP palette entry format used by the decoder.
    std::array<u32, MAX_COLORS> palette{};

    /// Actual number of palette entries produced (≤ 256).
    u32 color_count = 0;

    /// Map a single pixel to its palette index.
    /// @param r  Red   channel [0, 255]
    /// @param g  Green channel [0, 255]
    /// @param b  Blue  channel [0, 255]
    /// @return Palette index in [0, color_count).
    u8 mapPixel(u8 r, u8 g, u8 b) const;

    /// Map a full buffer of RGBA8 pixels to palette indices (alpha is ignored).
    /// @param rgba         Pointer to pixel_count × 4 bytes of RGBA8 data.
    /// @param pixel_count  Number of pixels.
    /// @param out_indices  Output buffer of at least pixel_count bytes.
    void mapPixels(const u8* rgba, u32 pixel_count, u8* out_indices) const;

private:
    friend QuantizeResult quantize(const u8* rgba, u32 pixel_count, u32 max_colors);

    /// 33³ tag volume: quantized-color → palette index.
    /// Index 0 on each axis is unused (sentinels for prefix sums).
    static constexpr u32 HIST_SIZE = 33;
    std::vector<u8> tag_; // HIST_SIZE³ entries
};

/// Run Wu's variance-minimization quantization on an RGBA8 pixel buffer.
///
/// @param rgba         Pointer to pixel_count × 4 bytes of RGBA8 data.
/// @param pixel_count  Number of pixels (width × height, or sum over mips).
/// @param max_colors   Maximum palette entries to produce (default 256, must be ≤ 256).
/// @return A QuantizeResult containing the palette and fast pixel-mapping tables.
QuantizeResult quantize(const u8* rgba, u32 pixel_count, u32 max_colors = MAX_COLORS);

} // namespace whiteout::textures::wu

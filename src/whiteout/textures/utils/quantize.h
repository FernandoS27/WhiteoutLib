// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file quantize.h
 * @brief Wu's optimal color quantization for 256-color palette generation
 *
 * Implements the algorithm described in:
 *   Xiaolin Wu, "Efficient Statistical Computations for Optimal Color Quantization"
 *   Graphics Gems II, Academic Press, 1991, pp. 126–133.
 *
 * Given an RGBA8 image, this produces an optimal 256-color palette and maps
 * every pixel to its palette index.
 *
 * Usage (builder pattern):
 * @code
 *   auto result = wu::Quantizer()
 *                     .maxColors(256)
 *                     .kmeansIterations(10)
 *                     .quantize(rgba, pixel_count);
 * @endcode
 */

#include <whiteout/common_types.h>

#include <array>
#include <memory>
#include <span>
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

    /// Map RGBA8 pixels to palette indices with blue-noise ordered dithering.
    /// Uses a Void-and-Cluster threshold map to perturb pixel colors before
    /// nearest-palette lookup, producing visually pleasing error distribution.
    /// @param rgba         Pointer to pixel_count × 4 bytes of RGBA8 data.
    /// @param width        Image width in pixels.
    /// @param height       Image height in pixels.
    /// @param strength     Dither strength in [0, 1]. 0 = no dithering, 1 = full.
    /// @param out_indices  Output buffer of at least width*height bytes.
    void mapPixelsDithered(const u8* rgba, u32 width, u32 height, f32 strength,
                           u8* out_indices) const;

    /// Refine the palette using gradient descent to minimize reconstruction
    /// error under blue-noise ordered dithering.  Each iteration simulates the
    /// dithering process (same noise pattern as mapPixelsDithered), assigns
    /// each pixel to its post-dither nearest palette entry, then shifts every
    /// palette color toward the mean of the *original* (undithered) pixels
    /// assigned to it, weighted by a geometrically decaying learning rate.
    /// After all iterations the tag volume is rebuilt.
    ///
    /// @param rgba         Original RGBA8 pixel data (undithered).
    /// @param width        Image width in pixels.
    /// @param height       Image height in pixels.
    /// @param strength     Dither strength in [0, 1], matching mapPixelsDithered().
    /// @param iterations   Number of gradient descent iterations (default 5).
    void refineDitherAware(const u8* rgba, u32 width, u32 height,
                           f32 strength, u32 iterations = 5);

private:
    friend class Quantizer;

    /// Whether the pixel data the result was built from is sRGB-encoded.
    bool srgb_ = false;

    /// 33³ tag volume: quantized-color → palette index.
    /// Index 0 on each axis is unused (sentinels for prefix sums).
    static constexpr u32 HIST_SIZE = 33;
    std::vector<u8> tag_; // HIST_SIZE³ entries
};

/// Builder-pattern quantizer for Wu's variance-minimization algorithm.
///
/// Configure parameters via chained setters, then call quantize() to produce
/// the palette.  Implementation details are hidden behind PImpl.
class Quantizer {
public:
    Quantizer();
    ~Quantizer();
    Quantizer(Quantizer&&) noexcept;
    Quantizer& operator=(Quantizer&&) noexcept;

    /// Set the maximum number of palette entries (default 256, clamped to [1, 256]).
    Quantizer& maxColors(u32 count);

    /// Set the number of k-means refinement iterations (default 10).
    /// Pass 0 to disable k-means refinement entirely.
    Quantizer& kmeansIterations(u32 iterations);

    /// Indicate whether the input pixel data is sRGB-encoded (default: false).
    /// When false, the quantizer assumes the RGBA8 values represent linear-light
    /// intensities (the BLP writer linearizes sRGB textures before quantization).
    /// When true, the sRGB gamma curve is decoded before CIELAB conversion.
    Quantizer& srgbInput(bool srgb);

    /// Enable dithering-aware palette refinement after initial quantization.
    /// When enabled, quantize() will run additional gradient descent iterations
    /// that simulate the blue-noise dithering process and shift palette colors
    /// to minimize the post-dither reconstruction error (CIELAB ΔE²).
    ///
    /// @param width      Image width (for blue noise tiling).
    /// @param height     Image height.
    /// @param strength   Dither strength in [0, 1].
    /// @param iterations Number of refinement iterations (default 5).
    Quantizer& ditherAware(u32 width, u32 height, f32 strength, u32 iterations = 5);

    /// Run the quantization on an RGBA8 pixel buffer.
    ///
    /// @param rgba         Pointer to pixel_count × 4 bytes of RGBA8 data.
    /// @param pixel_count  Number of pixels (width × height, or sum over mips).
    /// @return A QuantizeResult containing the palette and fast pixel-mapping tables.
    QuantizeResult quantize(const u8* rgba, u32 pixel_count) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::textures::wu

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file color_convert.h
/// @brief Colour-space and channel-order conversion helpers.
///
/// Internal header — not part of the public include path.
///
/// Contains BGRA↔RGBA channel swaps and Y'CbCr↔RGB colour-space
/// conversions used by BMP, TGA, BLP, JPEG, and TEX parsers/writers.

#pragma once

#include <whiteout/common_types.h>

namespace whiteout::textures {

/// Multiplier to expand a 4-bit nibble (0–15) to 8-bit range (0–255).
static constexpr u8 EXPAND_4BIT_TO_8BIT = 17;

// ============================================================================
// Channel-swap: BGRA ↔ RGBA (self-inverse).
// ============================================================================

/// Swap red and blue channels between @p src and @p dst.
/// The operation is its own inverse: BGRA→RGBA and RGBA→BGRA.
void swap_red_blue(const u8* src, u8* dst, u64 pixel_count);

/// Convert BGR(A) pixels to RGBA8, supporting both 3-byte (BGR) and 4-byte (BGRA) source.
/// When @p src_bytes_per_pixel is 3, alpha defaults to 0xFF.
void convert_bgr_to_rgba(const u8* src, u8* dst, u64 pixel_count, u32 src_bytes_per_pixel);

// ============================================================================
// Y'CbCr ↔ RGB colour-space conversion (ITU-R BT.601 / JFIF).
// ============================================================================

/// Convert one Y'CbCr pixel to RGB.
void ycbcr_to_rgb(u8 y, u8 cb, u8 cr, u8& r, u8& g, u8& b);

/// Convert one RGB pixel to Y'CbCr.
void rgb_to_ycbcr(u8 r, u8 g, u8 b, u8& y, u8& cb, u8& cr);

} // namespace whiteout::textures

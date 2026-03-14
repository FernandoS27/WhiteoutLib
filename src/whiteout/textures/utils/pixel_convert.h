// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pixel_convert.h
/// @brief Shared pixel-format conversion helpers used across texture modules.
///
/// Internal header — not part of the public include path.
///
/// Every uncompressed PixelFormat has a pair of per-pixel converters that go
/// through RGBA32F as the canonical intermediate representation:
///   to_rgba32f(src, dst)   – reads one pixel, writes 4 floats
///   from_rgba32f(src, dst) – reads 4 floats, writes one pixel

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures {

// ============================================================================
// Common pixel constants.
// ============================================================================

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

// ============================================================================
// Per-format → RGBA32F converters (one pixel at a time).
// ============================================================================

void r8_to_rgba32f(const u8* src, f32* dst);
void r16_to_rgba32f(const u8* src, f32* dst);
void r32f_to_rgba32f(const u8* src, f32* dst);
void rg8_to_rgba32f(const u8* src, f32* dst);
void rg16_to_rgba32f(const u8* src, f32* dst);
void rg32f_to_rgba32f(const u8* src, f32* dst);
/// RGBA8 → RGBA32F (4 bytes → 4 floats).
void rgba8_to_rgba32f(const u8* src, f32* dst);
/// RGBA16 (UNORM) → RGBA32F (4 × u16 → 4 floats).
void rgba16_to_rgba32f(const u8* src, f32* dst);
void rgba32f_to_rgba32f(const u8* src, f32* dst);

// ============================================================================
// RGBA32F → per-format converters (one pixel at a time).
// ============================================================================

void rgba32f_to_r8(const f32* src, u8* dst);
void rgba32f_to_r16(const f32* src, u8* dst);
void rgba32f_to_r32f(const f32* src, u8* dst);
void rgba32f_to_rg8(const f32* src, u8* dst);
void rgba32f_to_rg16(const f32* src, u8* dst);
void rgba32f_to_rg32f(const f32* src, u8* dst);
/// RGBA32F → RGBA8 (4 floats → 4 bytes, clamped).
void rgba32f_to_rgba8(const f32* src, u8* dst);
/// RGBA32F → RGBA16 UNORM (4 floats → 4 × u16, clamped).
void rgba32f_to_rgba16(const f32* src, u8* dst);
void rgba32f_to_rgba32f_out(const f32* src, u8* dst);

// ============================================================================
// Dispatcher: get per-pixel converter by PixelFormat
// ============================================================================

using ToRGBA32F = void (*)(const u8* src, f32* dst);
using FromRGBA32F = void (*)(const f32* src, u8* dst);

ToRGBA32F get_to_rgba32f(PixelFormat fmt);
FromRGBA32F get_from_rgba32f(PixelFormat fmt);

} // namespace whiteout::textures

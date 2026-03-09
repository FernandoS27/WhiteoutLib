// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pixel_convert.h
/// @brief Shared pixel-format conversion helpers used across texture modules.
///
/// Internal header — not part of the public include path.

#pragma once

#include <algorithm>
#include <array>

#include <whiteout/common_types.h>

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
inline void swap_red_blue(const u8* src, u8* dst, u64 pixel_count) {
    for (u64 i = 0; i < pixel_count; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
}

// ============================================================================
// Single-pixel format conversions (uncompressed RGBA variants).
// ============================================================================

/// RGBA8 → RGBA32F (4 bytes → 4 floats).
inline void rgba8_to_rgba32f(const u8* src, f32* dst) {
    dst[0] = src[0] / 255.0f;
    dst[1] = src[1] / 255.0f;
    dst[2] = src[2] / 255.0f;
    dst[3] = src[3] / 255.0f;
}

/// RGBA32F → RGBA8 (4 floats → 4 bytes, clamped).
inline void rgba32f_to_rgba8(const f32* src, u8* dst) {
    auto clamp01 = [](f32 v) -> u8 {
        return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    dst[0] = clamp01(src[0]);
    dst[1] = clamp01(src[1]);
    dst[2] = clamp01(src[2]);
    dst[3] = clamp01(src[3]);
}

/// RGBA16F → RGBA32F (4 × u16 → 4 floats).
inline void rgba16f_to_rgba32f(const u16* src, f32* dst) {
    dst[0] = f16::from_raw(src[0]).to_float();
    dst[1] = f16::from_raw(src[1]).to_float();
    dst[2] = f16::from_raw(src[2]).to_float();
    dst[3] = f16::from_raw(src[3]).to_float();
}

/// RGBA32F → RGBA16F (4 floats → 4 × u16).
inline void rgba32f_to_rgba16f(const f32* src, u16* dst) {
    dst[0] = f16::from_float(src[0]).raw;
    dst[1] = f16::from_float(src[1]).raw;
    dst[2] = f16::from_float(src[2]).raw;
    dst[3] = f16::from_float(src[3]).raw;
}

} // namespace whiteout::textures

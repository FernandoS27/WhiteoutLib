// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pixel_convert.cpp
/// @brief Implementation of shared pixel-format conversion helpers.

#include "pixel_convert.h"

#include <algorithm>
#include <cstring>

namespace whiteout::textures {

// ============================================================================
// Channel-swap helpers
// ============================================================================

void swap_red_blue(const u8* src, u8* dst, u64 pixel_count) {
    for (u64 i = 0; i < pixel_count; ++i) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
}

void convert_bgr_to_rgba(const u8* src, u8* dst, u64 pixel_count, u32 src_bytes_per_pixel) {
    for (u64 i = 0; i < pixel_count; ++i) {
        const u8* sp = src + i * src_bytes_per_pixel;
        u8* dp = dst + i * 4;
        dp[0] = sp[2];                                        // R
        dp[1] = sp[1];                                        // G
        dp[2] = sp[0];                                        // B
        dp[3] = (src_bytes_per_pixel >= 4) ? sp[3] : 0xFF;   // A
    }
}

// ============================================================================
// Y'CbCr ↔ RGB
// ============================================================================

void ycbcr_to_rgb(u8 y, u8 cb, u8 cr, u8& r, u8& g, u8& b) {
    f32 fy  = static_cast<f32>(y);
    f32 fcb = static_cast<f32>(cb) - 128.0f;
    f32 fcr = static_cast<f32>(cr) - 128.0f;

    r = static_cast<u8>(std::clamp(static_cast<i32>(fy + 1.402f * fcr + 0.5f), 0, 255));
    g = static_cast<u8>(
        std::clamp(static_cast<i32>(fy - 0.344136f * fcb - 0.714136f * fcr + 0.5f), 0, 255));
    b = static_cast<u8>(std::clamp(static_cast<i32>(fy + 1.772f * fcb + 0.5f), 0, 255));
}

void rgb_to_ycbcr(u8 r, u8 g, u8 b, u8& y, u8& cb, u8& cr) {
    f32 fr = static_cast<f32>(r);
    f32 fg = static_cast<f32>(g);
    f32 fb = static_cast<f32>(b);

    y = static_cast<u8>(
        std::clamp(static_cast<i32>(0.299f * fr + 0.587f * fg + 0.114f * fb + 0.5f), 0, 255));
    cb = static_cast<u8>(std::clamp(
        static_cast<i32>(-0.168736f * fr - 0.331264f * fg + 0.5f * fb + 128.5f), 0, 255));
    cr = static_cast<u8>(std::clamp(
        static_cast<i32>(0.5f * fr - 0.418688f * fg - 0.081312f * fb + 128.5f), 0, 255));
}

// ============================================================================
// Per-format → RGBA32F converters
// ============================================================================

void r8_to_rgba32f(const u8* src, f32* dst) {
    dst[0] = src[0] / 255.0f;
    dst[1] = 0.0f;
    dst[2] = 0.0f;
    dst[3] = 1.0f;
}

void r16_to_rgba32f(const u8* src, f32* dst) {
    u16 v;
    std::memcpy(&v, src, sizeof(v));
    dst[0] = v / 65535.0f;
    dst[1] = 0.0f;
    dst[2] = 0.0f;
    dst[3] = 1.0f;
}

void r32f_to_rgba32f(const u8* src, f32* dst) {
    std::memcpy(&dst[0], src, sizeof(f32));
    dst[1] = 0.0f;
    dst[2] = 0.0f;
    dst[3] = 1.0f;
}

void rg8_to_rgba32f(const u8* src, f32* dst) {
    dst[0] = src[0] / 255.0f;
    dst[1] = src[1] / 255.0f;
    dst[2] = 0.0f;
    dst[3] = 1.0f;
}

void rg16_to_rgba32f(const u8* src, f32* dst) {
    u16 r, g;
    std::memcpy(&r, src + 0, sizeof(u16));
    std::memcpy(&g, src + 2, sizeof(u16));
    dst[0] = r / 65535.0f;
    dst[1] = g / 65535.0f;
    dst[2] = 0.0f;
    dst[3] = 1.0f;
}

void rg32f_to_rgba32f(const u8* src, f32* dst) {
    std::memcpy(&dst[0], src + 0, sizeof(f32));
    std::memcpy(&dst[1], src + sizeof(f32), sizeof(f32));
    dst[2] = 0.0f;
    dst[3] = 1.0f;
}

void rgba8_to_rgba32f(const u8* src, f32* dst) {
    dst[0] = src[0] / 255.0f;
    dst[1] = src[1] / 255.0f;
    dst[2] = src[2] / 255.0f;
    dst[3] = src[3] / 255.0f;
}

void rgba16_to_rgba32f(const u8* src, f32* dst) {
    const u16* s = reinterpret_cast<const u16*>(src);
    dst[0] = s[0] / 65535.0f;
    dst[1] = s[1] / 65535.0f;
    dst[2] = s[2] / 65535.0f;
    dst[3] = s[3] / 65535.0f;
}

void rgba32f_to_rgba32f(const u8* src, f32* dst) {
    std::memcpy(dst, src, 4 * sizeof(f32));
}

// ============================================================================
// RGBA32F → per-format converters
// ============================================================================

void rgba32f_to_r8(const f32* src, u8* dst) {
    dst[0] = static_cast<u8>(std::clamp(src[0], 0.0f, 1.0f) * 255.0f + 0.5f);
}

void rgba32f_to_r16(const f32* src, u8* dst) {
    u16 v = static_cast<u16>(std::clamp(src[0], 0.0f, 1.0f) * 65535.0f + 0.5f);
    std::memcpy(dst, &v, sizeof(v));
}

void rgba32f_to_r32f(const f32* src, u8* dst) {
    std::memcpy(dst, &src[0], sizeof(f32));
}

void rgba32f_to_rg8(const f32* src, u8* dst) {
    dst[0] = static_cast<u8>(std::clamp(src[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    dst[1] = static_cast<u8>(std::clamp(src[1], 0.0f, 1.0f) * 255.0f + 0.5f);
}

void rgba32f_to_rg16(const f32* src, u8* dst) {
    u16 r = static_cast<u16>(std::clamp(src[0], 0.0f, 1.0f) * 65535.0f + 0.5f);
    u16 g = static_cast<u16>(std::clamp(src[1], 0.0f, 1.0f) * 65535.0f + 0.5f);
    std::memcpy(dst + 0, &r, sizeof(u16));
    std::memcpy(dst + 2, &g, sizeof(u16));
}

void rgba32f_to_rg32f(const f32* src, u8* dst) {
    std::memcpy(dst + 0, &src[0], sizeof(f32));
    std::memcpy(dst + sizeof(f32), &src[1], sizeof(f32));
}

void rgba32f_to_rgba8(const f32* src, u8* dst) {
    dst[0] = static_cast<u8>(std::clamp(src[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    dst[1] = static_cast<u8>(std::clamp(src[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    dst[2] = static_cast<u8>(std::clamp(src[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    dst[3] = static_cast<u8>(std::clamp(src[3], 0.0f, 1.0f) * 255.0f + 0.5f);
}

void rgba32f_to_rgba16(const f32* src, u8* dst) {
    u16* d = reinterpret_cast<u16*>(dst);
    d[0] = static_cast<u16>(std::clamp(src[0], 0.0f, 1.0f) * 65535.0f + 0.5f);
    d[1] = static_cast<u16>(std::clamp(src[1], 0.0f, 1.0f) * 65535.0f + 0.5f);
    d[2] = static_cast<u16>(std::clamp(src[2], 0.0f, 1.0f) * 65535.0f + 0.5f);
    d[3] = static_cast<u16>(std::clamp(src[3], 0.0f, 1.0f) * 65535.0f + 0.5f);
}

void rgba32f_to_rgba32f_out(const f32* src, u8* dst) {
    std::memcpy(dst, src, 4 * sizeof(f32));
}

// ============================================================================
// Dispatchers
// ============================================================================

ToRGBA32F get_to_rgba32f(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::R8:     return r8_to_rgba32f;
    case PixelFormat::R16:    return r16_to_rgba32f;
    case PixelFormat::R32F:   return r32f_to_rgba32f;
    case PixelFormat::RG8:    return rg8_to_rgba32f;
    case PixelFormat::RG16:   return rg16_to_rgba32f;
    case PixelFormat::RG32F:  return rg32f_to_rgba32f;
    case PixelFormat::RGBA8:  return rgba8_to_rgba32f;
    case PixelFormat::RGBA16: return rgba16_to_rgba32f;
    case PixelFormat::RGBA32F: return rgba32f_to_rgba32f;
    default:                  return nullptr;
    }
}

FromRGBA32F get_from_rgba32f(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::R8:     return rgba32f_to_r8;
    case PixelFormat::R16:    return rgba32f_to_r16;
    case PixelFormat::R32F:   return rgba32f_to_r32f;
    case PixelFormat::RG8:    return rgba32f_to_rg8;
    case PixelFormat::RG16:   return rgba32f_to_rg16;
    case PixelFormat::RG32F:  return rgba32f_to_rg32f;
    case PixelFormat::RGBA8:  return rgba32f_to_rgba8;
    case PixelFormat::RGBA16: return rgba32f_to_rgba16;
    case PixelFormat::RGBA32F: return rgba32f_to_rgba32f_out;
    default:                  return nullptr;
    }
}

} // namespace whiteout::textures

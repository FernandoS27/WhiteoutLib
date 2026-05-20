// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file color_convert.cpp
/// @brief Implementation of colour-space and channel-order conversion helpers.

#include "color_convert.h"

#include <algorithm>
#include <cmath>

namespace whiteout::textures {

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
        dp[0] = sp[2];                                     // R
        dp[1] = sp[1];                                     // G
        dp[2] = sp[0];                                     // B
        dp[3] = (src_bytes_per_pixel >= 4) ? sp[3] : 0xFF; // A
    }
}

/// Round @p v to nearest integer (banker-style via std::lround) and clamp into
/// the unsigned 8-bit range. Avoids the off-by-one near negatives that the
/// `(int)(x + 0.5f)` idiom produces.
static u8 roundClampU8(f32 v) {
    return static_cast<u8>(std::clamp<i32>(static_cast<i32>(std::lround(v)), 0, 255));
}

void ycbcr_to_rgb(u8 y, u8 cb, u8 cr, u8& r, u8& g, u8& b) {
    f32 const fy = static_cast<f32>(y);
    f32 const fcb = static_cast<f32>(cb) - 128.0f;
    f32 const fcr = static_cast<f32>(cr) - 128.0f;

    r = roundClampU8(fy + 1.402f * fcr);
    g = roundClampU8(fy - 0.344136f * fcb - 0.714136f * fcr);
    b = roundClampU8(fy + 1.772f * fcb);
}

void rgb_to_ycbcr(u8 r, u8 g, u8 b, u8& y, u8& cb, u8& cr) {
    f32 const fr = static_cast<f32>(r);
    f32 const fg = static_cast<f32>(g);
    f32 const fb = static_cast<f32>(b);

    y  = roundClampU8(0.299f * fr + 0.587f * fg + 0.114f * fb);
    cb = roundClampU8(-0.168736f * fr - 0.331264f * fg + 0.5f * fb + 128.0f);
    cr = roundClampU8(0.5f * fr - 0.418688f * fg - 0.081312f * fb + 128.0f);
}

} // namespace whiteout::textures

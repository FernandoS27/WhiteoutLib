// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc1.cpp
/// @brief BC1 (DXT1) encode and decode implementation.

#include "bc1.h"

#include <array>
#include <cmath>
#include <limits>

namespace whiteout::textures {
namespace bc1 {

/// Alpha value below which a pixel is considered transparent in punch-through mode.
static constexpr u8 ALPHA_TRANSPARENT_THRESHOLD = 128;

/// Bounding-box is inset by 1/16th of the range to improve endpoint selection.
static constexpr i32 BBOX_INSET_DIVISOR = 16;

// ============================================================================
// Decode
// ============================================================================

void decode_block(const u8* block, u8* out) {
    const u16 c0_raw = static_cast<u16>(block[0]) | (static_cast<u16>(block[1]) << 8);
    const u16 c1_raw = static_cast<u16>(block[2]) | (static_cast<u16>(block[3]) << 8);

    u32 r0 = expand5_to_8((c0_raw >> 11) & 0x1F);
    u32 g0 = expand6_to_8((c0_raw >> 5) & 0x3F);
    u32 b0 = expand5_to_8(c0_raw & 0x1F);

    u32 r1 = expand5_to_8((c1_raw >> 11) & 0x1F);
    u32 g1 = expand6_to_8((c1_raw >> 5) & 0x3F);
    u32 b1 = expand5_to_8(c1_raw & 0x1F);

    std::array<std::array<u8, 4>, 4> palette{}; // [index][RGBA]

    palette[0][0] = static_cast<u8>(r0);
    palette[0][1] = static_cast<u8>(g0);
    palette[0][2] = static_cast<u8>(b0);
    palette[0][3] = 255;

    palette[1][0] = static_cast<u8>(r1);
    palette[1][1] = static_cast<u8>(g1);
    palette[1][2] = static_cast<u8>(b1);
    palette[1][3] = 255;

    if (c0_raw > c1_raw) {
        // 4-colour opaque mode
        palette[2][0] = static_cast<u8>((2 * r0 + r1 + 1) / 3);
        palette[2][1] = static_cast<u8>((2 * g0 + g1 + 1) / 3);
        palette[2][2] = static_cast<u8>((2 * b0 + b1 + 1) / 3);
        palette[2][3] = 255;

        palette[3][0] = static_cast<u8>((r0 + 2 * r1 + 1) / 3);
        palette[3][1] = static_cast<u8>((g0 + 2 * g1 + 1) / 3);
        palette[3][2] = static_cast<u8>((b0 + 2 * b1 + 1) / 3);
        palette[3][3] = 255;
    } else {
        // 3-colour + transparent mode
        palette[2][0] = static_cast<u8>((r0 + r1) / 2);
        palette[2][1] = static_cast<u8>((g0 + g1) / 2);
        palette[2][2] = static_cast<u8>((b0 + b1) / 2);
        palette[2][3] = 255;

        palette[3][0] = 0;
        palette[3][1] = 0;
        palette[3][2] = 0;
        palette[3][3] = 0; // transparent black
    }

    // Unpack 16 × 2-bit indices from bytes 4..7
    u32 bits = static_cast<u32>(block[4]) | (static_cast<u32>(block[5]) << 8) |
               (static_cast<u32>(block[6]) << 16) | (static_cast<u32>(block[7]) << 24);

    for (u32 i = 0; i < 16; ++i) {
        u32 idx = (bits >> (i * 2)) & 3;
        out[i * 4 + 0] = palette[idx][0];
        out[i * 4 + 1] = palette[idx][1];
        out[i * 4 + 2] = palette[idx][2];
        out[i * 4 + 3] = palette[idx][3];
    }
}

std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error) {
    return transform_texture_impl(
        src, PixelFormat::BC1, PixelFormat::RGBA8, "bc1::decodeTexture",
        [](std::span<const u8> data, u32 w, u32 h) {
            return decode_image_rgba8<8>(data, w, h, decode_block);
        },
        out_error);
}

// ============================================================================
// Encode
// ============================================================================

namespace {

struct RGB {
    i32 r, g, b;
};

/// Pack an 8-bit RGB triple into a 16-bit RGB565 value.
inline u16 pack_565(i32 r, i32 g, i32 b) {
    u32 r5 = static_cast<u32>(std::clamp(r, 0, 255)) >> 3;
    u32 g6 = static_cast<u32>(std::clamp(g, 0, 255)) >> 2;
    u32 b5 = static_cast<u32>(std::clamp(b, 0, 255)) >> 3;
    return static_cast<u16>((r5 << 11) | (g6 << 5) | b5);
}

/// Unpack a RGB565 value to 8-bit RGB.
inline RGB unpack_565(u16 c) {
    return {static_cast<i32>(expand5_to_8((c >> 11) & 0x1F)),
            static_cast<i32>(expand6_to_8((c >> 5) & 0x3F)),
            static_cast<i32>(expand5_to_8(c & 0x1F))};
}

inline u64 colour_dist(RGB a, RGB b) {
    return bcn_sq(a.r - b.r) + bcn_sq(a.g - b.g) + bcn_sq(a.b - b.b);
}

// ============================================================================
// Block encoder (4-colour opaque mode)
// ============================================================================

/// Build a 4-entry palette from two 565 endpoints (c0 > c1 — 4-colour mode).
void build_palette_4c(u16 c0_565, u16 c1_565, std::array<RGB, 4>& palette) {
    palette[0] = unpack_565(c0_565);
    palette[1] = unpack_565(c1_565);
    palette[2] = {(2 * palette[0].r + palette[1].r + 1) / 3,
                  (2 * palette[0].g + palette[1].g + 1) / 3,
                  (2 * palette[0].b + palette[1].b + 1) / 3};
    palette[3] = {(palette[0].r + 2 * palette[1].r + 1) / 3,
                  (palette[0].g + 2 * palette[1].g + 1) / 3,
                  (palette[0].b + 2 * palette[1].b + 1) / 3};
}

/// Build a 3-entry palette + transparent (c0 <= c1 — punch-through mode).
void build_palette_3c(u16 c0_565, u16 c1_565, std::array<RGB, 4>& palette) {
    palette[0] = unpack_565(c0_565);
    palette[1] = unpack_565(c1_565);
    palette[2] = {(palette[0].r + palette[1].r) / 2, (palette[0].g + palette[1].g) / 2,
                  (palette[0].b + palette[1].b) / 2};
    palette[3] = {0, 0, 0}; // transparent black
}

/// Find the bounding box endpoints of 16 RGBA pixels.
void find_bbox(const u8* rgba, RGB& min_color, RGB& max_color) {
    min_color = {255, 255, 255};
    max_color = {0, 0, 0};
    for (u32 i = 0; i < 16; ++i) {
        i32 r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        min_color.r = std::min(min_color.r, r);
        min_color.g = std::min(min_color.g, g);
        min_color.b = std::min(min_color.b, b);
        max_color.r = std::max(max_color.r, r);
        max_color.g = std::max(max_color.g, g);
        max_color.b = std::max(max_color.b, b);
    }
}

/// Inset the bounding box by 1/16th to improve endpoint selection.
void inset_bbox(RGB& min_color, RGB& max_color) {
    i32 dr = max_color.r - min_color.r;
    i32 dg = max_color.g - min_color.g;
    i32 db = max_color.b - min_color.b;
    min_color.r = std::clamp(min_color.r + dr / BBOX_INSET_DIVISOR, 0, 255);
    min_color.g = std::clamp(min_color.g + dg / BBOX_INSET_DIVISOR, 0, 255);
    min_color.b = std::clamp(min_color.b + db / BBOX_INSET_DIVISOR, 0, 255);
    max_color.r = std::clamp(max_color.r - dr / BBOX_INSET_DIVISOR, 0, 255);
    max_color.g = std::clamp(max_color.g - dg / BBOX_INSET_DIVISOR, 0, 255);
    max_color.b = std::clamp(max_color.b - db / BBOX_INSET_DIVISOR, 0, 255);
}

/// Assign 2-bit indices to 16 pixels using 4-colour palette.
u64 assign_indices_4c(const u8* rgba, const std::array<RGB, 4>& palette,
                      std::array<u32, 16>& indices) {
    u64 total_err = 0;
    for (u32 i = 0; i < 16; ++i) {
        RGB pixel = {rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2]};
        u64 best_err = std::numeric_limits<u64>::max();
        u32 best_idx = 0;
        for (u32 j = 0; j < 4; ++j) {
            u64 distance = colour_dist(pixel, palette[j]);
            if (distance < best_err) {
                best_err = distance;
                best_idx = j;
            }
        }
        indices[i] = best_idx;
        total_err += best_err;
    }
    return total_err;
}

/// Pack a BC1 block: two u16 endpoints + 16 × 2-bit indices.
void pack_block(u16 c0, u16 c1, const std::array<u32, 16>& indices, u8* out) {
    out[0] = static_cast<u8>(c0);
    out[1] = static_cast<u8>(c0 >> 8);
    out[2] = static_cast<u8>(c1);
    out[3] = static_cast<u8>(c1 >> 8);

    u32 bits = 0;
    for (u32 i = 0; i < 16; ++i)
        bits |= (indices[i] & 3u) << (i * 2);
    out[4] = static_cast<u8>(bits);
    out[5] = static_cast<u8>(bits >> 8);
    out[6] = static_cast<u8>(bits >> 16);
    out[7] = static_cast<u8>(bits >> 24);
}

/// Assign 2-bit indices to 16 pixels using 3-colour palette + transparent.
/// Pixels with alpha below the threshold are assigned index 3 (transparent).
u64 assign_indices_3c(const u8* rgba, const std::array<RGB, 4>& palette,
                      std::array<u32, 16>& indices) {
    u64 total_err = 0;
    for (u32 i = 0; i < 16; ++i) {
        if (rgba[i * 4 + 3] < ALPHA_TRANSPARENT_THRESHOLD) {
            indices[i] = 3; // transparent
            continue;
        }
        RGB pixel = {rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2]};
        u64 best_err = std::numeric_limits<u64>::max();
        u32 best_idx = 0;
        for (u32 j = 0; j < 3; ++j) { // only 3 opaque colours
            u64 distance = colour_dist(pixel, palette[j]);
            if (distance < best_err) {
                best_err = distance;
                best_idx = j;
            }
        }
        indices[i] = best_idx;
        total_err += best_err;
    }
    return total_err;
}

} // anonymous namespace

void encode_block(const u8* rgba, u8* out, bool alpha) {
    // Check if any pixel is transparent (for punch-through mode).
    bool has_transparent = false;
    if (alpha) {
        for (u32 i = 0; i < 16; ++i) {
            if (rgba[i * 4 + 3] < ALPHA_TRANSPARENT_THRESHOLD) {
                has_transparent = true;
                break;
            }
        }
    }

    RGB min_color, max_color;
    find_bbox(rgba, min_color, max_color);
    inset_bbox(min_color, max_color);

    u16 c0 = pack_565(max_color.r, max_color.g, max_color.b); // brighter → c0
    u16 c1 = pack_565(min_color.r, min_color.g, min_color.b);

    if (!has_transparent) {
        // 4-colour opaque mode: ensure c0 > c1.
        if (c0 < c1)
            std::swap(c0, c1);
        if (c0 == c1) {
            // All-same colour: use c0 > c1 by bumping if possible.
            if (c0 < 0xFFFF)
                c0 = c0 + 1;
            else
                c1 = c1 - 1;
        }

        std::array<RGB, 4> palette{};
        build_palette_4c(c0, c1, palette);

        std::array<u32, 16> indices{};
        assign_indices_4c(rgba, palette, indices);
        pack_block(c0, c1, indices, out);
        return;
    }

    // 3-colour + transparent mode: ensure c0 <= c1.
    if (c0 > c1)
        std::swap(c0, c1);

    std::array<RGB, 4> palette{};
    build_palette_3c(c0, c1, palette);

    std::array<u32, 16> indices{};
    assign_indices_3c(rgba, palette, indices);
    pack_block(c0, c1, indices, out);
}

std::optional<Texture> encodeTexture(const Texture& src, bool alpha, std::string* out_error) {
    return transform_texture_impl(
        src, PixelFormat::RGBA8, PixelFormat::BC1, "bc1::encodeTexture",
        [alpha](std::span<const u8> data, u32 w, u32 h) {
            return encode_image_rgba8<8>(
                data, w, h, [alpha](const u8* b, u8* o) { encode_block(b, o, alpha); });
        },
        out_error);
}

} // namespace bc1
} // namespace whiteout::textures

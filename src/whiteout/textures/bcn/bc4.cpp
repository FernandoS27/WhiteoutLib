// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc4.cpp
/// @brief BC4 encode and decode implementation.

#include "bc4.h"

#include <array>
#include <cmath>
#include <limits>

namespace whiteout::textures {
namespace bc4 {

// ============================================================================
// Decode
// ============================================================================

void decode_block(const u8* block, u8* out) {
    const u32 endpoint0 = block[0];
    const u32 endpoint1 = block[1];

    std::array<u32, 8> palette{};
    bc4_build_palette(endpoint0, endpoint1, palette);

    // Unpack 48 bits of 3-bit indices (LSB-first) from bytes 2..7
    u64 bits = 0;
    for (u32 i = 0; i < 6; ++i)
        bits |= static_cast<u64>(block[2 + i]) << (i * 8);

    for (u32 i = 0; i < 16; ++i) {
        u32 idx = static_cast<u32>((bits >> (i * 3)) & 0x7);
        out[i] = static_cast<u8>(palette[idx]);
    }
}

namespace {

std::vector<u8> decode_image(std::span<const u8> bc4, u32 width, u32 height,
                             interfaces::WorkerPool* pool) {
    assert(width > 0 && height > 0);

    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_tall = (height + 3) / 4;
    assert(bc4.size() >= static_cast<size_t>(blocks_wide) * blocks_tall * 8);

    std::vector<u8> result(static_cast<size_t>(width) * height, 0);

    parallel_for_tiles(blocks_wide, blocks_tall, pool,
        [&](u32 bx0, u32 by0, u32 bx1, u32 by1) {
            for (u32 block_y = by0; block_y < by1; ++block_y) {
                for (u32 block_x = bx0; block_x < bx1; ++block_x) {
                    std::array<u8, 16> block_vals{};
                    decode_block(bc4.data() + (block_y * blocks_wide + block_x) * 8,
                                 block_vals.data());
                    scatter_channel_block(block_vals, width, height, block_x, block_y,
                                          result.data(), 1, 0);
                }
            }
        });

    return result;
}

} // anonymous namespace

std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::BC4, PixelFormat::R8, "bc4::decodeTexture",
        [pool](std::span<const u8> data, u32 w, u32 h) {
            return decode_image(data, w, h, pool);
        },
        out_error);
}

// ============================================================================
// Encode
// ============================================================================

namespace {

/// Compute total squared error for given endpoints and assign best indices.
u64 evaluate_endpoints(const u8* values, u32 e0, u32 e1, std::array<u8, 16>& indices) {
    std::array<u32, 8> palette{};
    bc4_build_palette(e0, e1, palette);

    u64 total_err = 0;
    for (u32 i = 0; i < 16; ++i) {
        u32 value = values[i];
        u32 best_idx = 0;
        u32 best_distance = 0xFFFFFFFFu;
        for (u32 j = 0; j < 8; ++j) {
            u32 distance = (value >= palette[j]) ? (value - palette[j]) : (palette[j] - value);
            if (distance < best_distance) {
                best_distance = distance;
                best_idx = j;
            }
        }
        indices[i] = static_cast<u8>(best_idx);
        total_err += static_cast<u64>(best_distance) * best_distance;
    }
    return total_err;
}

/// Pack endpoints + 16 three-bit indices into 8 bytes.
void pack_block(u32 e0, u32 e1, const std::array<u8, 16>& indices, u8* out) {
    out[0] = static_cast<u8>(e0);
    out[1] = static_cast<u8>(e1);

    // Pack 16 × 3-bit indices into 48 bits (6 bytes), LSB-first.
    u64 bits = 0;
    for (u32 i = 0; i < 16; ++i) {
        bits |= static_cast<u64>(indices[i] & 0x7) << (i * 3);
    }
    out[2] = static_cast<u8>(bits);
    out[3] = static_cast<u8>(bits >> 8);
    out[4] = static_cast<u8>(bits >> 16);
    out[5] = static_cast<u8>(bits >> 24);
    out[6] = static_cast<u8>(bits >> 32);
    out[7] = static_cast<u8>(bits >> 40);
}

// ============================================================================
// Optimal endpoint search
// ============================================================================

/// Refine endpoints for a given BC4 mode using iterative nudge search.
/// `e0_gt_e1` = true → 6-interpolated mode (e0 > e1)
/// `e0_gt_e1` = false → 4-interpolated+0/255 mode (e0 <= e1)
template <bool e0_gt_e1>
u64 bc4_refine_mode(const u8* values, u32& best_e0, u32& best_e1,
                    std::array<u8, 16>& best_indices) {
    u32 value_min = 255, value_max = 0;
    for (u32 i = 0; i < 16; ++i) {
        value_min = std::min(value_min, static_cast<u32>(values[i]));
        value_max = std::max(value_max, static_cast<u32>(values[i]));
    }

    u32 e0, e1;
    if constexpr (e0_gt_e1) {
        // 6-interp mode: e0 > e1
        e0 = value_max;
        e1 = value_min;
        if (e0 == e1 && e0 < 255)
            e0 = e1 + 1;
    } else {
        // 4-interp mode: e0 <= e1
        e0 = value_min;
        e1 = value_max;
        if (e0 > e1)
            std::swap(e0, e1);
    }

    std::array<u8, 16> indices{};
    u64 err = evaluate_endpoints(values, e0, e1, indices);
    best_e0 = e0;
    best_e1 = e1;
    best_indices = indices;

    // Refinement: nudge endpoints by ±1 and keep improvements
    for (int iter = 0; iter < 4; ++iter) {
        bool improved = false;
        for (i32 de0 = -1; de0 <= 1; ++de0) {
            for (i32 de1 = -1; de1 <= 1; ++de1) {
                if (de0 == 0 && de1 == 0)
                    continue;
                i32 new_e0 = static_cast<i32>(best_e0) + de0;
                i32 new_e1 = static_cast<i32>(best_e1) + de1;
                if (new_e0 < 0 || new_e0 > 255 || new_e1 < 0 || new_e1 > 255)
                    continue;
                if constexpr (e0_gt_e1) {
                    if (new_e0 <= new_e1)
                        continue;
                } else {
                    if (new_e0 > new_e1)
                        continue;
                }
                std::array<u8, 16> trial_indices{};
                u64 trial_error = evaluate_endpoints(values, static_cast<u32>(new_e0),
                                                     static_cast<u32>(new_e1), trial_indices);
                if (trial_error < err) {
                    err = trial_error;
                    best_e0 = static_cast<u32>(new_e0);
                    best_e1 = static_cast<u32>(new_e1);
                    best_indices = trial_indices;
                    improved = true;
                }
            }
        }
        if (!improved)
            break;
    }

    return err;
}

} // anonymous namespace

void encode_block(const u8* values, u8* out) {
    // Try both modes, keep the one with lower error.
    u32 e0_6, e1_6, e0_4, e1_4;
    std::array<u8, 16> idx_6{}, idx_4{};

    u64 err6 = bc4_refine_mode<true>(values, e0_6, e1_6, idx_6);
    u64 err4 = bc4_refine_mode<false>(values, e0_4, e1_4, idx_4);

    if (err6 <= err4) {
        pack_block(e0_6, e1_6, idx_6, out);
    } else {
        pack_block(e0_4, e1_4, idx_4, out);
    }
}

namespace {

std::vector<u8> encode_image(std::span<const u8> r8, u32 width, u32 height,
                             interfaces::WorkerPool* pool) {
    assert(width > 0 && height > 0);
    assert(r8.size() >= static_cast<size_t>(width) * height);

    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_tall = (height + 3) / 4;
    std::vector<u8> result(static_cast<size_t>(blocks_wide) * blocks_tall * 8);

    parallel_for_tiles(blocks_wide, blocks_tall, pool,
        [&](u32 bx0, u32 by0, u32 bx1, u32 by1) {
            for (u32 block_y = by0; block_y < by1; ++block_y) {
                for (u32 block_x = bx0; block_x < bx1; ++block_x) {
                    std::array<u8, 16> block{};
                    gather_channel_block(r8, width, height, block_x, block_y, block, 1, 0);

                    u32 block_offset = (block_y * blocks_wide + block_x) * 8;
                    encode_block(block.data(), result.data() + block_offset);
                }
            }
        });

    return result;
}

} // anonymous namespace

std::optional<Texture> encodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::R8, PixelFormat::BC4, "bc4::encodeTexture",
        [pool](std::span<const u8> data, u32 w, u32 h) {
            return encode_image(data, w, h, pool);
        },
        out_error);
}

} // namespace bc4
} // namespace whiteout::textures

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bcn_common.h
/// @brief Shared types, constants and helpers used by all BCn codec files.
///
/// Internal header — not part of the public include path.
/// Include this from the per-format encode/decode .cpp files only.

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include <whiteout/textures/texture.h>
#include <whiteout/utils/job_group.h>

namespace whiteout::textures {

// ============================================================================
// Bit-stream writer — writes bits into a 128-bit (16-byte) block, LSB-first.
// ============================================================================

struct BitWriter {
    std::array<u8, 16> data{};
    u32 pos = 0;

    constexpr void write(u64 value, u32 num_bits) {
        for (u32 i = 0; i < num_bits; ++i) {
            const u32 byte_idx = pos >> 3;
            const u32 bit_idx = pos & 7;
            data[byte_idx] |= static_cast<u8>(((value >> i) & 1u) << bit_idx);
            ++pos;
        }
    }
};

// ============================================================================
// Bit-stream reader — reads bits from a 128-bit (16-byte) block, LSB-first.
// ============================================================================

struct BitReader {
    const u8* data;
    u32 pos = 0;

    constexpr explicit BitReader(const u8* src) : data(src) {}

    constexpr u32 read(u32 num_bits) {
        u32 value = 0;
        for (u32 i = 0; i < num_bits; ++i) {
            const u32 byte_idx = pos >> 3;
            const u32 bit_idx = pos & 7;
            value |= ((data[byte_idx] >> bit_idx) & 1u) << i;
            ++pos;
        }
        return value;
    }
};

// ============================================================================
// Interpolation weight tables (BC7 / BC6H spec-defined)
// ============================================================================

static constexpr std::array<u32, 4> BCN_WEIGHT_2 = {0, 21, 43, 64};
static constexpr std::array<u32, 8> BCN_WEIGHT_3 = {0, 9, 18, 27, 37, 46, 55, 64};
static constexpr std::array<u32, 16> BCN_WEIGHT_4 = {0,  4,  9,  13, 17, 21, 26, 30,
                                                     34, 38, 43, 47, 51, 55, 60, 64};

constexpr u32 bcn_interpolate(u32 e0, u32 e1, u32 weight) {
    return (e0 * (64 - weight) + e1 * weight + 32) >> 6;
}

// ============================================================================

constexpr u32 bc7_unquantize(u32 val, u32 prec) {
    if (prec == 8)
        return val;
    return (val << (8 - prec)) | (val >> (2 * prec - 8));
}

// ============================================================================
// BC4 / BC3 / BC5 — 8-entry alpha/single-channel palette reconstruction
// ============================================================================

/// Build the 8-entry interpolation palette for a BC4 (or DXT5 alpha) block.
constexpr void bc4_build_palette(u32 endpoint0, u32 endpoint1, std::array<u32, 8>& palette) {
    palette[0] = endpoint0;
    palette[1] = endpoint1;
    if (endpoint0 > endpoint1) {
        // 6-interpolated mode
        palette[2] = (6 * endpoint0 + 1 * endpoint1 + 3) / 7;
        palette[3] = (5 * endpoint0 + 2 * endpoint1 + 3) / 7;
        palette[4] = (4 * endpoint0 + 3 * endpoint1 + 3) / 7;
        palette[5] = (3 * endpoint0 + 4 * endpoint1 + 3) / 7;
        palette[6] = (2 * endpoint0 + 5 * endpoint1 + 3) / 7;
        palette[7] = (1 * endpoint0 + 6 * endpoint1 + 3) / 7;
    } else {
        // 4-interpolated + 0/255 mode
        palette[2] = (4 * endpoint0 + 1 * endpoint1 + 2) / 5;
        palette[3] = (3 * endpoint0 + 2 * endpoint1 + 2) / 5;
        palette[4] = (2 * endpoint0 + 3 * endpoint1 + 2) / 5;
        palette[5] = (1 * endpoint0 + 4 * endpoint1 + 2) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }
}

// ============================================================================
// Texture creation helper: create a destination Texture matching src's
// structure (type / dimensions / mip count / layer count) but with `fmt`.
// ============================================================================

Texture make_texture_like(const Texture& src, PixelFormat format);

// ============================================================================
// Generic texture transform helper (encode or decode).
//
// Calls `transform_mip(src_data, width, height)` → vector<T>,
// then reinterpret-copies the result into the destination mip.
//
// TransformF: std::vector<T>(std::span<const SrcT>, u32 w, u32 h)
// ============================================================================

template <typename TransformF>
std::optional<Texture> transform_texture_impl(const Texture& src, PixelFormat src_fmt,
                                              PixelFormat dst_fmt, const char* function_name,
                                              TransformF transform_mip, std::string* out_error) {
    if (src.format() != src_fmt) {
        if (out_error)
            *out_error = std::string(function_name) + ": incompatible source format";
        return std::nullopt;
    }
    if (src.width() == 0 || src.height() == 0) {
        if (out_error)
            *out_error = std::string(function_name) + ": source has zero dimensions";
        return std::nullopt;
    }

    Texture dst = make_texture_like(src, dst_fmt);
    const u32 layers = src.layerCount();
    const u32 mips = src.mipCount();

    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mips; ++mip) {
            const auto& source_mip = src.mipLevel(mip, layer);
            auto src_data = src.mipData(mip, layer);
            auto dst_data = dst.mipData(mip, layer);
            auto result = transform_mip(src_data, source_mip.width, source_mip.height);

            const u8* bytes = reinterpret_cast<const u8*>(result.data());
            size_t byte_size = result.size() * sizeof(result[0]);

            if (byte_size != dst_data.size()) {
                if (out_error)
                    *out_error = std::string(function_name) + ": size mismatch at mip " +
                                 std::to_string(mip) + " layer " + std::to_string(layer);
                return std::nullopt;
            }
            std::memcpy(dst_data.data(), bytes, byte_size);
        }
    }
    return dst;
}

// ============================================================================
// Colour expansion helpers — expand 5-bit / 6-bit colour to 8-bit.
// ============================================================================

constexpr u32 expand5_to_8(u32 v) {
    return (v << 3) | (v >> 2);
}

constexpr u32 expand6_to_8(u32 v) {
    return (v << 2) | (v >> 4);
}

// ============================================================================
// Squared value helper.
// ============================================================================

constexpr u64 bcn_sq(i32 x) {
    return static_cast<u64>(x) * static_cast<u64>(x);
}

// ============================================================================
// RGBA colour type for encoder helpers (i32 per channel).
// ============================================================================

struct RGBA {
    i32 r, g, b, a;
};

/// Extract a single pixel from an interleaved RGBA8 buffer.
inline RGBA pixel_at(const u8* rgba, u32 idx) {
    return {rgba[idx * 4 + 0], rgba[idx * 4 + 1], rgba[idx * 4 + 2], rgba[idx * 4 + 3]};
}

/// Squared colour error including all four channels.
inline u64 colour_error_rgba(RGBA a, RGBA b) {
    return bcn_sq(a.r - b.r) + bcn_sq(a.g - b.g) + bcn_sq(a.b - b.b) + bcn_sq(a.a - b.a);
}

/// Squared colour error ignoring alpha.
inline u64 colour_error_rgb(RGBA a, RGBA b) {
    return bcn_sq(a.r - b.r) + bcn_sq(a.g - b.g) + bcn_sq(a.b - b.b);
}

// ============================================================================
// Generic gather_block: extract a 4×4 RGBA block with edge-clamping.
// Works with u8 (RGBA8) or u16 (RGBA16) or f32 (RGBA32F) element types.
// ============================================================================

template <typename T>
inline void gather_rgba_block(std::span<const T> data, u32 width, u32 block_x, u32 block_y,
                              std::array<T, 64>& block) {
    for (u32 pixel_y = 0; pixel_y < 4; ++pixel_y) {
        for (u32 pixel_x = 0; pixel_x < 4; ++pixel_x) {
            u32 src_x = std::min(block_x * 4 + pixel_x, width - 1);
            u32 src_y =
                std::min(block_y * 4 + pixel_y, (static_cast<u32>(data.size()) / (width * 4)) - 1);
            const u32 src_index = (src_y * width + src_x) * 4;
            const u32 dst_index = (pixel_y * 4 + pixel_x) * 4;
            block[dst_index + 0] = data[src_index + 0];
            block[dst_index + 1] = data[src_index + 1];
            block[dst_index + 2] = data[src_index + 2];
            block[dst_index + 3] = data[src_index + 3];
        }
    }
}

/// Convenience alias for u8.
inline void gather_rgba8_block(std::span<const u8> rgba, u32 width, u32 block_x, u32 block_y,
                               std::array<u8, 64>& block) {
    gather_rgba_block<u8>(rgba, width, block_x, block_y, block);
}

// ============================================================================
// Generic scatter_block: write a decoded 4×4 RGBA block back to the image.
// Works with u8 (RGBA8) or u16 (RGBA16) or f32 (RGBA32F) element types.
// ============================================================================

template <typename T>
inline void scatter_rgba_block(const std::array<T, 64>& block, u32 width, u32 height, u32 block_x,
                               u32 block_y, std::vector<T>& result) {
    for (u32 pixel_y = 0; pixel_y < 4; ++pixel_y) {
        for (u32 pixel_x = 0; pixel_x < 4; ++pixel_x) {
            const u32 x = block_x * 4 + pixel_x;
            const u32 y = block_y * 4 + pixel_y;
            if (x < width && y < height) {
                const u32 src_index = (pixel_y * 4 + pixel_x) * 4;
                const u32 dst_index = (y * width + x) * 4;
                result[dst_index + 0] = block[src_index + 0];
                result[dst_index + 1] = block[src_index + 1];
                result[dst_index + 2] = block[src_index + 2];
                result[dst_index + 3] = block[src_index + 3];
            }
        }
    }
}

/// Convenience alias for u8.
inline void scatter_rgba8_block(const std::array<u8, 64>& block, u32 width, u32 height, u32 block_x,
                                u32 block_y, std::vector<u8>& result) {
    scatter_rgba_block<u8>(block, width, height, block_x, block_y, result);
}

// ============================================================================
// Single-channel gather: extract one channel from an RGBA image into a 4×4
// block of scalar values, with edge-clamping.
// ============================================================================

void gather_channel_block(std::span<const u8> rgba, u32 width, u32 height, u32 block_x, u32 block_y,
                          std::array<u8, 16>& out, u32 stride, u32 channel_offset);

// ============================================================================
// Single-channel scatter: write a 4×4 block of scalar values into an image
// with the given number of channels per pixel.
// ============================================================================

void scatter_channel_block(const std::array<u8, 16>& values, u32 width, u32 height, u32 block_x,
                           u32 block_y, u8* result, u32 stride, u32 channel_offset);

// ============================================================================
// Parallel tile dispatch utility.
//
// Divides the block grid into tiles of up to kTileBlocks×kTileBlocks blocks
// (64×64 pixels) and submits them to a WorkerPool.
//
//   pool = nullptr → serial execution on the calling thread
//   pool != nullptr → each tile is submitted as a WorkerTask; pool->wait_idle()
//                     is called before returning
//
// TileFn signature: void(u32 bx_start, u32 by_start, u32 bx_end, u32 by_end)
// Each invocation processes all blocks in [bx_start,bx_end) × [by_start,by_end).
// ============================================================================

template <typename TileFn>
void parallel_for_tiles(u32 blocks_wide, u32 blocks_tall,
                        interfaces::WorkerPool* pool, TileFn&& tile_fn) {
    constexpr u32 kTileBlocks = 16; // 16×16 blocks = 64×64 pixels max

    const u32 tiles_wide = (blocks_wide + kTileBlocks - 1) / kTileBlocks;
    const u32 tiles_tall = (blocks_tall + kTileBlocks - 1) / kTileBlocks;
    const u32 total_tiles = tiles_wide * tiles_tall;

    if (!pool || total_tiles <= 1) {
        // Serial path — no pool.
        for (u32 ty = 0; ty < tiles_tall; ++ty) {
            for (u32 tx = 0; tx < tiles_wide; ++tx) {
                const u32 bx0 = tx * kTileBlocks;
                const u32 by0 = ty * kTileBlocks;
                tile_fn(bx0, by0, std::min(bx0 + kTileBlocks, blocks_wide),
                        std::min(by0 + kTileBlocks, blocks_tall));
            }
        }
        return;
    }

    utils::JobGroup jobGroup;
    // Parallel path — submit each tile to the pool.
    for (u32 ty = 0; ty < tiles_tall; ++ty) {
        for (u32 tx = 0; tx < tiles_wide; ++tx) {
            const u32 bx0 = tx * kTileBlocks;
            const u32 by0 = ty * kTileBlocks;
            const u32 bx1 = std::min(bx0 + kTileBlocks, blocks_wide);
            const u32 by1 = std::min(by0 + kTileBlocks, blocks_tall);
            jobGroup.add(1);
            interfaces::WorkerTask task{
                [bx0, by0, bx1, by1, &tile_fn, &jobGroup]() {
                    tile_fn(bx0, by0, bx1, by1);
                    jobGroup.done();
                }
            };
            pool->submit(task);
        }
    }
    jobGroup.wait();
}

// ============================================================================
// Generic decode_image for RGBA8 block formats.
//
// Iterates 4×4 blocks, calls DecodeBlockF(src_ptr, block_pixels) for each,
// then scatters into the output image.
//   BlockBytes : compressed block size in bytes (8 for BC1/BC4, 16 for others)
//   DecodeBlockF : void(const u8* src, u8* out_rgba64)
//   thread_count : 1 = serial, 0 = auto, >1 = that many threads
// ============================================================================

template <u32 BlockBytes, typename DecodeBlockF>
std::vector<u8> decode_image_rgba8(std::span<const u8> data, u32 width, u32 height,
                                   DecodeBlockF decode_block_fn,
                                   interfaces::WorkerPool* pool = nullptr) {
    assert(width > 0 && height > 0);

    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_tall = (height + 3) / 4;
    assert(data.size() >= static_cast<size_t>(blocks_wide) * blocks_tall * BlockBytes);

    std::vector<u8> result(static_cast<size_t>(width) * height * 4, 0);

    parallel_for_tiles(blocks_wide, blocks_tall, pool,
        [&](u32 bx0, u32 by0, u32 bx1, u32 by1) {
            for (u32 block_y = by0; block_y < by1; ++block_y) {
                for (u32 block_x = bx0; block_x < bx1; ++block_x) {
                    std::array<u8, 64> block_pixels{};
                    decode_block_fn(
                        data.data() + (block_y * blocks_wide + block_x) * BlockBytes,
                        block_pixels.data());
                    scatter_rgba8_block(block_pixels, width, height, block_x, block_y, result);
                }
            }
        });

    return result;
}

// ============================================================================
// Generic encode_image for RGBA8 block formats.
//
// Iterates 4×4 blocks, gathers RGBA8 pixels, calls
// EncodeBlockF(block_rgba64, dst_ptr) for each.
//   BlockBytes : compressed block size in bytes (8 for BC1/BC4, 16 for others)
//   EncodeBlockF : void(const u8* rgba64, u8* out)
//   thread_count : 1 = serial, 0 = auto, >1 = that many threads
// ============================================================================

template <u32 BlockBytes, typename EncodeBlockF>
std::vector<u8> encode_image_rgba8(std::span<const u8> rgba, u32 width, u32 height,
                                   EncodeBlockF encode_block_fn,
                                   interfaces::WorkerPool* pool = nullptr) {
    assert(width > 0 && height > 0);
    assert(rgba.size() >= static_cast<size_t>(width) * height * 4);

    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_tall = (height + 3) / 4;
    std::vector<u8> result(static_cast<size_t>(blocks_wide) * blocks_tall * BlockBytes);

    parallel_for_tiles(blocks_wide, blocks_tall, pool,
        [&](u32 bx0, u32 by0, u32 bx1, u32 by1) {
            for (u32 block_y = by0; block_y < by1; ++block_y) {
                for (u32 block_x = bx0; block_x < bx1; ++block_x) {
                    std::array<u8, 64> block{};
                    gather_rgba8_block(rgba, width, block_x, block_y, block);
                    encode_block_fn(
                        block.data(),
                        result.data() + (block_y * blocks_wide + block_x) * BlockBytes);
                }
            }
        });

    return result;
}

} // namespace whiteout::textures

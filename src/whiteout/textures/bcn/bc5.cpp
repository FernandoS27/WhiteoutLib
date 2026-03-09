// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc5.cpp
/// @brief BC5 encode and decode implementation.
///
/// BC5 is two independent BC4 blocks packed consecutively (16 bytes per
/// 4×4 block).  This file re-uses bc4::decode_block and bc4::encode_block.

#include "bc4.h"
#include "bc5.h"

#include <array>

namespace whiteout::textures {
namespace bc5 {

// ============================================================================
// Decode
// ============================================================================

namespace {

void decode_block(const u8* block, u8* out_ch0, u8* out_ch1) {
    bc4::decode_block(block, out_ch0);
    bc4::decode_block(block + 8, out_ch1);
}

std::vector<u8> decode_image(std::span<const u8> bc5, u32 width, u32 height) {
    assert(width > 0 && height > 0);

    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_tall = (height + 3) / 4;
    assert(bc5.size() >= static_cast<size_t>(blocks_wide) * blocks_tall * 16);

    // Output: 2 bytes per pixel (R, G interleaved)
    std::vector<u8> result(static_cast<size_t>(width) * height * 2, 0);

    for (u32 block_y = 0; block_y < blocks_tall; ++block_y) {
        for (u32 block_x = 0; block_x < blocks_wide; ++block_x) {
            std::array<u8, 16> channel0{}, channel1{};
            decode_block(bc5.data() + (block_y * blocks_wide + block_x) * 16, channel0.data(),
                         channel1.data());
            scatter_channel_block(channel0, width, height, block_x, block_y, result.data(), 2, 0);
            scatter_channel_block(channel1, width, height, block_x, block_y, result.data(), 2, 1);
        }
    }

    return result;
}

} // anonymous namespace

std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error) {
    return transform_texture_impl(
        src, PixelFormat::BC5, PixelFormat::RGBA8, "bc5::decodeTexture",
        [](std::span<const u8> data, u32 w, u32 h) {
            auto decoded = decode_image(data, w, h);
            // ch0 → R, ch1 → G, B = 0, A = 255
            std::vector<u8> rgba(static_cast<size_t>(w) * h * 4);
            for (u32 i = 0; i < w * h; ++i) {
                rgba[i * 4 + 0] = decoded[i * 2 + 0]; // R
                rgba[i * 4 + 1] = decoded[i * 2 + 1]; // G
                rgba[i * 4 + 2] = 0;                  // B
                rgba[i * 4 + 3] = 255;                // A
            }
            return rgba;
        },
        out_error);
}

// ============================================================================
// Encode
// ============================================================================

namespace {

/// Return the RGBA byte offsets for a given channel pair.
void channel_offsets(ChannelPair pair, u32& offset0, u32& offset1) {
    switch (pair) {
    case ChannelPair::RG:
        offset0 = 0;
        offset1 = 1;
        break;
    case ChannelPair::RB:
        offset0 = 0;
        offset1 = 2;
        break;
    case ChannelPair::RA:
        offset0 = 0;
        offset1 = 3;
        break;
    case ChannelPair::GB:
        offset0 = 1;
        offset1 = 2;
        break;
    case ChannelPair::GA:
        offset0 = 1;
        offset1 = 3;
        break;
    case ChannelPair::BA:
        offset0 = 2;
        offset1 = 3;
        break;
    }
}

void encode_block(const u8* ch0, const u8* ch1, u8* out) {
    // First 8 bytes: BC4 block for channel 0
    bc4::encode_block(ch0, out);
    // Second 8 bytes: BC4 block for channel 1
    bc4::encode_block(ch1, out + 8);
}

std::vector<u8> encode_image(std::span<const u8> rgba, u32 width, u32 height,
                             ChannelPair channels) {
    assert(width > 0 && height > 0);
    assert(rgba.size() >= static_cast<size_t>(width) * height * 4);

    u32 offset0, offset1;
    channel_offsets(channels, offset0, offset1);

    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_tall = (height + 3) / 4;
    std::vector<u8> result(static_cast<size_t>(blocks_wide) * blocks_tall * 16);

    for (u32 block_y = 0; block_y < blocks_tall; ++block_y) {
        for (u32 block_x = 0; block_x < blocks_wide; ++block_x) {
            // Extract 4×4 two-channel blocks with clamped edges
            std::array<u8, 16> block0{}, block1{};
            gather_channel_block(rgba, width, height, block_x, block_y, block0, 4, offset0);
            gather_channel_block(rgba, width, height, block_x, block_y, block1, 4, offset1);

            u32 block_offset = (block_y * blocks_wide + block_x) * 16;
            encode_block(block0.data(), block1.data(), result.data() + block_offset);
        }
    }

    return result;
}

} // anonymous namespace

std::optional<Texture> encodeTexture(const Texture& src, ChannelPair channels,
                                     std::string* out_error) {
    return transform_texture_impl(
        src, PixelFormat::RGBA8, PixelFormat::BC5, "bc5::encodeTexture",
        [channels](std::span<const u8> data, u32 w, u32 h) {
            return encode_image(data, w, h, channels);
        },
        out_error);
}

} // namespace bc5
} // namespace whiteout::textures

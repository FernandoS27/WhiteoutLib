// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc3.cpp
/// @brief BC3 (DXT5) encode and decode implementation.

#include "bc1.h"
#include "bc3.h"
#include "bc4.h"

#include <array>

namespace whiteout::textures {
namespace bc3 {

// ============================================================================
// Decode
// ============================================================================

namespace {

void decode_block(const u8* block, u8* out) {
    // ---- Decode colour from the BC1 sub-block (bytes 8..15) ----
    bc1::decode_block(block + 8, out);

    // ---- Decode alpha from the BC4-style sub-block (bytes 0..7) ----
    std::array<u8, 16> alpha_vals{};
    bc4::decode_block(block, alpha_vals.data());

    for (u32 i = 0; i < 16; ++i)
        out[i * 4 + 3] = alpha_vals[i];
}

} // anonymous namespace

std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::BC3, PixelFormat::RGBA8, "bc3::decodeTexture",
        [pool](std::span<const u8> data, u32 w, u32 h) {
            return decode_image_rgba8<16>(data, w, h, decode_block, pool);
        },
        out_error);
}

// ============================================================================
// Encode
// ============================================================================

namespace {

void encode_block(const u8* rgba, u8* out) {
    // ---- Alpha: extract alpha channel, encode as BC4 block ----
    std::array<u8, 16> alpha_vals{};
    for (u32 i = 0; i < 16; ++i)
        alpha_vals[i] = rgba[i * 4 + 3];

    bc4::encode_block(alpha_vals.data(), out);

    // ---- Colour: encode as a BC1 opaque block ----
    bc1::encode_block(rgba, out + 8, /*alpha=*/false);
}

} // anonymous namespace

std::optional<Texture> encodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::RGBA8, PixelFormat::BC3, "bc3::encodeTexture",
        [pool](std::span<const u8> data, u32 w, u32 h) {
            return encode_image_rgba8<16>(data, w, h, encode_block, pool);
        },
        out_error);
}

} // namespace bc3
} // namespace whiteout::textures

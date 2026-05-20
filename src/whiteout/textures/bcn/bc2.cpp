// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc2.cpp
/// @brief BC2 (DXT3) encode and decode implementation.

#include "bc1.h"
#include "bc2.h"

namespace whiteout::textures {
namespace bc2 {

// ============================================================================
// Decode
// ============================================================================

namespace {

void decode_block(const u8* block, u8* out) {
    // ---- Decode colour from the BC1 sub-block (bytes 8..15) ----
    bc1::decode_block(block + 8, out);

    // ---- Replace alpha with explicit 4-bit values from bytes 0..7 ----
    for (u32 i = 0; i < 16; i += 2) {
        u8 const packed = block[i / 2];
        u8 const alpha0_4bit = packed & 0x0F;
        u8 const alpha1_4bit = (packed >> 4) & 0x0F;
        // Expand 4-bit to 8-bit: a4 * 17 = a4 | (a4 << 4)
        out[i * 4 + 3] = static_cast<u8>(alpha0_4bit | (alpha0_4bit << 4));
        out[(i + 1) * 4 + 3] = static_cast<u8>(alpha1_4bit | (alpha1_4bit << 4));
    }
}

} // anonymous namespace

std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::BC2, PixelFormat::RGBA8, "bc2::decodeTexture",
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
    // ---- Alpha: quantize each texel's A to 4 bits, pack 2 per byte ----
    for (u32 i = 0; i < 16; i += 2) {
        u8 const a0 = static_cast<u8>((rgba[i * 4 + 3] * 15 + 127) / 255);
        u8 const a1 = static_cast<u8>((rgba[(i + 1) * 4 + 3] * 15 + 127) / 255);
        out[i / 2] = static_cast<u8>(a0 | (a1 << 4));
    }

    // ---- Colour: encode as a BC1 opaque block (no punch-through) ----
    bc1::encode_block(rgba, out + 8, /*alpha=*/false);
}

} // anonymous namespace

std::optional<Texture> encodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::RGBA8, PixelFormat::BC2, "bc2::encodeTexture",
        [pool](std::span<const u8> data, u32 w, u32 h) {
            return encode_image_rgba8<16>(data, w, h, encode_block, pool);
        },
        out_error);
}

} // namespace bc2
} // namespace whiteout::textures

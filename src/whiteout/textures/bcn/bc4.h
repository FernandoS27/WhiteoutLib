// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc4.h
/// @brief BC4 encode and decode.
///
/// BC4 (DXGI_FORMAT_BC4_UNORM) stores one 8-bit channel per 4x4 block
/// using 2 endpoint values and 16 three-bit indices (8 bytes per block).
///
/// No external dependencies -- pure C++20.

#pragma once

#include "bcn_common.h"

namespace whiteout::textures {
namespace bc4 {

// ---- Decode ----------------------------------------------------------------

/// Decode a single 8-byte BC4 block into 16 u8 values (row-major 4x4).
///
/// @param block  Pointer to 8 bytes of BC4 compressed data.
/// @param out    Pointer to 16 bytes that will receive the decoded values.
void decode_block(const u8* block, u8* out);

/// Decode a BC4 Texture into a new Texture with RGBA8 pixel format.
///
/// The decoded channel is placed in the R channel; G and B are set to 0,
/// A is set to 255.
///
/// @param src       Source texture (must be BC4).
/// @param out_error Optional string to receive an error message on failure.
/// @return The decoded texture, or std::nullopt on error.
std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error = nullptr);

// ---- Encode ----------------------------------------------------------------

/// Which channel of the RGBA8 source to compress.
enum class Channel : u32 {
    R = 0,
    G = 1,
    B = 2,
    A = 3,
};

/// Compress a single 4x4 block (16 single-channel values) into an 8-byte
/// BC4 block.
///
/// @param values  Pointer to 16 u8 values in row-major order.
/// @param out     Pointer to 8 bytes that will receive the compressed block.
void encode_block(const u8* values, u8* out);

/// Compress an RGBA8 Texture into a new Texture with BC4 pixel format.
///
/// The source texture must have format PixelFormat::RGBA8.  All mip levels
/// and layers are individually compressed.
///
/// @param src       Source texture (must be RGBA8).
/// @param channel   Which channel to encode (default: R).
/// @param out_error Optional string to receive an error message on failure.
/// @return The compressed texture, or std::nullopt on error.
std::optional<Texture> encodeTexture(const Texture& src, Channel channel = Channel::R,
                                     std::string* out_error = nullptr);

} // namespace bc4
} // namespace whiteout::textures

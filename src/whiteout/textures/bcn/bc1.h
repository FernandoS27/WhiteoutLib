// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc1.h
/// @brief BC1 (DXT1) encode and decode.
///
/// Compresses RGBA8 source data into BC1 (DXT1 / DXGI_FORMAT_BC1_UNORM)
/// blocks and decompresses BC1 blocks back into RGBA8 pixels.
///
/// BC1 stores colour as two RGB565 endpoints and 16 x 2-bit indices
/// (8 bytes per 4x4 block).  An optional 1-bit punch-through alpha mode
/// is available when `alpha` is true.
///
/// No external dependencies -- pure C++20.

#pragma once

#include "bcn_common.h"

namespace whiteout::textures {
namespace bc1 {

// ---- Decode ----------------------------------------------------------------

/// Decode a single 8-byte BC1 block into 16 RGBA8 pixels (64 bytes).
///
/// @param block  Pointer to 8 bytes of BC1 compressed data.
/// @param out    Pointer to 64 bytes (16 x RGBA) that will receive the
///               decoded pixels in row-major order.
void decode_block(const u8* block, u8* out);

/// Decode a BC1 Texture into a new Texture with RGBA8 pixel format.
///
/// @param src       Source texture (must be BC1).
/// @param out_error Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The decoded texture, or std::nullopt on error.
std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

// ---- Encode ----------------------------------------------------------------

/// Compress a single 4x4 block of RGBA8 pixels into an 8-byte BC1 block.
///
/// @param rgba   Pointer to 64 bytes (16 x RGBA) in row-major order.
/// @param out    Pointer to 8 bytes that will receive the compressed block.
/// @param alpha  If true, enable 1-bit punch-through alpha.
void encode_block(const u8* rgba, u8* out, bool alpha = false);

/// Compress an RGBA8 Texture into a new Texture with BC1 pixel format.
///
/// @param src       Source texture (must be RGBA8).
/// @param alpha     If true, enable 1-bit punch-through alpha.
/// @param out_error Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The compressed texture, or std::nullopt on error.
std::optional<Texture> encodeTexture(const Texture& src, bool alpha = false,
                                     std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

} // namespace bc1
} // namespace whiteout::textures

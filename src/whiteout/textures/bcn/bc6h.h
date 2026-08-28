// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc6h.h
/// @brief BC6H encode and decode.
///
/// Decompresses BC6H blocks into RGBA32F pixels and compresses RGBA32F data
/// back into BC6H.  All 14 modes decode, in both the unsigned (BC6H_UF16) and
/// signed (BC6H_SF16) variants; the encoder emits unsigned mode 11 for a good
/// quality/speed trade-off.
///
/// No external dependencies -- pure C++20.

#pragma once

#include "bc6h_tables.h"
#include "bcn_common.h"

namespace whiteout::textures {
namespace bc6h {

// ---- Decode ----------------------------------------------------------------

/// Decode a BC6H Texture into a new Texture with RGBA32F pixel format.
///
/// @param src       Source texture (must be BC6H).
/// @param out_error Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The decoded texture, or std::nullopt on error.
std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

/// Decode raw BC6H blocks straight into RGBA32F pixels.
///
/// `PixelFormat::BC6H` denotes the unsigned variant, so signed (SF16) data has
/// no compressed representation in `Texture` and callers holding it must decode
/// at load time.
///
/// @param blocks   Block data; at least ceil(w/4)*ceil(h/4)*16 bytes.
/// @param isSigned true for BC6H_SF16, false for BC6H_UF16.
/// @return width*height*4 floats, or an empty vector if `blocks` is too small.
std::vector<f32> decodeBlocks(std::span<const u8> blocks, u32 width, u32 height, bool isSigned,
                              interfaces::WorkerPool* pool = nullptr);

// ---- Encode ----------------------------------------------------------------

/// Compress an RGBA32F Texture into a new Texture with BC6H pixel format.
///
/// Internally converts float values to half-float precision for encoding.
///
/// @param src       Source texture (must be RGBA32F).
/// @param out_error Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The compressed texture, or std::nullopt on error.
std::optional<Texture> encodeTexture(const Texture& src, std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

} // namespace bc6h
} // namespace whiteout::textures

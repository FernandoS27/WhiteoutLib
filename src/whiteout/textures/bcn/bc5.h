// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc5.h
/// @brief BC5 encode and decode.
///
/// BC5 (DXGI_FORMAT_BC5_UNORM) is two independent BC4 blocks packed
/// together -- one per channel -- giving 16 bytes per 4x4 block.
/// Commonly used for tangent-space normal maps (RG).
///
/// No external dependencies -- pure C++20.

#pragma once

#include "bcn_common.h"

namespace whiteout::textures {
namespace bc5 {

// ---- Decode ----------------------------------------------------------------

/// Decode a BC5 Texture into a new Texture with RG8 pixel format.
///
/// Channel 0 -> R, Channel 1 -> G.
///
/// @param src       Source texture (must be BC5).
/// @param out_error Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The decoded texture, or std::nullopt on error.
std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

// ---- Encode ----------------------------------------------------------------

/// Compress an RG8 Texture into a new Texture with BC5 pixel format.
///
/// The source texture must have format PixelFormat::RG8.  All mip levels
/// and layers are individually compressed.
///
/// @param src       Source texture (must be RG8).
/// @param out_error Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The compressed texture, or std::nullopt on error.
std::optional<Texture> encodeTexture(const Texture& src, std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

} // namespace bc5
} // namespace whiteout::textures

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

/// Decode a BC5 Texture into a new Texture with RGBA8 pixel format.
///
/// Channel 0 -> R, Channel 1 -> G, B = 0, A = 255.
///
/// @param src       Source texture (must be BC5).
/// @param out_error Optional string to receive an error message on failure.
/// @return The decoded texture, or std::nullopt on error.
std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error = nullptr);

// ---- Encode ----------------------------------------------------------------

/// Which pair of RGBA8 channels to compress.
enum class ChannelPair : u32 {
    RG = 0, ///< Red -> first BC4 block, Green -> second.
    RB = 1, ///< Red -> first, Blue -> second.
    RA = 2, ///< Red -> first, Alpha -> second.
    GB = 3, ///< Green -> first, Blue -> second.
    GA = 4, ///< Green -> first, Alpha -> second.
    BA = 5, ///< Blue -> first, Alpha -> second.
};

/// Compress an RGBA8 Texture into a new Texture with BC5 pixel format.
///
/// The source texture must have format PixelFormat::RGBA8.  All mip levels
/// and layers are individually compressed.
///
/// @param src       Source texture (must be RGBA8).
/// @param channels  Which channel pair to encode (default: RG).
/// @param out_error Optional string to receive an error message on failure.
/// @return The compressed texture, or std::nullopt on error.
std::optional<Texture> encodeTexture(const Texture& src, ChannelPair channels = ChannelPair::RG,
                                     std::string* out_error = nullptr);

} // namespace bc5
} // namespace whiteout::textures

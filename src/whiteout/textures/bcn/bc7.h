// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7.h
/// @brief BC7 encode and decode.
///
/// Decompresses BC7 (DXGI_FORMAT_BC7_UNORM) blocks into RGBA8 pixels
/// and compresses RGBA8 data back into BC7.  All 8 BC7 modes (0-7)
/// are fully supported.
///
/// No external dependencies -- pure C++20.

#pragma once

#include "bc7_tables.h"
#include "bcn_common.h"

namespace whiteout::textures {
namespace bc7 {

// ---- Decode ----------------------------------------------------------------

/// Decode a BC7 Texture into a new Texture with RGBA8 pixel format.
///
/// @param src       Source texture (must be BC7).
/// @param out_error Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The decoded texture, or std::nullopt on error.
std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

// ---- Encode ----------------------------------------------------------------

/// Quality preset for the BC7 encoder.
enum class Quality : u32 {
    /// Fast encoding -- uses mode 6 only (1-subset, 4-bit indices, full RGBA).
    /// Fastest speed, lowest quality.
    Fast = 0,

    /// Normal encoding -- tries modes 6, 5, and 4, picks best per block.
    /// Good balance of speed and quality.
    Normal = 1,

    /// High quality -- tries all 8 BC7 modes with principal-component
    /// axis refinement and partition search.  Slower but produces
    /// noticeably better results on textures with high-frequency detail
    /// or smooth gradients.
    High = 2,
};

/// Compress an RGBA8 Texture into a new Texture with BC7 pixel format.
///
/// The source texture must have format PixelFormat::RGBA8.  All mip levels
/// and layers are individually compressed.  The returned texture has the
/// same dimensions, mip count, layer count, and type as the source but
/// with format PixelFormat::BC7.
///
/// @param src      Source texture (must be RGBA8).
/// @param quality  Encoding quality preset.
/// @param out_error  Optional string to receive an error message on failure.
/// @param pool  Optional WorkerPool for parallel tile processing.
/// @return The compressed texture, or std::nullopt on error.
std::optional<Texture> encodeTexture(const Texture& src, Quality quality = Quality::Normal,
                                     std::string* out_error = nullptr,
                                     interfaces::WorkerPool* pool = nullptr);

} // namespace bc7
} // namespace whiteout::textures

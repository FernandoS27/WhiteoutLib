// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Metadata and save options for the Diablo II: Resurrected `.texture`
 *        format.
 */

#include <optional>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::d2r_texture {

// ============================================================================
// Parsed metadata
// ============================================================================

/// Metadata extracted from a `.texture` file by the parser.
struct D2rTextureInfo {
    u16 formatCode = 0;                       ///< Raw `format` word from the header.
    PixelFormat pixelFormat = PixelFormat::RGBA8; ///< Decoded library pixel format.
    bool isSrgb = false;                      ///< Intended colour space.
    u32 width = 0;                            ///< Mip 0 width.
    u32 height = 0;                           ///< Mip 0 height.
    u32 mipCount = 0;                         ///< Number of stored mip levels.
};

// ============================================================================
// Save options
// ============================================================================

/// Controls how a Texture is serialised into a `.texture` container.
struct SaveOptions {
    /// Force a specific `format` code instead of deriving it from the
    /// texture's PixelFormat / sRGB flag. Use to reproduce a file byte-for-byte
    /// (e.g. the linear-BC3 code 61 vs the sRGB-BC3 code 62). When unset the
    /// writer derives the code automatically.
    std::optional<u16> formatCode;
};

} // namespace whiteout::textures::d2r_texture

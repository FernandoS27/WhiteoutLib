// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Type definitions and save options for the BLP texture format
 *
 * This file defines:
 * - BLP version, encoding, and alpha-depth enumerations
 * - SaveOptions structure controlling how textures are serialised
 */

#include <whiteout/common_types.h>

namespace whiteout::textures::blp {

// ============================================================================
// Enumerations
// ============================================================================

/// BLP file format version to produce on save.
enum class BlpVersion {
    BLP1, ///< Warcraft III / Reign of Chaos era format.
    BLP2, ///< The Burning Crusade era and later format.
};

/// Internal pixel encoding stored inside the BLP container.
enum class BlpEncoding {
    Infer,      ///< Auto-select based on texture format and version (default).
    BGRA,       ///< Uncompressed 32-bit BGRA pixels (BLP2 only).
    DXT,        ///< Block-compressed BC1 / BC2 / BC3 (BLP2 only; format derived from texture).
    Palettized, ///< 256-colour palette with a separate per-pixel alpha channel (BLP1 and BLP2).
    JPEG,       ///< JPEG-compressed with raw BGRA component order (BLP1 and BLP2).
};

/// Alpha channel precision for Palettized and JPEG encodings.
enum class BlpAlphaDepth {
    Zero = 0,  ///< No alpha (fully opaque).
    One = 1,   ///< 1-bit alpha (Palettized only).
    Four = 4,  ///< 4-bit alpha (Palettized only).
    Eight = 8, ///< 8-bit full-precision alpha.
};

// ============================================================================
// Save options
// ============================================================================

/// Controls how a Texture is serialised as a BLP file.
struct SaveOptions {
    BlpVersion version = BlpVersion::BLP2;      ///< BLP container version.
    BlpEncoding encoding = BlpEncoding::Infer;  ///< Internal pixel encoding.
    BlpAlphaDepth alpha = BlpAlphaDepth::Eight; ///< Alpha bit depth.
    i32 jpegQuality = 75;                       ///< JPEG quality [1-100] (JPEG encoding only).
};

} // namespace whiteout::textures::blp

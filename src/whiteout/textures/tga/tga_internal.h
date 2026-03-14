// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file tga_internal.h
/// @brief TGA packed structs, constants, and helpers.
///
/// Internal header — not part of the public include path.

#pragma once

#include <cstdint>
#include <cstring>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::tga {

// ============================================================================
// Constants
// ============================================================================

/// TGA image type codes.
static constexpr u8 TGA_TYPE_NO_IMAGE = 0;
static constexpr u8 TGA_TYPE_UNCOMPRESSED_COLOR_MAPPED = 1;
static constexpr u8 TGA_TYPE_UNCOMPRESSED_TRUE_COLOR = 2;
static constexpr u8 TGA_TYPE_UNCOMPRESSED_GRAYSCALE = 3;
static constexpr u8 TGA_TYPE_RLE_COLOR_MAPPED = 9;
static constexpr u8 TGA_TYPE_RLE_TRUE_COLOR = 10;
static constexpr u8 TGA_TYPE_RLE_GRAYSCALE = 11;

/// TGA v2 footer signature.
static constexpr char TGA_FOOTER_SIGNATURE[] = "TRUEVISION-XFILE.";

// ============================================================================
// Packed Structures
// ============================================================================

#pragma pack(push, 1)

/// 18-byte TGA file header.
struct TgaHeader {
    u8 idLength;     ///< Length of the image ID field.
    u8 colorMapType; ///< 0 = no color map, 1 = color map present.
    u8 imageType;    ///< Image type code (see TGA_TYPE_* constants).
    // Color map specification (5 bytes).
    u16 colorMapOrigin; ///< Index of first color map entry.
    u16 colorMapLength; ///< Number of color map entries.
    u8 colorMapDepth;   ///< Bits per color map entry (15, 16, 24, 32).
    // Image specification (10 bytes).
    u16 xOrigin;        ///< X origin of the image.
    u16 yOrigin;        ///< Y origin of the image.
    u16 width;          ///< Image width in pixels.
    u16 height;         ///< Image height in pixels.
    u8 bitsPerPixel;    ///< Bits per pixel (8, 16, 24, 32).
    u8 imageDescriptor; ///< Bits 3–0: alpha channel depth.
                        ///< Bit 4: left-to-right ordering.
                        ///< Bit 5: top-to-bottom ordering.
};

#pragma pack(pop)

static_assert(sizeof(TgaHeader) == 18, "TgaHeader must be 18 bytes");

// ============================================================================
// Helpers
// ============================================================================

/// Returns true if bit 5 of imageDescriptor is set (top-down origin).
inline bool is_top_down(const TgaHeader& h) {
    return (h.imageDescriptor & 0x20) != 0;
}

/// Returns the number of alpha bits from the image descriptor.
inline u8 alpha_bits(const TgaHeader& h) {
    return h.imageDescriptor & 0x0F;
}

} // namespace whiteout::textures::tga

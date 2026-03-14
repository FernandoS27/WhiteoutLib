// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bmp_internal.h
/// @brief BMP packed structs, constants, and helpers.
///
/// Internal header — not part of the public include path.

#pragma once

#include <cstdint>
#include <cstring>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::bmp {

// ============================================================================
// Constants
// ============================================================================

/// BMP file signature: 'BM' (0x4D42 in little-endian).
static constexpr u16 BMP_SIGNATURE = 0x4D42;

/// Compression methods.
static constexpr u32 BI_RGB = 0;
static constexpr u32 BI_BITFIELDS = 3;

// ============================================================================
// Packed Structures
// ============================================================================

#pragma pack(push, 1)

/// 14-byte BMP file header.
struct BmpFileHeader {
    u16 signature;  ///< Must be 0x4D42 ('BM').
    u32 fileSize;   ///< Total file size in bytes.
    u16 reserved1;  ///< Reserved (0).
    u16 reserved2;  ///< Reserved (0).
    u32 dataOffset; ///< Byte offset from file start to pixel data.
};

/// 40-byte BITMAPINFOHEADER (most common DIB header).
struct BmpInfoHeader {
    u32 headerSize;      ///< Size of this header (40).
    i32 width;           ///< Image width in pixels.
    i32 height;          ///< Image height (positive = bottom-up, negative = top-down).
    u16 planes;          ///< Number of color planes (must be 1).
    u16 bitsPerPixel;    ///< Bits per pixel (24 or 32 for our purposes).
    u32 compression;     ///< Compression method (BI_RGB or BI_BITFIELDS).
    u32 imageSize;       ///< Size of raw bitmap data (may be 0 for BI_RGB).
    i32 xPixelsPerMeter; ///< Horizontal resolution.
    i32 yPixelsPerMeter; ///< Vertical resolution.
    u32 colorsUsed;      ///< Number of colors in the palette (0 = default).
    u32 colorsImportant; ///< Number of important colors (0 = all).
};

#pragma pack(pop)

static_assert(sizeof(BmpFileHeader) == 14, "BmpFileHeader must be 14 bytes");
static_assert(sizeof(BmpInfoHeader) == 40, "BmpInfoHeader must be 40 bytes");

// ============================================================================
// Helpers
// ============================================================================

/// Row stride for a BMP row (padded to 4-byte boundary).
inline u32 bmp_row_stride(u32 width, u16 bitsPerPixel) {
    return ((width * bitsPerPixel + 31) / 32) * 4;
}

} // namespace whiteout::textures::bmp

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file gif_internal.h
/// @brief GIF89a format constants and packed structures.

#include <whiteout/common_types.h>

#include <cstdint>

namespace whiteout::textures::gif {

// ============================================================================
// GIF89a constants
// ============================================================================

static constexpr u8 GIF_TRAILER = 0x3B;
static constexpr u8 GIF_EXTENSION_INTRODUCER = 0x21;
static constexpr u8 GIF_IMAGE_SEPARATOR = 0x2C;
static constexpr u8 GIF_GRAPHIC_CONTROL_LABEL = 0xF9;
static constexpr u8 GIF_APPLICATION_EXTENSION_LABEL = 0xFF;
static constexpr u8 GIF_BLOCK_TERMINATOR = 0x00;

// ============================================================================
// Packed header structures
// ============================================================================

#pragma pack(push, 1)

struct GifHeader {
    u8 signature[3]; // "GIF"
    u8 version[3];   // "89a"
};
static_assert(sizeof(GifHeader) == 6, "GifHeader must be 6 bytes");

struct LogicalScreenDescriptor {
    u16 width;
    u16 height;
    u8 packed; // Global Color Table Flag | Color Resolution | Sort Flag | Size of GCT
    u8 bgColorIndex;
    u8 pixelAspectRatio;
};
static_assert(sizeof(LogicalScreenDescriptor) == 7, "LogicalScreenDescriptor must be 7 bytes");

struct GraphicControlExtension {
    u8 introducer; // 0x21
    u8 label;      // 0xF9
    u8 blockSize;  // 4
    u8 packed;     // Disposal Method | User Input | Transparent Color Flag
    u16 delayTime; // centiseconds
    u8 transparentIdx;
    u8 terminator; // 0x00
};
static_assert(sizeof(GraphicControlExtension) == 8, "GraphicControlExtension must be 8 bytes");

struct ImageDescriptor {
    u8 separator; // 0x2C
    u16 left;
    u16 top;
    u16 width;
    u16 height;
    u8 packed; // Local Color Table Flag | Interlace | Sort | Size of LCT
};
static_assert(sizeof(ImageDescriptor) == 10, "ImageDescriptor must be 10 bytes");

#pragma pack(pop)

} // namespace whiteout::textures::gif

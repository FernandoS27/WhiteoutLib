// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file png_internal.h
/// @brief PNG packed structs, constants, CRC32, and helpers.
///
/// Internal header — not part of the public include path.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

#include "../../common/checksum.h"

namespace whiteout::textures::png {

// ============================================================================
// Constants
// ============================================================================

/// PNG file signature (8 bytes).
static constexpr std::array<u8, 8> PNG_SIGNATURE = {137, 80, 78, 71, 13, 10, 26, 10};

/// Chunk type codes (big-endian 4-byte integers).
static constexpr u32 CHUNK_IHDR = 0x49484452; // "IHDR"
static constexpr u32 CHUNK_PLTE = 0x504C5445; // "PLTE"
static constexpr u32 CHUNK_IDAT = 0x49444154; // "IDAT"
static constexpr u32 CHUNK_IEND = 0x49454E44; // "IEND"
static constexpr u32 CHUNK_tRNS = 0x74524E53; // "tRNS"
static constexpr u32 CHUNK_sRGB = 0x73524742; // "sRGB"
static constexpr u32 CHUNK_gAMA = 0x67414D41; // "gAMA"

/// PNG colour types.
static constexpr u8 COLOR_GRAYSCALE = 0;
static constexpr u8 COLOR_TRUECOLOR = 2;
static constexpr u8 COLOR_INDEXED = 3;
static constexpr u8 COLOR_GRAYSCALE_ALPHA = 4;
static constexpr u8 COLOR_TRUECOLOR_ALPHA = 6;

/// PNG filter types.
static constexpr u8 FILTER_NONE = 0;
static constexpr u8 FILTER_SUB = 1;
static constexpr u8 FILTER_UP = 2;
static constexpr u8 FILTER_AVERAGE = 3;
static constexpr u8 FILTER_PAETH = 4;

// CRC32 and Adler-32 live in src/whiteout/common/checksum.h.
// Re-export crc32() into this namespace for existing call sites.
using ::whiteout::crc32;

// ============================================================================
// Big-endian helpers
// ============================================================================

inline u32 readU32BE(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

inline u16 readU16BE(const u8* p) {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
}

inline void writeU32BE(u8* p, u32 value) {
    p[0] = static_cast<u8>((value >> 24) & 0xFF);
    p[1] = static_cast<u8>((value >> 16) & 0xFF);
    p[2] = static_cast<u8>((value >> 8) & 0xFF);
    p[3] = static_cast<u8>(value & 0xFF);
}

// ============================================================================
// PNG filter helpers
// ============================================================================

/// Paeth predictor (used by filter type 4).
inline u8 paethPredictor(u8 a, u8 b, u8 c) {
    i32 p = static_cast<i32>(a) + static_cast<i32>(b) - static_cast<i32>(c);
    i32 pa = std::abs(p - static_cast<i32>(a));
    i32 pb = std::abs(p - static_cast<i32>(b));
    i32 pc = std::abs(p - static_cast<i32>(c));
    if (pa <= pb && pa <= pc)
        return a;
    if (pb <= pc)
        return b;
    return c;
}

} // namespace whiteout::textures::png

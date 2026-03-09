// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file blp_internal.h
 * @brief Internal constants, packed structs, and helpers shared by parser and writer.
 *
 * This header is NOT part of the public API.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include <whiteout/common_types.h>

#include "../pixel_convert.h"

namespace whiteout::textures::blp {

using whiteout::textures::swap_red_blue;

// ============================================================================
// Format constants
// ============================================================================

static constexpr u32 BLP1_MAGIC = 0x31504C42u;
static constexpr u32 BLP2_MAGIC = 0x32504C42u;

static constexpr u32 MAX_MIP_LEVELS = 16;

static constexpr u32 CONTENT_JPEG = 0;
static constexpr u32 CONTENT_DIRECT = 1;

static constexpr u8 ENCODING_JPEG = 0;
static constexpr u8 ENCODING_PALETTIZED = 1;
static constexpr u8 ENCODING_DXT = 2;
static constexpr u8 ENCODING_BGRA = 3;
static constexpr u8 ENCODING_BGRA_DUP = 4;

// ============================================================================
// Packed on-disk structures
// ============================================================================

#pragma pack(push, 1)

struct BLP1Header {
    u32 magic;
    u32 content;
    u32 alphaBitDepth;
    u32 width;
    u32 height;
    u32 extra;
    u32 hasMipmaps;
};

struct MipmapLocator {
    std::array<u32, 16> mipOffsets;
    std::array<u32, 16> mipSizes;
};

struct BLP2Header {
    u32 magic;
    u32 version;
    u8 colorEncoding;
    u8 alphaBitDepth;
    u8 alphaType;
    u8 hasMipmaps;
    u32 width;
    u32 height;
    std::array<u32, 16> mipOffsets;
    std::array<u32, 16> mipSizes;
    std::array<u32, 256> palette;
};

#pragma pack(pop)

static_assert(sizeof(BLP1Header) == 28, "BLP1Header must be 28 bytes");
static_assert(sizeof(MipmapLocator) == 128, "MipmapLocator must be 128 bytes");
static_assert(sizeof(BLP2Header) == 0x494, "BLP2Header must be 1172 bytes");

// ============================================================================
// Shared inline helpers
// ============================================================================

/// Fill mip offset/size arrays from payload vectors.
inline void fill_mip_table(std::array<u32, 16>& offsets, std::array<u32, 16>& sizes,
                           const std::vector<std::vector<u8>>& payloads, u32 count,
                           u32 base_offset) {
    u32 cursor = base_offset;
    for (u32 mip = 0; mip < count; ++mip) {
        offsets[mip] = cursor;
        sizes[mip] = static_cast<u32>(payloads[mip].size());
        cursor += sizes[mip];
    }
}

/// Copy mip payloads into an output buffer at the recorded offsets.
inline void write_mip_payloads(u8* output, const std::array<u32, 16>& offsets,
                               const std::vector<std::vector<u8>>& payloads, u32 count) {
    for (u32 mip = 0; mip < count; ++mip) {
        std::memcpy(output + offsets[mip], payloads[mip].data(), payloads[mip].size());
    }
}

} // namespace whiteout::textures::blp

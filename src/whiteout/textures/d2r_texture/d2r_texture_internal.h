// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file d2r_texture_internal.h
/// @brief On-disk layout, format table, and shared helpers for the D2R
///        (Diablo II: Resurrected) `.texture` container.
///
/// Internal header — not part of the public include path. See
/// docs/D2R_TEXTURES_FORMAT_SPEC.md for the reverse-engineered format notes.

#include <array>
#include <cstring>
#include <optional>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::d2r_texture {

// ============================================================================
// Container constants
// ============================================================================

/// Magic "<DE(" (bytes 3C 44 45 28) as a little-endian u32.
static constexpr u32 D2R_MAGIC = 0x2845443Cu;

/// Only 2-D textures appear in the corpus; the `type` byte is always 2.
static constexpr u8 D2R_TYPE_2D = 2;

/// Constant trailing header word (purpose unknown; always 4).
static constexpr u32 D2R_RESERVED2 = 4;

static constexpr u32 D2R_HEADER_SIZE = 0x24; ///< Fixed header, mip table follows.
static constexpr u32 D2R_MIP_ENTRY_SIZE = 8;

// ============================================================================
// Pixel-format codes (header field at offset 0x04)
// ============================================================================
//
// The D2R header carries no explicit BCn id; the `format` word both selects
// the surface encoding and implies its colour space (see the spec).
static constexpr u16 D2R_FMT_RGBA8 = 31;      ///< Uncompressed B8G8R8A8 (BGRA byte order).
static constexpr u16 D2R_FMT_BC1 = 58;        ///< BC1 / DXT1, sRGB albedo.
static constexpr u16 D2R_FMT_BC3_LINEAR = 61; ///< BC3 / DXT5, linear (normals, ORM, masks).
static constexpr u16 D2R_FMT_BC3_SRGB = 62;   ///< BC3 / DXT5, sRGB albedo.
static constexpr u16 D2R_FMT_BC4 = 63;        ///< BC4, single channel, linear.
static constexpr u16 D2R_FMT_BC5 = 65;        ///< BC5, two channel, linear (distortion).

#pragma pack(push, 1)

/// Fixed 36-byte file header.
struct Header {
    u32 magic;     ///< D2R_MAGIC.
    u16 format;    ///< Pixel-format code (see D2R_FMT_*).
    u8 type;       ///< Dimensionality (2 = 2-D).
    u8 mipCount;   ///< Mip level count (duplicate of mipCountU32).
    u32 width;     ///< Mip 0 width in pixels.
    u32 height;    ///< Mip 0 height in pixels.
    u32 arraySize; ///< Array layers / depth (always 1).
    u32 reserved0; ///< Always 0.
    u32 reserved1; ///< Always 0.
    u32 mipCountU32;
    u32 reserved2; ///< Always 4.
};

/// One mip-directory entry. `offset` is a pointer to the mip's data measured
/// *from the address of the offset field itself* (self-relative).
struct MipEntry {
    u32 size;
    u32 offset;
};

#pragma pack(pop)

static_assert(sizeof(Header) == D2R_HEADER_SIZE, "D2R Header must be 36 bytes");
static_assert(sizeof(MipEntry) == D2R_MIP_ENTRY_SIZE, "D2R MipEntry must be 8 bytes");

// ============================================================================
// Format mapping
// ============================================================================

struct FormatMapping {
    PixelFormat format;
    bool is_srgb; ///< Intended colour space (albedo codes are sRGB).
    bool is_bgra; ///< True only for the uncompressed code (needs R/B swizzle).
};

/// Map a D2R `format` code to a library PixelFormat. Returns nullopt for
/// codes outside the known set.
inline std::optional<FormatMapping> d2r_format_to_pixel_format(u16 code) {
    switch (code) {
    case D2R_FMT_RGBA8:
        return FormatMapping{PixelFormat::RGBA8, false, true};
    case D2R_FMT_BC1:
        return FormatMapping{PixelFormat::BC1, true, false};
    case D2R_FMT_BC3_LINEAR:
        return FormatMapping{PixelFormat::BC3, false, false};
    case D2R_FMT_BC3_SRGB:
        return FormatMapping{PixelFormat::BC3, true, false};
    case D2R_FMT_BC4:
        return FormatMapping{PixelFormat::BC4, false, false};
    case D2R_FMT_BC5:
        return FormatMapping{PixelFormat::BC5, false, false};
    default:
        return std::nullopt;
    }
}

/// Pick the D2R `format` code that reproduces a PixelFormat. The sRGB flag
/// disambiguates the two BC3 codes. Returns nullopt for formats the container
/// cannot represent (BC2/BC6H/BC7, 16-bit, float, single/dual uncompressed).
inline std::optional<u16> pixel_format_to_d2r_format(PixelFormat fmt, bool is_srgb) {
    switch (fmt) {
    case PixelFormat::RGBA8:
        return D2R_FMT_RGBA8;
    case PixelFormat::BC1:
        return D2R_FMT_BC1;
    case PixelFormat::BC3:
        return is_srgb ? D2R_FMT_BC3_SRGB : D2R_FMT_BC3_LINEAR;
    case PixelFormat::BC4:
        return D2R_FMT_BC4;
    case PixelFormat::BC5:
        return D2R_FMT_BC5;
    default:
        return std::nullopt;
    }
}

/// Swap R and B for a tightly packed run of BGRA8 <-> RGBA8 pixels.
inline void swizzle_bgra_rgba(const u8* src, u8* dst, u64 pixel_count) {
    for (u64 i = 0; i < pixel_count; ++i) {
        const u8* s = src + i * 4;
        u8* d = dst + i * 4;
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
    }
}

/// Quick magic check for format detection.
inline bool looks_like_d2r_texture(std::span<const u8> buffer) {
    if (buffer.size() < D2R_HEADER_SIZE)
        return false;
    u32 magic = 0;
    std::memcpy(&magic, buffer.data(), sizeof(u32));
    if (magic != D2R_MAGIC)
        return false;
    u16 fmt = 0;
    std::memcpy(&fmt, buffer.data() + 4, sizeof(u16));
    return d2r_format_to_pixel_format(fmt).has_value();
}

} // namespace whiteout::textures::d2r_texture

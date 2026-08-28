// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// @file txtr_internal.h
/// @brief On-disk layout, surface-format table, and mip arithmetic for the
///        Overwatch TXTR container.
///
/// Every table here is transcribed from the Overwatch client's own code rather
/// than inferred from sample files; see docs/OW_TXTR_FORMAT.md for the
/// functions each one came from.
///
/// Internal header -- not part of the public include path.

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/txtr/types.h>

namespace whiteout::textures::txtr {

// ============================================================================
// Container constants
// ============================================================================

/// Smallest header the client will read fields out of.
static constexpr u32 TXTR_HEADER_SIZE = 0x20;

/// Rejection thresholds the client applies before it will touch a header.
static constexpr u8 TXTR_MAX_MIP_COUNT = 0x10;
static constexpr u8 TXTR_MAX_FORMAT_CODE = 0x71;

#pragma pack(push, 1)

/// The fields of the `004` header the client actually reads. Bytes 0x13-0x1F
/// are untouched by every function on the texture path; the inline pixel block
/// starts at 0x20.
struct Header {
    u16 flags;         ///< Dimension and behaviour bits.
    u8 mipCount;       ///< Total mip levels; rejected above 0x10.
    u8 format;         ///< Surface format code; rejected at 0x72 and above.
    u8 surfaces;       ///< Depth for 3D, array size or face count otherwise.
    u8 unknown05;      ///< Stored in the runtime object, never read back.
    u8 payloadCount;   ///< Payload chain length; rejected above 4.
    u8 unknown07;      ///< `== 1` sets runtime flag 0x4000.
    u16 width;         ///< Mip 0 width; rejected when zero.
    u16 height;        ///< Mip 0 height; rejected when zero.
    u32 inlineSize;    ///< Pixel bytes of the whole chain, not of the block at 0x20.
    u8 headerMipCount; ///< Mips resident inline; rejected above mipCount.
    u8 streamMask;     ///< 4-bit mask driving the quality-based mip skip.
    u8 unknown12;      ///< Stored in the runtime object, never read back.
};

/// The prefix each payload starts with, including the header's inline block
/// whenever `payloadCount` is non-zero.
struct PayloadPrefix {
    u32 mipsFromSmallest; ///< First covered mip is `mipCount - this - 1`.
    u32 mipCount;         ///< Consecutive mips this payload carries.
    u32 dataSize;         ///< Byte count of pixel data.
    u32 dataOffset;       ///< Offset of that data from the payload's own start.
};

#pragma pack(pop)

static_assert(sizeof(Header) == 0x13, "TXTR Header covers bytes 0x00-0x12");
static_assert(sizeof(PayloadPrefix) == kPayloadPrefixSize, "TXTR PayloadPrefix must be 16 bytes");

// ============================================================================
// Surface formats
// ============================================================================

/// How a surface format compresses, as the client's own format table reports
/// it. Kinds 8-10 only appear in console builds and have no BCn equivalent.
enum class Compression : u32 {
    None = 0,
    BC1 = 1,
    BC2 = 2,
    BC3 = 3,
    BC4 = 4,
    BC5 = 5,
    BC6H = 6,
    BC7 = 7,
    ConsoleA = 8,
    ConsoleB = 9,
    ConsoleC = 10,
};

/// Everything the size arithmetic needs about one surface format.
struct FormatInfo {
    Compression compression = Compression::None;
    u32 bitsPerPixel = 0; ///< For compressed formats, bits per *pixel*, not per block.
    u32 blockWidth = 1;
    u32 blockHeight = 1;
    u32 blockBytes = 0; ///< Only meaningful when compressed.
};

/// Compression kind and bits-per-pixel for a raw format code.
///
/// Codes 90, 93 and 94 are deliberately absent: the client's table has no
/// entry for them either, so a header naming one cannot be loaded.
inline std::optional<FormatInfo> txtr_format_info(u8 code) {
    const auto uncompressed = [](u32 bpp) { return FormatInfo{Compression::None, bpp, 1, 1, 0}; };
    const auto block = [](Compression c, u32 bpp, u32 dim, u32 bytes) {
        return FormatInfo{c, bpp, dim, dim, bytes};
    };

    if (code >= 1 && code <= 4)
        return uncompressed(128);
    if (code >= 5 && code <= 8)
        return uncompressed(96);
    if (code >= 9 && code <= 22)
        return uncompressed(64);
    if ((code >= 23 && code <= 48) || (code >= 68 && code <= 70) || code == 88 || code == 89 ||
        code == 91 || code == 92)
        return uncompressed(32);
    if ((code >= 49 && code <= 60) || code == 86 || code == 87)
        return uncompressed(16);
    if (code >= 61 && code <= 66)
        return uncompressed(8);
    if (code == 67)
        return uncompressed(1);
    if (code == 113)
        return uncompressed(12);

    if (code >= 71 && code <= 73)
        return block(Compression::BC1, 4, 4, 8);
    if (code >= 74 && code <= 76)
        return block(Compression::BC2, 8, 4, 16);
    if (code >= 77 && code <= 79)
        return block(Compression::BC3, 8, 4, 16);
    if (code >= 80 && code <= 82)
        return block(Compression::BC4, 4, 4, 8);
    if (code >= 83 && code <= 85)
        return block(Compression::BC5, 8, 4, 16);
    if (code >= 95 && code <= 97)
        return block(Compression::BC6H, 8, 4, 16);
    if (code >= 98 && code <= 100)
        return block(Compression::BC7, 8, 4, 16);

    // Console-only compressed formats. Recognised so the parser can name them
    // in its error message, but not decodable here.
    if (code >= 101 && code <= 103)
        return block(Compression::BC1, 4, 4, 8);
    if (code >= 104 && code <= 106)
        return block(Compression::ConsoleA, 8, 4, 16);
    if (code >= 107 && code <= 109)
        return block(Compression::ConsoleB, 4, 6, 16);
    if (code >= 110 && code <= 112)
        return block(Compression::ConsoleC, 2, 8, 16);

    return std::nullopt;
}

/// The DXGI_FORMAT value a raw format code corresponds to.
///
/// The client's enum is DXGI with one extra 32-bit format spliced in at 23, so
/// everything from there up sits one slot higher than its DXGI counterpart.
constexpr u32 txtr_code_to_dxgi(u8 code) {
    return code >= 23 ? static_cast<u32>(code) - 1u : static_cast<u32>(code);
}

/// How a surface format maps onto the library's own pixel formats.
struct FormatMapping {
    PixelFormat format = PixelFormat::RGBA8;
    bool isSrgb = false;
    bool isBgra = false;      ///< Needs an R/B swap after the copy.
    bool forceOpaque = false; ///< X8 alpha channel; fill it with 0xFF.
};

/// Map a raw format code onto a library PixelFormat, or nullopt when the
/// library has no equivalent encoding.
inline std::optional<FormatMapping> txtr_format_mapping(u8 code) {
    const auto info = txtr_format_info(code);
    if (!info)
        return std::nullopt;

    const u32 dxgi = txtr_code_to_dxgi(code);

    switch (info->compression) {
    case Compression::BC1:
        if (code > 100)
            return std::nullopt; // Console re-use of the BC1 kind.
        return FormatMapping{PixelFormat::BC1, dxgi == 72, false, false};
    case Compression::BC2:
        return FormatMapping{PixelFormat::BC2, dxgi == 75, false, false};
    case Compression::BC3:
        return FormatMapping{PixelFormat::BC3, dxgi == 78, false, false};
    case Compression::BC4:
        return FormatMapping{PixelFormat::BC4, false, false, false};
    case Compression::BC5:
        return FormatMapping{PixelFormat::BC5, false, false, false};
    case Compression::BC6H:
        return FormatMapping{PixelFormat::BC6H, false, false, false};
    case Compression::BC7:
        return FormatMapping{PixelFormat::BC7, dxgi == 99, false, false};
    case Compression::ConsoleA:
    case Compression::ConsoleB:
    case Compression::ConsoleC:
        return std::nullopt;
    case Compression::None:
        break;
    }

    switch (dxgi) {
    case 2: // R32G32B32A32_FLOAT
        return FormatMapping{PixelFormat::RGBA32F, false, false, false};
    case 11: // R16G16B16A16_UNORM
        return FormatMapping{PixelFormat::RGBA16, false, false, false};
    case 16: // R32G32_FLOAT
        return FormatMapping{PixelFormat::RG32F, false, false, false};
    case 28: // R8G8B8A8_UNORM
        return FormatMapping{PixelFormat::RGBA8, false, false, false};
    case 29: // R8G8B8A8_UNORM_SRGB
        return FormatMapping{PixelFormat::RGBA8, true, false, false};
    case 35: // R16G16_UNORM
        return FormatMapping{PixelFormat::RG16, false, false, false};
    case 41: // R32_FLOAT
        return FormatMapping{PixelFormat::R32F, false, false, false};
    case 49: // R8G8_UNORM
        return FormatMapping{PixelFormat::RG8, false, false, false};
    case 56: // R16_UNORM
        return FormatMapping{PixelFormat::R16, false, false, false};
    case 61: // R8_UNORM
    case 65: // A8_UNORM
        return FormatMapping{PixelFormat::R8, false, false, false};
    case 87: // B8G8R8A8_UNORM
        return FormatMapping{PixelFormat::RGBA8, false, true, false};
    case 88: // B8G8R8X8_UNORM
        return FormatMapping{PixelFormat::RGBA8, false, true, true};
    case 91: // B8G8R8A8_UNORM_SRGB
        return FormatMapping{PixelFormat::RGBA8, true, true, false};
    default:
        return std::nullopt;
    }
}

// ============================================================================
// Mip arithmetic
// ============================================================================

/// Byte layout of one mip level, matching the client's own computation.
struct MipInfo {
    u32 width = 0;      ///< Block-aligned width.
    u32 height = 0;     ///< Block-aligned height.
    u32 depth = 1;      ///< Slice count at this level.
    u32 rowPitch = 0;   ///< Bytes per row of blocks (or of pixels).
    u32 slicePitch = 0; ///< Bytes per depth slice.
    u64 size = 0;       ///< slicePitch * depth -- what the payload advances by.
};

/// Size and pitch of mip @p mip of a surface whose level 0 is
/// @p width x @p height x @p depth.
///
/// Dimensions are clamped up to one block before alignment, so a 1x1 mip of a
/// BCn surface still occupies a full 4x4 block.
inline MipInfo txtr_mip_info(const FormatInfo& fi, u32 width, u32 height, u32 depth, u32 mip) {
    const u32 bw = std::max(fi.blockWidth, 1u);
    const u32 bh = std::max(fi.blockHeight, 1u);

    const u32 w = std::max(width >> mip, bw);
    const u32 h = std::max(height >> mip, bh);

    MipInfo out;
    out.width = ((w + bw - 1) / bw) * bw;
    out.height = ((h + bh - 1) / bh) * bh;
    out.depth = std::max(depth >> mip, 1u);

    if (fi.compression == Compression::None) {
        out.rowPitch = (fi.bitsPerPixel * out.width) / 8;
        out.slicePitch = out.height * out.rowPitch;
    } else {
        out.rowPitch = fi.blockBytes * (out.width / bw);
        out.slicePitch = out.rowPitch * (out.height / bh);
    }
    out.size = static_cast<u64>(out.slicePitch) * out.depth;
    return out;
}

// ============================================================================
// Header helpers
// ============================================================================

/// Read the fixed header fields out of @p buffer. Returns nullopt when the
/// buffer is too short to hold them.
inline std::optional<Header> txtr_read_header(std::span<const u8> buffer) {
    if (buffer.size() < TXTR_HEADER_SIZE)
        return std::nullopt;
    Header h{};
    std::memcpy(&h, buffer.data(), sizeof(Header));
    return h;
}

/// Apply exactly the checks `teTextureResource_Create` applies.
inline bool txtr_header_is_valid(const Header& h) {
    if (h.format > TXTR_MAX_FORMAT_CODE)
        return false;
    if (h.mipCount == 0 || h.mipCount > TXTR_MAX_MIP_COUNT)
        return false;
    if (h.width == 0 || h.height == 0)
        return false;
    if (h.payloadCount > kMaxPayloadCount)
        return false;
    if (h.headerMipCount > h.mipCount)
        return false;
    return true;
}

/// Derive the surface topology from the flags word. Exactly one dimension bit
/// must be set; anything else is a header the client would refuse to draw.
inline std::optional<TxtrDimension> txtr_dimension(u16 flags) {
    const bool isArray = hasFlag(flags, TxtrFlags::Array);
    switch (flags & 0x000F) {
    case 0x0001:
        return isArray ? TxtrDimension::Texture1DArray : TxtrDimension::Texture1D;
    case 0x0002:
        return isArray ? TxtrDimension::Texture2DArray : TxtrDimension::Texture2D;
    case 0x0004:
        return TxtrDimension::Texture3D; // No array form exists for volumes.
    case 0x0008:
        return isArray ? TxtrDimension::TextureCubeArray : TxtrDimension::TextureCube;
    default:
        return std::nullopt;
    }
}

/// TXTR carries no magic, so detection has to lean entirely on the header
/// being self-consistent and naming a format we can actually decode.
inline bool looks_like_txtr(std::span<const u8> buffer) {
    const auto h = txtr_read_header(buffer);
    if (!h || !txtr_header_is_valid(*h))
        return false;
    if (!txtr_dimension(h->flags))
        return false;
    if (!txtr_format_mapping(h->format))
        return false;
    if (h->surfaces == 0)
        return false;
    // The field at 0x0C sizes the whole texture rather than this file, so it
    // cannot bound the buffer — a streamed texture's header is a fraction of it.
    return buffer.size() >= static_cast<u64>(kInlineDataOffset);
}

/// Swap R and B for a tightly packed run of BGRA8 <-> RGBA8 pixels.
inline void txtr_swizzle_bgra(u8* pixels, u64 pixelCount, bool forceOpaque) {
    for (u64 i = 0; i < pixelCount; ++i) {
        u8* p = pixels + i * 4;
        std::swap(p[0], p[2]);
        if (forceOpaque)
            p[3] = 0xFF;
    }
}

} // namespace whiteout::textures::txtr

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Metadata, flags, and GUID helpers for the Overwatch TXTR format.
 *
 * An Overwatch texture is split across two asset types: a small `004` header
 * that describes the surface and carries the smallest mip levels inline, and
 * up to three `04D` payload files carrying the larger ones.  The payload GUIDs
 * are never stored -- they are derived from the texture's own GUID, which is
 * what makePayloadGuid() reproduces.
 *
 * See docs/OW_TXTR_FORMAT.md for the reverse-engineered format notes.
 */

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::txtr {

// ============================================================================
// Asset types
// ============================================================================

/// Client asset type id for a texture header (extension `004`).
inline constexpr u32 kAssetTypeTexture = 0x004;

/// Second texture type id the client registers against the same handler
/// (extension `0F1`).
inline constexpr u32 kAssetTypeTextureAlt = 0x0F1;

/// Client asset type id for a texture payload (extension `04D`).
inline constexpr u32 kAssetTypeTexturePayload = 0x04D;

/// Largest payload chain length a header may declare. The client rejects any
/// header that exceeds it.
inline constexpr u32 kMaxPayloadCount = 4;

/// Byte offset of the inline pixel block inside the header.
inline constexpr u32 kInlineDataOffset = 0x20;

/// Size of the prefix every payload begins with -- including the inline block,
/// but only when the header references external payloads.
inline constexpr u32 kPayloadPrefixSize = 16;

// ============================================================================
// Header flags
// ============================================================================

/// Bits of the u16 flags word at header offset 0x00.
enum class TxtrFlags : u16 {
    None = 0x0000,
    Texture1D = 0x0001,     ///< Surface is one-dimensional.
    Texture2D = 0x0002,     ///< Surface is two-dimensional.
    Texture3D = 0x0004,     ///< Surface is a volume; `surfaces` is its depth.
    TextureCube = 0x0008,   ///< Surface is a cube map; height is forced to width.
    Srgb = 0x0020,          ///< Surface holds sRGB-encoded colour.
    Array = 0x0040,         ///< Promotes 1D/2D/Cube to their array form.
    KeepHeader = 0x0100,    ///< Runtime keeps the header blob after upload.
    PlatformTiled = 0x0800, ///< Pixel data is in a platform-specific tiled layout.
    KeepPayloads = 0x1000,  ///< Runtime keeps payload handles after upload.
    Flag4000 = 0x4000,      ///< Set from the header byte at 0x07; purpose unknown.
};

/// Bitwise OR for TxtrFlags.
constexpr TxtrFlags operator|(TxtrFlags a, TxtrFlags b) {
    return static_cast<TxtrFlags>(static_cast<u16>(a) | static_cast<u16>(b));
}

/// Bitwise AND for TxtrFlags.
constexpr TxtrFlags operator&(TxtrFlags a, TxtrFlags b) {
    return static_cast<TxtrFlags>(static_cast<u16>(a) & static_cast<u16>(b));
}

/// True when every bit of @p bits is set in @p value.
constexpr bool hasFlag(u16 value, TxtrFlags bits) {
    return (value & static_cast<u16>(bits)) == static_cast<u16>(bits);
}

// ============================================================================
// Dimensionality
// ============================================================================

/// Surface topology, as the client derives it from the dimension flag bits
/// plus TxtrFlags::Array.
enum class TxtrDimension : u32 {
    Texture1D = 0,
    Texture1DArray = 1,
    Texture2D = 2,
    Texture2DArray = 3,
    Texture3D = 4,
    TextureCube = 5,
    TextureCubeArray = 6,
};

// ============================================================================
// Parsed metadata
// ============================================================================

/// Everything the parser recovered from a `004` header.
struct TxtrInfo {
    u16 flags = 0;                                      ///< Raw flags word at offset 0x00.
    TxtrDimension dimension = TxtrDimension::Texture2D; ///< Derived topology.
    u8 formatCode = 0;                                  ///< Raw surface format byte at 0x03.
    u32 dxgiFormat = 0;                                 ///< Equivalent DXGI_FORMAT value.
    PixelFormat pixelFormat = PixelFormat::RGBA8;       ///< Decoded library format.
    bool isSrgb = false;                                ///< Surface format is an sRGB variant.

    u32 width = 0;     ///< Mip 0 width, from offset 0x08.
    u32 height = 0;    ///< Mip 0 height, from offset 0x0A.
    u32 surfaces = 0;  ///< Raw byte at 0x04: depth, array size, or face count.
    u32 depth = 1;     ///< Derived mip 0 depth (>1 only for 3D).
    u32 arraySize = 1; ///< Derived array slice count.
    u32 faceCount = 1; ///< 6 for cube maps, otherwise 1.
    u32 mipCount = 0;  ///< Total mip levels the surface declares.

    u32 payloadCount = 0;   ///< Payload chain length, from offset 0x06.
    u32 headerMipCount = 0; ///< Mips resident in the header, from offset 0x10.
    /// Pixel bytes of the whole mip chain, from offset 0x0C — the inline block
    /// and every payload file together, not the inline block alone.
    u32 inlineSize = 0;
    u8 streamMask = 0; ///< 4-bit mip-skip mask at offset 0x11.

    u8 unknown05 = 0; ///< Byte at 0x05; stored by the client, never read.
    u8 unknown07 = 0; ///< Byte at 0x07; `== 1` sets TxtrFlags::Flag4000.
    u8 unknown12 = 0; ///< Byte at 0x12; stored by the client, never read.

    /// Index of the largest mip any supplied payload covered. The parsed
    /// Texture starts there, so its width is `width >> baseMip`. Zero when the
    /// whole chain was available.
    u32 baseMip = 0;
    /// Number of external `04D` payloads the header references but that were
    /// not supplied to the parser.
    u32 missingPayloads = 0;
};

// ============================================================================
// Surface formats
// ============================================================================

/// The DXGI_FORMAT a raw surface format code corresponds to, or 0 when the code
/// names one of the client's own formats that DXGI has no equivalent for.
///
/// Transcribed from the client's engine-to-DXGI table. The enum tracks DXGI
/// exactly up to R10G10B10A2_UNORM, splices one client-only 32-bit format in at
/// 25, then runs one slot ahead of DXGI as far as BC7_UNORM_SRGB. Codes 101-112
/// are console-only block formats, and 113 is NV12.
constexpr u32 dxgiFormatFor(u8 code) {
    if (code <= 24)
        return code;
    if (code >= 26 && code <= 100)
        return code - 1u;
    if (code == 113)
        return 103; // DXGI_FORMAT_NV12
    return 0;
}

// ============================================================================
// GUID helpers
// ============================================================================

/// Reverse the low 12 bits of @p value.
constexpr u32 bitreverse12(u32 value) {
    u32 reversed = 0;
    for (int i = 0; i < 12; ++i, value >>= 1)
        reversed = (reversed << 1) | (value & 1);
    return reversed;
}

/// The client's asset type id for @p guid. The type is not the leading hex
/// digits: the client bit-reverses the top 12 bits and adds one.
constexpr u32 assetTypeId(u64 guid) {
    return bitreverse12(static_cast<u32>((guid >> 48) & 0xFFF)) + 1;
}

/// Rebuild a GUID's top 16 bits from an asset type id.
constexpr u64 assetTypeField(u32 typeId) {
    return static_cast<u64>(bitreverse12(typeId - 1)) << 48;
}

/// @return true if @p guid names a texture header (type `004` or `0F1`).
constexpr bool isTextureGuid(u64 guid) {
    const u32 type = assetTypeId(guid);
    return type == kAssetTypeTexture || type == kAssetTypeTextureAlt;
}

/// Derive the GUID of payload @p payloadIndex for the texture @p textureGuid.
///
/// Payload 0 is the block inside the header itself and has no file of its own;
/// the external payloads run from 1 to `payloadCount - 1`, with detail rising
/// alongside the index, so the last one carries mip 0.
constexpr u64 makePayloadGuid(u64 textureGuid, u32 payloadIndex) {
    const u32 type = assetTypeId(textureGuid);
    u64 variant = 3; // What the client stores for a type it does not recognise.
    if (type == kAssetTypeTexture)
        variant = 0;
    else if (type == kAssetTypeTextureAlt)
        variant = 1;

    return (textureGuid & 0x000000F0FFFFFFFFull) | (static_cast<u64>(payloadIndex & 0xF) << 32) |
           (variant << 40) | assetTypeField(kAssetTypeTexturePayload);
}

/// Recover the texture GUID a payload belongs to, or 0 when the payload's
/// variant field names no known texture type.
constexpr u64 payloadGuidToTextureGuid(u64 payloadGuid) {
    const u64 variant = (payloadGuid >> 40) & 3;
    u32 type = 0;
    if (variant == 0)
        type = kAssetTypeTexture;
    else if (variant == 1)
        type = kAssetTypeTextureAlt;
    else
        return 0;

    return (payloadGuid & 0x000000F0FFFFFFFFull) | assetTypeField(type);
}

/// The payload index encoded in a payload GUID.
constexpr u32 payloadIndexOf(u64 payloadGuid) {
    return static_cast<u32>((payloadGuid >> 32) & 0xF);
}

} // namespace whiteout::textures::txtr

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file tiff_internal.h
/// @brief TIFF tag IDs, packed structs, endian-aware readers, and tag helpers.
///
/// Internal header — not part of the public include path.

#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../../common/byte_order.h"

namespace whiteout::textures::tiff {

// ============================================================================
// Constants
// ============================================================================

constexpr u16 kByteOrderLE = 0x4949; // 'II'
constexpr u16 kByteOrderBE = 0x4D4D; // 'MM'
constexpr u16 kMagicClassic = 42;
constexpr u16 kMagicBigTiff = 43;
constexpr size_t kClassicHeaderSize = 8;

// ============================================================================
// FieldType (TIFF 6.0 Table 2)
// ============================================================================

enum class FieldType : u16 {
    Byte = 1,
    Ascii = 2,
    Short = 3,
    Long = 4,
    Rational = 5,
    SByte = 6,
    Undefined = 7,
    SShort = 8,
    SLong = 9,
    SRational = 10,
    Float = 11,
    Double = 12,
};

inline u32 fieldTypeBytes(u16 type) {
    switch (static_cast<FieldType>(type)) {
    case FieldType::Byte:
    case FieldType::Ascii:
    case FieldType::SByte:
    case FieldType::Undefined:
        return 1;
    case FieldType::Short:
    case FieldType::SShort:
        return 2;
    case FieldType::Long:
    case FieldType::SLong:
    case FieldType::Float:
        return 4;
    case FieldType::Rational:
    case FieldType::SRational:
    case FieldType::Double:
        return 8;
    }
    return 0; // unknown
}

// ============================================================================
// Tag IDs we consume (TIFF 6.0 + Adobe / TIFF Technical Note 2 for Deflate)
// ============================================================================

namespace Tag {
constexpr u16 ImageWidth = 256;
constexpr u16 ImageLength = 257;
constexpr u16 BitsPerSample = 258;
constexpr u16 Compression = 259;
constexpr u16 PhotometricInterpretation = 262;
constexpr u16 StripOffsets = 273;
constexpr u16 Orientation = 274;
constexpr u16 SamplesPerPixel = 277;
constexpr u16 RowsPerStrip = 278;
constexpr u16 StripByteCounts = 279;
constexpr u16 PlanarConfiguration = 284;
constexpr u16 Predictor = 317;
constexpr u16 ColorMap = 320;
constexpr u16 TileWidth = 322;
constexpr u16 TileLength = 323;
constexpr u16 TileOffsets = 324;
constexpr u16 TileByteCounts = 325;
constexpr u16 ExtraSamples = 338;
constexpr u16 SampleFormat = 339;
} // namespace Tag

// ============================================================================
// Compression schemes
// ============================================================================

namespace Compression {
constexpr u16 None = 1;
constexpr u16 CcittRLE = 2;
constexpr u16 CcittGroup3 = 3;
constexpr u16 CcittGroup4 = 4;
constexpr u16 Lzw = 5;
constexpr u16 OldJpeg = 6;
constexpr u16 NewJpeg = 7;
constexpr u16 Deflate = 8;          // TIFF Technical Note 2
constexpr u16 PackBits = 32773;     // baseline RLE
constexpr u16 AdobeDeflate = 32946; // older Adobe code, same wire format
} // namespace Compression

// ============================================================================
// Photometric interpretations
// ============================================================================

namespace Photometric {
constexpr u16 WhiteIsZero = 0;
constexpr u16 BlackIsZero = 1;
constexpr u16 Rgb = 2;
constexpr u16 Palette = 3;
constexpr u16 TransparencyMask = 4;
constexpr u16 CmykSeparated = 5;
constexpr u16 YCbCr = 6;
constexpr u16 CieLab = 8;
} // namespace Photometric

// ============================================================================
// Endian-aware readers
// ============================================================================

/// Thin wrapper picking BE or LE readers based on the file's header.
struct EndianReader {
    bool littleEndian = true;

    u16 readU16(const u8* p) const {
        return littleEndian ? ::whiteout::common::readLE16(p)
                            : ::whiteout::common::readBE16(p);
    }
    u32 readU32(const u8* p) const {
        return littleEndian ? ::whiteout::common::readLE32(p)
                            : ::whiteout::common::readBE32(p);
    }
};

// ============================================================================
// IFD entry & tag-value access
// ============================================================================

/// Decoded IFD entry — already endian-normalized.
struct IFDEntry {
    u16 tag = 0;
    u16 type = 0;
    u32 count = 0;
    /// Raw 4-byte inline value-or-offset, *as it appears in the file*.
    /// Use tagValueU32 / tagValuesU16Vec etc. to interpret correctly.
    std::array<u8, 4> raw{};
};

/// Total payload size of an IFD entry's values.
inline u64 entryPayloadBytes(const IFDEntry& e) {
    return static_cast<u64>(e.count) * fieldTypeBytes(e.type);
}

/// Return a span over the entry's value bytes. If the payload fits in 4
/// bytes it lives inline; otherwise the inline u32 is an offset into the
/// file buffer.
inline std::optional<std::span<const u8>>
entryValueSpan(const IFDEntry& e, const EndianReader& er, std::span<const u8> file) {
    u64 total = entryPayloadBytes(e);
    if (total == 0)
        return std::span<const u8>{};
    if (total <= 4) {
        // Inline value occupies the first `total` bytes of e.raw.
        return std::span<const u8>{e.raw.data(), static_cast<size_t>(total)};
    }
    u32 offset = er.readU32(e.raw.data());
    if (offset > file.size() || offset + total > file.size())
        return std::nullopt;
    return std::span<const u8>{file.data() + offset, static_cast<size_t>(total)};
}

/// Read entry as a single scalar u32 (works for BYTE / SHORT / LONG counts).
/// Returns nullopt on invalid type or count > 1.
inline std::optional<u32> tagAsU32(const IFDEntry& e, const EndianReader& er) {
    if (e.count != 1)
        return std::nullopt;
    switch (static_cast<FieldType>(e.type)) {
    case FieldType::Byte:
        return static_cast<u32>(e.raw[0]);
    case FieldType::Short:
        return static_cast<u32>(er.readU16(e.raw.data()));
    case FieldType::Long:
        return er.readU32(e.raw.data());
    default:
        return std::nullopt;
    }
}

/// Read entry as a vector of u16 / u32 values. Promotes BYTE/SHORT to u32.
inline std::optional<std::vector<u32>>
tagAsU32Vec(const IFDEntry& e, const EndianReader& er, std::span<const u8> file) {
    auto bytes = entryValueSpan(e, er, file);
    if (!bytes)
        return std::nullopt;
    std::vector<u32> out;
    out.reserve(e.count);
    switch (static_cast<FieldType>(e.type)) {
    case FieldType::Byte:
        for (u32 i = 0; i < e.count; ++i)
            out.push_back(static_cast<u32>((*bytes)[i]));
        return out;
    case FieldType::Short:
        for (u32 i = 0; i < e.count; ++i)
            out.push_back(static_cast<u32>(er.readU16(bytes->data() + i * 2)));
        return out;
    case FieldType::Long:
        for (u32 i = 0; i < e.count; ++i)
            out.push_back(er.readU32(bytes->data() + i * 4));
        return out;
    default:
        return std::nullopt;
    }
}

} // namespace whiteout::textures::tiff

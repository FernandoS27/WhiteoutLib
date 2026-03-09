// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Huffman codec primitives shared by the JPEG encoder and decoder.
///
/// Provides:
///   - BitstreamReader / BitstreamWriter — MSB-first bit I/O with JPEG
///     byte-stuffing (0xFF 0x00) and restart-marker awareness.
///   - HuffmanTable — canonical Huffman decoder with a fast 9-bit look-up
///     table and a slow-path fallback for longer codes.
///   - HuffmanEncodeTable — symbol-to-code mapping for the encoder.
///   - Standard Huffman table data (ITU-T T.81 Annex K).

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <whiteout/common_types.h>

#include "jpeg_common.h"

namespace whiteout {
namespace textures {
namespace jpeg {

// ============================================================================
// Constants
// ============================================================================

/// Width (in bits) of the fast Huffman look-up table.  Codes up to this length
/// are resolved in a single table probe.
inline constexpr i32 HUFFMAN_FAST_BITS = 9;

/// Precomputed bit-masks: BIT_MASK[n] == (1 << n) - 1.
inline constexpr std::array<u32, 17> BIT_MASK = {{
    0,
    1,
    3,
    7,
    15,
    31,
    63,
    127,
    255,
    511,
    1023,
    2047,
    4095,
    8191,
    16383,
    32767,
    65535,
}};

// ============================================================================
// Standard Huffman Tables (ITU-T T.81, Annex K)
// ============================================================================

/// DC luminance Huffman table (Table K.3).
/// Code length counts for lengths 1-16, followed by symbol values.
inline constexpr std::array<u8, 16> DC_LUMA_COUNTS = {{
    0,
    1,
    5,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
}};
inline constexpr std::array<u8, 12> DC_LUMA_SYMBOLS = {{
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
}};

/// AC luminance Huffman table (Table K.5).
inline constexpr std::array<u8, 16> AC_LUMA_COUNTS = {{
    0,
    2,
    1,
    3,
    3,
    2,
    4,
    3,
    5,
    5,
    4,
    4,
    0,
    0,
    1,
    0x7D,
}};
inline constexpr std::array<u8, 162> AC_LUMA_SYMBOLS = {{
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52,
    0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3,
    0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
    0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
}};

// ============================================================================
// MSB-first Bit Reader (with JPEG byte-stuffing)
// ============================================================================

/// Reads bits from a JPEG entropy-coded segment, handling the 0xFF 0x00
/// byte-stuffing sequence and detecting embedded restart/end markers.
struct BitstreamReader {
    const u8* streamData = nullptr;
    size_t streamSize = 0;
    size_t readPosition = 0;

    u32 bitBuffer = 0; ///< Buffered bits, MSB-aligned (next bit to read is bit 31).
    i32 bitsAvailable = 0;
    u8 pendingMarker = 0; ///< Non-zero when a real marker (not byte-stuffing) was encountered.

    void reset(const u8* data, size_t size, size_t startOffset);

    /// Fill the bit buffer until it holds at least 25 bits.
    void refillBuffer();

    /// Peek at the next `count` bits without consuming them.
    u32 peekBits(i32 count);

    /// Discard `count` bits from the front of the buffer.
    void consumeBits(i32 count);

    /// Read and consume `count` bits.
    u32 readBits(i32 count);

    /// Reset the bit buffer and skip past a restart marker (0xFF 0xDn).
    void handleRestartMarker();
};

// ============================================================================
// Huffman Decode Table
// ============================================================================

/// Canonical Huffman table with a fast look-up table for short codes.
///
/// Build phase creates two acceleration structures:
///   1. A 2^HUFFMAN_FAST_BITS direct look-up table that resolves codes up
///      to HUFFMAN_FAST_BITS long in a single probe (covers ~95% of symbols).
///   2. maxcode[]/indexDelta[] arrays for a linear scan of longer codes.
struct HuffmanTable {
    std::array<u8, 1 << HUFFMAN_FAST_BITS> fastSymbol{}; ///< Symbol for each fast-table entry.
    std::array<u8, 1 << HUFFMAN_FAST_BITS>
        fastCodeLength{}; ///< Code length; 0 means use slow path.
    std::array<u32, 18>
        maxcode{}; ///< Exclusive upper bound for each code length (left-aligned to 16 bits).
    std::array<i32, 17>
        indexDelta{}; ///< Adjustment to convert a code value into a symbol table index.
    std::array<u8, 256> symbolTable{}; ///< All symbols in canonical order.
    bool isBuilt = false;

    /// Construct the Huffman table from JPEG-standard length counts and symbols.
    /// `codeLengthCounts[i]` is the number of symbols with code length (i+1).
    void build(const std::array<u8, 16>& codeLengthCounts, const u8* symbols);

    /// Decode one Huffman symbol from the bitstream.  Returns -1 on error.
    i32 decodeSymbol(BitstreamReader& reader) const;
};

// ============================================================================
// Huffman Encode Table
// ============================================================================

struct HuffmanCode {
    u16 code = 0;  ///< Left-aligned Huffman code word.
    u8 length = 0; ///< Number of bits in the code.
};

/// Pre-built code table for encoding: symbol → (code, length).
/// Supports symbols 0-255 for AC tables, 0-15 for DC tables.
struct HuffmanEncodeTable {
    std::array<HuffmanCode, 256> codes{};

    /// Build the code table from JPEG-standard length counts and symbols.
    void build(const u8* lengthCounts, const u8* symbols, i32 symbolCount);
};

// ============================================================================
// MSB-first Bit Writer (with JPEG byte-stuffing)
// ============================================================================

/// Writes bits to a JPEG entropy-coded segment, handling the 0xFF → 0xFF 0x00
/// byte-stuffing requirement.
struct BitstreamWriter {
    std::vector<u8>* output = nullptr;
    u32 bitBuffer = 0; ///< Buffered bits, MSB-aligned.
    i32 bitsUsed = 0;  ///< Number of valid bits in the buffer (counted from MSB).

    void init(std::vector<u8>* out);

    /// Write `count` bits (MSB-first) from the lower bits of `value`.
    void writeBits(u32 value, i32 count);

    /// Pad the remaining bits with 1-bits and flush (JPEG requires 1-bit padding).
    void flushWithPadding();
};

} // namespace jpeg
} // namespace textures
} // namespace whiteout

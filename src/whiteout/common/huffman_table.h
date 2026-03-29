// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file huffman_table.h
/// @brief Huffman codec primitives for both MSB-first (JPEG) and LSB-first (DEFLATE) codecs.
///
/// Provides:
///   - HuffmanDecodeBase<SymT, MaxBits> — common data for canonical Huffman decoders.
///   - MsbHuffmanTable — canonical Huffman decoder with a fast 9-bit LUT (MSB-first).
///   - LsbHuffmanTable — canonical Huffman decoder with a fast 9-bit LUT (LSB-first).
///   - HuffmanEncodeTable / HuffmanCode — symbol-to-code mapping for encoding.

#pragma once

#include <array>
#include <vector>

#include <whiteout/common_types.h>

#include "bit_io.h"

namespace whiteout {

// ============================================================================
// Constants
// ============================================================================

/// Width (in bits) of the fast Huffman look-up table. Codes up to this length
/// are resolved in a single table probe.
inline constexpr i32 HUFFMAN_FAST_BITS = 9;

// ============================================================================
// Common base for Huffman decode tables
// ============================================================================

/// Shared data layout for canonical Huffman decoders.
///
/// Both MSB-first (JPEG) and LSB-first (DEFLATE) decoders use the same
/// two-tier strategy: a 2^HUFFMAN_FAST_BITS direct look-up table for short
/// codes, plus maxcode[]/indexDelta[] arrays for a linear scan of longer codes.
///
/// @tparam SymT    Symbol type (u8 for JPEG, u16 for DEFLATE).
/// @tparam MaxBits Maximum Huffman code length (16 for JPEG, 15 for DEFLATE).
template <typename SymT, i32 MaxBits>
struct HuffmanDecodeBase {
    static constexpr i32 FAST_SIZE = 1 << HUFFMAN_FAST_BITS;
    static constexpr i32 MAX_BITS = MaxBits;

    /// Fast look-up table — resolves codes up to HUFFMAN_FAST_BITS long.
    std::array<SymT, FAST_SIZE> fastSymbol{};
    std::array<u8, FAST_SIZE> fastLen{};

    /// Slow-path tables for codes longer than HUFFMAN_FAST_BITS.
    std::array<u32, MaxBits + 2> maxcode{};
    std::array<i32, MaxBits + 1> indexDelta{};

    /// Symbol table, ordered by code-length then code-value.
    std::vector<SymT> symbols;

    /// Look up a symbol given a canonical code value and its length.
    /// Returns the symbol cast to i32, or -1 if out of bounds.
    i32 lookupSlow(u32 code, i32 codeLength) const {
        i32 idx = static_cast<i32>(code) + indexDelta[codeLength];
        if (idx >= 0 && idx < static_cast<i32>(symbols.size()))
            return static_cast<i32>(symbols[idx]);
        return -1;
    }
};

// ============================================================================
// MSB-first Huffman Decode Table (JPEG-style)
// ============================================================================

/// Canonical Huffman table with a fast look-up table for short codes (MSB-first).
struct MsbHuffmanTable : HuffmanDecodeBase<u8, 16> {
    bool isBuilt = false;

    /// Construct from JPEG-standard length counts and symbols.
    /// `codeLengthCounts[i]` = number of symbols with code length (i+1).
    void build(const std::array<u8, 16>& codeLengthCounts, const u8* syms);

    /// Decode one Huffman symbol from the bitstream. Returns -1 on error.
    i32 decodeSymbol(MsbBitReader& reader) const;
};

// ============================================================================
// LSB-first Huffman Decode Table (DEFLATE-style)
// ============================================================================

/// Huffman table for DEFLATE (LSB-first codes, bit-reversed fast LUT).
struct LsbHuffmanTable : HuffmanDecodeBase<u16, 15> {
    /// Build from an array of per-symbol code lengths.
    bool build(const u8* codeLengths, i32 count);

    /// Decode one symbol. Returns -1 on error.
    i32 decode(LsbBitReader& br) const;
};

// ============================================================================
// Huffman Encode Table (symbol → code mapping)
// ============================================================================

struct HuffmanCode {
    u16 code = 0;  ///< Left-aligned Huffman code word.
    u8 length = 0; ///< Number of bits in the code.
};

/// Pre-built code table for encoding: symbol → (code, length).
struct HuffmanEncodeTable {
    std::array<HuffmanCode, 256> codes{};

    /// Build the code table from JPEG-standard length counts and symbols.
    void build(const u8* lengthCounts, const u8* symbols, i32 symbolCount);
};

} // namespace whiteout

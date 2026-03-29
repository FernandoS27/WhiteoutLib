// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bit_reader.h
/// @brief Random-access bit-packed field extraction.
///
/// Internal header — not part of the public include path.
/// This is distinct from common/bit_io.h which provides sequential bit I/O
/// for Huffman/DEFLATE streams. This utility extracts arbitrary bit-aligned
/// fields from a byte buffer at known offsets — used by MPQ BET tables and
/// future CASC .IDX entries.

#pragma once

#include <cstddef>

#include <whiteout/common_types.h>

namespace whiteout::storages::common {

/// Extract `bitCount` bits starting at `bitOffset` from a byte buffer.
/// Supports widths up to 64 bits. Bits are read in little-endian order
/// (bit 0 of byte 0 is the least significant bit of the result).
///
/// @param data      Pointer to the byte buffer.
/// @param bitOffset Starting bit position (0-indexed from byte 0, bit 0).
/// @param bitCount  Number of bits to extract (0–64).
/// @return The extracted value, zero-extended to 64 bits.
inline u64 extractBits(const u8* data, size_t bitOffset, size_t bitCount) {
    if (bitCount == 0)
        return 0;

    u64 result = 0;
    size_t byteIdx = bitOffset / 8;
    size_t bitIdx = bitOffset % 8;

    // We need to read ceil((bitIdx + bitCount) / 8) bytes.
    size_t bitsRead = 0;
    size_t shift = 0;

    while (bitsRead < bitCount) {
        // How many bits can we take from the current byte?
        size_t availInByte = 8 - bitIdx;
        size_t toRead = bitCount - bitsRead;
        if (toRead > availInByte)
            toRead = availInByte;

        u64 mask = (1ULL << toRead) - 1;
        u64 bits = (static_cast<u64>(data[byteIdx]) >> bitIdx) & mask;
        result |= bits << shift;

        shift += toRead;
        bitsRead += toRead;
        bitIdx = 0;
        ++byteIdx;
    }

    return result;
}

} // namespace whiteout::storages::common

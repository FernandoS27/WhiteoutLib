// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file compression.h
/// @brief MPQ multi-algorithm compression dispatcher.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::mpq {

// ============================================================================
// Compression type flags (bitmask stored as first byte of compressed sector)
// ============================================================================

namespace CompressionFlag {
    static constexpr u8 kHuffman    = 0x01;
    static constexpr u8 kZlib       = 0x02;
    static constexpr u8 kPKware     = 0x08;
    static constexpr u8 kBZip2      = 0x10;
    static constexpr u8 kLZMA       = 0x12;
    static constexpr u8 kSparse     = 0x20;
    static constexpr u8 kAdpcmMono  = 0x40;
    static constexpr u8 kAdpcmStereo = 0x80;
} // namespace CompressionFlag

// ============================================================================
// Functions
// ============================================================================

/// Decompress a sector using the MPQ multi-algorithm pipeline.
/// The first byte of @p src is the compression mask; decompression is applied
/// innermost-first: BZip2 → PKware → zlib → Huffman → ADPCM-mono →
/// ADPCM-stereo → Sparse. LZMA (0x12) is handled as an exact-match special
/// case before the bitmask loop.
///
/// @param src              Compressed data (including the compression byte prefix).
/// @param uncompressedSize Expected decompressed size.
/// @param[out] error       Optional error message on failure.
/// @return Decompressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> mpqDecompress(
    std::span<const u8> src,
    size_t uncompressedSize,
    std::string* error = nullptr);

/// Compress data using a single MPQ compression algorithm.
/// The output includes the compression byte prefix.
///
/// @param src              Raw data to compress.
/// @param compressionType  One of the CompressionFlag constants (e.g. kZlib).
/// @param[out] error       Optional error message on failure.
/// @return Compressed data with compression byte prefix, or empty vector on failure.
[[nodiscard]] std::vector<u8> mpqCompress(
    std::span<const u8> src,
    u8 compressionType,
    std::string* error = nullptr);

} // namespace whiteout::storages::mpq

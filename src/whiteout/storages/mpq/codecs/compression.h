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

enum class CompressionFlag : u8 {
    None        = 0,
    kHuffman    = 0x01,
    kZlib       = 0x02,
    kPKware     = 0x08,
    kBZip2      = 0x10,
    kLZMA       = 0x12,
    kSparse     = 0x20,
    kAdpcmMono  = 0x40,
    kAdpcmStereo = 0x80,
};

inline CompressionFlag operator|(CompressionFlag a, CompressionFlag b) noexcept {
    return static_cast<CompressionFlag>(static_cast<u8>(a) | static_cast<u8>(b));
}
inline CompressionFlag operator&(CompressionFlag a, CompressionFlag b) noexcept {
    return static_cast<CompressionFlag>(static_cast<u8>(a) & static_cast<u8>(b));
}
inline CompressionFlag operator~(CompressionFlag a) noexcept {
    return static_cast<CompressionFlag>(~static_cast<u8>(a));
}
inline CompressionFlag& operator|=(CompressionFlag& a, CompressionFlag b) noexcept { a = a | b; return a; }
inline CompressionFlag& operator&=(CompressionFlag& a, CompressionFlag b) noexcept { a = a & b; return a; }
inline bool hasFlag(CompressionFlag flags, CompressionFlag flag) noexcept {
    return (flags & flag) != CompressionFlag::None;
}

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
[[nodiscard]] std::vector<u8> mpqDecompress(std::span<const u8> src, size_t uncompressedSize,
                                            std::string* error = nullptr);

/// Compress data using a single MPQ compression algorithm.
/// The output includes the compression byte prefix.
///
/// @param src              Raw data to compress.
/// @param compressionType  One of the CompressionFlag constants (e.g. kZlib).
/// @param[out] error       Optional error message on failure.
/// @return Compressed data with compression byte prefix, or empty vector on failure.
[[nodiscard]] std::vector<u8> mpqCompress(std::span<const u8> src, CompressionFlag compressionType,
                                          std::string* error = nullptr);

} // namespace whiteout::storages::mpq

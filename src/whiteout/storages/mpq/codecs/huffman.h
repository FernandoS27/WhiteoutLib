// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file huffman.h
/// @brief Blizzard adaptive Huffman decompression for MPQ sectors.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::mpq {

/// Decompress Huffman-encoded data using the Blizzard adaptive Huffman tree.
/// @param src          Compressed data (without the MPQ compression byte prefix).
/// @param expectedSize Expected decompressed size.
/// @return Decompressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> huffmanDecompress(std::span<const u8> src, size_t expectedSize);

/// huffmanDecompress() into a buffer the caller owns: decoding stops where
/// @p dst ends.
/// @return The bytes written; 0 on failure.
[[nodiscard]] size_t huffmanDecompressInto(std::span<const u8> src, std::span<u8> dst);

/// Compress data using the Blizzard adaptive Huffman algorithm.
/// @param src          Raw data to compress.
/// @param compType     Compression type (0-8), selects the initial weight table.
/// @return Compressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> huffmanCompress(std::span<const u8> src, u32 compType = 0);

} // namespace whiteout::storages::mpq

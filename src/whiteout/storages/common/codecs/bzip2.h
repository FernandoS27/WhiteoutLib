// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bzip2.h
/// @brief Minimal BZip2 decompression/compression.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::common {

/// Decompress BZip2-compressed data.
/// @param src          Compressed data (without the MPQ compression byte prefix).
/// @param expectedSize Expected decompressed size.
/// @return Decompressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> bzip2Decompress(std::span<const u8> src, size_t expectedSize);

/// bzip2Decompress() into a buffer the caller owns: decoding stops where
/// @p dst ends.
/// @return The bytes written; 0 on failure.
[[nodiscard]] size_t bzip2DecompressInto(std::span<const u8> src, std::span<u8> dst);

/// Compress data using BZip2.
/// @param src Raw data to compress.
/// @return Compressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> bzip2Compress(std::span<const u8> src);

} // namespace whiteout::storages::common

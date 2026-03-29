// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file sparse.h
/// @brief Sparse (zero-run RLE) decompression for MPQ sectors.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::mpq {

/// Decompress sparse-encoded data (zero-run RLE).
/// @param src          Compressed data (without the MPQ compression byte prefix).
/// @param expectedSize Expected decompressed size.
/// @return Decompressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> sparseDecompress(std::span<const u8> src, size_t expectedSize);

/// Compress data using sparse (zero-run RLE) encoding.
/// @param src Raw data to compress.
/// @return Compressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> sparseCompress(std::span<const u8> src);

} // namespace whiteout::storages::mpq

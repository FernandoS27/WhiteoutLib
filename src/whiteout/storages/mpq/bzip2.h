// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bzip2.h
/// @brief Minimal BZip2 decompression for MPQ sectors.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::mpq {

/// Decompress BZip2-compressed data.
/// @param src          Compressed data (without the MPQ compression byte prefix).
/// @param expectedSize Expected decompressed size.
/// @return Decompressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> bzip2Decompress(std::span<const u8> src, size_t expectedSize);

/// Compress data using BZip2.
/// @param src Raw data to compress.
/// @return Compressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> bzip2Compress(std::span<const u8> src);

} // namespace whiteout::storages::mpq

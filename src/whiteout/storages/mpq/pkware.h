// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pkware.h
/// @brief PKware Data Compression Library (DCL) explode/implode for MPQ.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::mpq {

/// Decompress data compressed with PKware DCL (implode).
/// @param src              Compressed data (without the MPQ compression byte prefix).
/// @param expectedSize     Expected decompressed size.
/// @return Decompressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> pkwareExplode(std::span<const u8> src, size_t expectedSize);

/// Compress data using PKware DCL (implode).
/// @param src Raw data to compress.
/// @return Compressed data (without the MPQ compression byte prefix), or empty vector on failure.
[[nodiscard]] std::vector<u8> pkwareImplode(std::span<const u8> src);

} // namespace whiteout::storages::mpq

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file lzma.h
/// @brief LZMA decompression (decode-only).
///
/// MPQ LZMA wire format: 5-byte properties header (lclppb packed byte +
/// 4-byte little-endian dictionary size) followed by the raw LZMA range-coded
/// stream. No uncompressed-size field in the stream — the caller supplies the
/// expected output size.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::common {

/// Decompress LZMA-compressed data.
/// @param src          Compressed data (without the MPQ compression byte prefix).
/// @param expectedSize Expected decompressed size.
/// @return Decompressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> lzmaDecompress(std::span<const u8> src, size_t expectedSize);

} // namespace whiteout::storages::common

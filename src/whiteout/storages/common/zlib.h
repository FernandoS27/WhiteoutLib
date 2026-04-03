// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file zlib.h
/// @brief Thin zlib compress/decompress wrappers over the internal DEFLATE codec.
///
/// Internal header — not part of the public include path.

#pragma once

#include <span>
#include <vector>

#include <whiteout/common_types.h>

#include "../../common/deflate.h"

namespace whiteout::storages::common {

/// Decompress a zlib-wrapped DEFLATE stream.
/// @param src           Compressed data (zlib header + DEFLATE + Adler-32).
/// @param expectedSize  Hint for output reservation (0 = no hint).
/// @return Decompressed bytes, or empty vector on failure.
inline std::vector<u8> zlibDecompress(std::span<const u8> src, size_t expectedSize = 0) {
    return ::whiteout::zlib_decompress(src, nullptr, expectedSize);
}

/// Compress data into a zlib-wrapped DEFLATE stream.
/// @param src  Raw data to compress.
/// @return Compressed bytes (zlib header + DEFLATE + Adler-32), or empty on failure.
inline std::vector<u8> zlibCompress(std::span<const u8> src) {
    return ::whiteout::zlib_compress(src);
}

} // namespace whiteout::storages::common

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file deflate.h
/// @brief zlib-wrapped DEFLATE inflate/deflate (built in, or zlib-ng).
///
/// Internal header — not part of the public include path.

#pragma once

#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {

/// Decompress a zlib-wrapped DEFLATE stream.
/// @param expectedSize Hint for output buffer reservation (0 = use default heuristic).
/// @return decompressed bytes, or empty vector on failure.
std::vector<u8> zlib_decompress(std::span<const u8> data, std::string* out_error = nullptr,
                                size_t expectedSize = 0);

/// Compress data into a zlib-wrapped DEFLATE stream, at zlib's default
/// level (6).
/// @return compressed bytes, or empty vector on failure.
std::vector<u8> zlib_compress(std::span<const u8> data, std::string* out_error = nullptr);

/// zlib_compress() appended to @p out, which is left as it was on failure.
/// What the compressor works with is kept per thread, so a caller that
/// compresses many small pieces pays for it once.
bool zlib_compress_append(std::vector<u8>& out, std::span<const u8> data, std::string* out_error = nullptr);

} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file deflate.h
/// @brief Minimal zlib-wrapped DEFLATE inflate/deflate.
///
/// Internal header — not part of the public include path.

#pragma once

#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {

/// Decompress a zlib-wrapped DEFLATE stream.
/// @return decompressed bytes, or empty vector on failure.
std::vector<u8> zlib_decompress(std::span<const u8> data, std::string* out_error = nullptr);

/// Compress data into a zlib-wrapped DEFLATE stream.
/// Uses fixed Huffman codes with LZ77 matching for a reasonable size/speed trade-off.
/// @return compressed bytes, or empty vector on failure.
std::vector<u8> zlib_compress(std::span<const u8> data, std::string* out_error = nullptr);

} // namespace whiteout

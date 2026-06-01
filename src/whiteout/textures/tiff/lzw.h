// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file lzw.h
/// @brief TIFF-flavour LZW (compression 5) decoder.
///
/// Internal header — not part of the public include path.
///
/// TIFF 6.0 LZW differs from canonical LZW in two important ways:
///   1. Bits are packed MSB-first (canonical LZW is typically LSB-first).
///   2. **Early-change**: code width bumps from W to W+1 when the next code
///      that will be assigned equals `(1 << W) - 1`, not `(1 << W)` as in
///      canonical LZW. This off-by-one is the #1 source of TIFF LZW bugs
///      and is tested by name in tiff_lzw_test.cpp.
///
/// Special codes:
///   256 = CLEAR  — reset string table, return to 9-bit codes
///   257 = EOI    — end of information

#pragma once

#include <span>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::textures::tiff {

/// Decode a TIFF LZW-compressed strip.
/// @param src           Compressed bytes (no header — just the LZW stream).
/// @param expectedSize  Hint for output buffer reservation (0 = no hint).
/// @return Decompressed bytes; empty vector on malformed input.
[[nodiscard]] std::vector<u8> lzwDecompress(std::span<const u8> src,
                                            size_t expectedSize = 0);

} // namespace whiteout::textures::tiff

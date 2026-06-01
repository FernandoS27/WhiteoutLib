// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file packbits.h
/// @brief Apple PackBits (TIFF compression 32773) decompressor.
///
/// Internal header — not part of the public include path.
///
/// Header byte n:
///   0..127     copy next n+1 bytes literally
///   128        no-op (skip header)
///   129..255   repeat next byte (257 - n) times, i.e. (-n + 1) in two's
///              complement reading

#pragma once

#include <span>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::textures::tiff {

/// Decompress a PackBits stream into a fresh buffer.
/// @return decompressed bytes, or empty vector on malformed input
///         (truncated literal/run, overshoot of @p expectedSize when nonzero).
[[nodiscard]] std::vector<u8> packBitsDecompress(std::span<const u8> src,
                                                 size_t expectedSize = 0);

} // namespace whiteout::textures::tiff

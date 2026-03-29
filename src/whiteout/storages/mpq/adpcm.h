// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file adpcm.h
/// @brief ADPCM decompression for MPQ audio sectors.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::mpq {

/// Decompress ADPCM-encoded audio data.
/// @param src          Compressed data (without the MPQ compression byte prefix).
/// @param expectedSize Expected decompressed size in bytes.
/// @param channelCount 1 for mono, 2 for stereo.
/// @return Decompressed 16-bit PCM data (little-endian), or empty vector on failure.
[[nodiscard]] std::vector<u8> adpcmDecompress(std::span<const u8> src, size_t expectedSize,
                                              int channelCount);

/// Compress 16-bit PCM audio data using ADPCM.
/// @param src          Uncompressed 16-bit PCM data (little-endian).
/// @param channelCount 1 for mono, 2 for stereo.
/// @return ADPCM-compressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> adpcmCompress(std::span<const u8> src, int channelCount);

} // namespace whiteout::storages::mpq

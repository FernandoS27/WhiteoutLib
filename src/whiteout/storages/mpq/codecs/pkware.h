// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pkware.h
/// @brief PKware Data Compression Library (DCL) explode/implode for MPQ.

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::mpq {

/// Decompress data compressed with PKware DCL (implode), binary or ASCII mode.
/// @param src              Compressed data (without the MPQ compression byte prefix).
/// @param expectedSize     Expected decompressed size; decoding stops there.
/// @return Decompressed data — shorter when the stream ends first — or an
///         empty vector when the stream is malformed.
[[nodiscard]] std::vector<u8> pkwareExplode(std::span<const u8> src, size_t expectedSize);

/// pkwareExplode() into a buffer the caller owns: decoding stops where
/// @p dst ends, and nothing is written past it.
/// @return The bytes written (fewer than dst.size() when the stream ends
///         first); 0 when the stream is malformed.
[[nodiscard]] size_t pkwareExplodeInto(std::span<const u8> src, std::span<u8> dst);

/// Compress data using PKware DCL (implode), binary mode.
/// @param src Raw data to compress.
/// @return Compressed data (without the MPQ compression byte prefix), or empty vector on failure.
[[nodiscard]] std::vector<u8> pkwareImplode(std::span<const u8> src);

} // namespace whiteout::storages::mpq

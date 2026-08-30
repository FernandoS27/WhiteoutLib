// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file inflate_fast.h
/// @brief Fast zlib-inflate for CASC BLTE frames (zlib-ng-backed).
///
/// Decompresses a zlib-wrapped DEFLATE stream. When @p expectedSize is known
/// (always for BLTE), the output is sized exactly and decompression is a single
/// zlib-ng pass. The implementation lives in common/deflate.cpp — the one TU
/// that includes <zlib-ng.h> — so consumers need neither the zlib-ng header nor
/// a direct link to it.
///
/// Internal header — not part of the public include path.

#pragma once

#include <optional>
#include <span>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::storages::common {

/// Fast zlib-inflate for CASC BLTE frames.
///
/// @param src           zlib-wrapped DEFLATE stream (RFC 1950).
/// @param expectedSize  Expected decompressed size (0 = unknown → growable path).
/// @return Decompressed bytes, or empty vector on failure.
std::vector<u8> zlibInflateFast(std::span<const u8> src, size_t expectedSize = 0);

/// As above, but writing into @p dst instead of allocating. BLTE blobs are
/// decoded frame by frame into one output buffer, so this lets the caller size
/// that buffer once and skip a per-frame allocation and copy.
///
/// @param dst  Destination, at least as large as the decompressed stream.
/// @param src  zlib-wrapped DEFLATE stream (RFC 1950).
/// @return Bytes written, or std::nullopt on failure.
std::optional<size_t> zlibInflateFastInto(std::span<u8> dst, std::span<const u8> src);

} // namespace whiteout::storages::common

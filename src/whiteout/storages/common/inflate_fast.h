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

} // namespace whiteout::storages::common

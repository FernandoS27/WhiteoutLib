// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file lzma.h
/// @brief LZMA decompression (decode-only).
///
/// Takes the 5 property bytes (the lc/lp/pb byte, then the little-endian
/// dictionary size) and the range-coded stream; the caller supplies the
/// output size. MPQ wraps these in its own header (see compression.cpp).

#pragma once

#include <whiteout/common_types.h>

#include <span>
#include <vector>

namespace whiteout::storages::common {

/// Decompress an LZMA stream.
/// @param props        The 5 property bytes.
/// @param stream       The range-coded data.
/// @param expectedSize Expected decompressed size.
/// @return Decompressed data, or empty vector on failure.
[[nodiscard]] std::vector<u8> lzmaDecompress(std::span<const u8> props, std::span<const u8> stream,
                                             size_t expectedSize);

/// The same, for property bytes followed directly by the stream.
[[nodiscard]] std::vector<u8> lzmaDecompress(std::span<const u8> src, size_t expectedSize);

} // namespace whiteout::storages::common

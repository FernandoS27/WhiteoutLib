// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Internal baseline JPEG decoder with raw component output (no colourspace
/// conversion).  Designed for BLP file parsing where JPEG components encode
/// BGRA rather than the standard Y'CbCr.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::textures::jpeg {

/// Decoded image returned by the JPEG decoder.
struct Image {
    u32 width = 0;
    u32 height = 0;
    u32 components = 0;     ///< Channel count (1-4).
    std::vector<u8> pixels; ///< width * height * components bytes, interleaved.
};

/// Decode a baseline JPEG (SOF0) image WITHOUT applying colourspace conversion.
/// Component sample values are returned exactly as stored in the DCT data.
/// Progressive, arithmetic-coded, and lossless JPEG variants are not supported.
std::optional<Image> decode_raw(std::span<const u8> data, std::string* out_error = nullptr);

} // namespace whiteout::textures::jpeg

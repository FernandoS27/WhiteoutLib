// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Internal JPEG decoder with raw component output (no colourspace conversion).
/// Supports both baseline sequential DCT (SOF0) and progressive DCT (SOF2).
/// Designed for BLP file parsing where JPEG components encode BGRA rather than
/// the standard Y'CbCr.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::textures::jpeg {

struct JpegContext;

/// Decoded image returned by the JPEG decoder.
struct Image {
    u32 width = 0;
    u32 height = 0;
    u32 components = 0;     ///< Channel count (1-4).
    std::vector<u8> pixels; ///< width * height * components bytes, interleaved.
};

/// Decode a JPEG (SOF0 baseline or SOF2 progressive) image WITHOUT applying
/// colourspace conversion.  Component sample values are returned exactly as
/// stored in the DCT data.
/// Arithmetic-coded and lossless JPEG variants are not supported.
///
/// @param ctx          Optional parallel context.  When non-null, the IDCT and
///                     pixel assembly phases are parallelised via the caller's
///                     DAG.  Marker parsing and entropy decode always run
///                     synchronously (they block inside this call).
/// @param asyncOutput  When ctx is non-null, the decoded image will be written
///                     here by the last DAG task.  Must remain valid until the
///                     caller waits on the timeline semaphore.  Ignored when
///                     ctx is null.
/// @return  The decoded image when ctx is null, or std::nullopt on error.
///          When ctx is non-null and parsing succeeds, returns std::nullopt
///          (the result will arrive in *asyncOutput after DAG completion).
std::optional<Image> decode_raw(std::span<const u8> data, std::string* out_error = nullptr,
                                JpegContext* ctx = nullptr, Image* asyncOutput = nullptr);

} // namespace whiteout::textures::jpeg

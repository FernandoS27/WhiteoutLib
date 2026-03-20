// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// JPEG encoder with raw component output (no colourspace conversion).
/// Supports both baseline (SOF0) and progressive (SOF2) modes.
/// Designed for BLP file writing where JPEG components encode BGRA rather
/// than the standard Y'CbCr.

#pragma once

#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "jpeg_decode.h" // For jpeg::Image

namespace whiteout::textures::jpeg {

struct JpegContext;

/// Encode an image as a JPEG WITHOUT applying colourspace conversion.
/// Component sample values are stored directly into the DCT data, matching
/// the behaviour of decode_raw().
///
/// @param image        Input image (1-4 interleaved components, 8-bit per sample).
/// @param quality      JPEG quality factor in the range [1, 100].  Higher values
///                     produce larger files with less quantisation loss.
/// @param out_error    Optional pointer to receive an error description on failure.
/// @param progressive  If true, encode as progressive JPEG (SOF2) with one
///                     interleaved DC scan + one AC scan per component.
///                     If false (default), encode as baseline (SOF0).
/// @param ctx          Optional parallel context.  When non-null, parallel stages
///                     are appended to the caller's DAG.  When null, runs serial.
/// @param asyncOutput  When ctx is non-null, the encoded bytes will be written
///                     here by the last DAG task.  Must remain valid until the
///                     caller waits on the timeline semaphore.  Ignored when
///                     ctx is null.
/// @return  The encoded JPEG byte stream when ctx is null, or an empty vector
///          when ctx is non-null (result is in *asyncOutput after DAG completes).
std::vector<u8> encode_raw(const Image& image, i32 quality = 75, std::string* out_error = nullptr,
                           bool progressive = false, JpegContext* ctx = nullptr,
                           std::vector<u8>* asyncOutput = nullptr);

} // namespace whiteout::textures::jpeg

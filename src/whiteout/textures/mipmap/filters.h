// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file filters.h
/// @brief Downsample filter kernels for mipmap generation.
///
/// Each filter has two overloads:
///   - (src)                       — halves dimensions (convenience).
///   - (src, dstWidth, dstHeight)  — downsamples to an arbitrary target size.
/// The arbitrary-size form is used by the generator so that every mip level
/// can be produced directly from the original full-resolution image.

#pragma once

#include "mip_image.h"

namespace whiteout::textures::mipmap {

/// Downsample to the given size using a box (area-average) filter.
MipImage boxFilter(const MipImage& src, u32 dstWidth, u32 dstHeight);
/// Convenience: halve dimensions.
MipImage boxFilter(const MipImage& src);

/// Downsample to the given size using a Lanczos3-windowed sinc filter (separable).
MipImage lanczos3Filter(const MipImage& src, u32 dstWidth, u32 dstHeight);
/// Convenience: halve dimensions.
MipImage lanczos3Filter(const MipImage& src);

/// Downsample to the given size using a Kaiser-windowed sinc filter (default β = 4.0).
MipImage kaiserFilter(const MipImage& src, u32 dstWidth, u32 dstHeight);
/// Convenience: halve dimensions.
MipImage kaiserFilter(const MipImage& src);

/// Downsample to the given size using a Kaiser-windowed sinc filter with configurable β.
MipImage kaiserFilter(const MipImage& src, u32 dstWidth, u32 dstHeight, f64 beta);
/// Convenience: halve dimensions.
MipImage kaiserFilter(const MipImage& src, f64 beta);

} // namespace whiteout::textures::mipmap

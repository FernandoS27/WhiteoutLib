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
///
/// All filters accept an optional PipelineContext* to parallelize output-row
/// computation across threads.  When ctx is nullptr, filters run
/// single-threaded.

#pragma once

#include "mip_image.h"

namespace whiteout::textures::mipmap {

struct PipelineContext;

// Main overloads: write into a pre-allocated @p dst.
void boxFilter(const MipImage& src, MipImage& dst, PipelineContext* ctx = nullptr);
void lanczos3Filter(const MipImage& src, MipImage& dst, PipelineContext* ctx = nullptr);
void kaiserFilter(const MipImage& src, MipImage& dst, PipelineContext* ctx = nullptr);
void kaiserFilter(const MipImage& src, MipImage& dst, f64 beta, PipelineContext* ctx = nullptr);
void environmentPrefilterGGX(const MipImage& src, MipImage& dst, PipelineContext* ctx = nullptr);
void sphericalKaiserFilter(const MipImage& src, MipImage& dst, PipelineContext* ctx = nullptr);
void maxPoolFilter(const MipImage& src, MipImage& dst, PipelineContext* ctx = nullptr);

// Convenience overloads: half-size, single-threaded.
MipImage boxFilter(const MipImage& src);
MipImage lanczos3Filter(const MipImage& src);
MipImage kaiserFilter(const MipImage& src);
MipImage kaiserFilter(const MipImage& src, f64 beta);

} // namespace whiteout::textures::mipmap

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
/// All filters accept an optional WorkerPool* to parallelize output-row
/// computation across threads.  When pool is nullptr, filters run
/// single-threaded.

#pragma once

#include "mip_image.h"

#include <whiteout/interfaces.h>

namespace whiteout::textures::mipmap {

MipImage boxFilter(const MipImage& src, u32 dstWidth, u32 dstHeight,
                   interfaces::WorkerPool* pool = nullptr);
MipImage boxFilter(const MipImage& src);

MipImage lanczos3Filter(const MipImage& src, u32 dstWidth, u32 dstHeight,
                        interfaces::WorkerPool* pool = nullptr);
MipImage lanczos3Filter(const MipImage& src);

MipImage kaiserFilter(const MipImage& src, u32 dstWidth, u32 dstHeight,
                      interfaces::WorkerPool* pool = nullptr);
MipImage kaiserFilter(const MipImage& src);

MipImage kaiserFilter(const MipImage& src, u32 dstWidth, u32 dstHeight, f64 beta,
                      interfaces::WorkerPool* pool = nullptr);
MipImage kaiserFilter(const MipImage& src, f64 beta);

MipImage environmentPrefilterGGX(const MipImage& src, u32 dstWidth, u32 dstHeight,
                                 interfaces::WorkerPool* pool = nullptr);

MipImage sphericalKaiserFilter(const MipImage& src, u32 dstWidth, u32 dstHeight,
                               interfaces::WorkerPool* pool = nullptr);

} // namespace whiteout::textures::mipmap

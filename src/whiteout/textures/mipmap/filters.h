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

/// Downsample an equirectangular environment map using GGX importance-sampled
/// convolution (split-sum prefilter, roughness derived from the downsample ratio).
///
/// The output texel at (u, v) accumulates contributions from `N` sample
/// directions drawn with the GGX NDF using a Hammersley low-discrepancy
/// sequence.  The roughness increases with each mip level and is derived
/// from the input/output size ratio so that the pipeline can always sample
/// directly from the original image.
///
/// Assumptions:
/// - The input image is an equirectangular (lat-long) panorama.
/// - The image stores data in linear light (sRGB decode should be done first).
/// - V == N (isotropic reflection; standard split-sum approximation).
MipImage environmentPrefilterGGX(const MipImage& src, u32 dstWidth, u32 dstHeight);

/// Downsample an equirectangular environment map using a solid-angle-weighted
/// spherical Kaiser-windowed sinc filter (β = 6, 3-lobe support).
///
/// Unlike the separable pixel-space Kaiser filter, each output texel accumulates
/// source samples weighted by:
///   - a Kaiser window applied over the great-circle angular distance, and
///   - sin(θ) (the solid-angle differential of the equirectangular projection).
///
/// U wraps horizontally; V clamps at the poles.  A bilinear fallback is used
/// for degenerate output texels (zero weight sum).
MipImage sphericalKaiserFilter(const MipImage& src, u32 dstWidth, u32 dstHeight);

} // namespace whiteout::textures::mipmap

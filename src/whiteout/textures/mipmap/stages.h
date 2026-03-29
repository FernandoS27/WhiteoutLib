// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file stages.h
/// @brief Pre- and post-processing stages for the mipmap pipeline.
///
/// Each stage is a free function that transforms a MipImage in-place.
/// Stages are assembled into a pipeline by the generator.
///
/// Every stage has a context-aware overload that accepts a PipelineContext*
/// to parallelize the per-pixel work across row-range tiles.  When ctx is
/// nullptr, the stage runs single-threaded.

#pragma once

#include "mip_image.h"

namespace whiteout::textures::mipmap {

struct PipelineContext;

// ── Gamma / colour-space stages ────────────────────────────────────────

void linearize(MipImage& img, PipelineContext* ctx = nullptr);
void delinearize(MipImage& img, PipelineContext* ctx = nullptr);

// ── Normal-map stages ──────────────────────────────────────────────────

void unpackNormals(MipImage& img, PipelineContext* ctx = nullptr);
void packNormals(MipImage& img, PipelineContext* ctx = nullptr);
void renormalize(MipImage& img, PipelineContext* ctx = nullptr);
void toksvigCorrection(MipImage& img, PipelineContext* ctx = nullptr);

// ── Roughness / gloss stages ───────────────────────────────────────────

void squareRoughness(MipImage& img, PipelineContext* ctx = nullptr);
void unsquareRoughness(MipImage& img, PipelineContext* ctx = nullptr);
void glossToRoughness(MipImage& img, PipelineContext* ctx = nullptr);
void roughnessToGloss(MipImage& img, PipelineContext* ctx = nullptr);

// ── Lightmap / alpha-mask stages ───────────────────────────────────────

void clampPositive(MipImage& img, PipelineContext* ctx = nullptr);

/// Apply a separable Gaussian blur to the alpha channel and capture
/// coverage.  Returns the pre-blur alpha coverage value that must be
/// passed to preserveAlphaCoverage().
f32 preBlurAlpha(MipImage& img, PipelineContext* ctx = nullptr);

/// Rescale the alpha channel so that coverage matches @p coverageTarget
/// (typically the value returned by preBlurAlpha).
void preserveAlphaCoverage(MipImage& img, f32 coverageTarget, PipelineContext* ctx = nullptr);

/// Compute the fraction of texels whose alpha (last channel) × @p scale
/// meets or exceeds the 0.5 threshold.
f32 computeAlphaCoverage(const MipImage& img, f32 scale = 1.0f);

// ── Mask stages ────────────────────────────────────────────────────────

/// 3×3 median filter applied to all channels independently.  Removes
/// salt-and-pepper noise without introducing fractional values on binary
/// data.  Uses a temporary buffer and two timeline-chained passes.
void medianFilter3x3(MipImage& img, PipelineContext* ctx = nullptr);

/// Edge-preserving bilateral filter (5×5 window) on all channels.
/// Smooths uniform regions while preserving sharp transitions.
void bilateralFilter(MipImage& img, PipelineContext* ctx = nullptr);

/// Snap every channel value to 0 or 1 using a 0.5 threshold.
void clampBinary(MipImage& img, PipelineContext* ctx = nullptr);

/// Clamp every channel value to [0, 1].
void clampUnit(MipImage& img, PipelineContext* ctx = nullptr);

} // namespace whiteout::textures::mipmap

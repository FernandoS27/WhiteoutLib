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
void preserveAlphaCoverage(MipImage& img, f32 coverageTarget,
                           PipelineContext* ctx = nullptr);

} // namespace whiteout::textures::mipmap

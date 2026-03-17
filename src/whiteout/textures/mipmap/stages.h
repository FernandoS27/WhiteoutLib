// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file stages.h
/// @brief Pre- and post-processing stages for the mipmap pipeline.
///
/// Each stage is a free function that transforms a MipImage in-place.
/// Stages are assembled into a pipeline by the generator.
///
/// Every stage has a pool-aware overload that accepts a WorkerPool* to
/// parallelize the per-pixel work across row-range tiles.  When pool is
/// nullptr, the stage runs single-threaded.

#pragma once

#include "mip_image.h"

#include <whiteout/interfaces.h>

namespace whiteout::textures::mipmap {

// ── Gamma / colour-space stages ────────────────────────────────────────

void linearize(MipImage& img, interfaces::WorkerPool* pool = nullptr);
void delinearize(MipImage& img, interfaces::WorkerPool* pool = nullptr);

// ── Normal-map stages ──────────────────────────────────────────────────

void unpackNormals(MipImage& img, interfaces::WorkerPool* pool = nullptr);
void packNormals(MipImage& img, interfaces::WorkerPool* pool = nullptr);
void renormalize(MipImage& img, interfaces::WorkerPool* pool = nullptr);
void toksvigCorrection(MipImage& img, interfaces::WorkerPool* pool = nullptr);

// ── Roughness / gloss stages ───────────────────────────────────────────

void squareRoughness(MipImage& img, interfaces::WorkerPool* pool = nullptr);
void unsquareRoughness(MipImage& img, interfaces::WorkerPool* pool = nullptr);
void glossToRoughness(MipImage& img, interfaces::WorkerPool* pool = nullptr);
void roughnessToGloss(MipImage& img, interfaces::WorkerPool* pool = nullptr);

// ── Lightmap / alpha-mask stages ───────────────────────────────────────

void clampPositive(MipImage& img, interfaces::WorkerPool* pool = nullptr);

/// Apply a separable Gaussian blur to the alpha channel and capture
/// coverage.  Returns the pre-blur alpha coverage value that must be
/// passed to preserveAlphaCoverage().
f32 preBlurAlpha(MipImage& img, interfaces::WorkerPool* pool = nullptr);

/// Rescale the alpha channel so that coverage matches @p coverageTarget
/// (typically the value returned by preBlurAlpha).
void preserveAlphaCoverage(MipImage& img, f32 coverageTarget,
                           interfaces::WorkerPool* pool = nullptr);

} // namespace whiteout::textures::mipmap

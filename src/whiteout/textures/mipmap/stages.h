// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file stages.h
/// @brief Pre- and post-processing stages for the mipmap pipeline.
///
/// Each stage is a free function that transforms a MipImage in-place.
/// Stages are assembled into a pipeline by the generator.

#pragma once

#include "mip_image.h"

namespace whiteout::textures::mipmap {

// ── Gamma / colour-space stages ────────────────────────────────────────

/// Convert colour channels from sRGB gamma to linear (in-place).
/// Operates on channels 0..min(channels,3)-1; alpha (ch 3) is unchanged.
void linearize(MipImage& img);

/// Convert colour channels from linear to sRGB gamma (in-place).
void delinearize(MipImage& img);

// ── Normal-map stages ──────────────────────────────────────────────────

/// Unpack normal-map encoding: remap channels from [0,1] to [-1,1].
void unpackNormals(MipImage& img);

/// Pack normal-map encoding: remap channels from [-1,1] back to [0,1].
void packNormals(MipImage& img);

/// Re-normalise 2-D or 3-D normal vectors (in-place).
/// 2-channel images: normalises (R, G) as an XY normal.
/// 3+ channel images: normalises (R, G, B) as an XYZ normal.
void renormalize(MipImage& img);

/// Compute Toksvig specular anti-aliasing factor from filtered normals.
/// Must be called after the downsample filter and before renormalize().
/// Stores the factor (filtered normal length) in the alpha channel.
/// No-op for images with fewer than 4 channels.
void toksvigCorrection(MipImage& img);

// ── Roughness / gloss stages ───────────────────────────────────────────

/// Square all channel values: r → r² (variance-preserving pre-filter stage).
void squareRoughness(MipImage& img);

/// Reconstruct roughness from squared values: r² → √r (post-filter stage).
void unsquareRoughness(MipImage& img);

/// Convert gloss to roughness: g → (1 − g).
void glossToRoughness(MipImage& img);

/// Convert roughness back to gloss: r → (1 − r).
void roughnessToGloss(MipImage& img);

// ── Alpha-mask stages ──────────────────────────────────────────────────

/// Clamp the R, G, B channels to [0, ∞) — eliminates ringing artefacts
/// introduced by the Lanczos3 filter in HDR lightmap data.
/// Alpha (channel 3, if present) is left unchanged.
void clampPositive(MipImage& img);

/// Apply a separable Gaussian blur (σ ≈ 0.5, 3-tap [¼ ½ ¼]) to the alpha
/// channel only.  Intended as a pre-process stage for AlphaMask textures.
/// Also records the pre-blur alpha coverage so that preserveAlphaCoverage
/// can restore it after downsampling.
void preBlurAlpha(MipImage& img);

/// Rescale the alpha channel so that coverage (fraction of texels with
/// alpha ≥ 0.5) matches the value captured by the preceding preBlurAlpha
/// call.  Must be used as a post-process stage in the same pipeline execution.
void preserveAlphaCoverage(MipImage& img);

} // namespace whiteout::textures::mipmap

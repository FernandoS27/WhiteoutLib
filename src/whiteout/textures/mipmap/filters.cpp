// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file filters.cpp
/// @brief Box and Kaiser downsample filter implementations.

#include "filters.h"

#include <algorithm>
#include <cmath>

namespace whiteout::textures::mipmap {

// ============================================================================
// Box filter
// ============================================================================

MipImage boxFilter(const MipImage& src, u32 dstWidth, u32 dstHeight) {
    const u32 numChannels = src.channels;
    MipImage dst(dstWidth, dstHeight, numChannels);

    // For each destination pixel, average the source pixels that map to it.
    for (u32 dstY = 0; dstY < dstHeight; ++dstY) {
        // Source row span covered by this destination row.
        const f32 srcYStart = static_cast<f32>(dstY) * src.height / dstHeight;
        const f32 srcYEnd = static_cast<f32>(dstY + 1) * src.height / dstHeight;
        const u32 srcRowMin = static_cast<u32>(srcYStart);
        const u32 srcRowMax = std::min(static_cast<u32>(std::ceil(srcYEnd)), src.height) - 1;

        for (u32 dstX = 0; dstX < dstWidth; ++dstX) {
            const f32 srcXStart = static_cast<f32>(dstX) * src.width / dstWidth;
            const f32 srcXEnd = static_cast<f32>(dstX + 1) * src.width / dstWidth;
            const u32 srcColMin = static_cast<u32>(srcXStart);
            const u32 srcColMax = std::min(static_cast<u32>(std::ceil(srcXEnd)), src.width) - 1;

            f32* outPixel = dst.pixel(dstX, dstY);
            for (u32 channel = 0; channel < numChannels; ++channel)
                outPixel[channel] = 0.0f;

            u32 sampleCount = 0;
            for (u32 srcY = srcRowMin; srcY <= srcRowMax; ++srcY) {
                for (u32 srcX = srcColMin; srcX <= srcColMax; ++srcX) {
                    const f32* srcPixel = src.pixel(srcX, srcY);
                    for (u32 channel = 0; channel < numChannels; ++channel)
                        outPixel[channel] += srcPixel[channel];
                    ++sampleCount;
                }
            }
            if (sampleCount > 0) {
                const f32 invCount = 1.0f / static_cast<f32>(sampleCount);
                for (u32 channel = 0; channel < numChannels; ++channel)
                    outPixel[channel] *= invCount;
            }
        }
    }
    return dst;
}

MipImage boxFilter(const MipImage& src) {
    return boxFilter(src, std::max(src.width / 2, 1u), std::max(src.height / 2, 1u));
}

// ============================================================================
// Shared separable filter infrastructure
// ============================================================================

namespace {

constexpr f64 PI = 3.14159265358979323846;
constexpr i32 BESSEL_MAX_TERMS = 20;
constexpr f64 BESSEL_CONVERGENCE = 1e-15;
constexpr f64 SINC_EPSILON = 1e-12;
constexpr i32 LANCZOS_LOBES = 3;
constexpr f64 HALF_PIXEL = 0.5;

/// Modified Bessel function I0 via power-series (Abramowitz & Stegun 9.6.10).
f64 bessel_I0(f64 x) {
    f64 sum = 1.0;
    f64 term = 1.0;
    const f64 x2_over_4 = (x * x) * 0.25;
    for (int k = 1; k <= BESSEL_MAX_TERMS; ++k) {
        term *= x2_over_4 / (static_cast<f64>(k) * k);
        sum += term;
        if (term < sum * BESSEL_CONVERGENCE)
            break;
    }
    return sum;
}

/// Normalised sinc: sin(pi*x) / (pi*x).
f64 sinc(f64 x) {
    if (std::abs(x) < SINC_EPSILON)
        return 1.0;
    const f64 px = PI * x;
    return std::sin(px) / px;
}

/// Destination-space filter half-width. For a 2× downsample this gives a
/// 6-source-pixel radius (≤14 taps). For larger ratios the actual tap count
/// is computed from the downsample ratio so that the kernel always covers
/// the full support.
constexpr i32 BASE_RADIUS = 3;

/// Compute the effective source-space radius for a given downsample ratio.
/// The radius must grow proportionally so that a 4× downsample uses twice
/// the tap span of a 2× downsample, preventing aliasing.
inline i32 effectiveRadius(f64 ratio) {
    return static_cast<i32>(std::ceil(BASE_RADIUS * std::max(ratio, 1.0)));
}

// ---- Weight builders -------------------------------------------------------

/// Kaiser-windowed sinc weights for one destination sample.
/// @p ratio is srcSize / dstSize (≥ 1).
u32 buildKaiserWeights(f64 center, u32 srcSize, f32* weights, i32* indices, f64 beta, f64 ratio) {
    const i32 srcRadius = effectiveRadius(ratio);
    const i32 firstSrcIndex = static_cast<i32>(std::ceil(center - srcRadius));
    const i32 lastSrcIndex = static_cast<i32>(std::floor(center + srcRadius));
    const f64 windowLength = srcRadius * 2.0 + 1.0;
    const f64 halfWindow = (windowLength - 1.0) * 0.5;
    const f64 invI0Beta = 1.0 / bessel_I0(beta);

    u32 tapCount = 0;
    f64 weightSum = 0.0;
    for (i32 srcIdx = firstSrcIndex; srcIdx <= lastSrcIndex; ++srcIdx) {
        const f64 distFromCenter = static_cast<f64>(srcIdx) - center;
        const f64 windowPosition = distFromCenter + srcRadius;
        const f64 normalizedPos = (windowPosition - halfWindow) / halfWindow;
        const f64 kaiserArg = 1.0 - normalizedPos * normalizedPos;
        const f64 kaiserValue =
            (kaiserArg < 0.0) ? 0.0 : bessel_I0(beta * std::sqrt(kaiserArg)) * invI0Beta;
        // Divide distance by the ratio so the sinc frequency matches the target.
        const f64 tapWeight = sinc(distFromCenter / ratio) * kaiserValue;
        weights[tapCount] = static_cast<f32>(tapWeight);
        indices[tapCount] = std::clamp(srcIdx, 0, static_cast<i32>(srcSize) - 1);
        weightSum += tapWeight;
        ++tapCount;
    }
    if (weightSum > 0.0) {
        const f32 normFactor = static_cast<f32>(1.0 / weightSum);
        for (u32 tapIdx = 0; tapIdx < tapCount; ++tapIdx)
            weights[tapIdx] *= normFactor;
    }
    return tapCount;
}

/// Lanczos3-windowed sinc weights for one destination sample.
/// @p ratio is srcSize / dstSize (≥ 1).
u32 buildLanczos3Weights(f64 center, u32 srcSize, f32* weights, i32* indices, f64 ratio) {
    const i32 srcRadius = effectiveRadius(ratio);
    const i32 firstSrcIndex = static_cast<i32>(std::ceil(center - srcRadius));
    const i32 lastSrcIndex = static_cast<i32>(std::floor(center + srcRadius));

    u32 tapCount = 0;
    f64 weightSum = 0.0;
    for (i32 srcIdx = firstSrcIndex; srcIdx <= lastSrcIndex; ++srcIdx) {
        const f64 destPixelDist = (static_cast<f64>(srcIdx) - center) / ratio;
        const f64 tapWeight = sinc(destPixelDist) * sinc(destPixelDist / LANCZOS_LOBES);
        weights[tapCount] = static_cast<f32>(tapWeight);
        indices[tapCount] = std::clamp(srcIdx, 0, static_cast<i32>(srcSize) - 1);
        weightSum += tapWeight;
        ++tapCount;
    }
    if (weightSum > 0.0) {
        const f32 normFactor = static_cast<f32>(1.0 / weightSum);
        for (u32 tapIdx = 0; tapIdx < tapCount; ++tapIdx)
            weights[tapIdx] *= normFactor;
    }
    return tapCount;
}

// ---- Generic separable 2-pass downsample -----------------------------------

template <typename WeightFn>
MipImage separableDownsample(const MipImage& src, u32 dstWidth, u32 dstHeight,
                             WeightFn buildWeights) {
    const u32 numChannels = src.channels;
    const f64 ratioX = static_cast<f64>(src.width) / dstWidth;
    const f64 ratioY = static_cast<f64>(src.height) / dstHeight;

    // Max taps per sample: 2 * effectiveRadius + 2, for safety.
    const size_t maxTapsX = static_cast<size_t>(effectiveRadius(ratioX)) * 2 + 2;
    const size_t maxTapsY = static_cast<size_t>(effectiveRadius(ratioY)) * 2 + 2;

    // Pass 1 — horizontal: src.width → dstWidth, keep src.height.
    MipImage horizontalPass(dstWidth, src.height, numChannels);
    std::vector<f32> tapWeightsH(maxTapsX);
    std::vector<i32> tapIndicesH(maxTapsX);
    for (u32 srcY = 0; srcY < src.height; ++srcY) {
        for (u32 dstX = 0; dstX < dstWidth; ++dstX) {
            const f64 center = (static_cast<f64>(dstX) + HALF_PIXEL) * ratioX - HALF_PIXEL;

            const u32 tapCount =
                buildWeights(center, src.width, tapWeightsH.data(), tapIndicesH.data(), ratioX);

            f32* outPixel = horizontalPass.pixel(dstX, srcY);
            for (u32 channel = 0; channel < numChannels; ++channel)
                outPixel[channel] = 0.0f;
            for (u32 tap = 0; tap < tapCount; ++tap) {
                const f32* srcPixel = src.pixel(static_cast<u32>(tapIndicesH[tap]), srcY);
                for (u32 channel = 0; channel < numChannels; ++channel)
                    outPixel[channel] += srcPixel[channel] * tapWeightsH[tap];
            }
        }
    }

    // Pass 2 — vertical: src.height → dstHeight, keep dstWidth.
    MipImage dst(dstWidth, dstHeight, numChannels);
    std::vector<f32> tapWeightsV(maxTapsY);
    std::vector<i32> tapIndicesV(maxTapsY);
    for (u32 dstX = 0; dstX < dstWidth; ++dstX) {
        for (u32 dstY = 0; dstY < dstHeight; ++dstY) {
            const f64 center = (static_cast<f64>(dstY) + HALF_PIXEL) * ratioY - HALF_PIXEL;

            const u32 tapCount =
                buildWeights(center, src.height, tapWeightsV.data(), tapIndicesV.data(), ratioY);

            f32* outPixel = dst.pixel(dstX, dstY);
            for (u32 channel = 0; channel < numChannels; ++channel)
                outPixel[channel] = 0.0f;
            for (u32 tap = 0; tap < tapCount; ++tap) {
                const f32* srcPixel =
                    horizontalPass.pixel(dstX, static_cast<u32>(tapIndicesV[tap]));
                for (u32 channel = 0; channel < numChannels; ++channel)
                    outPixel[channel] += srcPixel[channel] * tapWeightsV[tap];
            }
        }
    }

    return dst;
}

} // anonymous namespace

// ============================================================================
// Public filter functions
// ============================================================================

constexpr f64 DEFAULT_KAISER_BETA = 4.0;

MipImage kaiserFilter(const MipImage& src, u32 dstWidth, u32 dstHeight, f64 beta) {
    return separableDownsample(
        src, dstWidth, dstHeight,
        [beta](f64 center, u32 srcSize, f32* tapWeights, i32* tapIndices, f64 ratio) {
            return buildKaiserWeights(center, srcSize, tapWeights, tapIndices, beta, ratio);
        });
}

MipImage kaiserFilter(const MipImage& src, u32 dstWidth, u32 dstHeight) {
    return kaiserFilter(src, dstWidth, dstHeight, DEFAULT_KAISER_BETA);
}

MipImage kaiserFilter(const MipImage& src, f64 beta) {
    return kaiserFilter(src, std::max(src.width / 2, 1u), std::max(src.height / 2, 1u), beta);
}

MipImage kaiserFilter(const MipImage& src) {
    return kaiserFilter(src, DEFAULT_KAISER_BETA);
}

MipImage lanczos3Filter(const MipImage& src, u32 dstWidth, u32 dstHeight) {
    return separableDownsample(src, dstWidth, dstHeight, buildLanczos3Weights);
}

MipImage lanczos3Filter(const MipImage& src) {
    return lanczos3Filter(src, std::max(src.width / 2, 1u), std::max(src.height / 2, 1u));
}

} // namespace whiteout::textures::mipmap

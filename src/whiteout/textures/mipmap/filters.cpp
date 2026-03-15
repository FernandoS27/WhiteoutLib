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

// ============================================================================
// GGX importance-sampled environment prefilter
// ============================================================================

namespace {

constexpr u32 GGX_SAMPLE_COUNT = 128;

/// Van der Corput radical inverse in base 2.
f32 radicalInverseBase2(u32 n) {
    n = (n << 16u) | (n >> 16u);
    n = ((n & 0x55555555u) << 1u) | ((n & 0xAAAAAAAAu) >> 1u);
    n = ((n & 0x33333333u) << 2u) | ((n & 0xCCCCCCCCu) >> 2u);
    n = ((n & 0x0F0F0F0Fu) << 4u) | ((n & 0xF0F0F0F0u) >> 4u);
    n = ((n & 0x00FF00FFu) << 8u) | ((n & 0xFF00FF00u) >> 8u);
    return f32(n) * 2.3283064365386963e-10f; // / 0x100000000
}

/// 2D Hammersley point (i/N, VdC(i)).
void hammersley2(u32 i, u32 N, f32& xi1, f32& xi2) {
    xi1 = f32(i) / f32(N);
    xi2 = radicalInverseBase2(i);
}

/// GGX importance-sample a half-vector in tangent space (hz > 0).
void importanceSampleGGX(f32 xi1, f32 xi2, f32 roughness, f32& hx, f32& hy, f32& hz) {
    const f32 a = roughness * roughness;
    const f32 phi = 2.0f * static_cast<f32>(PI) * xi1;
    const f32 denom = std::max(1.0f + (a * a - 1.0f) * xi2, 1e-7f);
    const f32 cosTheta = std::sqrt((1.0f - xi2) / denom);
    const f32 sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
    hx = std::cos(phi) * sinTheta;
    hy = std::sin(phi) * sinTheta;
    hz = cosTheta;
}

/// Equirectangular UV → unit direction.
/// Convention: u ∈ [0,1) maps azimuth [0, 2π), v ∈ [0,1] maps inclination [0 (top), π (bottom)].
void uvToDir(f32 u, f32 v, f32& dx, f32& dy, f32& dz) {
    const f32 phi   = u * 2.0f * static_cast<f32>(PI);
    const f32 theta = v * static_cast<f32>(PI);
    const f32 sinT  = std::sin(theta);
    dx = sinT * std::cos(phi);
    dy = std::cos(theta);
    dz = sinT * std::sin(phi);
}

/// Unit direction → equirectangular UV.
void dirToUV(f32 dx, f32 dy, f32 dz, f32& ou, f32& ov) {
    const f32 phi = std::atan2(dz, dx); // [-π, π]
    ou = phi / (2.0f * static_cast<f32>(PI));
    if (ou < 0.0f) ou += 1.0f;
    ov = std::acos(std::clamp(dy, -1.0f, 1.0f)) / static_cast<f32>(PI);
}

/// Build a tangent basis (T, B) orthonormal to N.
void buildTBN(f32 nx, f32 ny, f32 nz,
              f32& tx, f32& ty, f32& tz,
              f32& bx, f32& by, f32& bz) {
    // Choose an up vector that is not collinear with N.
    const bool nearPole = std::abs(ny) > 0.999f;
    const f32 ux = nearPole ? 1.0f : 0.0f;
    const f32 uy = nearPole ? 0.0f : 1.0f;
    // T = normalize(up × N)
    tx = uy * nz;           // uz = 0, so uy*nz - uz*ny = uy*nz
    ty = -ux * nz;          // uz*nx - ux*nz = -ux*nz
    tz = ux * ny - uy * nx;
    const f32 tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (tlen > 1e-7f) { tx /= tlen; ty /= tlen; tz /= tlen; }
    // B = N × T
    bx = ny * tz - nz * ty;
    by = nz * tx - nx * tz;
    bz = nx * ty - ny * tx;
}

/// Bilinear sample of a MipImage at (u, v) ∈ [0,1]².
/// U wraps horizontally; V clamps at poles.
void sampleBilinear(const MipImage& img, f32 u, f32 v, f32* result, u32 channels) {
    u -= std::floor(u); // wrap U
    v = std::clamp(v, 0.0f, 1.0f);
    const f32 px = u * f32(img.width  - 1);
    const f32 py = v * f32(img.height - 1);
    const u32 x0 = u32(px);
    const u32 y0 = u32(py);
    const u32 x1 = std::min(x0 + 1, img.width  - 1);
    const u32 y1 = std::min(y0 + 1, img.height - 1);
    const f32 fx = px - f32(x0);
    const f32 fy = py - f32(y0);
    const f32* p00 = img.pixel(x0, y0);
    const f32* p10 = img.pixel(x1, y0);
    const f32* p01 = img.pixel(x0, y1);
    const f32* p11 = img.pixel(x1, y1);
    for (u32 c = 0; c < channels; ++c)
        result[c] = (p00[c] * (1.0f - fx) + p10[c] * fx) * (1.0f - fy) +
                    (p01[c] * (1.0f - fx) + p11[c] * fx) * fy;
}

} // anonymous namespace

MipImage environmentPrefilterGGX(const MipImage& src, u32 dstW, u32 dstH) {
    // Derive roughness from the mip level implied by the downsample ratio.
    // roughness = log2(srcW / dstW) / log2(srcW) maps linearly from mip 0
    // (roughness = 0) to the smallest mip (roughness = 1).
    const f32 srcLog2 = std::log2(f32(src.width));
    const f32 roughness = srcLog2 > 0.0f
        ? std::clamp(std::log2(f32(src.width) / f32(dstW)) / srcLog2, 0.0f, 1.0f)
        : 0.0f;

    MipImage dst(dstW, dstH, src.channels);

    for (u32 y = 0; y < dstH; ++y) {
        const f32 tv = (f32(y) + 0.5f) / f32(dstH);
        for (u32 x = 0; x < dstW; ++x) {
            const f32 tu = (f32(x) + 0.5f) / f32(dstW);

            // Convert texel centre to a world direction on the sphere.
            f32 nx, ny, nz;
            uvToDir(tu, tv, nx, ny, nz);

            // Build tangent basis (V == N, isotropic split-sum assumption).
            f32 tx, ty, tz, bx, by, bz;
            buildTBN(nx, ny, nz, tx, ty, tz, bx, by, bz);

            // Accumulate GGX importance-sampled contributions.
            f32 color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            f32 totalWeight = 0.0f;

            for (u32 i = 0; i < GGX_SAMPLE_COUNT; ++i) {
                f32 xi1, xi2;
                hammersley2(i, GGX_SAMPLE_COUNT, xi1, xi2);

                // Half-vector in tangent space, transform to world space.
                f32 hx, hy, hz;
                importanceSampleGGX(xi1, xi2, roughness, hx, hy, hz);
                const f32 wx = tx * hx + bx * hy + nx * hz;
                const f32 wy = ty * hx + by * hy + ny * hz;
                const f32 wz = tz * hx + bz * hy + nz * hz;

                // Sample direction L = 2*(N·H)*H - N.
                const f32 NdotH = nx * wx + ny * wy + nz * wz;
                const f32 lx = 2.0f * NdotH * wx - nx;
                const f32 ly = 2.0f * NdotH * wy - ny;
                const f32 lz = 2.0f * NdotH * wz - nz;
                const f32 NdotL = nx * lx + ny * ly + nz * lz;
                if (NdotL <= 0.0f)
                    continue;

                f32 su, sv;
                dirToUV(lx, ly, lz, su, sv);
                f32 sample[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                sampleBilinear(src, su, sv, sample, src.channels);

                for (u32 c = 0; c < src.channels; ++c)
                    color[c] += sample[c] * NdotL;
                totalWeight += NdotL;
            }

            f32* dstPixel = dst.pixel(x, y);
            if (totalWeight > 0.0f) {
                const f32 invW = 1.0f / totalWeight;
                for (u32 c = 0; c < src.channels; ++c)
                    dstPixel[c] = color[c] * invW;
            } else {
                // Roughness ≈ 0 or degenerate: copy the source texel verbatim.
                sampleBilinear(src, tu, tv, dstPixel, src.channels);
            }
        }
    }
    return dst;
}

// ============================================================================
// Spherical Kaiser filter (solid-angle-weighted, equirectangular)
// ============================================================================

MipImage sphericalKaiserFilter(const MipImage& src, u32 dstW, u32 dstH) {
    constexpr f64 SKAISER_BETA = 6.0;
    constexpr i32 SKAISER_LOBES = 3;
    const f64 invI0Beta = 1.0 / bessel_I0(SKAISER_BETA);

    // Angular support radius in radians: 3 source-texel angular widths.
    const f64 angularRadius =
        SKAISER_LOBES * PI / static_cast<f64>(src.height);
    const f64 cosRadiusCutoff = std::cos(angularRadius);

    MipImage dst(dstW, dstH, src.channels);

    for (u32 y = 0; y < dstH; ++y) {
        const f32 tv = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(dstH);
        for (u32 x = 0; x < dstW; ++x) {
            const f32 tu = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(dstW);

            f32 nx, ny, nz;
            uvToDir(tu, tv, nx, ny, nz);

            // Conservative UV bounding box in source texel space.
            // Near the poles sinTheta → 0, so cap duPad at 0.5 (full-wrap).
            const f32 sinThetaN =
                std::max(std::sin(tv * static_cast<f32>(PI)), 0.01f);
            const f32 dvPad = static_cast<f32>(angularRadius / PI);
            const f32 duPad = std::min(
                static_cast<f32>(angularRadius / (PI * static_cast<f64>(sinThetaN))), 0.5f);

            const u32 srcY0 = static_cast<u32>(
                std::max(0.0f, (tv - dvPad) * static_cast<f32>(src.height) - 0.5f));
            const u32 srcY1 = std::min(
                static_cast<u32>((tv + dvPad) * static_cast<f32>(src.height) + 0.5f),
                src.height - 1u);
            const i32 srcX0 =
                static_cast<i32>((tu - duPad) * static_cast<f32>(src.width) - 0.5f);
            const i32 srcX1 =
                static_cast<i32>((tu + duPad) * static_cast<f32>(src.width) + 0.5f);

            f32 color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            f64 weightSum = 0.0;

            for (u32 sy = srcY0; sy <= srcY1; ++sy) {
                const f32 sv = (static_cast<f32>(sy) + 0.5f) / static_cast<f32>(src.height);
                // Solid-angle differential: sin(θ) dθ dφ.
                const f64 solidAngle = std::sin(static_cast<f64>(sv) * PI);

                for (i32 sx = srcX0; sx <= srcX1; ++sx) {
                    const f32 su = (static_cast<f32>(sx) + 0.5f) / static_cast<f32>(src.width);
                    f32 lx, ly, lz;
                    uvToDir(su, sv, lx, ly, lz);

                    const f32 cosAngle = nx * lx + ny * ly + nz * lz;
                    if (static_cast<f64>(cosAngle) < cosRadiusCutoff)
                        continue;

                    // Normalised angular distance t ∈ [0, 1].
                    const f64 angle = std::acos(
                        static_cast<f64>(std::clamp(cosAngle, -1.0f, 1.0f)));
                    const f64 t2 = (angle / angularRadius) * (angle / angularRadius);
                    const f64 kaiserW = (t2 < 1.0)
                        ? bessel_I0(SKAISER_BETA * std::sqrt(1.0 - t2)) * invI0Beta
                        : 0.0;
                    const f64 weight = kaiserW * solidAngle;

                    // Wrap sx across U boundary.
                    const u32 wrappedSx = static_cast<u32>(
                        (sx % static_cast<i32>(src.width) + static_cast<i32>(src.width)) %
                        static_cast<i32>(src.width));
                    const f32* srcPixel = src.pixel(wrappedSx, sy);

                    for (u32 c = 0; c < src.channels; ++c)
                        color[c] += srcPixel[c] * static_cast<f32>(weight);
                    weightSum += weight;
                }
            }

            f32* dstPixel = dst.pixel(x, y);
            if (weightSum > 0.0) {
                const f32 invW = static_cast<f32>(1.0 / weightSum);
                for (u32 c = 0; c < src.channels; ++c)
                    dstPixel[c] = color[c] * invW;
            } else {
                sampleBilinear(src, tu, tv, dstPixel, src.channels);
            }
        }
    }
    return dst;
}

} // namespace whiteout::textures::mipmap

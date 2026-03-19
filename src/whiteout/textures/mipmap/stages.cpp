// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file stages.cpp
/// @brief Implementation of mipmap pipeline pre/post-processing stages.

#include "stages.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace whiteout::textures::mipmap {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

/// Apply a per-element transform to every float in the image.
/// The flat inner loop auto-vectorizes well for simple arithmetic.
template <typename F>
void transformAllElements(MipImage& img, PipelineContext* ctx, F fn) {
    f32* data = img.pixels.data();
    const u32 rowStride = img.width * img.channels;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        const size_t begin = static_cast<size_t>(y0) * rowStride;
        const size_t end = static_cast<size_t>(y1) * rowStride;
        for (size_t i = begin; i < end; ++i)
            data[i] = fn(data[i]);
    });
}

/// Apply a per-element transform to the first `channelCount` channels of each
/// pixel, leaving any remaining channels (e.g. alpha) untouched.
template <typename F>
void transformColorChannels(MipImage& img, PipelineContext* ctx, u32 channelCount, F fn) {
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 width = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            f32* row = data + static_cast<size_t>(y) * width * channels;
            for (u32 x = 0; x < width; ++x) {
                f32* pixel = row + x * channels;
                for (u32 c = 0; c < channelCount; ++c)
                    pixel[c] = fn(pixel[c]);
            }
        }
    });
}

// ============================================================================
// sRGB  ⇄  Linear
// ============================================================================

// IEC 61966-2-1 sRGB transfer function constants.
constexpr f32 SRGB_LINEAR_CUTOFF = 0.04045f;
constexpr f32 SRGB_INVERSE_CUTOFF = 0.0031308f;
constexpr f32 SRGB_LINEAR_SLOPE = 12.92f;
constexpr f32 SRGB_CURVE_OFFSET = 0.055f;
constexpr f32 SRGB_CURVE_SCALE = 1.055f;
constexpr f32 SRGB_GAMMA = 2.4f;

f32 srgbToLinearExact(f32 srgbValue) {
    if (srgbValue <= SRGB_LINEAR_CUTOFF)
        return srgbValue / SRGB_LINEAR_SLOPE;
    return std::pow((srgbValue + SRGB_CURVE_OFFSET) / SRGB_CURVE_SCALE, SRGB_GAMMA);
}

f32 linearToSrgbExact(f32 linearValue) {
    if (linearValue <= SRGB_INVERSE_CUTOFF)
        return linearValue * SRGB_LINEAR_SLOPE;
    return SRGB_CURVE_SCALE * std::pow(linearValue, 1.0f / SRGB_GAMMA) - SRGB_CURVE_OFFSET;
}

/// 4096-entry LUT + linear interpolation replaces per-pixel std::pow (~50-100
/// cycles → ~5 cycles).  Max error < 1e-5 in [0,1] which is well within the
/// precision of f32 mipmap data.
constexpr u32 SRGB_LUT_SIZE = 4096;

struct SrgbLuts {
    f32 toLinear[SRGB_LUT_SIZE + 1];
    f32 toSrgb[SRGB_LUT_SIZE + 1];

    SrgbLuts() {
        for (u32 i = 0; i <= SRGB_LUT_SIZE; ++i) {
            const f32 x = static_cast<f32>(i) / static_cast<f32>(SRGB_LUT_SIZE);
            toLinear[i] = srgbToLinearExact(x);
            toSrgb[i] = linearToSrgbExact(x);
        }
    }
};

const SrgbLuts& srgbLuts() {
    static const SrgbLuts luts;
    return luts;
}

/// Linearly interpolate a precomputed LUT for values in [0, 1].
f32 lookupLut(const f32* lut, f32 value) {
    const f32 clamped = std::clamp(value, 0.0f, 1.0f);
    const f32 index = clamped * static_cast<f32>(SRGB_LUT_SIZE);
    const u32 lo = static_cast<u32>(index);
    const u32 hi = std::min(lo + 1, SRGB_LUT_SIZE);
    const f32 fraction = index - static_cast<f32>(lo);
    return lut[lo] + (lut[hi] - lut[lo]) * fraction;
}

f32 srgbToLinear(f32 value) { return lookupLut(srgbLuts().toLinear, value); }
f32 linearToSrgb(f32 value) { return lookupLut(srgbLuts().toSrgb, value); }

// ============================================================================
// Shared constants
// ============================================================================

/// Near-zero threshold for safe vector normalization.
constexpr f32 NORMALIZE_EPSILON = 1e-7f;
constexpr f32 NORMALIZE_EPSILON_SQ = NORMALIZE_EPSILON * NORMALIZE_EPSILON;

/// Alpha-mask constants.
constexpr f32 ALPHA_THRESHOLD = 0.5f;
constexpr f32 ALPHA_COVERAGE_SCALE_MAX = 4.0f;
constexpr int ALPHA_COVERAGE_SEARCH_ITERS = 16;

/// Clamp an offset coordinate to [0, size-1]. Simplifies border handling
/// in kernel-based filters.
u32 clampToRange(i32 value, u32 size) {
    return static_cast<u32>(std::clamp(value, 0, static_cast<i32>(size) - 1));
}

/// Squared length of the first three float elements (XYZ normal components).
f32 lengthSquared3(const f32* v) {
    return v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
}

} // anonymous namespace

// ============================================================================
// sRGB  ⇄  Linear
// ============================================================================

void linearize(MipImage& img, PipelineContext* ctx) {
    transformColorChannels(img, ctx, std::min(img.channels, 3u), srgbToLinear);
}

void delinearize(MipImage& img, PipelineContext* ctx) {
    transformColorChannels(img, ctx, std::min(img.channels, 3u), linearToSrgb);
}

// ============================================================================
// Normal-map helpers
// ============================================================================

void unpackNormals(MipImage& img, PipelineContext* ctx) {
    const u32 normalChannels = std::min(img.channels, 3u);
    auto unpack = [](f32 v) { return v * 2.0f - 1.0f; };

    // When all channels are normal channels, a flat contiguous loop
    // auto-vectorizes into a single fused multiply-add.
    if (normalChannels == img.channels)
        transformAllElements(img, ctx, unpack);
    else
        transformColorChannels(img, ctx, normalChannels, unpack);
}

void packNormals(MipImage& img, PipelineContext* ctx) {
    const u32 normalChannels = std::min(img.channels, 3u);
    auto pack = [](f32 v) { return (v + 1.0f) * 0.5f; };

    if (normalChannels == img.channels)
        transformAllElements(img, ctx, pack);
    else
        transformColorChannels(img, ctx, normalChannels, pack);
}

void renormalize(MipImage& img, PipelineContext* ctx) {
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 width = img.width;

    if (channels == 2) {
        // Two-channel normal map: reconstruct Z from X,Y then normalize XY.
        parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                f32* row = data + static_cast<size_t>(y) * width * 2;
                for (u32 x = 0; x < width; ++x) {
                    f32* pixel = row + x * 2;
                    const f32 nx = pixel[0];
                    const f32 ny = pixel[1];
                    const f32 nxSq = nx * nx, nySq = ny * ny;
                    const f32 nz = std::sqrt(std::max(0.0f, 1.0f - nxSq - nySq));
                    const f32 lenSq = nxSq + nySq + nz * nz;
                    if (lenSq > NORMALIZE_EPSILON_SQ) {
                        const f32 invLen = 1.0f / std::sqrt(lenSq);
                        pixel[0] = nx * invLen;
                        pixel[1] = ny * invLen;
                    }
                }
            }
        });
    } else if (channels >= 3) {
        // Three-channel (or more) normal map: normalize XYZ in-place.
        parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                f32* row = data + static_cast<size_t>(y) * width * channels;
                for (u32 x = 0; x < width; ++x) {
                    f32* pixel = row + x * channels;
                    const f32 lenSq = lengthSquared3(pixel);
                    if (lenSq > NORMALIZE_EPSILON_SQ) {
                        const f32 invLen = 1.0f / std::sqrt(lenSq);
                        pixel[0] *= invLen;
                        pixel[1] *= invLen;
                        pixel[2] *= invLen;
                    }
                }
            }
        });
    }
}

void toksvigCorrection(MipImage& img, PipelineContext* ctx) {
    if (img.channels < 4)
        return;
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 width = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            f32* row = data + static_cast<size_t>(y) * width * channels;
            for (u32 x = 0; x < width; ++x) {
                f32* pixel = row + x * channels;
                const f32 lenSq = lengthSquared3(pixel);
                pixel[3] = std::clamp(std::sqrt(lenSq), 0.0f, 1.0f);
            }
        }
    });
}

// ============================================================================
// Roughness / Gloss helpers
// ============================================================================

void squareRoughness(MipImage& img, PipelineContext* ctx) {
    transformAllElements(img, ctx, [](f32 v) { return v * v; });
}

void unsquareRoughness(MipImage& img, PipelineContext* ctx) {
    transformAllElements(img, ctx, [](f32 v) { return std::sqrt(std::max(v, 0.0f)); });
}

void glossToRoughness(MipImage& img, PipelineContext* ctx) {
    transformAllElements(img, ctx, [](f32 v) { return 1.0f - v; });
}

void roughnessToGloss(MipImage& img, PipelineContext* ctx) {
    glossToRoughness(img, ctx); // same operation: f(x) = 1 − x
}

// ============================================================================
// Lightmap helpers
// ============================================================================

void clampPositive(MipImage& img, PipelineContext* ctx) {
    const u32 colorChannels = std::min(img.channels, 3u);
    auto clampPos = [](f32 v) { return std::max(v, 0.0f); };

    // Flat loop when all channels need clamping: auto-vectorizes into MAXPS.
    if (colorChannels == img.channels)
        transformAllElements(img, ctx, clampPos);
    else
        transformColorChannels(img, ctx, colorChannels, clampPos);
}

// ============================================================================
// Alpha-mask helpers
// ============================================================================

f32 computeAlphaCoverage(const MipImage& img, f32 scale) {
    if (img.channels == 0)
        return 0.0f;
    const u32 channels = img.channels;
    const u32 alphaChannel = channels - 1;
    const size_t pixelCount = img.pixelCount();
    size_t aboveThreshold = 0;
    for (size_t i = 0; i < pixelCount; ++i)
        if (img.pixels[i * channels + alphaChannel] * scale >= ALPHA_THRESHOLD)
            ++aboveThreshold;
    return static_cast<f32>(aboveThreshold) / static_cast<f32>(pixelCount);
}

f32 preBlurAlpha(MipImage& img, PipelineContext* ctx) {
    if (img.channels == 0)
        return 0.5f;

    const u32 alphaChannel = img.channels - 1;
    const u32 width = img.width;
    const u32 height = img.height;
    const u32 channels = img.channels;

    const f32 coverageTarget = computeAlphaCoverage(img, 1.0f);

    // Separable 3-tap Gaussian blur [1/4  1/2  1/4] on alpha only.
    // Safety: `tmp` is a shared_ptr captured by value in both pass lambdas,
    // keeping the intermediate buffer alive across async tile tasks.  Timeline
    // ordering from parallelForRows ensures the vertical pass never reads tmp
    // before the horizontal pass has finished writing it.
    auto tmp = std::make_shared<std::vector<f32>>(static_cast<size_t>(width) * height);
    f32* imgData = img.pixels.data();
    f32* tmpData = tmp->data();

    // Horizontal pass → tmp
    parallelForRows(
        height, ctx, [alphaChannel, width, channels, imgData, tmpData, tmp](u32 y0, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                const size_t rowBase = static_cast<size_t>(y) * width;
                for (u32 x = 0; x < width; ++x) {
                    const u32 leftX = (x > 0) ? x - 1 : 0;
                    const u32 rightX = (x + 1 < width) ? x + 1 : width - 1;
                    const f32 alphaLeft = imgData[(rowBase + leftX) * channels + alphaChannel];
                    const f32 alphaCenter = imgData[(rowBase + x) * channels + alphaChannel];
                    const f32 alphaRight = imgData[(rowBase + rightX) * channels + alphaChannel];
                    tmpData[rowBase + x] =
                        0.25f * alphaLeft + 0.5f * alphaCenter + 0.25f * alphaRight;
                }
            }
        });

    // Vertical pass → img
    parallelForRows(
        height, ctx,
        [alphaChannel, width, height, channels, imgData, tmpData, tmp](u32 y0, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                const u32 aboveY = (y > 0) ? y - 1 : 0;
                const u32 belowY = (y + 1 < height) ? y + 1 : height - 1;
                const size_t aboveRow = static_cast<size_t>(aboveY) * width;
                const size_t currentRow = static_cast<size_t>(y) * width;
                const size_t belowRow = static_cast<size_t>(belowY) * width;
                for (u32 x = 0; x < width; ++x) {
                    imgData[(currentRow + x) * channels + alphaChannel] = std::clamp(
                        0.25f * tmpData[aboveRow + x] + 0.5f * tmpData[currentRow + x] +
                            0.25f * tmpData[belowRow + x],
                        0.0f, 1.0f);
                }
            }
        });

    return coverageTarget;
}

void preserveAlphaCoverage(MipImage& img, f32 coverageTarget, PipelineContext* ctx) {
    if (img.channels == 0)
        return;

    // Nothing to do for degenerate cases.
    if (coverageTarget <= 0.0f || coverageTarget >= 1.0f)
        return;

    // Binary search for scale s ∈ [0, ALPHA_COVERAGE_SCALE_MAX] such that
    // the fraction of texels with alpha * s >= 0.5 equals `coverageTarget`.
    f32 lo = 0.0f;
    f32 hi = ALPHA_COVERAGE_SCALE_MAX;
    for (int iter = 0; iter < ALPHA_COVERAGE_SEARCH_ITERS; ++iter) {
        const f32 mid = (lo + hi) * 0.5f;
        if (computeAlphaCoverage(img, mid) < coverageTarget)
            lo = mid;
        else
            hi = mid;
    }
    const f32 scale = (lo + hi) * 0.5f;

    const u32 alphaChannel = img.channels - 1;
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 width = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * width;
        const size_t end = static_cast<size_t>(y1) * width;
        for (size_t i = start; i < end; ++i) {
            const size_t alphaIdx = i * channels + alphaChannel;
            data[alphaIdx] = std::clamp(data[alphaIdx] * scale, 0.0f, 1.0f);
        }
    });
}

// ============================================================================
// Mask stages
// ============================================================================

void medianFilter3x3(MipImage& img, PipelineContext* ctx) {
    const u32 width = img.width;
    const u32 height = img.height;
    const u32 channels = img.channels;

    // Output buffer; shared_ptr keeps it alive in async mode.
    auto out = std::make_shared<std::vector<f32>>(img.pixels.size());
    f32* imgData = img.pixels.data();
    f32* outData = out->data();

    // Compute pass: read from img, write to out.
    parallelForRows(
        height, ctx, [width, height, channels, imgData, outData, out](u32 y0, u32 y1) {
            f32 window[9];
            for (u32 y = y0; y < y1; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    for (u32 c = 0; c < channels; ++c) {
                        u32 count = 0;
                        for (i32 dy = -1; dy <= 1; ++dy) {
                            for (i32 dx = -1; dx <= 1; ++dx) {
                                const u32 sampleX =
                                    clampToRange(static_cast<i32>(x) + dx, width);
                                const u32 sampleY =
                                    clampToRange(static_cast<i32>(y) + dy, height);
                                window[count++] =
                                    imgData[(static_cast<size_t>(sampleY) * width + sampleX) *
                                                channels +
                                            c];
                            }
                        }
                        std::nth_element(window, window + count / 2, window + count);
                        outData[(static_cast<size_t>(y) * width + x) * channels + c] =
                            window[count / 2];
                    }
                }
            }
        });

    // Copy-back pass: timeline ordering ensures compute is done first.
    parallelForRows(height, ctx, [width, channels, imgData, outData, out](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * width * channels;
        const size_t end = static_cast<size_t>(y1) * width * channels;
        std::memcpy(imgData + start, outData + start, (end - start) * sizeof(f32));
    });
}

void bilateralFilter(MipImage& img, PipelineContext* ctx) {
    const u32 width = img.width;
    const u32 height = img.height;
    const u32 channels = img.channels;

    constexpr i32 RADIUS = 2;                                               // 5x5 window
    constexpr f32 SIGMA_SPATIAL = 1.5f;                                     // spatial sigma
    constexpr f32 SIGMA_RANGE = 0.1f;                                       // range sigma
    constexpr f32 INV_2_SIGMA_S2 = 1.0f / (2.0f * SIGMA_SPATIAL * SIGMA_SPATIAL);
    constexpr f32 INV_2_SIGMA_R2 = 1.0f / (2.0f * SIGMA_RANGE * SIGMA_RANGE);
    constexpr i32 KERNEL_SIZE = 2 * RADIUS + 1;

    // Precompute the spatial Gaussian weights: these depend only on (dx,dy),
    // not on pixel data.  Avoids 25 std::exp calls per pixel per channel.
    f32 spatialKernel[KERNEL_SIZE * KERNEL_SIZE];
    for (i32 dy = -RADIUS; dy <= RADIUS; ++dy)
        for (i32 dx = -RADIUS; dx <= RADIUS; ++dx)
            spatialKernel[(dy + RADIUS) * KERNEL_SIZE + (dx + RADIUS)] =
                std::exp(-static_cast<f32>(dx * dx + dy * dy) * INV_2_SIGMA_S2);

    auto out = std::make_shared<std::vector<f32>>(img.pixels.size());
    f32* imgData = img.pixels.data();
    f32* outData = out->data();

    // Compute pass: per-channel bilateral.
    parallelForRows(height, ctx, [=, out = out](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const size_t centerIdx = (static_cast<size_t>(y) * width + x) * channels;

                for (u32 c = 0; c < channels; ++c) {
                    f32 weightedSum = 0.0f;
                    f32 totalWeight = 0.0f;
                    const f32 centerValue = imgData[centerIdx + c];

                    for (i32 dy = -RADIUS; dy <= RADIUS; ++dy) {
                        const u32 sampleY =
                            clampToRange(static_cast<i32>(y) + dy, height);
                        for (i32 dx = -RADIUS; dx <= RADIUS; ++dx) {
                            const u32 sampleX =
                                clampToRange(static_cast<i32>(x) + dx, width);

                            const f32 neighborValue =
                                imgData[(static_cast<size_t>(sampleY) * width + sampleX) *
                                            channels +
                                        c];
                            const f32 rangeDiff = neighborValue - centerValue;

                            const f32 weight =
                                spatialKernel[(dy + RADIUS) * KERNEL_SIZE + (dx + RADIUS)] *
                                std::exp(-rangeDiff * rangeDiff * INV_2_SIGMA_R2);
                            weightedSum += neighborValue * weight;
                            totalWeight += weight;
                        }
                    }
                    outData[centerIdx + c] =
                        (totalWeight > 0.0f) ? weightedSum / totalWeight : centerValue;
                }
            }
        }
    });

    // Copy-back pass.
    parallelForRows(height, ctx, [width, channels, imgData, outData, out](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * width * channels;
        const size_t end = static_cast<size_t>(y1) * width * channels;
        std::memcpy(imgData + start, outData + start, (end - start) * sizeof(f32));
    });
}

void clampBinary(MipImage& img, PipelineContext* ctx) {
    transformAllElements(img, ctx, [](f32 v) { return (v >= 0.5f) ? 1.0f : 0.0f; });
}

void clampUnit(MipImage& img, PipelineContext* ctx) {
    transformAllElements(img, ctx, [](f32 v) { return std::clamp(v, 0.0f, 1.0f); });
}

} // namespace whiteout::textures::mipmap

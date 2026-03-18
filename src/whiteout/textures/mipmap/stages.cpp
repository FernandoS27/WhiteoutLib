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
// sRGB  ⇄  Linear
// ============================================================================

namespace {

// IEC 61966-2-1 sRGB transfer function constants.
constexpr f32 SRGB_LINEAR_CUTOFF = 0.04045f;
constexpr f32 SRGB_INVERSE_CUTOFF = 0.0031308f;
constexpr f32 SRGB_LINEAR_SLOPE = 12.92f;
constexpr f32 SRGB_CURVE_OFFSET = 0.055f;
constexpr f32 SRGB_CURVE_SCALE = 1.055f;
constexpr f32 SRGB_GAMMA = 2.4f;

/// Near-zero threshold for safe vector normalization.
constexpr f32 NORMALIZE_EPSILON = 1e-7f;

f32 srgbToLinear(f32 srgbValue) {
    if (srgbValue <= SRGB_LINEAR_CUTOFF)
        return srgbValue / SRGB_LINEAR_SLOPE;
    return std::pow((srgbValue + SRGB_CURVE_OFFSET) / SRGB_CURVE_SCALE, SRGB_GAMMA);
}

f32 linearToSrgb(f32 linearValue) {
    if (linearValue <= SRGB_INVERSE_CUTOFF)
        return linearValue * SRGB_LINEAR_SLOPE;
    return SRGB_CURVE_SCALE * std::pow(linearValue, 1.0f / SRGB_GAMMA) - SRGB_CURVE_OFFSET;
}

} // anonymous namespace

void linearize(MipImage& img, PipelineContext* ctx) {
    const u32 colorChannelCount = std::min(img.channels, 3u);
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            f32* row = data + static_cast<size_t>(y) * w * channels;
            for (u32 x = 0; x < w; ++x) {
                f32* pixel = row + x * channels;
                for (u32 c = 0; c < colorChannelCount; ++c)
                    pixel[c] = srgbToLinear(pixel[c]);
            }
        }
    });
}

void delinearize(MipImage& img, PipelineContext* ctx) {
    const u32 colorChannelCount = std::min(img.channels, 3u);
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            f32* row = data + static_cast<size_t>(y) * w * channels;
            for (u32 x = 0; x < w; ++x) {
                f32* pixel = row + x * channels;
                for (u32 c = 0; c < colorChannelCount; ++c)
                    pixel[c] = linearToSrgb(pixel[c]);
            }
        }
    });
}

// ============================================================================
// Normal-map helpers
// ============================================================================

void unpackNormals(MipImage& img, PipelineContext* ctx) {
    const u32 normalChannelCount = std::min(img.channels, 3u);
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            f32* row = data + static_cast<size_t>(y) * w * channels;
            for (u32 x = 0; x < w; ++x) {
                f32* pixel = row + x * channels;
                for (u32 c = 0; c < normalChannelCount; ++c)
                    pixel[c] = pixel[c] * 2.0f - 1.0f;
            }
        }
    });
}

void packNormals(MipImage& img, PipelineContext* ctx) {
    const u32 normalChannelCount = std::min(img.channels, 3u);
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            f32* row = data + static_cast<size_t>(y) * w * channels;
            for (u32 x = 0; x < w; ++x) {
                f32* pixel = row + x * channels;
                for (u32 c = 0; c < normalChannelCount; ++c)
                    pixel[c] = (pixel[c] + 1.0f) * 0.5f;
            }
        }
    });
}

void renormalize(MipImage& img, PipelineContext* ctx) {
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    if (channels == 2) {
        parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                f32* row = data + static_cast<size_t>(y) * w * 2;
                for (u32 x = 0; x < w; ++x) {
                    f32* pixel = row + x * 2;
                    const f32 normalX = pixel[0], normalY = pixel[1];
                    const f32 normalZ =
                        std::sqrt(std::max(0.0f, 1.0f - normalX * normalX - normalY * normalY));
                    const f32 length =
                        std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
                    if (length > NORMALIZE_EPSILON) {
                        pixel[0] = normalX / length;
                        pixel[1] = normalY / length;
                    }
                }
            }
        });
    } else if (channels >= 3) {
        parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                f32* row = data + static_cast<size_t>(y) * w * channels;
                for (u32 x = 0; x < w; ++x) {
                    f32* pixel = row + x * channels;
                    const f32 length = std::sqrt(
                        pixel[0] * pixel[0] + pixel[1] * pixel[1] + pixel[2] * pixel[2]);
                    if (length > NORMALIZE_EPSILON) {
                        pixel[0] /= length;
                        pixel[1] /= length;
                        pixel[2] /= length;
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
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            f32* row = data + static_cast<size_t>(y) * w * channels;
            for (u32 x = 0; x < w; ++x) {
                f32* pixel = row + x * channels;
                const f32 normalLength =
                    std::sqrt(pixel[0] * pixel[0] + pixel[1] * pixel[1] + pixel[2] * pixel[2]);
                pixel[3] = std::clamp(normalLength, 0.0f, 1.0f);
            }
        }
    });
}

// ============================================================================
// Roughness / Gloss helpers
// ============================================================================

void squareRoughness(MipImage& img, PipelineContext* ctx) {
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * w * channels;
        const size_t end = static_cast<size_t>(y1) * w * channels;
        for (size_t i = start; i < end; ++i)
            data[i] *= data[i];
    });
}

void unsquareRoughness(MipImage& img, PipelineContext* ctx) {
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * w * channels;
        const size_t end = static_cast<size_t>(y1) * w * channels;
        for (size_t i = start; i < end; ++i)
            data[i] = std::sqrt(std::max(data[i], 0.0f));
    });
}

void glossToRoughness(MipImage& img, PipelineContext* ctx) {
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * w * channels;
        const size_t end = static_cast<size_t>(y1) * w * channels;
        for (size_t i = start; i < end; ++i)
            data[i] = 1.0f - data[i];
    });
}

void roughnessToGloss(MipImage& img, PipelineContext* ctx) {
    glossToRoughness(img, ctx); // same operation: f(x) = 1 − x
}

// ============================================================================
// Lightmap helpers
// ============================================================================

void clampPositive(MipImage& img, PipelineContext* ctx) {
    const u32 colorChannelCount = std::min(img.channels, 3u);
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            f32* row = data + static_cast<size_t>(y) * w * channels;
            for (u32 x = 0; x < w; ++x) {
                f32* pixel = row + x * channels;
                for (u32 c = 0; c < colorChannelCount; ++c)
                    pixel[c] = std::max(pixel[c], 0.0f);
            }
        }
    });
}

// ============================================================================
// Alpha-mask helpers
// ============================================================================

namespace {

constexpr f32 ALPHA_THRESHOLD = 0.5f;
constexpr f32 ALPHA_COVERAGE_SCALE_MAX = 4.0f;
constexpr int ALPHA_COVERAGE_SEARCH_ITERS = 16;

} // anonymous namespace

f32 computeAlphaCoverage(const MipImage& img, f32 scale) {
    if (img.channels == 0)
        return 0.0f;
    const u32 alphaCh = img.channels - 1;
    const size_t pixelCount = img.pixelCount();
    size_t above = 0;
    for (size_t i = 0; i < pixelCount; ++i)
        if (img.pixels[i * img.channels + alphaCh] * scale >= ALPHA_THRESHOLD)
            ++above;
    return static_cast<f32>(above) / static_cast<f32>(pixelCount);
}

f32 preBlurAlpha(MipImage& img, PipelineContext* ctx) {
    if (img.channels == 0)
        return 0.5f;

    const u32 alphaCh = img.channels - 1;
    const u32 w = img.width;
    const u32 h = img.height;
    const u32 channels = img.channels;

    // Capture pre-blur coverage.
    const f32 coverageTarget = computeAlphaCoverage(img, 1.0f);

    // Separable 3-tap Gaussian blur [¼ ½ ¼] on alpha only.
    // Safety: `tmp` is a shared_ptr captured by value in both pass lambdas,
    // keeping the intermediate buffer alive across async tile tasks.  Timeline
    // ordering from parallelForRows ensures the vertical pass never reads tmp
    // before the horizontal pass has finished writing it.
    auto tmp = std::make_shared<std::vector<f32>>(static_cast<size_t>(w) * h);
    f32* imgData = img.pixels.data();
    f32* tmpData = tmp->data();

    // Horizontal pass → tmp
    parallelForRows(h, ctx, [alphaCh, w, channels, imgData, tmpData, tmp](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u32 xL = (x > 0) ? x - 1 : 0;
                const u32 xR = (x + 1 < w) ? x + 1 : w - 1;
                const f32 aL = imgData[(y * w + xL) * channels + alphaCh];
                const f32 aM = imgData[(y * w + x)  * channels + alphaCh];
                const f32 aR = imgData[(y * w + xR) * channels + alphaCh];
                tmpData[y * w + x] = 0.25f * aL + 0.5f * aM + 0.25f * aR;
            }
        }
    });

    // Vertical pass → img
    parallelForRows(h, ctx, [alphaCh, w, h, channels, imgData, tmpData, tmp](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            const u32 yU = (y > 0) ? y - 1 : 0;
            const u32 yD = (y + 1 < h) ? y + 1 : h - 1;
            for (u32 x = 0; x < w; ++x) {
                imgData[(y * w + x) * channels + alphaCh] =
                    std::clamp(0.25f * tmpData[yU * w + x] + 0.5f * tmpData[y * w + x] +
                                   0.25f * tmpData[yD * w + x],
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
    // the fraction of texels with alpha * s >= 0.5 equals `target`.
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

    const u32 alphaCh = img.channels - 1;
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * w;
        const size_t end = static_cast<size_t>(y1) * w;
        for (size_t i = start; i < end; ++i)
            data[i * channels + alphaCh] =
                std::clamp(data[i * channels + alphaCh] * scale, 0.0f, 1.0f);
    });
}

// ============================================================================
// Mask stages
// ============================================================================

void medianFilter3x3(MipImage& img, PipelineContext* ctx) {
    const u32 w = img.width;
    const u32 h = img.height;
    const u32 channels = img.channels;

    // Output buffer; shared_ptr keeps it alive in async mode.
    auto out = std::make_shared<std::vector<f32>>(img.pixels.size());
    f32* imgData = img.pixels.data();
    f32* outData = out->data();

    // Compute pass: read from img, write to out.
    parallelForRows(h, ctx, [w, h, channels, imgData, outData, out](u32 y0, u32 y1) {
        f32 window[9];
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = 0; x < w; ++x) {
                for (u32 c = 0; c < channels; ++c) {
                    u32 count = 0;
                    for (i32 dy = -1; dy <= 1; ++dy) {
                        for (i32 dx = -1; dx <= 1; ++dx) {
                            const u32 sx = static_cast<u32>(
                                std::clamp(static_cast<i32>(x) + dx, 0, static_cast<i32>(w) - 1));
                            const u32 sy = static_cast<u32>(
                                std::clamp(static_cast<i32>(y) + dy, 0, static_cast<i32>(h) - 1));
                            window[count++] =
                                imgData[(static_cast<size_t>(sy) * w + sx) * channels + c];
                        }
                    }
                    std::nth_element(window, window + count / 2, window + count);
                    outData[(static_cast<size_t>(y) * w + x) * channels + c] = window[count / 2];
                }
            }
        }
    });

    // Copy-back pass: timeline ordering ensures compute is done first.
    parallelForRows(h, ctx, [w, channels, imgData, outData, out](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * w * channels;
        const size_t end = static_cast<size_t>(y1) * w * channels;
        std::memcpy(imgData + start, outData + start, (end - start) * sizeof(f32));
    });
}

void bilateralFilter(MipImage& img, PipelineContext* ctx) {
    const u32 w = img.width;
    const u32 h = img.height;
    const u32 channels = img.channels;

    constexpr i32 RADIUS = 2;       // 5x5 window
    constexpr f32 SIGMA_S = 1.5f;   // spatial sigma
    constexpr f32 SIGMA_R = 0.1f;   // range sigma (edge sensitivity)
    constexpr f32 INV_2_SIGMA_S2 = 1.0f / (2.0f * SIGMA_S * SIGMA_S);
    constexpr f32 INV_2_SIGMA_R2 = 1.0f / (2.0f * SIGMA_R * SIGMA_R);

    auto out = std::make_shared<std::vector<f32>>(img.pixels.size());
    f32* imgData = img.pixels.data();
    f32* outData = out->data();

    // Compute pass: per-channel bilateral.
    parallelForRows(h, ctx, [=, out = out](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const size_t centerIdx = (static_cast<size_t>(y) * w + x) * channels;

                for (u32 c = 0; c < channels; ++c) {
                    f32 sumValue = 0.0f;
                    f32 sumWeight = 0.0f;
                    const f32 centerVal = imgData[centerIdx + c];

                    for (i32 dy = -RADIUS; dy <= RADIUS; ++dy) {
                        const u32 sy = static_cast<u32>(
                            std::clamp(static_cast<i32>(y) + dy, 0, static_cast<i32>(h) - 1));
                        for (i32 dx = -RADIUS; dx <= RADIUS; ++dx) {
                            const u32 sx = static_cast<u32>(
                                std::clamp(static_cast<i32>(x) + dx, 0, static_cast<i32>(w) - 1));

                            const f32 srcVal =
                                imgData[(static_cast<size_t>(sy) * w + sx) * channels + c];
                            const f32 spatialDist2 =
                                static_cast<f32>(dx * dx + dy * dy);
                            const f32 rangeDiff = srcVal - centerVal;

                            const f32 weight = std::exp(
                                -spatialDist2 * INV_2_SIGMA_S2 -
                                rangeDiff * rangeDiff * INV_2_SIGMA_R2);
                            sumValue += srcVal * weight;
                            sumWeight += weight;
                        }
                    }
                    outData[centerIdx + c] =
                        (sumWeight > 0.0f) ? sumValue / sumWeight : centerVal;
                }
            }
        }
    });

    // Copy-back pass.
    parallelForRows(h, ctx, [w, channels, imgData, outData, out](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * w * channels;
        const size_t end = static_cast<size_t>(y1) * w * channels;
        std::memcpy(imgData + start, outData + start, (end - start) * sizeof(f32));
    });
}

void clampBinary(MipImage& img, PipelineContext* ctx) {
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * w * channels;
        const size_t end = static_cast<size_t>(y1) * w * channels;
        for (size_t i = start; i < end; ++i)
            data[i] = (data[i] >= 0.5f) ? 1.0f : 0.0f;
    });
}

void clampUnit(MipImage& img, PipelineContext* ctx) {
    const u32 channels = img.channels;
    f32* data = img.pixels.data();
    const u32 w = img.width;
    parallelForRows(img.height, ctx, [=](u32 y0, u32 y1) {
        const size_t start = static_cast<size_t>(y0) * w * channels;
        const size_t end = static_cast<size_t>(y1) * w * channels;
        for (size_t i = start; i < end; ++i)
            data[i] = std::clamp(data[i], 0.0f, 1.0f);
    });
}

} // namespace whiteout::textures::mipmap

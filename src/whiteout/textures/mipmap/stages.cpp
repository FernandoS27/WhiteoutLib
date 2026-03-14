// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file stages.cpp
/// @brief Implementation of mipmap pipeline pre/post-processing stages.

#include "stages.h"

#include <algorithm>
#include <cmath>

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

void linearize(MipImage& img) {
    const u32 colorChannelCount = std::min(img.channels, 3u);
    const size_t pixelCount = img.pixelCount();
    for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
        f32* pixel = img.pixels.data() + pixelIdx * img.channels;
        for (u32 channel = 0; channel < colorChannelCount; ++channel)
            pixel[channel] = srgbToLinear(pixel[channel]);
    }
}

void delinearize(MipImage& img) {
    const u32 colorChannelCount = std::min(img.channels, 3u);
    const size_t pixelCount = img.pixelCount();
    for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
        f32* pixel = img.pixels.data() + pixelIdx * img.channels;
        for (u32 channel = 0; channel < colorChannelCount; ++channel)
            pixel[channel] = linearToSrgb(pixel[channel]);
    }
}

// ============================================================================
// Normal-map helpers
// ============================================================================

void unpackNormals(MipImage& img) {
    const u32 normalChannelCount = std::min(img.channels, 3u);
    const size_t pixelCount = img.pixelCount();
    for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
        f32* pixel = img.pixels.data() + pixelIdx * img.channels;
        for (u32 channel = 0; channel < normalChannelCount; ++channel)
            pixel[channel] = pixel[channel] * 2.0f - 1.0f;
    }
}

void packNormals(MipImage& img) {
    const u32 normalChannelCount = std::min(img.channels, 3u);
    const size_t pixelCount = img.pixelCount();
    for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
        f32* pixel = img.pixels.data() + pixelIdx * img.channels;
        for (u32 channel = 0; channel < normalChannelCount; ++channel)
            pixel[channel] = (pixel[channel] + 1.0f) * 0.5f;
    }
}

void renormalize(MipImage& img) {
    const size_t pixelCount = img.pixelCount();
    if (img.channels == 2) {
        // 2-channel normal map: derive Z = sqrt(1 - x² - y²), normalize the
        // full 3-D vector, then store back just (x, y).  This keeps the
        // implicit-Z convention intact for shaders that reconstruct Z.
        for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
            f32* pixel = img.pixels.data() + pixelIdx * 2;
            const f32 normalX = pixel[0], normalY = pixel[1];
            const f32 normalZ =
                std::sqrt(std::max(0.0f, 1.0f - normalX * normalX - normalY * normalY));
            const f32 length = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
            if (length > NORMALIZE_EPSILON) {
                pixel[0] = normalX / length;
                pixel[1] = normalY / length;
            }
        }
    } else if (img.channels >= 3) {
        for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
            f32* pixel = img.pixels.data() + pixelIdx * img.channels;
            const f32 length =
                std::sqrt(pixel[0] * pixel[0] + pixel[1] * pixel[1] + pixel[2] * pixel[2]);
            if (length > NORMALIZE_EPSILON) {
                pixel[0] /= length;
                pixel[1] /= length;
                pixel[2] /= length;
            }
        }
    }
}

void toksvigCorrection(MipImage& img) {
    if (img.channels < 4)
        return;
    const size_t pixelCount = img.pixelCount();
    for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
        f32* pixel = img.pixels.data() + pixelIdx * img.channels;
        // The length of the averaged (un-renormalised) normal encodes how
        // aligned the source normals were.  A length of 1 means perfect
        // alignment; shorter lengths indicate increasing divergence and
        // require reduced specular power to avoid shimmering.
        const f32 normalLength =
            std::sqrt(pixel[0] * pixel[0] + pixel[1] * pixel[1] + pixel[2] * pixel[2]);
        pixel[3] = std::clamp(normalLength, 0.0f, 1.0f);
    }
}

// ============================================================================
// Roughness / Gloss helpers
// ============================================================================

void squareRoughness(MipImage& img) {
    for (auto& value : img.pixels)
        value *= value;
}

void unsquareRoughness(MipImage& img) {
    for (auto& value : img.pixels)
        value = std::sqrt(std::max(value, 0.0f));
}

void glossToRoughness(MipImage& img) {
    for (auto& value : img.pixels)
        value = 1.0f - value;
}

void roughnessToGloss(MipImage& img) {
    glossToRoughness(img); // same operation: f(x) = 1 − x
}

} // namespace whiteout::textures::mipmap

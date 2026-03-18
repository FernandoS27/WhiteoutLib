// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file srgb_linearize.cpp
/// @brief Implementation of sRGB linearization utility.

#include "srgb_linearize.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace whiteout::textures {

namespace {

// IEC 61966-2-1 sRGB transfer function constants.
constexpr f32 kSrgbLinearCutoff = 0.04045f;
constexpr f32 kSrgbCurveOffset = 0.055f;
constexpr f32 kSrgbCurveScale = 1.0f + kSrgbCurveOffset;
constexpr f32 kSrgbLinearSlope = 12.92f;
constexpr f32 kSrgbGamma = 2.4f;
constexpr f32 kSrgbInverseGamma = 1.0f / kSrgbGamma;
constexpr f32 kSrgbInverseCutoff = kSrgbLinearCutoff / kSrgbLinearSlope;

constexpr u32 kLutEntries = 256;
constexpr f32 kU8Max = 255.0f;
constexpr f32 kRoundingBias = 0.5f;

const std::array<u8, kLutEntries>& srgbToLinearLUT() {
    static const auto lut = [] {
        std::array<u8, kLutEntries> t{};
        for (u32 i = 0; i < kLutEntries; ++i) {
            const f32 s = static_cast<f32>(i) / kU8Max;
            const f32 l = (s <= kSrgbLinearCutoff)
                              ? (s / kSrgbLinearSlope)
                              : std::pow((s + kSrgbCurveOffset) / kSrgbCurveScale, kSrgbGamma);
            t[i] = static_cast<u8>(std::clamp(l * kU8Max + kRoundingBias, 0.0f, kU8Max));
        }
        return t;
    }();
    return lut;
}

const std::array<u8, kLutEntries>& linearToSrgbLUT() {
    static const auto lut = [] {
        std::array<u8, kLutEntries> t{};
        for (u32 i = 0; i < kLutEntries; ++i) {
            const f32 l = static_cast<f32>(i) / kU8Max;
            const f32 s = (l <= kSrgbInverseCutoff)
                              ? (l * kSrgbLinearSlope)
                              : (kSrgbCurveScale * std::pow(l, kSrgbInverseGamma) - kSrgbCurveOffset);
            t[i] = static_cast<u8>(std::clamp(s * kU8Max + kRoundingBias, 0.0f, kU8Max));
        }
        return t;
    }();
    return lut;
}

} // anonymous namespace

Texture linearizeSrgbCopy(const Texture& texture) {
    if (!texture.isSrgb())
        return Texture{texture}; // plain copy

    // Decompress BCn → RGBA8 if necessary.
    Texture work = (blockEdge(texture.format()) > 1) ? texture.copyAsFormat(PixelFormat::RGBA8)
                                                     : Texture{texture};

    const auto& lut = srgbToLinearLUT();
    const u32 channels = bytesPerBlock(work.format()); // bytes-per-pixel for uncompressed
    // Only convert colour channels (first 3); leave alpha untouched.
    const u32 colorCh = std::min(channels, 3u);

    u8* ptr = work.dataPtr();
    const u64 total = work.dataSize();

    for (u64 i = 0; i < total; i += channels) {
        for (u32 c = 0; c < colorCh; ++c)
            ptr[i + c] = lut[ptr[i + c]];
    }

    work.setSrgb(false);
    return work;
}

void ensureColorSpace(Texture& tex, bool srgb) {
    if (tex.isSrgb() == srgb)
        return;

    // Decompress BCn → RGBA8 if necessary.
    if (blockEdge(tex.format()) > 1)
        tex.format(PixelFormat::RGBA8);

    const u32 channels = bytesPerBlock(tex.format());
    const u32 colorCh = std::min(channels, 3u);

    // Pick the appropriate u8 LUT for the conversion direction.
    const auto& lut = srgb ? linearToSrgbLUT() : srgbToLinearLUT();

    u8* ptr = tex.dataPtr();
    const u64 total = tex.dataSize();

    for (u64 i = 0; i < total; i += channels) {
        for (u32 c = 0; c < colorCh; ++c)
            ptr[i + c] = lut[ptr[i + c]];
    }

    tex.setSrgb(srgb);
}

} // namespace whiteout::textures

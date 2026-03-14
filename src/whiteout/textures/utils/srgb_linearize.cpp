// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file srgb_linearize.cpp
/// @brief Implementation of sRGB linearization utility.

#include "srgb_linearize.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace whiteout::textures {

namespace detail {

static const std::array<u8, 256>& srgbToLinearLUT() {
    static const auto lut = [] {
        std::array<u8, 256> t{};
        for (int i = 0; i < 256; ++i) {
            float s = static_cast<float>(i) / 255.0f;
            float l = (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
            t[i] = static_cast<u8>(std::clamp(l * 255.0f + 0.5f, 0.0f, 255.0f));
        }
        return t;
    }();
    return lut;
}

} // namespace detail

Texture linearizeSrgbCopy(const Texture& texture) {
    if (!texture.isSrgb())
        return Texture{texture}; // plain copy

    // Decompress BCn → RGBA8 if necessary.
    Texture work = (blockEdge(texture.format()) > 1) ? texture.copyAsFormat(PixelFormat::RGBA8)
                                                     : Texture{texture};

    const auto& lut = detail::srgbToLinearLUT();
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

} // namespace whiteout::textures

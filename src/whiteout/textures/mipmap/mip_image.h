// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file mip_image.h
/// @brief Float-per-channel image buffer used internally by the mipmap pipeline.

#pragma once

#include <whiteout/common_types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

namespace whiteout::textures::mipmap {

/// Intermediate float image used during mipmap generation.
///
/// Every channel is stored as f32 in [0, 1] (or [-1, 1] after unpacking).
/// Layout is tightly packed: pixel (x,y) starts at index (y * width + x) * channels.
struct MipImage {
    std::vector<f32> pixels;
    u32 width = 0;
    u32 height = 0;
    u32 channels = 0;

    MipImage() = default;

    MipImage(u32 width, u32 height, u32 channels)
        : pixels(static_cast<size_t>(width) * height * channels, 0.0f), width(width),
          height(height), channels(channels) {}

    f32* pixel(u32 x, u32 y) {
        return pixels.data() + (static_cast<size_t>(y) * width + x) * channels;
    }
    const f32* pixel(u32 x, u32 y) const {
        return pixels.data() + (static_cast<size_t>(y) * width + x) * channels;
    }

    size_t pixelCount() const {
        return static_cast<size_t>(width) * height;
    }
};

} // namespace whiteout::textures::mipmap

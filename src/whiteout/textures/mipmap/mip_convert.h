// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file mip_convert.h
/// @brief Shared helpers for converting between Texture mip levels and MipImage.

#pragma once

#include "mip_image.h"

#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cstring>

namespace whiteout::textures::mipmap {

// ============================================================================
// Format helpers
// ============================================================================

constexpr f32 UNORM8_MAX = 255.0f;
constexpr f32 UNORM16_MAX = 65535.0f;
constexpr f32 ROUNDING_BIAS = 0.5f;

inline u32 channelCount(PixelFormat format) {
    switch (format) {
    case PixelFormat::R8:
    case PixelFormat::R16:
    case PixelFormat::R32F:
        return 1;
    case PixelFormat::RG8:
    case PixelFormat::RG16:
    case PixelFormat::RG32F:
        return 2;
    default:
        return 4;
    }
}

/// Returns bytes-per-component for uncompressed formats, or 0 for BCn.
inline u32 bytesPerComponent(PixelFormat format) {
    switch (format) {
    case PixelFormat::R8:
    case PixelFormat::RG8:
    case PixelFormat::RGBA8:
        return 1;
    case PixelFormat::R16:
    case PixelFormat::RG16:
    case PixelFormat::RGBA16:
        return 2;
    case PixelFormat::R32F:
    case PixelFormat::RG32F:
    case PixelFormat::RGBA32F:
        return 4;
    default:
        return 0;
    }
}

// ============================================================================
// Texture mip ↔ MipImage conversion
// ============================================================================

/// Unpack integer/float texel data into [0,1] float.
inline void unpackToFloat(const u8* src, f32* dst, size_t elementCount, u32 bytesPerElem) {
    switch (bytesPerElem) {
    case 1:
        for (size_t i = 0; i < elementCount; ++i)
            dst[i] = src[i] / UNORM8_MAX;
        break;
    case 2:
        for (size_t i = 0; i < elementCount; ++i) {
            u16 raw;
            std::memcpy(&raw, src + i * 2, 2);
            dst[i] = raw / UNORM16_MAX;
        }
        break;
    case 4:
        std::memcpy(dst, src, elementCount * sizeof(f32));
        break;
    default:
        break;
    }
}

/// Pack [0,1] float data back into integer/float texel data.
inline void packFromFloat(const f32* src, u8* dst, size_t elementCount, u32 bytesPerElem) {
    switch (bytesPerElem) {
    case 1:
        for (size_t i = 0; i < elementCount; ++i)
            dst[i] =
                static_cast<u8>(std::clamp(src[i] * UNORM8_MAX + ROUNDING_BIAS, 0.0f, UNORM8_MAX));
        break;
    case 2:
        for (size_t i = 0; i < elementCount; ++i) {
            u16 raw = static_cast<u16>(
                std::clamp(src[i] * UNORM16_MAX + ROUNDING_BIAS, 0.0f, UNORM16_MAX));
            std::memcpy(dst + i * 2, &raw, 2);
        }
        break;
    case 4:
        std::memcpy(dst, src, elementCount * sizeof(f32));
        break;
    default:
        break;
    }
}

/// Extract a mip level from a Texture into a float MipImage.
inline MipImage extractMip(const Texture& tex, u32 mip, u32 layer) {
    const auto& ml = tex.mipLevel(mip, layer);
    const u32 nc = channelCount(tex.format());
    const u32 bpc = bytesPerComponent(tex.format());
    MipImage img(ml.width, ml.height, nc);
    if (bpc > 0) {
        const auto srcData = tex.mipData(mip, layer);
        unpackToFloat(srcData.data(), img.pixels.data(), img.pixelCount() * nc, bpc);
    }
    return img;
}

/// Write a float MipImage back into a Texture mip level.
inline void writeMip(Texture& tex, u32 mip, u32 layer, const MipImage& img) {
    const u32 nc = channelCount(tex.format());
    const u32 bpc = bytesPerComponent(tex.format());
    if (bpc > 0) {
        auto dstData = tex.mipData(mip, layer);
        packFromFloat(img.pixels.data(), dstData.data(), img.pixelCount() * nc, bpc);
    }
}

} // namespace whiteout::textures::mipmap

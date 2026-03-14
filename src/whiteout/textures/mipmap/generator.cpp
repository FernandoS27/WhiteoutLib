// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file generator.cpp
/// @brief Mipmap generator implementation — kind-to-pipeline mapping and
///        Texture ↔ MipImage conversion.

#include "generator.h"

#include "filters.h"
#include "pipeline.h"
#include "stages.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace whiteout::textures::mipmap {

// ============================================================================
// Format helpers
// ============================================================================

namespace {

constexpr f32 UNORM8_MAX = 255.0f;
constexpr f32 UNORM16_MAX = 65535.0f;
constexpr f32 ROUNDING_BIAS = 0.5f;

u32 channelCount(PixelFormat format) {
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
u32 bytesPerComponent(PixelFormat format) {
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
void unpackToFloat(const u8* src, f32* dst, size_t elementCount, u32 bytesPerElem) {
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
void packFromFloat(const f32* src, u8* dst, size_t elementCount, u32 bytesPerElem) {
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

MipImage extractMip(const Texture& tex, u32 mip, u32 layer) {
    const auto& mipLevel = tex.mipLevel(mip, layer);
    const u32 numChannels = channelCount(tex.format());
    const u32 bpc = bytesPerComponent(tex.format());
    MipImage img(mipLevel.width, mipLevel.height, numChannels);
    if (bpc > 0) {
        const auto srcData = tex.mipData(mip, layer);
        unpackToFloat(srcData.data(), img.pixels.data(), img.pixelCount() * numChannels, bpc);
    }
    return img;
}

void writeMip(Texture& tex, u32 mip, u32 layer, const MipImage& img) {
    const u32 numChannels = channelCount(tex.format());
    const u32 bpc = bytesPerComponent(tex.format());
    if (bpc > 0) {
        auto dstData = tex.mipData(mip, layer);
        packFromFloat(img.pixels.data(), dstData.data(), img.pixelCount() * numChannels, bpc);
    }
}

// ============================================================================
// Kind → Pipeline mapping
// ============================================================================

/// Resolve overloaded free functions to the 3-argument Filter signature.
using FilterFn = MipImage (*)(const MipImage&, u32, u32);

/// Create a Filter that delegates to kaiserFilter with a fixed β value.
Filter makeKaiserFilter(f64 beta) {
    return [beta](const MipImage& image, u32 dstW, u32 dstH) {
        return kaiserFilter(image, dstW, dstH, beta);
    };
}

MipmapPipeline pipelineForKind(TextureKind kind, bool srgb) {
    const Filter lanczos3 = static_cast<FilterFn>(lanczos3Filter);
    const Filter box = static_cast<FilterFn>(boxFilter);
    const Filter kaiser6 = makeKaiserFilter(6.0);
    const Filter kaiser55 = makeKaiserFilter(5.5);
    const Filter kaiser65 = makeKaiserFilter(6.5);

    switch (kind) {
    case TextureKind::Diffuse:
    case TextureKind::Albedo:
        if (srgb)
            return {{linearize}, lanczos3, {delinearize}};
        return {{}, lanczos3, {}};

    case TextureKind::Normal:
        return {{unpackNormals}, kaiser6, {toksvigCorrection, renormalize, packNormals}};

    case TextureKind::Specular:
        if (srgb)
            return {{linearize}, kaiser6, {delinearize}};
        return {{}, kaiser6, {}};

    case TextureKind::Roughness:
        return {{squareRoughness}, kaiser65, {unsquareRoughness}};

    case TextureKind::Gloss:
        return {
            {glossToRoughness, squareRoughness}, kaiser65, {unsquareRoughness, roughnessToGloss}};

    case TextureKind::Metalness:
        return {{}, kaiser55, {}};

    case TextureKind::AmbientOcclusion:
        return {{}, kaiser6, {}};

    case TextureKind::Emissive:
        if (srgb)
            return {{linearize}, lanczos3, {delinearize}};
        return {{}, lanczos3, {}};

    // ORM is handled by per-channel splitting in generateMipmaps();
    // it never reaches here, but the compiler requires all enum values.
    case TextureKind::ORM:
    case TextureKind::Other:
    default:
        if (srgb)
            return {{linearize}, box, {delinearize}};
        return {{}, box, {}};
    }
}

// ============================================================================
// Normal-map channel expansion / collapse
// ============================================================================

/// Expand a 2- or 3-channel normal map to RGBA.
/// - 2ch (RG): reconstruct Z = sqrt(1 - x² - y²), set A = 1.
/// - 3ch (RGB): copy XYZ, set A = 1.
/// - 4ch: returned unchanged.
MipImage expandNormalToRGBA(const MipImage& src) {
    if (src.channels >= 4)
        return src;

    MipImage dst(src.width, src.height, 4);
    const size_t pixelCount = src.pixelCount();

    if (src.channels == 2) {
        for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
            const f32* srcPixel = src.pixels.data() + pixelIdx * 2;
            f32* dstPixel = dst.pixels.data() + pixelIdx * 4;
            // Data is still in [0,1] packed form.  Unpack to [-1,1] to
            // derive Z, then pack back so the pipeline's unpackNormals
            // stage works uniformly.
            const f32 normalX = srcPixel[0] * 2.0f - 1.0f;
            const f32 normalY = srcPixel[1] * 2.0f - 1.0f;
            const f32 normalZ =
                std::sqrt(std::max(0.0f, 1.0f - normalX * normalX - normalY * normalY));
            dstPixel[0] = srcPixel[0];
            dstPixel[1] = srcPixel[1];
            dstPixel[2] = (normalZ + 1.0f) * 0.5f; // pack Z back to [0,1]
            dstPixel[3] = 1.0f;
        }
    } else { // 3 channels
        for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
            const f32* srcPixel = src.pixels.data() + pixelIdx * 3;
            f32* dstPixel = dst.pixels.data() + pixelIdx * 4;
            dstPixel[0] = srcPixel[0];
            dstPixel[1] = srcPixel[1];
            dstPixel[2] = srcPixel[2];
            dstPixel[3] = 1.0f;
        }
    }
    return dst;
}

/// Collapse an RGBA MipImage back to @p targetChannels (2 or 3).
/// Drops alpha (Toksvig factor) and, for 2ch, drops the explicit Z.
MipImage collapseNormalFromRGBA(const MipImage& src, u32 targetChannels) {
    if (targetChannels >= 4)
        return src;

    MipImage dst(src.width, src.height, targetChannels);
    const size_t pixelCount = src.pixelCount();
    for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx) {
        const f32* srcPixel = src.pixels.data() + pixelIdx * 4;
        f32* dstPixel = dst.pixels.data() + pixelIdx * targetChannels;
        for (u32 channel = 0; channel < targetChannels; ++channel)
            dstPixel[channel] = srcPixel[channel];
    }
    return dst;
}

// ============================================================================
// Channel split / merge (for ORM per-channel processing)
// ============================================================================

/// Split a multi-channel MipImage into individual single-channel images.
std::vector<MipImage> splitChannels(const MipImage& src) {
    std::vector<MipImage> channelImages;
    channelImages.reserve(src.channels);
    const size_t pixelCount = src.pixelCount();
    for (u32 channelIdx = 0; channelIdx < src.channels; ++channelIdx) {
        MipImage singleChannel(src.width, src.height, 1);
        for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx)
            singleChannel.pixels[pixelIdx] = src.pixels[pixelIdx * src.channels + channelIdx];
        channelImages.push_back(std::move(singleChannel));
    }
    return channelImages;
}

/// Merge single-channel MipImages back into one multi-channel image.
MipImage mergeChannels(const std::vector<MipImage>& channels) {
    const u32 width = channels[0].width;
    const u32 height = channels[0].height;
    const u32 numChannels = static_cast<u32>(channels.size());
    MipImage dst(width, height, numChannels);
    const size_t pixelCount = dst.pixelCount();
    for (u32 channelIdx = 0; channelIdx < numChannels; ++channelIdx) {
        for (size_t pixelIdx = 0; pixelIdx < pixelCount; ++pixelIdx)
            dst.pixels[pixelIdx * numChannels + channelIdx] = channels[channelIdx].pixels[pixelIdx];
    }
    return dst;
}

} // anonymous namespace

// ============================================================================
// Public entry point
// ============================================================================

void generateMipmaps(Texture& tex) {
    const u32 mipCount = tex.mipCount();
    if (mipCount <= 1)
        return;

    const u32 layerCount = tex.layerCount();

    // ORM textures: split into individual channels, apply the correct
    // per-channel pipeline (AO / Roughness / Metalness), then recombine.
    // Each mip is generated directly from the original (mip 0) channels.
    if (tex.kind() == TextureKind::ORM) {
        // R = AO, G = Roughness, B = Metalness.
        constexpr TextureKind ORM_CHANNEL_KINDS[] = {
            TextureKind::AmbientOcclusion,
            TextureKind::Roughness,
            TextureKind::Metalness,
        };
        constexpr size_t ORM_CHANNEL_COUNT = std::size(ORM_CHANNEL_KINDS);

        MipmapPipeline channelPipelines[ORM_CHANNEL_COUNT];
        for (size_t i = 0; i < ORM_CHANNEL_COUNT; ++i)
            channelPipelines[i] = pipelineForKind(ORM_CHANNEL_KINDS[i], false);

        const Filter boxDown = static_cast<FilterFn>(boxFilter);

        for (u32 layer = 0; layer < layerCount; ++layer) {
            const auto originalChannels = splitChannels(extractMip(tex, 0, layer));

            for (u32 mip = 1; mip < mipCount; ++mip) {
                const auto& targetLevel = tex.mipLevel(mip, layer);
                const u32 targetWidth = targetLevel.width;
                const u32 targetHeight = targetLevel.height;

                std::vector<MipImage> mipChannels;
                mipChannels.reserve(originalChannels.size());

                // Known ORM channels: apply the matching per-channel pipeline.
                for (size_t ch = 0; ch < std::min(originalChannels.size(), ORM_CHANNEL_COUNT); ++ch)
                    mipChannels.push_back(channelPipelines[ch].execute(originalChannels[ch],
                                                                       targetWidth, targetHeight));

                // Any extra channels (e.g. alpha): simple box filter from original.
                for (size_t ch = ORM_CHANNEL_COUNT; ch < originalChannels.size(); ++ch)
                    mipChannels.push_back(boxDown(originalChannels[ch], targetWidth, targetHeight));

                writeMip(tex, mip, layer, mergeChannels(mipChannels));
            }
        }
        return;
    }

    const MipmapPipeline pipeline = pipelineForKind(tex.kind(), tex.isSrgb());
    const bool isNormal = tex.kind() == TextureKind::Normal;
    const u32 originalChannelCount = channelCount(tex.format());
    const bool needNormalExpansion = isNormal && originalChannelCount < 4;

    for (u32 layer = 0; layer < layerCount; ++layer) {
        // Extract the original (full-resolution) mip once per layer.
        MipImage originalMip = extractMip(tex, 0, layer);

        // Expand <4-channel normals to RGBA for correct 3-D filtering
        // and Toksvig correction.
        if (needNormalExpansion)
            originalMip = expandNormalToRGBA(originalMip);

        // Every mip is generated directly from the original — no cascading.
        for (u32 mip = 1; mip < mipCount; ++mip) {
            const auto& targetLevel = tex.mipLevel(mip, layer);
            MipImage mipResult =
                pipeline.execute(originalMip, targetLevel.width, targetLevel.height);

            // Collapse back to the texture's native channel count for writing.
            if (needNormalExpansion)
                writeMip(tex, mip, layer, collapseNormalFromRGBA(mipResult, originalChannelCount));
            else
                writeMip(tex, mip, layer, mipResult);
        }
    }
}

} // namespace whiteout::textures::mipmap

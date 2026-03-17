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
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include <whiteout/utils/job_group.h>

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

/// Create a PoolFilter that delegates to kaiserFilter with a fixed β value.
PoolFilter makeKaiserFilter(f64 beta) {
    return [beta](const MipImage& image, MipImage& dst, PipelineContext* ctx) {
        kaiserFilter(image, dst, beta, ctx);
    };
}

/// Wrap a stage function into a PoolStage (stages already accept ctx).
using StageFn = void (*)(MipImage&, PipelineContext*);

/// Wrap a 3-arg filter function into a PoolFilter.
using PoolFilterFn = void (*)(const MipImage&, MipImage&, PipelineContext*);

MipmapPipeline pipelineForKind(TextureKind kind, bool srgb) {
    const PoolFilter lanczos3 = static_cast<PoolFilterFn>(lanczos3Filter);
    const PoolFilter box = static_cast<PoolFilterFn>(boxFilter);
    const PoolFilter kaiser6 = makeKaiserFilter(6.0);
    const PoolFilter kaiser55 = makeKaiserFilter(5.5);
    const PoolFilter kaiser65 = makeKaiserFilter(6.5);
    const PoolFilter ggxEnv = static_cast<PoolFilterFn>(environmentPrefilterGGX);
    const PoolFilter spherKaiser = static_cast<PoolFilterFn>(sphericalKaiserFilter);

    // Stage function pointers are directly convertible to PoolStage.
    const PoolStage sLinearize = static_cast<StageFn>(linearize);
    const PoolStage sDelinearize = static_cast<StageFn>(delinearize);
    const PoolStage sUnpackNormals = static_cast<StageFn>(unpackNormals);
    const PoolStage sPackNormals = static_cast<StageFn>(packNormals);
    const PoolStage sRenormalize = static_cast<StageFn>(renormalize);
    const PoolStage sToksvig = static_cast<StageFn>(toksvigCorrection);
    const PoolStage sSquare = static_cast<StageFn>(squareRoughness);
    const PoolStage sUnsquare = static_cast<StageFn>(unsquareRoughness);
    const PoolStage sGlossToRough = static_cast<StageFn>(glossToRoughness);
    const PoolStage sRoughToGloss = static_cast<StageFn>(roughnessToGloss);
    const PoolStage sClampPos = static_cast<StageFn>(clampPositive);

    // AlphaMask stages: preBlurAlpha returns coverage, preserveAlphaCoverage
    // consumes it.  We share the value through a std::shared_ptr<f32>.
    // Both stages are submitted as single tasks when running in async mode,
    // because they perform synchronous pixel reads before parallelForRows.
    auto makeAlphaStages = []() -> std::pair<PoolStage, PoolStage> {
        // Safety: `coverage` is a shared_ptr<f32> captured by value in both
        // the pre and post PoolStage lambdas.  Timeline ordering ensures pre
        // completes (writing *coverage) before post reads it.  The shared_ptr
        // copies in each lambda keep the f32 alive even after the pipeline
        // object (and this factory lambda) is destroyed.
        auto coverage = std::make_shared<f32>(0.5f);
        PoolStage pre = [coverage](MipImage& img, PipelineContext* ctx) {
            if (ctx && ctx->sem) {
                // Safety: `&img` binds to the referent (state->input in
                // executeAsync), not the PoolStage parameter's stack slot.
                // submitSingleTask chains through the timeline, and
                // preBlurAlpha is called with ctx=nullptr so its internal
                // parallelForRows runs single-threaded — no nested dispatch.
                submitSingleTask(ctx, [&img, coverage]() {
                    *coverage = preBlurAlpha(img, nullptr);
                });
            } else {
                *coverage = preBlurAlpha(img, ctx);
            }
        };
        PoolStage post = [coverage](MipImage& img, PipelineContext* ctx) {
            if (ctx && ctx->sem) {
                submitSingleTask(ctx, [&img, coverage]() {
                    preserveAlphaCoverage(img, *coverage, nullptr);
                });
            } else {
                preserveAlphaCoverage(img, *coverage, ctx);
            }
        };
        return {std::move(pre), std::move(post)};
    };

    switch (kind) {
    case TextureKind::Diffuse:
    case TextureKind::Albedo:
        if (srgb)
            return {{sLinearize}, lanczos3, {sDelinearize}};
        return {{}, lanczos3, {}};

    case TextureKind::Normal:
        return {{sUnpackNormals}, kaiser6, {sToksvig, sRenormalize, sPackNormals}};

    case TextureKind::Specular:
        if (srgb)
            return {{sLinearize}, kaiser6, {sDelinearize}};
        return {{}, kaiser6, {}};

    case TextureKind::Roughness:
        return {{sSquare}, kaiser65, {sUnsquare}};

    case TextureKind::Gloss:
        return {{sGlossToRough, sSquare}, kaiser65, {sUnsquare, sRoughToGloss}};

    case TextureKind::Metalness:
        return {{}, kaiser55, {}};

    case TextureKind::AmbientOcclusion:
        return {{}, kaiser6, {}};

    case TextureKind::Emissive:
        if (srgb)
            return {{sLinearize}, lanczos3, {sDelinearize}};
        return {{}, lanczos3, {}};

    case TextureKind::AlphaMask: {
        auto [pre, post] = makeAlphaStages();
        return {{std::move(pre)}, kaiser6, {std::move(post)}};
    }

    case TextureKind::Lightmap:
        if (srgb)
            return {{sLinearize}, lanczos3, {sClampPos, sDelinearize}};
        return {{}, lanczos3, {sClampPos}};

    case TextureKind::EnvironmentPBR:
        if (srgb)
            return {{sLinearize}, ggxEnv, {sDelinearize}};
        return {{}, ggxEnv, {}};

    case TextureKind::EnvironmentLegacy:
        if (srgb)
            return {{sLinearize}, spherKaiser, {sDelinearize}};
        return {{}, spherKaiser, {}};

    // ORM is split into per-channel textures by Texture::generateMipmaps()
    // before reaching this function, so it falls through to the default.
    case TextureKind::ORM:
    case TextureKind::Other:
    default:
        if (srgb)
            return {{sLinearize}, box, {sDelinearize}};
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

} // anonymous namespace

// ============================================================================
// Public entry point
// ============================================================================

std::optional<std::string> generateMipmaps(Texture& tex, interfaces::WorkerPool* pool) {
    const u32 mipCount = tex.mipCount();
    if (mipCount <= 1)
        return std::nullopt;

    const u32 layerCount = tex.layerCount();
    const bool usePool = pool != nullptr && pool->threadCount() > 1;

    std::optional<std::string> firstError;
    std::mutex errorMutex;
    std::atomic<bool> hasError{false};

    utils::JobGroup jobGroup;

    auto captureError = [&](const std::string& errMsg) {
        std::lock_guard<std::mutex> guard(errorMutex);
        if (!firstError) {
            firstError = errMsg;
            hasError.store(true, std::memory_order_release);
        }
    };

    auto submitJob = [&](const auto& job) {
        if (hasError.load(std::memory_order_acquire))
            return;
        if (!usePool) {
            try {
                job();
            } catch (const std::exception& e) {
                captureError(std::string("Mipmap generation error: ") + e.what());
            } catch (...) {
                captureError("Mipmap generation error: unknown exception");
            }
            return;
        }

        interfaces::WorkerTask task;
        task.fn = [&, job]() {
            if (!hasError.load(std::memory_order_acquire)) {
                try {
                    job();
                } catch (const std::exception& e) {
                    captureError(std::string("Mipmap generation error: ") + e.what());
                } catch (...) {
                    captureError("Mipmap generation error: unknown exception");
                }
            }
            jobGroup.done();
        };
        jobGroup.add(1);
        pool->submit(task);
    };

    // Check if the pool supports timeline semaphore creation.  When it does,
    // compute and write tasks are chained through per-job semaphores so all
    // (mip,layer) pairs run concurrently without nested-pool deadlock.
    const bool useSemaphores = usePool && [&]() {
        auto test = pool->createTimelineSemaphore();
        return test != nullptr;
    }();

    const TextureKind texKind = tex.kind();
    const bool texSrgb = tex.isSrgb();
    const bool isNormal = texKind == TextureKind::Normal;
    const u32 originalChannelCount = channelCount(tex.format());
    const bool needNormalExpansion = isNormal && originalChannelCount < 4;

    std::vector<MipImage> originalByLayer;
    originalByLayer.reserve(layerCount);
    for (u32 layer = 0; layer < layerCount; ++layer) {
        MipImage originalMip = extractMip(tex, 0, layer);
        if (needNormalExpansion)
            originalMip = expandNormalToRGBA(originalMip);
        originalByLayer.push_back(std::move(originalMip));
    }

    if (useSemaphores) {
        const size_t totalJobs = static_cast<size_t>(layerCount) * (mipCount - 1);
        std::vector<std::unique_ptr<interfaces::TimelineSemaphore>> sems(totalJobs);
        std::vector<MipImage> results(totalJobs);
        std::vector<interfaces::TimelineSemaphore::Value> finalVals(totalJobs);

        for (size_t i = 0; i < totalJobs; ++i)
            sems[i] = pool->createTimelineSemaphore();

        // Submit flattened pipelines — each stage/filter is a series of
        // tile tasks chained through the per-job timeline semaphore.
        // No task ever blocks waiting for sub-tasks it spawned.
        for (u32 layer = 0; layer < layerCount; ++layer) {
            for (u32 mip = 1; mip < mipCount; ++mip) {
                if (hasError.load(std::memory_order_acquire))
                    break;

                const size_t idx =
                    static_cast<size_t>(layer) * (mipCount - 1) + (mip - 1);
                auto* sem = sems[idx].get();

                const MipmapPipeline pipeline = pipelineForKind(texKind, texSrgb);
                const auto& targetLevel = tex.mipLevel(mip, layer);

                // Seed the timeline so the first tile tasks can proceed.
                const auto startVal = sem->next();
                sem->signal(startVal);

                // executeAsync submits all pipeline steps as individual tasks.
                const auto computeDoneVal = pipeline.executeAsync(
                    originalByLayer[layer], targetLevel.width, targetLevel.height,
                    pool, sem, startVal, &results[idx]);

                // Submit write task.
                const auto writeDoneVal = sem->next();
                interfaces::WorkerTask writeTask;
                writeTask.fn = [&, mip, layer, idx]() {
                    if (!hasError.load(std::memory_order_acquire)) {
                        if (needNormalExpansion)
                            writeMip(tex, mip, layer,
                                     collapseNormalFromRGBA(results[idx], originalChannelCount));
                        else
                            writeMip(tex, mip, layer, results[idx]);
                    }
                };
                writeTask.waitSemaphore = sem;
                writeTask.waitValue = computeDoneVal;
                writeTask.signalSemaphore = sem;
                writeTask.signalValue = writeDoneVal;
                pool->submit(writeTask);

                finalVals[idx] = writeDoneVal;
            }
        }

        for (size_t i = 0; i < totalJobs; ++i)
            sems[i]->wait(finalVals[i]);
    } else {
        // Fallback: single-task compute+write via submitJob.
        // Each job builds its own pipeline for AlphaMask thread safety.
        for (u32 layer = 0; layer < layerCount; ++layer) {
            for (u32 mip = 1; mip < mipCount; ++mip) {
                submitJob([&, layer, mip, texKind, texSrgb]() {
                    const MipmapPipeline pipeline = pipelineForKind(texKind, texSrgb);
                    const auto& targetLevel = tex.mipLevel(mip, layer);
                    MipImage mipResult = pipeline.execute(originalByLayer[layer],
                                                          targetLevel.width, targetLevel.height);
                    if (needNormalExpansion)
                        writeMip(tex, mip, layer,
                                 collapseNormalFromRGBA(mipResult, originalChannelCount));
                    else
                        writeMip(tex, mip, layer, mipResult);
                });
            }
        }
        if (usePool)
            jobGroup.wait();
    }
    return firstError;
}

} // namespace whiteout::textures::mipmap

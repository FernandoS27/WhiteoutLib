// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file generator.cpp
/// @brief Mipmap generator implementation — kind-to-pipeline mapping and
///        Texture ↔ MipImage conversion.

#include "generator.h"

#include "filters.h"
#include "mip_convert.h"
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
// Slice helpers (3D textures only — not shared)
// ============================================================================

namespace {

/// Extract a single depth slice @p z from a mip level as a 2D MipImage.
MipImage extractSlice(const Texture& tex, u32 mip, u32 layer, u32 z) {
    const auto& mipLevel = tex.mipLevel(mip, layer);
    const u32 numChannels = channelCount(tex.format());
    const u32 bpc = bytesPerComponent(tex.format());
    MipImage img(mipLevel.width, mipLevel.height, numChannels);
    if (bpc > 0) {
        const auto srcData = tex.mipData(mip, layer);
        const size_t sliceElems =
            static_cast<size_t>(mipLevel.width) * mipLevel.height * numChannels;
        const size_t sliceBytes = sliceElems * bpc;
        unpackToFloat(srcData.data() + z * sliceBytes, img.pixels.data(), sliceElems, bpc);
    }
    return img;
}

/// Write a single depth slice @p z of a mip level from a 2D MipImage.
void writeSlice(Texture& tex, u32 mip, u32 layer, u32 z, const MipImage& img) {
    const auto& mipLevel = tex.mipLevel(mip, layer);
    const u32 numChannels = channelCount(tex.format());
    const u32 bpc = bytesPerComponent(tex.format());
    if (bpc > 0) {
        auto dstData = tex.mipData(mip, layer);
        const size_t sliceElems =
            static_cast<size_t>(mipLevel.width) * mipLevel.height * numChannels;
        const size_t sliceBytes = sliceElems * bpc;
        packFromFloat(img.pixels.data(), dstData.data() + z * sliceBytes, sliceElems, bpc);
    }
}

/// Accumulate @p src into @p acc element-wise.
void accumulateSlice(MipImage& acc, const MipImage& src) {
    const size_t count = acc.pixels.size();
    for (size_t i = 0; i < count; ++i)
        acc.pixels[i] += src.pixels[i];
}

/// Scale all elements in @p img by @p factor.
void scaleImage(MipImage& img, f32 factor) {
    for (auto& v : img.pixels)
        v *= factor;
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

    const PoolFilter maxPool = static_cast<PoolFilterFn>(maxPoolFilter);

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
    const PoolStage sMedian3x3 = static_cast<StageFn>(medianFilter3x3);
    const PoolStage sClampBin = static_cast<StageFn>(clampBinary);

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
                submitSingleTask(ctx,
                                 [&img, coverage]() { *coverage = preBlurAlpha(img, nullptr); });
            } else {
                *coverage = preBlurAlpha(img, ctx);
            }
        };
        PoolStage post = [coverage](MipImage& img, PipelineContext* ctx) {
            if (ctx && ctx->sem) {
                submitSingleTask(
                    ctx, [&img, coverage]() { preserveAlphaCoverage(img, *coverage, nullptr); });
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

    case TextureKind::BinaryMask:
        return {{sMedian3x3}, maxPool, {sClampBin}};

    case TextureKind::TransparencyMask: {
        auto [pre, post] = makeAlphaStages();
        return {{std::move(pre)}, kaiser6, {std::move(post)}};
    }

    case TextureKind::BlendMask: {
        // Capture alpha coverage before bilateral smoothing,
        // then restore it after downsampling + clamping.
        auto coverage = std::make_shared<f32>(0.5f);
        PoolStage pre = [coverage](MipImage& img, PipelineContext* ctx) {
            if (ctx && ctx->sem) {
                submitSingleTask(ctx, [&img, coverage]() {
                    *coverage = computeAlphaCoverage(img);
                    bilateralFilter(img, nullptr);
                });
            } else {
                *coverage = computeAlphaCoverage(img);
                bilateralFilter(img, ctx);
            }
        };
        PoolStage post = [coverage](MipImage& img, PipelineContext* ctx) {
            clampUnit(img, ctx);
            if (ctx && ctx->sem) {
                submitSingleTask(
                    ctx, [&img, coverage]() { preserveAlphaCoverage(img, *coverage, nullptr); });
            } else {
                preserveAlphaCoverage(img, *coverage, ctx);
            }
        };
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

    // ORM (deprecated) and Multikind are split into per-channel textures by
    // Texture::generateMipmaps() before reaching this function, so they fall
    // through to the default.  Unused channels also use a plain box filter.
    case TextureKind::ORM:
    case TextureKind::Multikind:
    case TextureKind::Unused:
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

    std::atomic<bool> hasError{false};

    utils::JobGroup jobGroup;

    auto submitJob = [&](const auto& job) {
        if (hasError.load(std::memory_order_acquire))
            return;
        if (!usePool) {
            job();
            return;
        }

        interfaces::WorkerTask task;
        task.fn = [&, job]() {
            if (!hasError.load(std::memory_order_acquire)) {
                job();
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
    const bool is3D = tex.mipLevel(0, 0).depth > 1;

    // For 2D/cube textures: one MipImage per layer (as before).
    // For 3D textures: per-slice MipImages for Z-aware mipmap generation.
    std::vector<MipImage> originalByLayer;
    std::vector<std::vector<MipImage>> srcSlicesByLayer;

    if (is3D) {
        srcSlicesByLayer.resize(layerCount);
        for (u32 layer = 0; layer < layerCount; ++layer) {
            const u32 srcDepth = tex.mipLevel(0, layer).depth;
            srcSlicesByLayer[layer].reserve(srcDepth);
            for (u32 z = 0; z < srcDepth; ++z) {
                MipImage slice = extractSlice(tex, 0, layer, z);
                if (needNormalExpansion)
                    slice = expandNormalToRGBA(slice);
                srcSlicesByLayer[layer].push_back(std::move(slice));
            }
        }
    } else {
        originalByLayer.reserve(layerCount);
        for (u32 layer = 0; layer < layerCount; ++layer) {
            MipImage originalMip = extractMip(tex, 0, layer);
            if (needNormalExpansion)
                originalMip = expandNormalToRGBA(originalMip);
            originalByLayer.push_back(std::move(originalMip));
        }
    }

    // Semaphore-based async path only supports 2D/cube textures.
    if (useSemaphores && !is3D) {
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

                const size_t idx = static_cast<size_t>(layer) * (mipCount - 1) + (mip - 1);
                auto* sem = sems[idx].get();

                const MipmapPipeline pipeline = pipelineForKind(texKind, texSrgb);
                const auto& targetLevel = tex.mipLevel(mip, layer);

                // Seed the timeline so the first tile tasks can proceed.
                const auto startVal = sem->next();
                sem->signal(startVal);

                // executeAsync submits all pipeline steps as individual tasks.
                const auto computeDoneVal =
                    pipeline.executeAsync(originalByLayer[layer], targetLevel.width,
                                          targetLevel.height, pool, sem, startVal, &results[idx]);

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
                if (is3D) {
                    // 3D textures: process each depth slice independently with
                    // Z-dimension averaging from the mip-0 source slices.
                    submitJob([&, layer, mip, texKind, texSrgb]() {
                        const auto& targetLevel = tex.mipLevel(mip, layer);
                        const u32 srcDepth = tex.mipLevel(0, layer).depth;
                        const u32 tgtDepth = targetLevel.depth;
                        const auto& srcSlices = srcSlicesByLayer[layer];

                        for (u32 z = 0; z < tgtDepth; ++z) {
                            // Map target slice z to source slice range.
                            const f32 zStart = static_cast<f32>(z) * srcDepth / tgtDepth;
                            const f32 zEnd = static_cast<f32>(z + 1) * srcDepth / tgtDepth;
                            const u32 zMin = static_cast<u32>(zStart);
                            const u32 zMax =
                                std::min(static_cast<u32>(std::ceil(zEnd)), srcDepth) - 1;

                            // Average contributing source slices for Z downsampling.
                            MipImage src = srcSlices[zMin];
                            for (u32 sz = zMin + 1; sz <= zMax; ++sz)
                                accumulateSlice(src, srcSlices[sz]);
                            const u32 sliceCount = zMax - zMin + 1;
                            if (sliceCount > 1)
                                scaleImage(src, 1.0f / static_cast<f32>(sliceCount));

                            const MipmapPipeline pipeline = pipelineForKind(texKind, texSrgb);
                            MipImage const result =
                                pipeline.execute(src, targetLevel.width, targetLevel.height);

                            if (needNormalExpansion)
                                writeSlice(tex, mip, layer, z,
                                           collapseNormalFromRGBA(result, originalChannelCount));
                            else
                                writeSlice(tex, mip, layer, z, result);
                        }
                    });
                } else {
                    submitJob([&, layer, mip, texKind, texSrgb]() {
                        const MipmapPipeline pipeline = pipelineForKind(texKind, texSrgb);
                        const auto& targetLevel = tex.mipLevel(mip, layer);
                        MipImage const mipResult = pipeline.execute(
                            originalByLayer[layer], targetLevel.width, targetLevel.height);
                        if (needNormalExpansion)
                            writeMip(tex, mip, layer,
                                     collapseNormalFromRGBA(mipResult, originalChannelCount));
                        else
                            writeMip(tex, mip, layer, mipResult);
                    });
                }
            }
        }
        if (usePool)
            jobGroup.wait();
    }
    return std::nullopt;
}

} // namespace whiteout::textures::mipmap

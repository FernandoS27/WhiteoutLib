// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pipeline.h
/// @brief Mipmap generation pipeline: pre-process → downsample → post-process.
///
/// A MipmapPipeline chains a configurable sequence of in-place processing
/// stages around a central downsample filter.  Calling `execute()` produces
/// a target-sized mip level from a source image.
///
/// When a WorkerPool is provided, stages and filters are parallelized by
/// splitting work into row-range tiles, using the same JobGroup pattern as
/// the BCn codec.

#pragma once

#include "mip_image.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <functional>
#include <vector>

namespace whiteout::textures::mipmap {

/// An in-place image transform (e.g. linearize, renormalize).
using Stage = std::function<void(MipImage&)>;

/// A pool-aware in-place image transform.
/// The pool may be nullptr, in which case the stage runs single-threaded.
using PoolStage = std::function<void(MipImage&, interfaces::WorkerPool*)>;

/// A downsample operation: produces a target-sized image from a source.
/// Parameters: (source image, target width, target height).
using Filter = std::function<MipImage(const MipImage&, u32, u32)>;

/// A pool-aware downsample operation.
using PoolFilter = std::function<MipImage(const MipImage&, u32, u32, interfaces::WorkerPool*)>;

// ── Row-parallel dispatch utility ──────────────────────────────────────

/// Minimum number of rows per tile to avoid excessive dispatch overhead.
constexpr u32 kMinRowsPerTile = 64;

/// Divide @p totalRows into tiles and execute @p fn(startRow, endRow) for each,
/// optionally in parallel via @p pool.  Falls back to serial when the pool is
/// null or the image is too small to benefit.
template <typename RowFn>
void parallelForRows(u32 totalRows, interfaces::WorkerPool* pool, RowFn&& fn) {
    if (!pool || totalRows <= kMinRowsPerTile) {
        fn(0u, totalRows);
        return;
    }

    const u32 threadCount = static_cast<u32>(pool->threadCount());
    const u32 tilesWanted = std::min(threadCount * 2, totalRows); // ~2 tiles per thread
    const u32 rowsPerTile = std::max(totalRows / tilesWanted, 1u);
    const u32 tileCount = (totalRows + rowsPerTile - 1) / rowsPerTile;

    if (tileCount <= 1) {
        fn(0u, totalRows);
        return;
    }

    utils::JobGroup jobGroup;
    for (u32 t = 0; t < tileCount; ++t) {
        const u32 startRow = t * rowsPerTile;
        const u32 endRow = std::min(startRow + rowsPerTile, totalRows);
        jobGroup.add(1);
        interfaces::WorkerTask task{
            [startRow, endRow, &fn, &jobGroup]() {
                fn(startRow, endRow);
                jobGroup.done();
            }
        };
        pool->submit(task);
    }
    jobGroup.wait();
}

/// Configurable pipeline that generates one mip level.
///
/// Pre-process stages are applied to a copy of the source before filtering.
/// The downsample filter then reduces from source resolution to the given
/// target dimensions.  Post-process stages finalize the result.
///
/// Usage:
/// @code
///     MipmapPipeline pipe;
///     pipe.preProcess  = { linearize };
///     pipe.downsample  = lanczos3Filter;
///     pipe.postProcess = { delinearize };
///
///     MipImage mip1 = pipe.execute(mip0, mip0.width / 2, mip0.height / 2);
/// @endcode
struct MipmapPipeline {
    std::vector<PoolStage> preProcess; ///< Stages applied to a **copy** of the source before filtering.
    PoolFilter downsample;             ///< The core downsample filter.
    std::vector<PoolStage> postProcess; ///< Stages applied to the filtered result.

    /// Run the full pipeline on @p src, downsampling to the given target size.
    /// @p pool may be nullptr for single-threaded execution.
    MipImage execute(const MipImage& src, u32 targetWidth, u32 targetHeight,
                     interfaces::WorkerPool* pool = nullptr) const {
        // Pre-process operates on a copy so the caller's image is unchanged.
        MipImage input = src;
        for (const auto& stage : preProcess)
            stage(input, pool);

        MipImage result = downsample(input, targetWidth, targetHeight, pool);

        for (const auto& stage : postProcess)
            stage(result, pool);

        return result;
    }
};

} // namespace whiteout::textures::mipmap

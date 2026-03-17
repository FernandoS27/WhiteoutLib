// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file pipeline.h
/// @brief Mipmap generation pipeline: pre-process → downsample → post-process.
///
/// A MipmapPipeline chains a configurable sequence of in-place processing
/// stages around a central downsample filter.  Calling `execute()` produces
/// a target-sized mip level from a source image.
///
/// When a PipelineContext with a timeline semaphore is provided, the pipeline
/// is flattened into a series of tile tasks chained through the timeline.
/// Each `parallelForRows` step uses `JobGroup::signalOnComplete()` instead
/// of blocking, so no worker thread ever waits on sub-tasks it submitted —
/// eliminating nested-dispatch deadlocks even with single-thread pools.

#pragma once

#include "mip_image.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace whiteout::textures::mipmap {

// ── Pipeline context ───────────────────────────────────────────────────

/// Carries a pool + optional timeline semaphore through the pipeline.
///
/// When `sem` is non-null, `parallelForRows` operates in **async mode**:
/// tile tasks are submitted with `waitSemaphore = sem` / `waitValue =
/// currentValue`, and a `JobGroup::signalOnComplete()` advances
/// `currentValue` when all tiles finish.  No thread blocks.
///
/// When `sem` is null, `parallelForRows` falls back to the original
/// blocking `JobGroup::wait()` path.
struct PipelineContext {
    interfaces::WorkerPool* pool = nullptr;
    interfaces::TimelineSemaphore* sem = nullptr;
    interfaces::TimelineSemaphore::Value currentValue = 0;
};

// ── Type aliases ───────────────────────────────────────────────────────

/// An in-place image transform (e.g. linearize, renormalize).
using Stage = std::function<void(MipImage&)>;

/// A context-aware in-place image transform.
/// ctx may be nullptr, in which case the stage runs single-threaded.
using PoolStage = std::function<void(MipImage&, PipelineContext*)>;

/// A downsample operation: writes the result into a pre-allocated destination.
using Filter = std::function<void(const MipImage&, MipImage&)>;

/// A context-aware downsample operation.
using PoolFilter = std::function<void(const MipImage&, MipImage&, PipelineContext*)>;

// ── Utilities ──────────────────────────────────────────────────────────

/// Submit a single function as a task chained through the PipelineContext
/// timeline.  In synchronous mode (no timeline), runs the function directly.
inline void submitSingleTask(PipelineContext* ctx, std::function<void()> fn) {
    if (!ctx || !ctx->sem) {
        fn();
        return;
    }
    const auto waitVal = ctx->currentValue;
    ctx->currentValue = ctx->sem->next();
    interfaces::WorkerTask task;
    task.fn = std::move(fn);
    task.waitSemaphore = ctx->sem;
    task.waitValue = waitVal;
    task.signalSemaphore = ctx->sem;
    task.signalValue = ctx->currentValue;
    ctx->pool->submit(task);
}

// ── Row-parallel dispatch ──────────────────────────────────────────────

/// Minimum number of rows per tile to avoid excessive dispatch overhead.
constexpr u32 kMinRowsPerTile = 64;

/// Divide @p totalRows into tiles and execute @p fn(startRow, endRow) for
/// each, optionally in parallel via @p ctx.
///
/// When `ctx->sem` is set (**async mode**), tiles are submitted with
/// wait-dependencies on the current timeline value, and a heap-allocated
/// `JobGroup::signalOnComplete()` advances the timeline when all tiles
/// finish.  The function returns immediately — no thread blocks.
///
/// When `ctx->sem` is null (**blocking mode**), the original blocking
/// `JobGroup::wait()` path is used.
template <typename RowFn>
void parallelForRows(u32 totalRows, PipelineContext* ctx, RowFn&& fn) {
    if (!ctx || !ctx->pool || totalRows <= kMinRowsPerTile) {
        fn(0u, totalRows);
        return;
    }

    const u32 threadCount = static_cast<u32>(ctx->pool->threadCount());
    const u32 tilesWanted = std::min(threadCount * 2, totalRows); // ~2 tiles per thread
    const u32 rowsPerTile = std::max(totalRows / tilesWanted, 1u);
    const u32 tileCount = (totalRows + rowsPerTile - 1) / rowsPerTile;

    if (tileCount <= 1) {
        fn(0u, totalRows);
        return;
    }

    if (ctx->sem) {
        // ── Async (non-blocking) path ──────────────────────────────
        // Safety: each parallelForRows call reserves its own (waitVal, signalVal)
        // pair on the timeline.  All tiles in this batch wait on waitVal (the
        // previous step's completion), and signalVal is only signaled when every
        // tile calls done(). This guarantees strict sequential ordering between
        // pipeline steps — no stage's tiles can overlap with a prior stage's.
        // signalOnComplete is registered *before* any task is submitted, so
        // remaining >= tileCount > 0 at that point and the early-signal check
        // inside signalOnComplete cannot fire prematurely.
        const auto waitVal = ctx->currentValue;
        ctx->currentValue = ctx->sem->next();
        const auto signalVal = ctx->currentValue;

        auto jobGroup = std::make_shared<utils::JobGroup>();
        jobGroup->add(tileCount);
        jobGroup->signalOnComplete(ctx->sem, signalVal);

        for (u32 t = 0; t < tileCount; ++t) {
            const u32 startRow = t * rowsPerTile;
            const u32 endRow = std::min(startRow + rowsPerTile, totalRows);
            interfaces::WorkerTask task;
            task.fn = [startRow, endRow, fn, jg = jobGroup]() {
                fn(startRow, endRow);
                jg->done();
            };
            task.waitSemaphore = ctx->sem;
            task.waitValue = waitVal;
            ctx->pool->submit(task);
        }
    } else {
        // ── Blocking path ──────────────────────────────────────────
        utils::JobGroup jobGroup;
        jobGroup.add(tileCount);
        for (u32 t = 0; t < tileCount; ++t) {
            const u32 startRow = t * rowsPerTile;
            const u32 endRow = std::min(startRow + rowsPerTile, totalRows);
            interfaces::WorkerTask task{
                [startRow, endRow, &fn, &jobGroup]() {
                    fn(startRow, endRow);
                    jobGroup.done();
                }
            };
            ctx->pool->submit(task);
        }
        jobGroup.wait();
    }
}

// ── Pipeline ───────────────────────────────────────────────────────────

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

    /// Synchronous execution.  @p ctx may be nullptr for single-threaded.
    MipImage execute(const MipImage& src, u32 targetWidth, u32 targetHeight,
                     PipelineContext* ctx = nullptr) const {
        MipImage input = src;
        for (const auto& stage : preProcess)
            stage(input, ctx);

        MipImage result(targetWidth, targetHeight, input.channels);
        downsample(input, result, ctx);

        for (const auto& stage : postProcess)
            stage(result, ctx);

        return result;
    }

    /// Async execution: flattens the entire pipeline into tasks chained
    /// through a timeline semaphore.  Does **not** block.
    ///
    /// Each stage / filter call internally uses `parallelForRows` in async
    /// mode, so every row-tile is a top-level pool task — no nesting.
    ///
    /// A final copy-task moves the result into @p output and signals the
    /// returned semaphore value.  The caller must wait on that value
    /// before reading *output.
    interfaces::TimelineSemaphore::Value executeAsync(
        const MipImage& src, u32 targetWidth, u32 targetHeight,
        interfaces::WorkerPool* pool,
        interfaces::TimelineSemaphore* sem,
        interfaces::TimelineSemaphore::Value startValue,
        MipImage* output) const
    {
        // Safety: shared_ptr<State> is captured by the final copy task's
        // lambda, which waits on the entire pipeline chain.  This keeps both
        // input and result alive for the duration of all preceding tile tasks.
        // Raw f32* pointers and const MipImage& references used inside tile
        // lambdas point into state->input / state->result, which are stable
        // (no reallocation) because the MipImages are pre-allocated here and
        // never resized.
        struct State {
            MipImage input;
            MipImage result;
        };
        auto state = std::make_shared<State>();
        state->input = src;
        state->result = MipImage(targetWidth, targetHeight, src.channels);

        PipelineContext ctx;
        ctx.pool = pool;
        ctx.sem = sem;
        ctx.currentValue = startValue;

        for (const auto& stage : preProcess)
            stage(state->input, &ctx);

        downsample(state->input, state->result, &ctx);

        for (const auto& stage : postProcess)
            stage(state->result, &ctx);

        // Final task: move the result into the caller's output buffer.
        const auto waitVal = ctx.currentValue;
        const auto signalVal = sem->next();
        interfaces::WorkerTask copyTask;
        copyTask.fn = [state, output]() {
            *output = std::move(state->result);
        };
        copyTask.waitSemaphore = sem;
        copyTask.waitValue = waitVal;
        copyTask.signalSemaphore = sem;
        copyTask.signalValue = signalVal;
        pool->submit(copyTask);

        return signalVal;
    }
};

} // namespace whiteout::textures::mipmap

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Shared constants, tables, and parallel dispatch for the JPEG codec.

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <numbers>

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

namespace whiteout::textures::jpeg {

// ============================================================================
// Block & Component Limits
// ============================================================================

/// Side length of a JPEG DCT block (always 8).
inline constexpr i32 BLOCK_SIZE = 8;

/// Total number of coefficients in one 8×8 block.
inline constexpr i32 BLOCK_PIXELS = BLOCK_SIZE * BLOCK_SIZE; // 64

/// Maximum number of image components supported (JPEG baseline limit is 4).
inline constexpr i32 MAX_COMPONENTS = 4;

/// Maximum number of quantisation / Huffman tables (JPEG spec allows 4 each).
inline constexpr i32 MAX_TABLES = 4;

// ============================================================================
// DCT / IDCT Constants
// ============================================================================
//
// All constants are derived from the DCT basis functions using the notation:
//   c_k = cos(k * pi / 16)
//
// The IDCT uses the Loeffler-Ligtenberg-Moschytz (LLM) factorisation, which
// decomposes the 8-point DCT into cascaded 2-point rotations (11 multiplies,
// 29 adds).  The FDCT uses the Arai-Agui-Nakajima (AAN) butterfly.
//
// References:
//   - IDCT: Loeffler et al., "Practical Fast 1-D DCT Algorithms with 11
//     Multiplications", IEEE ICASSP 1989.
//   - FDCT: Arai, Agui & Nakajima, "A Fast DCT-SQ Scheme for Images",
//     Transactions of IEICE, vol. E-71(11), 1988.
// ============================================================================

inline constexpr f32 PI_F = std::numbers::pi_v<f32>;
inline constexpr f32 SQRT2_F = std::numbers::sqrt2_v<f32>;

/// Constexpr cosine via Taylor series (sufficient precision for f32).
/// Valid for any real x; internally reduces to [0, pi].
constexpr f32 cx_cos(f32 x) {
    // Reduce to [0, 2*pi].
    if (x < 0.0f)
        x = -x;
    while (x > 2.0f * PI_F)
        x -= 2.0f * PI_F;
    // cos(x) = -cos(x - pi) for x in (pi, 2*pi].
    f32 sign = 1.0f;
    if (x > PI_F) {
        x = 2.0f * PI_F - x;
    }
    if (x > PI_F / 2.0f) {
        x = PI_F - x;
        sign = -1.0f;
    }
    // Taylor: cos(x) = 1 - x^2/2! + x^4/4! - ...  (12 terms for f32 precision).
    f32 x2 = x * x;
    f32 term = 1.0f, sum = 1.0f;
    for (i32 n = 1; n <= 12; ++n) {
        term *= -x2 / static_cast<f32>((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sign * sum;
}

/// Constexpr sine via cos identity: sin(x) = cos(pi/2 - x).
constexpr f32 cx_sin(f32 x) {
    return cx_cos(PI_F / 2.0f - x);
}

/// Helper: cos(k * pi / 16).
constexpr f32 dct_cos(i32 k) {
    return cx_cos(static_cast<f32>(k) * PI_F / 16.0f);
}

/// Helper: sin(k * pi / 16).
constexpr f32 dct_sin(i32 k) {
    return cx_sin(static_cast<f32>(k) * PI_F / 16.0f);
}

// -- Shared normalisation ---------------------------------------------------

/// 2-D DCT/IDCT normalisation factor: 1 / BLOCK_SIZE = 1/8.
inline constexpr f32 DCT_2D_NORMALISATION = 1.0f / static_cast<f32>(BLOCK_SIZE);

// -- Even-part rotation (indices 2 & 6) -------------------------------------
//
// Implements the 2-point rotation by 3*pi/8, scaled by sqrt(2):
//   even2 = x2 * sqrt2*c6 - x6 * sqrt2*c2
//   even3 = x2 * sqrt2*c2 + x6 * sqrt2*c6
// Using 3 multiplies:
//   rotation = (x2 + x6) * K
//   even2    = rotation - x6 * A     (where A = sqrt2*(c2 + c6))
//   even3    = rotation + x2 * B     (where B = sqrt2*(c2 - c6))

/// sqrt(2) * cos(6*pi/16)  —  the shared rotation factor.
inline constexpr f32 EVEN_ROTATION_K = SQRT2_F * dct_cos(6);

/// sqrt(2) * (cos(2*pi/16) + cos(6*pi/16))  —  correction for x6.
inline constexpr f32 EVEN_ROTATION_A = SQRT2_F * (dct_cos(2) + dct_cos(6));

/// sqrt(2) * (cos(2*pi/16) - cos(6*pi/16))  —  correction for x2.
inline constexpr f32 EVEN_ROTATION_B = SQRT2_F * (dct_cos(2) - dct_cos(6));

// -- Odd-part scale factor --------------------------------------------------

/// sqrt(2) * cos(3*pi/16)  —  the cross-rotation scale factor.
inline constexpr f32 ODD_SCALE = SQRT2_F * dct_cos(3);

// -- Odd-part per-input coefficients ----------------------------------------
//
// Each input x_k is multiplied by a combined coefficient that folds the
// individual rotation corrections into one multiply per input.

/// sqrt(2) * (-c1 + c3 + c5 - c7)  —  coefficient for x7.
inline constexpr f32 ODD_COEFF_X7 = SQRT2_F * (-dct_cos(1) + dct_cos(3) + dct_cos(5) - dct_cos(7));

/// sqrt(2) * ( c1 + c3 - c5 + c7)  —  coefficient for x5.
inline constexpr f32 ODD_COEFF_X5 = SQRT2_F * (dct_cos(1) + dct_cos(3) - dct_cos(5) + dct_cos(7));

/// sqrt(2) * ( c1 + c3 + c5 - c7)  —  coefficient for x3.
inline constexpr f32 ODD_COEFF_X3 = SQRT2_F * (dct_cos(1) + dct_cos(3) + dct_cos(5) - dct_cos(7));

/// sqrt(2) * ( c1 + c3 - c5 - c7)  —  coefficient for x1.
inline constexpr f32 ODD_COEFF_X1 = SQRT2_F * (dct_cos(1) + dct_cos(3) - dct_cos(5) - dct_cos(7));

// -- Odd-part pair corrections ----------------------------------------------
//
// After the per-input multiplies, pair sums are corrected with these factors.

/// sqrt(2) * (c7 - c3)  —  correction for (x7 + x1).
inline constexpr f32 ODD_PAIR_71 = SQRT2_F * (dct_cos(7) - dct_cos(3));

/// sqrt(2) * (-c1 - c3)  —  correction for (x5 + x3).
inline constexpr f32 ODD_PAIR_53 = SQRT2_F * (-dct_cos(1) - dct_cos(3));

/// sqrt(2) * (-c3 - c5)  —  correction for (x7 + x3).
inline constexpr f32 ODD_PAIR_73 = SQRT2_F * (-dct_cos(3) - dct_cos(5));

/// sqrt(2) * (c5 - c3)  —  correction for (x5 + x1).
inline constexpr f32 ODD_PAIR_51 = SQRT2_F * (dct_cos(5) - dct_cos(3));

// -- Forward DCT (AAN) specific constants -----------------------------------

/// cos(pi/4) = 1 / sqrt(2)  —  used in the FDCT even-part butterfly.
inline constexpr f32 COS_PI_OVER_4 = 1.0f / SQRT2_F;

/// sin(pi/8)  —  used in the FDCT odd-part rotation.
inline constexpr f32 SIN_PI_OVER_8 = dct_sin(2);

/// sqrt(2) * cos(pi/8)  —  used in the FDCT odd-part rotation.
inline constexpr f32 SQRT2_COS_PI_OVER_8 = SQRT2_F * dct_cos(2);

// -- DC level shift ---------------------------------------------------------

/// DC level shift applied before FDCT / after IDCT (JPEG baseline = 128).
inline constexpr f32 DC_LEVEL_SHIFT = 128.0f;

/// DC level shift + 0.5 rounding bias, combined for float→int truncation.
inline constexpr f32 DC_LEVEL_SHIFT_AND_ROUND = DC_LEVEL_SHIFT + 0.5f;

// ============================================================================
// Zig-Zag Scan Order
// ============================================================================

/// JPEG zig-zag scan order.  Maps coefficient index in the encoded stream to
/// the natural (row-major) position inside an 8×8 block.
inline constexpr std::array<u8, BLOCK_PIXELS> ZIGZAG_ORDER = {{
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
}};

// ============================================================================
// JPEG Marker Bytes  (the byte that follows the 0xFF prefix)
// ============================================================================

inline constexpr u8 MARKER_SOF0 = 0xC0; // Baseline DCT
inline constexpr u8 MARKER_SOF2 = 0xC2; // Progressive DCT
inline constexpr u8 MARKER_DHT = 0xC4;  // Define Huffman Table
inline constexpr u8 MARKER_SOI = 0xD8;  // Start Of Image
inline constexpr u8 MARKER_EOI = 0xD9;  // End Of Image
inline constexpr u8 MARKER_SOS = 0xDA;  // Start Of Scan
inline constexpr u8 MARKER_DQT = 0xDB;  // Define Quantization Table
inline constexpr u8 MARKER_DRI = 0xDD;  // Define Restart Interval
inline constexpr u8 MARKER_RST0 = 0xD0; // Restart marker base (RST0–RST7)

// ============================================================================
// Parallel Dispatch Infrastructure
// ============================================================================

/// Carries a pool + optional timeline semaphore through the JPEG pipeline.
/// When sem is non-null, parallel_for_blocks operates in async (non-blocking)
/// mode.  When sem is null, falls back to blocking JobGroup::wait().
struct JpegContext {
    interfaces::WorkerPool* pool = nullptr;
    interfaces::TimelineSemaphore* sem = nullptr;
    interfaces::TimelineSemaphore::Value currentValue = 0;
};

/// Submit a single function as a task chained through the timeline.
/// In synchronous mode (no timeline), runs the function directly.
inline void submitSingleTask(JpegContext* ctx, std::function<void()> fn) {
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

/// Minimum items per work tile to avoid excessive dispatch overhead.
inline constexpr u32 kMinItemsPerTile = 64;

/// Invoke fn(begin, end) for each chunk of [0, count).
/// In async mode (ctx->sem set), tiles are chained through the timeline and
/// the call returns immediately.  In blocking mode, uses JobGroup::wait().
/// No task submitted here ever creates sub-tasks — all nodes are flat.
template <typename Fn>
void parallel_for_blocks(u32 count, JpegContext* ctx, Fn&& fn) {
    if (!ctx || !ctx->pool || ctx->pool->threadCount() <= 1 || count < kMinItemsPerTile) {
        if (ctx && ctx->sem) {
            submitSingleTask(ctx, [count, fn = std::forward<Fn>(fn)]() { fn(0u, count); });
        } else {
            fn(0u, count);
        }
        return;
    }

    const u32 nThreads = static_cast<u32>(ctx->pool->threadCount());
    const u32 tilesWanted = std::min(nThreads * 2, count);
    const u32 itemsPerTile = std::max(count / tilesWanted, 1u);
    const u32 tileCount = (count + itemsPerTile - 1) / itemsPerTile;

    if (tileCount <= 1) {
        if (ctx->sem) {
            submitSingleTask(ctx, [count, fn = std::forward<Fn>(fn)]() { fn(0u, count); });
        } else {
            fn(0u, count);
        }
        return;
    }

    if (ctx->sem) {
        // Async (non-blocking) path — no thread blocks.
        const auto waitVal = ctx->currentValue;
        ctx->currentValue = ctx->sem->next();
        const auto signalVal = ctx->currentValue;

        auto jobGroup = std::make_shared<utils::JobGroup>();
        jobGroup->add(tileCount);
        jobGroup->signalOnComplete(ctx->sem, signalVal);

        for (u32 t = 0; t < tileCount; ++t) {
            const u32 begin = t * itemsPerTile;
            const u32 end = std::min(begin + itemsPerTile, count);
            interfaces::WorkerTask task;
            task.fn = [begin, end, fn, jg = jobGroup]() {
                fn(begin, end);
                jg->done();
            };
            task.waitSemaphore = ctx->sem;
            task.waitValue = waitVal;
            ctx->pool->submit(task);
        }
    } else {
        // Blocking path — caller waits for all tiles.
        const u32 chunk = (count + nThreads - 1) / nThreads;
        utils::JobGroup group;
        for (u32 t = 0; t < nThreads; ++t) {
            const u32 begin = t * chunk;
            const u32 end = std::min(begin + chunk, count);
            if (begin >= end) break;
            group.add(1);
            interfaces::WorkerTask task{
                [begin, end, &fn, &group]() {
                    fn(begin, end);
                    group.done();
                }};
            ctx->pool->submit(task);
        }
        group.wait();
    }
}

/// Dispatch fn(index) for each item in [0, count), one task per item.
/// Designed for coarse-grained work items where each item is expensive.
/// No minimum threshold — uses the full pool even for small counts.
/// In async mode (ctx->sem set), tasks are chained through the timeline.
/// In blocking mode, uses JobGroup::wait().
template <typename Fn>
void parallel_for_tasks(u32 count, JpegContext* ctx, Fn&& fn) {
    if (!ctx || !ctx->pool || ctx->pool->threadCount() <= 1 || count <= 1) {
        if (ctx && ctx->sem) {
            submitSingleTask(ctx, [count, fn = std::forward<Fn>(fn)]() {
                for (u32 i = 0; i < count; ++i) fn(i);
            });
        } else {
            for (u32 i = 0; i < count; ++i) fn(i);
        }
        return;
    }

    if (ctx->sem) {
        // Async (non-blocking) path — chained through timeline.
        const auto waitVal = ctx->currentValue;
        ctx->currentValue = ctx->sem->next();
        const auto signalVal = ctx->currentValue;

        auto jobGroup = std::make_shared<utils::JobGroup>();
        jobGroup->add(count);
        jobGroup->signalOnComplete(ctx->sem, signalVal);

        for (u32 i = 0; i < count; ++i) {
            interfaces::WorkerTask task;
            task.fn = [i, fn, jg = jobGroup]() {
                fn(i);
                jg->done();
            };
            task.waitSemaphore = ctx->sem;
            task.waitValue = waitVal;
            ctx->pool->submit(task);
        }
    } else {
        // Blocking path — caller waits for all tasks.
        utils::JobGroup group;
        for (u32 i = 0; i < count; ++i) {
            group.add(1);
            interfaces::WorkerTask task{
                [i, &fn, &group]() {
                    fn(i);
                    group.done();
                }};
            ctx->pool->submit(task);
        }
        group.wait();
    }
}

} // namespace whiteout::textures::jpeg

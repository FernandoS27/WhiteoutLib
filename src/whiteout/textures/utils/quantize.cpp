// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file quantize.cpp
 * @brief Wu's optimal color quantization — implementation
 *
 * Reference: Xiaolin Wu, "Efficient Statistical Computations for Optimal
 * Color Quantization", Graphics Gems II, 1991.
 *
 * Overview of the algorithm:
 *   1. Build a 33×33×33 3D histogram by quantizing each pixel's RGB to 5 bits
 *      (indices 1–32; index 0 is reserved for prefix-sum sentinels).
 *   2. Compute 3D prefix sums of weight, R/G/B first moments, and the second
 *      moment (R²+G²+B²) over the histogram.
 *   3. Start with a single box covering [1,32]³.
 *   4. Repeatedly find the box with the largest weighted variance and cut it
 *      along the axis/plane that maximally reduces total variance, until the
 *      desired number of boxes (palette entries) is reached.
 *   5. Each box's weighted-mean color becomes its palette entry.
 *   6. Build a 33³ "tag" volume mapping every quantized RGB triplet to its
 *      palette index, enabling O(1) per-pixel lookup.
 *
 * Parallelism strategy:
 *   All parallel work is expressed as a flat DAG of timeline-semaphore-linked
 *   tasks.  No task ever submits sub-tasks and waits — every fan-out and
 *   serial step is a direct top-level DAG node.  Iterative algorithms
 *   (K-means, dither refine) pre-submit all iteration nodes at once; early
 *   termination is handled via a shared atomic convergence flag that makes
 *   subsequent nodes into cheap no-ops.
 */

#include "quantize.h"

#include "blue_noise.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>
#include <whiteout/vector_types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <span>

namespace whiteout::textures::wu {

namespace {

// ============================================================================
// Constants
// ============================================================================

constexpr i32 HIST_SIZE = 33;                                 // per-axis bin count
constexpr u32 HIST_TOTAL = HIST_SIZE * HIST_SIZE * HIST_SIZE; // 35 937
constexpr i32 QUANT_SHIFT = 3; // 8-bit → 5-bit: >> 3 gives 0–31, +1 → 1–32

/// A single 3D moment array (33³ entries).
using MomentArray = std::array<f64, HIST_TOTAL>;

// ============================================================================
// CIELAB perceptual color space
// ============================================================================

/// A color in CIELAB space.
struct Lab {
    f64 L, a, b;
};

/// sRGB [0,255] → linear [0,1] with gamma decode.
inline f64 srgb_to_linear(f64 c) {
    c /= 255.0;
    return (c <= 0.04045) ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/// CIE XYZ helper — the piecewise function used in Lab conversion.
inline f64 xyz_to_lab_f(f64 t) {
    constexpr f64 delta = 6.0 / 29.0;
    constexpr f64 delta2 = delta * delta;
    constexpr f64 delta3 = delta * delta * delta;
    return (t > delta3) ? std::cbrt(t) : (t / (3.0 * delta2)) + (4.0 / 29.0);
}

/// Convert linear RGB [0,1] to CIELAB using D65 illuminant.
inline Lab linear_rgb_to_lab(f64 rl, f64 gl, f64 bl) {
    // Linear RGB → XYZ (sRGB/D65 matrix)
    const f64 x = 0.4124564 * rl + 0.3575761 * gl + 0.1804375 * bl;
    const f64 y = 0.2126729 * rl + 0.7151522 * gl + 0.0721750 * bl;
    const f64 z = 0.0193339 * rl + 0.0950227 * gl + 0.8503491 * bl;

    // Normalize by D65 white point
    constexpr f64 xn = 0.95047;
    constexpr f64 yn = 1.00000;
    constexpr f64 zn = 1.08883;

    const f64 fx = xyz_to_lab_f(x / xn);
    const f64 fy = xyz_to_lab_f(y / yn);
    const f64 fz = xyz_to_lab_f(z / zn);

    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

/// Convert RGB [0,255] to CIELAB using D65 illuminant.
/// @param srgb  If true, apply sRGB gamma decode; otherwise treat as linear.
inline Lab rgb_to_lab(f64 r, f64 g, f64 b, bool srgb) {
    f64 rl, gl, bl;
    if (srgb) {
        rl = srgb_to_linear(r);
        gl = srgb_to_linear(g);
        bl = srgb_to_linear(b);
    } else {
        rl = r / 255.0;
        gl = g / 255.0;
        bl = b / 255.0;
    }
    return linear_rgb_to_lab(rl, gl, bl);
}

/// Squared CIELAB distance (ΔE²) — the perceptual error metric.
inline f64 lab_dist_sq(const Lab& a, const Lab& b) {
    const f64 dL = a.L - b.L;
    const f64 da = a.a - b.a;
    const f64 db = a.b - b.b;
    return dL * dL + da * da + db * db;
}

/// Pre-computed [0,255] → linear lookup table for fast Lab conversion.
/// When srgb=true, applies sRGB gamma decode; otherwise just divides by 255.
struct ToLinearLut {
    std::array<f64, 256> table;
    explicit ToLinearLut(bool srgb) {
        if (srgb) {
            for (u32 i = 0; i < 256; ++i)
                table[i] = srgb_to_linear(static_cast<f64>(i));
        } else {
            for (u32 i = 0; i < 256; ++i)
                table[i] = static_cast<f64>(i) / 255.0;
        }
    }
};

/// Fast RGB→Lab using a pre-computed →linear LUT.
inline Lab rgb_to_lab_fast(u8 r, u8 g, u8 b, const ToLinearLut& lut) {
    return linear_rgb_to_lab(lut.table[r], lut.table[g], lut.table[b]);
}

/// Read pixel @p i from an RGBA8 buffer and convert directly to CIELAB.
inline Lab pixel_lab(const u8* rgba, u32 i, const ToLinearLut& lut) {
    return rgb_to_lab_fast(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], lut);
}

// Flatten (r, g, b) into a linear index.  r, g, b ∈ [0, 32].
inline u32 idx(i32 r, i32 g, i32 b) {
    return static_cast<u32>((r * HIST_SIZE + g) * HIST_SIZE + b);
}

/// Axis used for box splitting.
enum class Axis { R, G, B };

/// Quantize an 8-bit channel value to a histogram bin index in [1, 32].
inline i32 quantize_channel(u8 val) {
    return (static_cast<i32>(val) >> QUANT_SHIFT) + 1;
}

// ============================================================================
// Shared helper types and functions
// ============================================================================

/// Floating-point RGB triplet for palette centroid arithmetic.
using ColorF = Vector3d;

/// Overload accepting a ColorF (Vector3d with channels in x/y/z).
inline Lab rgb_to_lab(const ColorF& c, bool srgb) {
    return rgb_to_lab(c.x, c.y, c.z, srgb);
}

/// Unpack a BGRX palette entry to floating-point RGB.
inline ColorF unpack_color(u32 packed) {
    return {static_cast<f64>((packed >> 16) & 0xFF),
            static_cast<f64>((packed >> 8) & 0xFF),
            static_cast<f64>(packed & 0xFF)};
}

/// Pack floating-point RGB (with rounding) into a BGRX palette entry.
inline u32 pack_color(const ColorF& c) {
    const auto ri = static_cast<u8>(std::clamp(c.x + 0.5, 0.0, 255.0));
    const auto gi = static_cast<u8>(std::clamp(c.y + 0.5, 0.0, 255.0));
    const auto bi = static_cast<u8>(std::clamp(c.z + 0.5, 0.0, 255.0));
    return (static_cast<u32>(ri) << 16) | (static_cast<u32>(gi) << 8) | static_cast<u32>(bi);
}

/// Unpack palette entries into a vector of ColorF.
std::vector<ColorF> unpack_palette(const std::array<u32, MAX_COLORS>& palette, u32 count) {
    std::vector<ColorF> colors(count);
    for (u32 i = 0; i < count; ++i)
        colors[i] = unpack_color(palette[i]);
    return colors;
}

/// Write ColorF entries back to a packed palette array.
void pack_palette(const ColorF* colors, u32 count, std::array<u32, MAX_COLORS>& palette) {
    for (u32 i = 0; i < count; ++i)
        palette[i] = pack_color(colors[i]);
}

/// Convert a ColorF array to CIELAB.
std::vector<Lab> colors_to_labs(const ColorF* colors, u32 count, bool srgb) {
    std::vector<Lab> labs(count);
    for (u32 c = 0; c < count; ++c)
        labs[c] = rgb_to_lab(colors[c], srgb);
    return labs;
}

/// Convert packed palette to CIELAB (unpacks then delegates to colors_to_labs).
std::vector<Lab> palette_to_labs(const std::array<u32, MAX_COLORS>& palette, u32 count, bool srgb) {
    const auto colors = unpack_palette(palette, count);
    return colors_to_labs(colors.data(), count, srgb);
}

/// Find the index of the nearest palette entry in CIELAB space.
inline u32 find_nearest(const Lab& target, std::span<const Lab> pal_labs) {
    f64 best_dist = std::numeric_limits<f64>::max();
    u32 best_idx = 0;
    for (u32 c = 0; c < static_cast<u32>(pal_labs.size()); ++c) {
        const f64 dist = lab_dist_sq(target, pal_labs[c]);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = c;
        }
    }
    return best_idx;
}

/// Find the pixel with the highest minimum-distance to any palette entry.
/// Used for starved cluster reinitialization.
u32 find_worst_error_pixel(std::span<const Lab> pixel_labs, std::span<const Lab> pal_labs) {
    f64 worst_err = -1.0;
    u32 worst_px = 0;
    for (u32 p = 0; p < static_cast<u32>(pixel_labs.size()); ++p) {
        const f64 err = lab_dist_sq(pixel_labs[p], pal_labs[find_nearest(pixel_labs[p], pal_labs)]);
        if (err > worst_err) {
            worst_err = err;
            worst_px = p;
        }
    }
    return worst_px;
}

/// Dither-related constants derived from a strength value.
struct DitherParams {
    f64 spread; ///< strength * 32.0  (~1 Wu histogram bin)
    f64 norm;   ///< 1.0 / (BLUE_NOISE_TOTAL - 1)
};

inline DitherParams make_dither_params(f32 strength) {
    return {static_cast<f64>(strength) * 32.0, 1.0 / static_cast<f64>(BLUE_NOISE_TOTAL - 1)};
}

// ============================================================================
// DAG scheduling context — analogous to mipmap::PipelineContext
// ============================================================================

/// Carries a pool + optional timeline semaphore through the quantize pipeline.
/// When sem is non-null, parallel_for_chunks operates in async (non-blocking)
/// mode.  When sem is null, falls back to blocking JobGroup::wait().
struct QuantizeContext {
    interfaces::WorkerPool* pool = nullptr;
    interfaces::TimelineSemaphore* sem = nullptr;
    interfaces::TimelineSemaphore::Value currentValue = 0;
};

// ============================================================================
// Parallel for utility — splits [0, count) into chunks
// ============================================================================

/// Minimum items per work tile to avoid excessive dispatch overhead.
constexpr u32 kMinItemsPerTile = 1024;

/// Invoke fn(begin, end) for each chunk of [0, count).
/// In async mode (ctx->sem set), tiles are chained through the timeline and
/// the call returns immediately.  In blocking mode, uses JobGroup::wait().
/// No task submitted here ever creates sub-tasks — all nodes are flat.
template <typename Fn>
void parallel_for_chunks(u32 count, QuantizeContext* ctx, Fn&& fn) {
    if (!ctx || !ctx->pool || ctx->pool->threadCount() <= 1 || count < kMinItemsPerTile) {
        fn(0u, count);
        return;
    }

    const u32 nThreads = static_cast<u32>(ctx->pool->threadCount());
    const u32 tilesWanted = std::min(nThreads * 2, count);
    const u32 itemsPerTile = std::max(count / tilesWanted, 1u);
    const u32 tileCount = (count + itemsPerTile - 1) / itemsPerTile;

    if (tileCount <= 1) {
        fn(0u, count);
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

/// Submit a single function as a task chained through the timeline.
/// In synchronous mode (no timeline), runs the function directly.
inline void submitSingleTask(QuantizeContext* ctx, std::function<void()> fn) {
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

/// Compute the blue-noise dither offset for a pixel at (x, y).
inline f64 dither_offset(u32 x, u32 y, std::span<const u16, BLUE_NOISE_TOTAL> noise,
                         const DitherParams& dp) {
    const u32 nx = x % BLUE_NOISE_SIZE;
    const u32 ny = y % BLUE_NOISE_SIZE;
    return (static_cast<f64>(noise[ny * BLUE_NOISE_SIZE + nx]) * dp.norm - 0.5) * dp.spread;
}

/// Clamp each channel of a ColorF to [0, 255].
inline ColorF clamp_color(const ColorF& c) {
    return {std::clamp(c.x, 0.0, 255.0), std::clamp(c.y, 0.0, 255.0), std::clamp(c.z, 0.0, 255.0)};
}

/// Read the RGB channels of pixel @p i from an RGBA8 buffer.
inline ColorF read_pixel_rgb(const u8* rgba, u32 i) {
    return {static_cast<f64>(rgba[i * 4 + 0]),
            static_cast<f64>(rgba[i * 4 + 1]),
            static_cast<f64>(rgba[i * 4 + 2])};
}

// ============================================================================
// Internal structures
// ============================================================================

/// A box (cube) in quantized RGB space.  Inclusive on both ends.
struct Box {
    i32 r0 = 0, r1 = 0;
    i32 g0 = 0, g1 = 0;
    i32 b0 = 0, b1 = 0;
    i32 vol = 0; // cached: (r1-r0)*(g1-g0)*(b1-b0)
};

/// The five 3D moment arrays, each of size HIST_TOTAL.
struct Moments {
    MomentArray weight; // pixel count (weight)
    MomentArray sum_r;  // Σ R
    MomentArray sum_g;  // Σ G
    MomentArray sum_b;  // Σ B
    MomentArray sum_sq; // Σ (R² + G² + B²)
};

// ============================================================================
// Step 1 — Build histogram (serial single-thread version)
// ============================================================================

void build_histogram(const u8* rgba, u32 pixel_count, Moments& moments) {
    std::memset(&moments, 0, sizeof(Moments));

    for (u32 i = 0; i < pixel_count; ++i) {
        const f64 red = rgba[i * 4 + 0];
        const f64 green = rgba[i * 4 + 1];
        const f64 blue = rgba[i * 4 + 2];

        const i32 ri = quantize_channel(rgba[i * 4 + 0]);
        const i32 gi = quantize_channel(rgba[i * 4 + 1]);
        const i32 bi = quantize_channel(rgba[i * 4 + 2]);

        const u32 bin = idx(ri, gi, bi);
        moments.weight[bin] += 1.0;
        moments.sum_r[bin] += red;
        moments.sum_g[bin] += green;
        moments.sum_b[bin] += blue;
        moments.sum_sq[bin] += red * red + green * green + blue * blue;
    }
}

/// Build histogram with per-thread local accumulation, then merge.
/// @param threadMoments  Pre-allocated vector of nThreads Moments (zeroed by caller).
/// @param out            Merged result (zeroed by caller).
void build_histogram_range(const u8* rgba, u32 begin, u32 end, Moments& out) {
    for (u32 i = begin; i < end; ++i) {
        const f64 red = rgba[i * 4 + 0];
        const f64 green = rgba[i * 4 + 1];
        const f64 blue = rgba[i * 4 + 2];

        const i32 ri = quantize_channel(rgba[i * 4 + 0]);
        const i32 gi = quantize_channel(rgba[i * 4 + 1]);
        const i32 bi = quantize_channel(rgba[i * 4 + 2]);

        const u32 bin = idx(ri, gi, bi);
        out.weight[bin] += 1.0;
        out.sum_r[bin] += red;
        out.sum_g[bin] += green;
        out.sum_b[bin] += blue;
        out.sum_sq[bin] += red * red + green * green + blue * blue;
    }
}

/// Merge nThreads per-thread Moments into a single output.
void merge_moments(const std::vector<Moments>& sources, Moments& out) {
    std::memset(&out, 0, sizeof(Moments));
    for (const auto& src : sources) {
        for (u32 i = 0; i < HIST_TOTAL; ++i) {
            out.weight[i] += src.weight[i];
            out.sum_r[i] += src.sum_r[i];
            out.sum_g[i] += src.sum_g[i];
            out.sum_b[i] += src.sum_b[i];
            out.sum_sq[i] += src.sum_sq[i];
        }
    }
}

// ============================================================================
// Step 2 — Compute 3D prefix sums (cumulative moments)
// ============================================================================

void compute_cumulative_moments(Moments& moments) {
    // For each fixed r, compute 2D prefix sums over (g, b), then accumulate
    // along the r axis.
    for (i32 r = 1; r < HIST_SIZE; ++r) {
        // 2D area sums over g × b for this r-slice.
        std::array<f64, HIST_SIZE> area_weight{};
        std::array<f64, HIST_SIZE> area_sum_r{};
        std::array<f64, HIST_SIZE> area_sum_g{};
        std::array<f64, HIST_SIZE> area_sum_b{};
        std::array<f64, HIST_SIZE> area_sum_sq{};

        for (i32 g = 1; g < HIST_SIZE; ++g) {
            f64 line_weight = 0, line_sum_r = 0, line_sum_g = 0, line_sum_b = 0, line_sum_sq = 0;
            for (i32 b = 1; b < HIST_SIZE; ++b) {
                const u32 bin = idx(r, g, b);
                line_weight += moments.weight[bin];
                line_sum_r += moments.sum_r[bin];
                line_sum_g += moments.sum_g[bin];
                line_sum_b += moments.sum_b[bin];
                line_sum_sq += moments.sum_sq[bin];

                area_weight[b] += line_weight;
                area_sum_r[b] += line_sum_r;
                area_sum_g[b] += line_sum_g;
                area_sum_b[b] += line_sum_b;
                area_sum_sq[b] += line_sum_sq;

                // Add cumulative sum from previous r-slice.
                const u32 prev = idx(r - 1, g, b);
                moments.weight[bin] = moments.weight[prev] + area_weight[b];
                moments.sum_r[bin] = moments.sum_r[prev] + area_sum_r[b];
                moments.sum_g[bin] = moments.sum_g[prev] + area_sum_g[b];
                moments.sum_b[bin] = moments.sum_b[prev] + area_sum_b[b];
                moments.sum_sq[bin] = moments.sum_sq[prev] + area_sum_sq[b];
            }
        }
    }
}

// ============================================================================
// Inclusive-box volume queries using the 3D prefix sums
// ============================================================================

// Evaluate the inclusion-exclusion formula for a single moment array.
// The box covers [r0+1 .. r1], [g0+1 .. g1], [b0+1 .. b1] in the
// prefix-sum coordinate system.
inline f64 box_sum(const MomentArray& arr, const Box& box) {
    // Standard 3D prefix-sum inclusion-exclusion (8 terms).
    return arr[idx(box.r1, box.g1, box.b1)] - arr[idx(box.r1, box.g1, box.b0)] -
           arr[idx(box.r1, box.g0, box.b1)] + arr[idx(box.r1, box.g0, box.b0)] -
           arr[idx(box.r0, box.g1, box.b1)] + arr[idx(box.r0, box.g1, box.b0)] +
           arr[idx(box.r0, box.g0, box.b1)] - arr[idx(box.r0, box.g0, box.b0)];
}

/// Evaluate inclusion-exclusion for the "lower half" of a box cut along @p axis at @p pos.
/// Equivalent to box_sum(arr, sub) where sub has its upper bound on @p axis set to @p pos.
inline f64 half_box_sum(const MomentArray& arr, const Box& box, Axis axis, i32 pos) {
    Box sub = box;
    switch (axis) {
    case Axis::R:
        sub.r1 = pos;
        break;
    case Axis::G:
        sub.g1 = pos;
        break;
    case Axis::B:
        sub.b1 = pos;
        break;
    }
    return box_sum(arr, sub);
}

// ============================================================================
// Step 3 — Variance of a box
// ============================================================================

/// Weighted variance of all pixels inside a box.
f64 variance(const Moments& moments, const Box& box) {
    const f64 weight = box_sum(moments.weight, box);
    if (weight <= 0.0)
        return 0.0;
    const f64 total_r = box_sum(moments.sum_r, box);
    const f64 total_g = box_sum(moments.sum_g, box);
    const f64 total_b = box_sum(moments.sum_b, box);
    const f64 total_sq = box_sum(moments.sum_sq, box);
    // Var = Σ(x²) - (Σx)²/n
    return total_sq - (total_r * total_r + total_g * total_g + total_b * total_b) / weight;
}

// ============================================================================
// Step 4 — Find the optimal cut plane along one axis
// ============================================================================

/// Try every plane perpendicular to @p axis and return the one that maximises
/// the total variance *reduction*.  Returns the achieved value through
/// @p out_max and the cut position through the return value.
/// Returns -1 if no valid cut exists.
i32 maximize_on_axis(const Moments& moments, const Box& box, Axis axis, f64 total_weight,
                     f64 total_r, f64 total_g, f64 total_b, f64* out_max) {
    i32 lo = 0, hi = 0;
    switch (axis) {
    case Axis::R:
        lo = box.r0;
        hi = box.r1;
        break;
    case Axis::G:
        lo = box.g0;
        hi = box.g1;
        break;
    case Axis::B:
        lo = box.b0;
        hi = box.b1;
        break;
    }

    f64 best_score = 0.0;
    i32 best_pos = -1;

    for (i32 pos = lo + 1; pos < hi; ++pos) {
        const f64 lo_weight = half_box_sum(moments.weight, box, axis, pos);
        if (lo_weight <= 0.0)
            continue;
        const f64 lo_r = half_box_sum(moments.sum_r, box, axis, pos);
        const f64 lo_g = half_box_sum(moments.sum_g, box, axis, pos);
        const f64 lo_b = half_box_sum(moments.sum_b, box, axis, pos);

        const f64 hi_weight = total_weight - lo_weight;
        if (hi_weight <= 0.0)
            continue;
        const f64 hi_r = total_r - lo_r;
        const f64 hi_g = total_g - lo_g;
        const f64 hi_b = total_b - lo_b;

        const f64 score = (lo_r * lo_r + lo_g * lo_g + lo_b * lo_b) / lo_weight +
                          (hi_r * hi_r + hi_g * hi_g + hi_b * hi_b) / hi_weight;
        if (score > best_score) {
            best_score = score;
            best_pos = pos;
        }
    }
    *out_max = best_score;
    return best_pos;
}

// ============================================================================
// Step 5 — Cut a box into two along the best axis
// ============================================================================

/// Attempt to cut box @p b along the axis that maximally reduces variance.
/// On success, @p b is shrunk to the lower half and @p b2 receives the upper
/// half; returns true.  Returns false if the box cannot be split.
bool cut(const Moments& moments, Box& lower, Box& upper) {
    const f64 weight = box_sum(moments.weight, lower);
    if (weight <= 0.0)
        return false;
    const f64 total_r = box_sum(moments.sum_r, lower);
    const f64 total_g = box_sum(moments.sum_g, lower);
    const f64 total_b = box_sum(moments.sum_b, lower);

    f64 score_r = 0.0, score_g = 0.0, score_b = 0.0;
    const i32 pos_r =
        maximize_on_axis(moments, lower, Axis::R, weight, total_r, total_g, total_b, &score_r);
    const i32 pos_g =
        maximize_on_axis(moments, lower, Axis::G, weight, total_r, total_g, total_b, &score_g);
    const i32 pos_b =
        maximize_on_axis(moments, lower, Axis::B, weight, total_r, total_g, total_b, &score_b);

    // Pick the axis with the largest variance reduction.
    Axis best_axis;
    i32 best_pos;

    if (score_r >= score_g && score_r >= score_b) {
        if (pos_r < 0)
            return false;
        best_axis = Axis::R;
        best_pos = pos_r;
    } else if (score_g >= score_r && score_g >= score_b) {
        if (pos_g < 0)
            return false;
        best_axis = Axis::G;
        best_pos = pos_g;
    } else {
        if (pos_b < 0)
            return false;
        best_axis = Axis::B;
        best_pos = pos_b;
    }

    // Split: lower keeps the low side, upper gets the high side.
    upper = lower;
    switch (best_axis) {
    case Axis::R:
        lower.r1 = best_pos;
        upper.r0 = best_pos;
        break;
    case Axis::G:
        lower.g1 = best_pos;
        upper.g0 = best_pos;
        break;
    case Axis::B:
        lower.b1 = best_pos;
        upper.b0 = best_pos;
        break;
    }
    lower.vol = (lower.r1 - lower.r0) * (lower.g1 - lower.g0) * (lower.b1 - lower.b0);
    upper.vol = (upper.r1 - upper.r0) * (upper.g1 - upper.g0) * (upper.b1 - upper.b0);
    return true;
}

// ============================================================================
// Shared state for async quantization pipeline
// ============================================================================

/// All mutable state shared across DAG tasks.  Heap-allocated and held via
/// shared_ptr so that async task lambdas keep the state alive.
struct QuantizeState {
    // --- Config (immutable after construction) ---
    const u8* rgba = nullptr;
    u32 pixelCount = 0;
    u32 maxColors = MAX_COLORS;
    u32 kmeansIterations = 10;
    bool srgb = false;
    bool ditherAwareEnabled = false;
    u32 ditherWidth = 0;
    u32 ditherHeight = 0;
    f32 ditherStrength = 0.0f;
    u32 ditherIterations = 5;
    u32 nThreads = 1;

    // --- Histogram ---
    std::unique_ptr<Moments> moments;
    std::vector<Moments> threadMoments; // per-thread local histograms

    // --- Box splitting ---
    std::vector<Box> boxes;
    std::vector<f64> variances;
    u32 boxCount = 0;

    // --- Result fields (written incrementally by DAG tasks) ---
    // palette and color_count are public on QuantizeResult, so accessed directly.
    // tag_ and srgb_ are private, so we store them here and copy at the end.
    std::array<u32, MAX_COLORS> palette{};
    u32 colorCount = 0;
    std::vector<u8> tag;
    bool resultSrgb = false;

    // --- K-means shared buffers ---
    ToLinearLut lut{false};
    std::vector<ColorF> centroids;
    std::vector<Lab> centroidLabs;
    std::vector<Lab> pixelLabs;
    std::vector<u8> assignments;
    u32 kmeansWeightThreshold = 1;
    std::shared_ptr<std::atomic<bool>> kmeansConverged;

    // --- Tag volume ---
    std::vector<Lab> palLabs; // pre-computed palette Labs for tag fan-out tiles

    // --- Dither refine shared buffers ---
    std::vector<ColorF> palColors;
    std::vector<f64> ditherOffsets;
    f64 ditherPrevError = std::numeric_limits<f64>::max();
    f64 ditherLr = 1.0;
    u32 ditherStarveThreshold = 1;
    DitherParams ditherParams{};
    std::shared_ptr<std::atomic<bool>> ditherConverged;
};

// ============================================================================
// Tag volume build — maps quantized color → palette index
// ============================================================================

/// Build the tag volume: each bin center maps to the nearest palette entry.
/// Parallelized by r-slices (32 slices of 33×33 work each).
void build_tag_volume(std::shared_ptr<QuantizeState> state, QuantizeContext* ctx) {
    state->tag.assign(HIST_TOTAL, 0);
    state->palLabs = palette_to_labs(state->palette, state->colorCount, state->srgb);

    constexpr u32 rSlices = HIST_SIZE - 1; // 32
    parallel_for_chunks(rSlices, ctx, [state](u32 begin, u32 end) {
        const auto& pal_labs = state->palLabs;
        auto& tag = state->tag;

        for (u32 s = begin; s < end; ++s) {
            const i32 ri = static_cast<i32>(s) + 1;
            const f64 rc = (ri - 1) * 8.0 + 4.0;
            for (i32 gi = 1; gi < HIST_SIZE; ++gi) {
                const f64 gc = (gi - 1) * 8.0 + 4.0;
                for (i32 bi = 1; bi < HIST_SIZE; ++bi) {
                    const f64 bc = (bi - 1) * 8.0 + 4.0;
                    tag[idx(ri, gi, bi)] =
                        static_cast<u8>(find_nearest(rgb_to_lab(rc, gc, bc, false), pal_labs));
                }
            }
        }
    });
}

} // anonymous namespace

// ============================================================================
// Quantizer::Impl — hidden state
// ============================================================================

struct Quantizer::Impl {
    u32 maxColors = MAX_COLORS;
    u32 kmeansIterations = 10;
    bool srgbInput = false;
    bool ditherAware = false;
    u32 ditherWidth = 0;
    u32 ditherHeight = 0;
    f32 ditherStrength = 0.0f;
    u32 ditherIterations = 5;
    interfaces::WorkerPool* pool = nullptr;
};

// ============================================================================
// Quantizer — construction and builder methods
// ============================================================================

Quantizer::Quantizer() : m_impl(std::make_unique<Impl>()) {}
Quantizer::~Quantizer() = default;
Quantizer::Quantizer(Quantizer&&) noexcept = default;
Quantizer& Quantizer::operator=(Quantizer&&) noexcept = default;

Quantizer& Quantizer::maxColors(u32 count) {
    m_impl->maxColors = std::clamp(count, 1u, MAX_COLORS);
    return *this;
}

Quantizer& Quantizer::kmeansIterations(u32 iterations) {
    m_impl->kmeansIterations = iterations;
    return *this;
}

Quantizer& Quantizer::srgbInput(bool srgb) {
    m_impl->srgbInput = srgb;
    return *this;
}

Quantizer& Quantizer::ditherAware(u32 width, u32 height, f32 strength, u32 iterations) {
    m_impl->ditherAware = true;
    m_impl->ditherWidth = width;
    m_impl->ditherHeight = height;
    m_impl->ditherStrength = strength;
    m_impl->ditherIterations = iterations;
    return *this;
}

Quantizer& Quantizer::workerPool(interfaces::WorkerPool* pool) {
    m_impl->pool = pool;
    return *this;
}

// ============================================================================
// Internal: build the full quantization DAG
// ============================================================================

namespace {

/// Submit all DAG nodes for the quantize pipeline.  When ctx->sem is set,
/// all nodes are non-blocking and chained through the timeline.  When
/// ctx->sem is null, each node runs synchronously inline.
///
/// CRITICAL INVARIANT: No task submitted here ever creates sub-tasks.
/// Every parallel fan-out and serial step is a direct top-level DAG node.
void build_quantize_dag(std::shared_ptr<QuantizeState> state, QuantizeContext* ctx) {
    const u32 pixelCount = state->pixelCount;
    const u32 nThreads = state->nThreads;

    // ── 1. Histogram build (per-thread fan-out) ────────────────────────
    if (nThreads > 1 && ctx->pool) {
        state->threadMoments.resize(nThreads);
        for (auto& m : state->threadMoments)
            std::memset(&m, 0, sizeof(Moments));

        // Fan-out: each tile accumulates into its own Moments.
        // We manually tile by thread index so each thread gets exactly one Moments.
        const u32 chunk = (pixelCount + nThreads - 1) / nThreads;

        if (ctx->sem) {
            const auto waitVal = ctx->currentValue;
            ctx->currentValue = ctx->sem->next();
            const auto signalVal = ctx->currentValue;

            auto jobGroup = std::make_shared<utils::JobGroup>();
            jobGroup->add(nThreads);
            jobGroup->signalOnComplete(ctx->sem, signalVal);

            for (u32 t = 0; t < nThreads; ++t) {
                const u32 begin = t * chunk;
                const u32 end = std::min(begin + chunk, pixelCount);
                if (begin >= end) {
                    jobGroup->done();
                    continue;
                }
                interfaces::WorkerTask task;
                task.fn = [state, t, begin, end, jg = jobGroup]() {
                    build_histogram_range(state->rgba, begin, end, state->threadMoments[t]);
                    jg->done();
                };
                task.waitSemaphore = ctx->sem;
                task.waitValue = waitVal;
                ctx->pool->submit(task);
            }
        } else {
            // Blocking path
            utils::JobGroup group;
            for (u32 t = 0; t < nThreads; ++t) {
                const u32 begin = t * chunk;
                const u32 end = std::min(begin + chunk, pixelCount);
                if (begin >= end) continue;
                group.add(1);
                interfaces::WorkerTask task{
                    [&, t, begin, end]() {
                        build_histogram_range(state->rgba, begin, end, state->threadMoments[t]);
                        group.done();
                    }};
                ctx->pool->submit(task);
            }
            group.wait();
        }

        // ── 2. Histogram merge ─────────────────────────────────────────
        submitSingleTask(ctx, [state]() {
            merge_moments(state->threadMoments, *state->moments);
            state->threadMoments.clear(); // free ~1.4 MB/thread
            state->threadMoments.shrink_to_fit();
        });
    } else {
        // Serial: single-threaded histogram build + no merge needed
        submitSingleTask(ctx, [state]() {
            build_histogram(state->rgba, state->pixelCount, *state->moments);
        });
    }

    // ── 3. Prefix sums ────────────────────────────────────────────────
    submitSingleTask(ctx, [state]() {
        compute_cumulative_moments(*state->moments);
    });

    // ── 4. Box splitting + palette extraction ──────────────────────────
    submitSingleTask(ctx, [state]() {
        auto& moments = *state->moments;
        auto& boxes = state->boxes;
        auto& variances = state->variances;
        const u32 maxColors = state->maxColors;

        boxes.resize(maxColors);
        variances.assign(maxColors, 0.0);

        boxes[0].r0 = boxes[0].g0 = boxes[0].b0 = 0;
        boxes[0].r1 = boxes[0].g1 = boxes[0].b1 = HIST_SIZE - 1;
        boxes[0].vol = (HIST_SIZE - 1) * (HIST_SIZE - 1) * (HIST_SIZE - 1);

        u32 boxCount = 1;

        for (u32 i = 1; i < maxColors; ++i) {
            if (boxCount >= 2) {
                variances[boxCount - 2] = variance(moments, boxes[boxCount - 2]);
                variances[boxCount - 1] = variance(moments, boxes[boxCount - 1]);
            } else {
                variances[0] = variance(moments, boxes[0]);
            }

            u32 best_idx = 0;
            f64 best_variance = variances[0];
            for (u32 j = 1; j < boxCount; ++j) {
                if (variances[j] > best_variance) {
                    best_variance = variances[j];
                    best_idx = j;
                }
            }

            if (best_variance <= 0.0)
                break;

            if (!cut(moments, boxes[best_idx], boxes[boxCount]))
                break;

            variances[best_idx] = variance(moments, boxes[best_idx]);
            variances[boxCount] = variance(moments, boxes[boxCount]);
            ++boxCount;
        }

        // Compute palette colors (weighted centroids).
        for (u32 i = 0; i < boxCount; ++i) {
            const f64 weight = box_sum(moments.weight, boxes[i]);
            if (weight > 0.0) {
                state->palette[i] =
                    pack_color({box_sum(moments.sum_r, boxes[i]) / weight,
                                box_sum(moments.sum_g, boxes[i]) / weight,
                                box_sum(moments.sum_b, boxes[i]) / weight});
            }
        }
        state->colorCount = boxCount;
        state->boxCount = boxCount;

        // Free moments — no longer needed after box splitting.
        state->moments.reset();
    });

    // ── 5. K-means refinement (flattened iteration DAG) ────────────────
    // Each iteration is TWO top-level DAG nodes: assignment fan-out + centroid update.
    // All nodes are pre-submitted. Early termination via shared atomic flag.

    // 5a. Allocate K-means buffers + pre-compute pixel Labs.
    submitSingleTask(ctx, [state]() {
        state->kmeansConverged = std::make_shared<std::atomic<bool>>(false);

        const u32 colorCount = state->colorCount;
        if (colorCount <= 1 || state->pixelCount == 0 || state->kmeansIterations == 0) {
            state->kmeansConverged->store(true, std::memory_order_release);
            return;
        }

        state->lut = ToLinearLut(state->srgb);
        state->centroids = unpack_palette(state->palette, colorCount);
        state->centroidLabs = colors_to_labs(state->centroids.data(), colorCount, state->srgb);
        state->pixelLabs.resize(state->pixelCount);
        state->assignments.resize(state->pixelCount);
        state->kmeansWeightThreshold =
            std::max(state->pixelCount / (colorCount * 16u), 1u);
    });

    // 5b. Fan-out: compute pixel Labs.
    parallel_for_chunks(pixelCount, ctx, [state](u32 begin, u32 end) {
        if (state->kmeansConverged->load(std::memory_order_acquire))
            return;
        for (u32 p = begin; p < end; ++p)
            state->pixelLabs[p] = pixel_lab(state->rgba, p, state->lut);
    });

    // 5c. Pre-submit all K-means iteration nodes.
    // We use the builder's kmeansIterations value; the actual state->kmeansIterations
    // is identical (set during QuantizeState construction).
    for (u32 iter = 0; iter < state->kmeansIterations; ++iter) {
        // 5c-i. Assignment fan-out: write to disjoint assignments[] ranges;
        // the serial centroid update reads them to compute sums.
        parallel_for_chunks(pixelCount, ctx, [state](u32 begin, u32 end) {
            if (state->kmeansConverged->load(std::memory_order_acquire))
                return;

            for (u32 p = begin; p < end; ++p) {
                state->assignments[p] = static_cast<u8>(
                    find_nearest(state->pixelLabs[p], state->centroidLabs));
            }
        });

        // 5c-ii. Serial centroid update.
        submitSingleTask(ctx, [state]() {
            if (state->kmeansConverged->load(std::memory_order_acquire))
                return;

            const u32 colorCount = state->colorCount;
            if (colorCount <= 1)
                return;

            const u32 pc = state->pixelCount;

            // Accumulate sums from assignments.
            std::vector<ColorF> sums(colorCount, ColorF{0, 0, 0});
            std::vector<u32> counts(colorCount, 0u);
            for (u32 p = 0; p < pc; ++p) {
                const u32 c = state->assignments[p];
                sums[c] += read_pixel_rgb(state->rgba, p);
                counts[c]++;
            }

            // Recompute centroids.
            bool changed = false;
            for (u32 c = 0; c < colorCount; ++c) {
                if (counts[c] > 0) {
                    const auto newCentroid = sums[c] / static_cast<f64>(counts[c]);
                    if (pack_color(newCentroid) != pack_color(state->centroids[c]))
                        changed = true;
                    state->centroids[c] = newCentroid;
                    state->centroidLabs[c] = rgb_to_lab(newCentroid, state->srgb);
                }
            }

            // Stabilization: reinitialize starved clusters.
            for (u32 c = 0; c < colorCount; ++c) {
                if (counts[c] >= state->kmeansWeightThreshold)
                    continue;
                f64 worst_err = -1.0;
                u32 worst_px = 0;
                for (u32 p = 0; p < pc; ++p) {
                    const f64 err =
                        lab_dist_sq(state->pixelLabs[p],
                                    state->centroidLabs[state->assignments[p]]);
                    if (err > worst_err) {
                        worst_err = err;
                        worst_px = p;
                    }
                }
                state->centroids[c] = read_pixel_rgb(state->rgba, worst_px);
                state->centroidLabs[c] = state->pixelLabs[worst_px];
                changed = true;
            }

            // If nothing changed at the packed-color level, converge early.
            if (!changed)
                state->kmeansConverged->store(true, std::memory_order_release);
        });
    }

    // 5d. Pack K-means result into palette.
    submitSingleTask(ctx, [state]() {
        const u32 colorCount = state->colorCount;
        if (colorCount > 1 && !state->centroids.empty())
            pack_palette(state->centroids.data(), colorCount, state->palette);
    });

    // ── 6. Build tag volume (fan-out by r-slices) ──────────────────────
    submitSingleTask(ctx, [state]() {
        state->tag.assign(HIST_TOTAL, 0);
        state->palLabs = palette_to_labs(state->palette, state->colorCount, state->srgb);
        state->resultSrgb = state->srgb;
    });

    // Fan-out: 32 r-slices of independent work.
    constexpr u32 rSlices = HIST_SIZE - 1; // 32
    parallel_for_chunks(rSlices, ctx, [state](u32 begin, u32 end) {
        const auto& pal_labs = state->palLabs;
        auto& tag = state->tag;

        for (u32 s = begin; s < end; ++s) {
            const i32 ri = static_cast<i32>(s) + 1;
            const f64 rc = (ri - 1) * 8.0 + 4.0;
            for (i32 gi = 1; gi < HIST_SIZE; ++gi) {
                const f64 gc = (gi - 1) * 8.0 + 4.0;
                for (i32 bi = 1; bi < HIST_SIZE; ++bi) {
                    const f64 bc = (bi - 1) * 8.0 + 4.0;
                    tag[idx(ri, gi, bi)] =
                        static_cast<u8>(find_nearest(rgb_to_lab(rc, gc, bc, false), pal_labs));
                }
            }
        }
    });

    // ── 7. Dither-aware refinement (flattened iteration DAG) ───────────
    if (!state->ditherAwareEnabled || state->ditherWidth == 0 ||
        state->ditherHeight == 0 || state->ditherStrength <= 0.0f) {
        // No dither refinement — skip.
        return;
    }

    const u32 ditherPixelCount = std::min(state->ditherWidth * state->ditherHeight, state->pixelCount);

    // 7a. Allocate dither buffers + pre-compute pixel Labs + offsets.
    submitSingleTask(ctx, [state, ditherPixelCount]() {
        state->ditherConverged = std::make_shared<std::atomic<bool>>(false);

        const u32 colorCount = state->colorCount;
        if (colorCount <= 1) {
            state->ditherConverged->store(true, std::memory_order_release);
            return;
        }

        state->palColors = unpack_palette(state->palette, colorCount);

        state->ditherParams = make_dither_params(state->ditherStrength);
        state->ditherOffsets.resize(ditherPixelCount);

        state->ditherStarveThreshold =
            std::max(ditherPixelCount / (colorCount * 16u), 1u);

        state->ditherPrevError = std::numeric_limits<f64>::max();
        state->ditherLr = 1.0;

        // Ensure pixelLabs covers dither pixel count (may differ from k-means count).
        if (state->pixelLabs.size() < ditherPixelCount)
            state->pixelLabs.resize(ditherPixelCount);
        if (state->assignments.size() < ditherPixelCount)
            state->assignments.resize(ditherPixelCount);
    });

    // 7b. Fan-out: compute dither offsets + fill any pixelLabs not covered by K-means.
    parallel_for_chunks(ditherPixelCount, ctx, [state](u32 begin, u32 end) {
        if (state->ditherConverged->load(std::memory_order_acquire))
            return;
        const auto noise = blueNoiseMap();
        const u32 w = state->ditherWidth;
        const u32 kmeansCount = state->pixelCount; // pixelLabs[0..kmeansCount) already filled
        for (u32 i = begin; i < end; ++i) {
            state->ditherOffsets[i] = dither_offset(i % w, i / w, noise, state->ditherParams);
            if (i >= kmeansCount)
                state->pixelLabs[i] = pixel_lab(state->rgba, i, state->lut);
        }
    });

    // 7c. Pre-submit all dither iteration nodes.
    for (u32 iter = 0; iter < state->ditherIterations; ++iter) {
        // 7c-i. Serial: prepare per-iteration state (pal_labs, clear accumulators).
        submitSingleTask(ctx, [state]() {
            if (state->ditherConverged->load(std::memory_order_acquire))
                return;
            if (state->colorCount <= 1) {
                state->ditherConverged->store(true, std::memory_order_release);
                return;
            }

            // Recompute centroid Labs from current palette colors.
            state->centroidLabs =
                colors_to_labs(state->palColors.data(), state->colorCount, state->srgb);
        });

        // 7c-ii. Assignment fan-out: simulate dithering, assign pixels.
        // Each tile writes to disjoint assignments[] range + its own accumulator.
        // We use the approach of writing assignments[] per tile, then the serial
        // centroid update reads all of them.
        parallel_for_chunks(ditherPixelCount, ctx, [state](u32 begin, u32 end) {
            if (state->ditherConverged->load(std::memory_order_acquire))
                return;

            for (u32 i = begin; i < end; ++i) {
                const auto px = read_pixel_rgb(state->rgba, i);
                const f64 d = state->ditherOffsets[i];
                const auto dithered = clamp_color(px + ColorF{d, d, d});
                state->assignments[i] = static_cast<u8>(
                    find_nearest(rgb_to_lab(dithered, state->srgb), state->centroidLabs));
            }
        });

        // 7c-iii. Serial centroid update + convergence check.
        submitSingleTask(ctx, [state]() {
            if (state->ditherConverged->load(std::memory_order_acquire))
                return;

            const u32 colorCount = state->colorCount;
            const u32 pc = std::min(state->ditherWidth * state->ditherHeight, state->pixelCount);

            // Accumulate sums + error from assignments.
            std::vector<ColorF> accum(colorCount, ColorF{0, 0, 0});
            std::vector<u32> counts(colorCount, 0u);
            f64 totalError = 0.0;
            for (u32 i = 0; i < pc; ++i) {
                const u32 best = state->assignments[i];
                totalError += lab_dist_sq(state->pixelLabs[i], state->centroidLabs[best]);
                accum[best] += read_pixel_rgb(state->rgba, i);
                counts[best]++;
            }

            // Early termination if error is no longer decreasing.
            if (totalError >= state->ditherPrevError) {
                state->ditherConverged->store(true, std::memory_order_release);
                return;
            }
            state->ditherPrevError = totalError;

            // Move each palette entry toward the mean of original pixels.
            constexpr f64 lr_decay = 0.7;
            for (u32 c = 0; c < colorCount; ++c) {
                if (counts[c] == 0)
                    continue;
                const ColorF target = accum[c] / static_cast<f64>(counts[c]);
                state->palColors[c] =
                    clamp_color(ColorF::lerp(state->palColors[c], target, state->ditherLr));
            }

            // Reinitialize starved clusters.
            const auto pal_labs =
                colors_to_labs(state->palColors.data(), colorCount, state->srgb);
            for (u32 c = 0; c < colorCount; ++c) {
                if (counts[c] >= state->ditherStarveThreshold)
                    continue;
                const u32 worst_px = find_worst_error_pixel(state->pixelLabs, pal_labs);
                state->palColors[c] = read_pixel_rgb(state->rgba, worst_px);
            }

            state->ditherLr *= lr_decay;
        });
    }

    // 7d. Pack dither-refined palette + rebuild tag volume.
    submitSingleTask(ctx, [state]() {
        if (!state->palColors.empty() && state->colorCount > 1)
            pack_palette(state->palColors.data(), state->colorCount, state->palette);
    });

    // Rebuild tag volume after dither refinement.
    submitSingleTask(ctx, [state]() {
        state->tag.assign(HIST_TOTAL, 0);
        state->palLabs = palette_to_labs(state->palette, state->colorCount, state->srgb);
    });

    parallel_for_chunks(rSlices, ctx, [state](u32 begin, u32 end) {
        const auto& pal_labs = state->palLabs;
        auto& tag = state->tag;

        for (u32 s = begin; s < end; ++s) {
            const i32 ri = static_cast<i32>(s) + 1;
            const f64 rc = (ri - 1) * 8.0 + 4.0;
            for (i32 gi = 1; gi < HIST_SIZE; ++gi) {
                const f64 gc = (gi - 1) * 8.0 + 4.0;
                for (i32 bi = 1; bi < HIST_SIZE; ++bi) {
                    const f64 bc = (bi - 1) * 8.0 + 4.0;
                    tag[idx(ri, gi, bi)] =
                        static_cast<u8>(find_nearest(rgb_to_lab(rc, gc, bc, false), pal_labs));
                }
            }
        }
    });
}

} // anonymous namespace

// ============================================================================
// Quantizer::quantize — main entry point (blocking)
// ============================================================================

QuantizeResult Quantizer::quantize(const u8* rgba, u32 pixel_count) const {
    return quantize(rgba, pixel_count, m_impl->pool);
}

QuantizeResult Quantizer::quantize(const u8* rgba, u32 pixel_count,
                                   interfaces::WorkerPool* pool) const {
    if (pixel_count == 0 || m_impl->maxColors == 0) {
        QuantizeResult result;
        result.color_count = 0;
        return result;
    }

    const auto& d = *m_impl;

    // Build shared state.
    auto state = std::make_shared<QuantizeState>();
    state->rgba = rgba;
    state->pixelCount = pixel_count;
    state->maxColors = std::min(d.maxColors, MAX_COLORS);
    state->kmeansIterations = d.kmeansIterations;
    state->srgb = d.srgbInput;
    state->ditherAwareEnabled = d.ditherAware;
    state->ditherWidth = d.ditherWidth;
    state->ditherHeight = d.ditherHeight;
    state->ditherStrength = d.ditherStrength;
    state->ditherIterations = d.ditherIterations;
    state->moments = std::make_unique<Moments>();
    state->nThreads = (pool && pool->threadCount() > 1) ? static_cast<u32>(pool->threadCount()) : 1;

    if (pool && pool->threadCount() > 1) {
        auto sem = pool->createTimelineSemaphore();
        if (sem) {
            // Async DAG path — submit all nodes, then wait.
            const auto startVal = sem->next();
            sem->signal(startVal);
            QuantizeContext ctx{pool, sem.get(), startVal};

            build_quantize_dag(state, &ctx);

            sem->wait(ctx.currentValue);

            QuantizeResult result;
            result.palette = state->palette;
            result.color_count = state->colorCount;
            result.tag_ = std::move(state->tag);
            result.srgb_ = state->resultSrgb;
            return result;
        }
    }

    // Serial fallback (no pool, single thread, or no semaphore support).
    QuantizeContext ctx{pool, nullptr, 0};
    build_quantize_dag(state, &ctx);

    QuantizeResult result;
    result.palette = state->palette;
    result.color_count = state->colorCount;
    result.tag_ = std::move(state->tag);
    result.srgb_ = state->resultSrgb;
    return result;
}

// ============================================================================
// Quantizer::quantizeAsync — non-blocking entry point
// ============================================================================

interfaces::TimelineSemaphore::Value Quantizer::quantizeAsync(
    const u8* rgba, u32 pixel_count, interfaces::WorkerPool* pool,
    interfaces::TimelineSemaphore* sem, interfaces::TimelineSemaphore::Value startValue,
    QuantizeResult* outResult) const {

    if (pixel_count == 0 || m_impl->maxColors == 0) {
        outResult->color_count = 0;
        return startValue;
    }

    const auto& d = *m_impl;

    auto state = std::make_shared<QuantizeState>();
    state->rgba = rgba;
    state->pixelCount = pixel_count;
    state->maxColors = std::min(d.maxColors, MAX_COLORS);
    state->kmeansIterations = d.kmeansIterations;
    state->srgb = d.srgbInput;
    state->ditherAwareEnabled = d.ditherAware;
    state->ditherWidth = d.ditherWidth;
    state->ditherHeight = d.ditherHeight;
    state->ditherStrength = d.ditherStrength;
    state->ditherIterations = d.ditherIterations;
    state->moments = std::make_unique<Moments>();
    state->nThreads = (pool && pool->threadCount() > 1) ? static_cast<u32>(pool->threadCount()) : 1;

    QuantizeContext ctx{pool, sem, startValue};
    build_quantize_dag(state, &ctx);

    // Final task: copy result to caller's output pointer.
    submitSingleTask(&ctx, [state, outResult]() {
        outResult->palette = state->palette;
        outResult->color_count = state->colorCount;
        outResult->tag_ = std::move(state->tag);
        outResult->srgb_ = state->resultSrgb;
    });

    return ctx.currentValue;
}

// ============================================================================
// QuantizeResult — pixel mapping
// ============================================================================

u8 QuantizeResult::mapPixel(u8 r, u8 g, u8 b) const {
    if (tag_.empty())
        return 0;
    return tag_[idx(quantize_channel(r), quantize_channel(g), quantize_channel(b))];
}

void QuantizeResult::mapPixels(const u8* rgba, u32 pixel_count, u8* out_indices) const {
    if (tag_.empty()) {
        std::memset(out_indices, 0, pixel_count);
        return;
    }
    for (u32 i = 0; i < pixel_count; ++i) {
        out_indices[i] = tag_[idx(quantize_channel(rgba[i * 4 + 0]),
                                  quantize_channel(rgba[i * 4 + 1]),
                                  quantize_channel(rgba[i * 4 + 2]))];
    }
}

void QuantizeResult::mapPixelsDithered(const u8* rgba, u32 width, u32 height, f32 strength,
                                       u8* out_indices) const {
    if (color_count <= 1 || width == 0 || height == 0) {
        mapPixels(rgba, width * height, out_indices);
        return;
    }

    const auto noise = blueNoiseMap();
    const auto pal_labs = palette_to_labs(palette, color_count, srgb_);
    const auto dp = make_dither_params(strength);

    const u32 pixel_count = width * height;
    for (u32 i = 0; i < pixel_count; ++i) {
        const f64 d = dither_offset(i % width, i / width, noise, dp);
        const auto dithered = clamp_color(read_pixel_rgb(rgba, i) + ColorF{d, d, d});
        out_indices[i] = static_cast<u8>(
            find_nearest(rgb_to_lab(dithered, srgb_), pal_labs));
    }
}

// ============================================================================
// Dither-aware refinement — kept as public method for standalone use
// ============================================================================

void QuantizeResult::refineDitherAware(const u8* rgba, u32 width, u32 height, f32 strength,
                                       u32 iterations, interfaces::WorkerPool* pool) {
    if (color_count <= 1 || width == 0 || height == 0 || iterations == 0 || strength <= 0.0f)
        return;

    const u32 pixel_count = width * height;
    const auto noise = blueNoiseMap();
    const ToLinearLut lut(srgb_);
    const auto dp = make_dither_params(strength);

    auto pal_colors = unpack_palette(palette, color_count);

    // Pre-compute Lab for all original pixels and per-pixel dither offsets.
    std::vector<Lab> pixel_labs(pixel_count);
    std::vector<f64> offsets(pixel_count);

    QuantizeContext ctx{pool, nullptr, 0};
    parallel_for_chunks(pixel_count, &ctx, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            pixel_labs[i] = pixel_lab(rgba, i, lut);
            offsets[i] = dither_offset(i % width, i / width, noise, dp);
        }
    });

    const u32 starve_threshold = std::max(pixel_count / (color_count * 16u), 1u);

    const bool usePool = pool && pool->threadCount() > 1 && pixel_count >= 1024;
    const u32 nThreads = usePool ? static_cast<u32>(pool->threadCount()) : 1;

    // Per-thread accumulators.
    std::vector<std::vector<ColorF>> thread_accum(nThreads, std::vector<ColorF>(color_count));
    std::vector<std::vector<u32>> thread_counts(nThreads, std::vector<u32>(color_count));
    std::vector<f64> thread_errors(nThreads, 0.0);

    // Descending learning rate: start at 1.0, decay geometrically.
    f64 lr = 1.0;
    constexpr f64 lr_decay = 0.7;
    f64 prev_error = std::numeric_limits<f64>::max();

    for (u32 iter = 0; iter < iterations; ++iter) {
        const auto pal_labs = colors_to_labs(pal_colors.data(), color_count, srgb_);

        for (u32 t = 0; t < nThreads; ++t) {
            std::fill(thread_accum[t].begin(), thread_accum[t].end(), ColorF{0, 0, 0});
            std::fill(thread_counts[t].begin(), thread_counts[t].end(), 0u);
            thread_errors[t] = 0.0;
        }

        // Simulate dithering: assign pixels based on dithered color,
        // but accumulate original pixel color for the gradient step.
        if (usePool) {
            const u32 chunk = (pixel_count + nThreads - 1) / nThreads;
            utils::JobGroup group;
            for (u32 t = 0; t < nThreads; ++t) {
                const u32 begin = t * chunk;
                const u32 end = std::min(begin + chunk, pixel_count);
                if (begin >= end) break;
                group.add(1);
                interfaces::WorkerTask task{
                    [&, t, begin, end]() {
                        f64 local_error = 0.0;
                        for (u32 i = begin; i < end; ++i) {
                            const auto px = read_pixel_rgb(rgba, i);
                            const auto dithered = clamp_color(px + ColorF{offsets[i], offsets[i], offsets[i]});
                            const u32 best = find_nearest(rgb_to_lab(dithered, srgb_), pal_labs);
                            local_error += lab_dist_sq(pixel_labs[i], pal_labs[best]);
                            thread_accum[t][best] += px;
                            thread_counts[t][best]++;
                        }
                        thread_errors[t] = local_error;
                        group.done();
                    }};
                pool->submit(task);
            }
            group.wait();
        } else {
            for (u32 i = 0; i < pixel_count; ++i) {
                const auto px = read_pixel_rgb(rgba, i);
                const auto dithered = clamp_color(px + ColorF{offsets[i], offsets[i], offsets[i]});
                const u32 best = find_nearest(rgb_to_lab(dithered, srgb_), pal_labs);
                thread_errors[0] += lab_dist_sq(pixel_labs[i], pal_labs[best]);
                thread_accum[0][best] += px;
                thread_counts[0][best]++;
            }
        }

        // Reduce per-thread accumulators.
        std::vector<ColorF> accum(color_count, ColorF{0, 0, 0});
        std::vector<u32> counts(color_count, 0u);
        f64 total_error = 0.0;
        for (u32 t = 0; t < nThreads; ++t) {
            total_error += thread_errors[t];
            for (u32 c = 0; c < color_count; ++c) {
                accum[c] += thread_accum[t][c];
                counts[c] += thread_counts[t][c];
            }
        }

        // Early termination if error is no longer decreasing.
        if (total_error >= prev_error)
            break;
        prev_error = total_error;

        // Move each palette entry toward the mean of original pixel colors
        // assigned to it under dithering.
        for (u32 c = 0; c < color_count; ++c) {
            if (counts[c] == 0)
                continue;
            const ColorF target = accum[c] / static_cast<f64>(counts[c]);
            pal_colors[c] = clamp_color(ColorF::lerp(pal_colors[c], target, lr));
        }

        // Reinitialize starved clusters from the highest-error pixel.
        for (u32 c = 0; c < color_count; ++c) {
            if (counts[c] >= starve_threshold)
                continue;
            const u32 worst_px = find_worst_error_pixel(pixel_labs, pal_labs);
            pal_colors[c] = read_pixel_rgb(rgba, worst_px);
        }

        lr *= lr_decay;
    }

    pack_palette(pal_colors.data(), color_count, palette);

    // Rebuild tag volume.
    auto state = std::make_shared<QuantizeState>();
    state->palette = palette;
    state->colorCount = color_count;
    state->srgb = srgb_;
    build_tag_volume(state, &ctx);
    tag_ = std::move(state->tag);
}

} // namespace whiteout::textures::wu

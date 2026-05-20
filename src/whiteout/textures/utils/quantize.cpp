// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file quantize.cpp
 * @brief Wu's optimal color quantization -- implementation
 *
 * Reference: Xiaolin Wu, "Efficient Statistical Computations for Optimal
 * Color Quantization", Graphics Gems II, 1991.
 *
 * Overview of the algorithm:
 *   1. Build a 33x33x33 3D histogram by quantizing each pixel's RGB to 5 bits
 *      (indices 1-32; index 0 is reserved for prefix-sum sentinels).
 *   2. Compute 3D prefix sums of weight, R/G/B first moments, and the second
 *      moment (R^2+G^2+B^2) over the histogram.
 *   3. Start with a single box covering [1,32]^3.
 *   4. Repeatedly find the box with the largest weighted variance and cut it
 *      along the axis/plane that maximally reduces total variance, until the
 *      desired number of boxes (palette entries) is reached.
 *   5. Each box's weighted-mean color becomes its palette entry.
 *   6. Build a 33^3 "tag" volume mapping every quantized RGB triplet to its
 *      palette index, enabling O(1) per-pixel lookup.
 *
 * Parallelism strategy:
 *   All parallel work is expressed as a flat DAG of timeline-semaphore-linked
 *   tasks.  No task ever submits sub-tasks and waits -- every fan-out and
 *   serial step is a direct top-level DAG node.  Iterative algorithms
 *   (K-means, dither refine) pre-submit all iteration nodes at once; early
 *   termination is handled via a shared atomic convergence flag that makes
 *   subsequent nodes into cheap no-ops.
 *
 * Performance notes:
 *   - Palette nearest-neighbor search uses SoA (Structure-of-Arrays) layout
 *     with f32 CIELAB components (LabPalette).  The two-pass design (compute
 *     all distances, then argmin) has no loop-carried dependencies in the
 *     first pass, enabling wide SIMD auto-vectorization (8-wide with AVX2).
 *   - Per-pixel Lab values use f32 (12 bytes vs 24), halving cache pressure
 *     and doubling the effective SIMD throughput.
 *   - Bin-center Lab values for the 32^3 tag volume are lazily computed once
 *     and cached in a process-wide static, eliminating repeated pow/cbrt
 *     calls on tag-volume rebuilds.
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
#include <numeric>
#include <span>

namespace whiteout::textures::wu {

namespace {

// ============================================================================
// Constants
// ============================================================================

constexpr i32 HIST_SIZE = 33;
constexpr u32 HIST_TOTAL = HIST_SIZE * HIST_SIZE * HIST_SIZE; // 35 937
constexpr i32 QUANT_SHIFT = 3; // 8-bit -> 5-bit: >> 3 gives 0-31, +1 -> 1-32

using MomentArray = std::array<f64, HIST_TOTAL>;

// ============================================================================
// CIELAB perceptual color type  (f32 for cache efficiency + wider SIMD)
// ============================================================================

struct Lab {
    f32 L, a, b;
};

// ============================================================================
// sRGB / CIELAB conversion  (internal math in f64, stored as f32)
// ============================================================================

inline f64 srgb_to_linear(f64 c) {
    c /= 255.0;
    return (c <= 0.04045) ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

inline f64 xyz_to_lab_f(f64 t) {
    constexpr f64 delta = 6.0 / 29.0;
    constexpr f64 delta2 = delta * delta;
    constexpr f64 delta3 = delta * delta * delta;
    return (t > delta3) ? std::cbrt(t) : (t / (3.0 * delta2)) + (4.0 / 29.0);
}

/// Convert linear RGB [0,1] to CIELAB (D65).  Internal f64, result f32.
inline Lab linear_rgb_to_lab(f64 rl, f64 gl, f64 bl) {
    const f64 x = 0.4124564 * rl + 0.3575761 * gl + 0.1804375 * bl;
    const f64 y = 0.2126729 * rl + 0.7151522 * gl + 0.0721750 * bl;
    const f64 z = 0.0193339 * rl + 0.0950227 * gl + 0.8503491 * bl;

    constexpr f64 xn = 0.95047, yn = 1.00000, zn = 1.08883;
    const f64 fx = xyz_to_lab_f(x / xn);
    const f64 fy = xyz_to_lab_f(y / yn);
    const f64 fz = xyz_to_lab_f(z / zn);

    return {static_cast<f32>(116.0 * fy - 16.0), static_cast<f32>(500.0 * (fx - fy)),
            static_cast<f32>(200.0 * (fy - fz))};
}

/// Convert RGB [0,255] to CIELAB.  @p srgb applies gamma decode when true.
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

/// Pre-computed [0,255] -> linear LUT (f32 -- sufficient for Lab precision).
struct ToLinearLut {
    std::array<f32, 256> table{};
    explicit ToLinearLut(bool srgb) {
        for (u32 i = 0; i < 256; ++i) {
            if (srgb)
                table[i] = static_cast<f32>(srgb_to_linear(static_cast<f64>(i)));
            else
                table[i] = static_cast<f32>(i) / 255.0f;
        }
    }
};

/// Fast RGB->Lab via pre-computed ->linear LUT.
inline Lab rgb_to_lab_lut(u8 r, u8 g, u8 b, const ToLinearLut& lut) {
    return linear_rgb_to_lab(lut.table[r], lut.table[g], lut.table[b]);
}

/// Read pixel @p i from RGBA8 buffer and convert to CIELAB.
inline Lab pixel_to_lab(const u8* rgba, u32 i, const ToLinearLut& lut) {
    const u32 base = i * 4;
    return rgb_to_lab_lut(rgba[base], rgba[base + 1], rgba[base + 2], lut);
}

// ============================================================================
// Bin index helpers
// ============================================================================

inline u32 idx(i32 r, i32 g, i32 b) {
    return static_cast<u32>((r * HIST_SIZE + g) * HIST_SIZE + b);
}

inline i32 quantize_channel(u8 val) {
    return (static_cast<i32>(val) >> QUANT_SHIFT) + 1;
}

enum class Axis { R, G, B };

// ============================================================================
// SoA palette for vectorizable nearest-neighbor search
//
// Stores palette Lab components in three separate 64-byte-aligned arrays so
// the distance-computation loop processes contiguous f32 streams.  The two-
// pass design (compute distances, then argmin) eliminates loop-carried deps
// in the hot first pass, enabling 8-wide AVX2 auto-vectorization.
// ============================================================================

struct LabPalette {
    alignas(64) f32 L[MAX_COLORS]{};
    alignas(64) f32 a[MAX_COLORS]{};
    alignas(64) f32 b[MAX_COLORS]{};
    u32 count = 0;

    void set(u32 i, Lab lab) {
        L[i] = lab.L;
        a[i] = lab.a;
        b[i] = lab.b;
    }

    /// Two-pass nearest-neighbor search.
    /// Pass 1: compute all squared distances (no loop-carried deps).
    /// Pass 2: argmin reduction.
    u32 findNearest(Lab target) const {
        alignas(64) f32 dists[MAX_COLORS];
        const f32 tL = target.L, ta = target.a, tb = target.b;

        for (u32 c = 0; c < count; ++c) {
            const f32 dL = tL - L[c];
            const f32 da = ta - a[c];
            const f32 db = tb - b[c];
            dists[c] = dL * dL + da * da + db * db;
        }

        u32 best = 0;
        f32 bestDist = dists[0];
        for (u32 c = 1; c < count; ++c) {
            if (dists[c] < bestDist) {
                bestDist = dists[c];
                best = c;
            }
        }
        return best;
    }

    /// Squared CIELAB distance from @p target to palette entry @p i.
    f32 distSq(u32 i, Lab target) const {
        const f32 dL = target.L - L[i];
        const f32 da = target.a - a[i];
        const f32 db = target.b - b[i];
        return dL * dL + da * da + db * db;
    }
};

// ============================================================================
// Precomputed bin-center Lab values for the tag volume
//
// 32^3 bin centers are fixed ({4,12,...,252} per axis).  Lab conversion
// involves pow/cbrt -- we cache once (lazily, thread-safe) and reuse.
// ============================================================================

const std::array<Lab, 32 * 32 * 32>& get_bin_center_labs(bool srgb) {
    auto build = [](bool s) {
        std::array<Lab, 32 * 32 * 32> result{};
        for (i32 r = 0; r < 32; ++r) {
            const f64 rc = r * 8.0 + 4.0;
            for (i32 g = 0; g < 32; ++g) {
                const f64 gc = g * 8.0 + 4.0;
                for (i32 b = 0; b < 32; ++b) {
                    result[(r * 32 + g) * 32 + b] = rgb_to_lab(rc, gc, b * 8.0 + 4.0, s);
                }
            }
        }
        return result;
    };
    static const auto linear_labs = build(false);
    static const auto srgb_labs = build(true);
    return srgb ? srgb_labs : linear_labs;
}

// ============================================================================
// Color helpers
// ============================================================================

using ColorF = Vector3d;

inline Lab colorf_to_lab(const ColorF& c, bool srgb) {
    return rgb_to_lab(c.x, c.y, c.z, srgb);
}

inline ColorF unpack_color(u32 packed) {
    return {static_cast<f64>((packed >> 16) & 0xFF), static_cast<f64>((packed >> 8) & 0xFF),
            static_cast<f64>(packed & 0xFF)};
}

inline u32 pack_color(const ColorF& c) {
    const auto ri = static_cast<u8>(std::clamp(c.x + 0.5, 0.0, 255.0));
    const auto gi = static_cast<u8>(std::clamp(c.y + 0.5, 0.0, 255.0));
    const auto bi = static_cast<u8>(std::clamp(c.z + 0.5, 0.0, 255.0));
    return (static_cast<u32>(ri) << 16) | (static_cast<u32>(gi) << 8) | static_cast<u32>(bi);
}

inline ColorF read_pixel_rgb(const u8* rgba, u32 i) {
    const u32 base = i * 4;
    return {static_cast<f64>(rgba[base]), static_cast<f64>(rgba[base + 1]),
            static_cast<f64>(rgba[base + 2])};
}

inline ColorF clamp_color(const ColorF& c) {
    return {std::clamp(c.x, 0.0, 255.0), std::clamp(c.y, 0.0, 255.0), std::clamp(c.z, 0.0, 255.0)};
}

std::vector<ColorF> unpack_palette(const std::array<u32, MAX_COLORS>& palette, u32 count) {
    std::vector<ColorF> colors(count);
    for (u32 i = 0; i < count; ++i)
        colors[i] = unpack_color(palette[i]);
    return colors;
}

void pack_palette(const ColorF* colors, u32 count, std::array<u32, MAX_COLORS>& palette) {
    for (u32 i = 0; i < count; ++i)
        palette[i] = pack_color(colors[i]);
}

/// Build a LabPalette from packed palette entries.
LabPalette make_lab_palette(const std::array<u32, MAX_COLORS>& palette, u32 count, bool srgb) {
    LabPalette pal{};
    pal.count = count;
    for (u32 i = 0; i < count; ++i) {
        const auto c = unpack_color(palette[i]);
        pal.set(i, rgb_to_lab(c.x, c.y, c.z, srgb));
    }
    return pal;
}

/// Build a LabPalette from a ColorF array.
LabPalette make_lab_palette(const ColorF* colors, u32 count, bool srgb) {
    LabPalette pal{};
    pal.count = count;
    for (u32 i = 0; i < count; ++i)
        pal.set(i, rgb_to_lab(colors[i].x, colors[i].y, colors[i].z, srgb));
    return pal;
}

/// Find the pixel with the highest min-distance to any palette entry.
u32 find_worst_error_pixel(const Lab* pixel_labs, u32 pixel_count, const LabPalette& pal) {
    f32 worst_err = -1.0f;
    u32 worst_px = 0;
    for (u32 p = 0; p < pixel_count; ++p) {
        const u32 nearest = pal.findNearest(pixel_labs[p]);
        const f32 err = pal.distSq(nearest, pixel_labs[p]);
        if (err > worst_err) {
            worst_err = err;
            worst_px = p;
        }
    }
    return worst_px;
}

// ============================================================================
// Dither helpers
// ============================================================================

struct DitherParams {
    f64 spread;
    f64 norm;
};

inline DitherParams make_dither_params(f32 strength) {
    return {static_cast<f64>(strength) * 32.0, 1.0 / static_cast<f64>(BLUE_NOISE_TOTAL - 1)};
}

inline f64 dither_offset(u32 x, u32 y, std::span<const u16, BLUE_NOISE_TOTAL> noise,
                         const DitherParams& dp) {
    const u32 nx = x % BLUE_NOISE_SIZE;
    const u32 ny = y % BLUE_NOISE_SIZE;
    return (static_cast<f64>(noise[ny * BLUE_NOISE_SIZE + nx]) * dp.norm - 0.5) * dp.spread;
}

// ============================================================================
// DAG scheduling context
// ============================================================================

struct QuantizeContext {
    interfaces::WorkerPool* pool = nullptr;
    interfaces::TimelineSemaphore* sem = nullptr;
    interfaces::TimelineSemaphore::Value currentValue = 0;
};

// ============================================================================
// Submit a single task through the timeline
// (Defined before parallel_for_chunks for two-phase template lookup.)
// ============================================================================

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

// ============================================================================
// Parallel for utility -- splits [0, count) into chunks
// ============================================================================

constexpr u32 kMinItemsPerTile = 1024;

template <typename Fn>
void parallel_for_chunks(u32 count, QuantizeContext* ctx, Fn&& fn) {
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
        const u32 chunk = (count + nThreads - 1) / nThreads;
        utils::JobGroup group;
        for (u32 t = 0; t < nThreads; ++t) {
            const u32 begin = t * chunk;
            const u32 end = std::min(begin + chunk, count);
            if (begin >= end)
                break;
            group.add(1);
            interfaces::WorkerTask const task{[begin, end, &fn, &group]() {
                fn(begin, end);
                group.done();
            }};
            ctx->pool->submit(task);
        }
        group.wait();
    }
}

// ============================================================================
// Internal structures
// ============================================================================

struct Box {
    i32 r0 = 0, r1 = 0;
    i32 g0 = 0, g1 = 0;
    i32 b0 = 0, b1 = 0;
    i32 vol = 0;
};

struct Moments {
    MomentArray weight;
    MomentArray sum_r;
    MomentArray sum_g;
    MomentArray sum_b;
    MomentArray sum_sq;
};

// ============================================================================
// Step 1 -- Build histogram  (reads each pixel exactly once)
// ============================================================================

void build_histogram(const u8* rgba, u32 pixel_count, Moments& moments) {
    std::memset(&moments, 0, sizeof(Moments));
    for (u32 i = 0; i < pixel_count; ++i) {
        const u32 base = i * 4;
        const u8 r8 = rgba[base + 0];
        const u8 g8 = rgba[base + 1];
        const u8 b8 = rgba[base + 2];

        const f64 r = r8, g = g8, b = b8;
        const u32 bin =
            idx((r8 >> QUANT_SHIFT) + 1, (g8 >> QUANT_SHIFT) + 1, (b8 >> QUANT_SHIFT) + 1);

        moments.weight[bin] += 1.0;
        moments.sum_r[bin] += r;
        moments.sum_g[bin] += g;
        moments.sum_b[bin] += b;
        moments.sum_sq[bin] += r * r + g * g + b * b;
    }
}

void build_histogram_range(const u8* rgba, u32 begin, u32 end, Moments& out) {
    for (u32 i = begin; i < end; ++i) {
        const u32 base = i * 4;
        const u8 r8 = rgba[base + 0];
        const u8 g8 = rgba[base + 1];
        const u8 b8 = rgba[base + 2];

        const f64 r = r8, g = g8, b = b8;
        const u32 bin =
            idx((r8 >> QUANT_SHIFT) + 1, (g8 >> QUANT_SHIFT) + 1, (b8 >> QUANT_SHIFT) + 1);

        out.weight[bin] += 1.0;
        out.sum_r[bin] += r;
        out.sum_g[bin] += g;
        out.sum_b[bin] += b;
        out.sum_sq[bin] += r * r + g * g + b * b;
    }
}

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
// Step 2 -- 3D prefix sums
// ============================================================================

void compute_cumulative_moments(Moments& moments) {
    for (i32 r = 1; r < HIST_SIZE; ++r) {
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
// Box volume queries (3D prefix-sum inclusion-exclusion)
// ============================================================================

inline f64 box_sum(const MomentArray& arr, const Box& box) {
    return arr[idx(box.r1, box.g1, box.b1)] - arr[idx(box.r1, box.g1, box.b0)] -
           arr[idx(box.r1, box.g0, box.b1)] + arr[idx(box.r1, box.g0, box.b0)] -
           arr[idx(box.r0, box.g1, box.b1)] + arr[idx(box.r0, box.g1, box.b0)] +
           arr[idx(box.r0, box.g0, box.b1)] - arr[idx(box.r0, box.g0, box.b0)];
}

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
// Step 3 -- Variance
// ============================================================================

f64 variance(const Moments& moments, const Box& box) {
    const f64 weight = box_sum(moments.weight, box);
    if (weight <= 0.0)
        return 0.0;
    const f64 total_r = box_sum(moments.sum_r, box);
    const f64 total_g = box_sum(moments.sum_g, box);
    const f64 total_b = box_sum(moments.sum_b, box);
    const f64 total_sq = box_sum(moments.sum_sq, box);
    return total_sq - (total_r * total_r + total_g * total_g + total_b * total_b) / weight;
}

// ============================================================================
// Step 4 -- Optimal cut plane
// ============================================================================

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
// Step 5 -- Cut box
// ============================================================================

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
// Shared state for the async quantization pipeline
// ============================================================================

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
    std::vector<Moments> threadMoments;

    // --- Box splitting ---
    std::vector<Box> boxes;
    std::vector<f64> variances;
    u32 boxCount = 0;

    // --- Result ---
    std::array<u32, MAX_COLORS> palette{};
    u32 colorCount = 0;
    std::vector<u8> tag;
    bool resultSrgb = false;

    // --- K-means ---
    ToLinearLut lut{false};
    std::vector<ColorF> centroids;
    LabPalette centroidPal;     // SoA for vectorized nearest search
    std::vector<Lab> pixelLabs; // f32 per-pixel Labs (12 B each)
    std::vector<u8> assignments;
    u32 kmeansWeightThreshold = 1;
    std::shared_ptr<std::atomic<bool>> kmeansConverged;

    // --- Tag volume ---
    LabPalette palLabels; // SoA palette Labs for tag build

    // --- Dither refine ---
    std::vector<ColorF> palColors;
    std::vector<f64> ditherOffsets;
    f64 ditherPrevError = std::numeric_limits<f64>::max();
    f64 ditherLr = 1.0;
    u32 ditherStarveThreshold = 1;
    DitherParams ditherParams{};
    std::shared_ptr<std::atomic<bool>> ditherConverged;
};

// ============================================================================
// 3D Hilbert curve -- palette sorting  (Skilling's transpose algorithm)
// ============================================================================

inline u64 hilbert_xyz2d(u32 x, u32 y, u32 z, u32 order) {
    u32 coords[3] = {x, y, z};
    const u32 n = 3;
    const u32 M = 1u << (order - 1);

    for (u32 q = M; q > 1; q >>= 1) {
        const u32 p = q - 1;
        for (u32 i = 0; i < n; ++i) {
            if (coords[i] & q)
                coords[0] ^= p;
            else {
                const u32 t = (coords[0] ^ coords[i]) & p;
                coords[0] ^= t;
                coords[i] ^= t;
            }
        }
    }

    for (u32 i = 1; i < n; ++i)
        coords[i] ^= coords[i - 1];

    u32 t2 = 0;
    for (u32 q = M; q > 1; q >>= 1) {
        if (coords[n - 1] & q)
            t2 ^= q - 1;
    }
    for (u32 i = 0; i < n; ++i)
        coords[i] ^= t2;

    u64 d = 0;
    for (i32 bit = static_cast<i32>(order) - 1; bit >= 0; --bit) {
        for (u32 i = 0; i < n; ++i)
            d = (d << 1) | ((coords[i] >> bit) & 1u);
    }
    return d;
}

void sort_palette_hilbert(std::shared_ptr<QuantizeState> state) {
    const u32 colorCount = state->colorCount;
    if (colorCount <= 1)
        return;

    constexpr u32 kOrder = 8;
    std::vector<u32> order(colorCount);
    std::iota(order.begin(), order.end(), 0u);

    std::vector<u64> hilbertKeys(colorCount);
    for (u32 i = 0; i < colorCount; ++i) {
        const u32 packed = state->palette[i];
        hilbertKeys[i] =
            hilbert_xyz2d((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF, kOrder);
    }

    std::sort(order.begin(), order.end(),
              [&](u32 a, u32 b) { return hilbertKeys[a] < hilbertKeys[b]; });

    std::vector<u8> remap(colorCount);
    std::array<u32, MAX_COLORS> newPalette{};
    for (u32 newIdx = 0; newIdx < colorCount; ++newIdx) {
        const u32 oldIdx = order[newIdx];
        remap[oldIdx] = static_cast<u8>(newIdx);
        newPalette[newIdx] = state->palette[oldIdx];
    }
    state->palette = newPalette;

    for (auto& t : state->tag)
        t = remap[t];
}

// ============================================================================
// Tag volume -- uses precomputed bin-center Labs + SoA palette search
// ============================================================================

void build_tag_volume(std::shared_ptr<QuantizeState> state, QuantizeContext* ctx) {
    state->tag.assign(HIST_TOTAL, 0);
    state->palLabels = make_lab_palette(state->palette, state->colorCount, state->srgb);

    const auto& bin_labs = get_bin_center_labs(state->srgb);
    constexpr u32 rSlices = HIST_SIZE - 1;

    parallel_for_chunks(rSlices, ctx, [state, &bin_labs](u32 begin, u32 end) {
        const auto& pal = state->palLabels;
        auto& tag = state->tag;

        for (u32 s = begin; s < end; ++s) {
            const i32 ri = static_cast<i32>(s) + 1;
            for (i32 gi = 1; gi < HIST_SIZE; ++gi) {
                for (i32 bi = 1; bi < HIST_SIZE; ++bi) {
                    const u32 lab_idx =
                        static_cast<u32>(((ri - 1) * 32 + (gi - 1)) * 32 + (bi - 1));
                    tag[idx(ri, gi, bi)] = static_cast<u8>(pal.findNearest(bin_labs[lab_idx]));
                }
            }
        }
    });
}

} // anonymous namespace

// ============================================================================
// Quantizer::Impl
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
// Quantizer -- construction and builder methods
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
// build_quantize_dag -- the full quantization pipeline as DAG nodes
// ============================================================================

namespace {

void build_quantize_dag(std::shared_ptr<QuantizeState> state, QuantizeContext* ctx) {
    const u32 pixelCount = state->pixelCount;
    const u32 nThreads = state->nThreads;

    // -- 1. Histogram build --
    if (nThreads > 1 && ctx->pool) {
        state->threadMoments.resize(nThreads);
        for (auto& m : state->threadMoments)
            std::memset(&m, 0, sizeof(Moments));

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
            utils::JobGroup group;
            for (u32 t = 0; t < nThreads; ++t) {
                const u32 begin = t * chunk;
                const u32 end = std::min(begin + chunk, pixelCount);
                if (begin >= end)
                    continue;
                group.add(1);
                interfaces::WorkerTask const task{[&, t, begin, end]() {
                    build_histogram_range(state->rgba, begin, end, state->threadMoments[t]);
                    group.done();
                }};
                ctx->pool->submit(task);
            }
            group.wait();
        }

        // -- 2. Histogram merge --
        submitSingleTask(ctx, [state]() {
            merge_moments(state->threadMoments, *state->moments);
            state->threadMoments.clear();
            state->threadMoments.shrink_to_fit();
        });
    } else {
        submitSingleTask(
            ctx, [state]() { build_histogram(state->rgba, state->pixelCount, *state->moments); });
    }

    // -- 3. Prefix sums --
    submitSingleTask(ctx, [state]() { compute_cumulative_moments(*state->moments); });

    // -- 4. Box splitting + palette extraction --
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

        for (u32 i = 0; i < boxCount; ++i) {
            const f64 weight = box_sum(moments.weight, boxes[i]);
            if (weight > 0.0) {
                state->palette[i] = pack_color({box_sum(moments.sum_r, boxes[i]) / weight,
                                                box_sum(moments.sum_g, boxes[i]) / weight,
                                                box_sum(moments.sum_b, boxes[i]) / weight});
            }
        }
        state->colorCount = boxCount;
        state->boxCount = boxCount;
        state->moments.reset();
    });

    // -- 5. K-means refinement --

    // 5a. Init buffers + pixel Labs.
    submitSingleTask(ctx, [state]() {
        state->kmeansConverged = std::make_shared<std::atomic<bool>>(false);
        state->lut = ToLinearLut(state->srgb);

        const u32 colorCount = state->colorCount;
        if (colorCount <= 1 || state->pixelCount == 0 || state->kmeansIterations == 0) {
            state->kmeansConverged->store(true, std::memory_order_release);
            return;
        }
        state->centroids = unpack_palette(state->palette, colorCount);
        state->centroidPal = make_lab_palette(state->centroids.data(), colorCount, state->srgb);
        state->pixelLabs.resize(state->pixelCount);
        state->assignments.resize(state->pixelCount);
        state->kmeansWeightThreshold = std::max(state->pixelCount / (colorCount * 16u), 1u);
    });

    // 5b. Compute pixel Labs.
    parallel_for_chunks(pixelCount, ctx, [state](u32 begin, u32 end) {
        if (state->kmeansConverged->load(std::memory_order_acquire))
            return;
        for (u32 p = begin; p < end; ++p)
            state->pixelLabs[p] = pixel_to_lab(state->rgba, p, state->lut);
    });

    // 5c. K-means iterations.
    for (u32 iter = 0; iter < state->kmeansIterations; ++iter) {
        // Assignment.
        parallel_for_chunks(pixelCount, ctx, [state](u32 begin, u32 end) {
            if (state->kmeansConverged->load(std::memory_order_acquire))
                return;
            const auto& pal = state->centroidPal;
            for (u32 p = begin; p < end; ++p)
                state->assignments[p] = static_cast<u8>(pal.findNearest(state->pixelLabs[p]));
        });

        // Centroid update.
        submitSingleTask(ctx, [state]() {
            if (state->kmeansConverged->load(std::memory_order_acquire))
                return;

            const u32 colorCount = state->colorCount;
            if (colorCount <= 1)
                return;

            const u32 pc = state->pixelCount;

            std::vector<ColorF> sums(colorCount, ColorF{0, 0, 0});
            std::vector<u32> counts(colorCount, 0u);
            for (u32 p = 0; p < pc; ++p) {
                const u32 c = state->assignments[p];
                sums[c] += read_pixel_rgb(state->rgba, p);
                counts[c]++;
            }

            bool changed = false;
            for (u32 c = 0; c < colorCount; ++c) {
                if (counts[c] > 0) {
                    const auto newCentroid = sums[c] / static_cast<f64>(counts[c]);
                    if (pack_color(newCentroid) != pack_color(state->centroids[c]))
                        changed = true;
                    state->centroids[c] = newCentroid;
                    state->centroidPal.set(c, colorf_to_lab(newCentroid, state->srgb));
                }
            }

            // Reinitialize starved clusters from highest-error pixel.
            for (u32 c = 0; c < colorCount; ++c) {
                if (counts[c] >= state->kmeansWeightThreshold)
                    continue;
                f32 worst_err = -1.0f;
                u32 worst_px = 0;
                for (u32 p = 0; p < pc; ++p) {
                    const f32 err =
                        state->centroidPal.distSq(state->assignments[p], state->pixelLabs[p]);
                    if (err > worst_err) {
                        worst_err = err;
                        worst_px = p;
                    }
                }
                state->centroids[c] = read_pixel_rgb(state->rgba, worst_px);
                state->centroidPal.set(c, state->pixelLabs[worst_px]);
                changed = true;
            }

            if (!changed)
                state->kmeansConverged->store(true, std::memory_order_release);
        });
    }

    // 5d. Pack K-means result.
    submitSingleTask(ctx, [state]() {
        const u32 colorCount = state->colorCount;
        if (colorCount > 1 && !state->centroids.empty())
            pack_palette(state->centroids.data(), colorCount, state->palette);
    });

    // -- 6. Tag volume --
    submitSingleTask(ctx, [state]() {
        state->tag.assign(HIST_TOTAL, 0);
        state->palLabels = make_lab_palette(state->palette, state->colorCount, state->srgb);
        state->resultSrgb = state->srgb;
    });

    {
        const auto& bin_labs = get_bin_center_labs(state->srgb);
        constexpr u32 rSlices = HIST_SIZE - 1;
        parallel_for_chunks(rSlices, ctx, [state, &bin_labs](u32 begin, u32 end) {
            const auto& pal = state->palLabels;
            auto& tag = state->tag;
            for (u32 s = begin; s < end; ++s) {
                const i32 ri = static_cast<i32>(s) + 1;
                for (i32 gi = 1; gi < HIST_SIZE; ++gi) {
                    for (i32 bi = 1; bi < HIST_SIZE; ++bi) {
                        const u32 lab_idx =
                            static_cast<u32>(((ri - 1) * 32 + (gi - 1)) * 32 + (bi - 1));
                        tag[idx(ri, gi, bi)] = static_cast<u8>(pal.findNearest(bin_labs[lab_idx]));
                    }
                }
            }
        });
    }

    // -- 7. Dither-aware refinement --
    const bool doDither = state->ditherAwareEnabled && state->ditherWidth > 0 &&
                          state->ditherHeight > 0 && state->ditherStrength > 0.0f;
    if (doDither) {

        const u32 ditherPixelCount =
            std::min(state->ditherWidth * state->ditherHeight, state->pixelCount);

        // 7a. Allocate dither buffers.
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
            state->ditherStarveThreshold = std::max(ditherPixelCount / (colorCount * 16u), 1u);
            state->ditherPrevError = std::numeric_limits<f64>::max();
            state->ditherLr = 1.0;

            if (state->pixelLabs.size() < ditherPixelCount)
                state->pixelLabs.resize(ditherPixelCount);
            if (state->assignments.size() < ditherPixelCount)
                state->assignments.resize(ditherPixelCount);
        });

        // 7b. Compute dither offsets + pixel Labs.
        //     Always (re)compute pixelLabs here: kmeans may have been disabled
        //     (kmeansIterations == 0), leaving pixelLabs uninitialized.
        parallel_for_chunks(ditherPixelCount, ctx, [state](u32 begin, u32 end) {
            if (state->ditherConverged->load(std::memory_order_acquire))
                return;
            const auto noise = blueNoiseMap();
            const u32 w = state->ditherWidth;
            for (u32 i = begin; i < end; ++i) {
                state->ditherOffsets[i] = dither_offset(i % w, i / w, noise, state->ditherParams);
                state->pixelLabs[i] = pixel_to_lab(state->rgba, i, state->lut);
            }
        });

        // 7c. Dither iterations.
        for (u32 iter = 0; iter < state->ditherIterations; ++iter) {
            // Prepare centroid Labs.
            submitSingleTask(ctx, [state]() {
                if (state->ditherConverged->load(std::memory_order_acquire))
                    return;
                if (state->colorCount <= 1) {
                    state->ditherConverged->store(true, std::memory_order_release);
                    return;
                }
                state->centroidPal =
                    make_lab_palette(state->palColors.data(), state->colorCount, state->srgb);
            });

            // Assignment with dithering.
            parallel_for_chunks(ditherPixelCount, ctx, [state](u32 begin, u32 end) {
                if (state->ditherConverged->load(std::memory_order_acquire))
                    return;
                const auto& pal = state->centroidPal;
                for (u32 i = begin; i < end; ++i) {
                    const auto px = read_pixel_rgb(state->rgba, i);
                    const f64 d = state->ditherOffsets[i];
                    const auto dithered = clamp_color(px + ColorF{d, d, d});
                    state->assignments[i] =
                        static_cast<u8>(pal.findNearest(colorf_to_lab(dithered, state->srgb)));
                }
            });

            // Centroid update + convergence.
            submitSingleTask(ctx, [state]() {
                if (state->ditherConverged->load(std::memory_order_acquire))
                    return;

                const u32 colorCount = state->colorCount;
                const u32 pc =
                    std::min(state->ditherWidth * state->ditherHeight, state->pixelCount);

                std::vector<ColorF> accum(colorCount, ColorF{0, 0, 0});
                std::vector<u32> counts(colorCount, 0u);
                f64 totalError = 0.0;
                for (u32 i = 0; i < pc; ++i) {
                    const u32 best = state->assignments[i];
                    totalError += state->centroidPal.distSq(best, state->pixelLabs[i]);
                    accum[best] += read_pixel_rgb(state->rgba, i);
                    counts[best]++;
                }

                if (totalError >= state->ditherPrevError) {
                    state->ditherConverged->store(true, std::memory_order_release);
                    return;
                }
                state->ditherPrevError = totalError;

                constexpr f64 lr_decay = 0.7;
                for (u32 c = 0; c < colorCount; ++c) {
                    if (counts[c] == 0)
                        continue;
                    const ColorF target = accum[c] / static_cast<f64>(counts[c]);
                    state->palColors[c] =
                        clamp_color(ColorF::lerp(state->palColors[c], target, state->ditherLr));
                }

                const auto pal = make_lab_palette(state->palColors.data(), colorCount, state->srgb);
                for (u32 c = 0; c < colorCount; ++c) {
                    if (counts[c] >= state->ditherStarveThreshold)
                        continue;
                    const u32 worst_px = find_worst_error_pixel(
                        state->pixelLabs.data(), static_cast<u32>(state->pixelLabs.size()), pal);
                    state->palColors[c] = read_pixel_rgb(state->rgba, worst_px);
                }

                state->ditherLr *= lr_decay;
            });
        }

        // 7d. Pack palette + rebuild tag volume.
        submitSingleTask(ctx, [state]() {
            if (!state->palColors.empty() && state->colorCount > 1)
                pack_palette(state->palColors.data(), state->colorCount, state->palette);
        });

        {
            submitSingleTask(ctx, [state]() {
                state->tag.assign(HIST_TOTAL, 0);
                state->palLabels = make_lab_palette(state->palette, state->colorCount, state->srgb);
            });

            const auto& bin_labs = get_bin_center_labs(state->srgb);
            constexpr u32 rSlices = HIST_SIZE - 1;
            parallel_for_chunks(rSlices, ctx, [state, &bin_labs](u32 begin, u32 end) {
                const auto& pal = state->palLabels;
                auto& tag = state->tag;
                for (u32 s = begin; s < end; ++s) {
                    const i32 ri = static_cast<i32>(s) + 1;
                    for (i32 gi = 1; gi < HIST_SIZE; ++gi) {
                        for (i32 bi = 1; bi < HIST_SIZE; ++bi) {
                            const u32 lab_idx =
                                static_cast<u32>(((ri - 1) * 32 + (gi - 1)) * 32 + (bi - 1));
                            tag[idx(ri, gi, bi)] =
                                static_cast<u8>(pal.findNearest(bin_labs[lab_idx]));
                        }
                    }
                }
            });
        }

    } // end doDither

    // -- 8. Hilbert sort --
    submitSingleTask(ctx, [state]() { sort_palette_hilbert(state); });
}

} // anonymous namespace

// ============================================================================
// Quantizer::quantize -- blocking entry point
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
// Quantizer::quantizeAsync -- non-blocking entry point
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

    submitSingleTask(&ctx, [state, outResult]() {
        outResult->palette = state->palette;
        outResult->color_count = state->colorCount;
        outResult->tag_ = std::move(state->tag);
        outResult->srgb_ = state->resultSrgb;
    });

    return ctx.currentValue;
}

// ============================================================================
// QuantizeResult -- pixel mapping
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
        const u32 base = i * 4;
        out_indices[i] = tag_[idx(quantize_channel(rgba[base]), quantize_channel(rgba[base + 1]),
                                  quantize_channel(rgba[base + 2]))];
    }
}

void QuantizeResult::mapPixelsDithered(const u8* rgba, u32 width, u32 height, f32 strength,
                                       u8* out_indices) const {
    if (color_count <= 1 || width == 0 || height == 0) {
        mapPixels(rgba, width * height, out_indices);
        return;
    }

    const auto noise = blueNoiseMap();
    const auto pal = make_lab_palette(palette, color_count, srgb_);
    const auto dp = make_dither_params(strength);

    const u32 pixel_count = width * height;
    for (u32 i = 0; i < pixel_count; ++i) {
        const f64 d = dither_offset(i % width, i / width, noise, dp);
        const auto dithered = clamp_color(read_pixel_rgb(rgba, i) + ColorF{d, d, d});
        out_indices[i] = static_cast<u8>(pal.findNearest(colorf_to_lab(dithered, srgb_)));
    }
}

// ============================================================================
// Dither-aware refinement
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

    std::vector<Lab> pixel_labs(pixel_count);
    std::vector<f64> offsets(pixel_count);

    QuantizeContext ctx{pool, nullptr, 0};
    parallel_for_chunks(pixel_count, &ctx, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            pixel_labs[i] = pixel_to_lab(rgba, i, lut);
            offsets[i] = dither_offset(i % width, i / width, noise, dp);
        }
    });

    const u32 starve_threshold = std::max(pixel_count / (color_count * 16u), 1u);
    const bool usePool = pool && pool->threadCount() > 1 && pixel_count >= 1024;
    const u32 nThreads = usePool ? static_cast<u32>(pool->threadCount()) : 1;

    std::vector<std::vector<ColorF>> thread_accum(nThreads, std::vector<ColorF>(color_count));
    std::vector<std::vector<u32>> thread_counts(nThreads, std::vector<u32>(color_count));
    std::vector<f64> thread_errors(nThreads, 0.0);

    f64 lr = 1.0;
    constexpr f64 lr_decay = 0.7;
    f64 prev_error = std::numeric_limits<f64>::max();

    for (u32 iter = 0; iter < iterations; ++iter) {
        const auto pal_lab = make_lab_palette(pal_colors.data(), color_count, srgb_);

        for (u32 t = 0; t < nThreads; ++t) {
            std::fill(thread_accum[t].begin(), thread_accum[t].end(), ColorF{0, 0, 0});
            std::fill(thread_counts[t].begin(), thread_counts[t].end(), 0u);
            thread_errors[t] = 0.0;
        }

        if (usePool) {
            const u32 chunk = (pixel_count + nThreads - 1) / nThreads;
            utils::JobGroup group;
            for (u32 t = 0; t < nThreads; ++t) {
                const u32 begin = t * chunk;
                const u32 end = std::min(begin + chunk, pixel_count);
                if (begin >= end)
                    break;
                group.add(1);
                interfaces::WorkerTask const task{[&, t, begin, end]() {
                    f64 local_error = 0.0;
                    for (u32 i = begin; i < end; ++i) {
                        const auto px = read_pixel_rgb(rgba, i);
                        const auto dithered =
                            clamp_color(px + ColorF{offsets[i], offsets[i], offsets[i]});
                        const u32 best = pal_lab.findNearest(colorf_to_lab(dithered, srgb_));
                        local_error += pal_lab.distSq(best, pixel_labs[i]);
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
                const u32 best = pal_lab.findNearest(colorf_to_lab(dithered, srgb_));
                thread_errors[0] += pal_lab.distSq(best, pixel_labs[i]);
                thread_accum[0][best] += px;
                thread_counts[0][best]++;
            }
        }

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

        if (total_error >= prev_error)
            break;
        prev_error = total_error;

        for (u32 c = 0; c < color_count; ++c) {
            if (counts[c] == 0)
                continue;
            const ColorF target = accum[c] / static_cast<f64>(counts[c]);
            pal_colors[c] = clamp_color(ColorF::lerp(pal_colors[c], target, lr));
        }

        for (u32 c = 0; c < color_count; ++c) {
            if (counts[c] >= starve_threshold)
                continue;
            const u32 worst_px = find_worst_error_pixel(pixel_labs.data(), pixel_count, pal_lab);
            pal_colors[c] = read_pixel_rgb(rgba, worst_px);
        }

        lr *= lr_decay;
    }

    pack_palette(pal_colors.data(), color_count, palette);

    auto state = std::make_shared<QuantizeState>();
    state->palette = palette;
    state->colorCount = color_count;
    state->srgb = srgb_;
    build_tag_volume(state, &ctx);
    sort_palette_hilbert(state);
    palette = state->palette;
    tag_ = std::move(state->tag);
}

} // namespace whiteout::textures::wu

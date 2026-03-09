// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file wu_quantize.cpp
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
 */

#include "wu_quantize.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

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
// Step 1 — Build histogram
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
// Build tag volume — maps quantized color → palette index
// ============================================================================

void build_tag_volume(const Box* boxes, u32 box_count, std::vector<u8>& tag) {
    tag.assign(HIST_TOTAL, 0);

    for (u32 i = 0; i < box_count; ++i) {
        const Box& box = boxes[i];
        for (i32 ri = box.r0 + 1; ri <= box.r1; ++ri)
            for (i32 gi = box.g0 + 1; gi <= box.g1; ++gi)
                for (i32 bi = box.b0 + 1; bi <= box.b1; ++bi)
                    tag[idx(ri, gi, bi)] = static_cast<u8>(i);
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

QuantizeResult quantize(const u8* rgba, u32 pixel_count, u32 max_colors) {
    QuantizeResult result;
    if (pixel_count == 0 || max_colors == 0) {
        result.color_count = 0;
        return result;
    }
    max_colors = std::min(max_colors, MAX_COLORS);

    // --- 1. Build 3D histogram ---
    // Allocate on the heap to avoid ~1.4 MB on the stack.
    auto moments = std::make_unique<Moments>();
    build_histogram(rgba, pixel_count, *moments);

    // --- 2. Compute cumulative 3D prefix sums ---
    compute_cumulative_moments(*moments);

    // --- 3. Iterative box splitting ---
    std::vector<Box> boxes(max_colors);
    std::vector<f64> variances(max_colors, 0.0);

    // Initial box covers the entire color space.
    boxes[0].r0 = boxes[0].g0 = boxes[0].b0 = 0;
    boxes[0].r1 = boxes[0].g1 = boxes[0].b1 = HIST_SIZE - 1; // 32
    boxes[0].vol = (HIST_SIZE - 1) * (HIST_SIZE - 1) * (HIST_SIZE - 1);

    u32 box_count = 1;

    for (u32 i = 1; i < max_colors; ++i) {
        // Recompute variances for the two most recently produced boxes.
        if (box_count >= 2) {
            variances[box_count - 2] = variance(*moments, boxes[box_count - 2]);
            variances[box_count - 1] = variance(*moments, boxes[box_count - 1]);
        } else {
            variances[0] = variance(*moments, boxes[0]);
        }

        // Find the box with the largest variance.
        u32 best_idx = 0;
        f64 best_variance = variances[0];
        for (u32 j = 1; j < box_count; ++j) {
            if (variances[j] > best_variance) {
                best_variance = variances[j];
                best_idx = j;
            }
        }

        if (best_variance <= 0.0)
            break; // All remaining boxes are single-color; nothing to split.

        // Split the chosen box.
        if (!cut(*moments, boxes[best_idx], boxes[box_count]))
            break;

        // Update variances for the two children.
        variances[best_idx] = variance(*moments, boxes[best_idx]);
        variances[box_count] = variance(*moments, boxes[box_count]);
        ++box_count;
    }

    // --- 4. Compute palette colors (weighted centroids) ---
    for (u32 i = 0; i < box_count; ++i) {
        const f64 weight = box_sum(moments->weight, boxes[i]);
        if (weight > 0.0) {
            const f64 avg_r = box_sum(moments->sum_r, boxes[i]) / weight + 0.5;
            const f64 avg_g = box_sum(moments->sum_g, boxes[i]) / weight + 0.5;
            const f64 avg_b = box_sum(moments->sum_b, boxes[i]) / weight + 0.5;
            const u8 red = static_cast<u8>(std::min(std::max(avg_r, 0.0), 255.0));
            const u8 green = static_cast<u8>(std::min(std::max(avg_g, 0.0), 255.0));
            const u8 blue = static_cast<u8>(std::min(std::max(avg_b, 0.0), 255.0));
            // BLP palette format: [23:16]=R, [15:8]=G, [7:0]=B
            result.palette[i] = (static_cast<u32>(red) << 16) | (static_cast<u32>(green) << 8) |
                                static_cast<u32>(blue);
        } else {
            result.palette[i] = 0;
        }
    }
    result.color_count = box_count;

    // --- 5. Build tag volume for fast pixel lookup ---
    build_tag_volume(boxes.data(), box_count, result.tag_);

    return result;
}

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
        const i32 ri = quantize_channel(rgba[i * 4 + 0]);
        const i32 gi = quantize_channel(rgba[i * 4 + 1]);
        const i32 bi = quantize_channel(rgba[i * 4 + 2]);
        out_indices[i] = tag_[idx(ri, gi, bi)];
    }
}

} // namespace whiteout::textures::wu

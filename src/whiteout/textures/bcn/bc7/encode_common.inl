// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_common.inl
/// @brief BC7 encode: shared colour helpers, PCA, and endpoint fitting.

// ############################################################################
//  ENCODE
// ############################################################################

namespace {

// ============================================================================
// Colour helpers
// ============================================================================

// RGBA, pixel_at(), colour_error_rgba(), colour_error_rgb() are in bcn_common.h.

// ============================================================================
// Principal Component Analysis (simple power-iteration for 1 axis)
// ============================================================================

struct Vec4 {
    f64 x, y, z, w;
};

Vec4 pca_axis_rgba(const u8* rgba, u32 count) {
    // Compute mean
    f64 mean_r = 0, mean_g = 0, mean_b = 0, mean_a = 0;
    for (u32 i = 0; i < count; ++i) {
        mean_r += rgba[i * 4 + 0];
        mean_g += rgba[i * 4 + 1];
        mean_b += rgba[i * 4 + 2];
        mean_a += rgba[i * 4 + 3];
    }
    const f64 inv_count = 1.0 / count;
    mean_r *= inv_count;
    mean_g *= inv_count;
    mean_b *= inv_count;
    mean_a *= inv_count;

    // Covariance matrix (4×4, symmetric → 10 unique)
    std::array<std::array<f64, 4>, 4> cov{};
    for (u32 i = 0; i < count; ++i) {
        f64 dr = rgba[i * 4 + 0] - mean_r;
        f64 dg = rgba[i * 4 + 1] - mean_g;
        f64 db = rgba[i * 4 + 2] - mean_b;
        f64 da = rgba[i * 4 + 3] - mean_a;
        cov[0][0] += dr * dr;
        cov[0][1] += dr * dg;
        cov[0][2] += dr * db;
        cov[0][3] += dr * da;
        cov[1][1] += dg * dg;
        cov[1][2] += dg * db;
        cov[1][3] += dg * da;
        cov[2][2] += db * db;
        cov[2][3] += db * da;
        cov[3][3] += da * da;
    }
    cov[1][0] = cov[0][1];
    cov[2][0] = cov[0][2];
    cov[2][1] = cov[1][2];
    cov[3][0] = cov[0][3];
    cov[3][1] = cov[1][3];
    cov[3][2] = cov[2][3];

    // Power iteration (8 iters is plenty for 4D)
    Vec4 v{1.0, 1.0, 1.0, 1.0};
    for (int iter = 0; iter < 8; ++iter) {
        Vec4 nv;
        nv.x = cov[0][0] * v.x + cov[0][1] * v.y + cov[0][2] * v.z + cov[0][3] * v.w;
        nv.y = cov[1][0] * v.x + cov[1][1] * v.y + cov[1][2] * v.z + cov[1][3] * v.w;
        nv.z = cov[2][0] * v.x + cov[2][1] * v.y + cov[2][2] * v.z + cov[2][3] * v.w;
        nv.w = cov[3][0] * v.x + cov[3][1] * v.y + cov[3][2] * v.z + cov[3][3] * v.w;
        f64 len = std::sqrt(nv.x * nv.x + nv.y * nv.y + nv.z * nv.z + nv.w * nv.w);
        if (len < 1e-12) {
            return {1, 0, 0, 0};
        }
        v = {nv.x / len, nv.y / len, nv.z / len, nv.w / len};
    }
    return v;
}

Vec4 pca_axis_rgb(const u8* rgba, const u8* subset_mask, u8 subset_id, u32 count) {
    // PCA on RGB channels only, for pixels belonging to `subset_id`.
    f64 mean_r = 0, mean_g = 0, mean_b = 0;
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        if (subset_mask[i] != subset_id)
            continue;
        mean_r += rgba[i * 4 + 0];
        mean_g += rgba[i * 4 + 1];
        mean_b += rgba[i * 4 + 2];
        ++n;
    }
    if (n == 0)
        return {1, 0, 0, 0};
    const f64 inv_count = 1.0 / n;
    mean_r *= inv_count;
    mean_g *= inv_count;
    mean_b *= inv_count;

    std::array<std::array<f64, 3>, 3> cov{};
    for (u32 i = 0; i < count; ++i) {
        if (subset_mask[i] != subset_id)
            continue;
        f64 dr = rgba[i * 4 + 0] - mean_r;
        f64 dg = rgba[i * 4 + 1] - mean_g;
        f64 db = rgba[i * 4 + 2] - mean_b;
        cov[0][0] += dr * dr;
        cov[0][1] += dr * dg;
        cov[0][2] += dr * db;
        cov[1][1] += dg * dg;
        cov[1][2] += dg * db;
        cov[2][2] += db * db;
    }
    cov[1][0] = cov[0][1];
    cov[2][0] = cov[0][2];
    cov[2][1] = cov[1][2];

    Vec4 v{1.0, 1.0, 1.0, 0.0};
    for (int iter = 0; iter < 8; ++iter) {
        Vec4 nv;
        nv.x = cov[0][0] * v.x + cov[0][1] * v.y + cov[0][2] * v.z;
        nv.y = cov[1][0] * v.x + cov[1][1] * v.y + cov[1][2] * v.z;
        nv.z = cov[2][0] * v.x + cov[2][1] * v.y + cov[2][2] * v.z;
        nv.w = 0;
        f64 len = std::sqrt(nv.x * nv.x + nv.y * nv.y + nv.z * nv.z);
        if (len < 1e-12)
            return {1, 0, 0, 0};
        v = {nv.x / len, nv.y / len, nv.z / len, 0};
    }
    return v;
}

// ============================================================================
// Endpoint fitting helpers
// ============================================================================

/// Compute min/max bounding box endpoints over all 16 pixels (RGBA).
void bbox_endpoints_rgba(const u8* rgba, RGBA& min_color, RGBA& max_color) {
    min_color = {255, 255, 255, 255};
    max_color = {0, 0, 0, 0};
    for (u32 i = 0; i < 16; ++i) {
        auto p = pixel_at(rgba, i);
        min_color.r = std::min(min_color.r, p.r);
        min_color.g = std::min(min_color.g, p.g);
        min_color.b = std::min(min_color.b, p.b);
        min_color.a = std::min(min_color.a, p.a);
        max_color.r = std::max(max_color.r, p.r);
        max_color.g = std::max(max_color.g, p.g);
        max_color.b = std::max(max_color.b, p.b);
        max_color.a = std::max(max_color.a, p.a);
    }
}

/// Project pixels onto `axis`, find min/max projection, derive endpoints.
void pca_endpoints_rgba(const u8* rgba, const Vec4& axis, RGBA& min_color, RGBA& max_color) {
    f64 proj_min = 1e30, proj_max = -1e30;
    u32 imin = 0, imax = 0;
    for (u32 i = 0; i < 16; ++i) {
        f64 p = rgba[i * 4 + 0] * axis.x + rgba[i * 4 + 1] * axis.y + rgba[i * 4 + 2] * axis.z +
                rgba[i * 4 + 3] * axis.w;
        if (p < proj_min) {
            proj_min = p;
            imin = i;
        }
        if (p > proj_max) {
            proj_max = p;
            imax = i;
        }
    }
    min_color = pixel_at(rgba, imin);
    max_color = pixel_at(rgba, imax);
}

void pca_endpoints_rgb_subset(const u8* rgba, const u8* subset_mask, u8 subset_id, const Vec4& axis,
                              RGBA& min_color, RGBA& max_color) {
    f64 proj_min = 1e30, proj_max = -1e30;
    u32 imin = 0, imax = 0;
    bool found = false;
    for (u32 i = 0; i < 16; ++i) {
        if (subset_mask[i] != subset_id)
            continue;
        f64 p = rgba[i * 4 + 0] * axis.x + rgba[i * 4 + 1] * axis.y + rgba[i * 4 + 2] * axis.z;
        if (!found || p < proj_min) {
            proj_min = p;
            imin = i;
            found = true;
        }
        if (p > proj_max) {
            proj_max = p;
            imax = i;
        }
    }
    min_color = pixel_at(rgba, imin);
    max_color = pixel_at(rgba, imax);
}

/// Clamp to [0, max_val].
inline i32 clamp_i(i32 v, i32 max_val) {
    return std::max(0, std::min(v, max_val));
}

/// Quantize a value to `prec` bits.
inline u32 quantize(i32 val, u32 prec) {
    const i32 max_val = (1 << prec) - 1;
    return static_cast<u32>(clamp_i((val * max_val + 127) / 255, max_val));
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_mode7.inl
/// @brief BC7 encode: Mode 7 — 2 subsets, RGBA 5555 + per-endpoint P-bit, 2-bit indices.

// ============================================================================
// Mode 7 : 2 subsets, RGBA 5555 + per-endpoint P-bit, 2-bit indices
// ============================================================================

u64 encode_mode7_partition(const u8* rgba, u8* out, u32 partition) {
    const u8* part = BC7_PARTITION_TABLE_2[partition].data();
    u8 anchor2 = BC7_ANCHOR_2[partition];

    // PCA on RGB per subset; find alpha range separately.
    Vec4 axis0 = pca_axis_rgb(rgba, part, 0, 16);
    Vec4 axis1 = pca_axis_rgb(rgba, part, 1, 16);

    RGBA lo0, hi0, lo1, hi1;
    pca_endpoints_rgb_subset(rgba, part, 0, axis0, lo0, hi0);
    pca_endpoints_rgb_subset(rgba, part, 1, axis1, lo1, hi1);

    // Alpha range per subset
    std::array<i32, 2> alo = {255, 255};
    std::array<i32, 2> ahi_val = {0, 0};
    for (u32 i = 0; i < 16; ++i) {
        u8 s = part[i];
        i32 a = rgba[i * 4 + 3];
        alo[s] = std::min(alo[s], a);
        ahi_val[s] = std::max(ahi_val[s], a);
    }
    lo0.a = alo[0];
    hi0.a = ahi_val[0];
    lo1.a = alo[1];
    hi1.a = ahi_val[1];

    // Mode 7: 5-bit endpoints + 1 P-bit per endpoint → 6-bit effective.
    auto quantize5 = [](i32 val, u32 pbit) -> u32 {
        i32 v6_target = (val * 63 + 127) / 255;
        i32 q = clamp_i((v6_target - static_cast<i32>(pbit) + 1) >> 1, 31);
        return static_cast<u32>(q);
    };

    auto unq5p = [](u32 q, u32 pbit) -> u32 {
        u32 v6 = (q << 1) | pbit;
        return bc7_unquantize(v6, 6);
    };

    u64 best_err = std::numeric_limits<u64>::max();
    std::array<u32, 16> best_ep{};
    std::array<u32, 4> best_pb{};
    std::array<u8, 16> best_indices{};

    // Try all 16 combinations of 4 P-bits
    for (u32 pmask = 0; pmask < 16; ++pmask) {
        std::array<u32, 4> pb = {pmask & 1, (pmask >> 1) & 1, (pmask >> 2) & 1, (pmask >> 3) & 1};

        std::array<u32, 16> ep;
        // Subset 0 ep0
        ep[0] = quantize5(lo0.r, pb[0]);
        ep[1] = quantize5(lo0.g, pb[0]);
        ep[2] = quantize5(lo0.b, pb[0]);
        ep[3] = quantize5(lo0.a, pb[0]);
        // Subset 0 ep1
        ep[4] = quantize5(hi0.r, pb[1]);
        ep[5] = quantize5(hi0.g, pb[1]);
        ep[6] = quantize5(hi0.b, pb[1]);
        ep[7] = quantize5(hi0.a, pb[1]);
        // Subset 1 ep0
        ep[8] = quantize5(lo1.r, pb[2]);
        ep[9] = quantize5(lo1.g, pb[2]);
        ep[10] = quantize5(lo1.b, pb[2]);
        ep[11] = quantize5(lo1.a, pb[2]);
        // Subset 1 ep1
        ep[12] = quantize5(hi1.r, pb[3]);
        ep[13] = quantize5(hi1.g, pb[3]);
        ep[14] = quantize5(hi1.b, pb[3]);
        ep[15] = quantize5(hi1.a, pb[3]);

        std::array<std::array<RGBA, 2>, 2> c;
        c[0][0] = {(i32)unq5p(ep[0], pb[0]), (i32)unq5p(ep[1], pb[0]), (i32)unq5p(ep[2], pb[0]),
                   (i32)unq5p(ep[3], pb[0])};
        c[0][1] = {(i32)unq5p(ep[4], pb[1]), (i32)unq5p(ep[5], pb[1]), (i32)unq5p(ep[6], pb[1]),
                   (i32)unq5p(ep[7], pb[1])};
        c[1][0] = {(i32)unq5p(ep[8], pb[2]), (i32)unq5p(ep[9], pb[2]), (i32)unq5p(ep[10], pb[2]),
                   (i32)unq5p(ep[11], pb[2])};
        c[1][1] = {(i32)unq5p(ep[12], pb[3]), (i32)unq5p(ep[13], pb[3]), (i32)unq5p(ep[14], pb[3]),
                   (i32)unq5p(ep[15], pb[3])};

        // 2-bit indices → 4-entry palette (RGBA)
        std::array<std::array<RGBA, 4>, 2> palette;
        for (u32 s = 0; s < 2; ++s) {
            for (u32 idx = 0; idx < 4; ++idx) {
                u32 w = BCN_WEIGHT_2[idx];
                palette[s][idx] = {(i32)bcn_interpolate(c[s][0].r, c[s][1].r, w),
                               (i32)bcn_interpolate(c[s][0].g, c[s][1].g, w),
                               (i32)bcn_interpolate(c[s][0].b, c[s][1].b, w),
                               (i32)bcn_interpolate(c[s][0].a, c[s][1].a, w)};
            }
        }

        std::array<u8, 16> indices;
        u64 total_err = 0;
        for (u32 i = 0; i < 16; ++i) {
            u8 s = part[i];
            auto px = pixel_at(rgba, i);
            u64 best_index_err = std::numeric_limits<u64>::max();
            u32 best_idx = 0;
            for (u32 idx = 0; idx < 4; ++idx) {
                u64 e = colour_error_rgba(px, palette[s][idx]);
                if (e < best_index_err) {
                    best_index_err = e;
                    best_idx = idx;
                }
            }
            indices[i] = static_cast<u8>(best_idx);
            total_err += best_index_err;
        }

        if (total_err < best_err) {
            best_err = total_err;
            best_ep = ep;
            best_pb = pb;
            best_indices = indices;
        }
    }

    // Fix anchors
    if (best_indices[0] >= 2) {
        for (u32 ch = 0; ch < 4; ++ch)
            std::swap(best_ep[ch], best_ep[4 + ch]);
        std::swap(best_pb[0], best_pb[1]);
        for (u32 i = 0; i < 16; ++i)
            if (part[i] == 0)
                best_indices[i] = static_cast<u8>(3 - best_indices[i]);
    }
    if (best_indices[anchor2] >= 2) {
        for (u32 ch = 0; ch < 4; ++ch)
            std::swap(best_ep[8 + ch], best_ep[12 + ch]);
        std::swap(best_pb[2], best_pb[3]);
        for (u32 i = 0; i < 16; ++i)
            if (part[i] == 1)
                best_indices[i] = static_cast<u8>(3 - best_indices[i]);
    }

    // Write bitstream: mode 7 = 0b10000000 (bit 7 set)
    BitWriter writer;
    writer.write(0b10000000, 8);

    // Partition: 6 bits
    writer.write(partition, 6);

    // Endpoints: R then G then B then A, 5 bits each
    // Order per channel: s0ep0 s0ep1 s1ep0 s1ep1
    writer.write(best_ep[0], 5);
    writer.write(best_ep[4], 5);
    writer.write(best_ep[8], 5);
    writer.write(best_ep[12], 5);
    writer.write(best_ep[1], 5);
    writer.write(best_ep[5], 5);
    writer.write(best_ep[9], 5);
    writer.write(best_ep[13], 5);
    writer.write(best_ep[2], 5);
    writer.write(best_ep[6], 5);
    writer.write(best_ep[10], 5);
    writer.write(best_ep[14], 5);
    writer.write(best_ep[3], 5);
    writer.write(best_ep[7], 5);
    writer.write(best_ep[11], 5);
    writer.write(best_ep[15], 5);

    // P-bits: 4 individual
    for (u32 i = 0; i < 4; ++i)
        writer.write(best_pb[i], 1);

    // Indices: 2 bits per pixel, anchors lose MSB → 1 bit
    for (u32 i = 0; i < 16; ++i) {
        bool is_anchor = (i == 0) || (i == anchor2);
        writer.write(best_indices[i], is_anchor ? 1 : 2);
    }

    std::memcpy(out, writer.data.data(), 16);
    return best_err;
}

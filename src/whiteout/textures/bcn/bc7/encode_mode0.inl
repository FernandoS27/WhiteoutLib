// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_mode0.inl
/// @brief BC7 encode: Mode 0 — 3 subsets, RGB 444 + per-endpoint P-bit, 4-bit partition, 3-bit idx.

// ============================================================================
// Mode 0 : 3 subsets, RGB 444 + per-endpoint P-bit, 4-bit partition, 3-bit idx
// ============================================================================

u64 encode_mode0_partition(const u8* rgba, u8* out, u32 partition) {
    const u8* part = BC7_PARTITION_TABLE_3[partition].data();
    u8 anchorA = BC7_ANCHOR_3A[partition];
    u8 anchorB = BC7_ANCHOR_3B[partition];

    // PCA per subset
    Vec4 axis0 = pca_axis_rgb(rgba, part, 0, 16);
    Vec4 axis1 = pca_axis_rgb(rgba, part, 1, 16);
    Vec4 axis2 = pca_axis_rgb(rgba, part, 2, 16);

    RGBA lo0, hi0, lo1, hi1, lo2, hi2;
    pca_endpoints_rgb_subset(rgba, part, 0, axis0, lo0, hi0);
    pca_endpoints_rgb_subset(rgba, part, 1, axis1, lo1, hi1);
    pca_endpoints_rgb_subset(rgba, part, 2, axis2, lo2, hi2);

    // Mode 0: 4-bit endpoints + 1 P-bit per endpoint → 5-bit effective.
    // Reconstruct: v5 = (q4 << 1) | pbit, then bc7_unquantize(v5, 5).

    auto quantize4 = [](i32 val, u32 pbit) -> u32 {
        i32 v5_target = (val * 31 + 127) / 255;
        i32 q = clamp_i((v5_target - static_cast<i32>(pbit) + 1) >> 1, 15);
        return static_cast<u32>(q);
    };

    auto unq4p = [](u32 q, u32 pbit) -> u32 {
        u32 v5 = (q << 1) | pbit;
        return bc7_unquantize(v5, 5);
    };

    u64 best_err = std::numeric_limits<u64>::max();
    std::array<u32, 18> best_ep{};
    std::array<u32, 6> best_pb{};
    std::array<u8, 16> best_indices{};

    // Try all 64 combinations of 6 P-bits
    for (u32 pmask = 0; pmask < 64; ++pmask) {
        std::array<u32, 6> pb{};
        for (u32 i = 0; i < 6; ++i)
            pb[i] = (pmask >> i) & 1;

        std::array<u32, 18> ep{};
        // Subset 0
        ep[0] = quantize4(lo0.r, pb[0]);
        ep[1] = quantize4(lo0.g, pb[0]);
        ep[2] = quantize4(lo0.b, pb[0]);
        ep[3] = quantize4(hi0.r, pb[1]);
        ep[4] = quantize4(hi0.g, pb[1]);
        ep[5] = quantize4(hi0.b, pb[1]);
        // Subset 1
        ep[6] = quantize4(lo1.r, pb[2]);
        ep[7] = quantize4(lo1.g, pb[2]);
        ep[8] = quantize4(lo1.b, pb[2]);
        ep[9] = quantize4(hi1.r, pb[3]);
        ep[10] = quantize4(hi1.g, pb[3]);
        ep[11] = quantize4(hi1.b, pb[3]);
        // Subset 2
        ep[12] = quantize4(lo2.r, pb[4]);
        ep[13] = quantize4(lo2.g, pb[4]);
        ep[14] = quantize4(lo2.b, pb[4]);
        ep[15] = quantize4(hi2.r, pb[5]);
        ep[16] = quantize4(hi2.g, pb[5]);
        ep[17] = quantize4(hi2.b, pb[5]);

        // Reconstruct
        std::array<std::array<RGBA, 2>, 3> c;
        for (u32 s = 0; s < 3; ++s) {
            for (u32 e = 0; e < 2; ++e) {
                u32 base_idx = (s * 2 + e) * 3;
                u32 pi = s * 2 + e;
                c[s][e] = {(i32)unq4p(ep[base_idx + 0], pb[pi]), (i32)unq4p(ep[base_idx + 1], pb[pi]),
                           (i32)unq4p(ep[base_idx + 2], pb[pi]), 0};
            }
        }

        // 3-bit indices → 8-entry palette per subset
        std::array<std::array<RGBA, 8>, 3> palette;
        for (u32 s = 0; s < 3; ++s) {
            for (u32 idx = 0; idx < 8; ++idx) {
                u32 w = BCN_WEIGHT_3[idx];
                palette[s][idx] = {(i32)bcn_interpolate(c[s][0].r, c[s][1].r, w),
                               (i32)bcn_interpolate(c[s][0].g, c[s][1].g, w),
                               (i32)bcn_interpolate(c[s][0].b, c[s][1].b, w), 0};
            }
        }

        std::array<u8, 16> indices{};
        u64 total_err = 0;
        for (u32 i = 0; i < 16; ++i) {
            u8 s = part[i];
            auto px = pixel_at(rgba, i);
            u64 best_index_err = std::numeric_limits<u64>::max();
            u32 best_idx = 0;
            for (u32 idx = 0; idx < 8; ++idx) {
                u64 e = colour_error_rgb(px, palette[s][idx]);
                if (e < best_index_err) {
                    best_index_err = e;
                    best_idx = idx;
                }
            }
            indices[i] = static_cast<u8>(best_idx);
            total_err += best_index_err;
            // Modes without alpha encode → decoder outputs 255
            total_err += bcn_sq(static_cast<i32>(rgba[i * 4 + 3]) - 255);
        }

        if (total_err < best_err) {
            best_err = total_err;
            best_ep = ep;
            best_pb = pb;
            best_indices = indices;
        }
    }

    // Fix anchors (3 anchors: pixel 0, anchorA, anchorB)
    if (best_indices[0] >= 4) {
        std::swap(best_ep[0], best_ep[3]);
        std::swap(best_ep[1], best_ep[4]);
        std::swap(best_ep[2], best_ep[5]);
        std::swap(best_pb[0], best_pb[1]);
        for (u32 i = 0; i < 16; ++i)
            if (part[i] == 0)
                best_indices[i] = static_cast<u8>(7 - best_indices[i]);
    }
    if (best_indices[anchorA] >= 4) {
        std::swap(best_ep[6], best_ep[9]);
        std::swap(best_ep[7], best_ep[10]);
        std::swap(best_ep[8], best_ep[11]);
        std::swap(best_pb[2], best_pb[3]);
        for (u32 i = 0; i < 16; ++i)
            if (part[i] == 1)
                best_indices[i] = static_cast<u8>(7 - best_indices[i]);
    }
    if (best_indices[anchorB] >= 4) {
        std::swap(best_ep[12], best_ep[15]);
        std::swap(best_ep[13], best_ep[16]);
        std::swap(best_ep[14], best_ep[17]);
        std::swap(best_pb[4], best_pb[5]);
        for (u32 i = 0; i < 16; ++i)
            if (part[i] == 2)
                best_indices[i] = static_cast<u8>(7 - best_indices[i]);
    }

    // Write bitstream: mode 0 = 0b1 (bit 0 set)
    BitWriter writer;
    writer.write(0b1, 1);

    // Partition: 4 bits
    writer.write(partition, 4);

    // Endpoints: R then G then B, 4 bits each
    // Order per channel: s0ep0 s0ep1 s1ep0 s1ep1 s2ep0 s2ep1
    writer.write(best_ep[0], 4);
    writer.write(best_ep[3], 4);
    writer.write(best_ep[6], 4);
    writer.write(best_ep[9], 4);
    writer.write(best_ep[12], 4);
    writer.write(best_ep[15], 4);
    writer.write(best_ep[1], 4);
    writer.write(best_ep[4], 4);
    writer.write(best_ep[7], 4);
    writer.write(best_ep[10], 4);
    writer.write(best_ep[13], 4);
    writer.write(best_ep[16], 4);
    writer.write(best_ep[2], 4);
    writer.write(best_ep[5], 4);
    writer.write(best_ep[8], 4);
    writer.write(best_ep[11], 4);
    writer.write(best_ep[14], 4);
    writer.write(best_ep[17], 4);

    // P-bits: 6 individual (s0ep0 s0ep1 s1ep0 s1ep1 s2ep0 s2ep1)
    for (u32 i = 0; i < 6; ++i)
        writer.write(best_pb[i], 1);

    // Indices: 3 bits per pixel, anchors lose MSB → 2 bits
    for (u32 i = 0; i < 16; ++i) {
        bool is_anchor = (i == 0) || (i == anchorA) || (i == anchorB);
        writer.write(best_indices[i], is_anchor ? 2 : 3);
    }

    std::memcpy(out, writer.data.data(), 16);
    return best_err;
}

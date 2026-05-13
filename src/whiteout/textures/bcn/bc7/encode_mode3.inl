// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_mode3.inl
/// @brief BC7 encode: Mode 3 — 2 subsets, RGB 777 + per-endpoint P-bit, 2-bit indices.

// ============================================================================
// Mode 3 : 2 subsets, RGB 777 + per-endpoint P-bit, 2-bit indices
// ============================================================================

u64 encode_mode3_partition(const u8* rgba, u8* out, u32 partition) {
    const u8* part = BC7_PARTITION_TABLE_2[partition].data();
    u8 anchor2 = BC7_ANCHOR_2[partition];

    Vec4 axis0 = pca_axis_rgb(rgba, part, 0, 16);
    Vec4 axis1 = pca_axis_rgb(rgba, part, 1, 16);

    RGBA lo0, hi0, lo1, hi1;
    pca_endpoints_rgb_subset(rgba, part, 0, axis0, lo0, hi0);
    pca_endpoints_rgb_subset(rgba, part, 1, axis1, lo1, hi1);

    // Mode 3: 7-bit endpoints + 1 P-bit per endpoint → 8-bit effective.

    auto quantize7 = [](i32 val, u32 pbit) -> u32 {
        i32 q = clamp_i(((val - static_cast<i32>(pbit)) + 1) >> 1, 127);
        return static_cast<u32>(q);
    };

    auto unq7p = [](u32 q, u32 pbit) -> u32 { return (q << 1) | pbit; };

    u64 best_err = std::numeric_limits<u64>::max();
    std::array<u32, 12> best_ep{};
    std::array<u32, 4> best_pb{};
    std::array<u8, 16> best_indices{};

    // Try all 16 combinations of 4 P-bits
    for (u32 pmask = 0; pmask < 16; ++pmask) {
        std::array<u32, 4> pb = {pmask & 1, (pmask >> 1) & 1, (pmask >> 2) & 1, (pmask >> 3) & 1};

        std::array<u32, 12> ep{};
        ep[0] = quantize7(lo0.r, pb[0]);
        ep[1] = quantize7(lo0.g, pb[0]);
        ep[2] = quantize7(lo0.b, pb[0]);
        ep[3] = quantize7(hi0.r, pb[1]);
        ep[4] = quantize7(hi0.g, pb[1]);
        ep[5] = quantize7(hi0.b, pb[1]);
        ep[6] = quantize7(lo1.r, pb[2]);
        ep[7] = quantize7(lo1.g, pb[2]);
        ep[8] = quantize7(lo1.b, pb[2]);
        ep[9] = quantize7(hi1.r, pb[3]);
        ep[10] = quantize7(hi1.g, pb[3]);
        ep[11] = quantize7(hi1.b, pb[3]);

        std::array<std::array<RGBA, 2>, 2> c;
        c[0][0] = {(i32)unq7p(ep[0], pb[0]), (i32)unq7p(ep[1], pb[0]), (i32)unq7p(ep[2], pb[0]), 0};
        c[0][1] = {(i32)unq7p(ep[3], pb[1]), (i32)unq7p(ep[4], pb[1]), (i32)unq7p(ep[5], pb[1]), 0};
        c[1][0] = {(i32)unq7p(ep[6], pb[2]), (i32)unq7p(ep[7], pb[2]), (i32)unq7p(ep[8], pb[2]), 0};
        c[1][1] = {(i32)unq7p(ep[9], pb[3]), (i32)unq7p(ep[10], pb[3]), (i32)unq7p(ep[11], pb[3]),
                   0};

        // 2-bit indices → 4-entry palette
        std::array<std::array<RGBA, 4>, 2> palette;
        for (u32 s = 0; s < 2; ++s) {
            for (u32 idx = 0; idx < 4; ++idx) {
                u32 w = BCN_WEIGHT_2[idx];
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
            for (u32 idx = 0; idx < 4; ++idx) {
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

    // Fix anchors
    if (best_indices[0] >= 2) {
        std::swap(best_ep[0], best_ep[3]);
        std::swap(best_ep[1], best_ep[4]);
        std::swap(best_ep[2], best_ep[5]);
        std::swap(best_pb[0], best_pb[1]);
        for (u32 i = 0; i < 16; ++i) {
            if (part[i] == 0)
                best_indices[i] = static_cast<u8>(3 - best_indices[i]);
        }
    }
    if (best_indices[anchor2] >= 2) {
        std::swap(best_ep[6], best_ep[9]);
        std::swap(best_ep[7], best_ep[10]);
        std::swap(best_ep[8], best_ep[11]);
        std::swap(best_pb[2], best_pb[3]);
        for (u32 i = 0; i < 16; ++i) {
            if (part[i] == 1)
                best_indices[i] = static_cast<u8>(3 - best_indices[i]);
        }
    }

    // Write bitstream: mode 3 = 0b1000 (bit 3 set)
    BitWriter writer;
    writer.write(0b1000, 4);

    // Partition: 6 bits
    writer.write(partition, 6);

    // Endpoints: 7 bits each, same order as mode 1 but wider
    writer.write(best_ep[0], 7);
    writer.write(best_ep[3], 7);
    writer.write(best_ep[6], 7);
    writer.write(best_ep[9], 7);
    writer.write(best_ep[1], 7);
    writer.write(best_ep[4], 7);
    writer.write(best_ep[7], 7);
    writer.write(best_ep[10], 7);
    writer.write(best_ep[2], 7);
    writer.write(best_ep[5], 7);
    writer.write(best_ep[8], 7);
    writer.write(best_ep[11], 7);

    // P-bits: 4 individual P-bits
    writer.write(best_pb[0], 1);
    writer.write(best_pb[1], 1);
    writer.write(best_pb[2], 1);
    writer.write(best_pb[3], 1);

    // Indices: 2 bits per pixel, anchors lose MSB → 1 bit
    for (u32 i = 0; i < 16; ++i) {
        bool is_anchor = (i == 0) || (i == anchor2);
        writer.write(best_indices[i], is_anchor ? 1 : 2);
    }

    std::memcpy(out, writer.data.data(), 16);
    return best_err;
}

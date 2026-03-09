// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_mode2.inl
/// @brief BC7 encode: Mode 2 — 3 subsets, RGB 555, no P-bits, 6-bit partition, 2-bit indices.

// ============================================================================
// Mode 2 : 3 subsets, RGB 555, no P-bits, 6-bit partition, 2-bit indices
// ============================================================================

u64 encode_mode2_partition(const u8* rgba, u8* out, u32 partition) {
    const u8* part = BC7_PARTITION_TABLE_3[partition].data();
    u8 anchorA = BC7_ANCHOR_3A[partition];
    u8 anchorB = BC7_ANCHOR_3B[partition];

    Vec4 axis0 = pca_axis_rgb(rgba, part, 0, 16);
    Vec4 axis1 = pca_axis_rgb(rgba, part, 1, 16);
    Vec4 axis2 = pca_axis_rgb(rgba, part, 2, 16);

    RGBA lo0, hi0, lo1, hi1, lo2, hi2;
    pca_endpoints_rgb_subset(rgba, part, 0, axis0, lo0, hi0);
    pca_endpoints_rgb_subset(rgba, part, 1, axis1, lo1, hi1);
    pca_endpoints_rgb_subset(rgba, part, 2, axis2, lo2, hi2);

    // Quantize to 5 bits, no P-bits
    std::array<u32, 18> ep;
    ep[0] = quantize(lo0.r, 5);
    ep[1] = quantize(lo0.g, 5);
    ep[2] = quantize(lo0.b, 5);
    ep[3] = quantize(hi0.r, 5);
    ep[4] = quantize(hi0.g, 5);
    ep[5] = quantize(hi0.b, 5);
    ep[6] = quantize(lo1.r, 5);
    ep[7] = quantize(lo1.g, 5);
    ep[8] = quantize(lo1.b, 5);
    ep[9] = quantize(hi1.r, 5);
    ep[10] = quantize(hi1.g, 5);
    ep[11] = quantize(hi1.b, 5);
    ep[12] = quantize(lo2.r, 5);
    ep[13] = quantize(lo2.g, 5);
    ep[14] = quantize(lo2.b, 5);
    ep[15] = quantize(hi2.r, 5);
    ep[16] = quantize(hi2.g, 5);
    ep[17] = quantize(hi2.b, 5);

    // Reconstruct and build palettes (2-bit → 4 entries)
    std::array<std::array<RGBA, 2>, 3> c;
    for (u32 s = 0; s < 3; ++s) {
        for (u32 e = 0; e < 2; ++e) {
            u32 base_idx = (s * 2 + e) * 3;
            c[s][e] = {(i32)bc7_unquantize(ep[base_idx + 0], 5), (i32)bc7_unquantize(ep[base_idx + 1], 5),
                       (i32)bc7_unquantize(ep[base_idx + 2], 5), 0};
        }
    }

    std::array<std::array<RGBA, 4>, 3> palette;
    for (u32 s = 0; s < 3; ++s) {
        for (u32 idx = 0; idx < 4; ++idx) {
            u32 w = BCN_WEIGHT_2[idx];
            palette[s][idx] = {(i32)bcn_interpolate(c[s][0].r, c[s][1].r, w),
                           (i32)bcn_interpolate(c[s][0].g, c[s][1].g, w),
                           (i32)bcn_interpolate(c[s][0].b, c[s][1].b, w), 0};
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

    // Fix anchors
    if (indices[0] >= 2) {
        std::swap(ep[0], ep[3]);
        std::swap(ep[1], ep[4]);
        std::swap(ep[2], ep[5]);
        for (u32 i = 0; i < 16; ++i)
            if (part[i] == 0)
                indices[i] = static_cast<u8>(3 - indices[i]);
    }
    if (indices[anchorA] >= 2) {
        std::swap(ep[6], ep[9]);
        std::swap(ep[7], ep[10]);
        std::swap(ep[8], ep[11]);
        for (u32 i = 0; i < 16; ++i)
            if (part[i] == 1)
                indices[i] = static_cast<u8>(3 - indices[i]);
    }
    if (indices[anchorB] >= 2) {
        std::swap(ep[12], ep[15]);
        std::swap(ep[13], ep[16]);
        std::swap(ep[14], ep[17]);
        for (u32 i = 0; i < 16; ++i)
            if (part[i] == 2)
                indices[i] = static_cast<u8>(3 - indices[i]);
    }

    // Write bitstream: mode 2 = 0b100 (bit 2 set)
    BitWriter writer;
    writer.write(0b100, 3);

    // Partition: 6 bits
    writer.write(partition, 6);

    // Endpoints: R then G then B, 5 bits each
    writer.write(ep[0], 5);
    writer.write(ep[3], 5);
    writer.write(ep[6], 5);
    writer.write(ep[9], 5);
    writer.write(ep[12], 5);
    writer.write(ep[15], 5);
    writer.write(ep[1], 5);
    writer.write(ep[4], 5);
    writer.write(ep[7], 5);
    writer.write(ep[10], 5);
    writer.write(ep[13], 5);
    writer.write(ep[16], 5);
    writer.write(ep[2], 5);
    writer.write(ep[5], 5);
    writer.write(ep[8], 5);
    writer.write(ep[11], 5);
    writer.write(ep[14], 5);
    writer.write(ep[17], 5);

    // Indices: 2 bits per pixel, anchors lose MSB → 1 bit
    for (u32 i = 0; i < 16; ++i) {
        bool is_anchor = (i == 0) || (i == anchorA) || (i == anchorB);
        writer.write(indices[i], is_anchor ? 1 : 2);
    }

    std::memcpy(out, writer.data.data(), 16);
    return total_err;
}

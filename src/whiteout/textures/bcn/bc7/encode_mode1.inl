// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_mode1.inl
/// @brief BC7 encode: Mode 1 — 2 subsets, RGB 666 + shared P-bit, 3-bit indices.

// ============================================================================
// Mode 1 : 2 subsets, RGB 666 + shared P-bit, 3-bit indices
// ============================================================================

u64 encode_mode1_partition(const u8* rgba, u8* out, u32 partition) {
    const u8* part = BC7_PARTITION_TABLE_2[partition].data();
    u8 anchor2 = BC7_ANCHOR_2[partition];

    // PCA per subset
    Vec4 axis0 = pca_axis_rgb(rgba, part, 0, 16);
    Vec4 axis1 = pca_axis_rgb(rgba, part, 1, 16);

    RGBA lo0, hi0, lo1, hi1;
    pca_endpoints_rgb_subset(rgba, part, 0, axis0, lo0, hi0);
    pca_endpoints_rgb_subset(rgba, part, 1, axis1, lo1, hi1);

    // Mode 1: 6-bit endpoints + 1 shared P-bit per subset pair
    // Shared P-bit → both endpoints in a subset get the same P-bit.
    // Effective precision = 7 bits: (q6 << 1) | pbit.

    auto quantize6 = [](i32 val, u32 pbit) -> u32 {
        i32 q = clamp_i(((val * 2 - static_cast<i32>(pbit)) + 1) / 4, 63);
        // Reconstruct check
        return static_cast<u32>(clamp_i(q, 63));
    };

    auto unq6p = [](u32 q, u32 pbit) -> u32 {
        u32 v8 = (q << 1) | pbit;
        // 7-bit → 8-bit: replicate top bit
        return (v8 << 1) | (v8 >> 6);
    };

    u64 best_err = std::numeric_limits<u64>::max();
    std::array<u32, 12> best_ep{}; // r0a g0a b0a r1a g1a b1a  r0b g0b b0b r1b g1b b1b
    std::array<u32, 2> best_pb{};
    std::array<u8, 16> best_indices{};

    // Try both shared P-bit values for each subset
    for (u32 pb0 = 0; pb0 < 2; ++pb0) {
        for (u32 pb1 = 0; pb1 < 2; ++pb1) {
            std::array<u32, 12> ep{};
            ep[0] = quantize6(lo0.r, pb0);
            ep[1] = quantize6(lo0.g, pb0);
            ep[2] = quantize6(lo0.b, pb0);
            ep[3] = quantize6(hi0.r, pb0);
            ep[4] = quantize6(hi0.g, pb0);
            ep[5] = quantize6(hi0.b, pb0);
            ep[6] = quantize6(lo1.r, pb1);
            ep[7] = quantize6(lo1.g, pb1);
            ep[8] = quantize6(lo1.b, pb1);
            ep[9] = quantize6(hi1.r, pb1);
            ep[10] = quantize6(hi1.g, pb1);
            ep[11] = quantize6(hi1.b, pb1);

            // Reconstruct 8-bit colours for both subsets
            std::array<std::array<RGBA, 2>, 2> c; // [subset][endpoint]
            c[0][0] = {(i32)unq6p(ep[0], pb0), (i32)unq6p(ep[1], pb0), (i32)unq6p(ep[2], pb0), 0};
            c[0][1] = {(i32)unq6p(ep[3], pb0), (i32)unq6p(ep[4], pb0), (i32)unq6p(ep[5], pb0), 0};
            c[1][0] = {(i32)unq6p(ep[6], pb1), (i32)unq6p(ep[7], pb1), (i32)unq6p(ep[8], pb1), 0};
            c[1][1] = {(i32)unq6p(ep[9], pb1), (i32)unq6p(ep[10], pb1), (i32)unq6p(ep[11], pb1), 0};

            // Build palettes (8 entries each, 3-bit)
            std::array<std::array<RGBA, 8>, 2> palette;
            for (u32 s = 0; s < 2; ++s) {
                for (u32 idx = 0; idx < 8; ++idx) {
                    u32 w = BCN_WEIGHT_3[idx];
                    palette[s][idx] = {(i32)bcn_interpolate(c[s][0].r, c[s][1].r, w),
                                   (i32)bcn_interpolate(c[s][0].g, c[s][1].g, w),
                                   (i32)bcn_interpolate(c[s][0].b, c[s][1].b, w), 0};
                }
            }

            // Assign indices
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
                best_pb[0] = pb0;
                best_pb[1] = pb1;
                best_indices = indices;
            }
        }
    }

    // Fix anchors: pixel 0 (subset 0 anchor) and anchor2 (subset 1 anchor)
    if (best_indices[0] >= 4) {
        // Swap subset 0 endpoints
        std::swap(best_ep[0], best_ep[3]);
        std::swap(best_ep[1], best_ep[4]);
        std::swap(best_ep[2], best_ep[5]);
        for (u32 i = 0; i < 16; ++i) {
            if (part[i] == 0)
                best_indices[i] = static_cast<u8>(7 - best_indices[i]);
        }
    }
    if (best_indices[anchor2] >= 4) {
        std::swap(best_ep[6], best_ep[9]);
        std::swap(best_ep[7], best_ep[10]);
        std::swap(best_ep[8], best_ep[11]);
        for (u32 i = 0; i < 16; ++i) {
            if (part[i] == 1)
                best_indices[i] = static_cast<u8>(7 - best_indices[i]);
        }
    }

    // Write bitstream: mode 1 = 0b10 (bit 1 set)
    BitWriter writer;
    writer.write(0b10, 2);

    // Partition: 6 bits
    writer.write(partition, 6);

    // Endpoints: r0a r0b g0a g0b b0a b0b r1a r1b g1a g1b b1a b1b — 6 bits each
    // Order: for each channel: e0_subset0, e0_subset1, e1_subset0, e1_subset1
    // Actually BC7 mode 1 layout:
    //   R[0].ep0, R[0].ep1, R[1].ep0, R[1].ep1  (6 bits each)
    //   G same, B same
    writer.write(best_ep[0], 6);  // R subset0 ep0
    writer.write(best_ep[3], 6);  // R subset0 ep1
    writer.write(best_ep[6], 6);  // R subset1 ep0
    writer.write(best_ep[9], 6);  // R subset1 ep1
    writer.write(best_ep[1], 6);  // G subset0 ep0
    writer.write(best_ep[4], 6);  // G subset0 ep1
    writer.write(best_ep[7], 6);  // G subset1 ep0
    writer.write(best_ep[10], 6); // G subset1 ep1
    writer.write(best_ep[2], 6);  // B subset0 ep0
    writer.write(best_ep[5], 6);  // B subset0 ep1
    writer.write(best_ep[8], 6);  // B subset1 ep0
    writer.write(best_ep[11], 6); // B subset1 ep1

    // Shared P-bits: 1 per subset
    writer.write(best_pb[0], 1);
    writer.write(best_pb[1], 1);

    // Indices: 3 bits per pixel, anchor pixels lose MSB
    for (u32 i = 0; i < 16; ++i) {
        bool is_anchor = (i == 0) || (i == anchor2);
        writer.write(best_indices[i], is_anchor ? 2 : 3);
    }

    std::memcpy(out, writer.data.data(), 16);
    return best_err;
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_mode4.inl
/// @brief BC7 encode: Mode 4 — 1 subset, RGB 555 + A 6, rotation, idx selection, 2+3 bit indices.

// ============================================================================
// Mode 4 : 1 subset, RGB 555 + A 6, rotation, idx selection, 2+3 bit indices
// ============================================================================

u64 encode_mode4(const u8* rgba, u8* out, bool use_pca) {
    // Mode 4: similar to mode 5 but lower-precision endpoints and an
    // index-selection bit that swaps which index set (2-bit vs 3-bit)
    // is used for colour vs alpha.  We try rotation = 0 only.

    RGBA min_color, max_color;
    if (use_pca) {
        std::array<u8, 16> mask{};
        Vec4 axis = pca_axis_rgb(rgba, mask.data(), 0, 16);
        pca_endpoints_rgb_subset(rgba, mask.data(), 0, axis, min_color, max_color);
    } else {
        bbox_endpoints_rgba(rgba, min_color, max_color);
    }

    // Alpha range
    i32 alo = 255, ahi = 0;
    for (u32 i = 0; i < 16; ++i) {
        i32 a = rgba[i * 4 + 3];
        alo = std::min(alo, a);
        ahi = std::max(ahi, a);
    }

    // Quantize RGB to 5 bits, A to 6 bits
    u32 r0 = quantize(min_color.r, 5), g0 = quantize(min_color.g, 5), b0 = quantize(min_color.b, 5);
    u32 r1 = quantize(max_color.r, 5), g1 = quantize(max_color.g, 5), b1 = quantize(max_color.b, 5);
    u32 a0 = quantize(alo, 6);
    u32 a1 = quantize(ahi, 6);

    // Reconstruct
    RGBA c0{(i32)bc7_unquantize(r0, 5), (i32)bc7_unquantize(g0, 5), (i32)bc7_unquantize(b0, 5),
            (i32)bc7_unquantize(a0, 6)};
    RGBA c1{(i32)bc7_unquantize(r1, 5), (i32)bc7_unquantize(g1, 5), (i32)bc7_unquantize(b1, 5),
            (i32)bc7_unquantize(a1, 6)};

    u64 best_total_err = std::numeric_limits<u64>::max();
    std::array<u8, 16> best_out{};

    for (u32 idx_sel = 0; idx_sel < 2; ++idx_sel) {
        // idx_sel=0: colour uses 2-bit indices, alpha uses 3-bit
        // idx_sel=1: colour uses 3-bit indices, alpha uses 2-bit
        const u32 color_count = idx_sel == 0 ? 4u : 8u;
        const u32 alpha_count = idx_sel == 0 ? 8u : 4u;
        const u32* color_weights = idx_sel == 0 ? BCN_WEIGHT_2.data() : BCN_WEIGHT_3.data();
        const u32* alpha_weights = idx_sel == 0 ? BCN_WEIGHT_3.data() : BCN_WEIGHT_2.data();
        const u32 color_bits = idx_sel == 0 ? 2u : 3u;
        const u32 alpha_bits = idx_sel == 0 ? 3u : 2u;

        // Build palettes
        std::array<RGBA, 8> color_palette;
        for (u32 idx = 0; idx < color_count; ++idx) {
            u32 w = color_weights[idx];
            color_palette[idx] = {(i32)bcn_interpolate(c0.r, c1.r, w), (i32)bcn_interpolate(c0.g, c1.g, w),
                         (i32)bcn_interpolate(c0.b, c1.b, w), 0};
        }
        std::array<u32, 8> alpha_palette;
        for (u32 idx = 0; idx < alpha_count; ++idx) {
            u32 w = alpha_weights[idx];
            alpha_palette[idx] = bcn_interpolate(c0.a, c1.a, w);
        }

        // Assign indices
        std::array<u8, 16> color_indices;
        std::array<u8, 16> alpha_indices;
        u64 total_err = 0;
        for (u32 i = 0; i < 16; ++i) {
            auto px = pixel_at(rgba, i);
            u64 best_color_err = std::numeric_limits<u64>::max();
            u32 best_color_idx = 0;
            for (u32 j = 0; j < color_count; ++j) {
                u64 e = colour_error_rgb(px, color_palette[j]);
                if (e < best_color_err) {
                    best_color_err = e;
                    best_color_idx = j;
                }
            }
            color_indices[i] = static_cast<u8>(best_color_idx);

            u64 best_alpha_err = std::numeric_limits<u64>::max();
            u32 best_alpha_idx = 0;
            for (u32 j = 0; j < alpha_count; ++j) {
                u64 e = bcn_sq(px.a - static_cast<i32>(alpha_palette[j]));
                if (e < best_alpha_err) {
                    best_alpha_err = e;
                    best_alpha_idx = j;
                }
            }
            alpha_indices[i] = static_cast<u8>(best_alpha_idx);
            total_err += best_color_err + best_alpha_err;
        }

        // Temporary endpoint copies for anchor fixing
        u32 tr0 = r0, tg0 = g0, tb0 = b0, ta0 = a0;
        u32 tr1 = r1, tg1 = g1, tb1 = b1, ta1 = a1;

        // Fix colour anchor (pixel 0)
        if (color_indices[0] >= color_count / 2) {
            std::swap(tr0, tr1);
            std::swap(tg0, tg1);
            std::swap(tb0, tb1);
            for (u32 i = 0; i < 16; ++i)
                color_indices[i] = static_cast<u8>((color_count - 1) - color_indices[i]);
        }
        // Fix alpha anchor (pixel 0)
        if (alpha_indices[0] >= alpha_count / 2) {
            std::swap(ta0, ta1);
            for (u32 i = 0; i < 16; ++i)
                alpha_indices[i] = static_cast<u8>((alpha_count - 1) - alpha_indices[i]);
        }

        if (total_err < best_total_err) {
            best_total_err = total_err;

            BitWriter writer;
            writer.write(0b10000, 5); // mode 4
            writer.write(0, 2);       // rotation = 0
            writer.write(idx_sel, 1);

            // Endpoints
            writer.write(tr0, 5);
            writer.write(tr1, 5);
            writer.write(tg0, 5);
            writer.write(tg1, 5);
            writer.write(tb0, 5);
            writer.write(tb1, 5);
            writer.write(ta0, 6);
            writer.write(ta1, 6);

            // Index data 0: always the 2-bit set (31 bits)
            // Index data 1: always the 3-bit set (47 bits)
            if (idx_sel == 0) {
                // 2-bit = colour, 3-bit = alpha
                writer.write(color_indices[0], color_bits - 1);
                for (u32 i = 1; i < 16; ++i)
                    writer.write(color_indices[i], color_bits);
                writer.write(alpha_indices[0], alpha_bits - 1);
                for (u32 i = 1; i < 16; ++i)
                    writer.write(alpha_indices[i], alpha_bits);
            } else {
                // 2-bit = alpha, 3-bit = colour
                writer.write(alpha_indices[0], alpha_bits - 1);
                for (u32 i = 1; i < 16; ++i)
                    writer.write(alpha_indices[i], alpha_bits);
                writer.write(color_indices[0], color_bits - 1);
                for (u32 i = 1; i < 16; ++i)
                    writer.write(color_indices[i], color_bits);
            }

            std::memcpy(best_out.data(), writer.data.data(), 16);
        }
    }

    std::memcpy(out, best_out.data(), 16);
    return best_total_err;
}

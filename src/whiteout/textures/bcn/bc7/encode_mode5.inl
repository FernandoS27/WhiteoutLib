// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_mode5.inl
/// @brief BC7 encode: Mode 5 — 1 subset, RGB 777 + A 8, rotation, 2-bit colour + 2-bit alpha idx.

// ============================================================================
// Mode 5 : 1 subset, RGB 777 + A 8, rotation, 2-bit colour + 2-bit alpha idx
// ============================================================================

u64 encode_mode5(const u8* rgba, u8* out, bool use_pca) {
    // Mode 5: 1 subset, no partition bits, 2-bit rotation.
    // Endpoints: RGB 7.7.7 per endpoint, A 8 per endpoint.
    // Indices: 2-bit for colour, 3-bit for alpha (separate).
    // Rotation field can swap a channel with alpha before indexing.

    // We'll try rotation = 0 (no swap) only for speed in Normal mode.
    // In High mode the caller can try all 4 rotations externally (not implemented
    // here for simplicity; rotation 0 is generally good enough).

    RGBA min_color, max_color;
    if (use_pca) {
        // PCA on RGB
        std::array<u8, 16> mask{};
        Vec4 axis = pca_axis_rgb(rgba, mask.data(), 0, 16);
        pca_endpoints_rgb_subset(rgba, mask.data(), 0, axis, min_color, max_color);
    } else {
        bbox_endpoints_rgba(rgba, min_color, max_color);
    }

    // Also find alpha range
    i32 alo = 255, ahi = 0;
    for (u32 i = 0; i < 16; ++i) {
        i32 a = rgba[i * 4 + 3];
        alo = std::min(alo, a);
        ahi = std::max(ahi, a);
    }

    // Quantize RGB to 7 bits, A to 8 bits
    u32 r0 = quantize(min_color.r, 7), g0 = quantize(min_color.g, 7), b0 = quantize(min_color.b, 7);
    u32 r1 = quantize(max_color.r, 7), g1 = quantize(max_color.g, 7), b1 = quantize(max_color.b, 7);
    u32 a0 = static_cast<u32>(clamp_i(alo, 255));
    u32 a1 = static_cast<u32>(clamp_i(ahi, 255));

    // Reconstruct
    RGBA c0{static_cast<i32>(bc7_unquantize(r0, 7)), static_cast<i32>(bc7_unquantize(g0, 7)),
            static_cast<i32>(bc7_unquantize(b0, 7)), static_cast<i32>(a0)};
    RGBA c1{static_cast<i32>(bc7_unquantize(r1, 7)), static_cast<i32>(bc7_unquantize(g1, 7)),
            static_cast<i32>(bc7_unquantize(b1, 7)), static_cast<i32>(a1)};

    // Build colour palette (4 entries, 2-bit)
    std::array<RGBA, 4> color_palette;
    for (u32 idx = 0; idx < 4; ++idx) {
        u32 w = BCN_WEIGHT_2[idx];
        color_palette[idx] = {static_cast<i32>(bcn_interpolate(c0.r, c1.r, w)),
                     static_cast<i32>(bcn_interpolate(c0.g, c1.g, w)),
                     static_cast<i32>(bcn_interpolate(c0.b, c1.b, w)), 0};
    }

    // Build alpha palette (4 entries, 2-bit)
    std::array<u32, 4> alpha_palette{};
    for (u32 idx = 0; idx < 4; ++idx) {
        u32 w = BCN_WEIGHT_2[idx];
        alpha_palette[idx] = bcn_interpolate(a0, a1, w);
    }

    // Assign indices
    std::array<u8, 16> color_indices{};
    std::array<u8, 16> alpha_indices{};
    u64 total_err = 0;
    for (u32 i = 0; i < 16; ++i) {
        auto px = pixel_at(rgba, i);

        // Colour index (RGB error only)
        u64 best_color_err = std::numeric_limits<u64>::max();
        u32 best_color_idx = 0;
        for (u32 j = 0; j < 4; ++j) {
            u64 e = colour_error_rgb(px, color_palette[j]);
            if (e < best_color_err) {
                best_color_err = e;
                best_color_idx = j;
            }
        }
        color_indices[i] = static_cast<u8>(best_color_idx);

        // Alpha index
        u64 best_alpha_err = std::numeric_limits<u64>::max();
        u32 best_alpha_idx = 0;
        for (u32 j = 0; j < 4; ++j) {
            u64 e = bcn_sq(px.a - static_cast<i32>(alpha_palette[j]));
            if (e < best_alpha_err) {
                best_alpha_err = e;
                best_alpha_idx = j;
            }
        }
        alpha_indices[i] = static_cast<u8>(best_alpha_idx);
        total_err += best_color_err + best_alpha_err;
    }

    // Fix anchors: pixel 0 for both colour and alpha indices.
    if (color_indices[0] >= 2) {
        std::swap(r0, r1);
        std::swap(g0, g1);
        std::swap(b0, b1);
        for (u32 i = 0; i < 16; ++i)
            color_indices[i] = static_cast<u8>(3 - color_indices[i]);
    }
    if (alpha_indices[0] >= 2) {
        std::swap(a0, a1);
        for (u32 i = 0; i < 16; ++i)
            alpha_indices[i] = static_cast<u8>(3 - alpha_indices[i]);
    }

    // Write bitstream
    BitWriter writer;
    writer.write(0b100000, 6); // mode 5: bit 5 set

    // Rotation: 2 bits (0 = no swap)
    writer.write(0, 2);

    // Endpoints: r0 r1 g0 g1 b0 b1 (7 bits each), a0 a1 (8 bits each)
    writer.write(r0, 7);
    writer.write(r1, 7);
    writer.write(g0, 7);
    writer.write(g1, 7);
    writer.write(b0, 7);
    writer.write(b1, 7);
    writer.write(a0, 8);
    writer.write(a1, 8);

    // Colour indices: pixel 0 = 1 bit (anchor), rest = 2 bits
    writer.write(color_indices[0], 1);
    for (u32 i = 1; i < 16; ++i)
        writer.write(color_indices[i], 2);

    // Alpha indices: pixel 0 = 1 bit (anchor), rest = 2 bits
    writer.write(alpha_indices[0], 1);
    for (u32 i = 1; i < 16; ++i)
        writer.write(alpha_indices[i], 2);

    std::memcpy(out, writer.data.data(), 16);
    return total_err;
}

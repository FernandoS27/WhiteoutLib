// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_mode6.inl
/// @brief BC7 encode: Mode 6 — 1 subset, RGBA 7777+P, 4-bit indices (64 levels).

// ============================================================================
// Mode 6 : 1 subset, RGBA 7777+P, 4-bit indices (64 levels)
// ============================================================================

/// Encode a block with mode 6.  Returns total squared error.
u64 encode_mode6(const u8* rgba, u8* out, bool use_pca) {
    RGBA min_color, max_color;
    if (use_pca) {
        Vec4 axis = pca_axis_rgba(rgba, 16);
        pca_endpoints_rgba(rgba, axis, min_color, max_color);
    } else {
        bbox_endpoints_rgba(rgba, min_color, max_color);
    }

    // In mode 6, endpoints are 7 bits + 1 P-bit → effective 8 bits.
    // We quantize to 7 bits; the P-bit extends to 8 bits.
    // Try both P-bit = 0 and P-bit = 1 for each endpoint.
    struct Endpoint8 {
        u32 r, g, b, a; // 7-bit quantized
        u32 pbit;       // 0 or 1
    };

    auto quantize7 = [](i32 val, u32 pbit) -> u32 {
        // 7-bit value, P-bit appended makes it 8-bit: (val << 1) | pbit, replicated.
        // We want the 8-bit reconstructed value closest to `val` (0..255).
        // Reconstruct: (q7 << 1) | p.
        // So target q7 = (val - p) / 2, clamped.
        i32 q = clamp_i(((val - static_cast<i32>(pbit)) + 1) >> 1, 127);
        return static_cast<u32>(q);
    };

    auto unq7 = [](u32 q, u32 pbit) -> u32 { return (q << 1) | pbit; };

    // Try all 4 combinations of (p0, p1)
    u64 best_err = std::numeric_limits<u64>::max();
    Endpoint8 best_e0{}, best_e1{};
    std::array<u8, 16> best_indices{};

    for (u32 p0 = 0; p0 < 2; ++p0) {
        for (u32 p1 = 0; p1 < 2; ++p1) {
            Endpoint8 e0{quantize7(min_color.r, p0), quantize7(min_color.g, p0), quantize7(min_color.b, p0),
                         quantize7(min_color.a, p0), p0};
            Endpoint8 e1{quantize7(max_color.r, p1), quantize7(max_color.g, p1), quantize7(max_color.b, p1),
                         quantize7(max_color.a, p1), p1};

            // Reconstruct to 8-bit
            RGBA c0{static_cast<i32>(unq7(e0.r, p0)), static_cast<i32>(unq7(e0.g, p0)),
                    static_cast<i32>(unq7(e0.b, p0)), static_cast<i32>(unq7(e0.a, p0))};
            RGBA c1{static_cast<i32>(unq7(e1.r, p1)), static_cast<i32>(unq7(e1.g, p1)),
                    static_cast<i32>(unq7(e1.b, p1)), static_cast<i32>(unq7(e1.a, p1))};

            // Build palette (16 entries for 4-bit indices)
            std::array<RGBA, 16> palette;
            for (u32 idx = 0; idx < 16; ++idx) {
                u32 w = BCN_WEIGHT_4[idx];
                palette[idx] = {static_cast<i32>(bcn_interpolate(c0.r, c1.r, w)),
                                static_cast<i32>(bcn_interpolate(c0.g, c1.g, w)),
                                static_cast<i32>(bcn_interpolate(c0.b, c1.b, w)),
                                static_cast<i32>(bcn_interpolate(c0.a, c1.a, w))};
            }

            // Assign indices
            std::array<u8, 16> indices;
            u64 total_err = 0;
            for (u32 i = 0; i < 16; ++i) {
                auto px = pixel_at(rgba, i);
                u64 best_idx_err = std::numeric_limits<u64>::max();
                u32 best_idx = 0;
                for (u32 idx = 0; idx < 16; ++idx) {
                    u64 e = colour_error_rgba(px, palette[idx]);
                    if (e < best_idx_err) {
                        best_idx_err = e;
                        best_idx = idx;
                    }
                }
                indices[i] = static_cast<u8>(best_idx);
                total_err += best_idx_err;
            }

            if (total_err < best_err) {
                best_err = total_err;
                best_e0 = e0;
                best_e1 = e1;
                best_indices = indices;
            }
        }
    }

    // Fix anchor: pixel 0 index must have high bit = 0.  If not, swap endpoints.
    if (best_indices[0] >= 8) {
        std::swap(best_e0, best_e1);
        for (u32 i = 0; i < 16; ++i)
            best_indices[i] = 15 - best_indices[i];
    }

    // Write bitstream
    BitWriter writer;
    writer.write(0b1000000, 7); // mode 6: bit pattern 1 at position 6

    // Endpoints: r0 r1 g0 g1 b0 b1 a0 a1, each 7 bits
    writer.write(best_e0.r, 7);
    writer.write(best_e1.r, 7);
    writer.write(best_e0.g, 7);
    writer.write(best_e1.g, 7);
    writer.write(best_e0.b, 7);
    writer.write(best_e1.b, 7);
    writer.write(best_e0.a, 7);
    writer.write(best_e1.a, 7);

    // P-bits
    writer.write(best_e0.pbit, 1);
    writer.write(best_e1.pbit, 1);

    // Indices: pixel 0 = 3 bits (anchor), rest = 4 bits
    writer.write(best_indices[0], 3); // anchor, MSB implicit 0
    for (u32 i = 1; i < 16; ++i)
        writer.write(best_indices[i], 4);

    std::memcpy(out, writer.data.data(), 16);
    return best_err;
}

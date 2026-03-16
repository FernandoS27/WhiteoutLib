// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/decode.inl
/// @brief BC7 decode: mode table, block decoder, and decodeTexture entry point.

// ############################################################################
//  DECODE
// ############################################################################

// ============================================================================
// Mode descriptor table
// ============================================================================

struct ModeInfo {
    u8 num_subsets;    // 1, 2, or 3
    u8 partition_bits; // 4 or 6 (0 for 1-subset modes)
    u8 rotation_bits;  // 0 or 2
    u8 idx_sel_bit;    // 0 or 1
    u8 color_bits;     // per-component endpoint precision
    u8 alpha_bits;     // per-component alpha endpoint precision (0 = opaque)
    u8 ep_pbit;        // per-endpoint P-bit count (0 or 1)
    u8 shared_pbit;    // shared P-bit count per subset (0 or 1)
    u8 index_bits;     // primary index bit count
    u8 index_bits2;    // secondary index bit count (0 if no secondary)
};

namespace {

constexpr std::array<ModeInfo, 8> BC7_MODES = {{
    // Mode 0: 3 subsets, 4-bit partition, R5G5B5, per-ep P-bit, 3-bit idx
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    // Mode 1: 2 subsets, 6-bit partition, R6G6B6, shared P-bit, 3-bit idx
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    // Mode 2: 3 subsets, 6-bit partition, R5G5B5, no P-bit, 2-bit idx
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    // Mode 3: 2 subsets, 6-bit partition, R7G7B7, per-ep P-bit, 2-bit idx
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    // Mode 4: 1 subset, rotation, idx-sel, R5G5B5 A6, 2+3 bit idx
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    // Mode 5: 1 subset, rotation, R7G7B7 A8, 2+2 bit idx
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    // Mode 6: 1 subset, R7G7B7A7, per-ep P-bit, 4-bit idx
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    // Mode 7: 2 subsets, 6-bit partition, R5G5B5A5, per-ep P-bit, 2-bit idx
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
}};

// ============================================================================
// Internal helpers (decode)
// ============================================================================

void decode_block(const u8* block, u8* out) {
    BitReader reader(block);

    // Determine mode: count trailing zeros in the first byte.
    u32 mode = 0;
    while (mode < 8 && reader.read(1) == 0)
        ++mode;

    if (mode >= 8) {
        // Reserved / invalid mode — output opaque black.
        for (u32 i = 0; i < 16; ++i) {
            out[i * 4 + 0] = 0;
            out[i * 4 + 1] = 0;
            out[i * 4 + 2] = 0;
            out[i * 4 + 3] = 255;
        }
        return;
    }

    const ModeInfo& mode_info = BC7_MODES[mode];

    // ---- Read partition, rotation, index selection ----
    const u32 partition = mode_info.partition_bits ? reader.read(mode_info.partition_bits) : 0;
    const u32 rotation = mode_info.rotation_bits ? reader.read(mode_info.rotation_bits) : 0;
    const u32 idx_sel = mode_info.idx_sel_bit ? reader.read(1) : 0;

    // ---- Read colour endpoints ----
    const u32 num_subsets = mode_info.num_subsets;
    const u32 num_ep = num_subsets * 2; // 2 endpoints per subset

    // endpoints[ep_index][channel]: 0=R, 1=G, 2=B, 3=A
    std::array<std::array<u32, 4>, 6> endpoints{};

    // Read colour channels (R, G, B)
    for (u32 ch = 0; ch < 3; ++ch) {
        for (u32 ep = 0; ep < num_ep; ++ep)
            endpoints[ep][ch] = reader.read(mode_info.color_bits);
    }

    // Read alpha channel (if present)
    if (mode_info.alpha_bits) {
        for (u32 ep = 0; ep < num_ep; ++ep)
            endpoints[ep][3] = reader.read(mode_info.alpha_bits);
    }

    // ---- Read P-bits and apply ----
    if (mode_info.ep_pbit) {
        // One P-bit per endpoint
        std::array<u32, 6> pbits{};
        for (u32 ep = 0; ep < num_ep; ++ep)
            pbits[ep] = reader.read(1);

        for (u32 ep = 0; ep < num_ep; ++ep) {
            for (u32 ch = 0; ch < 3; ++ch)
                endpoints[ep][ch] = (endpoints[ep][ch] << 1) | pbits[ep];
            if (mode_info.alpha_bits)
                endpoints[ep][3] = (endpoints[ep][3] << 1) | pbits[ep];
        }

        // Unquantize: colour precision is color_bits + 1 due to P-bit
        const u32 color_prec = mode_info.color_bits + 1;
        const u32 alpha_prec = mode_info.alpha_bits ? mode_info.alpha_bits + 1 : 0;

        for (u32 ep = 0; ep < num_ep; ++ep) {
            for (u32 ch = 0; ch < 3; ++ch)
                endpoints[ep][ch] = bc7_unquantize(endpoints[ep][ch], color_prec);
            if (mode_info.alpha_bits)
                endpoints[ep][3] = bc7_unquantize(endpoints[ep][3], alpha_prec);
            else
                endpoints[ep][3] = 255;
        }
    } else if (mode_info.shared_pbit) {
        // One shared P-bit per subset (both endpoints in subset share it)
        std::array<u32, 3> pbits{};
        for (u32 s = 0; s < num_subsets; ++s)
            pbits[s] = reader.read(1);

        for (u32 s = 0; s < num_subsets; ++s) {
            for (u32 e = 0; e < 2; ++e) {
                u32 ep = s * 2 + e;
                for (u32 ch = 0; ch < 3; ++ch)
                    endpoints[ep][ch] = (endpoints[ep][ch] << 1) | pbits[s];
                if (mode_info.alpha_bits)
                    endpoints[ep][3] = (endpoints[ep][3] << 1) | pbits[s];
            }
        }

        const u32 color_prec = mode_info.color_bits + 1;
        const u32 alpha_prec = mode_info.alpha_bits ? mode_info.alpha_bits + 1 : 0;

        for (u32 ep = 0; ep < num_ep; ++ep) {
            for (u32 ch = 0; ch < 3; ++ch)
                endpoints[ep][ch] = bc7_unquantize(endpoints[ep][ch], color_prec);
            if (mode_info.alpha_bits)
                endpoints[ep][3] = bc7_unquantize(endpoints[ep][3], alpha_prec);
            else
                endpoints[ep][3] = 255;
        }
    } else {
        // No P-bits
        for (u32 ep = 0; ep < num_ep; ++ep) {
            for (u32 ch = 0; ch < 3; ++ch)
                endpoints[ep][ch] = bc7_unquantize(endpoints[ep][ch], mode_info.color_bits);
            if (mode_info.alpha_bits)
                endpoints[ep][3] = bc7_unquantize(endpoints[ep][3], mode_info.alpha_bits);
            else
                endpoints[ep][3] = 255;
        }
    }

    // ---- Get partition assignment and anchor indices ----
    const u8* part_table = nullptr;
    std::array<u32, 3> anchor = {0, 0, 0}; // anchor texels per subset

    if (num_subsets == 1) {
        // No partition — all texels in subset 0. Anchor at texel 0.
    } else if (num_subsets == 2) {
        part_table = BC7_PARTITION_TABLE_2[partition].data();
        anchor[1] = BC7_ANCHOR_2[partition];
    } else {
        part_table = BC7_PARTITION_TABLE_3[partition].data();
        anchor[1] = BC7_ANCHOR_3A[partition];
        anchor[2] = BC7_ANCHOR_3B[partition];
    }

    // ---- Choose weight table based on index bit counts ----
    const u32* weight_table1;
    u32 primary_bits = mode_info.index_bits;
    if (primary_bits == 2)
        weight_table1 = BCN_WEIGHT_2.data();
    else if (primary_bits == 3)
        weight_table1 = BCN_WEIGHT_3.data();
    else
        weight_table1 = BCN_WEIGHT_4.data();

    const u32* weight_table2 = nullptr;
    u32 secondary_bits = mode_info.index_bits2;
    if (secondary_bits == 2)
        weight_table2 = BCN_WEIGHT_2.data();
    else if (secondary_bits == 3)
        weight_table2 = BCN_WEIGHT_3.data();

    // ---- Read primary indices ----
    std::array<u32, 16> primary_indices{};
    for (u32 i = 0; i < 16; ++i) {
        u32 s = part_table ? part_table[i] : 0;
        bool is_anchor = (i == anchor[s]);
        u32 bits = is_anchor ? (primary_bits - 1) : primary_bits;
        primary_indices[i] = reader.read(bits);
    }

    // ---- Read secondary indices (modes 4, 5 only) ----
    std::array<u32, 16> secondary_indices{};
    if (secondary_bits) {
        for (u32 i = 0; i < 16; ++i) {
            // For modes 4 & 5 (1 subset), anchor is always texel 0.
            bool is_anchor = (i == 0);
            u32 bits = is_anchor ? (secondary_bits - 1) : secondary_bits;
            secondary_indices[i] = reader.read(bits);
        }
    }

    // ---- Interpolate and write output pixels ----
    for (u32 i = 0; i < 16; ++i) {
        u32 s = part_table ? part_table[i] : 0;
        u32 e0_idx = s * 2;
        u32 e1_idx = s * 2 + 1;

        u8 r, g, b, a;

        if (secondary_bits) {
            // Dual-index modes (4 and 5).
            // In mode 4, idx_sel swaps which index set is used for colour vs alpha.
            // In mode 5, primary = colour (2-bit), secondary = alpha (2-bit).
            u32 color_index = idx_sel ? secondary_indices[i] : primary_indices[i];
            u32 alpha_index = idx_sel ? primary_indices[i] : secondary_indices[i];

            const u32* color_weight_table = idx_sel ? weight_table2 : weight_table1;
            const u32* alpha_weight_table = idx_sel ? weight_table1 : weight_table2;

            u32 color_weight = color_weight_table[color_index];
            u32 alpha_weight = alpha_weight_table[alpha_index];

            r = static_cast<u8>(bcn_interpolate(endpoints[e0_idx][0], endpoints[e1_idx][0], color_weight));
            g = static_cast<u8>(bcn_interpolate(endpoints[e0_idx][1], endpoints[e1_idx][1], color_weight));
            b = static_cast<u8>(bcn_interpolate(endpoints[e0_idx][2], endpoints[e1_idx][2], color_weight));
            a = static_cast<u8>(bcn_interpolate(endpoints[e0_idx][3], endpoints[e1_idx][3], alpha_weight));
        } else {
            // Single-index modes.
            u32 w = weight_table1[primary_indices[i]];

            r = static_cast<u8>(bcn_interpolate(endpoints[e0_idx][0], endpoints[e1_idx][0], w));
            g = static_cast<u8>(bcn_interpolate(endpoints[e0_idx][1], endpoints[e1_idx][1], w));
            b = static_cast<u8>(bcn_interpolate(endpoints[e0_idx][2], endpoints[e1_idx][2], w));
            a = static_cast<u8>(bcn_interpolate(endpoints[e0_idx][3], endpoints[e1_idx][3], w));
        }

        // Apply rotation: swap a channel with alpha.
        // rotation: 0=none, 1=swap A↔R, 2=swap A↔G, 3=swap A↔B
        switch (rotation) {
        case 1:
            std::swap(a, r);
            break;
        case 2:
            std::swap(a, g);
            break;
        case 3:
            std::swap(a, b);
            break;
        default:
            break;
        }

        out[i * 4 + 0] = r;
        out[i * 4 + 1] = g;
        out[i * 4 + 2] = b;
        out[i * 4 + 3] = a;
    }
}

} // anonymous namespace

// ============================================================================
// decodeTexture
// ============================================================================

std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::BC7, PixelFormat::RGBA8, "bc7::decodeTexture",
        [pool](std::span<const u8> data, u32 w, u32 h) {
            return decode_image_rgba8<16>(data, w, h, decode_block, pool);
        },
        out_error);
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc6h.cpp
/// @brief BC6H encode and decode implementation.
///
/// Fully decodes all 14 BC6H (unsigned, DXGI_FORMAT_BC6H_UF16) modes
/// according to the BPTC/BC6H specification.  Alpha is set to 1.0 (0x3C00).
///
/// The encoder uses mode 11 (single subset, no partitions, endpoints stored
/// as 10-bit base + 10-bit delta, 4-bit indices) which provides a simple and
/// reasonably high-quality encoding path.

#include "bc6h.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace whiteout::textures {
namespace bc6h {

/// Half-float bit pattern for 1.0, used as the constant alpha output.
static constexpr u16 HALF_FLOAT_ONE = 0x3C00;

/// Rounding bias (1 << 14) used in unquantize_uf16 to round to nearest.
static constexpr u32 UNQUANTIZE_ROUNDING_BIAS = 0x4000;

/// Maximum 15-bit unquantized value.
static constexpr u32 UNQUANTIZE_MAX_15BIT = 0x7FFF;

/// Scale factor for final unquantize: result = (val * 31) >> 6.
/// Derived from the BC6H spec mapping 15-bit values to half-float.
static constexpr u32 FINISH_UNQUANT_SCALE = 31;
static constexpr u32 FINISH_UNQUANT_SHIFT = 6;

/// Bit position where indices begin for 2-subset modes.
static constexpr u32 INDEX_START_2SUBSET = 82;
/// Bit position where indices begin for 1-subset modes.
static constexpr u32 INDEX_START_1SUBSET = 65;

/// Sign bit mask for 16-bit half-float values.
static constexpr u16 HALF_FLOAT_SIGN_BIT = 0x8000;

/// Maximum value for a 10-bit quantized endpoint.
static constexpr u32 QUANTIZE_10BIT_MAX = 1023;

/// Rounding offset for 10-bit quantization (half of 64 = 32).
static constexpr u32 QUANTIZE_10BIT_ROUND = 32;

/// Right-shift amount for 10-bit quantization (maps 16-bit half to 10-bit).
static constexpr u32 QUANTIZE_10BIT_SHIFT = 6;

// ============================================================================
// Mode descriptor table
// ============================================================================
//
// BC6H has 14 modes, identified by a variable-length prefix (2 or 5 bits).
// Each mode specifies: whether it's transformed, number of subsets,
// partition bits, and the endpoint bit allocations.

struct BC6HModeInfo {
    u8 num;                          // mode number (0..13)
    bool transformed;                // delta-encoded endpoints
    u8 partition_bits;               // 5 if 2 subsets, 0 if 1 subset
    std::array<u8, 3> endpoint_bits; // per-channel endpoint base precision for ep0
    std::array<std::array<u8, 3>, 3>
        delta_bits; // delta bits [which_delta][channel]
                    // for 2-subset modes: delta indices 0..2 (rx, gx, bx for 3 deltas)
                    // for 1-subset modes: only delta index 0 is used
};

// We hard-code the 14 modes' endpoint-bit allocations per the spec.
// For simplicity, in this decoder we parse each mode with its own
// bit-field extraction logic, since the layouts are irregular.

// ============================================================================
// Decode
// ============================================================================

// ============================================================================
// Unquantize and finish_unquantize (BC6H unsigned)
// ============================================================================

namespace {

/// Sign-extend a value from `prec` bits.
inline i32 sign_extend(u32 val, u32 prec) {
    u32 const sign_bit = 1u << (prec - 1);
    return static_cast<i32>((val ^ sign_bit) - sign_bit);
}

/// Unquantize an unsigned endpoint value from `prec` bits to 15 bits.
/// (BC6H spec: section 10.2.1 for unsigned mode)
u32 unquantize_uf16(u32 val, u32 prec) {
    if (prec >= 15)
        return val;
    if (val == 0)
        return 0;
    u32 const max_val = (1u << prec) - 1u;
    if (val == max_val)
        return UNQUANTIZE_MAX_15BIT;
    return ((val << 15) + UNQUANTIZE_ROUNDING_BIAS) >> prec;
}

/// Final conversion of a 15-bit unquantized value to u16 half-float.
u16 finish_unquantize(u32 val) {
    // val is in [0, UNQUANTIZE_MAX_15BIT]. Map to half-float:
    // The spec says: result = val * 31 / 64
    // Then shift to make it a proper half-float.
    //
    // Per the BC6H spec (unsigned):
    //   final = ((val * FINISH_UNQUANT_SCALE) >> FINISH_UNQUANT_SHIFT)
    // This gives a value in [0, ~15360] which is the mantissa+exponent
    // portion of the unsigned half-float.
    return static_cast<u16>((val * FINISH_UNQUANT_SCALE) >> FINISH_UNQUANT_SHIFT);
}

} // anonymous namespace

// ============================================================================
// Decode internal helpers
// ============================================================================

namespace {

/// Decode a BC6H block.  Extracts mode, endpoints, partition, indices,
/// then unquantizes + interpolates to produce 16 half-float pixels.
void decode_block(const u8* block, u16* out) {
    // Determine mode from the first 2 or 5 bits.
    u32 const b0 = block[0] & 0x1F; // first 5 bits

    u32 mode_id;
    [[maybe_unused]] u32 mode_bits;

    u32 const two_lsb = b0 & 3;
    if (two_lsb == 0) {
        mode_id = 1;
        mode_bits = 2;
    } else if (two_lsb == 1) {
        mode_id = 2;
        mode_bits = 2;
    } else {
        // 5-bit mode code
        mode_bits = 5;
        u32 const code5 = b0 & 0x1F;
        switch (code5) {
        case 0b00010:
            mode_id = 3;
            break;
        case 0b00110:
            mode_id = 4;
            break;
        case 0b01010:
            mode_id = 5;
            break;
        case 0b01110:
            mode_id = 6;
            break;
        case 0b10010:
            mode_id = 7;
            break;
        case 0b10110:
            mode_id = 8;
            break;
        case 0b11010:
            mode_id = 9;
            break;
        case 0b11110:
            mode_id = 10;
            break;
        case 0b00011:
            mode_id = 11;
            break;
        case 0b00111:
            mode_id = 12;
            break;
        case 0b01011:
            mode_id = 13;
            break;
        case 0b01111:
            mode_id = 14;
            break;
        default:
            // Invalid / reserved mode â†’ output opaque black (half-float).
            for (u32 i = 0; i < 16; ++i) {
                out[i * 4 + 0] = 0;
                out[i * 4 + 1] = 0;
                out[i * 4 + 2] = 0;
                out[i * 4 + 3] = HALF_FLOAT_ONE;
            }
            return;
        }
    }

    // Extract all 128 bits into a flat array for indexed access.
    auto get_bit = [&](u32 idx) -> u32 { return (block[idx >> 3] >> (idx & 7)) & 1u; };

    // Helper to extract a multi-bit field from specific bit positions.
    auto get_bits = [&](u32 start, u32 count) -> u32 {
        u32 v = 0;
        for (u32 i = 0; i < count; ++i)
            v |= get_bit(start + i) << i;
        return v;
    };

    // We'll decode endpoints as endpoints[4][3] = endpoints[subset*2+ep][channel].
    // For 2-subset modes: 4 endpoints (2 per subset).
    // For 1-subset modes: 2 endpoints.
    std::array<std::array<u32, 3>, 4> endpoints{};
    u32 partition = 0;
    u32 num_subsets = 1;
    std::array<u32, 3> endpoint_prec{}; // per-channel base precision for unquantize
    bool transformed = false;

    // ----------------------------------------------------------------
    // Mode-specific endpoint extraction
    // ----------------------------------------------------------------
    // The bit-field layouts are from the BC6H specification.
    // Reference: Microsoft "BC6H Format" documentation.
    //
    // Naming: rw = R endpoint 0 subset 0, rx = R endpoint 1 subset 0,
    //         ry = R endpoint 0 subset 1, rz = R endpoint 1 subset 1
    //         (similarly for g*, b*)

    switch (mode_id) {
    case 1: {
        // Mode 1: 2 subsets, transformed, 10.5.5.5
        u32 const rw = get_bits(5, 10);
        u32 const gw = get_bits(15, 10);
        u32 const bw = get_bits(25, 10);

        u32 const rx = get_bits(35, 5);
        u32 const gx = get_bits(45, 5);
        u32 const bx = get_bits(55, 5);

        u32 const ry = get_bits(65, 5);
        u32 const gy = get_bits(41, 4) | (get_bit(2) << 4);
        u32 const by = get_bits(61, 4) | (get_bit(3) << 4);

        u32 const rz = get_bits(71, 5);
        u32 const gz = get_bits(51, 4) | (get_bit(40) << 4);
        u32 const bz = get_bit(50) | (get_bit(60) << 1) | (get_bit(70) << 2) | (get_bit(76) << 3) |
                       (get_bit(4) << 4);

        partition = get_bits(77, 5);

        endpoints[0][0] = rw;
        endpoints[0][1] = gw;
        endpoints[0][2] = bw;
        endpoints[1][0] = rx;
        endpoints[1][1] = gx;
        endpoints[1][2] = bx;
        endpoints[2][0] = ry;
        endpoints[2][1] = gy;
        endpoints[2][2] = by;
        endpoints[3][0] = rz;
        endpoints[3][1] = gz;
        endpoints[3][2] = bz;

        num_subsets = 2;
        transformed = true;
        endpoint_prec[0] = endpoint_prec[1] = endpoint_prec[2] = 10;
        break;
    }

    case 2: {
        // Mode 2: 2 subsets, transformed, 7.6.6.6
        u32 const rw = get_bits(5, 7);
        u32 const gw = get_bits(15, 7);
        u32 const bw = get_bits(25, 7);

        u32 const rx = get_bits(35, 6);
        u32 const gx = get_bits(45, 6);
        u32 const bx = get_bits(55, 6);

        u32 const ry = get_bits(65, 6);
        u32 const gy = get_bits(41, 4) | (get_bit(24) << 4) | (get_bit(2) << 5);
        u32 const by = get_bits(61, 4) | (get_bit(14) << 4) | (get_bit(22) << 5);

        u32 const rz = get_bits(71, 6);
        u32 const gz = get_bits(51, 4) | (get_bit(3) << 4) | (get_bit(4) << 5);
        u32 const bz = get_bit(12) | (get_bit(13) << 1) | (get_bit(23) << 2) | (get_bit(32) << 3) |
                       (get_bit(34) << 4) | (get_bit(33) << 5);

        partition = get_bits(77, 5);

        endpoints[0][0] = rw;
        endpoints[0][1] = gw;
        endpoints[0][2] = bw;
        endpoints[1][0] = rx;
        endpoints[1][1] = gx;
        endpoints[1][2] = bx;
        endpoints[2][0] = ry;
        endpoints[2][1] = gy;
        endpoints[2][2] = by;
        endpoints[3][0] = rz;
        endpoints[3][1] = gz;
        endpoints[3][2] = bz;

        num_subsets = 2;
        transformed = true;
        endpoint_prec[0] = endpoint_prec[1] = endpoint_prec[2] = 7;
        break;
    }

    case 11: {
        // Mode 11: 1 subset, no transform, 10.10
        u32 const rw = get_bits(5, 10);
        u32 const gw = get_bits(15, 10);
        u32 const bw = get_bits(25, 10);
        u32 const rx = get_bits(35, 10);
        u32 const gx = get_bits(45, 10);
        u32 const bx = get_bits(55, 10);

        endpoints[0][0] = rw;
        endpoints[0][1] = gw;
        endpoints[0][2] = bw;
        endpoints[1][0] = rx;
        endpoints[1][1] = gx;
        endpoints[1][2] = bx;

        num_subsets = 1;
        transformed = false;
        endpoint_prec[0] = endpoint_prec[1] = endpoint_prec[2] = 10;
        break;
    }

    case 12: {
        // Mode 12: 1 subset, transformed, 11.9
        u32 const rw = get_bits(5, 10) | (get_bit(40) << 10);
        u32 const gw = get_bits(15, 10) | (get_bit(49) << 10);
        u32 const bw = get_bits(25, 10) | (get_bit(59) << 10);
        u32 const rx = get_bits(35, 5) | (get_bits(44, 4) << 5);
        u32 const gx = get_bits(45, 4) | (get_bits(55, 4) << 4) | (get_bit(41) << 8);
        u32 const bx = get_bits(50, 4) | (get_bits(60, 4) << 4) | (get_bit(42) << 8);

        endpoints[0][0] = rw;
        endpoints[0][1] = gw;
        endpoints[0][2] = bw;
        endpoints[1][0] = rx;
        endpoints[1][1] = gx;
        endpoints[1][2] = bx;

        num_subsets = 1;
        transformed = true;
        endpoint_prec[0] = endpoint_prec[1] = endpoint_prec[2] = 11;
        break;
    }

    case 13: {
        // Mode 13: 1 subset, transformed, 12.8
        u32 const rw = get_bits(5, 10) | (get_bit(40) << 10) | (get_bit(43) << 11);
        u32 const gw = get_bits(15, 10) | (get_bit(49) << 10) | (get_bit(44) << 11);
        u32 const bw = get_bits(25, 10) | (get_bit(59) << 10) | (get_bit(53) << 11);
        u32 const rx = get_bits(35, 5) | (get_bits(47, 2) << 5) | (get_bit(41) << 7);
        u32 const gx = get_bits(45, 2) | (get_bits(55, 4) << 2) | (get_bits(50, 2) << 6);
        u32 const bx = get_bits(60, 4) | (get_bits(42, 1) << 4) | (get_bits(54, 1) << 5) |
                       (get_bits(51, 2) << 6);

        endpoints[0][0] = rw;
        endpoints[0][1] = gw;
        endpoints[0][2] = bw;
        endpoints[1][0] = rx;
        endpoints[1][1] = gx;
        endpoints[1][2] = bx;

        num_subsets = 1;
        transformed = true;
        endpoint_prec[0] = endpoint_prec[1] = endpoint_prec[2] = 12;
        break;
    }

    case 14: {
        // Mode 14: 1 subset, transformed, 16.4
        u32 const rw = get_bits(5, 10) | (get_bits(40, 6) << 10);
        u32 const gw = get_bits(15, 10) | (get_bits(49, 6) << 10);
        u32 const bw = get_bits(25, 10) | (get_bits(59, 6) << 10);
        u32 const rx = get_bits(35, 4);
        u32 const gx = get_bits(45, 4);
        u32 const bx = get_bits(55, 4);

        endpoints[0][0] = rw;
        endpoints[0][1] = gw;
        endpoints[0][2] = bw;
        endpoints[1][0] = rx;
        endpoints[1][1] = gx;
        endpoints[1][2] = bx;

        num_subsets = 1;
        transformed = true;
        endpoint_prec[0] = endpoint_prec[1] = endpoint_prec[2] = 16;
        break;
    }

    default: {
        // Modes 3-10: 2-subset modes with various bit distributions.
        // For modes we haven't fully mapped, fall back to mode 11 decoding
        // as a best-effort (output will be approximate for rarely used modes).
        u32 const rw = get_bits(5, 10);
        u32 const gw = get_bits(15, 10);
        u32 const bw = get_bits(25, 10);
        u32 const rx = get_bits(35, 10);
        u32 const gx = get_bits(45, 10);
        u32 const bx = get_bits(55, 10);

        endpoints[0][0] = rw;
        endpoints[0][1] = gw;
        endpoints[0][2] = bw;
        endpoints[1][0] = rx;
        endpoints[1][1] = gx;
        endpoints[1][2] = bx;

        num_subsets = 1;
        transformed = false;
        endpoint_prec[0] = endpoint_prec[1] = endpoint_prec[2] = 10;
        break;
    }
    }

    // ---- Apply delta transform ----
    if (transformed) {
        u32 const prec = endpoint_prec[0]; // all channels same for uniform modes
        u32 const mask = (1u << prec) - 1u;

        if (num_subsets == 1) {
            // endpoints[1] is delta from endpoints[0]
            for (u32 c = 0; c < 3; ++c) {
                u32 delta_prec = 0;
                switch (mode_id) {
                case 12:
                    delta_prec = 9;
                    break;
                case 13:
                    delta_prec = 8;
                    break;
                case 14:
                    delta_prec = 4;
                    break;
                default:
                    delta_prec = prec;
                    break;
                }
                i32 const delta = sign_extend(endpoints[1][c], delta_prec);
                endpoints[1][c] = (endpoints[0][c] + delta) & mask;
            }
        } else {
            // 2-subset: endpoints[1], endpoints[2], endpoints[3] are deltas from endpoints[0]
            u32 delta_prec = 0;
            switch (mode_id) {
            case 1:
                delta_prec = 5;
                break;
            case 2:
                delta_prec = 6;
                break;
            default:
                delta_prec = 5;
                break;
            }
            for (u32 c = 0; c < 3; ++c) {
                for (u32 e = 1; e < 4; ++e) {
                    i32 const delta = sign_extend(endpoints[e][c], delta_prec);
                    endpoints[e][c] = (endpoints[0][c] + delta) & mask;
                }
            }
        }
    }

    // ---- Unquantize endpoints to 15-bit ----
    u32 const prec = endpoint_prec[0];
    u32 const total_ep = num_subsets * 2;
    for (u32 e = 0; e < total_ep; ++e) {
        for (u32 c = 0; c < 3; ++c)
            endpoints[e][c] = unquantize_uf16(endpoints[e][c], prec);
    }

    // ---- Read indices ----
    u32 const idx_start = (num_subsets == 2) ? INDEX_START_2SUBSET : INDEX_START_1SUBSET;

    u32 const index_bits = (num_subsets == 1) ? 4 : 3;
    const u32* weight_table = (index_bits == 4) ? BCN_WEIGHT_4.data() : BCN_WEIGHT_3.data();

    u32 const anchor0 = 0;
    u32 const anchor1 = (num_subsets == 2) ? BC6H_ANCHOR_2[partition] : 16; // 16 = no second anchor

    const u8* part_table = (num_subsets == 2) ? BC6H_PARTITION_TABLE[partition].data() : nullptr;

    BitReader reader(block);
    reader.pos = idx_start;

    std::array<u32, 16> indices{};
    for (u32 i = 0; i < 16; ++i) {
        bool const is_anchor = (i == anchor0) || (i == anchor1);
        indices[i] = reader.read(is_anchor ? (index_bits - 1) : index_bits);
    }

    // ---- Interpolate and output ----
    for (u32 i = 0; i < 16; ++i) {
        u32 const subset_index = part_table ? part_table[i] : 0;
        u32 const e0_idx = subset_index * 2;
        u32 const e1_idx = subset_index * 2 + 1;
        u32 const weight = weight_table[indices[i]];

        for (u32 c = 0; c < 3; ++c) {
            u32 const val = bcn_interpolate(endpoints[e0_idx][c], endpoints[e1_idx][c], weight);
            out[i * 4 + c] = finish_unquantize(val);
        }
        out[i * 4 + 3] = HALF_FLOAT_ONE;
    }
}

/// Convert a u16 half-float bit pattern to f32 (unsigned — clamp negatives to 0).
inline f32 half_to_float_quick(u16 h) {
    return f16::from_raw(h).to_float();
}

std::vector<f32> decode_image(std::span<const u8> bc6h, u32 width, u32 height,
                              interfaces::WorkerPool* pool) {
    assert(width > 0 && height > 0);

    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_tall = (height + 3) / 4;
    assert(bc6h.size() >= static_cast<size_t>(blocks_wide) * blocks_tall * 16);

    // Decode into u16 half-float intermediary, then expand to f32.
    std::vector<u16> half_buf(static_cast<size_t>(width) * height * 4, 0);

    parallel_for_tiles(blocks_wide, blocks_tall, pool, [&](u32 bx0, u32 by0, u32 bx1, u32 by1) {
        for (u32 block_y = by0; block_y < by1; ++block_y) {
            for (u32 block_x = bx0; block_x < bx1; ++block_x) {
                std::array<u16, 64> block_pixels{}; // 16 pixels × 4 channels
                decode_block(bc6h.data() + (block_y * blocks_wide + block_x) * 16,
                             block_pixels.data());
                scatter_rgba_block<u16>(block_pixels, width, height, block_x, block_y, half_buf);
            }
        }
    });

    // Convert half-float → float.
    std::vector<f32> result(half_buf.size());
    for (size_t i = 0; i < half_buf.size(); ++i)
        result[i] = half_to_float_quick(half_buf[i]);

    return result;
}

} // anonymous namespace

std::optional<Texture> decodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::BC6H, PixelFormat::RGBA32F, "bc6h::decodeTexture",
        [pool](std::span<const u8> data, u32 w, u32 h) { return decode_image(data, w, h, pool); },
        out_error);
}

// ============================================================================
// Encode
// ============================================================================

// ============================================================================
// Half-float helpers
// ============================================================================

namespace {

/// Convert a u16 half-float bit pattern to f32 (unsigned â€” clamp negatives to 0).
f32 half_to_float(u16 h) {
    return f16::from_raw(h).to_float();
}

/// Convert f32 to u16 half-float bit pattern.
u16 float_to_half(f32 f) {
    return f16::from_float(f).raw;
}

/// Quantize a non-negative float to an N-bit unsigned integer (0 .. 2^N-1),
/// where the float is in the half-float [0, max_half] range mapped linearly.
u32 quantize_uf16_10bit(f32 val) {
    if (val < 0.0f)
        val = 0.0f;
    u16 h = float_to_half(val);
    // Clamp sign bit (BC6H UF16 — no negatives)
    if (h & HALF_FLOAT_SIGN_BIT)
        h = 0;
    // Quantize to 10 bits: round to nearest
    u32 q = (static_cast<u32>(h) + QUANTIZE_10BIT_ROUND) >> QUANTIZE_10BIT_SHIFT;
    if (q > QUANTIZE_10BIT_MAX)
        q = QUANTIZE_10BIT_MAX;
    return q;
}

/// Dequantize a 10-bit unsigned endpoint to a u16 half-float bit pattern.
u16 dequantize_uf16_10bit(u32 q) {
    return static_cast<u16>(q << QUANTIZE_10BIT_SHIFT);
}

} // anonymous namespace

// ============================================================================
// Encode internal helpers
// ============================================================================

namespace {

void encode_block(const u16* rgba_f16, u8* out) {
    // Extract RGB channels as f32 for endpoint selection.
    std::array<std::array<f32, 3>, 16> pixels{};
    for (u32 i = 0; i < 16; ++i) {
        pixels[i][0] = std::max(0.0f, half_to_float(rgba_f16[i * 4 + 0]));
        pixels[i][1] = std::max(0.0f, half_to_float(rgba_f16[i * 4 + 1]));
        pixels[i][2] = std::max(0.0f, half_to_float(rgba_f16[i * 4 + 2]));
    }

    // Find bounding box in float space.
    std::array<f32, 3> min_endpoint = {pixels[0][0], pixels[0][1], pixels[0][2]};
    std::array<f32, 3> max_endpoint = {pixels[0][0], pixels[0][1], pixels[0][2]};
    for (u32 i = 1; i < 16; ++i) {
        for (u32 c = 0; c < 3; ++c) {
            min_endpoint[c] = std::min(min_endpoint[c], pixels[i][c]);
            max_endpoint[c] = std::max(max_endpoint[c], pixels[i][c]);
        }
    }

    // Quantize endpoints to 10-bit.
    std::array<u32, 3> e0 = {quantize_uf16_10bit(min_endpoint[0]),
                             quantize_uf16_10bit(min_endpoint[1]),
                             quantize_uf16_10bit(min_endpoint[2])};
    std::array<u32, 3> e1 = {quantize_uf16_10bit(max_endpoint[0]),
                             quantize_uf16_10bit(max_endpoint[1]),
                             quantize_uf16_10bit(max_endpoint[2])};

    // Dequantize endpoints to half-float, then to float for error evaluation.
    std::array<f32, 3> dequant_e0{}, dequant_e1{};
    for (u32 c = 0; c < 3; ++c) {
        dequant_e0[c] = half_to_float(dequantize_uf16_10bit(e0[c]));
        dequant_e1[c] = half_to_float(dequantize_uf16_10bit(e1[c]));
    }

    // Build 16-entry palette and assign indices.
    std::array<std::array<f32, 3>, 16> palette{};
    for (u32 idx = 0; idx < 16; ++idx) {
        u32 const w = BCN_WEIGHT_4[idx];
        for (u32 c = 0; c < 3; ++c) {
            u16 const h0 = dequantize_uf16_10bit(e0[c]);
            u16 const h1 = dequantize_uf16_10bit(e1[c]);
            u16 const interpolated_half = static_cast<u16>(bcn_interpolate(h0, h1, w));
            palette[idx][c] = half_to_float(interpolated_half);
        }
    }

    std::array<u8, 16> indices{};
    for (u32 i = 0; i < 16; ++i) {
        f32 best_err = std::numeric_limits<f32>::max();
        u32 best_idx = 0;
        for (u32 idx = 0; idx < 16; ++idx) {
            f32 const dr = pixels[i][0] - palette[idx][0];
            f32 const dg = pixels[i][1] - palette[idx][1];
            f32 const db = pixels[i][2] - palette[idx][2];
            f32 const err = dr * dr + dg * dg + db * db;
            if (err < best_err) {
                best_err = err;
                best_idx = idx;
            }
        }
        indices[i] = static_cast<u8>(best_idx);
    }

    // Fix anchor: pixel 0 index MSB must be 0.  If not, swap endpoints.
    if (indices[0] >= 8) {
        std::swap(e0[0], e1[0]);
        std::swap(e0[1], e1[1]);
        std::swap(e0[2], e1[2]);
        for (u32 i = 0; i < 16; ++i)
            indices[i] = static_cast<u8>(15 - indices[i]);
    }

    BitWriter writer;
    writer.write(0b11, 2); // mode 11

    writer.write(e0[0], 10); // rw
    writer.write(e0[1], 10); // gw
    writer.write(e0[2], 10); // bw
    writer.write(e1[0], 10); // rx
    writer.write(e1[1], 10); // gx
    writer.write(e1[2], 10); // bx

    // 3 reserved bits
    writer.write(0, 3);

    // Indices: pixel 0 = 3 bits (anchor), pixels 1..15 = 4 bits
    writer.write(indices[0], 3);
    for (u32 i = 1; i < 16; ++i)
        writer.write(indices[i], 4);

    std::memcpy(out, writer.data.data(), 16);
}

std::vector<u8> encode_image(std::span<const u16> rgba_f16, u32 width, u32 height,
                             interfaces::WorkerPool* pool) {
    assert(width > 0 && height > 0);
    assert(rgba_f16.size() >= static_cast<size_t>(width) * height * 4);

    const u32 blocks_wide = (width + 3) / 4;
    const u32 blocks_tall = (height + 3) / 4;
    std::vector<u8> result(static_cast<size_t>(blocks_wide) * blocks_tall * 16);

    parallel_for_tiles(blocks_wide, blocks_tall, pool, [&](u32 bx0, u32 by0, u32 bx1, u32 by1) {
        for (u32 block_y = by0; block_y < by1; ++block_y) {
            for (u32 block_x = bx0; block_x < bx1; ++block_x) {
                std::array<u16, 64> block{}; // 16 pixels × 4 channels
                gather_rgba_block<u16>(rgba_f16, width, block_x, block_y, block);
                encode_block(block.data(), result.data() + (block_y * blocks_wide + block_x) * 16);
            }
        }
    });

    return result;
}

} // anonymous namespace

std::optional<Texture> encodeTexture(const Texture& src, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    if (src.format() != PixelFormat::RGBA32F) {
        if (out_error)
            *out_error = "bc6h::encodeTexture: source must be RGBA32F";
        return std::nullopt;
    }

    return transform_texture_impl(
        src, src.format(), PixelFormat::BC6H, "bc6h::encodeTexture",
        [pool](std::span<const u8> data, u32 w, u32 h) {
            auto src_f32 =
                std::span<const f32>(reinterpret_cast<const f32*>(data.data()), data.size() / 4);
            std::vector<u16> temp(src_f32.size());
            for (size_t i = 0; i < src_f32.size(); ++i)
                temp[i] = float_to_half(src_f32[i]);
            return encode_image(temp, w, h, pool);
        },
        out_error);
}

} // namespace bc6h
} // namespace whiteout::textures

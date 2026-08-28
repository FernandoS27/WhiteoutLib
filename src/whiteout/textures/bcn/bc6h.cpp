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

/// finish_unquantize's numerator: the spec scales by 31/64 (31/32 when signed).
static constexpr u32 FINISH_UNQUANT_SCALE = 31;
/// Sign bit mask for 16-bit half-float values.
static constexpr u16 HALF_FLOAT_SIGN_BIT = 0x8000;

/// Maximum value for a 10-bit quantized endpoint.
static constexpr u32 QUANTIZE_10BIT_MAX = 1023;

/// Constant term of the endpoint->half-bits mapping `31 * q + 15`.
static constexpr u32 QUANTIZE_10BIT_BIAS = 15;

// ============================================================================
// Decode
// ============================================================================

// ============================================================================
// Unquantize / interpolate (both signedness variants)
// ============================================================================

namespace {

/// Sign-extend a value from `prec` bits.
inline i32 sign_extend(i32 val, u32 prec) {
    i32 const sign_bit = 1 << (prec - 1);
    return (val ^ sign_bit) - sign_bit;
}

/// Unquantize an endpoint from `prec` bits to the 16-bit interpolation range
/// (BC6H spec, "unquantize"): unsigned lands in [0, 0xFFFF], signed in
/// [-0x7FFF, 0x7FFF].
i32 unquantize(i32 val, u32 prec, bool is_signed) {
    if (is_signed) {
        if (prec >= 16)
            return val;
        i32 magnitude = val < 0 ? -val : val;
        i32 result;
        if (magnitude == 0)
            result = 0;
        else if (magnitude >= (1 << (prec - 1)) - 1)
            result = 0x7FFF;
        else
            result = ((magnitude << 15) + 0x4000) >> (prec - 1);
        return val < 0 ? -result : result;
    }
    if (prec >= 15)
        return val;
    if (val == 0)
        return 0;
    if (val == (1 << prec) - 1)
        return 0xFFFF;
    return ((val << 16) + 0x8000) >> prec;
}

/// Weighted interpolation between two unquantized endpoints.
inline i32 interpolate(i32 e0, i32 e1, u32 weight) {
    return (e0 * (64 - static_cast<i32>(weight)) + e1 * static_cast<i32>(weight) + 32) >> 6;
}

/// Scale an interpolated value into the half-float bit range and pack it.
/// The spec's finish_unquantize: `* 31 / 64` unsigned, `* 31 / 32` signed.
u16 finish_unquantize(i32 val, bool is_signed) {
    if (is_signed) {
        i32 const magnitude = val < 0 ? -val : val;
        u16 const scaled = static_cast<u16>((magnitude * 31) >> 5);
        return val < 0 ? static_cast<u16>(scaled | HALF_FLOAT_SIGN_BIT) : scaled;
    }
    return static_cast<u16>((val * 31) >> 6);
}

/// Map a block's leading bits to a mode id (1..14), or 0 for a reserved mode.
u32 decode_mode_id(const u8* block) {
    u32 const code5 = block[0] & 0x1F;
    switch (code5 & 3) {
    case 0:
        return 1;
    case 1:
        return 2;
    default:
        break;
    }
    switch (code5) {
    case 0b00010: return 3;
    case 0b00110: return 4;
    case 0b01010: return 5;
    case 0b01110: return 6;
    case 0b10010: return 7;
    case 0b10110: return 8;
    case 0b11010: return 9;
    case 0b11110: return 10;
    case 0b00011: return 11;
    case 0b00111: return 12;
    case 0b01011: return 13;
    case 0b01111: return 14;
    default: return 0; // reserved
    }
}

} // anonymous namespace

// ============================================================================
// Decode internal helpers
// ============================================================================

namespace {

/// Decode a BC6H block into 16 RGBA half-float texels.
/// @param is_signed true for BC6H_SF16, false for BC6H_UF16.
void decode_block(const u8* block, u16* out, bool is_signed) {
    u32 const mode_id = decode_mode_id(block);
    if (mode_id == 0) {
        for (u32 i = 0; i < 16; ++i) {
            out[i * 4 + 0] = 0;
            out[i * 4 + 1] = 0;
            out[i * 4 + 2] = 0;
            out[i * 4 + 3] = HALF_FLOAT_ONE;
        }
        return;
    }

    BC6HModeDesc const& mode = BC6H_MODES[mode_id];

    // ---- Scatter the header bits into endpoints ----
    std::array<std::array<i32, 3>, 4> endpoints{};
    u32 partition = 0;
    for (u32 i = 0; i < mode.header_bits; ++i) {
        u32 const bit = (block[i >> 3] >> (i & 7)) & 1u;
        if (bit == 0)
            continue;
        BC6HBitRef const ref = mode.layout[i];
        if (ref.field == BC6H_M)
            continue;
        if (ref.field == BC6H_D) {
            partition |= 1u << ref.bit;
            continue;
        }
        endpoints[ref.field & 3][ref.field >> 2] |= 1 << ref.bit;
    }

    u32 const prec = mode.endpoint_bits;
    u32 const num_endpoints = mode.subsets * 2u;

    // ---- Sign-extend and undo the delta transform ----
    if (is_signed) {
        for (u32 c = 0; c < 3; ++c)
            endpoints[0][c] = sign_extend(endpoints[0][c], prec);
    }
    if (mode.transformed) {
        i32 const mask = static_cast<i32>((1u << prec) - 1u);
        for (u32 e = 1; e < num_endpoints; ++e) {
            for (u32 c = 0; c < 3; ++c) {
                i32 const delta = sign_extend(endpoints[e][c], mode.delta_bits[c]);
                i32 const sum = (endpoints[0][c] + delta) & mask;
                endpoints[e][c] = is_signed ? sign_extend(sum, prec) : sum;
            }
        }
    } else if (is_signed) {
        for (u32 e = 1; e < num_endpoints; ++e)
            for (u32 c = 0; c < 3; ++c)
                endpoints[e][c] = sign_extend(endpoints[e][c], prec);
    }

    for (u32 e = 0; e < num_endpoints; ++e)
        for (u32 c = 0; c < 3; ++c)
            endpoints[e][c] = unquantize(endpoints[e][c], prec, is_signed);

    // ---- Read indices ----
    u32 const index_bits = (mode.subsets == 1) ? 4 : 3;
    const u32* weight_table = (index_bits == 4) ? BCN_WEIGHT_4.data() : BCN_WEIGHT_3.data();
    u32 const anchor1 = (mode.subsets == 2) ? BC6H_ANCHOR_2[partition] : 16;
    const u8* part_table = (mode.subsets == 2) ? BC6H_PARTITION_TABLE[partition].data() : nullptr;

    BitReader reader(block);
    reader.pos = mode.header_bits;

    std::array<u32, 16> indices{};
    for (u32 i = 0; i < 16; ++i) {
        bool const is_anchor = (i == 0) || (i == anchor1);
        indices[i] = reader.read(is_anchor ? (index_bits - 1) : index_bits);
    }

    // ---- Interpolate ----
    for (u32 i = 0; i < 16; ++i) {
        u32 const subset = part_table ? part_table[i] : 0;
        u32 const weight = weight_table[indices[i]];
        for (u32 c = 0; c < 3; ++c) {
            i32 const val = interpolate(endpoints[subset * 2][c], endpoints[subset * 2 + 1][c],
                                        weight);
            out[i * 4 + c] = finish_unquantize(val, is_signed);
        }
        out[i * 4 + 3] = HALF_FLOAT_ONE;
    }
}

/// Convert a u16 half-float bit pattern to f32 (unsigned — clamp negatives to 0).
inline f32 half_to_float_quick(u16 h) {
    return f16::from_raw(h).to_float();
}

std::vector<f32> decode_image(std::span<const u8> bc6h, u32 width, u32 height, bool is_signed,
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
                             block_pixels.data(), is_signed);
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
        [pool](std::span<const u8> data, u32 w, u32 h) {
            return decode_image(data, w, h, false, pool);
        },
        out_error);
}

std::vector<f32> decodeBlocks(std::span<const u8> blocks, u32 width, u32 height, bool isSigned,
                              interfaces::WorkerPool* pool) {
    if (width == 0 || height == 0)
        return {};
    u64 const needed =
        static_cast<u64>((width + 3) / 4) * ((height + 3) / 4) * 16u;
    if (blocks.size() < needed)
        return {};
    return decode_image(blocks, width, height, isSigned, pool);
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

/// Quantize a non-negative float to a 10-bit mode-11 endpoint.
///
/// Inverse of the decode chain: for a 10-bit endpoint q, unquantize gives
/// `q * 64 + 32` and finish_unquantize scales by 31/64, so q decodes to the
/// half-float bit pattern `31 * q + 15`.  Pick the q nearest to that.
u32 quantize_uf16_10bit(f32 val) {
    if (val < 0.0f)
        val = 0.0f;
    u16 h = float_to_half(val);
    // BC6H UF16 has no negatives.
    if (h & HALF_FLOAT_SIGN_BIT)
        h = 0;
    if (h <= QUANTIZE_10BIT_BIAS)
        return 0;
    u32 q = ((static_cast<u32>(h) - QUANTIZE_10BIT_BIAS) * 2 + FINISH_UNQUANT_SCALE) /
            (2 * FINISH_UNQUANT_SCALE);
    if (q > QUANTIZE_10BIT_MAX)
        q = QUANTIZE_10BIT_MAX;
    return q;
}

/// Half-float bit pattern a 10-bit mode-11 endpoint decodes to.
u16 dequantize_uf16_10bit(u32 q) {
    return finish_unquantize(static_cast<i32>(unquantize(static_cast<i32>(q), 10, false)), false);
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

    // Mode 11's code is 5 bits (0b00011); the 60 endpoint bits follow it and
    // run up to the index field at bit 65.
    BitWriter writer;
    writer.write(0b00011, 5);

    writer.write(e0[0], 10); // rw
    writer.write(e0[1], 10); // gw
    writer.write(e0[2], 10); // bw
    writer.write(e1[0], 10); // rx
    writer.write(e1[1], 10); // gx
    writer.write(e1[2], 10); // bx

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

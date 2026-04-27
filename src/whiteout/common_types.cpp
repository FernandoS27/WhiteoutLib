// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/common_types.h>

#include <cstring>

namespace whiteout {

namespace {
constexpr u32 F32_EXPONENT_MASK = 0x7F800000u;
constexpr u32 F32_MANTISSA_MASK = 0x007FFFFFu;
constexpr i32 F32_EXPONENT_BIAS = 127;
constexpr u32 F32_IMPLICIT_BIT = 0x00800000u;
constexpr u32 F32_MANTISSA_BITS = 23;

constexpr u16 F16_SIGN_MASK = 0x8000u;
constexpr u16 F16_MANTISSA_MASK = 0x03FFu;
constexpr i32 F16_EXPONENT_BIAS = 15;
constexpr u32 F16_MANTISSA_BITS = 10;
constexpr u32 F16_INF_BITS = 0x7C00u;

constexpr i32 F16_MAX_EXPONENT = 15;
constexpr i32 F16_MIN_NORMAL_EXPONENT = -14;
constexpr i32 F16_MIN_SUBNORMAL_EXPONENT = -24;

constexpr u32 MANTISSA_SHIFT = F32_MANTISSA_BITS - F16_MANTISSA_BITS; // 13
constexpr u32 ROUND_BIT_MASK = (1u << MANTISSA_SHIFT) - 1u;           // 0x1FFF
constexpr u32 ROUND_MIDPOINT = 1u << (MANTISSA_SHIFT - 1u);           // 0x1000
} // namespace

u16 f16::from_float_impl(f32 input) {
    u32 float_bits;
    static_assert(sizeof(f32) == sizeof(u32));
    std::memcpy(&float_bits, &input, sizeof(float_bits));

    const u32 sign_bit = (float_bits >> 16) & F16_SIGN_MASK;
    i32 exponent =
        static_cast<i32>((float_bits & F32_EXPONENT_MASK) >> F32_MANTISSA_BITS) - F32_EXPONENT_BIAS;
    u32 mantissa = float_bits & F32_MANTISSA_MASK;

    // Inf / NaN — exponent is all 1s in f32
    if (exponent == (F32_EXPONENT_BIAS + 1)) {
        const u16 nan_payload =
            mantissa ? static_cast<u16>(0x7E00u | (mantissa >> MANTISSA_SHIFT)) : 0;
        return static_cast<u16>(sign_bit | F16_INF_BITS | nan_payload);
    }

    // Overflow → ±Inf
    if (exponent > F16_MAX_EXPONENT) {
        return static_cast<u16>(sign_bit | F16_INF_BITS);
    }

    // Normal f16 range
    if (exponent >= F16_MIN_NORMAL_EXPONENT) {
        u32 truncated_mantissa = mantissa >> MANTISSA_SHIFT;
        const u32 round_remainder = mantissa & ROUND_BIT_MASK;

        // Round-to-nearest-even
        const bool above_midpoint = round_remainder > ROUND_MIDPOINT;
        const bool at_midpoint_odd =
            (round_remainder == ROUND_MIDPOINT) && (truncated_mantissa & 1u);
        if (above_midpoint || at_midpoint_odd) {
            truncated_mantissa++;
            if (truncated_mantissa > F16_MANTISSA_MASK) {
                truncated_mantissa = 0;
                exponent++;
                if (exponent > F16_MAX_EXPONENT) {
                    return static_cast<u16>(sign_bit | F16_INF_BITS);
                }
            }
        }

        const u32 biased_exponent = static_cast<u32>(exponent + F16_EXPONENT_BIAS)
                                    << F16_MANTISSA_BITS;
        return static_cast<u16>(sign_bit | biased_exponent | truncated_mantissa);
    }

    // Subnormal f16 range
    if (exponent >= F16_MIN_SUBNORMAL_EXPONENT) {
        const u32 full_mantissa = mantissa | F32_IMPLICIT_BIT; // restore implicit leading 1
        const u32 shift_amount =
            static_cast<u32>(F16_EXPONENT_BIAS - exponent + F32_MANTISSA_BITS - F16_MANTISSA_BITS);

        u32 subnormal_mantissa = full_mantissa >> shift_amount;
        const u32 round_remainder = full_mantissa & ((1u << shift_amount) - 1u);
        const u32 midpoint = 1u << (shift_amount - 1u);

        const bool above_midpoint = round_remainder > midpoint;
        const bool at_midpoint_odd = (round_remainder == midpoint) && (subnormal_mantissa & 1u);
        if (above_midpoint || at_midpoint_odd) {
            subnormal_mantissa++;
        }

        return static_cast<u16>(sign_bit | subnormal_mantissa);
    }

    // Too small → ±0
    return static_cast<u16>(sign_bit);
}

f32 f16::to_float_impl(u16 half_bits) {
    const u32 sign_bit = static_cast<u32>(half_bits & F16_SIGN_MASK) << 16;
    u32 exponent = (half_bits >> F16_MANTISSA_BITS) & 0x1Fu;
    u32 mantissa = half_bits & F16_MANTISSA_MASK;

    constexpr u32 EXPONENT_REBIAS = F32_EXPONENT_BIAS - F16_EXPONENT_BIAS; // 112

    u32 float_bits;

    // Zero or subnormal
    if (exponent == 0) {
        if (mantissa == 0) {
            float_bits = sign_bit; // ±0
        } else {
            // Subnormal → normalize: shift mantissa until the implicit bit is in position
            exponent = 1;
            while (!(mantissa & (F16_MANTISSA_MASK + 1))) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= F16_MANTISSA_MASK;
            float_bits = sign_bit |
                         (static_cast<u32>(exponent + EXPONENT_REBIAS) << F32_MANTISSA_BITS) |
                         (mantissa << MANTISSA_SHIFT);
        }
    } else if (exponent == 31) {
        // Inf / NaN
        float_bits = sign_bit | F32_EXPONENT_MASK | (mantissa << MANTISSA_SHIFT);
    } else {
        // Normal
        float_bits = sign_bit |
                     (static_cast<u32>(exponent + EXPONENT_REBIAS) << F32_MANTISSA_BITS) |
                     (mantissa << MANTISSA_SHIFT);
    }

    f32 result;
    std::memcpy(&result, &float_bits, sizeof(result));
    return result;
}

} // namespace whiteout

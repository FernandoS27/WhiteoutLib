// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/common_types.h>

#include <cstring>

namespace whiteout {

u16 f16::from_float_impl(f32 f) {
    u32 bits;
    static_assert(sizeof(f32) == sizeof(u32));
    std::memcpy(&bits, &f, sizeof(bits));

    u32 sign = (bits >> 16) & 0x8000u;
    i32 exp = static_cast<i32>((bits >> 23) & 0xFFu) - 127;
    u32 mant = bits & 0x007FFFFFu;

    // Inf / NaN
    if (exp == 128) {
        if (mant) { // NaN – keep some payload
            return static_cast<u16>(sign | 0x7E00u | (mant >> 13));
        }
        return static_cast<u16>(sign | 0x7C00u); // Inf
    }

    // Overflow -> Inf
    if (exp > 15) {
        return static_cast<u16>(sign | 0x7C00u);
    }

    // Normal or subnormal in f16
    if (exp > -15) {
        // Round-to-nearest-even
        u32 half_mant = mant >> 13;
        u32 remainder = mant & 0x1FFFu;
        if (remainder > 0x1000u || (remainder == 0x1000u && (half_mant & 1u))) {
            half_mant++;
            if (half_mant > 0x3FFu) {
                half_mant = 0;
                exp++;
                if (exp > 15) {
                    return static_cast<u16>(sign | 0x7C00u);
                }
            }
        }
        return static_cast<u16>(sign | (static_cast<u32>(exp + 15) << 10) | half_mant);
    }

    // Subnormal f16 range
    if (exp >= -24) {
        mant |= 0x00800000u;                          // restore implicit leading 1
        u32 shift = static_cast<u32>(-exp - 14 + 23); // total right-shift into 10-bit mantissa
        u32 half_mant = mant >> shift;
        u32 remainder = mant & ((1u << shift) - 1u);
        u32 midpoint = 1u << (shift - 1u);
        if (remainder > midpoint || (remainder == midpoint && (half_mant & 1u))) {
            half_mant++;
        }
        return static_cast<u16>(sign | half_mant);
    }

    // Too small -> +/-0
    return static_cast<u16>(sign);
}

f32 f16::to_float_impl(u16 h) {
    u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    u32 exp = (h >> 10) & 0x1Fu;
    u32 mant = h & 0x03FFu;

    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign; // +/-0
        } else {
            // Subnormal -> normalise
            exp = 1;
            while (!(mant & 0x0400u)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03FFu;
            bits = sign | (static_cast<u32>(exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13); // Inf / NaN
    } else {
        bits = sign | (static_cast<u32>(exp + (127 - 15)) << 23) | (mant << 13);
    }

    f32 result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

} // namespace whiteout

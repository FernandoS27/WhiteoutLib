// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Shared constants and tables for the baseline JPEG encoder and decoder.

#pragma once

#include <array>
#include <numbers>

#include <whiteout/common_types.h>

namespace whiteout {
namespace textures {
namespace jpeg {

// ============================================================================
// Block & Component Limits
// ============================================================================

/// Side length of a JPEG DCT block (always 8).
inline constexpr i32 BLOCK_SIZE = 8;

/// Total number of coefficients in one 8×8 block.
inline constexpr i32 BLOCK_PIXELS = BLOCK_SIZE * BLOCK_SIZE; // 64

/// Maximum number of image components supported (JPEG baseline limit is 4).
inline constexpr i32 MAX_COMPONENTS = 4;

/// Maximum number of quantisation / Huffman tables (JPEG spec allows 4 each).
inline constexpr i32 MAX_TABLES = 4;

// ============================================================================
// DCT / IDCT Constants — Loeffler-Ligtenberg-Moschytz (LLM) Butterfly
// ============================================================================
//
// All constants are derived from the DCT basis functions using the notation:
//   c_k = cos(k * pi / 16)
//
// The LLM butterfly decomposes the 8-point DCT into cascaded 2-point rotations.
// Each rotation is implemented with 3 multiplications instead of 4, at the cost
// of pre-combining sin/cos terms.
//
// Reference: Loeffler, Ligtenberg & Moschytz, "Practical Fast 1-D DCT
//            Algorithms with 11 Multiplications", IEEE ICASSP 1989.
// ============================================================================

inline constexpr f32 PI_F = std::numbers::pi_v<f32>;
inline constexpr f32 SQRT2_F = std::numbers::sqrt2_v<f32>;

/// Constexpr cosine via Taylor series (sufficient precision for f32).
/// Valid for any real x; internally reduces to [0, pi].
constexpr f32 cx_cos(f32 x) {
    // Reduce to [0, 2*pi].
    if (x < 0.0f)
        x = -x;
    while (x > 2.0f * PI_F)
        x -= 2.0f * PI_F;
    // cos(x) = -cos(x - pi) for x in (pi, 2*pi].
    f32 sign = 1.0f;
    if (x > PI_F) {
        x = 2.0f * PI_F - x;
    }
    if (x > PI_F / 2.0f) {
        x = PI_F - x;
        sign = -1.0f;
    }
    // Taylor: cos(x) = 1 - x^2/2! + x^4/4! - ...  (12 terms for f32 precision).
    f32 x2 = x * x;
    f32 term = 1.0f, sum = 1.0f;
    for (i32 n = 1; n <= 12; ++n) {
        term *= -x2 / static_cast<f32>((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sign * sum;
}

/// Constexpr sine via cos identity: sin(x) = cos(pi/2 - x).
constexpr f32 cx_sin(f32 x) {
    return cx_cos(PI_F / 2.0f - x);
}

/// Helper: cos(k * pi / 16).
constexpr f32 dct_cos(i32 k) {
    return cx_cos(static_cast<f32>(k) * PI_F / 16.0f);
}

/// Helper: sin(k * pi / 16).
constexpr f32 dct_sin(i32 k) {
    return cx_sin(static_cast<f32>(k) * PI_F / 16.0f);
}

// -- Shared normalisation ---------------------------------------------------

/// 2-D DCT/IDCT normalisation factor: 1 / BLOCK_SIZE = 1/8.
inline constexpr f32 DCT_2D_NORMALISATION = 1.0f / static_cast<f32>(BLOCK_SIZE);

// -- Even-part rotation (indices 2 & 6) -------------------------------------
//
// Implements the 2-point rotation by 3*pi/8, scaled by sqrt(2):
//   even2 = x2 * sqrt2*c6 - x6 * sqrt2*c2
//   even3 = x2 * sqrt2*c2 + x6 * sqrt2*c6
// Using 3 multiplies:
//   rotation = (x2 + x6) * K
//   even2    = rotation - x6 * A     (where A = sqrt2*(c2 + c6))
//   even3    = rotation + x2 * B     (where B = sqrt2*(c2 - c6))

/// sqrt(2) * cos(6*pi/16)  —  the shared rotation factor.
inline constexpr f32 EVEN_ROTATION_K = SQRT2_F * dct_cos(6);

/// sqrt(2) * (cos(2*pi/16) + cos(6*pi/16))  —  correction for x6.
inline constexpr f32 EVEN_ROTATION_A = SQRT2_F * (dct_cos(2) + dct_cos(6));

/// sqrt(2) * (cos(2*pi/16) - cos(6*pi/16))  —  correction for x2.
inline constexpr f32 EVEN_ROTATION_B = SQRT2_F * (dct_cos(2) - dct_cos(6));

// -- Odd-part scale factor --------------------------------------------------

/// sqrt(2) * cos(3*pi/16)  —  the cross-rotation scale factor.
inline constexpr f32 ODD_SCALE = SQRT2_F * dct_cos(3);

// -- Odd-part per-input coefficients ----------------------------------------
//
// Each input x_k is multiplied by a combined coefficient that folds the
// individual rotation corrections into one multiply per input.

/// sqrt(2) * (-c1 + c3 + c5 - c7)  —  coefficient for x7.
inline constexpr f32 ODD_COEFF_X7 = SQRT2_F * (-dct_cos(1) + dct_cos(3) + dct_cos(5) - dct_cos(7));

/// sqrt(2) * ( c1 + c3 - c5 + c7)  —  coefficient for x5.
inline constexpr f32 ODD_COEFF_X5 = SQRT2_F * (dct_cos(1) + dct_cos(3) - dct_cos(5) + dct_cos(7));

/// sqrt(2) * ( c1 + c3 + c5 - c7)  —  coefficient for x3.
inline constexpr f32 ODD_COEFF_X3 = SQRT2_F * (dct_cos(1) + dct_cos(3) + dct_cos(5) - dct_cos(7));

/// sqrt(2) * ( c1 + c3 - c5 - c7)  —  coefficient for x1.
inline constexpr f32 ODD_COEFF_X1 = SQRT2_F * (dct_cos(1) + dct_cos(3) - dct_cos(5) - dct_cos(7));

// -- Odd-part pair corrections ----------------------------------------------
//
// After the per-input multiplies, pair sums are corrected with these factors.

/// sqrt(2) * (c7 - c3)  —  correction for (x7 + x1).
inline constexpr f32 ODD_PAIR_71 = SQRT2_F * (dct_cos(7) - dct_cos(3));

/// sqrt(2) * (-c1 - c3)  —  correction for (x5 + x3).
inline constexpr f32 ODD_PAIR_53 = SQRT2_F * (-dct_cos(1) - dct_cos(3));

/// sqrt(2) * (-c3 - c5)  —  correction for (x7 + x3).
inline constexpr f32 ODD_PAIR_73 = SQRT2_F * (-dct_cos(3) - dct_cos(5));

/// sqrt(2) * (c5 - c3)  —  correction for (x5 + x1).
inline constexpr f32 ODD_PAIR_51 = SQRT2_F * (dct_cos(5) - dct_cos(3));

// -- Forward DCT specific constants -----------------------------------------

/// cos(pi/4) = 1 / sqrt(2)  —  used in the FDCT even-part butterfly.
inline constexpr f32 COS_PI_OVER_4 = 1.0f / SQRT2_F;

/// sin(pi/8)  —  used in the FDCT odd-part rotation.
inline constexpr f32 SIN_PI_OVER_8 = dct_sin(2);

/// sqrt(2) * cos(pi/8)  —  used in the FDCT odd-part rotation.
inline constexpr f32 SQRT2_COS_PI_OVER_8 = SQRT2_F * dct_cos(2);

// -- DC level shift ---------------------------------------------------------

/// DC level shift applied before FDCT / after IDCT (JPEG baseline = 128).
inline constexpr f32 DC_LEVEL_SHIFT = 128.0f;

/// DC level shift + 0.5 rounding bias, combined for float→int truncation.
inline constexpr f32 DC_LEVEL_SHIFT_AND_ROUND = DC_LEVEL_SHIFT + 0.5f;

// ============================================================================
// Zig-Zag Scan Order
// ============================================================================

/// JPEG zig-zag scan order.  Maps coefficient index in the encoded stream to
/// the natural (row-major) position inside an 8×8 block.
inline constexpr std::array<u8, BLOCK_PIXELS> ZIGZAG_ORDER = {{
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
}};

// ============================================================================
// JPEG Marker Bytes  (the byte that follows the 0xFF prefix)
// ============================================================================

inline constexpr u8 MARKER_SOF0 = 0xC0; // Baseline DCT
inline constexpr u8 MARKER_SOF2 = 0xC2; // Progressive DCT
inline constexpr u8 MARKER_DHT = 0xC4;  // Define Huffman Table
inline constexpr u8 MARKER_SOI = 0xD8;  // Start Of Image
inline constexpr u8 MARKER_EOI = 0xD9;  // End Of Image
inline constexpr u8 MARKER_SOS = 0xDA;  // Start Of Scan
inline constexpr u8 MARKER_DQT = 0xDB;  // Define Quantization Table
inline constexpr u8 MARKER_DRI = 0xDD;  // Define Restart Interval
inline constexpr u8 MARKER_RST0 = 0xD0; // Restart marker base (RST0–RST7)

} // namespace jpeg
} // namespace textures
} // namespace whiteout

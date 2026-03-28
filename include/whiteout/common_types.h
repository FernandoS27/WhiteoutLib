// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace whiteout {

// ============================================================================
// Basic Types
// ============================================================================

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

/// IEEE 754 half-precision (binary16) floating-point wrapper.
/// Trivially copyable – just a u16 under the hood.
///
/// Layout:  [15] sign  |  [14:10] exponent (bias 15)  |  [9:0] mantissa
struct f16 {
    u16 raw;

    f16() = default;

    /// Implicit conversion **from** float.
    f16(f32 f) : raw(from_float_impl(f)) {}

    /// Implicit conversion **to** float.
    operator f32() const {
        return to_float();
    }

    /// Build from the raw 16-bit pattern.
    static f16 from_raw(u16 bits) {
        f16 r;
        r.raw = bits;
        return r;
    }

    /// Convert a 32-bit float to a half-precision bit pattern.
    static f16 from_float(f32 f) {
        f16 r;
        r.raw = from_float_impl(f);
        return r;
    }

    /// Convert the stored half-precision value to 32-bit float.
    f32 to_float() const {
        return to_float_impl(raw);
    }

    // -- comparison (operate on the float value) -----------------------------
    bool operator==(f16 o) const {
        return raw == o.raw;
    }
    bool operator!=(f16 o) const {
        return raw != o.raw;
    }

private:
    /// float -> f16 bit pattern (round-to-nearest-even).
    static u16 from_float_impl(f32 f);

    /// f16 bit pattern -> float.
    static f32 to_float_impl(u16 h);
};

// ============================================================================
// Normalized Integer Types
// ============================================================================

template <typename SInt>
struct snorm;

template <typename UInt>
struct unorm;

template <typename SInt>
struct snorm {
    SInt value;
    using UInt = std::make_unsigned<SInt>::type;

    static constexpr f32 max_value() {
        return static_cast<f32>(std::numeric_limits<SInt>::max());
    }

    // Add constructor from raw value
    static constexpr snorm from_raw(SInt raw) {
        snorm result;
        result.value = raw;
        return result;
    }

    // from [-1, 1]
    static constexpr snorm from_float(f32 f) {
        if (f < -1.0f)
            f = -1.0f;
        if (f > 1.0f)
            f = 1.0f;

        snorm result;
        result.value = static_cast<SInt>(f * max_value());
        return result;
    }

    constexpr operator f32() const {
        return static_cast<f32>(value) / max_value();
    }

    constexpr operator unorm<UInt>() const {
        return unorm<UInt>((static_cast<f32>(*this) + 1.0f) * 0.5f);
    }

    // Arithmetic operators (operate via float, clamp to [-1, 1])
    constexpr snorm operator+(snorm o) const {
        return from_float(f32(*this) + f32(o));
    }
    constexpr snorm operator-(snorm o) const {
        return from_float(f32(*this) - f32(o));
    }
    constexpr snorm operator*(snorm o) const {
        return from_float(f32(*this) * f32(o));
    }
    constexpr snorm operator/(snorm o) const {
        return from_float(f32(*this) / f32(o));
    }

    // Unary operators
    constexpr snorm operator-() const {
        return from_raw(static_cast<SInt>(-value));
    }
    constexpr snorm operator+() const {
        return *this;
    }

    // Compound assignment operators
    constexpr snorm& operator+=(snorm o) {
        return *this = *this + o;
    }
    constexpr snorm& operator-=(snorm o) {
        return *this = *this - o;
    }
    constexpr snorm& operator*=(snorm o) {
        return *this = *this * o;
    }
    constexpr snorm& operator/=(snorm o) {
        return *this = *this / o;
    }

    // Comparison operators (raw value comparison preserves ordering)
    constexpr bool operator==(snorm o) const {
        return value == o.value;
    }
    constexpr bool operator!=(snorm o) const {
        return value != o.value;
    }
    constexpr bool operator<(snorm o) const {
        return value < o.value;
    }
    constexpr bool operator>(snorm o) const {
        return value > o.value;
    }
    constexpr bool operator<=(snorm o) const {
        return value <= o.value;
    }
    constexpr bool operator>=(snorm o) const {
        return value >= o.value;
    }
};

template <typename UInt>
struct unorm {
    UInt value;
    using SInt = std::make_signed_t<UInt>;

    static constexpr f32 max_value() {
        return static_cast<f32>(std::numeric_limits<UInt>::max());
    }

    // Add constructor from raw value
    static constexpr unorm from_raw(UInt raw) {
        unorm result;
        result.value = raw;
        return result;
    }

    // from [0, 1]
    static constexpr unorm from_float(f32 f) {
        if (f < 0.0f)
            f = 0.0f;
        if (f > 1.0f)
            f = 1.0f;
        unorm result;
        result.value = static_cast<UInt>(f * max_value() + 0.5f);
        return result;
    }

    constexpr operator f32() const {
        return static_cast<f32>(value) / max_value();
    }

    constexpr operator snorm<SInt>() const {
        return snorm<SInt>::from_float(static_cast<f32>(*this) * 2.0f - 1.0f);
    }

    // Arithmetic operators (operate via float, clamp to [0, 1])
    constexpr unorm operator+(unorm o) const {
        return from_float(f32(*this) + f32(o));
    }
    constexpr unorm operator-(unorm o) const {
        return from_float(f32(*this) - f32(o));
    }
    constexpr unorm operator*(unorm o) const {
        return from_float(f32(*this) * f32(o));
    }
    constexpr unorm operator/(unorm o) const {
        return from_float(f32(*this) / f32(o));
    }

    // Unary operators
    constexpr unorm operator+() const {
        return *this;
    }

    // Compound assignment operators
    constexpr unorm& operator+=(unorm o) {
        return *this = *this + o;
    }
    constexpr unorm& operator-=(unorm o) {
        return *this = *this - o;
    }
    constexpr unorm& operator*=(unorm o) {
        return *this = *this * o;
    }
    constexpr unorm& operator/=(unorm o) {
        return *this = *this / o;
    }

    // Comparison operators (raw value comparison preserves ordering)
    constexpr bool operator==(unorm o) const {
        return value == o.value;
    }
    constexpr bool operator!=(unorm o) const {
        return value != o.value;
    }
    constexpr bool operator<(unorm o) const {
        return value < o.value;
    }
    constexpr bool operator>(unorm o) const {
        return value > o.value;
    }
    constexpr bool operator<=(unorm o) const {
        return value <= o.value;
    }
    constexpr bool operator>=(unorm o) const {
        return value >= o.value;
    }
};

using unorm8 = unorm<u8>;
using unorm16 = unorm<u16>;
using unorm32 = unorm<u32>;

using snorm8 = snorm<i8>;
using snorm16 = snorm<i16>;
using snorm32 = snorm<i32>;

template <typename T, int FractionBits>
struct fixed_point {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                  "fixed_point<T, FractionBits>: T must be an integral type (except bool).");
    static_assert(FractionBits >= 0, "fixed_point<T, FractionBits>: FractionBits must be >= 0.");
    static_assert(FractionBits < std::numeric_limits<T>::digits,
                  "fixed_point<T, FractionBits>: FractionBits is too large for T.");

    using value_type = T;
    using unsigned_type = std::make_unsigned_t<T>;

    static constexpr int fractional_bits = FractionBits;
    static constexpr unsigned_type scale = (unsigned_type{1} << FractionBits);

    // No constructors/member initializers => stays trivial.
    value_type raw;

    static constexpr fixed_point from_raw(value_type v) {
        return fixed_point{v};
    }

    static constexpr fixed_point from_int(value_type v) {
        return fixed_point{static_cast<value_type>(v * static_cast<value_type>(scale))};
    }

    static constexpr fixed_point from_float(f32 v) {
        const f32 scaled = v * static_cast<f32>(scale);
        const f32 rounded = (scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f);
        return fixed_point{static_cast<value_type>(rounded)};
    }

    constexpr value_type to_int() const {
        return static_cast<value_type>(raw / static_cast<value_type>(scale));
    }

    constexpr f32 to_float() const {
        return static_cast<f32>(raw) / static_cast<f32>(scale);
    }

    // Arithmetic operators
    constexpr fixed_point operator+(const fixed_point& other) const {
        return from_raw(raw + other.raw);
    }

    constexpr fixed_point operator-(const fixed_point& other) const {
        return from_raw(raw - other.raw);
    }

    constexpr fixed_point operator*(const fixed_point& other) const {
        return from_raw(static_cast<value_type>((static_cast<i64>(raw) * other.raw) / scale));
    }

    constexpr fixed_point operator/(const fixed_point& other) const {
        return from_raw(static_cast<value_type>((static_cast<i64>(raw) * scale) / other.raw));
    }

    // Unary operators
    constexpr fixed_point operator-() const {
        return from_raw(-raw);
    }

    constexpr fixed_point operator+() const {
        return *this;
    }

    // Compound assignment operators
    constexpr fixed_point& operator+=(const fixed_point& other) {
        raw += other.raw;
        return *this;
    }

    constexpr fixed_point& operator-=(const fixed_point& other) {
        raw -= other.raw;
        return *this;
    }

    constexpr fixed_point& operator*=(const fixed_point& other) {
        raw = static_cast<value_type>((static_cast<i64>(raw) * other.raw) / scale);
        return *this;
    }

    constexpr fixed_point& operator/=(const fixed_point& other) {
        raw = static_cast<value_type>((static_cast<i64>(raw) * scale) / other.raw);
        return *this;
    }

    // Comparison operators
    constexpr bool operator==(const fixed_point& other) const {
        return raw == other.raw;
    }

    constexpr bool operator!=(const fixed_point& other) const {
        return raw != other.raw;
    }

    constexpr bool operator<(const fixed_point& other) const {
        return raw < other.raw;
    }

    constexpr bool operator>(const fixed_point& other) const {
        return raw > other.raw;
    }

    constexpr bool operator<=(const fixed_point& other) const {
        return raw <= other.raw;
    }

    constexpr bool operator>=(const fixed_point& other) const {
        return raw >= other.raw;
    }
};

using fixed8_5 = fixed_point<i8, 5>;
using fixed16_8 = fixed_point<i16, 8>;
using fixed16_11 = fixed_point<i16, 11>;
using fixed32_16 = fixed_point<i32, 16>;
using fixed16_9 = fixed_point<u16, 9>;

} // namespace whiteout

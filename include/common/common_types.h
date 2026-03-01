#pragma once

#include <cstdint>
#include <limits>
#include <array>
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

using f16 = u16; // Half-precision float (not natively supported, will need conversion)
using f32 = float;
using f64 = double;

// ============================================================================
// Normalized Integer Types
// ============================================================================

template<typename SInt>
struct snorm;

template<typename UInt>
struct unorm;

template<typename SInt>
struct snorm {
    SInt value = 0;
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
        if (f < -1.0f) f = -1.0f;
        if (f >  1.0f) f =  1.0f;

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
};

template<typename UInt>
struct unorm {
    UInt value = 0;
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
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
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
};

using unorm8  = unorm<u8>;
using unorm16 = unorm<u16>;
using unorm32 = unorm<u32>;

using snorm8  = snorm<i8>;
using snorm16 = snorm<i16>;
using snorm32 = snorm<i32>;

// ============================================================================
// Vector and Matrix Types
// ============================================================================

struct Vector3f {
    union {
        struct { f32 x, y, z; };
        std::array<f32, 3> data;
    };

    Vector3f() = default;
    constexpr Vector3f(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}
};

struct Vector4f {
    union {
        struct { f32 x, y, z, w; };
        std::array<f32, 4> data;
    };

    Vector4f() = default;
    constexpr Vector4f(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct Quaternion {
    union {
        struct { u16 x, y, z, w; };
        std::array<u16, 4> data;
    };

    Quaternion() = default;
    constexpr Quaternion(u16 x_, u16 y_, u16 z_, u16 w_) : x(x_), y(y_), z(z_), w(w_) {}
    
    // From normalized floats
    static constexpr Quaternion fromFloats(f32 fx, f32 fy, f32 fz, f32 fw) {
        return Quaternion(
            snorm16::from_float(fx).value,
            snorm16::from_float(fy).value,
            snorm16::from_float(fz).value,
            unorm16::from_float(fw).value
        );
    }

    operator Vector4f() const {
        return Vector4f{
            static_cast<f32>(snorm16::from_raw(x)),
            static_cast<f32>(snorm16::from_raw(y)),
            static_cast<f32>(snorm16::from_raw(z)),
            static_cast<f32>(unorm16::from_raw(w))
        };
    }
};

struct Vector2f {
    union {
        struct { f32 x, y; };
        std::array<f32, 2> data;
    };

    Vector2f() = default;
    constexpr Vector2f(f32 x_, f32 y_) : x(x_), y(y_) {}
};

} // namespace whiteout


// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"

#include <cmath>
#include <utility>

namespace whiteout {

// ============================================================================
// Vector and Matrix Types
// ============================================================================

template <typename T, typename BaseType>
struct VectorMethods {
    using FloatType =
        typename std::conditional<std::is_floating_point<BaseType>::value, BaseType, f32>::type;

    // Component-wise arithmetic
    T& operator+=(const T& other) {
        auto& self = static_cast<T&>(*this);
        for (size_t i = 0; i < self.data.size(); i++)
            self.data[i] += other.data[i];
        return self;
    }

    T& operator-=(const T& other) {
        auto& self = static_cast<T&>(*this);
        for (size_t i = 0; i < self.data.size(); i++)
            self.data[i] -= other.data[i];
        return self;
    }

    T operator+(const T& other) const {
        T result = static_cast<const T&>(*this);
        result += other;
        return result;
    }

    T operator-(const T& other) const {
        T result = static_cast<const T&>(*this);
        result -= other;
        return result;
    }

    // Scalar arithmetic
    T& operator*=(FloatType scalar) {
        auto& self = static_cast<T&>(*this);
        for (size_t i = 0; i < self.data.size(); i++)
            self.data[i] *= scalar;
        return self;
    }

    T& operator/=(FloatType scalar) {
        auto& self = static_cast<T&>(*this);
        const FloatType inv = 1.0f / scalar;
        for (size_t i = 0; i < self.data.size(); i++)
            self.data[i] *= inv;
        return self;
    }

    T operator*(FloatType scalar) const {
        T result = static_cast<const T&>(*this);
        result *= scalar;
        return result;
    }

    T operator/(FloatType scalar) const {
        T result = static_cast<const T&>(*this);
        result /= scalar;
        return result;
    }

    // Component-wise multiply/divide
    T& operator*=(const T& other) {
        auto& self = static_cast<T&>(*this);
        for (size_t i = 0; i < self.data.size(); i++)
            self.data[i] *= other.data[i];
        return self;
    }

    T& operator/=(const T& other) {
        auto& self = static_cast<T&>(*this);
        for (size_t i = 0; i < self.data.size(); i++)
            self.data[i] /= other.data[i];
        return self;
    }

    T operator*(const T& other) const {
        T result = static_cast<const T&>(*this);
        result *= other;
        return result;
    }

    T operator/(const T& other) const {
        T result = static_cast<const T&>(*this);
        result /= other;
        return result;
    }

    // Unary negation
    T operator-() const {
        auto& self = static_cast<const T&>(*this);
        T result;
        for (size_t i = 0; i < self.data.size(); i++)
            result.data[i] = -self.data[i];
        return result;
    }

    // Comparison
    bool operator==(const T& other) const {
        auto& self = static_cast<const T&>(*this);
        for (size_t i = 0; i < self.data.size(); i++) {
            if (self.data[i] != other.data[i])
                return false;
        }
        return true;
    }

    bool operator!=(const T& other) const {
        return !(*this == other);
    }

    // Geometric operations
    FloatType dot(const T& other) const {
        auto& self = static_cast<const T&>(*this);
        FloatType result = 0.0f;
        for (size_t i = 0; i < self.data.size(); i++)
            result += self.data[i] * other.data[i];
        return result;
    }

    FloatType length_squared() const {
        return dot(static_cast<const T&>(*this));
    }

    FloatType length() const {
        return std::sqrt(length_squared());
    }

    void normalize() {
        FloatType len = length();
        if (len > 0.0f)
            *this /= len;
    }

    T normalized() const {
        FloatType len = length();
        if (len > 0.0f)
            return *this / len;
        return static_cast<const T&>(*this);
    }

    // Interpolation
    static T lerp(const T& start, const T& end, FloatType t) {
        T result;
        for (size_t i = 0; i < start.data.size(); i++)
            result.data[i] = start.data[i] + t * (end.data[i] - start.data[i]);
        return result;
    }

    // TCB (Kochanek-Bartels) tangents for vector types
    static T tcb_in_tangent(const T& prev, const T& current, const T& next, FloatType tension,
                            FloatType continuity, FloatType bias) {
        FloatType s = (1 - tension) * (1 - continuity) * (1 + bias) * 0.5f;
        FloatType r = (1 - tension) * (1 + continuity) * (1 - bias) * 0.5f;
        return (current - prev) * s + (next - current) * r;
    }

    static T tcb_out_tangent(const T& prev, const T& current, const T& next, FloatType tension,
                             FloatType continuity, FloatType bias) {
        FloatType s = (1 - tension) * (1 + continuity) * (1 + bias) * 0.5f;
        FloatType r = (1 - tension) * (1 - continuity) * (1 - bias) * 0.5f;
        return (current - prev) * s + (next - current) * r;
    }

    static T bezier_lerp(const T& start, const T& outtan, const T& intan, const T& end,
                         FloatType t) {
        const auto simple_pow = [](FloatType base, size_t exp) {
            FloatType result = 1.0f;
            for (size_t i = 0; i < exp; i++)
                result *= base;
            return result;
        };
        T result;
        for (size_t i = 0; i < start.data.size(); i++) {
            FloatType p0 = start.data[i];
            FloatType p1 = outtan.data[i];
            FloatType p2 = intan.data[i];
            FloatType p3 = end.data[i];
            result.data[i] = simple_pow(1 - t, 3) * p0 + 3 * simple_pow(1 - t, 2) * t * p1 +
                             3 * (1 - t) * simple_pow(t, 2) * p2 + simple_pow(t, 3) * p3;
        }
        return result;
    }

    static T hermite_lerp(const T& prev, const T& start, const T& end, const T& next, FloatType t) {
        T result;
        for (size_t i = 0; i < start.data.size(); i++) {
            FloatType p0 = prev.data[i];
            FloatType p1 = start.data[i];
            FloatType p2 = end.data[i];
            FloatType p3 = next.data[i];
            result.data[i] =
                0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t +
                        (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
        }
        return result;
    }
};

template <typename T>
struct Vector2 : public VectorMethods<Vector2<T>, T> {
    union {
        struct {
            T x, y;
        };
        std::array<T, 2> data;
    };

    constexpr Vector2<T> xy() const {
        return *this;
    }
    constexpr Vector2<T> yx() const {
        return {y, x};
    }

    template <typename U>
    constexpr Vector2<U> cast() const {
        return {static_cast<U>(x), static_cast<U>(y)};
    }

    Vector2() = default;
    constexpr Vector2(T x_, T y_) : x(x_), y(y_) {}
};

using Vector2f = Vector2<f32>;
using Vector2i = Vector2<i32>;
using Vector2u = Vector2<u32>;

template <typename T>
struct Vector3 : public VectorMethods<Vector3<T>, T> {
    union {
        struct {
            T x, y, z;
        };
        std::array<T, 3> data;
    };

    constexpr Vector2<T> xy() const {
        return {x, y};
    }
    constexpr Vector2<T> xz() const {
        return {x, z};
    }
    constexpr Vector2<T> yx() const {
        return {y, x};
    }
    constexpr Vector2<T> yz() const {
        return {y, z};
    }
    constexpr Vector2<T> zx() const {
        return {z, x};
    }
    constexpr Vector2<T> zy() const {
        return {z, y};
    }

    constexpr Vector3<T> xyz() const {
        return *this;
    }
    constexpr Vector3<T> xzy() const {
        return {x, z, y};
    }
    constexpr Vector3<T> yxz() const {
        return {y, x, z};
    }
    constexpr Vector3<T> yzx() const {
        return {y, z, x};
    }
    constexpr Vector3<T> zxy() const {
        return {z, x, y};
    }
    constexpr Vector3<T> zyx() const {
        return {z, y, x};
    }

    Vector3() = default;
    constexpr Vector3(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    constexpr Vector3(const Vector2<T>& xy_, T z_) : x(xy_.x), y(xy_.y), z(z_) {}
    constexpr Vector3(T x_, const Vector2<T>& yz_) : x(x_), y(yz_.x), z(yz_.y) {}
};

using Vector3f = Vector3<f32>;
using Vector3d = Vector3<f64>;
using Vector3i = Vector3<i32>;
using Vector3u = Vector3<u32>;

template <typename T>
struct Vector4 : public VectorMethods<Vector4<T>, T> {
    union {
        struct {
            T x, y, z, w;
        };
        std::array<T, 4> data;
    };

    constexpr Vector2<T> xy() const {
        return {x, y};
    }
    constexpr Vector2<T> xz() const {
        return {x, z};
    }
    constexpr Vector2<T> xw() const {
        return {x, w};
    }
    constexpr Vector2<T> yx() const {
        return {y, x};
    }
    constexpr Vector2<T> yz() const {
        return {y, z};
    }
    constexpr Vector2<T> yw() const {
        return {y, w};
    }
    constexpr Vector2<T> zx() const {
        return {z, x};
    }
    constexpr Vector2<T> zy() const {
        return {z, y};
    }
    constexpr Vector2<T> zw() const {
        return {z, w};
    }
    constexpr Vector2<T> wx() const {
        return {w, x};
    }
    constexpr Vector2<T> wy() const {
        return {w, y};
    }
    constexpr Vector2<T> wz() const {
        return {w, z};
    }

    constexpr Vector3<T> xyz() const {
        return {x, y, z};
    }
    constexpr Vector3<T> xyw() const {
        return {x, y, w};
    }
    constexpr Vector3<T> xzy() const {
        return {x, z, y};
    }
    constexpr Vector3<T> xzw() const {
        return {x, z, w};
    }
    constexpr Vector3<T> xwy() const {
        return {x, w, y};
    }
    constexpr Vector3<T> xwz() const {
        return {x, w, z};
    }
    constexpr Vector3<T> yxz() const {
        return {y, x, z};
    }
    constexpr Vector3<T> yxw() const {
        return {y, x, w};
    }
    constexpr Vector3<T> yzx() const {
        return {y, z, x};
    }
    constexpr Vector3<T> yzw() const {
        return {y, z, w};
    }
    constexpr Vector3<T> ywx() const {
        return {y, w, x};
    }
    constexpr Vector3<T> ywz() const {
        return {y, w, z};
    }
    constexpr Vector3<T> zxy() const {
        return {z, x, y};
    }
    constexpr Vector3<T> zxw() const {
        return {z, x, w};
    }
    constexpr Vector3<T> zyx() const {
        return {z, y, x};
    }
    constexpr Vector3<T> zyw() const {
        return {z, y, w};
    }
    constexpr Vector3<T> zwx() const {
        return {z, w, x};
    }
    constexpr Vector3<T> zwy() const {
        return {z, w, y};
    }
    constexpr Vector3<T> wxy() const {
        return {w, x, y};
    }
    constexpr Vector3<T> wxz() const {
        return {w, x, z};
    }
    constexpr Vector3<T> wyx() const {
        return {w, y, x};
    }
    constexpr Vector3<T> wyz() const {
        return {w, y, z};
    }
    constexpr Vector3<T> wzx() const {
        return {w, z, x};
    }
    constexpr Vector3<T> wzy() const {
        return {w, z, y};
    }

    constexpr Vector4<T> xyzw() const {
        return *this;
    }
    constexpr Vector4<T> xywz() const {
        return {x, y, w, z};
    }
    constexpr Vector4<T> xzyw() const {
        return {x, z, y, w};
    }
    constexpr Vector4<T> xzwy() const {
        return {x, z, w, y};
    }
    constexpr Vector4<T> xwyz() const {
        return {x, w, y, z};
    }
    constexpr Vector4<T> xwzy() const {
        return {x, w, z, y};
    }
    constexpr Vector4<T> yxzw() const {
        return {y, x, z, w};
    }
    constexpr Vector4<T> yxwz() const {
        return {y, x, w, z};
    }
    constexpr Vector4<T> yzxw() const {
        return {y, z, x, w};
    }
    constexpr Vector4<T> yzwx() const {
        return {y, z, w, x};
    }
    constexpr Vector4<T> ywxz() const {
        return {y, w, x, z};
    }
    constexpr Vector4<T> ywzx() const {
        return {y, w, z, x};
    }
    constexpr Vector4<T> zxyw() const {
        return {z, x, y, w};
    }
    constexpr Vector4<T> zxwy() const {
        return {z, x, w, y};
    }
    constexpr Vector4<T> zyxw() const {
        return {z, y, x, w};
    }
    constexpr Vector4<T> zywx() const {
        return {z, y, w, x};
    }
    constexpr Vector4<T> zwxy() const {
        return {z, w, x, y};
    }
    constexpr Vector4<T> zwyx() const {
        return {z, w, y, x};
    }
    constexpr Vector4<T> wxyz() const {
        return {w, x, y, z};
    }
    constexpr Vector4<T> wxzy() const {
        return {w, x, z, y};
    }
    constexpr Vector4<T> wyxz() const {
        return {w, y, x, z};
    }
    constexpr Vector4<T> wyzx() const {
        return {w, y, z, x};
    }
    constexpr Vector4<T> wzxy() const {
        return {w, z, x, y};
    }
    constexpr Vector4<T> wzyx() const {
        return {w, z, y, x};
    }

    Vector4() = default;
    constexpr Vector4(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vector4(const Vector2<T>& xy_, T z_, T w_) : x(xy_.x), y(xy_.y), z(z_), w(w_) {}
    constexpr Vector4(const Vector2<T>& xy_, const Vector2<T>& zw_)
        : x(xy_.x), y(xy_.y), z(zw_.x), w(zw_.y) {}
    constexpr Vector4(T x_, const Vector2<T>& yz_, T w_) : x(x_), y(yz_.x), z(yz_.y), w(w_) {}
    constexpr Vector4(T x_, T y_, const Vector2<T>& zw_) : x(x_), y(y_), z(zw_.x), w(zw_.y) {}
    constexpr Vector4(const Vector3<T>& xyz_, T w_) : x(xyz_.x), y(xyz_.y), z(xyz_.z), w(w_) {}
    constexpr Vector4(T x_, const Vector3<T>& yzw_) : x(x_), y(yzw_.x), z(yzw_.y), w(yzw_.z) {}
};

using Vector4f = Vector4<f32>;
using Vector4i = Vector4<i32>;
using Vector4u = Vector4<u32>;

struct Quaternion : public VectorMethods<Quaternion, f32> {
    union {
        struct {
            f32 x, y, z, w;
        };
        std::array<f32, 4> data;
    };

    // Hamilton product (hides VectorMethods component-wise multiply)
    Quaternion& operator*=(const Quaternion& other);
    Quaternion operator*(const Quaternion& other) const;

    // Re-expose scalar multiply from VectorMethods (hidden by quaternion operator*)
    using VectorMethods<Quaternion, f32>::operator*=;
    using VectorMethods<Quaternion, f32>::operator*;

    Quaternion conjugate() const;
    Quaternion inverse() const;
    Quaternion log() const;
    Quaternion exp() const;
    Vector3f to_euler_angles() const;
    std::pair<Vector3f, f32> to_axis_angle() const;
    Vector3f rotate_vector(const Vector3f& v) const;

    static Quaternion ln_dif(const Quaternion& a, const Quaternion& b);

    static std::pair<Quaternion, Quaternion> tcb_tangents(const Quaternion* prev, f32 prev_time,
                                                          const Quaternion& current,
                                                          f32 current_time, f32 tension,
                                                          f32 continuity, f32 bias,
                                                          const Quaternion* next, f32 next_time);

    static Quaternion slerp(const Quaternion& a, const Quaternion& b, f32 t);
    static Quaternion squad(const Quaternion& start, const Quaternion& outtan,
                            const Quaternion& inttan, const Quaternion& end, f32 t);

    static Quaternion identity();
    static Quaternion from_axis_angle(const Vector3f& axis, f32 angle_rad);
    static Quaternion from_euler_angles(const Vector3f& euler_rad);

    Quaternion() = default;
    constexpr Quaternion(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
};

template <typename T, size_t Rows, size_t Cols>
struct MatrixMethods {
    constexpr static size_t rows = Rows;
    constexpr static size_t cols = Cols;

    std::array<std::array<f32, cols>, rows> data;

    T operator+(const T& other) const {
        T result = static_cast<const T&>(*this);
        for (size_t i = 0; i < rows; i++) {
            for (size_t j = 0; j < cols; j++) {
                result.data[i][j] += other.data[i][j];
            }
        }
        return result;
    }

    T operator-(const T& other) const {
        T result = static_cast<const T&>(*this);
        for (size_t i = 0; i < rows; i++) {
            for (size_t j = 0; j < cols; j++) {
                result.data[i][j] -= other.data[i][j];
            }
        }
        return result;
    }

    T operator*(f32 scalar) const {
        T result = static_cast<const T&>(*this);
        for (size_t i = 0; i < rows; i++) {
            for (size_t j = 0; j < cols; j++) {
                result.data[i][j] *= scalar;
            }
        }
        return result;
    }

    T operator*(const T& other) const {
        T result{};
        for (size_t i = 0; i < rows; i++) {
            for (size_t j = 0; j < cols; j++) {
                for (size_t k = 0; k < cols; k++) {
                    result.data[i][j] += static_cast<const T&>(*this).data[i][k] * other.data[k][j];
                }
            }
        }
        return result;
    }

    T& operator+=(const T& other) {
        *this = (*this) + other;
        return static_cast<T&>(*this);
    }

    T& operator-=(const T& other) {
        *this = (*this) - other;
        return static_cast<T&>(*this);
    }

    T& operator*=(const T& other) {
        *this = (*this) * other;
        return static_cast<T&>(*this);
    }

    T& operator*=(f32 scalar) {
        *this = (*this) * scalar;
        return static_cast<T&>(*this);
    }

    T& operator/=(f32 scalar) {
        *this = (*this) * (1.0f / scalar);
        return static_cast<T&>(*this);
    }

    static T identity() {
        T result{};
        for (size_t i = 0; i < rows && i < cols; i++) {
            result.data[i][i] = 1.0f;
        }
        return result;
    }

    static T zero() {
        T result{};
        for (size_t i = 0; i < rows; i++) {
            for (size_t j = 0; j < cols; j++) {
                result.data[i][j] = 0.0f;
            }
        }
        return result;
    }
};

struct Matrix44f : public MatrixMethods<Matrix44f, 4, 4> {

    static Matrix44f translation(const Vector3f& t);
    static Matrix44f rotation(const Quaternion& q);
    static Matrix44f scaling(const Vector3f& s);
    static Matrix44f compose(const Vector3f& translation, const Quaternion& rotation,
                             const Vector3f& scale);
    static Matrix44f inverse(const Matrix44f& m);

    Vector3f extract_translation() const;
    Quaternion extract_rotation() const;
    Vector3f extract_scale() const;

    Matrix44f() = default;
};

struct Matrix33f : public MatrixMethods<Matrix33f, 3, 3> {

    Matrix33f() = default;
};

} // namespace whiteout
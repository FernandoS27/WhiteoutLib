// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"

namespace whiteout {

// ============================================================================
// Vector and Matrix Types
// ============================================================================

template <typename T>
struct VectorMethods {
    T& operator+=(const T& other);
    T& operator-=(const T& other);
    T operator+(const T& other) const;
    T operator-(const T& other) const;
    f32 dot(const T& other) const;
    static T lerp(const T& start, const T& end, f32 t);
    static T bezier_lerp(const T& start, const T& outtan, const T& intan, const T& end, f32 t);
    static T hermite_lerp(const T& prev, const T& start, const T& end, const T& next, f32 t);
};

struct Vector3f : public VectorMethods<Vector3f> {
    union {
        struct {
            f32 x, y, z;
        };
        std::array<f32, 3> data;
    };

    Vector3f() = default;
    constexpr Vector3f(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}
};

struct Vector4f : public VectorMethods<Vector4f> {
    union {
        struct {
            f32 x, y, z, w;
        };
        std::array<f32, 4> data;
    };

    Vector4f() = default;
    constexpr Vector4f(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct Quaternion {
    union {
        struct {
            f32 x, y, z, w;
        };
        std::array<f32, 4> data;
    };

    Quaternion& operator*=(const Quaternion& other);
    Quaternion operator*(const Quaternion& other) const;
    Quaternion& operator+=(const Quaternion& other);
    Quaternion operator+(const Quaternion& other) const;
    Quaternion& operator-=(const Quaternion& other);
    Quaternion operator-(const Quaternion& other) const;
    Quaternion operator-() const;

    f32 dot(const Quaternion& other) const;
    void normalize();

    static Quaternion slerp(const Quaternion& a, const Quaternion& b, f32 t);
    static Quaternion squad(const Quaternion& start, const Quaternion& outtan,
                            const Quaternion& inttan, const Quaternion& end, f32 t);

    Quaternion() = default;
    constexpr Quaternion(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
};

struct Vector2f : public VectorMethods<Vector2f> {
    union {
        struct {
            f32 x, y;
        };
        std::array<f32, 2> data;
    };

    Vector2f() = default;
    constexpr Vector2f(f32 x_, f32 y_) : x(x_), y(y_) {}
};

struct Matrix44f {
    std::array<std::array<f32, 4>, 4> data;

    Matrix44f operator+(const Matrix44f& other) const;
    Matrix44f operator-(const Matrix44f& other) const;
    Matrix44f operator*(f32 scalar) const;
    Matrix44f operator*(const Matrix44f& other) const;
    Matrix44f& operator*=(const Matrix44f& other);

    static Matrix44f identity();
    static Matrix44f translation(const Vector3f& t);
    static Matrix44f rotation(const Quaternion& q);
    static Matrix44f scaling(const Vector3f& s);
    static Matrix44f compose(const Vector3f& translation, const Quaternion& rotation,
                             const Vector3f& scale);
    static Matrix44f inverse(const Matrix44f& m);

    Matrix44f() = default;
};

extern template struct VectorMethods<Vector2f>;
extern template struct VectorMethods<Vector3f>;
extern template struct VectorMethods<Vector4f>;

} // namespace whiteout
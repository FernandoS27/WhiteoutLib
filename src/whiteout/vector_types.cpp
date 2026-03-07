// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/vector_types.h>

#include <cmath>

namespace whiteout {

// ============================================================================
// VectorMethods<T>
// ============================================================================

template <typename T>
T& VectorMethods<T>::operator+=(const T& other) {
    auto& self = static_cast<T&>(*this);
    for (size_t i = 0; i < self.data.size(); i++) {
        self.data[i] += other.data[i];
    }
    return static_cast<T&>(*this);
}

template <typename T>
T& VectorMethods<T>::operator-=(const T& other) {
    auto& self = static_cast<T&>(*this);
    for (size_t i = 0; i < self.data.size(); i++) {
        self.data[i] -= other.data[i];
    }
    return static_cast<T&>(*this);
}

template <typename T>
T VectorMethods<T>::operator+(const T& other) const {
    T result = static_cast<const T&>(*this);
    result += other;
    return result;
}

template <typename T>
T VectorMethods<T>::operator-(const T& other) const {
    T result = static_cast<const T&>(*this);
    result -= other;
    return result;
}

template <typename T>
f32 VectorMethods<T>::dot(const T& other) const {
    auto& self = static_cast<const T&>(*this);
    f32 result = 0.0f;
    for (size_t i = 0; i < self.data.size(); i++) {
        result += self.data[i] * other.data[i];
    }
    return result;
}

template <typename T>
T VectorMethods<T>::lerp(const T& start, const T& end, f32 t) {
    T result;
    for (size_t i = 0; i < start.data.size(); i++) {
        result.data[i] = start.data[i] + t * (end.data[i] - start.data[i]);
    }
    return result;
}

template <typename T>
T VectorMethods<T>::bezier_lerp(const T& start, const T& outtan, const T& intan, const T& end,
                                f32 t) {
    const auto simple_pow = [](f32 base, size_t exp) {
        f32 result = 1.0f;
        for (size_t i = 0; i < exp; i++) {
            result *= base;
        }
        return result;
    };
    T result;
    for (size_t i = 0; i < start.data.size(); i++) {
        f32 p0 = start.data[i];
        f32 p1 = outtan.data[i];
        f32 p2 = intan.data[i];
        f32 p3 = end.data[i];
        result.data[i] = simple_pow(1 - t, 3) * p0 + 3 * simple_pow(1 - t, 2) * t * p1 +
                         3 * (1 - t) * simple_pow(t, 2) * p2 + simple_pow(t, 3) * p3;
    }
    return result;
}

template <typename T>
T VectorMethods<T>::hermite_lerp(const T& prev, const T& start, const T& end, const T& next,
                                 f32 t) {
    T result;
    for (size_t i = 0; i < start.data.size(); i++) {
        f32 p0 = prev.data[i];
        f32 p1 = start.data[i];
        f32 p2 = end.data[i];
        f32 p3 = next.data[i];
        result.data[i] =
            0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t +
                    (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
    }
    return result;
}

// Explicit instantiations
template struct VectorMethods<Vector2f>;
template struct VectorMethods<Vector3f>;
template struct VectorMethods<Vector4f>;

// ============================================================================
// Quaternion
// ============================================================================

Quaternion& Quaternion::operator*=(const Quaternion& other) {
    f32 newX = w * other.x + x * other.w + y * other.z - z * other.y;
    f32 newY = w * other.y - x * other.z + y * other.w + z * other.x;
    f32 newZ = w * other.z + x * other.y - y * other.x + z * other.w;
    f32 newW = w * other.w - x * other.x - y * other.y - z * other.z;
    x = newX;
    y = newY;
    z = newZ;
    w = newW;
    return *this;
}

Quaternion Quaternion::operator*(const Quaternion& other) const {
    Quaternion result = *this;
    result *= other;
    return result;
}

Quaternion& Quaternion::operator+=(const Quaternion& other) {
    x += other.x;
    y += other.y;
    z += other.z;
    w += other.w;
    return *this;
}

Quaternion Quaternion::operator+(const Quaternion& other) const {
    Quaternion result = *this;
    result += other;
    return result;
}

Quaternion& Quaternion::operator-=(const Quaternion& other) {
    x -= other.x;
    y -= other.y;
    z -= other.z;
    w -= other.w;
    return *this;
}

Quaternion Quaternion::operator-(const Quaternion& other) const {
    Quaternion result = *this;
    result -= other;
    return result;
}

Quaternion Quaternion::operator-() const {
    return Quaternion(-x, -y, -z, -w);
}

f32 Quaternion::dot(const Quaternion& other) const {
    return x * other.x + y * other.y + z * other.z + w * other.w;
}

void Quaternion::normalize() {
    f32 len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > 0.0f) {
        f32 invLen = 1.0f / len;
        x *= invLen;
        y *= invLen;
        z *= invLen;
        w *= invLen;
    }
}

Quaternion Quaternion::slerp(const Quaternion& a, const Quaternion& b, f32 t) {
    // Compute the cosine of the angle between the two quaternions
    f32 d = a.dot(b);

    // If the dot product is negative, slerp won't take the shorter path.
    // Fix by reversing one quaternion.
    Quaternion end = b;
    if (d < 0.0f) {
        d = -d;
        end.x = -end.x;
        end.y = -end.y;
        end.z = -end.z;
        end.w = -end.w;
    }

    const f32 DOT_THRESHOLD = 0.9995f;
    if (d > DOT_THRESHOLD) {
        // If the quaternions are close, use linear interpolation
        return Quaternion(a.x + t * (end.x - a.x), a.y + t * (end.y - a.y), a.z + t * (end.z - a.z),
                          a.w + t * (end.w - a.w));
    }

    // Calculate the angle between the quaternions
    f32 theta_0 = std::acos(d); // angle between input quaternions
    f32 theta = theta_0 * t;    // angle between a and result
    f32 sin_theta = std::sin(theta);
    f32 sin_theta_0 = std::sin(theta_0);

    f32 s0 =
        std::cos(theta) - d * sin_theta / sin_theta_0; // == sin(theta_0 - theta) / sin(theta_0)
    f32 s1 = sin_theta / sin_theta_0;

    return Quaternion((a.x * s0) + (end.x * s1), (a.y * s0) + (end.y * s1),
                      (a.z * s0) + (end.z * s1), (a.w * s0) + (end.w * s1));
}

Quaternion Quaternion::squad(const Quaternion& start, const Quaternion& outtan,
                             const Quaternion& inttan, const Quaternion& end, f32 t) {
    Quaternion slerp1 = slerp(start, outtan, t);
    Quaternion slerp2 = slerp(inttan, end, t);
    return slerp(slerp1, slerp2, 2 * t * (1 - t));
}

// ============================================================================
// Matrix44f
// ============================================================================

Matrix44f Matrix44f::operator+(const Matrix44f& other) const {
    Matrix44f result{};
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            result.data[i][j] = data[i][j] + other.data[i][j];
        }
    }
    return result;
}

Matrix44f Matrix44f::operator-(const Matrix44f& other) const {
    Matrix44f result{};
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            result.data[i][j] = data[i][j] - other.data[i][j];
        }
    }
    return result;
}

Matrix44f Matrix44f::operator*(f32 scalar) const {
    Matrix44f result{};
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            result.data[i][j] = data[i][j] * scalar;
        }
    }
    return result;
}

Matrix44f Matrix44f::operator*(const Matrix44f& other) const {
    Matrix44f result{};
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            for (size_t k = 0; k < 4; k++) {
                result.data[i][j] += data[i][k] * other.data[k][j];
            }
        }
    }
    return result;
}

Matrix44f& Matrix44f::operator*=(const Matrix44f& other) {
    *this = (*this) * other;
    return *this;
}

Matrix44f Matrix44f::identity() {
    Matrix44f result{};
    for (size_t i = 0; i < 4; i++) {
        result.data[i][i] = 1.0f;
    }
    return result;
}

Matrix44f Matrix44f::translation(const Vector3f& t) {
    Matrix44f result = identity();
    result.data[3][0] = t.x;
    result.data[3][1] = t.y;
    result.data[3][2] = t.z;
    return result;
}

Matrix44f Matrix44f::rotation(const Quaternion& q) {
    Matrix44f result = identity();
    f32 xx = q.x * q.x;
    f32 yy = q.y * q.y;
    f32 zz = q.z * q.z;
    f32 xy = q.x * q.y;
    f32 xz = q.x * q.z;
    f32 yz = q.y * q.z;
    f32 wx = q.w * q.x;
    f32 wy = q.w * q.y;
    f32 wz = q.w * q.z;

    result.data[0][0] = 1.0f - 2.0f * (yy + zz);
    result.data[0][1] = 2.0f * (xy - wz);
    result.data[0][2] = 2.0f * (xz + wy);

    result.data[1][0] = 2.0f * (xy + wz);
    result.data[1][1] = 1.0f - 2.0f * (xx + zz);
    result.data[1][2] = 2.0f * (yz - wx);

    result.data[2][0] = 2.0f * (xz - wy);
    result.data[2][1] = 2.0f * (yz + wx);
    result.data[2][2] = 1.0f - 2.0f * (xx + yy);

    return result;
}

Matrix44f Matrix44f::scaling(const Vector3f& s) {
    Matrix44f result{};
    result.data[0][0] = s.x;
    result.data[1][1] = s.y;
    result.data[2][2] = s.z;
    result.data[3][3] = 1.0f;
    return result;
}

Matrix44f Matrix44f::compose(const Vector3f& trans, const Quaternion& rot, const Vector3f& scl) {
    return translation(trans) * rotation(rot) * scaling(scl);
}

Matrix44f Matrix44f::inverse(const Matrix44f& m) {
    Matrix44f inv{};
    f32 det;
    inv.data[0][0] =
        m.data[1][1] * m.data[2][2] * m.data[3][3] - m.data[1][1] * m.data[2][3] * m.data[3][2] -
        m.data[2][1] * m.data[1][2] * m.data[3][3] + m.data[2][1] * m.data[1][3] * m.data[3][2] +
        m.data[3][1] * m.data[1][2] * m.data[2][3] - m.data[3][1] * m.data[1][3] * m.data[2][2];
    inv.data[0][1] =
        -m.data[0][1] * m.data[2][2] * m.data[3][3] + m.data[0][1] * m.data[2][3] * m.data[3][2] +
        m.data[2][1] * m.data[0][2] * m.data[3][3] - m.data[2][1] * m.data[0][3] * m.data[3][2] -
        m.data[3][1] * m.data[0][2] * m.data[2][3] + m.data[3][1] * m.data[0][3] * m.data[2][2];
    inv.data[0][2] =
        m.data[0][1] * m.data[1][2] * m.data[3][3] - m.data[0][1] * m.data[1][3] * m.data[3][2] -
        m.data[1][1] * m.data[0][2] * m.data[3][3] + m.data[1][1] * m.data[0][3] * m.data[3][2] +
        m.data[3][1] * m.data[0][2] * m.data[1][3] - m.data[3][1] * m.data[0][3] * m.data[1][2];
    inv.data[0][3] =
        -m.data[0][1] * m.data[1][2] * m.data[2][3] + m.data[0][1] * m.data[1][3] * m.data[2][2] +
        m.data[1][1] * m.data[0][2] * m.data[2][3] - m.data[1][1] * m.data[0][3] * m.data[2][2] -
        m.data[2][1] * m.data[0][2] * m.data[1][3] + m.data[2][1] * m.data[0][3] * m.data[1][2];
    det = m.data[0][0] * inv.data[0][0] + m.data[0][1] * inv.data[0][1] +
          m.data[0][2] * inv.data[0][2] + m.data[0][3] * inv.data[0][3];
    if (det == 0)
        return Matrix44f{}; // Not invertible, return zero matrix or handle as needed
    det = 1.0f / det;
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            inv.data[i][j] *= det;
        }
    }
    return inv;
}

} // namespace whiteout

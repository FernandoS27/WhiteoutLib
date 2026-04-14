// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/vector_types.h>

#include <cmath>

namespace whiteout {

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

Quaternion Quaternion::conjugate() const {
    return Quaternion(-x, -y, -z, w);
}

Quaternion Quaternion::inverse() const {
    f32 n = length_squared();
    if (n < 1e-12f)
        return Quaternion(0, 0, 0, 0);
    f32 inv_n = 1.0f / n;
    return Quaternion(-x * inv_n, -y * inv_n, -z * inv_n, w * inv_n);
}

Quaternion Quaternion::log() const {
    // For unit quaternion q = (sin(θ)·axis, cos(θ)):
    // log(q) = (θ·axis, 0)
    f32 cw = w < -1.0f ? -1.0f : (w > 1.0f ? 1.0f : w);
    f32 theta = std::acos(cw);
    f32 sin_theta = std::sin(theta);
    if (std::abs(sin_theta) < 1e-6f)
        return Quaternion(0, 0, 0, 0);
    f32 coeff = theta / sin_theta;
    return Quaternion(x * coeff, y * coeff, z * coeff, 0.0f);
}

Quaternion Quaternion::exp() const {
    // For pure quaternion q = (v, 0):
    // exp(q) = (sin(|v|)/|v| · v, cos(|v|))
    f32 theta = std::sqrt(x * x + y * y + z * z);
    if (theta < 1e-6f)
        return Quaternion(0, 0, 0, 1);
    f32 coeff = std::sin(theta) / theta;
    return Quaternion(x * coeff, y * coeff, z * coeff, std::cos(theta));
}

Quaternion Quaternion::identity() {
    return Quaternion(0, 0, 0, 1);
}

Quaternion Quaternion::from_axis_angle(const Vector3f& axis, f32 angle_rad) {
    f32 half_angle = angle_rad * 0.5f;
    f32 s = std::sin(half_angle);
    return Quaternion(axis.x * s, axis.y * s, axis.z * s, std::cos(half_angle));
}

Quaternion Quaternion::from_euler_angles(const Vector3f& euler_rad) {
    f32 cx = std::cos(euler_rad.x * 0.5f);
    f32 sx = std::sin(euler_rad.x * 0.5f);
    f32 cy = std::cos(euler_rad.y * 0.5f);
    f32 sy = std::sin(euler_rad.y * 0.5f);
    f32 cz = std::cos(euler_rad.z * 0.5f);
    f32 sz = std::sin(euler_rad.z * 0.5f);
    return Quaternion(sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
                      cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz);
}

Quaternion Quaternion::ln_dif(const Quaternion& a, const Quaternion& b) {
    Quaternion diff = a.inverse() * b;
    diff.normalize();
    return diff.log();
}

Vector3f Quaternion::to_euler_angles() const {
    // YXZ intrinsic rotation order (pitch-yaw-roll)
    // x = pitch, y = yaw, z = roll
    const f32 sinp = 2.0f * (w * x - y * z);
    const f32 pitch =
        (std::abs(sinp) >= 1.0f) ? std::copysign(3.14159265358979f * 0.5f, sinp) : std::asin(sinp);
    const f32 yaw = std::atan2(2.0f * (w * y + x * z), 1.0f - 2.0f * (x * x + y * y));
    const f32 roll = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (x * x + z * z));
    return {pitch, yaw, roll};
}

std::pair<Vector3f, f32> Quaternion::to_axis_angle() const {
    const Quaternion q = (w < 0.0f) ? -*this : *this;
    const f32 cw = q.w < -1.0f ? -1.0f : (q.w > 1.0f ? 1.0f : q.w);
    const f32 angle = 2.0f * std::acos(cw);
    const f32 s = std::sqrt(1.0f - cw * cw);
    if (s < 1e-6f)
        return {{1.0f, 0.0f, 0.0f}, angle};
    return {{q.x / s, q.y / s, q.z / s}, angle};
}

Vector3f Quaternion::rotate_vector(const Vector3f& v) const {
    // q * v * q^-1, optimized to avoid full quaternion multiplies
    const Vector3f u{x, y, z};
    const f32 s = w;
    const f32 d = u.dot(v);
    // Cross product u × v
    const Vector3f c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return u * (2.0f * d) + v * (s * s - u.dot(u)) + c * (2.0f * s);
}

/// Computes TCB (Tension-Continuity-Bias / Kochanek-Bartels) spline tangents
/// for quaternion keyframe interpolation via squad().
///
/// Algorithm:
///   1. Compute log-space derivatives from neighboring keyframes
///   2. Adjust for non-uniform keyframe spacing
///   3. Apply TCB weights to blend the derivatives
///   4. Exponentiate back to quaternion space
///
/// @param prev         Previous keyframe value (nullptr at sequence start)
/// @param prev_time    Timestamp of previous keyframe
/// @param current      Current keyframe value
/// @param current_time Timestamp of current keyframe
/// @param tension      [-1,1] How sharp the curve is at this key
/// @param continuity   [-1,1] How much overshoot / undershoot
/// @param bias         [-1,1] Direction preference (toward prev or next)
/// @param next         Next keyframe value (nullptr at sequence end)
/// @param next_time    Timestamp of next keyframe
/// @return {in_tangent, out_tangent} for use with squad()
std::pair<Quaternion, Quaternion> Quaternion::tcb_tangents(
    const Quaternion* const prev, const f32 prev_time, const Quaternion& current,
    const f32 current_time, const f32 tension, const f32 continuity, const f32 bias,
    const Quaternion* const next, const f32 next_time) {

    // Endpoint with no neighbors — return identity tangents
    if (!prev && !next)
        return {current, current};

    // ---- Step 1: Log-space derivatives from neighboring keyframes ----
    // These represent the angular "velocity" of rotation toward/away from neighbors.
    Quaternion log_deriv_prev(0, 0, 0, 0);
    Quaternion log_deriv_next(0, 0, 0, 0);

    if (prev) {
        // Flip to ensure shortest-path (same hemisphere)
        Quaternion aligned_prev = *prev;
        if (current.dot(aligned_prev) < 0.0f)
            aligned_prev = -aligned_prev;
        log_deriv_prev = ln_dif(aligned_prev, current);
    }
    if (next) {
        Quaternion aligned_next = *next;
        if (current.dot(aligned_next) < 0.0f)
            aligned_next = -aligned_next;
        log_deriv_next = ln_dif(current, aligned_next);
    }

    // At sequence endpoints, mirror the single available derivative
    if (!prev)
        log_deriv_prev = log_deriv_next;
    if (!next)
        log_deriv_next = log_deriv_prev;

    // ---- Step 2: Non-uniform keyframe spacing adjustment ----
    // When keys are unevenly spaced, scale derivatives proportionally.
    // As |continuity| → 1, factors lerp toward 1.0 (uniform-spacing behavior).
    f32 spacing_prev = 1.0f;
    f32 spacing_next = 1.0f;

    if (prev && next) {
        const f32 half_span = 0.5f * (next_time - prev_time);
        if (half_span > 0.0f) {
            spacing_prev = (current_time - prev_time) / half_span;
            spacing_next = (next_time - current_time) / half_span;

            const f32 continuity_abs = std::abs(continuity);
            spacing_prev += continuity_abs * (1.0f - spacing_prev);
            spacing_next += continuity_abs * (1.0f - spacing_next);
        }
    }

    // ---- Step 3: TCB blend weights ----
    // Decompose tension/continuity/bias into per-derivative weights for
    // the in-tangent (arriving) and out-tangent (departing) sides.
    const f32 half_tension_compl = 0.5f * (1.0f - tension);
    const f32 cont_minus = 1.0f - continuity;
    const f32 cont_plus = 1.0f + continuity;
    const f32 bias_minus = 1.0f - bias;
    const f32 bias_plus = 1.0f + bias;

    const f32 in_weight_prev = half_tension_compl * cont_plus * bias_plus * spacing_next;
    const f32 in_weight_next = half_tension_compl * cont_minus * bias_minus * spacing_next - 1.0f;
    const f32 out_weight_prev = -half_tension_compl * cont_minus * bias_plus * spacing_prev + 1.0f;
    const f32 out_weight_next = -half_tension_compl * cont_plus * bias_minus * spacing_prev;

    // ---- Step 4: Blend log derivatives and exponentiate back to quaternion space ----
    const Quaternion in_log =
        (log_deriv_prev * in_weight_prev + log_deriv_next * in_weight_next) * 0.5f;
    const Quaternion out_log =
        (log_deriv_prev * out_weight_prev + log_deriv_next * out_weight_next) * 0.5f;

    const Quaternion in_tangent = current * in_log.exp();
    const Quaternion out_tangent = current * out_log.exp();

    return {in_tangent, out_tangent};
}

Quaternion Quaternion::slerp(const Quaternion& a, const Quaternion& b, f32 t) {
    // Compute the cosine of the angle between the two quaternions
    f32 d = a.dot(b);
    d = std::clamp(d, -1.0f, 1.0f);

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
        Quaternion result = Quaternion(
            a.x + t * (end.x - a.x),
            a.y + t * (end.y - a.y),
            a.z + t * (end.z - a.z),
            a.w + t * (end.w - a.w)
        );
        return result.normalized();
    }

    // Calculate the angle between the quaternions
    f32 theta_0 = std::acos(d); // angle between input quaternions
    f32 theta = theta_0 * t;    // angle between a and result

    f32 sin_theta = std::sin(theta);
    f32 sin_theta_0 = std::sin(theta_0);

    if (sin_theta_0 < 1e-6f)
        return a;

    f32 s0 = std::sin(theta_0 - theta) / sin_theta_0;
    f32 s1 = sin_theta / sin_theta_0;

    return Quaternion(
        a.x * s0 + end.x * s1,
        a.y * s0 + end.y * s1,
        a.z * s0 + end.z * s1,
        a.w * s0 + end.w * s1
    );
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

Vector3f Matrix44f::extract_scale() const {
    // Scale = length of each column of the upper-left 3x3 (R * S has columns = R_col * s)
    f32 sx = std::sqrt(data[0][0] * data[0][0] + data[1][0] * data[1][0] + data[2][0] * data[2][0]);
    f32 sy = std::sqrt(data[0][1] * data[0][1] + data[1][1] * data[1][1] + data[2][1] * data[2][1]);
    f32 sz = std::sqrt(data[0][2] * data[0][2] + data[1][2] * data[1][2] + data[2][2] * data[2][2]);
    return {sx, sy, sz};
}

Quaternion Matrix44f::extract_rotation() const {
    // Remove scale from the 3x3 to get a pure rotation matrix, then convert to quaternion
    const Vector3f s = extract_scale();
    const f32 inv_sx = s.x > 0.0f ? 1.0f / s.x : 0.0f;
    const f32 inv_sy = s.y > 0.0f ? 1.0f / s.y : 0.0f;
    const f32 inv_sz = s.z > 0.0f ? 1.0f / s.z : 0.0f;

    // Normalized rotation matrix entries
    const f32 m00 = data[0][0] * inv_sx, m01 = data[0][1] * inv_sy, m02 = data[0][2] * inv_sz;
    const f32 m10 = data[1][0] * inv_sx, m11 = data[1][1] * inv_sy, m12 = data[1][2] * inv_sz;
    const f32 m20 = data[2][0] * inv_sx, m21 = data[2][1] * inv_sy, m22 = data[2][2] * inv_sz;

    // Shepperd's method — numerically stable quaternion extraction
    const f32 trace = m00 + m11 + m22;
    Quaternion q;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    q.normalize();
    return q;
}

Vector3f Matrix44f::extract_translation() const {
    // In T*R*S, row 3 stores the translation transformed by R*S.
    // Undo by dividing out scale per column, then multiplying by the rotation matrix.
    const Vector3f s = extract_scale();
    const f32 inv_sx = s.x > 0.0f ? 1.0f / s.x : 0.0f;
    const f32 inv_sy = s.y > 0.0f ? 1.0f / s.y : 0.0f;
    const f32 inv_sz = s.z > 0.0f ? 1.0f / s.z : 0.0f;

    // Descale row 3 to undo the S factor, yielding R^T * t
    const f32 d0 = data[3][0] * inv_sx;
    const f32 d1 = data[3][1] * inv_sy;
    const f32 d2 = data[3][2] * inv_sz;

    // Normalized rotation matrix columns (= rows of R^T)
    const f32 r00 = data[0][0] * inv_sx, r01 = data[0][1] * inv_sy, r02 = data[0][2] * inv_sz;
    const f32 r10 = data[1][0] * inv_sx, r11 = data[1][1] * inv_sy, r12 = data[1][2] * inv_sz;
    const f32 r20 = data[2][0] * inv_sx, r21 = data[2][1] * inv_sy, r22 = data[2][2] * inv_sz;

    // t = R * descaled_row3 (R * R^T * t = t)
    return {
        r00 * d0 + r01 * d1 + r02 * d2,
        r10 * d0 + r11 * d1 + r12 * d2,
        r20 * d0 + r21 * d1 + r22 * d2,
    };
}

} // namespace whiteout

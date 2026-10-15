// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/m3_uv.h"

#include <cmath>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_uv {

Matrix3x2f MatrixOf(const UvValues& values) {
    const f32 c = std::cos(values.angle);
    const f32 s = std::sin(values.angle);
    const f32 tx = values.tiling.x;
    const f32 ty = values.tiling.y;

    Matrix3x2f matrix;
    matrix.m[0][0] = tx * c;
    matrix.m[0][1] = -tx * s;
    matrix.m[1][0] = ty * s;
    matrix.m[1][1] = ty * c;
    // The pivot and the offset, which the engine applies together: the offset
    // sits beside the 0.5 INSIDE the pre-multiply, so it is subtracted.
    const f32 px = 0.5f + values.offset.x;
    const f32 py = 0.5f + values.offset.y;
    matrix.m[0][2] = 0.5f - (matrix.m[0][0] * px + matrix.m[0][1] * py);
    matrix.m[1][2] = 0.5f - (matrix.m[1][0] * px + matrix.m[1][1] * py);
    return matrix;
}

UvValues ValuesOf(const Matrix3x2f& matrix) {
    // Row 0 is `tiling.x * (cos, -sin)`, so it names the angle; both tilings
    // are then the projection of their row onto that angle.
    UvValues values;
    values.angle = std::atan2(-matrix.m[0][1], matrix.m[0][0]);
    const f32 c = std::cos(values.angle);
    const f32 s = std::sin(values.angle);
    const f32 tx = matrix.m[0][0] * c - matrix.m[0][1] * s;
    const f32 ty = matrix.m[1][0] * s + matrix.m[1][1] * c;
    values.tiling = Vector2f(tx, ty);

    // `matrix.translation = 0.5 - tiling * R * (0.5 + offset)`, read backwards.
    const f32 vx = 0.5f - matrix.m[0][2];
    const f32 vy = 0.5f - matrix.m[1][2];
    const f32 wx = tx != 0.0f ? vx / tx : 0.0f;
    const f32 wy = ty != 0.0f ? vy / ty : 0.0f;
    values.offset = Vector2f(c * wx + s * wy - 0.5f, -s * wx + c * wy - 0.5f);
    return values;
}

} // namespace m3_uv
} // namespace wem
} // namespace models
} // namespace whiteout

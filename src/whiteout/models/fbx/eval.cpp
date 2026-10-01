// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/fbx/eval.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace whiteout {
namespace models {
namespace fbx {

namespace {

constexpr f64 kDegrees = 3.14159265358979323846 / 180.0;

Matrix4d AxisRotation(int axis, f64 degrees) {
    const f64 c = std::cos(degrees * kDegrees);
    const f64 s = std::sin(degrees * kDegrees);
    Matrix4d r;
    const int a = (axis + 1) % 3;
    const int b = (axis + 2) % 3;
    r.m[a][a] = c;
    r.m[a][b] = -s;
    r.m[b][a] = s;
    r.m[b][b] = c;
    return r;
}

f64 Length3(f64 x, f64 y, f64 z) {
    return std::sqrt(x * x + y * y + z * z);
}

struct Decomposed {
    Vec3d translation{0, 0, 0};
    Matrix4d rotation;
    Vec3d scale{1, 1, 1};
};

/// T · R · S read off a column-vector matrix; shear is folded into R.
Decomposed Decompose(const Matrix4d& m) {
    Decomposed d;
    d.translation = m.translation();
    for (int c = 0; c < 3; ++c) {
        const f64 length = Length3(m.m[0][c], m.m[1][c], m.m[2][c]);
        d.scale[static_cast<std::size_t>(c)] = length;
        for (int r = 0; r < 3; ++r) {
            d.rotation.m[r][c] = length > 1e-12 ? m.m[r][c] / length : (r == c ? 1.0 : 0.0);
        }
    }
    return d;
}

Vec3d Reciprocal(const Vec3d& v) {
    return {std::fabs(v[0]) > 1e-12 ? 1.0 / v[0] : 1.0, std::fabs(v[1]) > 1e-12 ? 1.0 / v[1] : 1.0,
            std::fabs(v[2]) > 1e-12 ? 1.0 / v[2] : 1.0};
}

Vec3d Negate(const Vec3d& v) {
    return {-v[0], -v[1], -v[2]};
}

} // namespace

// ============================================================================
// Matrix4d
// ============================================================================

Matrix4d Matrix4d::Translation(const Vec3d& t) {
    Matrix4d out;
    out.m[0][3] = t[0];
    out.m[1][3] = t[1];
    out.m[2][3] = t[2];
    return out;
}

Matrix4d Matrix4d::Scaling(const Vec3d& s) {
    Matrix4d out;
    out.m[0][0] = s[0];
    out.m[1][1] = s[1];
    out.m[2][2] = s[2];
    return out;
}

Matrix4d Matrix4d::Rotation(const Vec3d& degrees, i32 order) {
    const Matrix4d x = AxisRotation(0, degrees[0]);
    const Matrix4d y = AxisRotation(1, degrees[1]);
    const Matrix4d z = AxisRotation(2, degrees[2]);
    // The named axis turns first, so with column vectors it is rightmost.
    switch (order) {
    case 1: // XZY
        return y * z * x;
    case 2: // YZX
        return x * z * y;
    case 3: // YXZ
        return z * x * y;
    case 4: // ZXY
        return y * x * z;
    case 5: // ZYX
        return x * y * z;
    default: // XYZ, and spheric XYZ read as XYZ
        return z * y * x;
    }
}

Matrix4d Matrix4d::FromStored(const std::vector<f64>& values) {
    Matrix4d out;
    if (values.size() < 16) {
        return out;
    }
    // Stored as the SDK keeps an FbxAMatrix: four rows, the last one the
    // translation — the transpose of the column-vector matrix.
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] = values[static_cast<std::size_t>(c * 4 + r)];
        }
    }
    return out;
}

Matrix4d Matrix4d::operator*(const Matrix4d& o) const {
    Matrix4d out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] = m[r][0] * o.m[0][c] + m[r][1] * o.m[1][c] + m[r][2] * o.m[2][c] +
                          m[r][3] * o.m[3][c];
        }
    }
    return out;
}

Matrix4d Matrix4d::inverse() const {
    // Gauss-Jordan with partial pivoting, in double.
    f64 a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r][c];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-300) {
            return Matrix4d{};
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                std::swap(a[col][c], a[pivot][c]);
            }
        }
        const f64 inv = 1.0 / a[col][col];
        for (int c = 0; c < 8; ++c) {
            a[col][c] *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const f64 f = a[r][col];
            if (f == 0.0) {
                continue;
            }
            for (int c = 0; c < 8; ++c) {
                a[r][c] -= f * a[col][c];
            }
        }
    }
    Matrix4d out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] = a[r][c + 4];
        }
    }
    return out;
}

Vec3d Matrix4d::point(const Vec3d& p) const {
    return {m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3]};
}

Vec3d Matrix4d::direction(const Vec3d& d) const {
    return {m[0][0] * d[0] + m[0][1] * d[1] + m[0][2] * d[2],
            m[1][0] * d[0] + m[1][1] * d[1] + m[1][2] * d[2],
            m[2][0] * d[0] + m[2][1] * d[1] + m[2][2] * d[2]};
}

// ============================================================================
// TransformStack
// ============================================================================

TransformStack TransformStack::Of(const PropertyTable& p) {
    TransformStack s;
    s.translation = p.vec3("Lcl Translation");
    s.rotation = p.vec3("Lcl Rotation");
    s.scaling = p.vec3("Lcl Scaling", {1, 1, 1});
    s.rotationOffset = p.vec3("RotationOffset");
    s.rotationPivot = p.vec3("RotationPivot");
    s.scalingOffset = p.vec3("ScalingOffset");
    s.scalingPivot = p.vec3("ScalingPivot");
    s.preRotation = p.vec3("PreRotation");
    s.postRotation = p.vec3("PostRotation");
    s.geometricTranslation = p.vec3("GeometricTranslation");
    s.geometricRotation = p.vec3("GeometricRotation");
    s.geometricScaling = p.vec3("GeometricScaling", {1, 1, 1});
    s.rotationOrder = static_cast<i32>(p.integer("RotationOrder", 0));
    s.rotationActive = p.integer("RotationActive", 0) != 0;
    // The SDK's template states 0 (RrSs); 3ds Max states 1 on its nodes.
    s.inheritType = static_cast<i32>(p.integer("InheritType", 0));
    return s;
}

Matrix4d TransformStack::local() const {
    const i32 order = rotationActive ? rotationOrder : 0;
    const Matrix4d r = Matrix4d::Rotation(rotation, order);
    Matrix4d pre;
    Matrix4d postInverse;
    if (rotationActive) {
        pre = Matrix4d::Rotation(preRotation, 0);
        postInverse = Matrix4d::Rotation(postRotation, 0).inverse();
    }
    return Matrix4d::Translation(translation) * Matrix4d::Translation(rotationOffset) *
           Matrix4d::Translation(rotationPivot) * pre * r * postInverse *
           Matrix4d::Translation(Negate(rotationPivot)) * Matrix4d::Translation(scalingOffset) *
           Matrix4d::Translation(scalingPivot) * Matrix4d::Scaling(scaling) *
           Matrix4d::Translation(Negate(scalingPivot));
}

Matrix4d TransformStack::geometric() const {
    return Matrix4d::Translation(geometricTranslation) * Matrix4d::Rotation(geometricRotation, 0) *
           Matrix4d::Scaling(geometricScaling);
}

bool TransformStack::hasPivots() const {
    const auto nonZero = [](const Vec3d& v) {
        return std::fabs(v[0]) > 1e-9 || std::fabs(v[1]) > 1e-9 || std::fabs(v[2]) > 1e-9;
    };
    return nonZero(rotationOffset) || nonZero(rotationPivot) || nonZero(scalingOffset) ||
           nonZero(scalingPivot) || (rotationActive && (nonZero(postRotation) || rotationOrder != 0));
}

Matrix4d ComposeWorld(const Matrix4d& parentWorld, const TransformStack& parent,
                      const TransformStack& child, const Matrix4d& childLocal) {
    switch (child.inheritType) {
    case 2: // Rrs: the parent's own scaling does not reach the child.
        return parentWorld * Matrix4d::Scaling(Reciprocal(parent.scaling)) * childLocal;
    case 0: {
        // RrSs: rotations compose, then scales, so a non-uniform parent scale
        // stretches along the child's axes instead of shearing it.
        const Decomposed p = Decompose(parentWorld);
        const Decomposed c = Decompose(childLocal);
        const Vec3d at = parentWorld.point(c.translation);
        return Matrix4d::Translation(at) * p.rotation * c.rotation *
               Matrix4d::Scaling({p.scale[0] * c.scale[0], p.scale[1] * c.scale[1],
                                  p.scale[2] * c.scale[2]});
    }
    default: // RSrs
        return parentWorld * childLocal;
    }
}

// ============================================================================
// Curve
// ============================================================================

namespace {

template <class T>
std::vector<T> ArrayOf(const Node& node, const char* name) {
    const Node* child = node.child(name);
    if (child == nullptr || child->properties.empty()) {
        return {};
    }
    const Property& p = child->properties[0];
    std::vector<T> out;
    if constexpr (std::is_same_v<T, i64>) {
        out = p.toI64s();
    } else if constexpr (std::is_same_v<T, i32>) {
        out = p.toI32s();
    } else {
        const std::vector<f64> wide = p.toF64s();
        out.assign(wide.begin(), wide.end());
    }
    return out;
}

/// The two packed weights a key's third attribute float holds, as fractions.
std::pair<f64, f64> Weights(f32 packed) {
    u32 bits = 0;
    std::memcpy(&bits, &packed, sizeof(bits));
    const f64 right = static_cast<f64>(bits & 0xFFFFu) / 9999.0;
    const f64 nextLeft = static_cast<f64>(bits >> 16) / 9999.0;
    return {right, nextLeft};
}

f64 Bezier(f64 p0, f64 p1, f64 p2, f64 p3, f64 u) {
    const f64 v = 1.0 - u;
    return v * v * v * p0 + 3.0 * v * v * u * p1 + 3.0 * v * u * u * p2 + u * u * u * p3;
}

} // namespace

Curve Curve::Of(const Object& object) {
    Curve curve;
    const Node& node = *object.node;
    if (const Node* d = node.child("Default"); d != nullptr && !d->properties.empty()) {
        curve.defaultValue = static_cast<f32>(d->properties[0].toF64());
    }
    curve.times = ArrayOf<i64>(node, "KeyTime");
    curve.values = ArrayOf<f32>(node, "KeyValueFloat");
    if (curve.values.size() != curve.times.size()) {
        const std::size_t n = std::min(curve.values.size(), curve.times.size());
        curve.times.resize(n);
        curve.values.resize(n);
    }
    const std::vector<i32> flags = ArrayOf<i32>(node, "KeyAttrFlags");
    const std::vector<f32> data = ArrayOf<f32>(node, "KeyAttrDataFloat");
    const std::vector<i32> counts = ArrayOf<i32>(node, "KeyAttrRefCount");
    curve.flags.reserve(curve.times.size());
    curve.data.reserve(curve.times.size());
    for (std::size_t a = 0; a < flags.size() && curve.flags.size() < curve.times.size(); ++a) {
        const i32 count = a < counts.size() ? std::max(counts[a], 0) : 1;
        std::array<f32, 4> values{0, 0, 0, 0};
        for (std::size_t i = 0; i < 4 && a * 4 + i < data.size(); ++i) {
            values[i] = data[a * 4 + i];
        }
        for (i32 k = 0; k < count && curve.flags.size() < curve.times.size(); ++k) {
            curve.flags.push_back(flags[a]);
            curve.data.push_back(values);
        }
    }
    // A curve that states fewer attributes than keys interpolates linearly.
    while (curve.flags.size() < curve.times.size()) {
        curve.flags.push_back(keyflags::kLinear);
        curve.data.push_back({0, 0, 0, 0});
    }
    return curve;
}

f64 Curve::evaluate(i64 time) const {
    if (times.empty()) {
        return defaultValue;
    }
    if (time <= times.front()) {
        return values.front();
    }
    if (time >= times.back()) {
        return values.back();
    }
    const std::size_t k =
        static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), time) - times.begin()) - 1;
    const f64 v0 = values[k];
    const f64 v1 = values[k + 1];
    const i32 f = flags[k];
    const f64 span = static_cast<f64>(times[k + 1] - times[k]);
    if (span <= 0.0) {
        return v1;
    }
    const f64 u = static_cast<f64>(time - times[k]) / span;
    if ((f & keyflags::kConstant) != 0) {
        return (f & keyflags::kConstantNext) != 0 ? v1 : v0;
    }
    if ((f & keyflags::kCubic) == 0) {
        return v0 + (v1 - v0) * u;
    }
    // Cubic: slopes are per second; the segment's Bezier controls sit a weight
    // of the span along each tangent (a third when not weighted).
    const f64 seconds = span / static_cast<f64>(kTicksPerSecond);
    f64 slopeRight = data[k][0];
    f64 slopeLeft = data[k][1];
    if ((f & keyflags::kTangentTcb) != 0) {
        // Kochanek-Bartels parameters stand where the slopes would; without a
        // TCB solver the Catmull-Rom tangent is the honest neighbour.
        const std::size_t prev = k > 0 ? k - 1 : k;
        const std::size_t next = k + 2 < times.size() ? k + 2 : k + 1;
        const f64 left = static_cast<f64>(times[k + 1] - times[prev]) / kTicksPerSecond;
        const f64 right = static_cast<f64>(times[next] - times[k]) / kTicksPerSecond;
        slopeRight = left > 0.0 ? (v1 - values[prev]) / left : 0.0;
        slopeLeft = right > 0.0 ? (values[next] - v0) / right : 0.0;
    }
    f64 wRight = 1.0 / 3.0;
    f64 wLeft = 1.0 / 3.0;
    const bool weighted =
        (f & (keyflags::kWeightedRight | keyflags::kWeightedNextLeft)) != 0;
    if (weighted) {
        const auto [right, left] = Weights(data[k][2]);
        if ((f & keyflags::kWeightedRight) != 0) {
            wRight = right;
        }
        if ((f & keyflags::kWeightedNextLeft) != 0) {
            wLeft = left;
        }
    }
    const f64 y1 = v0 + slopeRight * wRight * seconds;
    const f64 y2 = v1 - slopeLeft * wLeft * seconds;
    if (!weighted) {
        return Bezier(v0, y1, y2, v1, u);
    }
    // Weighted: time is a Bezier too; find the parameter whose time is `u`.
    const f64 x1 = wRight;
    const f64 x2 = 1.0 - wLeft;
    f64 lo = 0.0;
    f64 hi = 1.0;
    f64 s = u;
    for (int i = 0; i < 48; ++i) {
        const f64 x = Bezier(0.0, x1, x2, 1.0, s);
        if (std::fabs(x - u) < 1e-12) {
            break;
        }
        if (x < u) {
            lo = s;
        } else {
            hi = s;
        }
        s = 0.5 * (lo + hi);
    }
    return Bezier(v0, y1, y2, v1, s);
}

} // namespace fbx
} // namespace models
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file eval.h
 * @brief FBX evaluation: the node transform stack and animation curves
 *        (FBX_OBJ_DESIGN §7, §8).
 *
 * The stack is Autodesk's published formula, column vectors, in double:
 *
 *     local = T · Roff · Rp · Rpre · R · Rpost⁻¹ · Rp⁻¹ · Soff · Sp · S · Sp⁻¹
 *
 * with `RotationOrder` applying to R alone, and the pre/post rotations and the
 * order counted only while `RotationActive` is set — every 3ds Max bone that
 * carries a pre-rotation sets it. `GeometricTranslation/Rotation/Scaling` move
 * the node's own geometry and nothing below it.
 *
 * Curves evaluate as the SDK's key attributes say: constant (standard or next),
 * linear, and cubic from the stored slopes, weighted tangents included. TCB and
 * velocity keys fall back to the stored slopes, with a note.
 */

#include <array>
#include <optional>
#include <vector>

#include "scene.h"

namespace whiteout {
namespace models {
namespace fbx {

using Vec3d = std::array<f64, 3>;

/// Column vectors: `M * p`, translation in column 3.
struct Matrix4d {
    f64 m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    static Matrix4d Translation(const Vec3d& t);
    static Matrix4d Scaling(const Vec3d& s);
    /// Euler angles in degrees, applied in @p order (0 = XYZ: X first).
    static Matrix4d Rotation(const Vec3d& degrees, i32 order = 0);
    /// The 16 doubles an FBX matrix property stores (`Transform`, `Matrix`):
    /// translation in elements 12-14.
    static Matrix4d FromStored(const std::vector<f64>& values);

    Matrix4d operator*(const Matrix4d& other) const;
    Matrix4d inverse() const;
    Vec3d point(const Vec3d& p) const;
    Vec3d direction(const Vec3d& d) const;
    Vec3d translation() const {
        return {m[0][3], m[1][3], m[2][3]};
    }
};

struct TransformStack {
    Vec3d translation{0, 0, 0};
    Vec3d rotation{0, 0, 0};
    Vec3d scaling{1, 1, 1};
    Vec3d rotationOffset{0, 0, 0};
    Vec3d rotationPivot{0, 0, 0};
    Vec3d scalingOffset{0, 0, 0};
    Vec3d scalingPivot{0, 0, 0};
    Vec3d preRotation{0, 0, 0};
    Vec3d postRotation{0, 0, 0};
    Vec3d geometricTranslation{0, 0, 0};
    Vec3d geometricRotation{0, 0, 0};
    Vec3d geometricScaling{1, 1, 1};
    i32 rotationOrder = 0;
    bool rotationActive = false;
    /// 0 RrSs, 1 RSrs, 2 Rrs (FbxTransform::EInheritType).
    i32 inheritType = 1;

    static TransformStack Of(const PropertyTable& properties);

    Matrix4d local() const;
    Matrix4d geometric() const;
    /// Whether anything beyond plain T·R·S is in play.
    bool hasPivots() const;
};

/// A node's world matrix from its parent's, under its inherit type.
Matrix4d ComposeWorld(const Matrix4d& parentWorld, const TransformStack& parent,
                      const TransformStack& child, const Matrix4d& childLocal);

struct Curve {
    std::vector<i64> times;
    std::vector<f32> values;
    /// Per key, expanded from the run-length attribute arrays.
    std::vector<i32> flags;
    std::vector<std::array<f32, 4>> data;
    f32 defaultValue = 0.0f;

    static Curve Of(const Object& animationCurve);
    f64 evaluate(i64 time) const;
    bool empty() const {
        return times.empty();
    }
};

/// FbxAnimCurveDef key flags, the ones evaluation reads.
namespace keyflags {
inline constexpr i32 kConstant = 0x00000002;
inline constexpr i32 kLinear = 0x00000004;
inline constexpr i32 kCubic = 0x00000008;
inline constexpr i32 kTangentTcb = 0x00000200;
inline constexpr i32 kConstantNext = 0x00000100;
inline constexpr i32 kWeightedRight = 0x01000000;
inline constexpr i32 kWeightedNextLeft = 0x02000000;
inline constexpr i32 kVelocityRight = 0x10000000;
inline constexpr i32 kVelocityNextLeft = 0x20000000;
} // namespace keyflags

} // namespace fbx
} // namespace models
} // namespace whiteout

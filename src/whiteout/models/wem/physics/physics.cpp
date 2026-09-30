// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/physics.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace whiteout {
namespace models {
namespace wem {

const char* ToString(PhysicsShapeKind kind) {
    switch (kind) {
    case PhysicsShapeKind::Box:
        return "box";
    case PhysicsShapeKind::Sphere:
        return "sphere";
    case PhysicsShapeKind::Capsule:
        return "capsule";
    case PhysicsShapeKind::Cylinder:
        return "cylinder";
    case PhysicsShapeKind::ConvexHull:
        return "convexHull";
    case PhysicsShapeKind::TriangleMesh:
        return "triangleMesh";
    case PhysicsShapeKind::Count:
        break;
    }
    return "invalid";
}

const char* ToString(BodyMotion motion) {
    switch (motion) {
    case BodyMotion::Dynamic:
        return "dynamic";
    case BodyMotion::Kinematic:
        return "kinematic";
    case BodyMotion::Static:
        return "static";
    case BodyMotion::Count:
        break;
    }
    return "invalid";
}

const char* ToString(JointKind kind) {
    switch (kind) {
    case JointKind::Spherical:
        return "spherical";
    case JointKind::Revolute:
        return "revolute";
    case JointKind::ConeTwist:
        return "coneTwist";
    case JointKind::Weld:
        return "weld";
    case JointKind::Prismatic:
        return "prismatic";
    case JointKind::Distance:
        return "distance";
    case JointKind::Count:
        break;
    }
    return "invalid";
}

const char* ToString(JointMotorMode mode) {
    switch (mode) {
    case JointMotorMode::Off:
        return "off";
    case JointMotorMode::Position:
        return "position";
    case JointMotorMode::Velocity:
        return "velocity";
    case JointMotorMode::Count:
        break;
    }
    return "invalid";
}

const char* ToString(WowPhysicsKind kind) {
    switch (kind) {
    case WowPhysicsKind::WornItem:
        return "wornItem";
    case WowPhysicsKind::Vegetation:
        return "vegetation";
    case WowPhysicsKind::Ragdoll:
        return "ragdoll";
    case WowPhysicsKind::PrivateWorld:
        return "privateWorld";
    case WowPhysicsKind::Count:
        break;
    }
    return "invalid";
}

namespace {

Vector3f Row(const Matrix44f& m, int r) {
    return Vector3f{m.data[r][0], m.data[r][1], m.data[r][2]};
}

bool IsIdentity(const Matrix44f& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (m.data[r][c] != (r == c ? 1.0f : 0.0f)) {
                return false;
            }
        }
    }
    return true;
}

/// @p p through the row-vector matrix @p m.
Vector3f Through(const Matrix44f& m, const Vector3f& p) {
    return Vector3f{p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0],
                    p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1],
                    p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2]};
}

} // namespace

std::pair<Vector3f, Vector3f> CapsuleEnds(const PhysicsShape& shape) {
    if (shape.points.size() == 2) {
        if (IsIdentity(shape.transform)) {
            return {shape.points[0], shape.points[1]};
        }
        return {Through(shape.transform, shape.points[0]), Through(shape.transform, shape.points[1])};
    }
    const Vector3f o = Row(shape.transform, 3);
    const Vector3f z = Row(shape.transform, 2);
    const f32 h = shape.length * 0.5f;
    const Vector3f e{z.x * h, z.y * h, z.z * h};
    return {Vector3f{o.x - e.x, o.y - e.y, o.z - e.z}, Vector3f{o.x + e.x, o.y + e.y, o.z + e.z}};
}

std::vector<Vector3f> CylinderPrism(const PhysicsShape& shape) {
    constexpr int kSides = 16;
    const f64 turn = 6.283185307179586;
    const f64 wide = shape.radius * std::sqrt(3.141592653589793 / (0.5 * kSides * std::sin(turn / kSides)));
    const f32 half = shape.length * 0.5f;
    std::vector<Vector3f> points;
    points.reserve(kSides * 2);
    for (int k = 0; k < kSides; ++k) {
        const f64 a = turn * k / kSides;
        const f32 x = static_cast<f32>(wide * std::cos(a)), y = static_cast<f32>(wide * std::sin(a));
        points.push_back(Vector3f{x, y, half});
        points.push_back(Vector3f{x, y, -half});
    }
    return points;
}

Matrix44f CapsuleFrame(const Vector3f& a, const Vector3f& b, f32& length) {
    const f64 dx = static_cast<f64>(b.x) - a.x;
    const f64 dy = static_cast<f64>(b.y) - a.y;
    const f64 dz = static_cast<f64>(b.z) - a.z;
    const f64 d = std::sqrt(dx * dx + dy * dy + dz * dz);
    length = static_cast<f32>(d);
    const f64 z[3] = {d > 0.0 ? dx / d : 0.0, d > 0.0 ? dy / d : 0.0, d > 0.0 ? dz / d : 1.0};
    // Any axis far from +Z completes the frame; a capsule has no roll.
    const f64 helper[3] = {std::fabs(z[0]) < 0.9 ? 1.0 : 0.0, std::fabs(z[0]) < 0.9 ? 0.0 : 1.0, 0.0};
    f64 x[3] = {helper[1] * z[2] - helper[2] * z[1], helper[2] * z[0] - helper[0] * z[2],
                helper[0] * z[1] - helper[1] * z[0]};
    const f64 xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (f64& c : x) {
        c /= xl;
    }
    const f64 y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Matrix44f m = Matrix44f::identity();
    for (int c = 0; c < 3; ++c) {
        m.data[0][c] = static_cast<f32>(x[c]);
        m.data[1][c] = static_cast<f32>(y[c]);
        m.data[2][c] = static_cast<f32>(z[c]);
    }
    m.data[3][0] = static_cast<f32>((static_cast<f64>(a.x) + b.x) * 0.5);
    m.data[3][1] = static_cast<f32>((static_cast<f64>(a.y) + b.y) * 0.5);
    m.data[3][2] = static_cast<f32>((static_cast<f64>(a.z) + b.z) * 0.5);
    return m;
}

namespace {

template <class List>
auto FindById(List& list, u32 id) -> decltype(list.data()) {
    const auto it = std::find_if(list.begin(), list.end(), [id](const auto& r) { return r.id == id; });
    return it == list.end() ? nullptr : &*it;
}

} // namespace

const PhysicsBody* PhysicsSet::body(u32 id) const {
    return FindById(bodies, id);
}

PhysicsBody* PhysicsSet::body(u32 id) {
    return FindById(bodies, id);
}

const PhysicsJoint* PhysicsSet::joint(u32 id) const {
    return FindById(joints, id);
}

const ClothCollider* PhysicsSet::collider(u32 id) const {
    return FindById(colliders, id);
}

const Cloth* PhysicsSet::cloth(u32 id) const {
    return FindById(cloths, id);
}

Cloth* PhysicsSet::cloth(u32 id) {
    return FindById(cloths, id);
}

const PhysicsRig* PhysicsSet::rig(u32 id) const {
    return FindById(rigs, id);
}

bool PhysicsSet::hasId(u32 id) const {
    return body(id) != nullptr || joint(id) != nullptr || collider(id) != nullptr ||
           cloth(id) != nullptr || rig(id) != nullptr;
}

const PhysicsBody* PhysicsSet::firstBodyOn(u32 node) const {
    const auto it =
        std::find_if(bodies.begin(), bodies.end(), [node](const PhysicsBody& b) { return b.node == node; });
    return it == bodies.end() ? nullptr : &*it;
}

PhysicsHost PhysicsHostFor(Game game) {
    PhysicsHost host;
    switch (game) {
    case Game::StarCraft:
        // Per-map data really; this is the default the viewer's host uses.
        host.gravity = {0.0f, 0.0f, -8.8f};
        host.step = 1.0f / 60.0f;
        host.maxSubsteps = 3;
        host.velocityIterations = 8;
        host.positionIterations = 2;
        host.snapLinear = 0.25f;
        host.snapAngular = std::numbers::pi_v<f32> / 8.0f;
        host.releaseLinear = 8.0f;
        host.releaseAngular = 4.0f;
        break;
    case Game::Wow:
        host.gravity = {0.0f, 0.0f, -10.0f};
        host.maxSubsteps = 1;
        break;
    case Game::Diablo:
        host.gravity = {0.0f, 0.0f, -32.2f};
        host.maxSubsteps = 1;
        break;
    case Game::Warcraft:
        break;
    }
    return host;
}

} // namespace wem
} // namespace models
} // namespace whiteout

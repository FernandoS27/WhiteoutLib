// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/physics.h>

#include <algorithm>
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

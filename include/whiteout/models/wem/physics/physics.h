// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file physics.h
 * @brief Rigid bodies, joints, cloth and their colliders (WEM_PHYSICS_DESIGN.md §3).
 *
 * **One vocabulary: Domino's.** StarCraft II, World of Warcraft and Diablo III
 * all run their rigid bodies on Domino and StarCraft II's cloth is Domino
 * cloth, so each record here is what a host hands Domino, in model terms. A
 * record holds what the concept means; an optional typed extension (`sc2`)
 * holds what only one game means, and no field is stated twice.
 *
 * **Lists on the model, not node kinds.** A bone that carries a body stays a
 * bone, and a node may carry several bodies. Bodies, joints, colliders and
 * cloths share one id space, from 1 and never reused, so a channel or a joint
 * names a record by id and survives a removal elsewhere.
 *
 * **Frames.** Every matrix maps the record's own frame into its node's frame,
 * in WEM's basis, row vectors (`p * M`). Rows may carry scale; how scale reaches
 * each shape kind is the runtime's rule, not data. Dimensions are what the
 * source authored.
 *
 * **Authoring inputs only.** A hull stores its points and a mesh its triangles;
 * every cooked table is rebuilt on export (`m3::CookHull`, `m3::CookMesh`).
 * Nothing here is a game's gravity, step or iteration count: those are host
 * policy (`PhysicsHostFor`).
 */

#include <optional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include "../profile.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Shapes
// ============================================================================

/// Appended, never reordered: the value is what a `PSHP` chunk stores.
enum class PhysicsShapeKind : u8 { Box, Sphere, Capsule, Cylinder, ConvexHull, TriangleMesh, Count };

const char* ToString(PhysicsShapeKind kind);

struct PhysicsMaterial {
    f32 density = 1000.0f; ///< The StarCraft II upgrade's default.
    f32 friction = 0.3f;
    f32 restitution = 0.0f;

    template <class V>
    void reflect(V& v) {
        v.field("density", density);
        v.field("friction", friction);
        v.field("restitution", restitution);
    }
};

struct PhysicsShape {
    PhysicsShapeKind kind = PhysicsShapeKind::Box;
    /// Own frame -> the body node's frame. A full matrix: rows carry independent
    /// scales that a TRS cannot hold.
    /// @bind skip — no value binding has a `Matrix44f` shape (`Node::poseMatrices`).
    Matrix44f transform = Matrix44f::identity();
    Vector3f halfExtents{0, 0, 0}; ///< Box.
    f32 radius = 0.0f;             ///< Sphere, capsule, cylinder.
    f32 length = 0.0f;             ///< Along own +Z: a capsule's between its cap centres, a cylinder's whole.
    std::vector<Vector3f> points;  ///< Convex hull: the vertex set, own frame.
    std::vector<Vector3f> vertices; ///< Triangle mesh, own frame.
    std::vector<u32> triangles;    ///< Triangle mesh, three per face.
    PhysicsMaterial material;

    template <class V>
    void reflect(V& v) {
        v.field("kind", kind);
        v.field("transform", transform);
        v.field("halfExtents", halfExtents);
        v.field("radius", radius);
        v.field("length", length);
        v.field("points", points);
        v.field("vertices", vertices);
        v.field("triangles", triangles);
        v.field("material", material);
    }
};

// ============================================================================
// Bodies
// ============================================================================

/// How a body is created, in `dmBodyType` order. Whether it simulates at a
/// moment is `PhysicsBody::simulates` and its channel.
enum class BodyMotion : u8 { Dynamic, Kinematic, Static, Count };

const char* ToString(BodyMotion motion);

/// What only StarCraft II means by a body.
struct Sc2BodyExtension {
    u32 physicsMaterial = 0;        ///< `physicsType`: a material id game data may override.
    bool collidable = true;         ///< Flag 0x1.   } The collision filter.
    bool walkable = false;          ///< Flag 0x2.   }
    bool stackable = false;         ///< Flag 0x4.   }
    bool simulateCollision = false; ///< Flag 0x8.   }
    bool keepsBoneDriven = false;   ///< Flag 0x80: setup and deactivation leave the bone's physics bit.
    /// Flags 0x10, 0x20 and 0x200: consistently authored, no reader found in the
    /// client yet (WEM_PHYSICS_DESIGN.md §14), so kept as they came.
    u32 untracedFlags = 0x20;

    template <class V>
    void reflect(V& v) {
        v.field("physicsMaterial", physicsMaterial);
        v.field("collidable", collidable);
        v.field("walkable", walkable);
        v.field("stackable", stackable);
        v.field("simulateCollision", simulateCollision);
        v.field("keepsBoneDriven", keepsBoneDriven);
        v.field("untracedFlags", untracedFlags);
    }
};

struct PhysicsBody {
    u32 id = 0;
    u32 node = kInvalidNode;
    BodyMotion motion = BodyMotion::Kinematic;
    /// The rest value of `Channel::PhysicsDynamic`: whether it simulates where
    /// no key says otherwise.
    bool simulates = true;
    std::vector<PhysicsShape> shapes;
    f32 linearDamping = 0.0f;
    f32 angularDamping = 0.0f;
    f32 inertiaScale = 1.0f;
    f32 gravityScale = 1.0f; ///< StarCraft II hard-wires 1; World of Warcraft authors it.
    /// Takes the nearest bodied ancestor's current state instead of its own
    /// (StarCraft II flag 0x40).
    bool inheritDynamic = false;
    /// Stays kinematic when the model ragdolls (StarCraft II flag 0x100).
    bool exemptFromRagdoll = false;
    /// Which force fields act on it: matched against `ForceFieldPayload::channels`.
    /// StarCraft II's `localForces | worldForces << 16`.
    u32 forceChannels = 0;
    std::optional<Sc2BodyExtension> sc2;

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("node", node);
        v.field("motion", motion);
        v.field("simulates", simulates);
        v.field("shapes", shapes);
        v.field("linearDamping", linearDamping);
        v.field("angularDamping", angularDamping);
        v.field("inertiaScale", inertiaScale);
        v.field("gravityScale", gravityScale);
        v.field("inheritDynamic", inheritDynamic);
        v.field("exemptFromRagdoll", exemptFromRagdoll);
        v.field("forceChannels", forceChannels);
        v.optional("sc2", sc2);
    }
};

// ============================================================================
// Joints
// ============================================================================

/// Appended, never reordered. StarCraft II has the first four.
enum class JointKind : u8 { Spherical, Revolute, ConeTwist, Weld, Prismatic, Distance, Count };

const char* ToString(JointKind kind);

enum class JointFriction : u8 {
    None,
    Torque,      ///< A holding torque (World of Warcraft).
    GravityHold, ///< A multiplier on an estimated gravity-holding torque (StarCraft II).
    Count
};

struct JointSpring {
    f32 hz = 0.0f;
    f32 damping = 0.0f;

    template <class V>
    void reflect(V& v) {
        v.field("hz", hz);
        v.field("damping", damping);
    }
};

struct PhysicsJoint {
    u32 id = 0;
    u32 bodyA = 0; ///< Body id, the parent side.
    u32 bodyB = 0; ///< Body id.
    JointKind kind = JointKind::Spherical;
    Matrix44f frameA = Matrix44f::identity(); ///< The joint frame in body A's node frame. @bind skip
    Matrix44f frameB = Matrix44f::identity(); ///< The joint frame in body B's node frame. @bind skip
    bool collideConnected = false;
    bool limitEnabled = false;
    f32 lower = 0.0f; ///< Radians.
    f32 upper = 0.0f; ///< Radians.
    f32 cone = 0.0f;  ///< Radians; a runtime clamps it to [10°, 170°] on use.
    JointFriction friction = JointFriction::None;
    f32 frictionAmount = 0.0f;
    JointSpring angularSpring; ///< StarCraft II's weld spring; World of Warcraft's weld and motors.
    // World of Warcraft and Diablo III only; the StarCraft II export reports them.
    JointSpring linearSpring;
    f32 restLength = 0.0f;  ///< Distance joint.
    f32 breakForce = 0.0f;  ///< 0 = unbreakable.
    f32 breakTorque = 0.0f; ///< 0 = unbreakable.

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("bodyA", bodyA);
        v.field("bodyB", bodyB);
        v.field("kind", kind);
        v.field("frameA", frameA);
        v.field("frameB", frameB);
        v.field("collideConnected", collideConnected);
        v.field("limitEnabled", limitEnabled);
        v.field("lower", lower);
        v.field("upper", upper);
        v.field("cone", cone);
        v.field("friction", friction);
        v.field("frictionAmount", frictionAmount);
        v.field("angularSpring", angularSpring);
        v.field("linearSpring", linearSpring);
        v.field("restLength", restLength);
        v.field("breakForce", breakForce);
        v.field("breakTorque", breakTorque);
    }
};

// ============================================================================
// Cloth
// ============================================================================

/// A section of a mesh of the same model.
struct SectionRef {
    u32 mesh = kInvalidIndex;
    u32 section = kInvalidIndex;

    bool valid() const {
        return mesh != kInvalidIndex && section != kInvalidIndex;
    }
    bool operator==(const SectionRef&) const = default;

    template <class V>
    void reflect(V& v) {
        v.field("mesh", mesh);
        v.field("section", section);
    }
};

enum class ClothColliderKind : u8 { Capsule, Plane, Count };

/// A collider cloth drapes over. Model-wide: a cloth lists the ones it uses,
/// and colliders no cloth uses are ones this model offers another's cloth.
struct ClothCollider {
    u32 id = 0;
    u32 node = kInvalidNode; ///< `kInvalidNode` is the model root.
    ClothColliderKind kind = ClothColliderKind::Capsule;
    /// Own frame -> the node's frame. A capsule runs along its own +Z,
    /// centred; a plane faces its own +Z (Diablo III).
    /// @bind skip — as `PhysicsShape::transform`.
    Matrix44f transform = Matrix44f::identity();
    f32 radius = 0.0f;
    f32 length = 0.0f; ///< Full length.

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("node", node);
        v.field("kind", kind);
        v.field("transform", transform);
        v.field("radius", radius);
        v.field("length", length);
    }
};

/// A section the cloth drives. Which cage vertices move each of its vertices,
/// and how much, are the `cloth.bind.*` layers of the mesh.
struct ClothBinding {
    SectionRef section;

    template <class V>
    void reflect(V& v) {
        v.field("section", section);
    }
};

/// Domino cloth as StarCraft II tunes it, 1:1 with `PHCL`.
struct Sc2ClothParams {
    f32 tracking = 0.0f;
    f32 horizontalStiffness = 0.0f;
    f32 shearStiffness = 0.1f;
    f32 explosionScale = 0.0f;
    f32 windScale = 0.0f;
    f32 dragFactor = 1.0f;
    f32 liftFactor = 0.5f;
    f32 sphereStiffness = 0.1f;
    bool flatten = false;
    bool useSkinCollision = false;
    f32 skinOffset = 0.0f;
    f32 skinExponent = 0.0f;
    f32 skinStiffness = 0.1f;

    template <class V>
    void reflect(V& v) {
        v.field("tracking", tracking);
        v.field("horizontalStiffness", horizontalStiffness);
        v.field("shearStiffness", shearStiffness);
        v.field("explosionScale", explosionScale);
        v.field("windScale", windScale);
        v.field("dragFactor", dragFactor);
        v.field("liftFactor", liftFactor);
        v.field("sphereStiffness", sphereStiffness);
        v.field("flatten", flatten);
        v.field("useSkinCollision", useSkinCollision);
        v.field("skinOffset", skinOffset);
        v.field("skinExponent", skinExponent);
        v.field("skinStiffness", skinStiffness);
    }
};

/**
 * @brief One cloth: its cage, what it drives, what it collides with.
 *
 * **Topology is mesh data.** The cage is a section flagged `ClothSimulated` and
 * each particle is one of its vertices; the cage's own skin is the particles'
 * anchors, and `geom::names::kClothMovable` says which may move. A bound section is flagged
 * `ClothInfluenced` and lives in the cage's mesh.
 */
struct Cloth {
    u32 id = 0;
    SectionRef cage;
    std::vector<ClothBinding> bindings;
    std::vector<u32> colliders; ///< `ClothCollider::id`s.
    /// The rest value of `Channel::ClothActive`. It gates the write-back, not
    /// the step.
    bool active = true;
    // Common to both solvers.
    f32 density = 0.0f;
    f32 damping = 0.0f;
    f32 friction = 0.0f;
    f32 stretchStiffness = 0.0f;
    f32 bendStiffness = 0.0f;
    f32 gravityScale = 1.0f; ///< On the host's gravity.
    Vector3f wind{0, 0, 0};  ///< Model space.
    std::optional<Sc2ClothParams> sc2;

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("cage", cage);
        v.field("bindings", bindings);
        v.field("colliders", colliders);
        v.field("active", active);
        v.field("density", density);
        v.field("damping", damping);
        v.field("friction", friction);
        v.field("stretchStiffness", stretchStiffness);
        v.field("bendStiffness", bendStiffness);
        v.field("gravityScale", gravityScale);
        v.field("wind", wind);
        v.optional("sc2", sc2);
    }
};

// ============================================================================
// Rigs
// ============================================================================

/// When a rig starts simulating.
enum class RigStart : u8 { Animated, OnDeath, Always, Never, Count };

/// A named subset of bodies a game switches on at once (World of Warcraft,
/// Diablo III). StarCraft II has none: its import makes none and its export
/// ignores them.
struct PhysicsRig {
    u32 id = 0;
    std::string name;
    RigStart start = RigStart::Animated;
    std::vector<u32> bodies; ///< Body ids.

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
        v.field("name", name);
        v.field("start", start);
        v.field("bodies", bodies);
    }
};

// ============================================================================
// The set
// ============================================================================

struct PhysicsSet {
    std::vector<PhysicsBody> bodies;
    std::vector<PhysicsJoint> joints;
    std::vector<ClothCollider> colliders;
    std::vector<Cloth> cloths;
    std::vector<PhysicsRig> rigs;
    /// The next id to hand out. Ids are never reused, so a channel naming a
    /// removed record never names a new one.
    u32 nextId = 1;

    bool empty() const {
        return bodies.empty() && joints.empty() && colliders.empty() && cloths.empty() && rigs.empty();
    }

    /// A fresh id, never handed out before.
    u32 allocateId() {
        return nextId++;
    }

    const PhysicsBody* body(u32 id) const;
    PhysicsBody* body(u32 id);
    const PhysicsJoint* joint(u32 id) const;
    const ClothCollider* collider(u32 id) const;
    const Cloth* cloth(u32 id) const;
    Cloth* cloth(u32 id);
    const PhysicsRig* rig(u32 id) const;

    /// Whether any record has id @p id.
    bool hasId(u32 id) const;

    /// The first body on @p node in list order: the one a StarCraft II joint
    /// binds. Null when the node carries none.
    const PhysicsBody* firstBodyOn(u32 node) const;

    template <class V>
    void reflect(V& v) {
        v.field("bodies", bodies);
        v.field("joints", joints);
        v.field("colliders", colliders);
        v.field("cloths", cloths);
        v.field("rigs", rigs);
        v.field("nextId", nextId);
    }
};

// ============================================================================
// Host policy
// ============================================================================

/// What a game's host decides, never a document: gravity, the step, the
/// solver iterations and the kinematic drive caps (WEM_PHYSICS_DESIGN.md §3.9).
struct PhysicsHost {
    Vector3f gravity{0, 0, 0};
    f32 step = 1.0f / 60.0f;
    u8 maxSubsteps = 1;
    u8 velocityIterations = 8;
    u8 positionIterations = 2;
    f32 snapLinear = 0.0f;
    f32 snapAngular = 0.0f;
    f32 releaseLinear = 0.0f;
    f32 releaseAngular = 0.0f;
};

/// The host policy of @p game's physics.
PhysicsHost PhysicsHostFor(Game game);

} // namespace wem
} // namespace models
} // namespace whiteout

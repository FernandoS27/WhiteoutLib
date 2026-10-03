// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_physics.h"

#include <whiteout/models/m3/physics_cook.h>
#include <whiteout/models/m3/physics_upgrade.h>
#include <whiteout/models/wem/nodes/emitters.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_physics {

namespace {

constexpr u16 kNoBone = 0xFFFFu;
constexpr u16 kNoLane = 0xFFFFu;

// PHRB flag bits (m3::RigidBodyFlag).
constexpr u32 kCollidable = 0x1u;
constexpr u32 kWalkable = 0x2u;
constexpr u32 kStackable = 0x4u;
constexpr u32 kSimulateCollision = 0x8u;
constexpr u32 kInheritDynamic = 0x40u;
constexpr u32 kKeepBoneDriven = 0x80u;
constexpr u32 kExemptFromRagdoll = 0x100u;
constexpr u32 kUntraced = 0x10u | 0x20u | 0x200u;

Vector3f Rebase(const Vector3f& v) {
    return Vector3f{-v.y, v.x, v.z};
}

Vector3f Unrebase(const Vector3f& v) {
    return Vector3f{v.y, -v.x, v.z};
}

bool IsIdentity(const Matrix44f& m) {
    const Matrix44f identity = Matrix44f::identity();
    return std::memcmp(&m, &identity, sizeof(Matrix44f)) == 0;
}

PhysicsShapeKind KindOf(m3::PhysicsShapeType type) {
    switch (type) {
    case m3::PhysicsShapeType::Box:
        return PhysicsShapeKind::Box;
    case m3::PhysicsShapeType::Sphere:
        return PhysicsShapeKind::Sphere;
    case m3::PhysicsShapeType::Capsule:
        return PhysicsShapeKind::Capsule;
    case m3::PhysicsShapeType::Cylinder:
        return PhysicsShapeKind::Cylinder;
    case m3::PhysicsShapeType::ConvexHull:
        return PhysicsShapeKind::ConvexHull;
    case m3::PhysicsShapeType::Mesh:
        return PhysicsShapeKind::TriangleMesh;
    }
    return PhysicsShapeKind::Box;
}

m3::PhysicsShapeType TypeOf(PhysicsShapeKind kind) {
    switch (kind) {
    case PhysicsShapeKind::Sphere:
        return m3::PhysicsShapeType::Sphere;
    case PhysicsShapeKind::Capsule:
        return m3::PhysicsShapeType::Capsule;
    case PhysicsShapeKind::Cylinder:
        return m3::PhysicsShapeType::Cylinder;
    case PhysicsShapeKind::ConvexHull:
        return m3::PhysicsShapeType::ConvexHull;
    case PhysicsShapeKind::TriangleMesh:
        return m3::PhysicsShapeType::Mesh;
    case PhysicsShapeKind::Box:
    case PhysicsShapeKind::Count:
        break;
    }
    return m3::PhysicsShapeType::Box;
}

PhysicsShape ImportShape(const m3::PhysicsShape& source, const PhysicsMaterial& material) {
    PhysicsShape shape;
    shape.kind = KindOf(source.shapeType);
    shape.transform = RebaseFrame(source.transform);
    shape.material = material;
    const Vector3f& d = source.shapeDimensions;
    switch (shape.kind) {
    case PhysicsShapeKind::Box:
        shape.halfExtents = d;
        break;
    case PhysicsShapeKind::Sphere:
        shape.radius = d.x;
        break;
    case PhysicsShapeKind::Capsule:
    case PhysicsShapeKind::Cylinder:
        shape.radius = d.x;
        shape.length = d.y;
        break;
    case PhysicsShapeKind::ConvexHull: {
        const std::size_t n = std::min<std::size_t>(source.hullVertexCount, source.hullVertices.size());
        shape.points.assign(source.hullVertices.begin(),
                            source.hullVertices.begin() + static_cast<std::ptrdiff_t>(n));
        break;
    }
    case PhysicsShapeKind::TriangleMesh: {
        // Stored about the tree centre, which the client adds back.
        const std::size_t n =
            std::min<std::size_t>(source.meshVertexCount, source.meshVertexPositions.size());
        const Vector3f& c = source.meshBoundsCenter;
        shape.vertices.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const Vector4f& v = source.meshVertexPositions[i];
            shape.vertices.push_back({v.x + c.x, v.y + c.y, v.z + c.z});
        }
        // The client takes MT16 when there is one, and its count field -- which
        // on 12 shipped meshes is three short of the array.
        if (!source.meshFaceIndices16.empty()) {
            const std::size_t count =
                std::min<std::size_t>(source.meshFaceIndex16Count, source.meshFaceIndices16.size());
            for (std::size_t t = 0; t < count; ++t) {
                for (std::size_t k = 0; k < 3; ++k) {
                    shape.triangles.push_back(source.meshFaceIndices16[t][k]);
                }
            }
        } else {
            const std::size_t count =
                std::min<std::size_t>(source.meshFaceIndex32Count, source.meshFaceIndices32.size());
            for (std::size_t t = 0; t < count; ++t) {
                for (std::size_t k = 0; k < 3; ++k) {
                    shape.triangles.push_back(source.meshFaceIndices32[t][k]);
                }
            }
        }
        break;
    }
    case PhysicsShapeKind::Count:
        break;
    }
    return shape;
}

/// A `PHCC` frame is the bind pose's, model space: the client carries it onto
/// its bone through IREF (`transform * IREF`). WEM's is the node's own.
ClothCollider ImportCollider(const m3::ClothCollider& source, const m3::Model& model) {
    ClothCollider collider;
    collider.node = source.bone < model.bones.size() ? source.bone : kInvalidNode;
    collider.kind = ClothColliderKind::Capsule;
    collider.transform = RebaseFrame(collider.node != kInvalidNode && source.bone < model.initialReference.size()
                                         ? source.transform * model.initialReference[source.bone].matrix
                                         : source.transform);
    collider.radius = source.radius;
    collider.length = source.height;
    return collider;
}

/// Four bytes, lane k in bits 8k.
std::array<u8, 4> Bytes(u32 packed) {
    return {static_cast<u8>(packed & 0xFFu), static_cast<u8>((packed >> 8) & 0xFFu),
            static_cast<u8>((packed >> 16) & 0xFFu), static_cast<u8>(packed >> 24)};
}

u32 PackBytes(const std::array<u8, 4>& bytes) {
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
           (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
}

/// A weight back to its byte: the import divided by 255, so this is exact.
u8 WeightByte(f32 weight) {
    return static_cast<u8>(std::clamp(std::lround(static_cast<double>(weight) * 255.0), 0L, 255L));
}

} // namespace

// ============================================================================
// The basis
// ============================================================================

// R = {{0,1,0,0},{-1,0,0,0},{0,0,1,0},{0,0,0,1}} (`v * R == Rebase(v)`), so
// `m * R` moves column 1 into column 0 negated and column 0 into column 1, and
// `R^T * m` does the same to the rows. Negation and moves are exact.

Matrix44f RebaseFrame(const Matrix44f& m) {
    Matrix44f out = m;
    for (int r = 0; r < 4; ++r) {
        out.data[r][0] = -m.data[r][1];
        out.data[r][1] = m.data[r][0];
    }
    return out;
}

Matrix44f UnrebaseFrame(const Matrix44f& m) {
    Matrix44f out = m;
    for (int r = 0; r < 4; ++r) {
        out.data[r][0] = m.data[r][1];
        out.data[r][1] = -m.data[r][0];
    }
    return out;
}

Matrix44f RebaseConjugate(const Matrix44f& m) {
    const Matrix44f cols = RebaseFrame(m);
    Matrix44f out = cols;
    for (int c = 0; c < 4; ++c) {
        out.data[0][c] = -cols.data[1][c];
        out.data[1][c] = cols.data[0][c];
    }
    return out;
}

Matrix44f UnrebaseConjugate(const Matrix44f& m) {
    const Matrix44f cols = UnrebaseFrame(m);
    Matrix44f out = cols;
    for (int c = 0; c < 4; ++c) {
        out.data[0][c] = cols.data[1][c];
        out.data[1][c] = -cols.data[0][c];
    }
    return out;
}

// ============================================================================
// Force fields and vertex warps
// ============================================================================

ForceFieldPayload ImportForce(const m3::Force& record) {
    ForceFieldPayload payload;
    const u32 type = static_cast<u32>(record.forceType);
    const u32 shape = static_cast<u32>(record.forceShape);
    payload.kind = type < static_cast<u32>(ForceKind::Count) ? static_cast<ForceKind>(type)
                                                             : ForceKind::Directional;
    payload.volume = shape < static_cast<u32>(ForceVolume::Count) ? static_cast<ForceVolume>(shape)
                                                                   : ForceVolume::Sphere;
    payload.falloff = m3::hasFlag(record.flags, m3::ForceFlag::Falloff);
    payload.heightGradient = m3::hasFlag(record.flags, m3::ForceFlag::HeightGradient);
    payload.unbounded = m3::hasFlag(record.flags, m3::ForceFlag::Unbounded);
    payload.affectsParticles = m3::hasFlag(record.flags, m3::ForceFlag::AffectsParticles);
    payload.affectsBodies = m3::hasFlag(record.flags, m3::ForceFlag::AffectsBodies);
    payload.channels = record.localChannels;
    payload.scope = record.unknown;
    payload.strength = record.strength.initValue;
    payload.width = record.width.initValue;
    payload.height = record.height.initValue;
    payload.length = record.length.initValue;
    return payload;
}

VertexWarpPayload ImportWarp(const m3::Warp& record) {
    VertexWarpPayload payload;
    payload.type = record.warpType;
    payload.reserved = record.unknown;
    payload.radius = record.radius.initValue;
    payload.height = record.height.initValue;
    payload.strength = record.strength.initValue;
    payload.angular = record.angular.initValue;
    payload.axial = record.axial.initValue;
    payload.radial = record.radial.initValue;
    return payload;
}

namespace {

m3::AnimRef<f32> Rest(f32 value) {
    m3::AnimRef<f32> ref;
    ref.initValue = value;
    ref.nullValue = 0.0f;
    ref.animId = 0;
    ref.flags = 0;
    ref.interpType = 0;
    ref.unused = -1;
    return ref;
}

} // namespace

m3::Force ExportForce(const ForceFieldPayload& payload) {
    m3::Force record;
    record.forceType = static_cast<m3::ForceType>(static_cast<u32>(payload.kind));
    record.forceShape = static_cast<m3::ForceShape>(static_cast<u32>(payload.volume));
    record.unknown = payload.scope;
    u32 flags = 0;
    flags |= payload.falloff ? static_cast<u32>(m3::ForceFlag::Falloff) : 0u;
    flags |= payload.heightGradient ? static_cast<u32>(m3::ForceFlag::HeightGradient) : 0u;
    flags |= payload.unbounded ? static_cast<u32>(m3::ForceFlag::Unbounded) : 0u;
    flags |= payload.affectsParticles ? static_cast<u32>(m3::ForceFlag::AffectsParticles) : 0u;
    flags |= payload.affectsBodies ? static_cast<u32>(m3::ForceFlag::AffectsBodies) : 0u;
    record.flags = static_cast<m3::ForceFlag>(flags);
    record.localChannels = payload.channels;
    record.strength = Rest(payload.strength);
    record.width = Rest(payload.width);
    record.height = Rest(payload.height);
    record.length = Rest(payload.length);
    return record;
}

m3::Warp ExportWarp(const VertexWarpPayload& payload) {
    m3::Warp record;
    record.warpType = payload.type;
    record.unknown = payload.reserved;
    record.radius = Rest(payload.radius);
    record.height = Rest(payload.height);
    record.strength = Rest(payload.strength);
    record.angular = Rest(payload.angular);
    record.axial = Rest(payload.axial);
    record.radial = Rest(payload.radial);
    return record;
}

// ============================================================================
// Import
// ============================================================================

ClothVertices PlanClothVertices(const m3::Model& source, const m3::MeshDivision& division,
                                u32 lowest, u32 vertexCount, Diagnostics& out) {
    ClothVertices plan;
    plan.anchorOf.assign(vertexCount, -1);
    plan.movable.assign(vertexCount, 0xFFu);
    std::array<u32, 4> noLanes;
    noLanes.fill(geom::kInvalidId);
    plan.bindVertex.assign(vertexCount, noLanes);
    plan.bindWeight.assign(vertexCount, std::array<f32, 4>{0, 0, 0, 0});
    const std::size_t regions = division.regions.size();

    for (std::size_t c = 0; c < source.clothPhysics.size(); ++c) {
        const m3::ClothPhysics& cloth = source.clothPhysics[c];
        if (cloth.simEnabled.empty()) {
            continue; // colliders only
        }
        if (cloth.cageRegion >= regions) {
            out.warn(DiagCode::ClothTopologyInvalid,
                     "PHCL " + std::to_string(c) + " names region " +
                         std::to_string(cloth.cageRegion) + ", past the division",
                     ElementRef(ElementKind::Mesh, 0));
            continue;
        }
        const m3::Region& cage = division.regions[cloth.cageRegion];
        const u32 count = cage.vertexCount;
        if (cloth.simEnabled.size() != count || cloth.vertexBones.size() != count ||
            cloth.vertexWeights.size() != count || cage.firstVertex < lowest) {
            out.warn(DiagCode::ClothTopologyInvalid,
                     "PHCL " + std::to_string(c) + "'s particles disagree with its cage region",
                     ElementRef(ElementKind::Section, cloth.cageRegion));
            continue;
        }
        plan.anyCage = true;
        const u32 cageBase = cage.firstVertex - lowest;
        for (u32 i = 0; i < count; ++i) {
            const u32 vertex = cageBase + i;
            if (vertex >= vertexCount) {
                break;
            }
            plan.movable[vertex] = (cloth.simEnabled[i] & 1u) != 0 ? 1u : 0u;
            ClothVertices::Anchors anchors;
            const std::array<u8, 4> bones = Bytes(cloth.vertexBones[i]);
            const std::array<u8, 4> weights = Bytes(cloth.vertexWeights[i]);
            for (std::size_t k = 0; k < 4; ++k) {
                if (weights[k] == 0) {
                    continue;
                }
                anchors.bones[anchors.count] = bones[k];
                anchors.weights[anchors.count] = static_cast<f32>(weights[k]) / 255.0f;
                ++anchors.count;
            }
            plan.anchorOf[vertex] = static_cast<i32>(plan.anchors.size());
            plan.anchors.push_back(anchors);
        }
        for (const m3::ClothProxy& proxy : cloth.proxies) {
            if (proxy.proxyIndex >= regions) {
                continue;
            }
            const m3::Region& bound = division.regions[proxy.proxyIndex];
            if (proxy.proxyVertices.size() != bound.vertexCount ||
                proxy.proxyWeights.size() != bound.vertexCount || bound.firstVertex < lowest) {
                out.warn(DiagCode::ClothTopologyInvalid,
                         "PHAC of PHCL " + std::to_string(c) + " disagrees with region " +
                             std::to_string(proxy.proxyIndex),
                         ElementRef(ElementKind::Section, proxy.proxyIndex));
                continue;
            }
            plan.anyBinding = true;
            const u32 boundBase = bound.firstVertex - lowest;
            for (u32 j = 0; j < bound.vertexCount; ++j) {
                const u32 vertex = boundBase + j;
                if (vertex >= vertexCount) {
                    break;
                }
                const u64 lanes = proxy.proxyVertices[j];
                const std::array<u8, 4> weights = Bytes(proxy.proxyWeights[j]);
                for (u32 k = 0; k < 4; ++k) {
                    const u16 lane = static_cast<u16>((lanes >> (16 * k)) & 0xFFFFu);
                    const bool live = lane != kNoLane && lane < count;
                    plan.bindVertex[vertex][k] = live ? cageBase + lane : geom::kInvalidId;
                    plan.bindWeight[vertex][k] = static_cast<f32>(weights[k]) / 255.0f;
                }
            }
        }
    }
    return plan;
}

ImportedIds Import(const m3::Model& source, Model& model, Diagnostics& out) {
    ImportedIds ids;
    PhysicsSet& physics = model.physics;
    const u32 boneCount = static_cast<u32>(source.bones.size());

    for (std::size_t b = 0; b < source.rigidBodies.size(); ++b) {
        const m3::RigidBody& record = source.rigidBodies[b];
        PhysicsBody body;
        body.id = physics.allocateId();
        body.node = record.parentBoneIndex < boneCount ? record.parentBoneIndex : kInvalidNode;
        if (body.node == kInvalidNode) {
            out.warn(DiagCode::PhysicsReferenceInvalid,
                     "PHRB " + std::to_string(b) + " names bone " +
                         std::to_string(record.parentBoneIndex),
                     ElementRef(ElementKind::PhysicsRecord, body.id));
        }
        switch (record.simulationType) {
        case 0:
            body.motion = BodyMotion::Dynamic;
            break;
        case 2:
            body.motion = BodyMotion::Static;
            break;
        default:
            body.motion = BodyMotion::Kinematic;
            break;
        }
        body.simulates = record.dynamicState.initValue != 0;
        PhysicsMaterial material;
        material.density = record.density;
        material.friction = record.friction;
        material.restitution = record.restitution;
        for (const m3::PhysicsShape& shape : record.rigidBodyShape) {
            body.shapes.push_back(ImportShape(shape, material));
        }
        body.linearDamping = record.linearDamping;
        body.angularDamping = record.angularDamping;
        body.inertiaScale = record.inertiaScale;
        body.gravityScale = 1.0f;
        const u32 flags = static_cast<u32>(record.flags);
        body.inheritDynamic = (flags & kInheritDynamic) != 0;
        body.exemptFromRagdoll = (flags & kExemptFromRagdoll) != 0;
        body.forceChannels =
            static_cast<u32>(record.localForces) | (static_cast<u32>(record.worldForces) << 16);
        Sc2BodyExtension sc2;
        sc2.physicsMaterial = record.physicsType;
        sc2.collidable = (flags & kCollidable) != 0;
        sc2.walkable = (flags & kWalkable) != 0;
        sc2.stackable = (flags & kStackable) != 0;
        sc2.simulateCollision = (flags & kSimulateCollision) != 0;
        sc2.keepsBoneDriven = (flags & kKeepBoneDriven) != 0;
        sc2.untracedFlags = flags & kUntraced;
        body.sc2 = sc2;
        ids.bodies.push_back(body.id);
        physics.bodies.push_back(std::move(body));
    }

    for (std::size_t j = 0; j < source.physicsJoints.size(); ++j) {
        const m3::PhysicsJoint& record = source.physicsJoints[j];
        const PhysicsBody* a = physics.firstBodyOn(record.boneIndex1);
        const PhysicsBody* b = physics.firstBodyOn(record.boneIndex2);
        if (a == nullptr || b == nullptr || record.jointType > 3) {
            out.warn(DiagCode::PhysicsReferenceInvalid,
                     "PHYJ " + std::to_string(j) + " joins bones " + std::to_string(record.boneIndex1) +
                         " and " + std::to_string(record.boneIndex2) +
                         (record.jointType > 3 ? " with an unknown type" : ", which carry no body"),
                     ElementRef(ElementKind::Chunk, static_cast<u32>(j)));
            continue;
        }
        PhysicsJoint joint;
        joint.id = physics.allocateId();
        joint.bodyA = a->id;
        joint.bodyB = b->id;
        joint.kind = static_cast<JointKind>(record.jointType);
        joint.frameA = RebaseFrame(record.matrixBody1);
        joint.frameB = RebaseFrame(record.matrixBody2);
        joint.collideConnected = record.enableShape != 0;
        joint.limitEnabled = (record.enableLimits & 0xFFu) != 0;
        joint.lower = record.limitMin;
        joint.upper = record.limitMax;
        joint.cone = record.coneAngle;
        joint.friction = (record.enableFriction & 0xFFu) != 0 ? JointFriction::GravityHold
                                                               : JointFriction::None;
        joint.frictionAmount = record.friction;
        joint.angularSpring.hz = record.angularFrequency;
        joint.angularSpring.damping = record.dampingRatio;
        physics.joints.push_back(joint);
    }

    const bool division = !source.divisions.empty();
    const std::size_t regions = division ? source.divisions[0].regions.size() : 0;
    for (std::size_t c = 0; c < source.clothPhysics.size(); ++c) {
        const m3::ClothPhysics& record = source.clothPhysics[c];
        std::vector<u32> colliderIds;
        for (const m3::ClothCollider& source_collider : record.colliders) {
            ClothCollider collider = ImportCollider(source_collider, source);
            collider.id = physics.allocateId();
            colliderIds.push_back(collider.id);
            physics.colliders.push_back(collider);
        }
        if (record.simEnabled.empty()) {
            ids.cloths.push_back(0); // a collider source: the colliders are the record
            continue;
        }
        if (record.cageRegion >= regions) {
            ids.cloths.push_back(0);
            continue;
        }
        Cloth cloth;
        cloth.id = physics.allocateId();
        cloth.cage = SectionRef{0, record.cageRegion};
        for (const m3::ClothProxy& proxy : record.proxies) {
            if (proxy.proxyIndex < regions) {
                cloth.bindings.push_back(ClothBinding{SectionRef{0, proxy.proxyIndex}});
            }
        }
        cloth.colliders = colliderIds;
        cloth.active = record.active.initValue != 0;
        cloth.density = record.density;
        cloth.damping = record.damping;
        cloth.friction = record.friction;
        cloth.stretchStiffness = record.stretchStiffness;
        cloth.bendStiffness = record.bendingStiffness;
        cloth.gravityScale = record.gravity;
        cloth.wind = Rebase(record.localWind);
        Sc2ClothParams params;
        params.tracking = record.tracking;
        params.horizontalStiffness = record.horizontalStiffness;
        params.shearStiffness = record.shearStiffness;
        params.explosionScale = record.explosionScale;
        params.windScale = record.windScale;
        params.dragFactor = record.dragFactor;
        params.liftFactor = record.liftFactor;
        params.sphereStiffness = record.sphereStiffness;
        params.flatten = record.flatten != 0;
        params.useSkinCollision = record.useSkinCollision != 0;
        params.skinOffset = record.skinOffset;
        params.skinExponent = record.skinExponent;
        params.skinStiffness = record.skinStiffness;
        cloth.sc2 = params;
        ids.cloths.push_back(cloth.id);
        physics.cloths.push_back(std::move(cloth));
    }
    return ids;
}

// ============================================================================
// Export
// ============================================================================

ClothEmission PlanClothEmission(const Model& model) {
    ClothEmission emission;
    for (const Cloth& cloth : model.physics.cloths) {
        if (!cloth.cage.valid() || cloth.cage.mesh >= model.meshes.size() ||
            cloth.cage.section >= model.meshes[cloth.cage.mesh].sections.size()) {
            continue;
        }
        emission.cageOf.emplace(std::make_pair(cloth.cage.mesh, cloth.cage.section), cloth.id);
        for (const ClothBinding& binding : cloth.bindings) {
            if (binding.section.valid() && binding.section.mesh == cloth.cage.mesh &&
                binding.section.section < model.meshes[binding.section.mesh].sections.size()) {
                emission.boundBy[{binding.section.mesh, binding.section.section}].push_back(cloth.id);
            }
        }
    }
    return emission;
}

namespace {

/// The PHRB flags a body states.
u32 FlagsOf(const PhysicsBody& body) {
    u32 flags = 0;
    if (body.sc2.has_value()) {
        const Sc2BodyExtension& sc2 = *body.sc2;
        flags |= sc2.collidable ? kCollidable : 0u;
        flags |= sc2.walkable ? kWalkable : 0u;
        flags |= sc2.stackable ? kStackable : 0u;
        flags |= sc2.simulateCollision ? kSimulateCollision : 0u;
        flags |= sc2.keepsBoneDriven ? kKeepBoneDriven : 0u;
        flags |= sc2.untracedFlags & kUntraced;
    } else {
        // The retail majority: collidable, with the untraced 0x20.
        flags = kCollidable | 0x20u;
    }
    flags |= body.inheritDynamic ? kInheritDynamic : 0u;
    flags |= body.exemptFromRagdoll ? kExemptFromRagdoll : 0u;
    return flags;
}

bool SameMaterial(const PhysicsMaterial& a, const PhysicsMaterial& b) {
    return a.density == b.density && a.friction == b.friction && a.restitution == b.restitution;
}

m3::PhysicsShape ExportShape(const PhysicsShape& shape, ProfileId profile, u32 bodyId,
                             Diagnostics& diagnostics) {
    m3::PhysicsShape out;
    out.shapeType = TypeOf(shape.kind);
    out.transform = UnrebaseFrame(shape.transform);
    switch (shape.kind) {
    case PhysicsShapeKind::Box:
        out.shapeDimensions = shape.halfExtents;
        break;
    case PhysicsShapeKind::Sphere:
        out.shapeDimensions = {shape.radius, 0.0f, 0.0f};
        break;
    case PhysicsShapeKind::Capsule:
        if (shape.points.size() == 2) {
            // Stated by its ends: StarCraft II's centred frame through them.
            const auto [a, b] = CapsuleEnds(shape);
            f32 length = 0.0f;
            out.transform = UnrebaseFrame(CapsuleFrame(a, b, length));
            out.shapeDimensions = {shape.radius, length, 0.0f};
            break;
        }
        out.shapeDimensions = {shape.radius, shape.length, 0.0f};
        break;
    case PhysicsShapeKind::Cylinder:
        out.shapeDimensions = {shape.radius, shape.length, 0.0f};
        break;
    case PhysicsShapeKind::ConvexHull:
    case PhysicsShapeKind::TriangleMesh: {
        // The client reads neither kind's matrix: the matrix goes into the
        // points and the record states identity. An identity matrix skips the
        // bake, so a shipped hull's points stay bit-exact.
        const bool bake = !IsIdentity(out.transform);
        const std::vector<Vector3f>& input =
            shape.kind == PhysicsShapeKind::ConvexHull ? shape.points : shape.vertices;
        std::vector<Vector3f> points;
        points.reserve(input.size());
        for (const Vector3f& p : input) {
            points.push_back(bake ? m3::TransformPointRowMajor(out.transform, p) : p);
        }
        out.transform = Matrix44f::identity();
        if (shape.kind == PhysicsShapeKind::ConvexHull) {
            const m3::HullCookReport report = m3::CookHull(out, points);
            if (report.simplified) {
                diagnostics.warn(DiagCode::PhysicsHullSimplified,
                                 "a hull of " + std::to_string(points.size()) +
                                     " points merged faces to fit the cooked tables",
                                 ElementRef(ElementKind::PhysicsRecord, bodyId), profile);
            }
            if (!report.ok) {
                diagnostics.warn(DiagCode::PhysicsShapeDegenerate,
                                 "a hull of " + std::to_string(points.size()) +
                                     " points has no volume; written empty",
                                 ElementRef(ElementKind::PhysicsRecord, bodyId), profile);
            }
        } else {
            m3::CookMesh(out, points, shape.triangles);
            if (shape.triangles.size() < 3) {
                diagnostics.warn(DiagCode::PhysicsShapeDegenerate, "a triangle mesh with no triangle",
                                 ElementRef(ElementKind::PhysicsRecord, bodyId), profile);
            }
        }
        break;
    }
    case PhysicsShapeKind::Count:
        break;
    }
    out.forceVersion(m3::kCurrentPhysicsShapeVersion);
    return out;
}

/// The switch's AnimRef at rest: never sampled, which is what the client's
/// own upgrade writes. A channel on it binds it later (`m3_anim::Export`).
m3::AnimRef<u32> SwitchAt(bool on) {
    return m3::UnsampledSwitch(on ? 1u : 0u);
}

} // namespace

ExportRecords Export(const Model& model, ProfileId profile, const ClothEmission& emission,
                     const std::function<u32(u32)>& boneOf, m3::Model& out,
                     Diagnostics& diagnostics) {
    ExportRecords records;
    const PhysicsSet& physics = model.physics;
    const PhysicsCaps& caps = Profile(profile).physics;
    if (physics.empty()) {
        return records;
    }
    if (!caps.any()) {
        diagnostics.warn(DiagCode::PhysicsUnsupported,
                         "the profile carries no physics; the records are not written",
                         ElementRef(ElementKind::Document, 0), profile);
        return records;
    }

    // --- bodies --------------------------------------------------------------
    for (const PhysicsBody& body : physics.bodies) {
        const u32 bone = body.node == kInvalidNode ? kNoBone : boneOf(body.node);
        if (bone == kNoBone || bone > 0xFFFFu) {
            diagnostics.warn(DiagCode::PhysicsReferenceInvalid,
                             "a body rides no bone; not written",
                             ElementRef(ElementKind::PhysicsRecord, body.id), profile);
            continue;
        }
        m3::RigidBody record;
        switch (body.motion) {
        case BodyMotion::Dynamic:
            record.simulationType = 0;
            break;
        case BodyMotion::Static:
            record.simulationType = 2;
            break;
        default:
            record.simulationType = 1;
            break;
        }
        record.parentBoneIndex = static_cast<u16>(bone);
        record.physicsType = body.sc2.has_value() ? body.sc2->physicsMaterial : 0u;
        PhysicsMaterial material;
        if (!body.shapes.empty()) {
            material = body.shapes.front().material;
            for (const PhysicsShape& shape : body.shapes) {
                if (!SameMaterial(shape.material, material)) {
                    diagnostics.info(DiagCode::PhysicsMaterialMerged,
                                     "a body's shapes disagree on material; the first shape's is written",
                                     ElementRef(ElementKind::PhysicsRecord, body.id), profile);
                    break;
                }
            }
        }
        record.density = material.density;
        record.friction = material.friction;
        record.restitution = material.restitution;
        record.linearDamping = body.linearDamping;
        record.angularDamping = body.angularDamping;
        record.inertiaScale = body.inertiaScale;
        if (body.gravityScale != 1.0f) {
            diagnostics.info(DiagCode::PhysicsGravityScaleDropped,
                             "StarCraft II fixes a body's gravity scale at 1",
                             ElementRef(ElementKind::PhysicsRecord, body.id), profile);
        }
        record.dynamicState = SwitchAt(body.simulates);
        record.dynamicBlendOut = 1.0f; // never read; the retail value
        // Box and cylinder first: the client finds their baked polytopes at
        // 64 * shapeIndex in a table it allocates for those two kinds only.
        std::vector<const PhysicsShape*> ordered;
        for (const PhysicsShape& shape : body.shapes) {
            if (shape.kind == PhysicsShapeKind::Box || shape.kind == PhysicsShapeKind::Cylinder) {
                ordered.push_back(&shape);
            }
        }
        for (const PhysicsShape& shape : body.shapes) {
            if (shape.kind != PhysicsShapeKind::Box && shape.kind != PhysicsShapeKind::Cylinder) {
                ordered.push_back(&shape);
            }
        }
        for (const PhysicsShape* shape : ordered) {
            if ((caps.shapeKinds & (1u << static_cast<u32>(shape->kind))) == 0) {
                diagnostics.warn(DiagCode::PhysicsUnsupported,
                                 std::string("a ") + ToString(shape->kind) + " shape; not written",
                                 ElementRef(ElementKind::PhysicsRecord, body.id), profile);
                continue;
            }
            record.rigidBodyShape.push_back(ExportShape(*shape, profile, body.id, diagnostics));
        }
        const u32 flags = FlagsOf(body);
        record.flags = static_cast<m3::RigidBodyFlag>(flags);
        record.localForces = static_cast<u16>(body.forceChannels & 0xFFFFu);
        record.worldForces = static_cast<u16>(body.forceChannels >> 16);
        record.priority = 0; // never read; the retail value
        record.forceVersion(m3::kCurrentRigidBodyVersion);
        records.bodyRecord.emplace(body.id, static_cast<u32>(out.rigidBodies.size()));
        out.rigidBodies.push_back(std::move(record));
    }

    // --- joints --------------------------------------------------------------
    for (const PhysicsJoint& joint : physics.joints) {
        const PhysicsBody* a = physics.body(joint.bodyA);
        const PhysicsBody* b = physics.body(joint.bodyB);
        if (a == nullptr || b == nullptr || records.bodyRecord.count(a->id) == 0 ||
            records.bodyRecord.count(b->id) == 0) {
            diagnostics.warn(DiagCode::PhysicsReferenceInvalid,
                             "a joint names a body that was not written; not written",
                             ElementRef(ElementKind::PhysicsRecord, joint.id), profile);
            continue;
        }
        if ((caps.jointKinds & (1u << static_cast<u32>(joint.kind))) == 0) {
            diagnostics.warn(DiagCode::PhysicsJointKindUnsupported,
                             std::string("a ") + ToString(joint.kind) + " joint; not written",
                             ElementRef(ElementKind::PhysicsRecord, joint.id), profile);
            continue;
        }
        // The client binds the first body on each bone.
        if (physics.firstBodyOn(a->node) != a || physics.firstBodyOn(b->node) != b) {
            diagnostics.warn(DiagCode::PhysicsJointBodyAmbiguous,
                             "a joint's body is not the first on its node, which StarCraft II binds",
                             ElementRef(ElementKind::PhysicsRecord, joint.id), profile);
        }
        // A motor is another game's: its spring is not StarCraft II's weld spring.
        const bool motor = joint.motor != JointMotorMode::Off;
        if (joint.linearSpring.hz != 0.0f || joint.restLength != 0.0f || joint.breakForce != 0.0f ||
            joint.breakTorque != 0.0f || joint.friction == JointFriction::Torque || motor) {
            diagnostics.info(DiagCode::PhysicsJointFieldDropped,
                             "a joint's springs, rest length, breaking, friction torque or motor",
                             ElementRef(ElementKind::PhysicsRecord, joint.id), profile);
        }
        m3::PhysicsJoint record;
        record.jointType = static_cast<u32>(joint.kind);
        record.boneIndex1 = out.rigidBodies[records.bodyRecord.at(a->id)].parentBoneIndex;
        record.boneIndex2 = out.rigidBodies[records.bodyRecord.at(b->id)].parentBoneIndex;
        record.matrixBody1 = UnrebaseFrame(joint.frameA);
        record.matrixBody2 = UnrebaseFrame(joint.frameB);
        record.enableLimits = joint.limitEnabled ? 1u : 0u;
        record.limitMin = joint.lower;
        record.limitMax = joint.upper;
        record.coneAngle = joint.cone;
        record.enableFriction = joint.friction == JointFriction::GravityHold ? 1u : 0u;
        record.friction = joint.friction == JointFriction::Torque ? 0.0f : joint.frictionAmount;
        record.dampingRatio = motor ? 0.0f : joint.angularSpring.damping;
        record.angularFrequency = motor ? 0.0f : joint.angularSpring.hz;
        record.breakThreshold = 1.0f; // never read; the retail value
        record.enableShape = joint.collideConnected ? 1u : 0u;
        record.forceVersion(0);
        out.physicsJoints.push_back(record);
    }

    // --- cloth ---------------------------------------------------------------
    // PHCC holds capsules only: a plane is left out.
    const auto writable = [&](u32 id) {
        const ClothCollider* collider = physics.collider(id);
        if (collider != nullptr && collider->kind != ClothColliderKind::Capsule) {
            diagnostics.warn(DiagCode::PhysicsUnsupported, "a plane cloth collider, which PHCC cannot hold; not written",
                             ElementRef(ElementKind::PhysicsRecord, id), profile);
            return false;
        }
        return collider != nullptr;
    };
    const auto colliderRecord = [&](const ClothCollider& collider, std::set<u32>& bones) {
        m3::ClothCollider record;
        // Back to the bind pose's model space (`ImportCollider`).
        const Matrix44f bind = collider.node < model.nodes.size()
                                   ? Matrix44f::inverse(model.nodes.inverseBindMatrix(collider.node))
                                   : Matrix44f::identity();
        record.transform = UnrebaseFrame(collider.transform * bind);
        record.radius = collider.radius;
        record.height = collider.length;
        const u32 bone = collider.node == kInvalidNode ? kNoBone : boneOf(collider.node);
        record.bone = bone > 0xFFFFu ? kNoBone : bone;
        if (record.bone != kNoBone) {
            bones.insert(record.bone);
        }
        return record;
    };
    std::set<u32> usedColliders;
    for (const Cloth& cloth : physics.cloths) {
        const auto cage = emission.cages.find({cloth.cage.mesh, cloth.cage.section});
        if (!caps.cloth || cage == emission.cages.end() || cage->second.region == kInvalidIndex) {
            diagnostics.warn(DiagCode::ClothTopologyInvalid,
                             "a cloth whose cage was not written; not written",
                             ElementRef(ElementKind::PhysicsRecord, cloth.id), profile);
            continue;
        }
        const CageRegion& written = cage->second;
        const std::size_t particles = written.vertices.size();
        // `FitPhysicsToProfile` decimated what it could; what is left out of
        // range is not written, the bound sections drawing skinned.
        if (particles < 3 || particles > caps.maxClothParticles) {
            diagnostics.warn(DiagCode::ClothParticleLimit,
                             "a cage of " + std::to_string(particles) + " particles, outside 3-" +
                                 std::to_string(caps.maxClothParticles) + "; not written",
                             ElementRef(ElementKind::PhysicsRecord, cloth.id), profile);
            continue;
        }
        m3::ClothPhysics record;
        record.cageRegion = written.region;
        std::set<u32> bones;
        const Mesh& mesh = model.meshes[cloth.cage.mesh];
        const auto movable = mesh.attributes.get<u8>(geom::names::kClothMovable, geom::Domain::Vertex);
        record.simEnabled.reserve(particles);
        for (std::size_t i = 0; i < particles; ++i) {
            const u32 vertex = written.vertices[i];
            // A cage with no layer is movable throughout.
            record.simEnabled.push_back(vertex < movable.size() ? (movable[vertex] != 0 ? 1u : 0u) : 1u);
            const std::array<u8, 4> anchorBones = Bytes(written.bones[i]);
            const std::array<u8, 4> anchorWeights = Bytes(written.weights[i]);
            for (std::size_t k = 0; k < 4; ++k) {
                if (anchorWeights[k] != 0) {
                    bones.insert(anchorBones[k]);
                }
            }
        }
        record.vertexBones = written.bones;
        record.vertexWeights = written.weights;
        for (const u32 id : cloth.colliders) {
            usedColliders.insert(id);
            if (writable(id)) {
                record.colliders.push_back(colliderRecord(*physics.collider(id), bones));
            }
        }
        // The cage-local number of each WEM vertex, for the PHAC lanes.
        std::map<u32, u16> cageLocal;
        for (std::size_t i = 0; i < particles; ++i) {
            cageLocal.emplace(written.vertices[i], static_cast<u16>(i));
        }
        const auto bindVertex =
            mesh.attributes.get<std::array<u32, 4>>(geom::names::kClothBindVertex, geom::Domain::Vertex);
        const auto bindWeight =
            mesh.attributes.get<std::array<f32, 4>>(geom::names::kClothBindWeight, geom::Domain::Vertex);
        for (const ClothBinding& binding : cloth.bindings) {
            const auto bound = emission.bound.find({binding.section.mesh, binding.section.section});
            if (bound == emission.bound.end() || bound->second.region == kInvalidIndex) {
                diagnostics.warn(DiagCode::ClothTopologyInvalid,
                                 "a cloth binding whose section was not written",
                                 ElementRef(ElementKind::PhysicsRecord, cloth.id), profile);
                continue;
            }
            m3::ClothProxy proxy;
            proxy.proxyIndex = bound->second.region;
            proxy.clothIndex = written.region;
            for (const u32 vertex : bound->second.vertices) {
                u64 lanes = 0;
                std::array<u8, 4> weights{0, 0, 0, 0};
                for (u32 k = 0; k < 4; ++k) {
                    u16 lane = kNoLane;
                    if (vertex < bindVertex.size()) {
                        const auto it = cageLocal.find(bindVertex[vertex][k]);
                        if (it != cageLocal.end()) {
                            lane = it->second;
                            weights[k] = vertex < bindWeight.size() ? WeightByte(bindWeight[vertex][k]) : 0;
                        }
                    }
                    lanes |= static_cast<u64>(lane) << (16 * k);
                }
                proxy.proxyVertices.push_back(lanes);
                proxy.proxyWeights.push_back(PackBytes(weights));
            }
            proxy.forceVersion(0);
            record.proxies.push_back(std::move(proxy));
        }
        record.skinBones.assign(bones.begin(), bones.end());
        record.skinBoneCount = static_cast<u32>(record.skinBones.size());
        record.density = cloth.density;
        record.stretchStiffness = cloth.stretchStiffness;
        record.bendingStiffness = cloth.bendStiffness;
        record.damping = cloth.damping;
        record.friction = cloth.friction;
        record.gravity = cloth.gravityScale;
        record.localWind = Unrebase(cloth.wind);
        const Sc2ClothParams params = cloth.sc2.value_or(Sc2ClothParams{});
        record.tracking = params.tracking;
        record.horizontalStiffness = params.horizontalStiffness;
        record.shearStiffness = params.shearStiffness;
        record.explosionScale = params.explosionScale;
        record.windScale = params.windScale;
        record.dragFactor = params.dragFactor;
        record.liftFactor = params.liftFactor;
        record.sphereStiffness = params.sphereStiffness;
        record.flatten = params.flatten ? 1u : 0u;
        record.useSkinCollision = params.useSkinCollision ? 1u : 0u;
        record.skinOffset = params.skinOffset;
        record.skinExponent = params.skinExponent;
        record.skinStiffness = params.skinStiffness;
        record.active = SwitchAt(cloth.active);
        record.localChannels = 1; // never read; every shipped cage states 1
        record.forceVersion(m3::kCurrentClothVersion);
        records.clothRecord.emplace(cloth.id, static_cast<u32>(out.clothPhysics.size()));
        out.clothPhysics.push_back(std::move(record));
        // The editor drapes one it made on its floor; StarCraft II's cloth is
        // "unaware of the ground" (EDIT_MODE_PHYSICS_CLOTH_DESIGN.md §9.3).
        if (cloth.recipe) {
            diagnostics.info(DiagCode::PhysicsUnsupported,
                             cloth.recipe->name + " meets the floor in the editor, not in StarCraft II",
                             ElementRef(ElementKind::PhysicsRecord, cloth.id), profile);
        }
    }

    // Colliders no cloth uses are ones this model offers another model's cloth:
    // one collider-only record, as retail ships them -- every parameter zero
    // and `active` resting on.
    m3::ClothPhysics offered;
    std::set<u32> offeredBones;
    for (const ClothCollider& collider : physics.colliders) {
        if (usedColliders.count(collider.id) == 0 && writable(collider.id)) {
            offered.colliders.push_back(colliderRecord(collider, offeredBones));
        }
    }
    if (!offered.colliders.empty()) {
        offered.shearStiffness = 0.0f;
        offered.active = SwitchAt(true);
        offered.localChannels = 0;
        offered.forceVersion(m3::kCurrentClothVersion);
        out.clothPhysics.push_back(std::move(offered));
    }
    return records;
}

} // namespace m3_physics
} // namespace wem
} // namespace models
} // namespace whiteout

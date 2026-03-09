// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file physics.h
 * @brief Physics structures — forces, warps, shapes, rigid bodies, joints, cloth
 *
 * Defines all M3 physics chunk types: Force (FOR_) and Warp (WRP_) for
 * particle/ribbon influences; PhysicsShape (PHSH) with box/sphere/capsule/
 * cylinder/convex-hull/mesh variants; RigidBody (PHRB) for Havok rigid body
 * configuration; PhysicsJoint (PHYJ) and PhysicsConstraint (PHCT) for articulated
 * connections; and ClothPhysics (PHCL) with ClothCollider (PHCC) and ClothProxy
 * (PHAC) for cloth simulation.
 *
 * @see M3_FILE_FORMAT_SPECIFICATION.md §13 Physics
 */

#include "base.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Physics
// ============================================================================

/**
 * @brief FOR_ — Force field (v0–v2, 104 bytes)
 *
 * Applies radial, wind, or explosion forces to particles and ribbons
 * within an influence volume shape (sphere, cylinder, box, hemisphere).
 */
struct Force {
    static constexpr u32 tag = TAG_FOR;   ///< FourCC tag
    static constexpr u32 max_version = 2; ///< Latest known FOR_ version
    ForceType forceType;                  ///< Force influence type (radial/wind/explosion)
    ForceShape forceShape;                ///< Influence volume shape
    u32 unknown;                          ///< Unknown field
    u32 boneIndex;                        ///< Index into BONE array
    ForceFlag flags = ForceFlag::None;    ///< Force flags (falloff, height gradient, unbounded)
    u32 localChannels;                    ///< Local channel bitmask
    AnimRef<f32> strength;                ///< Animated force strength
    AnimRef<f32> width;                   ///< Animated influence width
    AnimRef<f32> height;                  ///< Animated influence height
    AnimRef<f32> length;                  ///< Animated influence length
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief WRP_ — Warp field (v0–v1, 132 bytes)
 *
 * Warps particle/ribbon trajectories with animated radius, height,
 * and angular/axial/radial strength components.
 */
struct Warp {
    static constexpr u32 tag = TAG_WRP;   ///< FourCC tag
    static constexpr u32 max_version = 1; ///< Latest known WRP_ version
    u32 warpType;                         ///< Warp type
    u32 boneIndex;                        ///< Index into BONE array
    u32 unknown;                          ///< Unknown field
    AnimRef<f32> radius;                  ///< Animated warp radius
    AnimRef<f32> height;                  ///< Animated warp height
    AnimRef<f32> strength;                ///< Animated warp strength
    AnimRef<f32> angular;                 ///< Animated angular component
    AnimRef<f32> axial;                   ///< Animated axial component
    AnimRef<f32> radial;                  ///< Animated radial component
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMSE — Convex hull half-edge (v0, 4 bytes)
 *
 * Half-edge connectivity for PHSH convex hull shapes (shapeType = 4).
 * Entries are stored in consecutive twin pairs (forward 0x01 / reverse 0xFF).
 * The nextAroundVertex field chains half-edges into closed per-vertex rings.
 */
struct ConvexHullHalfEdge {
    static constexpr u32 tag = TAG_DMSE;  ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known DMSE version
    u8 type;                              ///< 0x01 = forward, 0xFF = reverse (twin)
    u8 faceIndex;                         ///< Face this half-edge borders
    u8 vertexIndex;                       ///< Target vertex of this half-edge
    u8 nextAroundVertex;                  ///< Next half-edge around the same vertex
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMMN — Physics mesh normal (v0–v1, 8–12 bytes)
 */
struct PhysicsMeshNormal {
    static constexpr u32 tag = TAG_DMMN;  ///< FourCC tag
    static constexpr u32 max_version = 1; ///< Latest known DMMN version
    Vector3f normal;                      ///< Face normal vector
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMMT — Physics mesh triangle (v0, 28 bytes)
 */
struct PhysicsMeshTriangle {
    static constexpr u32 tag = TAG_DMMT;  ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known DMMT version
    u32 vertexIndex0;                     ///< First vertex index
    u32 vertexIndex1;                     ///< Second vertex index
    u32 vertexIndex2;                     ///< Third vertex index
    u32 edgeIndex0;                       ///< First edge index
    u32 edgeIndex1;                       ///< Second edge index
    u32 edgeIndex2;                       ///< Third edge index
    u16 reserved;                         ///< Reserved
    u16 flags;                            ///< Triangle flags
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMME — Physics mesh edge (v0, 20 bytes)
 */
struct PhysicsMeshEdge {
    static constexpr u32 tag = TAG_DMME;  ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known DMME version
    u32 edgeType;                         ///< Edge type
    u32 vertexA;                          ///< First vertex index
    u32 vertexB;                          ///< Second vertex index
    u32 faceA;                            ///< First adjacent face
    u32 faceB;                            ///< Second adjacent face
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHSH — Physics shape (v0–v3, 132–300 bytes)
 *
 * The 300-byte v3 layout is a three-part union. Bytes 0–79 are the common
 * header. Bytes 80–103 hold shape dimensions for simple shapes (0–3) or
 * are zero for complex shapes. Bytes 80–183 form the convex hull section
 * (shapeType 4); bytes 184–299 form the mesh section (shapeType 5).
 */
struct PhysicsShape {
    static constexpr u32 tag = TAG_PHSH;  ///< FourCC tag
    static constexpr u32 max_version = 3; ///< Latest known PHSH version
    Matrix44f transform;                  ///< 4×4 shape transform matrix

    // v1: collisionMargin + shapeType at offsets 64-71
    // v2+: shapeType at offset 64
    f32 collisionMargin;        ///< Havok convex radius (v1 only, ≈ 0.019685)
    PhysicsShapeType shapeType; ///< Shape type (box/sphere/capsule/cylinder/hull/mesh)
    // Note: 3 bytes alignment padding follow shapeType in the binary layout
    Vector3f oldSizes;        ///< Legacy sizes (v1 only, zero for shapeType 4–5)
    Reference reserved0;      ///< Reserved reference
    Vector3f shapeDimensions; ///< Shape dimensions (v2+, zero for complex shapes)

    // --- Convex Hull section (binary offsets 80–183, shapeType = 4 only) ---
    std::vector<Vector3f> hullFaceNormals;         ///< Per-face unit normals (VEC3)
    std::vector<Vector4f> hullVertexPositions;     ///< Vertex positions, w=0 (VEC4)
    std::vector<ConvexHullHalfEdge> hullHalfEdges; ///< Half-edge table (DMSE)
    std::vector<u8> hullVertexFaceIndices;         ///< One face index per vertex (U8__)
    Vector3f hullCenter;                           ///< Hull centroid
    u32 hullFaceNormalCount;                       ///< Number of face normals
    u32 hullVertexCount;                           ///< Number of vertices
    u32 hullHalfEdgeCount;                         ///< Number of half-edges
    f32 hullUnknown0;                              ///< Unknown hull parameter 0
    f32 hullUnknown1;                              ///< Unknown hull parameter 1

    // --- Mesh section (binary offsets 184–299, shapeType = 5 only) ---
    std::vector<PhysicsMeshNormal> meshFaceNormals;    ///< Face normals (DMMN)
    std::vector<Vector4f> meshVertexPositions;         ///< Vertex positions, w=0 (VEC4)
    std::vector<std::array<u16, 7>> meshFaceIndices16; ///< 16-bit face data (MT16, or empty)
    std::vector<std::array<u32, 7>> meshFaceIndices32; ///< 32-bit face data (MT32, or empty)
    // MT32/MT16 per-entry layout: {v0, v1, v2, adj0, adj1, adj2, flags}
    Vector3f meshBoundsCenter; ///< Mesh bounding center
    Vector3f meshBoundsExtent; ///< Mesh bounding half-extents
    Vector3f meshTolerance;    ///< Mesh tolerance vector
    u32 meshNormalCount;       ///< Number of mesh normals
    u32 meshVertexCount;       ///< Number of mesh vertices
    u32 meshFaceIndex16Count;  ///< MT16 face count (0 when MT32)
    u32 meshFaceIndex32Count;  ///< MT32 face count (0 when MT16)
    u32 meshUnknown1;          ///< Unknown mesh parameter
    u32 meshReserved;          ///< Reserved (always 0)
    u32 meshTreeDepth;         ///< BVH tree depth (1–12)
    f32 meshCollisionMargin;   ///< Collision margin (MT16: small float; MT32: 0.0)

    /// Deprecated shape data from older versions
    struct {
        struct {
            std::vector<PhysicsMeshNormal> meshFaceNormals; ///< v2 only: face normals
            std::vector<Vector3f> meshVertexPositions;      ///< v2 only: vertex positions
            std::vector<PhysicsMeshTriangle> unknown;       ///< v2 only: triangles
            std::vector<PhysicsMeshEdge> unknown2;          ///< v2 only: edges
        } v2;
        struct {
            std::vector<Vector3f> legacyVertices; ///< v1 only: convex hull / mesh vertices
            std::vector<u8> unknown0;             ///< v1 only: convex hull unknown data
            std::vector<u16> faceIndices;         ///< v1 only: mesh face indices
            std::vector<Vector4f> planeEquations; ///< v1 only: convex hull plane equations
            Vector3f halfExtents;                 ///< v1 only: shape bounding half-extents
        } v1;
    } deprecated;
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHRB — Rigid body (v2–v4, 56–104 bytes)
 *
 * Havok rigid body with density, friction, restitution, damping,
 * gravity scale, and collision shape references.
 */
struct RigidBody {
    static constexpr u32 tag = TAG_PHRB;       ///< FourCC tag
    static constexpr u32 max_version = 4;      ///< Latest known PHRB version
    u16 simulationType;                        ///< Simulation mode (v3+)
    u16 parentBoneIndex;                       ///< Parent bone index
    u32 physicsType;                           ///< Engine-specific body type (v3+)
    f32 density;                               ///< Body density
    f32 friction;                              ///< Surface friction
    f32 restitution;                           ///< Elasticity / bounciness
    f32 linearDamping;                         ///< Linear velocity damping
    f32 angularDamping;                        ///< Angular velocity damping
    f32 gravityScale;                          ///< Gravity influence scale
    AnimRef<u32> dynamicState;                 ///< Animated dynamic state (v4+)
    f32 dynamicBlendOut;                       ///< Dynamic blend-out duration (v4+)
    std::vector<PhysicsShape> rigidBodyShape;  ///< Collision shapes (PHSH)
    RigidBodyFlag flags = RigidBodyFlag::None; ///< Rigid body flags
    u16 localForces;                           ///< Local force channel bitmask
    u16 worldForces;                           ///< World force channel bitmask
    u32 priority;                              ///< Simulation priority

    /// Deprecated rigid body data from v2
    struct {
        std::array<std::array<f32, 3>, 3> inertiaTensor =
            {};        ///< v2 only: 3×3 inertia tensor (36 bytes)
        u16 boneIndex; ///< v2 only: bone index (typically same as parentBoneIndex)
        std::array<u32, 4> reserved = {}; ///< v2 only: reserved (always 0)
    } deprecated;
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHYJ — Physics joint (v0, 180 bytes)
 *
 * Connects two rigid bodies with limit, friction, and break-threshold parameters.
 */
struct PhysicsJoint {
    static constexpr u32 tag = TAG_PHYJ;  ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known PHYJ version
    u32 jointType;                        ///< Joint type
    u32 boneIndex1;                       ///< First bone index
    u32 boneIndex2;                       ///< Second bone index
    Matrix44f matrixBody1;                ///< Transform for body 1
    Matrix44f matrixBody2;                ///< Transform for body 2
    u32 enableLimits;                     ///< Enable angular limits
    f32 limitMin;                         ///< Minimum limit angle
    f32 limitMax;                         ///< Maximum limit angle
    f32 coneAngle;                        ///< Cone constraint angle
    u32 enableFriction;                   ///< Enable joint friction
    f32 friction;                         ///< Friction coefficient
    f32 dampingRatio;                     ///< Damping ratio
    f32 angularFrequency;                 ///< Angular frequency
    f32 breakThreshold;                   ///< Force threshold to break joint
    u8 enableShape;                       ///< Enable shape constraint
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHCT — Physics constraint (v0, 24 bytes)
 *
 * Constrains two rigid bodies with break-force threshold.
 */
struct PhysicsConstraint {
    static constexpr u32 tag = TAG_PHCT;  ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known PHCT version
    std::vector<u16> dependents;          ///< Dependent bone indices (U16_)
    u16 rigidBody1;                       ///< First rigid body index
    u16 rigidBody2;                       ///< Second rigid body index
    Flag flags;                           ///< Constraint flags
    f32 breakForce;                       ///< Force required to break constraint
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHCC — Cloth collider (v0, 76 bytes)
 *
 * Capsule-shaped collider used by cloth simulation.
 */
struct ClothCollider {
    static constexpr u32 tag = TAG_PHCC;  ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known PHCC version
    Matrix44f transform;                  ///< 4×4 collider transform
    f32 radius;                           ///< Capsule radius
    f32 height;                           ///< Capsule height
    u32 padding;                          ///< Alignment padding
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHAC — Cloth proxy (v0, 32 bytes)
 *
 * Maps cloth vertices to proxy geometry for collision.
 */
struct ClothProxy {
    static constexpr u32 tag = TAG_PHAC;  ///< FourCC tag
    static constexpr u32 max_version = 0; ///< Latest known PHAC version
    u32 proxyIndex;                       ///< Proxy mesh index
    u32 clothIndex;                       ///< Cloth mesh index
    std::vector<u64> proxyVertices;       ///< Proxy vertex data (U64_)
    std::vector<u32> proxyWeights;        ///< Proxy blend weights (U32_)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHCL — Cloth physics (v0–v4, 192 bytes)
 *
 * Full cloth simulation configuration: skin bone binding, stiffness
 * parameters, damping, wind/explosion/gravity scales, colliders, and proxies.
 * Added in MODL v28.
 */
struct ClothPhysics {
    static constexpr u32 tag = TAG_PHCL;  ///< FourCC tag
    static constexpr u32 max_version = 4; ///< Latest known PHCL version
    u32 clothMeshCount;                   ///< Number of cloth mesh sections
    u32 skinBoneCount;                    ///< Number of skin bones
    std::vector<u16> skinBones;           ///< Skin bone indices (U16_)
    std::vector<u8> simEnabled;           ///< Per-vertex simulation enable flags (U8__)
    std::vector<u32> vertexBones;         ///< Per-vertex bone indices (U32_)
    std::vector<u32> vertexWeights;       ///< Per-vertex bone weights (U32_)
    std::vector<ClothCollider> colliders; ///< Cloth colliders (PHCC)
    std::vector<ClothProxy> proxies;      ///< Cloth proxies (PHAC)
    f32 density;                          ///< Cloth density
    f32 tracking;                         ///< Tracking factor
    f32 stretchStiffness;                 ///< Stretch stiffness
    f32 horizontalStiffness;              ///< Horizontal stiffness
    f32 bendingStiffness;                 ///< Bending stiffness
    f32 damping;                          ///< Damping coefficient
    f32 friction;                         ///< Friction coefficient
    f32 gravity;                          ///< Gravity influence
    f32 explosionScale;                   ///< Explosion force scale
    f32 windScale;                        ///< Wind force scale
    f32 shearStiffness;                   ///< Shear stiffness
    f32 dragFactor;                       ///< Drag factor
    f32 liftFactor;                       ///< Lift factor
    f32 sphereStiffness;                  ///< Sphere collider stiffness
    u32 flatten;                          ///< Flatten mode
    AnimRef<u32> active;                  ///< Animated active state
    u32 useSkinCollision;                 ///< Use skin mesh for collision
    f32 skinOffset;                       ///< Skin collision offset
    f32 skinExponent;                     ///< Skin collision exponent
    f32 skinStiffness;                    ///< Skin collision stiffness
    u32 localChannels;                    ///< Local force channel bitmask
    Vector3f localWind;                   ///< Local wind direction and magnitude
    M3_DEFINE_VERSION_ACCESSORS()
};

} // namespace m3
} // namespace whiteout

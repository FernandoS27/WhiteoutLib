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
    ForceType forceType;               ///< Force influence type (radial/wind/explosion)
    ForceShape forceShape;             ///< Influence volume shape
    u32 unknown;                       ///< Unknown field
    u32 boneIndex;                     ///< Index into BONE array
    ForceFlag flags = ForceFlag::None; ///< Force flags (falloff, height gradient, unbounded)
    u32 localChannels;                 ///< Local channel bitmask
    AnimRef<f32> strength;             ///< Animated force strength
    AnimRef<f32> width;                ///< Animated influence width
    AnimRef<f32> height;               ///< Animated influence height
    AnimRef<f32> length;               ///< Animated influence length
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief WRP_ — Warp field (v0–v1, 132 bytes)
 *
 * Warps particle/ribbon trajectories with animated radius, height,
 * and angular/axial/radial strength components.
 */
struct Warp {
    u32 warpType;          ///< Warp type
    u32 boneIndex;         ///< Index into BONE array
    u32 unknown;           ///< Unknown field
    AnimRef<f32> radius;   ///< Animated warp radius
    AnimRef<f32> height;   ///< Animated warp height
    AnimRef<f32> strength; ///< Animated warp strength
    AnimRef<f32> angular;  ///< Animated angular component
    AnimRef<f32> axial;    ///< Animated axial component
    AnimRef<f32> radial;   ///< Animated radial component
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
    u8 type;             ///< 0x01 = forward, 0xFF = reverse (twin)
    u8 faceIndex;        ///< Face this half-edge borders
    u8 vertexIndex;      ///< Target vertex of this half-edge
    u8 nextAroundVertex; ///< Next half-edge around the same vertex
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMMN — Physics mesh BVH node (v0: 12 bytes, v1: 8 bytes)
 *
 * DMMN entries form a linearized k-DOP Bounding Volume Hierarchy (BVH) tree
 * for concave mesh collision. The entry count is always odd: n = 2*n_leaves - 1.
 *
 * **Tree structure** — right-skewed binary tree stored in DFS preorder:
 *   - Array layout: (INT_0, LEAF_1), (INT_2, LEAF_3), ..., LEAF_{n-1}
 *   - Even indices 0..n-3: internal nodes
 *   - Odd indices 1..n-2: leaf nodes
 *   - Last index n-1: leaf node
 *   - Each internal node 2k: left child = leaf 2k+1, right child = node 2k+2
 *
 * **v0** (Havok-era, 12 bytes per node) — stores only the slab normal direction
 * as a plain Vector3f. No quantized slab bounds are present; the tree topology
 * and bounding-slab directions are identical to v1, but distance culling relies
 * on the runtime computing slab projections against meshBoundsCenter/Extent.
 * Only 3 files in the corpus use v0 (all with PHSH v2).
 *
 * **v1** (Domino physics, 8 bytes per node) — octahedral-encoded normal +
 * quantized slab bounds:
 *   - i16 octX, octY: octahedral-mapped slab normal (snorm16 pair)
 *   - u16 slabMin, slabMax: quantized bounding-slab distances along the normal
 *   - Internal nodes: slabMax != 0; leaf sentinel: slabMax == 0
 *     (except the last node, which may have slabMax != 0 despite being a leaf)
 *
 * **Quantization** (v1, universally confirmed across 468 corpus files):
 *   - Per-axis step: tol_i = extent_i / 32767
 *   - Projected step: tol_proj = dot(tolerance, |normal|)
 *   - Slab values quantized as: q = round(projection / tol_proj)
 *   - Root node slab range approaches [-32767, +32767] (full AABB)
 *
 * Internal nodes use one slab direction; their paired leaf uses a DIFFERENT
 * slab direction, forming a 2-DOP bound per primitive group. Most trees (391/468)
 * use multiple slab normals across internal levels for tighter culling.
 *
 * PHSH meshTreeDepth gives the tree height (longest root-to-leaf path in nodes).
 */
struct PhysicsMeshBvhNode {
    /// BVH node: octahedral-encoded slab normal + quantized slab bounds (v1, 8 bytes).
    struct Octahedral {
        i16 octX;    ///< Octahedral-encoded X (snorm16)
        i16 octY;    ///< Octahedral-encoded Y (snorm16)
        u16 slabMin; ///< Quantized bounding-slab min distance
        u16 slabMax; ///< Quantized bounding-slab max distance (0 = leaf sentinel)

        /// Decode octahedral (octX, octY) to a unit-length slab normal.
        Vector3f decodeNormal() const {
            f32 x = static_cast<f32>(snorm16::from_raw(octX));
            f32 y = static_cast<f32>(snorm16::from_raw(octY));
            f32 z = 1.0f - std::abs(x) - std::abs(y);
            if (z < 0.0f) {
                f32 ox = x;
                x = (1.0f - std::abs(y)) * (ox >= 0.0f ? 1.0f : -1.0f);
                y = (1.0f - std::abs(ox)) * (y >= 0.0f ? 1.0f : -1.0f);
            }
            return Vector3f{x, y, z}.normalized();
        }

        /// Encode a unit-length normal into octahedral (octX, octY).
        /// slabMin and slabMax are set from the provided arguments.
        static Octahedral fromNormal(const Vector3f& n, u16 sMin = 0, u16 sMax = 0) {
            f32 invL1 = 1.0f / (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
            f32 ox = n.x * invL1;
            f32 oy = n.y * invL1;
            if (n.z < 0.0f) {
                f32 tmpX = ox;
                ox = (1.0f - std::abs(oy)) * (tmpX >= 0.0f ? 1.0f : -1.0f);
                oy = (1.0f - std::abs(tmpX)) * (oy >= 0.0f ? 1.0f : -1.0f);
            }
            Octahedral result;
            result.octX = snorm16::from_float(ox).value;
            result.octY = snorm16::from_float(oy).value;
            result.slabMin = sMin;
            result.slabMax = sMax;
            return result;
        }

        /// Compute the projected quantization step for this node's normal.
        /// @param tolerance  PHSH meshTolerance (= meshBoundsExtent / 32767).
        f32 projectedStep(const Vector3f& tolerance) const {
            Vector3f n = decodeNormal();
            return std::abs(n.x) * tolerance.x + std::abs(n.y) * tolerance.y +
                   std::abs(n.z) * tolerance.z;
        }

        /// Dequantize slabMin to a signed slab distance in model-space units.
        /// @param tolerance  PHSH meshTolerance (= meshBoundsExtent / 32767).
        f32 decodeSlabMin(const Vector3f& tolerance) const {
            return static_cast<f32>(static_cast<i16>(slabMin)) * projectedStep(tolerance);
        }

        /// Dequantize slabMax to a signed slab distance in model-space units.
        /// @param tolerance  PHSH meshTolerance (= meshBoundsExtent / 32767).
        f32 decodeSlabMax(const Vector3f& tolerance) const {
            return static_cast<f32>(static_cast<i16>(slabMax)) * projectedStep(tolerance);
        }

        /// Dequantize both slab bounds as a (min, max) pair.
        /// @param tolerance  PHSH meshTolerance (= meshBoundsExtent / 32767).
        /// @return {min distance, max distance} in model-space units.
        std::pair<f32, f32> decodeSlabRange(const Vector3f& tolerance) const {
            f32 step = projectedStep(tolerance);
            return {static_cast<f32>(static_cast<i16>(slabMin)) * step,
                    static_cast<f32>(static_cast<i16>(slabMax)) * step};
        }
    };
    union {
        Vector3f normal{};     ///< Slab normal direction (v0, 12 bytes)
        Octahedral octahedral; ///< Packed octahedral normal + slab bounds (v1, 8 bytes)
    };
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMMT — Physics mesh triangle (v0, 28 bytes)
 */
struct PhysicsMeshTriangle {
    u32 vertexIndex0; ///< First vertex index
    u32 vertexIndex1; ///< Second vertex index
    u32 vertexIndex2; ///< Third vertex index
    u32 edgeIndex0;   ///< First edge index
    u32 edgeIndex1;   ///< Second edge index
    u32 edgeIndex2;   ///< Third edge index
    u16 reserved;     ///< Reserved
    u16 flags;        ///< Triangle flags
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMME — Physics mesh edge (v0, 20 bytes)
 */
struct PhysicsMeshEdge {
    u32 edgeType; ///< Edge type
    u32 vertexA;  ///< First vertex index
    u32 vertexB;  ///< Second vertex index
    u32 faceA;    ///< First adjacent face
    u32 faceB;    ///< Second adjacent face
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHSH — Physics shape (v0–v3, 132/292/300 bytes)
 *
 * The 300-byte v3 layout is a three-part union. Bytes 0–79 are the common
 * header. Bytes 80–103 hold shape dimensions for simple shapes (0–3) or
 * are zero for complex shapes. Bytes 80–183 form the convex hull section
 * (shapeType 4); bytes 184–299 form the mesh section (shapeType 5).
 *
 * v2 shares the v3 layout through the hull section but has a shorter mesh
 * section (292 bytes total): bounds/tolerance, four legacy geometry refs,
 * then a 6-dword tail (unknown, vertexCount, faceCount, 2× unknown,
 * treeDepth) — verified against the SC2 client's version-upgrade copier.
 */
struct PhysicsShape {
    Matrix44f transform; ///< 4×4 shape transform matrix

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
    std::vector<PhysicsMeshBvhNode> meshBvhNodes;      ///< BVH tree nodes (DMMN)
    std::vector<Vector4f> meshVertexPositions;         ///< Vertex positions, w=0 (VEC4)
    std::vector<std::array<u16, 7>> meshFaceIndices16; ///< 16-bit face data (MT16, or empty)
    std::vector<std::array<u32, 7>> meshFaceIndices32; ///< 32-bit face data (MT32, or empty)
    // MT32/MT16 per-entry layout: {v0, v1, v2, adj0, adj1, adj2, flags}
    Vector3f meshBoundsCenter; ///< AABB center in model space (quantization grid origin)
    Vector3f
        meshBoundsExtent;   ///< AABB half-extents (quantization range: tolerance = extent / 32767)
    Vector3f meshTolerance; ///< Per-axis quantization step (= extent / 32767)
    u32 meshNormalCount;    ///< Number of mesh normals
    u32 meshVertexCount;    ///< Number of mesh vertices
    u32 meshFaceIndex16Count; ///< MT16 face count (0 when MT32)
    u32 meshFaceIndex32Count; ///< MT32 face count (0 when MT16)
    u32 meshUnknown1;         ///< Unknown mesh parameter
    u32 meshReserved;         ///< Reserved (always 0)
    u32 meshTreeDepth;        ///< BVH tree height (root-to-leaf path length, 1–12)
    f32 meshCollisionMargin;  ///< Collision margin (MT16: small float; MT32: 0.0)

    /// Deprecated shape data with no v3 counterpart. Legacy data that has a
    /// canonical home is migrated on parse instead: v1 halfExtents →
    /// shapeDimensions, v1 vertices → hull/meshVertexPositions (by shape
    /// type), v2 BVH nodes / vertex positions / DMMT triangles →
    /// meshBvhNodes / meshVertexPositions / meshFaceIndices32. The writer
    /// reconstructs the old chunks from the canonical fields.
    struct {
        struct {
            std::vector<PhysicsMeshEdge> unknown2; ///< v2 only: edges
            u32 tailUnknown0 = 0;                  ///< v2 only: tail dword the client never reads
            u32 tailUnknown1 = 0;                  ///< v2 only: tail dword the client never reads
            u32 tailUnknown2 = 0;                  ///< v2 only: tail dword the client never reads
        } v2;
        struct {
            std::vector<u8> unknown0;             ///< v1 only: convex hull unknown data
            std::vector<u16> faceIndices;         ///< v1 only: mesh face indices
            std::vector<Vector4f> planeEquations; ///< v1 only: convex hull plane equations
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
    u32 jointType;         ///< Joint type
    u32 boneIndex1;        ///< First bone index
    u32 boneIndex2;        ///< Second bone index
    Matrix44f matrixBody1; ///< Transform for body 1
    Matrix44f matrixBody2; ///< Transform for body 2
    u32 enableLimits;      ///< Enable angular limits
    f32 limitMin;          ///< Minimum limit angle
    f32 limitMax;          ///< Maximum limit angle
    f32 coneAngle;         ///< Cone constraint angle
    u32 enableFriction;    ///< Enable joint friction
    f32 friction;          ///< Friction coefficient
    f32 dampingRatio;      ///< Damping ratio
    f32 angularFrequency;  ///< Angular frequency
    f32 breakThreshold;    ///< Force threshold to break joint
    u8 enableShape;        ///< Enable shape constraint
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHCT — Physics constraint (v0, 24 bytes)
 *
 * Constrains two rigid bodies with break-force threshold.
 */
struct PhysicsConstraint {
    std::vector<u16> dependents; ///< Dependent bone indices (U16_)
    u16 rigidBody1;              ///< First rigid body index
    u16 rigidBody2;              ///< Second rigid body index
    Flag flags;                  ///< Constraint flags
    f32 breakForce;              ///< Force required to break constraint
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHCC — Cloth collider (v0, 76 bytes)
 *
 * Capsule-shaped collider used by cloth simulation.
 */
struct ClothCollider {
    Matrix44f transform; ///< 4×4 collider transform
    f32 radius;          ///< Capsule radius
    f32 height;          ///< Capsule height
    u32 padding;         ///< Alignment padding
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHAC — Cloth proxy (v0, 32 bytes)
 *
 * Maps cloth vertices to proxy geometry for collision.
 */
struct ClothProxy {
    u32 proxyIndex;                 ///< Proxy mesh index
    u32 clothIndex;                 ///< Cloth mesh index
    std::vector<u64> proxyVertices; ///< Proxy vertex data (U64_)
    std::vector<u32> proxyWeights;  ///< Proxy blend weights (U32_)
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
    f32 liftFactor;                       ///< Lift factor (v4+)
    f32 sphereStiffness;                  ///< Sphere collider stiffness (v4+)
    u32 flatten;                          ///< Flatten mode (v4+)
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

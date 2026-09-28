// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file physics.h
 * @brief Physics structures — forces, warps, shapes, rigid bodies, joints, cloth
 *
 * Defines all M3 physics chunk types: Force (FOR_) and Warp (WRP_) for
 * particle/ribbon/body influences; PhysicsShape (PHSH) with box/sphere/capsule/
 * cylinder/convex-hull/mesh variants; RigidBody (PHRB); PhysicsJoint (PHYJ) for
 * articulated connections; and ClothPhysics (PHCL) with ClothCollider (PHCC) and
 * ClothProxy (PHAC) for cloth simulation.
 *
 * **Every physics struct holds the current layout.** The parser reads older
 * versions into it and `UpgradePhysics` (physics_upgrade.h) then applies the SC2
 * client's own upgrade — its defaults and its load-time cook — so a parsed model
 * is what the game simulates, and the writer only ever writes current versions.
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
 * Pushes particles and ribbons (flag 0x8) and rigid bodies (flag 0x10) inside
 * an influence volume. A body is affected when its `localForces |
 * worldForces << 16` mask shares a bit with `localChannels`.
 */
struct Force {
    ForceType forceType = ForceType::Directional; ///< Force kind
    ForceShape forceShape = ForceShape::Sphere;   ///< Influence volume shape
    u32 unknown = 0;                   ///< Read as local/world scope; no reader traced yet
    u32 boneIndex = 0;                 ///< Index into BONE array
    ForceFlag flags = ForceFlag::None; ///< Falloff, height gradient, unbounded, targets
    u32 localChannels = 0;             ///< Channel mask matched against body and emitter masks
    AnimRef<f32> strength;             ///< Animated force strength
    AnimRef<f32> width;                ///< Animated influence width
    AnimRef<f32> height;               ///< Animated influence height
    AnimRef<f32> length;               ///< Animated influence length
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief WRP_ — Vertex warp (v1, 132 bytes)
 *
 * A vertex-shader deformation particles and ribbons opt into. The client
 * refuses a v0 record, so the parser drops one.
 */
struct Warp {
    u32 warpType = 0;      ///< Warp type
    u32 boneIndex = 0;     ///< Index into BONE array
    u32 unknown = 0;       ///< Unknown field
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
 * Entries come in consecutive twin pairs: an even entry's twin is the next one
 * (`twinOffset` +1), an odd entry's the previous (-1).
 */
struct ConvexHullHalfEdge {
    i8 twinOffset = 0;  ///< +1 on the even entry of a pair, -1 on the odd one
    u8 originVertex = 0; ///< Vertex the half-edge leaves
    u8 face = 0;         ///< Face the half-edge borders
    u8 nextInFace = 0;   ///< Next half-edge around the same face
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMMN — Physics mesh BVH node (v0: 12 bytes, v1: 8 bytes)
 *
 * The SC2 5.0 client never reads DMMN: it rebuilds each mesh's tree at load and
 * then takes the tree's centre, extent, tolerance and height from the PHSH. The
 * cooker (physics_cook.h) writes none. Kept so a shipped v3 mesh reads and
 * writes back whole.
 *
 * **v1** (8 bytes per node) — octahedral-encoded normal + quantized slab bounds:
 *   - i16 octX, octY: octahedral-mapped slab normal (snorm16 pair)
 *   - u16 slabMin, slabMax: quantized bounding-slab distances along the normal
 *
 * **v0** (Havok era, 12 bytes per node) is a plain normal; only v2 meshes carry
 * it, and those are rebuilt by the upgrade.
 */
struct PhysicsMeshBvhNode {
    /// BVH node: octahedral-encoded slab normal + quantized slab bounds (v1, 8 bytes).
    struct Octahedral {
        i16 octX = 0;    ///< Octahedral-encoded X (snorm16)
        i16 octY = 0;    ///< Octahedral-encoded Y (snorm16)
        u16 slabMin = 0; ///< Quantized bounding-slab min distance
        u16 slabMax = 0; ///< Quantized bounding-slab max distance (0 = leaf sentinel)

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
    };
    // The initializer must sit on the member with the non-trivial default ctor:
    // AppleClang 15 / Clang 16 otherwise treat the implicit ctor as deleted.
    union {
        Vector3f normal;         ///< Slab normal direction (v0, 12 bytes)
        Octahedral octahedral{}; ///< Packed octahedral normal + slab bounds (v1, 8 bytes)
    };
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMMT — Havok-era mesh triangle (v0, 28 bytes)
 *
 * Only a v2 PHSH references it; the upgrade keeps the three vertex indices.
 */
struct PhysicsMeshTriangle {
    u32 vertexIndex0 = 0; ///< First vertex index
    u32 vertexIndex1 = 0; ///< Second vertex index
    u32 vertexIndex2 = 0; ///< Third vertex index
    u32 edgeIndex0 = 0;   ///< First edge index
    u32 edgeIndex1 = 0;   ///< Second edge index
    u32 edgeIndex2 = 0;   ///< Third edge index
    u16 reserved = 0; ///< Reserved
    u16 flags = 0;    ///< Triangle flags
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DMME — Havok-era mesh edge (v0, 20 bytes)
 *
 * Only a v2 PHSH references it, and the upgrade discards it.
 */
struct PhysicsMeshEdge {
    u32 edgeType = 0; ///< Edge type
    u32 vertexA = 0;  ///< First vertex index
    u32 vertexB = 0;  ///< Second vertex index
    u32 faceA = 0;    ///< First adjacent face
    u32 faceB = 0;    ///< Second adjacent face
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHSH — Physics shape (v3, 300 bytes; v0 96, v1 132 and v2 292 read)
 *
 * Bytes 0–103 are common: the matrix, the kind, the two source Refs and the
 * dimensions. Bytes 104–183 are the cooked convex hull (kind 4) and 184–299 the
 * cooked mesh (kind 5).
 *
 * **Source vs cooked.** `sourcePoints`/`sourceTriangles` (+68/+80) are raw
 * input the client cooks at load, with the matrix baked in: a hull from the
 * points, a mesh from both. Only the upgrade of a v0/v1 shape fills them, and
 * `UpgradePhysics` cooks them the same way, so a parsed shape carries the
 * cooked tables and empty sources.
 *
 * **Hull tables** are used directly as a Domino polytope: the counts at
 * +164/+168/+172 rather than the Ref counts, the volume and surface area as
 * cached mass data (buoyancy; mass under a physics-material override).
 *
 * **Mesh tables.** The client rebuilds the tree from the vertices and the three
 * indices of each triangle (plus the low byte of its seventh value), then
 * overwrites the tree's centre, extent, tolerance and height with this record's,
 * so those four must be what its builder computes (`CookMesh`). DMMN and the
 * adjacency are never read.
 */
struct PhysicsShape {
    Matrix44f transform = Matrix44f::identity(); ///< Shape frame -> body bone frame, rows may scale
    PhysicsShapeType shapeType = PhysicsShapeType::Box; ///< Shape type (box/sphere/capsule/cylinder/hull/mesh)
    // Note: 3 bytes alignment padding follow shapeType in the binary layout
    std::vector<Vector3f> sourcePoints;  ///< Uncooked points (VEC3, +68), matrix not yet applied
    std::vector<u16> sourceTriangles;    ///< Uncooked triangle list (U16_, +80), three per face
    Vector3f shapeDimensions{};          ///< Box half-extents; sphere radius; capsule/cylinder radius, length

    // --- Convex Hull section (binary offsets 104–183, shapeType = 4 only) ---
    std::vector<Vector3f> hullVertices;              ///< Vertex positions (VEC3)
    std::vector<Vector4f> hullPlanes;                ///< Face planes (n, d), n unit length (VEC4)
    std::vector<ConvexHullHalfEdge> hullHalfEdges;   ///< Half-edge table (DMSE), twin pairs
    std::vector<u8> hullFaceFirstEdges;              ///< Each face's first half-edge (U8__)
    Vector3f hullCentroid{};                         ///< Volume centroid
    u32 hullVertexCount = 0;                         ///< Vertices the client reads
    u32 hullFaceCount = 0;                           ///< Faces the client reads
    u32 hullHalfEdgeCount = 0;                       ///< Half-edges the client reads
    f32 hullVolume = 0.0f;                           ///< Enclosed volume
    f32 hullSurfaceArea = 0.0f;                      ///< Surface area

    // --- Mesh section (binary offsets 184–299, shapeType = 5 only) ---
    std::vector<PhysicsMeshBvhNode> meshBvhNodes;      ///< BVH tree nodes (DMMN), never read
    std::vector<Vector4f> meshVertexPositions;         ///< Vertex positions, w=0 (VEC4)
    std::vector<std::array<u16, 7>> meshFaceIndices16; ///< 16-bit face data (MT16, or empty)
    std::vector<std::array<u32, 7>> meshFaceIndices32; ///< 32-bit face data (MT32, or empty)
    // MT32/MT16 per-entry layout: {v0, v1, v2, adj0, adj1, adj2, flags}
    Vector3f meshBoundsCenter{}; ///< Tree centre, as the client's builder computes it
    Vector3f meshBoundsExtent{}; ///< Tree half-extent, likewise
    Vector3f meshTolerance{};    ///< Per-axis quantization step (= extent / 32767)
    u32 meshNormalCount = 0;     ///< DMMN count
    u32 meshVertexCount = 0;     ///< Number of mesh vertices
    u32 meshFaceIndex16Count = 0; ///< MT16 face count (0 when MT32); the client reads this, not the Ref
    u32 meshFaceIndex32Count = 0; ///< MT32 face count (0 when MT16)
    u32 meshUnknown1 = 0;     ///< Never read
    u32 meshReserved = 0;     ///< Never read
    u32 meshTreeDepth = 0;    ///< Tree height, as the client's builder computes it
    f32 meshCollisionMargin = 0.0f;  ///< Never read
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHRB — Rigid body (v4, 80 bytes; v0 72, v1 96, v2 104 and v3 56 read)
 *
 * A Domino body on `parentBoneIndex`, with its shapes. `simulationType` is how
 * the body is created; `dynamicState` whether it simulates at a moment.
 */
struct RigidBody {
    u16 simulationType = 0;                    ///< Creation type: 0 dynamic, 1 kinematic, 2 static
    u16 parentBoneIndex = 0;                   ///< Parent bone index
    u32 physicsType = 0;                       ///< Physics-material id game data may override
    f32 density = 0.0f;                        ///< Body density
    f32 friction = 0.0f;                       ///< Surface friction
    f32 restitution = 0.0f;                    ///< Elasticity / bounciness
    f32 linearDamping = 0.0f;                  ///< Linear velocity damping
    f32 angularDamping = 0.0f;                 ///< Angular velocity damping
    f32 inertiaScale = 0.0f;                   ///< Domino inertia scale (gravity scale is fixed at 1)
    AnimRef<u32> dynamicState;                 ///< Simulates now; sampled only when flag bit 1 is set
    f32 dynamicBlendOut = 0.0f;                ///< Never read
    std::vector<PhysicsShape> rigidBodyShape;  ///< Collision shapes (PHSH)
    RigidBodyFlag flags = RigidBodyFlag::None; ///< Rigid body flags
    u16 localForces = 0;                       ///< Local force channel bitmask
    u16 worldForces = 0;                       ///< World force channel bitmask
    u32 priority = 0;                          ///< Never read
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHYJ — Physics joint (v0, 180 bytes)
 *
 * Joins the first body on each of two bones. Angles are radians.
 * `enableLimits` and `enableFriction` are bytes to the client; the upper three
 * bytes are never read.
 */
struct PhysicsJoint {
    u32 jointType = 0;     ///< 0 spherical, 1 revolute, 2 cone-twist, 3 weld
    u32 boneIndex1 = 0;    ///< First bone index
    u32 boneIndex2 = 0;    ///< Second bone index
    Matrix44f matrixBody1 = Matrix44f::identity(); ///< Joint frame in bone 1's frame
    Matrix44f matrixBody2 = Matrix44f::identity(); ///< Joint frame in bone 2's frame
    u32 enableLimits = 0;  ///< Enable angular limits (low byte)
    f32 limitMin = 0.0f;   ///< Minimum limit angle
    f32 limitMax = 0.0f;   ///< Maximum limit angle
    f32 coneAngle = 0.0f;  ///< Cone constraint angle
    u32 enableFriction = 0;    ///< Enable joint friction (low byte)
    f32 friction = 0.0f;   ///< Multiplier on an estimated gravity-holding torque
    f32 dampingRatio = 0.0f;      ///< Weld spring damping ratio
    f32 angularFrequency = 0.0f;  ///< Weld spring frequency
    f32 breakThreshold = 0.0f;    ///< Never read
    u8 enableShape = 0;    ///< Collide connected
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHCC — Cloth collider (v0, 76 bytes)
 *
 * A capsule along its own +Z, centred, on `bone`.
 */
struct ClothCollider {
    Matrix44f transform = Matrix44f::identity(); ///< 4×4 collider transform
    f32 radius = 0.0f;   ///< Capsule radius
    f32 height = 0.0f;   ///< Capsule full length
    u32 bone = 0;        ///< Bone index; 0xFFFF is the model root
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHAC — Cloth proxy (v0, 32 bytes)
 *
 * Binds one cloth-influenced region to its cage: per vertex of `proxyIndex`,
 * four cage-local `u16` lanes packed in a `u64` and four byte weights (/255)
 * packed in a `u32`.
 */
struct ClothProxy {
    u32 proxyIndex = 0;             ///< The bound region (REGN index)
    u32 clothIndex = 0;             ///< The cage's region (REGN index)
    std::vector<u64> proxyVertices; ///< Four cage vertices per bound vertex (U64_)
    std::vector<u32> proxyWeights;  ///< Four byte weights per bound vertex (U32_)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PHCL — Cloth physics (v4, 192 bytes; v0 140, v1 116, v2 128 and v3 192 read)
 *
 * One cloth: the cage region its particles are, per-particle anchors and
 * movability, colliders, the regions it drives (PHAC) and the solver
 * parameters. A record with colliders and no cage exports them to other models.
 * Added in MODL v28.
 */
struct ClothPhysics {
    u32 cageRegion = 0;                   ///< The cage's REGN index
    u32 skinBoneCount = 0;                ///< Never read
    std::vector<u16> skinBones;           ///< Bones the anchors and colliders use (U16_)
    std::vector<u8> simEnabled;           ///< Per-particle flags, bit 0 movable (U8__)
    std::vector<u32> vertexBones;         ///< Per-particle anchor bones, four bytes (U32_)
    std::vector<u32> vertexWeights;       ///< Per-particle anchor weights, four bytes (U32_)
    std::vector<ClothCollider> colliders; ///< Cloth colliders (PHCC)
    std::vector<ClothProxy> proxies;      ///< Cloth proxies (PHAC)
    f32 density = 0.0f;                   ///< Cloth density
    f32 tracking = 0.0f;                  ///< Tracking factor
    f32 stretchStiffness = 0.0f;          ///< Stretch stiffness
    f32 horizontalStiffness = 0.0f;       ///< Horizontal stiffness
    f32 bendingStiffness = 0.0f;          ///< Bending stiffness
    f32 damping = 0.0f;                   ///< Damping coefficient
    f32 friction = 0.0f;                  ///< Friction coefficient
    f32 gravity = 0.0f;                   ///< Gravity influence
    f32 explosionScale = 0.0f;            ///< Explosion force scale
    f32 windScale = 0.0f;                 ///< Wind force scale
    f32 shearStiffness = 0.0f;            ///< Shear stiffness
    f32 dragFactor = 0.0f;                ///< Drag factor
    f32 liftFactor = 0.0f;                ///< Lift factor
    f32 sphereStiffness = 0.0f;           ///< Sphere collider stiffness
    u32 flatten = 0;                      ///< Flatten mode
    AnimRef<u32> active;                  ///< Animated active state; sampled only when flag bit 1 is set
    u32 useSkinCollision = 0;             ///< Use skin mesh for collision
    f32 skinOffset = 0.0f;                ///< Skin collision offset
    f32 skinExponent = 0.0f;              ///< Skin collision exponent
    f32 skinStiffness = 0.0f;             ///< Skin collision stiffness
    u32 localChannels = 0;                ///< Never read
    Vector3f localWind{};                 ///< Local wind direction and magnitude
    M3_DEFINE_VERSION_ACCESSORS()
};

} // namespace m3
} // namespace whiteout

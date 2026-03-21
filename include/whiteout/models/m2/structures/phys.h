// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file phys.h
 * @brief M2 physics file (.phys) and PFDC inline physics structures
 *
 * Physics data defines ragdoll simulation: rigid bodies, collision shapes,
 * and constraints (joints). Standalone `.phys` files and inline PFDC chunks
 * share the same internal chunked format.
 *
 * The physics system has evolved through 7 versions (0–6):
 * - v0–1 (MoP/Legion): BODY + SHAP + WELJ
 * - v2 (Legion 7.0.1): Renamed to BDY2/SHP2/WLJ2, added fields
 * - v3 (Legion 7.0.3): Added PLYT polytope shapes
 * - v4 (Legion 7.0.3): Added SHOJ shoulder + REVJ revolute joints
 * - v5 (Legion 7.3): PLYT header changes
 * - v6 (7.3–9.0): Newer joint structs (SHJ2, WLJ3, REV2)
 *
 * All chunks use reversed 4-byte ASCII tags (standard WoW format).
 *
 * @see M2_FILE_FORMAT_SPECIFICATION.md §17 for binary layout and corpus statistics
 */

#include <vector>
#include "base.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// Matrix Types
// ============================================================================

/**
 * @brief 3×4 transformation matrix for physics reference frames
 *
 * Stores rotation (3×3) and translation (4th column) as 12 contiguous floats.
 * Used to define joint reference frames in body-local coordinates.
 */
struct Matrix3x4 {
    std::array<f32, 12> data; ///< Row-major 3×4 matrix (12 floats)

    Matrix3x4() : data{} {}
    constexpr Matrix3x4(std::array<f32, 12> d) : data(d) {}
};

// ============================================================================
// PHYS Chunk Tags
// ============================================================================

constexpr u32 PHYS_TAG = makeTag("PHYS"); ///< Physics header chunk
constexpr u32 PHYT_TAG = makeTag("PHYT"); ///< Physics type chunk (v1+)
constexpr u32 BODY_TAG = makeTag("BODY"); ///< Body definition (v0–1)
constexpr u32 BDY2_TAG = makeTag("BDY2"); ///< Body definition v2
constexpr u32 BDY3_TAG = makeTag("BDY3"); ///< Body definition v3
constexpr u32 BDY4_TAG = makeTag("BDY4"); ///< Body definition v4 (current)
constexpr u32 SHAP_TAG = makeTag("SHAP"); ///< Shape definition (v0–1)
constexpr u32 SHP2_TAG = makeTag("SHP2"); ///< Shape definition v2 (current)
constexpr u32 BOXS_TAG = makeTag("BOXS"); ///< Box shape data
constexpr u32 CAPS_TAG = makeTag("CAPS"); ///< Capsule shape data
constexpr u32 SPHS_TAG = makeTag("SPHS"); ///< Sphere shape data
constexpr u32 PLYT_TAG = makeTag("PLYT"); ///< Polytope (convex hull) shape data (v3+)
constexpr u32 JOIN_TAG = makeTag("JOIN"); ///< Joint entry array
constexpr u32 WELJ_TAG = makeTag("WELJ"); ///< Weld joint (v0–1)
constexpr u32 WLJ2_TAG = makeTag("WLJ2"); ///< Weld joint v2
constexpr u32 WLJ3_TAG = makeTag("WLJ3"); ///< Weld joint v3 (v6+)
constexpr u32 SPHJ_TAG = makeTag("SPHJ"); ///< Spherical joint
constexpr u32 SHOJ_TAG = makeTag("SHOJ"); ///< Shoulder joint (v4–5)
constexpr u32 SHJ2_TAG = makeTag("SHJ2"); ///< Shoulder joint v2 (v6+)
constexpr u32 PRSJ_TAG = makeTag("PRSJ"); ///< Prismatic joint (v4–5)
constexpr u32 PRS2_TAG = makeTag("PRS2"); ///< Prismatic joint v2 (v6+)
constexpr u32 REVJ_TAG = makeTag("REVJ"); ///< Revolute joint (v4–5)
constexpr u32 REV2_TAG = makeTag("REV2"); ///< Revolute joint v2 (v6+)
constexpr u32 DSTJ_TAG = makeTag("DSTJ"); ///< Distance joint
constexpr u32 PHYV_TAG = makeTag("PHYV"); ///< Physics values (v1+)

// ============================================================================
// PHYS Header
// ============================================================================

/**
 * @brief Physics file header (2 bytes)
 *
 * Contains a single version field. See the version history table in the
 * file-level documentation for chunk compatibility per version.
 */
struct PHYSHeader {
    u16 version = 0; ///< Physics format version (0–6)
    // version history:
    // 0: Mists (5.0.1.15464)
    // 1: Legion (7.0.1.20773)
    // 2: Legion (7.0.1.20979) - changed chunk names to BDY2, SHP2, WLJ2
    // 3: Legion (7.0.3.21287) - added PLYT polytope shapes
    // 4: Legion (7.0.3.21846)
    // 5: Legion (7.3.0.24500)
    // 6: >= 7.3, <= 9.0, but seen in 34003
};

// ============================================================================
// PHYT — Physics Type (v1+)
// ============================================================================

/**
 * @brief Physics simulation type
 *
 * Likely controls ragdoll complexity: 0=none/simple, 3=standard, 4=enhanced.
 * Always 4 bytes.
 */
struct PHYTEntry {
    u32 phyt = 0; ///< Physics type (0=simple, 3=standard, 4=enhanced)
};

// ============================================================================
// Body Structures (BODY / BDY2 / BDY3 / BDY4)
// ============================================================================

/**
 * @brief Rigid body definition v0–1 (BODY)
 *
 * Used in physics versions 0–1. Superseded by BDY2 in v2.
 */
struct BODYEntry {
    u16 type = 0;           ///< Body type: 0=kinematic/static, 1=dynamic
    u8 padding_a[2] = {};   ///< Padding
    Vector3f position;      ///< Body position in bone space
    u16 modelBoneIndex = 0; ///< Model bone index
    u8 padding_b[2] = {};   ///< Padding
    i32 shapes_base = 0;    ///< Starting index into shapes[]
    i32 shapes_count = 0;   ///< Number of shapes in this body
};

/**
 * @brief Rigid body definition v2 (BDY2)
 *
 * Adds mass scale field. Used in physics version 2.
 */
struct BDY2Entry {
    u16 type = 0;           ///< Body type: 0=kinematic/static, 1=dynamic
    u8 padding_a[2] = {};   ///< Padding
    Vector3f position;      ///< Body position in bone space
    u16 modelBoneIndex = 0; ///< Model bone index
    u8 padding_b[2] = {};   ///< Padding
    i32 shapes_base = 0;    ///< Starting index into shapes[]
    i32 shapes_count = 0;   ///< Number of shapes in this body
    f32 massScale = 1.0f;   ///< Mass multiplier (1.0 or 10.0)
};

/**
 * @brief Rigid body definition v3 (BDY3)
 *
 * Adds full physics material properties. Used in physics version 3.
 */
struct BDY3Entry {
    u16 type = 0;                    ///< Body type: 0=kinematic/static, 1=dynamic
    u16 boneIndex = 0;               ///< Model bone index
    Vector3f position;               ///< Body position in bone space
    u16 shapeIndex = 0;              ///< Starting index into SHP2 array
    u8 padding_b[2] = {};            ///< Padding
    i32 shapesCount = 0;             ///< Number of shapes in this body
    f32 mass = 0.0f;                 ///< Body mass factor (0.0–3.0, typical: 0.5/0.75/1.0)
    f32 massScale = 1.0f;            ///< Mass multiplier (1.0 or 10.0)
    f32 drag = 0.0f;                 ///< Drag coefficient (0.0–10.0)
    f32 angularDamping = 0.0f;       ///< Angular velocity damping (0.0–20.0)
    f32 linearDamping = 0.89999998f; ///< Linear velocity damping (0.01–0.9)
};

/**
 * @brief Rigid body definition v4 (BDY4, 48 bytes)
 *
 * Current version used in v4–6. Adds flags/collision group metadata.
 * The corpus uses BDY4 exclusively (all 429 bodies across 54 files).
 */
struct BDY4Entry {
    u16 type = 0;                    ///< Body type: 0=kinematic/static, 1=dynamic
    u16 boneIndex = 0;               ///< Model bone index
    Vector3f position;               ///< Body position in bone space
    u16 shapeIndex = 0;              ///< Starting index into SHP2 array
    u8 padding_b[2] = {};            ///< Padding
    i32 shapesCount = 0;             ///< Number of shapes in this body
    f32 mass = 0.0f;                 ///< Body mass factor (0.0–3.0, typical: 0.5/0.75/1.0)
    f32 massScale = 1.0f;            ///< Mass multiplier (1.0 or 10.0)
    f32 drag = 0.0f;                 ///< Drag coefficient / air resistance (0.0–10.0)
    f32 angularDamping = 0.0f;       ///< Angular velocity damping / spin slowdown (0.0–20.0)
    f32 linearDamping = 0.89999998f; ///< Linear velocity damping (0.01–0.9)
    u8 unk_2c[4] = {};               ///< v4: bit flags (0xC000/0x8000), v6: collision group index
};

// ============================================================================
// Shape Structures (SHAP / SHP2)
// ============================================================================

/**
 * @brief Collision shape type enumeration
 */
enum class ShapeType : i16 {
    Box = 0,     ///< Box shape (→ BOXS data)
    Capsule = 1, ///< Capsule shape (→ CAPS data)
    Sphere = 2,  ///< Sphere shape (→ SPHS data)
    Polytope = 3 ///< Convex hull polyhedron (→ PLYT data, v3+)
};

/**
 * @brief Shape definition v0–1 (SHAP)
 */
struct SHAPEntry {
    i16 shapeType = 0;      ///< ShapeType enum value
    i16 shapeIndex = 0;     ///< Index into the corresponding shape data chunk
    u8 unk[4] = {};         ///< Unknown (always 0)
    f32 friction = 0.0f;    ///< Surface friction (0.0–0.7, typical: 0.6)
    f32 restitution = 0.0f; ///< Bounciness (0.0–1.0, typical: 0.0)
    f32 density = 0.0f;     ///< Mass per unit volume
};

/**
 * @brief Shape definition v2 (SHP2, 32 bytes)
 *
 * Current version used in v2–6. The corpus uses SHP2 exclusively
 * (all 426 shapes). Distribution: Capsule=131, Polytope=295, Box/Sphere=0.
 */
struct SHP2Entry {
    i16 shapeType = 0;      ///< ShapeType enum value
    i16 shapeIndex = 0;     ///< Index into the corresponding shape data chunk
    u8 unk[4] = {};         ///< Unknown (always 0 in corpus)
    f32 friction = 0.0f;    ///< Surface friction (0.0–0.7, typical: 0.6)
    f32 restitution = 0.0f; ///< Bounciness (0.0–1.0, typical: 0.0)
    f32 density = 0.0f;     ///< Mass per unit volume (20000 for capsules, varies for polytopes)
    u32 unk_14 = 0;         ///< Unknown (always 0 in corpus)
    f32 unk_18 = 1.0f;      ///< Likely a scale factor (1.0 in 425/426 entries)
    u16 unk_1c = 0;         ///< Unknown (always 0 in corpus)
    u16 padding_1e = 0;     ///< Uninitialized in v4 (memory garbage), zeroed in v5+
};

// ============================================================================
// Shape Data Structures (BOXS / CAPS / SPHS / PLYT)
// ============================================================================

/**
 * @brief Axis-aligned box shape data (BOXS)
 */
struct BOXSEntry {
    Matrix3x4 matrix; ///< 3×4 transform matrix (orientation + half-extents)
    Vector3f center;  ///< Box center position
};

/**
 * @brief Capsule shape data (CAPS, 28 bytes)
 *
 * A capsule defined by two endpoints and a radius.
 * 131 entries across 44 files in corpus.
 */
struct CAPSEntry {
    Vector3f localPosition1; ///< First endpoint in local space
    Vector3f localPosition2; ///< Second endpoint in local space
    f32 radius = 0.0f;       ///< Capsule radius
};

/**
 * @brief Sphere shape data (SPHS)
 */
struct SPHSEntry {
    Vector3f localPosition; ///< Sphere center in local space
    f32 radius = 0.0f;      ///< Sphere radius
};

/**
 * @brief Single vertex for a polytope convex hull
 */
struct PLYTVertex {
    Vector3f vertex; ///< Convex hull vertex position
};

/**
 * @brief BSP tree node for polytope collision detection (4 bytes)
 */
struct PLYTNode {
    u8 byte0 = 0; ///< Node metadata / face reference
    u8 byte1 = 0; ///< Index value
    u8 byte2 = 0; ///< Child reference (0xFF = leaf)
    u8 byte3 = 0; ///< Child reference
};

// NOTE: PLYT chunk uses a "headers-then-data" layout:
//   u32 entryCount
//   PLYTHeader[entryCount]  (all headers contiguous)
//   PLYTData[entryCount]    (all data blocks contiguous)
// The current PLYTEntry struct works for in-memory representation,
// but the binary parser must read all headers first, then all data.

/**
 * @brief Polytope shape variable-length data block
 *
 * Contains the actual geometric data for one convex hull polytope.
 * Data size per entry: vertexCount×12 + faceCount×16 + nodeCount×4 + faceCount.
 */
struct PLYTData {
    std::vector<Vector3f> vertices;   ///< Convex hull mesh vertices
    std::vector<Vector4f> facePlanes; ///< Face plane equations (nx, ny, nz, d)
    std::vector<PLYTNode> nodes;      ///< BSP tree nodes for collision detection
    std::vector<u8> faceIndices;      ///< Per-face index/metadata
};

/**
 * @brief Polytope shape header (80 bytes)
 *
 * All runtime pointer fields are zero on disk and filled at load time.
 * In the corpus all entries are uniform: 8 vertices, 6 faces, 24 nodes.
 */
struct PLYTHeader {
    u32 vertexCount = 0;    ///< Convex hull vertex count (corpus: always 8)
    u32 unk_04 = 0;         ///< 0 in v4, non-zero in v5+ (version metadata)
    u64 runtime_08_ptr = 0; ///< Always 0 on disk, filled at runtime
    u32 faceCount = 0;      ///< Number of faces / face planes (corpus: always 6)
    u32 unk_14 = 0;         ///< 1 in v4, varies in v5+
    u64 runtime_18_ptr = 0; ///< Always 0 on disk
    u64 runtime_20_ptr = 0; ///< Always 0 on disk
    u32 nodeCount = 0;      ///< BSP tree node count (corpus: always 24)
    u32 unk_2c = 0;         ///< 0 in v4, non-zero in v5+
    u64 runtime_30_ptr = 0; ///< Always 0 on disk
    f32 bounds[6] = {};     ///< Bounding data (min/max extents)
};

/**
 * @brief Complete polytope shape (header + geometric data)
 */
struct PLYTEntry {
    PLYTHeader header; ///< Fixed-size header (80 bytes)
    PLYTData data;     ///< Variable-length geometric data
};

// ============================================================================
// JOIN — Joint Entry (16 bytes)
// ============================================================================

/**
 * @brief Joint type enumeration
 *
 * Version mapping: v4–5 use SHOJ/WLJ2/REVJ, v6 uses SHJ2/WLJ3/REV2.
 */
enum class JointType : i16 {
    Spherical = 0, ///< Ball-and-socket joint (→ SPHJ)
    Shoulder = 1,  ///< Cone-twist shoulder joint (→ SHOJ/SHJ2)
    Weld = 2,      ///< Rigid weld joint (→ WELJ/WLJ2/WLJ3)
    Revolute = 3,  ///< Hinge joint (→ REVJ/REV2, v2+)
    Prismatic = 4, ///< Sliding joint (→ PRSJ/PRS2, v2+)
    Distance = 5   ///< Distance constraint (→ DSTJ, v2+)
};

/**
 * @brief Joint constraint definition (16 bytes)
 *
 * Connects two bodies with a specific constraint type. The jointId field
 * indexes into the corresponding joint data chunk.
 *
 * Corpus: Shoulder=252, Weld=35, Revolute=62. 1–18 joints per file.
 */
struct JOINEntry {
    u32 bodyAIdx = 0;  ///< First body index
    u32 bodyBIdx = 0;  ///< Second body index
    u8 unk[4] = {};    ///< Unknown (always 0 in corpus)
    i16 jointType = 0; ///< JointType enum value
    i16 jointId = 0;   ///< Index into the corresponding joint data chunk
};

// ============================================================================
// Joint Data Structures
// ============================================================================

/**
 * @brief Weld joint v0–1 (WELJ)
 *
 * Rigidly connects two bodies. Spring parameters control how much
 * the weld can flex.
 */
struct WELJEntry {
    Matrix3x4 frameA;               ///< Reference frame in body A (48 bytes)
    Matrix3x4 frameB;               ///< Reference frame in body B (48 bytes)
    f32 angularFrequencyHz = 0.0f;  ///< Angular spring frequency (Hz)
    f32 angularDampingRatio = 0.0f; ///< Angular spring damping ratio
};

/**
 * @brief Weld joint v2 (WLJ2, 112 bytes)
 *
 * Adds linear spring parameters. Corpus: angularFreq=0.6, all others=0.0.
 */
struct WLJ2Entry {
    Matrix3x4 frameA;               ///< Reference frame in body A
    Matrix3x4 frameB;               ///< Reference frame in body B
    f32 angularFrequencyHz = 0.0f;  ///< Angular spring frequency (Hz)
    f32 angularDampingRatio = 0.0f; ///< Angular spring damping ratio
    f32 linearFrequencyHz = 0.0f;   ///< Linear spring frequency (Hz)
    f32 linearDampingRatio = 0.0f;  ///< Linear spring damping ratio
};

/**
 * @brief Weld joint v3 (WLJ3, 116 bytes, v6+)
 *
 * Extends WLJ2 with an additional spring stiffness parameter.
 */
struct WLJ3Entry {
    Matrix3x4 frameA;               ///< Reference frame in body A
    Matrix3x4 frameB;               ///< Reference frame in body B
    f32 angularFrequencyHz = 0.0f;  ///< Angular spring frequency (Hz)
    f32 angularDampingRatio = 0.0f; ///< Angular spring damping ratio
    f32 linearFrequencyHz = 0.0f;   ///< Linear spring frequency (Hz)
    f32 linearDampingRatio = 0.0f;  ///< Linear spring damping ratio
    f32 unk70 = 0.0f;               ///< Likely spring stiffness (corpus: 0.0 or 0.005)
};

/**
 * @brief Spherical (ball-and-socket) joint (SPHJ)
 */
struct SPHJEntry {
    Vector3f anchorA;          ///< Anchor point in body A local space
    Vector3f anchorB;          ///< Anchor point in body B local space
    f32 frictionTorque = 0.0f; ///< Friction torque at the joint
};

/**
 * @brief Shoulder (cone-twist) joint v1 (SHOJ, 116 bytes, v4–5)
 *
 * A cone-twist joint with configurable twist limits and a cone angle.
 * Motor mode 1 enables position-tracking motor.
 */
struct SHOJEntry {
    Matrix3x4 frameA;           ///< Reference frame in body A
    Matrix3x4 frameB;           ///< Reference frame in body B
    f32 lowerTwistAngle = 0.0f; ///< Lower twist limit in degrees (corpus: -15 or -20)
    f32 upperTwistAngle = 0.0f; ///< Upper twist limit in degrees (corpus: 15 or 20)
    f32 coneAngle = 0.0f;       ///< Cone angle in degrees (corpus: 20–60)
    f32 maxMotorTorque = 0.0f;  ///< Maximum motor torque (v2+)
    u32 motorMode = 0;          ///< 0=free, 1=motor position mode (v2+)
};

/**
 * @brief Shoulder (cone-twist) joint v2 (SHJ2, 124 bytes, v6+)
 *
 * Extends SHOJ with motor frequency/damping parameters.
 */
struct SHJ2Entry {
    Matrix3x4 frameA;             ///< Reference frame in body A
    Matrix3x4 frameB;             ///< Reference frame in body B
    f32 lowerTwistAngle = 0.0f;   ///< Lower twist limit in degrees
    f32 upperTwistAngle = 0.0f;   ///< Upper twist limit in degrees
    f32 coneAngle = 0.0f;         ///< Cone angle in degrees
    f32 maxMotorTorque = 0.0f;    ///< Maximum motor torque
    u32 motorMode = 0;            ///< 0=free, 1=motor position mode
    f32 motorFrequencyHz = 0.0f;  ///< Motor spring frequency (corpus: 1.0–3.0 Hz)
    f32 motorDampingRatio = 0.0f; ///< Motor spring damping (corpus: 0.0 or 0.7)
};

/**
 * @brief Prismatic (sliding) joint v1 (PRSJ, v4–5)
 *
 * A joint that allows linear sliding along one axis.
 */
struct PRSJEntry {
    Matrix3x4 frameA;         ///< Reference frame in body A
    Matrix3x4 frameB;         ///< Reference frame in body B
    f32 lowerLimit = 0.0f;    ///< Lower slide limit
    f32 upperLimit = 0.0f;    ///< Upper slide limit
    f32 unk_68 = 0.0f;        ///< Unknown
    f32 maxMotorForce = 0.0f; ///< Maximum motor force
    f32 unk_70 = 0.0f;        ///< Unknown
    u32 motorMode = 0;        ///< Motor mode
};

/**
 * @brief Prismatic (sliding) joint v2 (PRS2, v6+)
 *
 * Extends PRSJ with motor frequency/damping parameters.
 */
struct PRS2Entry {
    Matrix3x4 frameA;             ///< Reference frame in body A
    Matrix3x4 frameB;             ///< Reference frame in body B
    f32 lowerLimit = 0.0f;        ///< Lower slide limit
    f32 upperLimit = 0.0f;        ///< Upper slide limit
    f32 unk_68 = 0.0f;            ///< Unknown
    f32 maxMotorForce = 0.0f;     ///< Maximum motor force
    f32 unk_70 = 0.0f;            ///< Unknown
    u32 motorMode = 0;            ///< Motor mode
    f32 motorFrequencyHz = 0.0f;  ///< Motor spring frequency
    f32 motorDampingRatio = 0.0f; ///< Motor spring damping
};

/**
 * @brief Revolute (hinge) joint v1 (REVJ, 112 bytes, v4–5)
 *
 * A single-axis rotational constraint. Motor mode: 1=position, 2=velocity.
 */
struct REVJEntry {
    Matrix3x4 frameA;          ///< Reference frame in body A
    Matrix3x4 frameB;          ///< Reference frame in body B
    f32 lowerAngle = 0.0f;     ///< Lower angle limit in degrees (corpus: -60 to -10)
    f32 upperAngle = 0.0f;     ///< Upper angle limit in degrees (corpus: 10 to 60)
    f32 maxMotorTorque = 0.0f; ///< Maximum motor torque
    u32 motorMode = 0;         ///< 0=free, 1=position, 2=velocity
};

/**
 * @brief Revolute (hinge) joint v2 (REV2, 120 bytes, v6+)
 *
 * Extends REVJ with motor frequency/damping parameters.
 * Corpus: motorFreq=1.0, motorDamping=0.7.
 */
struct REV2Entry {
    Matrix3x4 frameA;             ///< Reference frame in body A
    Matrix3x4 frameB;             ///< Reference frame in body B
    f32 lowerAngle = 0.0f;        ///< Lower angle limit in degrees
    f32 upperAngle = 0.0f;        ///< Upper angle limit in degrees
    f32 maxMotorTorque = 0.0f;    ///< Maximum motor torque
    u32 motorMode = 0;            ///< 0=free, 1=position, 2=velocity
    f32 motorFrequencyHz = 0.0f;  ///< Motor spring frequency (corpus: 1.0 Hz)
    f32 motorDampingRatio = 0.0f; ///< Motor spring damping (corpus: 0.7)
};

/**
 * @brief Distance joint (DSTJ)
 *
 * Constrains two bodies to maintain a maximum distance between
 * their anchor points.
 */
struct DSTJEntry {
    Vector3f localAnchorA;           ///< Anchor point in body A local space
    Vector3f localAnchorB;           ///< Anchor point in body B local space
    f32 some_distance_factor = 0.0f; ///< Distance factor / maximum distance
};

// ============================================================================
// PHYV — Physics Values (v1+)
// ============================================================================

/**
 * @brief Physics simulation values
 *
 * Additional simulation parameters. Purpose of individual floats is unknown.
 */
struct PHYVEntry {
    f32 unk[6] = {}; ///< Unknown physics values (6 floats)
};

} // namespace m2
} // namespace whiteout

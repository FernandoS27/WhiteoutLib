// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file misc.h
 * @brief Miscellaneous structures — hit-test, attachments, behaviors, IK, and more
 *
 * Defines M3 structures that don't fit into other categories: HitTestShape
 * (SSGS) and AttachmentVolume (ATVL) for collision/selection geometry;
 * TriggerData (TRGD) and TurretBehavior (PATU) for gameplay systems;
 * BillboardBehavior (BBSC) for camera-facing bones; IKJoint (IKJT),
 * IKTwoJoint (IK2J), IKCCD (IKCC), and OneBoneSolver (PAOB) for inverse
 * kinematics; ShadowBox (SHBX), ViewVolume (VVOL), and TrailingModel (TMD_).
 *
 * @see M3_FILE_FORMAT_SPECIFICATION.md §14 Miscellaneous
 */

#include "base.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Miscellaneous
// ============================================================================

/**
 * @brief SSGS — Hit-test shape (v0–v1, 108 bytes)
 *
 * Defines a collision / selection volume (box, sphere, capsule, cylinder,
 * or mesh) attached to a bone. Used for both tight and fuzzy hit testing.
 */
struct HitTestShape {
    HitTestShapeType shapeType;            ///< Shape type (box/sphere/capsule/cylinder/mesh)
    u16 boneIndex = 0;                     ///< Index into BONE array
    u16 padding = 0;                       ///< Alignment padding
    Matrix44f transform = Matrix44f::identity();                   ///< 4×4 shape transform
    std::vector<Vector3f> vertexPositions; ///< Mesh vertex positions (VEC3, mesh type only)
    std::vector<u16> faceIndices;          ///< Mesh triangle indices (U16_, mesh type only)
    f32 sizeX = 0.0f;                      ///< X dimension (radius for sphere/capsule)
    f32 sizeY = 0.0f;                      ///< Y dimension (height for capsule/cylinder)
    f32 sizeZ = 0.0f;                      ///< Z dimension
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief ATVL — Attachment volume (v0, 116 bytes)
 *
 * Like HitTestShape but with two bone indices for attachment-point volumes.
 */
struct AttachmentVolume {
    u32 bone1 = 0;                         ///< First bone index
    u32 bone2 = 0;                         ///< Second bone index
    HitTestShapeType shapeType;            ///< Shape type
    u16 boneIndex = 0;                     ///< Primary bone index
    u16 padding = 0;                       ///< Alignment padding
    Matrix44f transform = Matrix44f::identity();                   ///< 4×4 volume transform
    std::vector<Vector3f> vertexPositions; ///< Mesh vertex positions (VEC3)
    std::vector<u16> faceIndices;          ///< Mesh triangle indices (U16_)
    f32 sizeX = 0.0f;                      ///< X dimension
    f32 sizeY = 0.0f;                      ///< Y dimension
    f32 sizeZ = 0.0f;                      ///< Z dimension
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief TRGD — Trigger data (v0, 24 bytes)
 *
 * Named trigger with associated data indices for gameplay events.
 */
struct TriggerData {
    std::vector<u32> dataIndices; ///< Data index array (U32_)
    std::string name;             ///< Trigger name (Ref<CHAR>)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PATU — Turret behavior (v0–v4, 152 bytes)
 *
 * Configures turret rotation constraints for a bone with yaw/pitch
 * limits, weights, and an optional main-turret flag.
 */
struct TurretBehavior {
    Matrix44f transform = Matrix44f::identity();     ///< 4×4 turret transform
    Vector4f unknown1{};     ///< Unknown vector 1
    Vector4f unknown2{};     ///< Unknown vector 2
    u16 boneIndex = 0;       ///< Index into BONE array
    u8 useAsMainTurret = 0;  ///< Non-zero if this is the main turret
    u8 turretGroupId = 0;    ///< Turret group identifier
    u32 yawLimited = 0;      ///< Enable yaw limits
    f32 yawMin = 0.0f;       ///< Minimum yaw angle (radians)
    f32 yawMax = 0.0f;       ///< Maximum yaw angle (radians)
    f32 yawWeight = 0.0f;    ///< Yaw rotation weight
    u32 pitchLimited = 0;    ///< Enable pitch limits
    f32 pitchMin = 0.0f;     ///< Minimum pitch angle (radians)
    f32 pitchMax = 0.0f;     ///< Maximum pitch angle (radians)
    f32 pitchWeight = 0.0f;  ///< Pitch rotation weight
    f32 unknown3 = 0.0f;     ///< Unknown field
    f32 unknown4 = 0.0f;     ///< Unknown field
    Vector3f mainBoneOffset{}; ///< Offset from main bone
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief BBSC — Billboard behavior (v0, 48 bytes)
 *
 * Turns one bone to face the camera. The only source of billboarding there is:
 * `BoneFlag::Billboard1/2` are set on no bone in the whole corpus.
 *
 * StarCraft II applies these at draw time, per view, from `sub_10290A890` —
 * `CBBSolver::Solve` is a stub — and writes the bone's *local* rotation, so
 * the subtree follows.
 */
struct BillboardBehavior {
    std::vector<u16> dependents; ///< Dependent bone indices (U16_). Empty in every
                                 ///< shipped record; the engine never reads them
    u16 boneIndex = 0;           ///< Index into BONE array
    u8 billboardType = 0;        ///< Which axes may turn — see BillboardType
    u8 cameraLookAt = 1;         ///< Non-zero: aim from this bone at the eye. Zero:
                                 ///< aim along the camera's view direction instead,
                                 ///< so every such bone shares one orientation
    Quaternion up{};             ///< MISNAMED: not a direction. A rotation applied
                                 ///< *before* the billboard basis, and only by the
                                 ///< axis-locked types 0/1/2
    Quaternion forward{};        ///< MISNAMED likewise: the same kind of pre-rotation,
                                 ///< taken only by type 6, and only on a bone whose
                                 ///< parent is another bone. Types 3/4/5 take neither
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief IKJT — IK joint (v0, 32 bytes)
 *
 * Inverse kinematics joint with raycast up/down range, max speed,
 * and goal threshold for terrain-following or foot-planting.
 */
struct IKJoint {
    std::vector<u16> dependents; ///< Dependent bone indices (U16_)
    u16 boneIndex1 = 0;          ///< First bone index
    u16 boneIndex2 = 0;          ///< Second bone index
    f32 raycastUp = 0.0f;        ///< Raycast upward distance (positive; shipped 1.5 / 3.0)
    f32 raycastDown = 0.0f;      ///< Raycast downward offset, SIGNED (shipped -4.0 / -3.0):
                                 ///< the surface window is [z + raycastDown, z + raycastUp]
    f32 maxSpeed = 0.0f;         ///< Maximum IK solving speed
    f32 goalThreshold = 0.0f;    ///< Goal distance threshold
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief IK2J — Two-joint IK solver (v0, 48 bytes)
 *
 * Classic two-bone IK (e.g. elbow/knee) with hinge axis, angle limits,
 * and search range for target acquisition.
 */
struct IKTwoJoint {
    std::vector<u16> dependents; ///< Dependent bone indices (U16_)
    u16 boneBase = 0;            ///< Base bone (e.g. upper arm/thigh)
    u16 boneTarget = 0;          ///< Target bone (e.g. forearm/shin)
    u16 boneEnd = 0;             ///< End effector bone (e.g. hand/foot)
    u16 padding = 0;             ///< Alignment padding
    Vector3f hingeAxis{};        ///< Hinge rotation axis
    f32 maxAngleInner = 0.0f;    ///< Maximum inner angle
    f32 maxAngleOuter = 0.0f;    ///< Maximum outer angle
    f32 searchUp = 0.0f;         ///< Search range upward
    f32 searchDown = 0.0f;       ///< Search range downward
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief IKCC — CCD IK solver (v0, 24 bytes)
 *
 * Cyclic Coordinate Descent IK solver with base/target bones
 * and vertical search range.
 */
struct IKCCD {
    std::vector<u16> dependents; ///< Dependent bone indices (U16_)
    u16 boneBase = 0;            ///< Base bone index
    u16 boneTarget = 0;          ///< Target bone index
    f32 searchUp = 0.0f;         ///< Search range upward
    f32 searchDown = 0.0f;       ///< Search range downward
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief PAOB — One-bone IK solver (v0, 24 bytes)
 *
 * Simple single-bone orientation solver with angle limit and fallback bone.
 */
struct OneBoneSolver {
    std::vector<u16> dependents; ///< Dependent bone indices (U16_)
    u16 bone = 0;                ///< Primary bone index
    u16 boneFallback = 0;        ///< Fallback bone index
    Flag flags;                  ///< Solver flags
    f32 maxAngle = 0.0f;         ///< Maximum rotation angle
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief SHBX — Shadow box (v0, 64 bytes)
 *
 * Axis-aligned shadow volume defined by a 4×4 transform matrix.
 */
struct ShadowBox {
    Matrix44f matrix = Matrix44f::identity(); ///< 4×4 shadow box transform
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief VVOL — View volume (v0, 40 bytes)
 *
 * Animated visibility volume bound to a bone, used for culling decisions.
 */
struct ViewVolume {
    u32 nodeIndex = 0;      ///< Index into BONE array
    AnimRef<Vector3f> size; ///< Animated half-extents (36 bytes)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief TMD_ — Trailing model (v0–v1, defunct)
 *
 * Legacy trailing model data. Observed in older files but no longer
 * actively used by the engine.
 */
struct TrailingModel {
    std::vector<Vector3f> vectors; ///< Control vectors (VEC3)
    f32 param0 = 0.0f;             ///< Parameter 0 (observed: 5.0)
    f32 param1 = 0.0f;             ///< Parameter 1 (observed: 1.0)
    AnimRef<f32> animFloat0;       ///< Animated float 0 (init 0.5)
    AnimRef<f32> animFloat1;       ///< Animated float 1 (init 1.0)
    u32 flag = 0;                  ///< Flag (observed: 1)
    u32 reserved0 = 0;             ///< Reserved
    u32 reserved1 = 0;             ///< Reserved
    M3_DEFINE_VERSION_ACCESSORS()
};

} // namespace m3
} // namespace whiteout

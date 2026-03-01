#pragma once

#include "m2_base.h"
#include <vector>

namespace m2 {

// Matrix types for physics
struct Matrix3x4 {
    std::array<f32, 12> data; // 3x4 = 12 floats
    
    Matrix3x4() : data{} {}
    constexpr Matrix3x4(std::array<f32, 12> d) : data(d) {}
};

// ============================================================================
// PHYS Chunk Tags and Structures
// ============================================================================

constexpr u32 PHYS_TAG = makeTag("PHYS");
constexpr u32 PHYT_TAG = makeTag("PHYT");
constexpr u32 BODY_TAG = makeTag("BODY");
constexpr u32 BDY2_TAG = makeTag("BDY2");
constexpr u32 BDY3_TAG = makeTag("BDY3");
constexpr u32 BDY4_TAG = makeTag("BDY4");
constexpr u32 SHAP_TAG = makeTag("SHAP");
constexpr u32 SHP2_TAG = makeTag("SHP2");
constexpr u32 BOXS_TAG = makeTag("BOXS");
constexpr u32 CAPS_TAG = makeTag("CAPS");
constexpr u32 SPHS_TAG = makeTag("SPHS");
constexpr u32 PLYT_TAG = makeTag("PLYT");
constexpr u32 JOIN_TAG = makeTag("JOIN");
constexpr u32 WELJ_TAG = makeTag("WELJ");
constexpr u32 WLJ2_TAG = makeTag("WLJ2");
constexpr u32 WLJ3_TAG = makeTag("WLJ3");
constexpr u32 SPHJ_TAG = makeTag("SPHJ");
constexpr u32 SHOJ_TAG = makeTag("SHOJ");
constexpr u32 SHJ2_TAG = makeTag("SHJ2");
constexpr u32 PRSJ_TAG = makeTag("PRSJ");
constexpr u32 PRS2_TAG = makeTag("PRS2");
constexpr u32 REVJ_TAG = makeTag("REVJ");
constexpr u32 REV2_TAG = makeTag("REV2");
constexpr u32 DSTJ_TAG = makeTag("DSTJ");
constexpr u32 PHYV_TAG = makeTag("PHYV");

// ============================================================================
// PHYS Header
// ============================================================================

struct PHYSHeader {
    u16 version = 0;
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
// PHYT - Physics Type (version 1+)
// ============================================================================

struct PHYTEntry {
    u32 phyt = 0; // default: 0
};

// ============================================================================
// BODY Structures
// ============================================================================

struct BODYEntry {
    /*0x00*/ u16 type = 0; // maps to dmBodyDef type enum. 0 -> 1, 1 -> 0 (dm_dynamicBody), * -> 2
    /*0x02*/ u8 padding_a[2] = {};
    /*0x04*/ Vector3f position;
    /*0x10*/ u16 modelBoneIndex = 0;
    /*0x12*/ u8 padding_b[2] = {};
    /*0x14*/ i32 shapes_base = 0; // starting at shapes[shapes_base]
    /*0x18*/ i32 shapes_count = 0; // number of shapes in this body
};

struct BDY2Entry {
    /*0x00*/ u16 type = 0;
    /*0x02*/ u8 padding_a[2] = {};
    /*0x04*/ Vector3f position;
    /*0x10*/ u16 modelBoneIndex = 0;
    /*0x12*/ u8 padding_b[2] = {};
    /*0x14*/ i32 shapes_base = 0;
    /*0x18*/ i32 shapes_count = 0;
    /*0x1c*/ f32 unk_1c = 1.0f; // default 1.0
};

struct BDY3Entry {
    /*0x00*/ u16 type = 0;
    /*0x02*/ u16 boneIndex = 0;
    /*0x04*/ Vector3f position;
    /*0x10*/ u16 shapeIndex = 0;
    /*0x12*/ u8 padding_b[2] = {};
    /*0x14*/ i32 shapesCount = 0;
    /*0x18*/ f32 unk0 = 0.0f;
    /*0x1c*/ f32 unk_1c = 1.0f; // default 1.0
    /*0x20*/ f32 drag = 0.0f; // default 0
    /*0x24*/ f32 unk1 = 0.0f; // default 0, sort of weight
    /*0x28*/ f32 unk_28 = 0.89999998f; // default 0.89999998
};

struct BDY4Entry {
    /*0x00*/ u16 type = 0;
    /*0x02*/ u16 boneIndex = 0;
    /*0x04*/ Vector3f position;
    /*0x10*/ u16 shapeIndex = 0;
    /*0x12*/ u8 padding_b[2] = {};
    /*0x14*/ i32 shapesCount = 0;
    /*0x18*/ f32 unk0 = 0.0f;
    /*0x1c*/ f32 unk_1c = 1.0f;
    /*0x20*/ f32 drag = 0.0f;
    /*0x24*/ f32 unk1 = 0.0f;
    /*0x28*/ f32 unk_28 = 0.89999998f;
    /*0x2c*/ u8 unk_2c[4] = {}; // default 0x00000000
};

// ============================================================================
// SHAP Structures - Shape Definitions
// ============================================================================

enum class ShapeType : i16 {
    Box = 0,
    Capsule = 1,
    Sphere = 2,
    Polytope = 3 // version 3+
};

struct SHAPEntry {
    /*0x00*/ i16 shapeType = 0;
    /*0x02*/ i16 shapeIndex = 0; // into the corresponding chunk
    /*0x04*/ u8 unk[4] = {};
    /*0x08*/ f32 friction = 0.0f;
    /*0x0c*/ f32 restitution = 0.0f;
    /*0x10*/ f32 density = 0.0f;
};

struct SHP2Entry {
    /*0x00*/ i16 shapeType = 0;
    /*0x02*/ i16 shapeIndex = 0;
    /*0x04*/ u8 unk[4] = {};
    /*0x08*/ f32 friction = 0.0f;
    /*0x0c*/ f32 restitution = 0.0f;
    /*0x10*/ f32 density = 0.0f;
    /*0x14*/ u32 unk_14 = 0; // default 0
    /*0x18*/ f32 unk_18 = 1.0f; // default 1.0
    /*0x1c*/ u16 unk_1c = 0; // default 0
    /*0x1e*/ u16 unk_1e = 0; // no default, padding?
};

// ============================================================================
// Shape Data Structures
// ============================================================================

struct BOXSEntry {
    /*0x00*/ Matrix3x4 matrix;
    /*0x30*/ Vector3f center;
};

struct CAPSEntry {
    Vector3f localPosition1;
    Vector3f localPosition2;
    f32 radius = 0.0f;
};

struct SPHSEntry {
    Vector3f localPosition;
    f32 radius = 0.0f;
};

struct PLYTVertex {
    Vector3f vertex;
};

struct PLYTNode {
    i8 unk_00 = 0; // 1 or -1
    i8 vertexIndex = 0; // index in vertex list
    i8 unkIndex0 = 0; // index into the nodes
    i8 unkIndex1 = 0; // index into the nodes
};

struct PLYTData {
    std::vector<Vector3f> vertices; // convex hull mesh vertices
    std::vector<u8> unk_1; // len = count_10
    std::vector<u8> unk_2; // len = count_10
    std::vector<PLYTNode> nodes; // tree structure
};

struct PLYTHeader {
    /*0x00*/ u32 vertexCount = 0; // mostly 8
    /*0x04*/ u8 unk_04[4] = {};
    /*0x08*/ u64 runtime_08_ptr = 0; // RUNTIME_08_ptr_data_0 in file
    /*0x10*/ u32 count_10 = 0; // mostly 6
    /*0x14*/ u8 unk_14[4] = {};
    /*0x18*/ u64 runtime_18_ptr = 0; // RUNTIME_18_ptr_data_1
    /*0x20*/ u64 runtime_20_ptr = 0; // RUNTIME_20_ptr_data_2
    /*0x28*/ u32 nodeCount = 0; // mostly 24
    /*0x2c*/ u8 unk_2c[4] = {};
    /*0x30*/ u64 runtime_30_ptr = 0; // RUNTIME_30_ptr_data_3
    /*0x38*/ f32 unk_38[6] = {}; // might be floats with e-08 values
};

struct PLYTEntry {
    PLYTHeader header;
    PLYTData data;
};

// ============================================================================
// JOIN - Joint Entry
// ============================================================================

enum class JointType : i16 {
    Spherical = 0,
    Shoulder = 1,
    Weld = 2,
    Revolute = 3, // version 2+
    Prismatic = 4, // version 2+
    Distance = 5 // version 2+
};

struct JOINEntry {
    u32 bodyAIdx = 0;
    u32 bodyBIdx = 0;
    u8 unk[4] = {};
    i16 jointType = 0;
    i16 jointId = 0; // reference into the corresponding chunk
};

// ============================================================================
// Joint Data Structures
// ============================================================================

struct WELJEntry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 angularFrequencyHz = 0.0f;
    /*0x64*/ f32 angularDampingRatio = 0.0f;
};

struct WLJ2Entry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 angularFrequencyHz = 0.0f;
    /*0x64*/ f32 angularDampingRatio = 0.0f;
    /*0x68*/ f32 linearFrequencyHz = 0.0f; // default 0
    /*0x6c*/ f32 linearDampingRatio = 0.0f; // default 0
};

struct WLJ3Entry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 angularFrequencyHz = 0.0f;
    /*0x64*/ f32 angularDampingRatio = 0.0f;
    /*0x68*/ f32 linearFrequencyHz = 0.0f;
    /*0x6c*/ f32 linearDampingRatio = 0.0f;
    /*0x70*/ f32 unk70 = 0.0f;
};

struct SPHJEntry {
    Vector3f anchorA;
    Vector3f anchorB;
    f32 frictionTorque = 0.0f;
};

struct SHOJEntry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 lowerTwistAngle = 0.0f;
    /*0x64*/ f32 upperTwistAngle = 0.0f;
    /*0x68*/ f32 coneAngle = 0.0f;
    /*0x6c*/ f32 maxMotorTorque = 0.0f; // version 2+
    /*0x70*/ u32 motorMode = 0; // version 2+
};

struct SHJ2Entry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 lowerTwistAngle = 0.0f;
    /*0x64*/ f32 upperTwistAngle = 0.0f;
    /*0x68*/ f32 coneAngle = 0.0f;
    /*0x6c*/ f32 maxMotorTorque = 0.0f;
    /*0x70*/ u32 motorMode = 0;
    /*0x74*/ f32 motorFrequencyHz = 0.0f;
    /*0x78*/ f32 motorDampingRatio = 0.0f;
};

struct PRSJEntry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 lowerLimit = 0.0f;
    /*0x64*/ f32 upperLimit = 0.0f;
    /*0x68*/ f32 unk_68 = 0.0f;
    /*0x6c*/ f32 maxMotorForce = 0.0f;
    /*0x70*/ f32 unk_70 = 0.0f;
    /*0x74*/ u32 motorMode = 0;
};

struct PRS2Entry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 lowerLimit = 0.0f;
    /*0x64*/ f32 upperLimit = 0.0f;
    /*0x68*/ f32 unk_68 = 0.0f;
    /*0x6c*/ f32 maxMotorForce = 0.0f;
    /*0x70*/ f32 unk_70 = 0.0f;
    /*0x74*/ u32 motorMode = 0;
    /*0x78*/ f32 motorFrequencyHz = 0.0f;
    /*0x7c*/ f32 motorDampingRatio = 0.0f;
};

struct REVJEntry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 lowerAngle = 0.0f;
    /*0x64*/ f32 upperAngle = 0.0f;
    /*0x68*/ f32 maxMotorTorque = 0.0f;
    /*0x6c*/ u32 motorMode = 0; // 1: motorPositionMode, 2: motorVelocityMode
};

struct REV2Entry {
    /*0x00*/ Matrix3x4 frameA;
    /*0x30*/ Matrix3x4 frameB;
    /*0x60*/ f32 lowerAngle = 0.0f;
    /*0x64*/ f32 upperAngle = 0.0f;
    /*0x68*/ f32 maxMotorTorque = 0.0f;
    /*0x6c*/ u32 motorMode = 0;
    /*0x70*/ f32 motorFrequencyHz = 0.0f;
    /*0x74*/ f32 motorDampingRatio = 0.0f;
};

struct DSTJEntry {
    /*0x00*/ Vector3f localAnchorA;
    /*0x0c*/ Vector3f localAnchorB;
    /*0x18*/ f32 some_distance_factor = 0.0f;
};

// ============================================================================
// PHYV - Physics Values (version 1+)
// ============================================================================

struct PHYVEntry {
    /*0x00*/ f32 unk[6] = {};
};

} // namespace m2

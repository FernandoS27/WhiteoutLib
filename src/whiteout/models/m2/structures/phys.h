
#pragma once

#include <vector>
#include <whiteout/models/m2/structures/base.h>

namespace whiteout {
namespace m2 {

struct Matrix3x4 {
    std::array<f32, 12> data;

    Matrix3x4() : data{} {}
    constexpr Matrix3x4(std::array<f32, 12> d) : data(d) {}
};

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

struct PHYSHeader {
    u16 version = 0;

};

struct PHYTEntry {
    u32 phyt = 0;
};

struct BODYEntry {
    u16 type = 0;
    u8 padding_a[2] = {};
    Vector3f position;
    u16 modelBoneIndex = 0;
    u8 padding_b[2] = {};
    i32 shapes_base = 0;
    i32 shapes_count = 0;
};

struct BDY2Entry {
    u16 type = 0;
    u8 padding_a[2] = {};
    Vector3f position;
    u16 modelBoneIndex = 0;
    u8 padding_b[2] = {};
    i32 shapes_base = 0;
    i32 shapes_count = 0;
    f32 massScale = 1.0f;
};

struct BDY3Entry {
    u16 type = 0;
    u16 boneIndex = 0;
    Vector3f position;
    u16 shapeIndex = 0;
    u8 padding_b[2] = {};
    i32 shapesCount = 0;
    f32 mass = 0.0f;
    f32 massScale = 1.0f;
    f32 drag = 0.0f;
    f32 angularDamping = 0.0f;
    f32 linearDamping = 0.89999998f;
};

struct BDY4Entry {
    u16 type = 0;
    u16 boneIndex = 0;
    Vector3f position;
    u16 shapeIndex = 0;
    u8 padding_b[2] = {};
    i32 shapesCount = 0;
    f32 mass = 0.0f;
    f32 massScale = 1.0f;
    f32 drag = 0.0f;
    f32 angularDamping = 0.0f;
    f32 linearDamping = 0.89999998f;
    u8 unk_2c[4] = {};
};

enum class ShapeType : i16 {
    Box = 0,
    Capsule = 1,
    Sphere = 2,
    Polytope = 3
};

struct SHAPEntry {
    i16 shapeType = 0;
    i16 shapeIndex = 0;
    u8 unk[4] = {};
    f32 friction = 0.0f;
    f32 restitution = 0.0f;
    f32 density = 0.0f;
};

struct SHP2Entry {
    i16 shapeType = 0;
    i16 shapeIndex = 0;
    u8 unk[4] = {};
    f32 friction = 0.0f;
    f32 restitution = 0.0f;
    f32 density = 0.0f;
    u32 unk_14 = 0;
    f32 unk_18 = 1.0f;
    u16 unk_1c = 0;
    u16 padding_1e = 0;
};

struct BOXSEntry {
    Matrix3x4 matrix;
    Vector3f center;
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
    u8 byte0 = 0;
    u8 byte1 = 0;
    u8 byte2 = 0;
    u8 byte3 = 0;
};

struct PLYTData {
    std::vector<Vector3f> vertices;
    std::vector<Vector4f> facePlanes;
    std::vector<PLYTNode> nodes;
    std::vector<u8> faceIndices;
};

struct PLYTHeader {
    u32 vertexCount = 0;
    u32 unk_04 = 0;
    u64 runtime_08_ptr = 0;
    u32 faceCount = 0;
    u32 unk_14 = 0;
    u64 runtime_18_ptr = 0;
    u64 runtime_20_ptr = 0;
    u32 nodeCount = 0;
    u32 unk_2c = 0;
    u64 runtime_30_ptr = 0;
    f32 bounds[6] = {};
};

struct PLYTEntry {
    PLYTHeader header;
    PLYTData data;
};

enum class JointType : i16 {
    Spherical = 0,
    Shoulder = 1,
    Weld = 2,
    Revolute = 3,
    Prismatic = 4,
    Distance = 5
};

struct JOINEntry {
    u32 bodyAIdx = 0;
    u32 bodyBIdx = 0;
    u8 unk[4] = {};
    i16 jointType = 0;
    i16 jointId = 0;
};

struct WELJEntry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 angularFrequencyHz = 0.0f;
    f32 angularDampingRatio = 0.0f;
};

struct WLJ2Entry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 angularFrequencyHz = 0.0f;
    f32 angularDampingRatio = 0.0f;
    f32 linearFrequencyHz = 0.0f;
    f32 linearDampingRatio = 0.0f;
};

struct WLJ3Entry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 angularFrequencyHz = 0.0f;
    f32 angularDampingRatio = 0.0f;
    f32 linearFrequencyHz = 0.0f;
    f32 linearDampingRatio = 0.0f;
    f32 unk70 = 0.0f;
};

struct SPHJEntry {
    Vector3f anchorA;
    Vector3f anchorB;
    f32 frictionTorque = 0.0f;
};

struct SHOJEntry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 lowerTwistAngle = 0.0f;
    f32 upperTwistAngle = 0.0f;
    f32 coneAngle = 0.0f;
    f32 maxMotorTorque = 0.0f;
    u32 motorMode = 0;
};

struct SHJ2Entry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 lowerTwistAngle = 0.0f;
    f32 upperTwistAngle = 0.0f;
    f32 coneAngle = 0.0f;
    f32 maxMotorTorque = 0.0f;
    u32 motorMode = 0;
    f32 motorFrequencyHz = 0.0f;
    f32 motorDampingRatio = 0.0f;
};

struct PRSJEntry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 lowerLimit = 0.0f;
    f32 upperLimit = 0.0f;
    f32 unk_68 = 0.0f;
    f32 maxMotorForce = 0.0f;
    f32 unk_70 = 0.0f;
    u32 motorMode = 0;
};

struct PRS2Entry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 lowerLimit = 0.0f;
    f32 upperLimit = 0.0f;
    f32 unk_68 = 0.0f;
    f32 maxMotorForce = 0.0f;
    f32 unk_70 = 0.0f;
    u32 motorMode = 0;
    f32 motorFrequencyHz = 0.0f;
    f32 motorDampingRatio = 0.0f;
};

struct REVJEntry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 lowerAngle = 0.0f;
    f32 upperAngle = 0.0f;
    f32 maxMotorTorque = 0.0f;
    u32 motorMode = 0;
};

struct REV2Entry {
    Matrix3x4 frameA;
    Matrix3x4 frameB;
    f32 lowerAngle = 0.0f;
    f32 upperAngle = 0.0f;
    f32 maxMotorTorque = 0.0f;
    u32 motorMode = 0;
    f32 motorFrequencyHz = 0.0f;
    f32 motorDampingRatio = 0.0f;
};

struct DSTJEntry {
    Vector3f localAnchorA;
    Vector3f localAnchorB;
    f32 some_distance_factor = 0.0f;
};

struct PHYVEntry {
    f32 unk[6] = {};
};

}
}

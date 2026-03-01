#pragma once

#include "../types.h"

namespace whiteout {
namespace m2 {

enum class GlobalFlag : u32 {
    None = 0,
    TiltX = 0x00000001,
    TiltY = 0x00000002,
    UseTextureCombinerCombos = 0x00000008,
    LoadPhysicsData = 0x00000020,
    Unk_0x80 = 0x00000080,
    CameraRelated = 0x00000100,
    NewParticleRecord = 0x00000200,
    Unk_0x400 = 0x00000400,
    TextureTransformsUsesBoneSequences = 0x00000800,
    Unk_0x1000 = 0x00001000,
    ChunkedAnimFiles = 0x00002000,
};

inline GlobalFlag operator|(GlobalFlag lhs, GlobalFlag rhs) {
    return static_cast<GlobalFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline GlobalFlag operator&(GlobalFlag lhs, GlobalFlag rhs) {
    return static_cast<GlobalFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline GlobalFlag operator|=(GlobalFlag& lhs, GlobalFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline GlobalFlag operator&=(GlobalFlag& lhs, GlobalFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline GlobalFlag operator~(GlobalFlag flag) {
    return static_cast<GlobalFlag>(~static_cast<u32>(flag));
}

inline bool hasFlag(GlobalFlag flags, GlobalFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

struct GlobalFlags {
    GlobalFlag value = GlobalFlag::None;
};

struct GlobalSequence {
    u32 timestamp = 0;
};

struct Sequence {
    u16 id = 0;
    u16 variationIndex = 0;
    u32 duration = 0;
    f32 movespeed = 0.0f;
    u32 flags = 0;
    i16 frequency = 0;
    u16 padding = 0;

    u32 replayMin = 0;
    u32 replayMax = 0;

    u16 blendTimeIn = 0;
    u16 blendTimeOut = 0;

    Extent bounding;

    i16 variationNext = -1;
    u16 aliasNext = 0;
};

enum class SequenceFlag : u32 {
    None = 0,
    Looping = 0x00000020,
    NoLag = 0x00000040,
    AnimatedSetup = 0x00000080,
    EnableComposite = 0x00000200,
    StoredAnimated = 0x00000100,
};

inline SequenceFlag operator|(SequenceFlag lhs, SequenceFlag rhs) {
    return static_cast<SequenceFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline SequenceFlag operator&(SequenceFlag lhs, SequenceFlag rhs) {
    return static_cast<SequenceFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline SequenceFlag operator|=(SequenceFlag& lhs, SequenceFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline SequenceFlag operator&=(SequenceFlag& lhs, SequenceFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline SequenceFlag operator~(SequenceFlag flag) {
    return static_cast<SequenceFlag>(~static_cast<u32>(flag));
}

inline bool hasFlag(SequenceFlag flags, SequenceFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

struct Vertex {
    Vector3f position;
    std::array<u8, 4> boneWeights = {0};
    std::array<u8, 4> boneIndices = {0};
    Vector3f normal;
    std::array<Vector2f, 2> texCoords;
};

struct Bone {
    i32 keyBoneId = -1;
    u32 flags = 0;
    i16 parentBoneId = -1;
    u16 submeshId = 0;

    u32 boneNameCRC = 0;

    AnimationTrack<Vector3f> translation;
    AnimationTrack<Quaternion> rotation;
    AnimationTrack<Vector3f> scale;
    Vector3f pivot;
};

enum class BoneFlag : u32 {
    None = 0,
    IgnoreParentTranslate = 0x001,
    IgnoreParentScale = 0x002,
    IgnoreParentRotation = 0x004,
    SphericalBillboard = 0x008,
    CylindricalBillboardX = 0x010,
    CylindricalBillboardY = 0x020,
    CylindricalBillboardZ = 0x040,
    Transformed = 0x200,
    Kinematic = 0x400,
    HelmetAnimScaled = 0x1000,
};

inline BoneFlag operator|(BoneFlag lhs, BoneFlag rhs) {
    return static_cast<BoneFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

inline BoneFlag operator&(BoneFlag lhs, BoneFlag rhs) {
    return static_cast<BoneFlag>(static_cast<u32>(lhs) & static_cast<u32>(rhs));
}

inline BoneFlag operator|=(BoneFlag& lhs, BoneFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline BoneFlag operator&=(BoneFlag& lhs, BoneFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline BoneFlag operator~(BoneFlag flag) {
    return static_cast<BoneFlag>(~static_cast<u32>(flag));
}

inline bool hasFlag(BoneFlag flags, BoneFlag flag) {
    return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;
}

struct Texture {
    u32 type = 0;
    u32 flags = 0;
    std::string filename;
};

struct Material {
    u16 flags = 0;
    u16 blendingMode = 0;
};

enum class MaterialFlag : u16 {
    None = 0,
    Unlit = 0x01,
    Unfogged = 0x02,
    TwoSided = 0x04,
    DepthTest = 0x08,
    DepthWrite = 0x10,
    NoAlphaComposite = 0x800,
};

inline MaterialFlag operator|(MaterialFlag lhs, MaterialFlag rhs) {
    return static_cast<MaterialFlag>(static_cast<u16>(lhs) | static_cast<u16>(rhs));
}

inline MaterialFlag operator&(MaterialFlag lhs, MaterialFlag rhs) {
    return static_cast<MaterialFlag>(static_cast<u16>(lhs) & static_cast<u16>(rhs));
}

inline MaterialFlag operator|=(MaterialFlag& lhs, MaterialFlag rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline MaterialFlag operator&=(MaterialFlag& lhs, MaterialFlag rhs) {
    lhs = lhs & rhs;
    return lhs;
}

inline MaterialFlag operator~(MaterialFlag flag) {
    return static_cast<MaterialFlag>(~static_cast<u16>(flag));
}

inline bool hasFlag(MaterialFlag flags, MaterialFlag flag) {
    return (static_cast<u16>(flags) & static_cast<u16>(flag)) != 0;
}

struct TextureWeight {
    AnimationTrack<i16> weight;
};

struct TextureTransform {
    AnimationTrack<Vector3f> translation;
    AnimationTrack<Quaternion> rotation;
    AnimationTrack<Vector3f> scaling;
};

struct ColorAnimation {
    AnimationTrack<Vector3f> color;
    AnimationTrack<i16> alpha;
};

struct Light {
    u16 type = 0;
    i16 boneId = -1;
    Vector3f position;

    AnimationTrack<Vector3f> ambientColor;
    AnimationTrack<f32> ambientIntensity;
    AnimationTrack<Vector3f> diffuseColor;
    AnimationTrack<f32> diffuseIntensity;
    AnimationTrack<f32> attenuationStart;
    AnimationTrack<f32> attenuationEnd;
    AnimationTrack<u8> visibility;
};

struct CameraSpline {
    Vector3f value;
    Vector3f inTangent;
    Vector3f outTangent;
};

struct Camera {
    u32 type = 0;
    f32 fieldOfView = 0.0f;
    f32 farClip = 0.0f;
    f32 nearClip = 0.0f;

    AnimationTrack<CameraSpline> positions;
    Vector3f positionBase;

    AnimationTrack<CameraSpline> targetPositions;
    Vector3f targetPositionBase;

    AnimationTrack<f32> roll;
    AnimationTrack<f32> fieldOfViewTrack;
};

struct Attachment {
    u32 id = 0;
    u16 boneId = 0;
    u16 unknown = 0;
    Vector3f position;
    AnimationTrack<u8> animate;
};

struct RibbonEmitter {
    u32 ribbonId = UINT32_MAX;
    u32 boneId = 0;
    Vector3f position;

    std::vector<u16> textureIndices;
    std::vector<u16> materialIndices;

    AnimationTrack<Vector3f> colorTrack;
    AnimationTrack<i16> alphaTrack;
    AnimationTrack<f32> heightAbove;
    AnimationTrack<f32> heightBelow;

    f32 edgesPerSecond = 0.0f;
    f32 edgeLifetime = 0.0f;
    f32 gravity = 0.0f;
    u16 textureRows = 1;
    u16 textureCols = 1;
    AnimationTrack<u16> texSlot;
    AnimationTrack<u8> visibility;
    i16 priorityPlane = 0;
    i8 ribbonColorIndex = -1;
    i8 textureTransformIndex = -1;
};

struct ParticleEmitter {
    u32 particleId = UINT32_MAX;
    u32 flags = 0;
    Vector3f position;
    u16 boneId = 0;

    std::string geometryModelFilename;
    std::string recursionModelFilename;

    u8 blendingType = 0;
    u8 emitterType = 0;
    u16 particleColorIndex = 0;

    AnimationTrack<f32> emissionSpeed;
    AnimationTrack<f32> speedVariation;
    AnimationTrack<f32> verticalRange;
    AnimationTrack<f32> horizontalRange;
    AnimationTrack<f32> gravity;
    AnimationTrack<f32> lifespan;
    f32 lifespanVary = 0.0f;
    AnimationTrack<f32> emissionRate;
    f32 emissionRateVary = 0.0f;
};

struct Event {
    u32 identifier = 0;
    u32 data = 0;
    u32 boneId = 0;
    Vector3f position;
    AnimationTrackBase enabled;
};

struct MD20Header {
    u32 magic = MD20_TAG;
    u32 version = M2_VERSION_LEGION;

    std::string modelName;
    GlobalFlags globalFlags;

    std::vector<GlobalSequence> globalLoops;
    std::vector<Sequence> sequences;
    std::vector<u16> sequenceIdxHashById;

    std::vector<Bone> bones;
    std::vector<u16> keyBoneIds;

    std::vector<Vertex> vertices;
    u32 numSkinProfiles = 0;

    std::vector<ColorAnimation> colors;
    std::vector<Texture> textures;
    std::vector<TextureWeight> textureWeights;
    std::vector<TextureTransform> textureTransforms;
    std::vector<u16> textureIndicesById;
    std::vector<Material> materials;
    std::vector<u16> boneCombos;
    std::vector<u16> textureCombos;
    std::vector<u16> textureCoordCombos;
    std::vector<u16> textureWeightCombos;
    std::vector<u16> textureTransformCombos;

    Extent bounding;
    Extent collision;

    std::vector<u16> collisionTriangleIndices;
    std::vector<Vector3f> collisionVertices;
    std::vector<Vector3f> collisionFaceNormals;

    std::vector<Attachment> attachments;
    std::vector<u16> attachmentIndicesById;
    std::vector<Event> events;
    std::vector<Light> lights;
    std::vector<Camera> cameras;
    std::vector<u16> cameraIndicesById;
    std::vector<RibbonEmitter> ribbonEmitters;
    std::vector<ParticleEmitter> particleEmitters;
    std::vector<u16> textureCombinerCombos;
};

} // namespace m2
} // namespace whiteout

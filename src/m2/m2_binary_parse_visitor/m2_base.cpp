#include "../m2_binary_parse_visitor.h"
#include "common/binary_reader.h"

namespace m2 {

using common::BinaryReader;

M2BinaryParseVisitor::M2BinaryParseVisitor(common::BinaryReader& reader, i32 maxSize_)
    : reader(reader), maxSize(maxSize_) {
}

void M2BinaryParseVisitor::start() {
    baseOffset = reader.getPosition();
}

void M2BinaryParseVisitor::visit(GlobalFlags& flags) {
    flags.value = static_cast<GlobalFlag>(reader.read<u32>());
}

void M2BinaryParseVisitor::visit(GlobalSequence& seq) {
    seq.timestamp = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(Sequence& seq) {
    seq.id = reader.read<u16>();
    seq.variationIndex = reader.read<u16>();
    seq.duration = reader.read<u32>();
    seq.movespeed = reader.read<f32>();
    seq.flags = reader.read<u32>();
    seq.frequency = reader.read<i16>();
    seq.padding = reader.read<u16>();
    seq.replayMin = reader.read<u32>();
    seq.replayMax = reader.read<u32>();
    seq.blendTimeIn = reader.read<u16>();
    seq.blendTimeOut = reader.read<u16>();
    
    seq.bounding.minimum = reader.read<Vector3f>();
    seq.bounding.maximum = reader.read<Vector3f>();
    seq.bounding.sphereRadius = reader.read<f32>();
    
    seq.variationNext = reader.read<i16>();
    seq.aliasNext = reader.read<u16>();
}

void M2BinaryParseVisitor::visit(Vertex& vertex) {
    vertex.position = reader.read<Vector3f>();
    vertex.boneWeights = reader.readArray<u8, 4>();
    vertex.boneIndices = reader.readArray<u8, 4>();
    vertex.normal = reader.read<Vector3f>();
    vertex.texCoords = reader.readArray<Vector2f, 2>();
}

void M2BinaryParseVisitor::visit(Bone& bone) {
    bone.keyBoneId = reader.read<i32>();
    bone.flags = reader.read<u32>();
    bone.parentBoneId = reader.read<i16>();
    bone.submeshId = reader.read<u16>();
    bone.boneNameCRC = reader.read<u32>();
    
    visit(bone.translation);
    visit(bone.rotation);
    visit(bone.scale);
    
    bone.pivot = reader.read<Vector3f>();
}

void M2BinaryParseVisitor::visit(Texture& texture) {
    texture.type = reader.read<u32>();
    texture.flags = reader.read<u32>();
    visit(texture.filename);
}

void M2BinaryParseVisitor::visit(Material& material) {
    material.flags = reader.read<u16>();
    material.blendingMode = reader.read<u16>();
}

void M2BinaryParseVisitor::visit(TextureWeight& weight) {
    visit(weight.weight);
}

void M2BinaryParseVisitor::visit(TextureTransform& transform) {
    visit(transform.translation);
    visit(transform.rotation);
    visit(transform.scaling);
}

void M2BinaryParseVisitor::visit(ColorAnimation& color) {
    visit(color.color);
    visit(color.alpha);
}

void M2BinaryParseVisitor::visit(Light& light) {
    light.type = reader.read<u16>();
    light.boneId = reader.read<i16>();
    light.position = reader.read<Vector3f>();
    
    visit(light.ambientColor);
    visit(light.ambientIntensity);
    visit(light.diffuseColor);
    visit(light.diffuseIntensity);
    visit(light.attenuationStart);
    visit(light.attenuationEnd);
    visit(light.visibility);
}

void M2BinaryParseVisitor::visit(CameraSpline& spline) {
    spline.value = reader.read<Vector3f>();
    spline.inTangent = reader.read<Vector3f>();
    spline.outTangent = reader.read<Vector3f>();
}

void M2BinaryParseVisitor::visit(Camera& camera) {
    camera.type = reader.read<u32>();
    if (version < M2_VERSION_CATA) {
        camera.fieldOfView = reader.read<f32>();
    }
    camera.farClip = reader.read<f32>();
    camera.nearClip = reader.read<f32>();
    
    visit(camera.positions);
    
    camera.positionBase = reader.read<Vector3f>();
    
    visit(camera.targetPositions);
    
    camera.targetPositionBase = reader.read<Vector3f>();
    
    visit(camera.roll);
    if (version >= M2_VERSION_CATA) {
        visit(camera.fieldOfViewTrack);
    }
}

void M2BinaryParseVisitor::visit(Attachment& attachment) {
    attachment.id = reader.read<u32>();
    attachment.boneId = reader.read<u16>();
    attachment.unknown = reader.read<u16>();
    attachment.position = reader.read<Vector3f>();
    visit(attachment.animate);
}

void M2BinaryParseVisitor::visit(RibbonEmitter& emitter) {
    emitter.ribbonId = reader.read<u32>();
    emitter.boneId = reader.read<u32>();
    emitter.position = reader.read<Vector3f>();
    
    visit(emitter.textureIndices);
    visit(emitter.materialIndices);
    
    visit(emitter.colorTrack);
    visit(emitter.alphaTrack);
    visit(emitter.heightAbove);
    visit(emitter.heightBelow);
    
    emitter.edgesPerSecond = reader.read<f32>();
    emitter.edgeLifetime = reader.read<f32>();
    emitter.gravity = reader.read<f32>();
    emitter.textureRows = reader.read<u16>();
    emitter.textureCols = reader.read<u16>();
    
    visit(emitter.texSlot);
    visit(emitter.visibility);
    
    emitter.priorityPlane = reader.read<i16>();
    emitter.ribbonColorIndex = reader.read<i8>();
    emitter.textureTransformIndex = reader.read<i8>();
}

void M2BinaryParseVisitor::visit(ParticleEmitter& emitter) {
    emitter.particleId = reader.read<u32>();
    emitter.flags = reader.read<u32>();
    emitter.position = reader.read<Vector3f>();
    emitter.boneId = reader.read<u16>();
    
    visit(emitter.geometryModelFilename);
    visit(emitter.recursionModelFilename);
    
    emitter.blendingType = reader.read<u8>();
    emitter.emitterType = reader.read<u8>();
    emitter.particleColorIndex = reader.read<u16>();
    
    visit(emitter.emissionSpeed);
    visit(emitter.speedVariation);
    visit(emitter.verticalRange);
    visit(emitter.horizontalRange);
    visit(emitter.gravity);
    visit(emitter.lifespan);
    
    emitter.lifespanVary = reader.read<f32>();
    
    visit(emitter.emissionRate);
    
    emitter.emissionRateVary = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(Event& event) {
    event.identifier = reader.read<u32>();
    event.data = reader.read<u32>();
    event.boneId = reader.read<u32>();
    event.position = reader.read<Vector3f>();
    visit(event.enabled);
}

void M2BinaryParseVisitor::visit(MD20Header& header) {
    header.magic = reader.read<u32>();
    header.version = reader.read<u32>();
    version = header.version;
    
    visit(header.modelName);
    visit(header.globalFlags);
    
    visit(header.globalLoops);
    visit(header.sequences);
    visit(header.sequenceIdxHashById);
    
    visit(header.bones);
    visit(header.keyBoneIds);
    
    visit(header.vertices);
    
    if (version >= M2_VERSION_WOTLK) {
        header.numSkinProfiles = reader.read<u32>();
    }
    
    visit(header.colors);
    visit(header.textures);
    visit(header.textureWeights);
    visit(header.textureTransforms);
    visit(header.textureIndicesById);
    visit(header.materials);
    visit(header.boneCombos);
    visit(header.textureCombos);
    visit(header.textureCoordCombos);
    visit(header.textureWeightCombos);
    visit(header.textureTransformCombos);
    header.bounding.minimum = reader.read<Vector3f>();
    header.bounding.maximum = reader.read<Vector3f>();
    header.bounding.sphereRadius = reader.read<f32>();

    header.collision.minimum = reader.read<Vector3f>();
    header.collision.maximum = reader.read<Vector3f>();
    header.collision.sphereRadius = reader.read<f32>();
    
    visit(header.collisionTriangleIndices);
    visit(header.collisionVertices);
    visit(header.collisionFaceNormals);
    
    visit(header.attachments);
    visit(header.attachmentIndicesById);
    visit(header.events);
    visit(header.lights);
    visit(header.cameras);
    visit(header.cameraIndicesById);
    visit(header.ribbonEmitters);
    visit(header.particleEmitters);

    if (hasFlag(header.globalFlags.value, GlobalFlag::UseTextureCombinerCombos)) {
        visit(header.textureCombinerCombos);
    }
}

void M2BinaryParseVisitor::visit(std::string& str) {
    const auto count = reader.read<u32>();
    const auto offset = reader.read<u32>();
    if (count == 0) {
        str.clear();
        return;
    }
    const auto currentPos = reader.getPosition();
    reader.setPosition(offset + baseOffset);
    str = reader.readString(count);
    reader.setPosition(currentPos);
}

void M2BinaryParseVisitor::visit(AnimationTrackBase& track) {
    track.interpolationType = reader.read<u16>();
    track.globalSequenceId = reader.read<u16>();
    visit(track.timestamps);
}

} // namespace m2

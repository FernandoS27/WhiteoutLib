#include "../m2_binary_writer_visitor.h"
#include "../include/common/binary_writer.h"

#include <type_traits>

namespace m2 {

using common::BinaryWriter;

M2BinaryWriterVisitor::M2BinaryWriterVisitor(common::BinaryWriter& writer)
    : writer(writer) {
}

void M2BinaryWriterVisitor::start() {
    baseOffset = writer.getPosition();
}

void M2BinaryWriterVisitor::visit(const GlobalFlags& flags) {
    writer.write(static_cast<u32>(flags.value));
}

void M2BinaryWriterVisitor::visit(const GlobalSequence& seq) {
    writer.write(seq.timestamp);
}

void M2BinaryWriterVisitor::visit(const Sequence& seq) {
    writer.write(seq.id);
    writer.write(seq.variationIndex);
    writer.write(seq.duration);
    writer.write(seq.movespeed);
    writer.write(seq.flags);
    writer.write(seq.frequency);
    writer.write(seq.padding);
    writer.write(seq.replayMin);
    writer.write(seq.replayMax);
    writer.write(seq.blendTimeIn);
    writer.write(seq.blendTimeOut);

    writer.write(seq.bounding.minimum);
    writer.write(seq.bounding.maximum);

    writer.write(seq.bounding.sphereRadius);

    writer.write(seq.variationNext);
    writer.write(seq.aliasNext);
}

void M2BinaryWriterVisitor::visit(const Vertex& vertex) {
    writer.write(vertex.position.x);
    writer.write(vertex.position.y);
    writer.write(vertex.position.z);

    for (int i = 0; i < 4; ++i) {
        writer.write(vertex.boneWeights[i]);
    }
    for (int i = 0; i < 4; ++i) {
        writer.write(vertex.boneIndices[i]);
    }

    writer.write(vertex.normal.x);
    writer.write(vertex.normal.y);
    writer.write(vertex.normal.z);

    for (int i = 0; i < 2; ++i) {
        writer.write(vertex.texCoords[i].x);
        writer.write(vertex.texCoords[i].y);
    }
}

void M2BinaryWriterVisitor::visit(const Bone& bone) {
    writer.write(bone.keyBoneId);
    writer.write(bone.flags);
    writer.write(bone.parentBoneId);
    writer.write(bone.submeshId);
    writer.write(bone.boneNameCRC);

    visit(bone.translation);
    visit(bone.rotation);
    visit(bone.scale);

    writer.write(bone.pivot.x);
    writer.write(bone.pivot.y);
    writer.write(bone.pivot.z);
}

void M2BinaryWriterVisitor::visit(const Texture& texture) {
    writer.write(texture.type);
    writer.write(texture.flags);
    visit(texture.filename);
}

void M2BinaryWriterVisitor::visit(const Material& material) {
    writer.write(material.flags);
    writer.write(material.blendingMode);
}

void M2BinaryWriterVisitor::visit(const TextureWeight& weight) {
    visit(weight.weight);
}

void M2BinaryWriterVisitor::visit(const TextureTransform& transform) {
    visit(transform.translation);
    visit(transform.rotation);
    visit(transform.scaling);
}

void M2BinaryWriterVisitor::visit(const ColorAnimation& color) {
    visit(color.color);
    visit(color.alpha);
}

void M2BinaryWriterVisitor::visit(const Light& light) {
    writer.write(light.type);
    writer.write(light.boneId);
    writer.write(light.position.x);
    writer.write(light.position.y);
    writer.write(light.position.z);

    visit(light.ambientColor);
    visit(light.ambientIntensity);
    visit(light.diffuseColor);
    visit(light.diffuseIntensity);
    visit(light.attenuationStart);
    visit(light.attenuationEnd);
    visit(light.visibility);
}

void M2BinaryWriterVisitor::visit(const CameraSpline& spline) {
    writer.write(spline.value.x);
    writer.write(spline.value.y);
    writer.write(spline.value.z);
    writer.write(spline.inTangent.x);
    writer.write(spline.inTangent.y);
    writer.write(spline.inTangent.z);
    writer.write(spline.outTangent.x);
    writer.write(spline.outTangent.y);
    writer.write(spline.outTangent.z);
}

void M2BinaryWriterVisitor::visit(const Camera& camera) {
    writer.write(camera.type);
    if (version < M2_VERSION_CATA) {
        writer.write(camera.fieldOfView);
    }
    writer.write(camera.farClip);
    writer.write(camera.nearClip);

    visit(camera.positions);

    writer.write(camera.positionBase.x);
    writer.write(camera.positionBase.y);
    writer.write(camera.positionBase.z);

    visit(camera.targetPositions);

    writer.write(camera.targetPositionBase.x);
    writer.write(camera.targetPositionBase.y);
    writer.write(camera.targetPositionBase.z);

    visit(camera.roll);
    if (version >= M2_VERSION_CATA) {
        visit(camera.fieldOfViewTrack);
    }
}

void M2BinaryWriterVisitor::visit(const Attachment& attachment) {
    writer.write(attachment.id);
    writer.write(attachment.boneId);
    writer.write(attachment.unknown);
    writer.write(attachment.position.x);
    writer.write(attachment.position.y);
    writer.write(attachment.position.z);
    visit(attachment.animate);
}

void M2BinaryWriterVisitor::visit(const RibbonEmitter& emitter) {
    writer.write(emitter.ribbonId);
    writer.write(emitter.boneId);
    writer.write(emitter.position.x);
    writer.write(emitter.position.y);
    writer.write(emitter.position.z);

    visit(emitter.textureIndices);
    visit(emitter.materialIndices);

    visit(emitter.colorTrack);
    visit(emitter.alphaTrack);
    visit(emitter.heightAbove);
    visit(emitter.heightBelow);

    writer.write(emitter.edgesPerSecond);
    writer.write(emitter.edgeLifetime);
    writer.write(emitter.gravity);
    writer.write(emitter.textureRows);
    writer.write(emitter.textureCols);

    visit(emitter.texSlot);
    visit(emitter.visibility);

    writer.write(emitter.priorityPlane);
    writer.write(emitter.ribbonColorIndex);
    writer.write(emitter.textureTransformIndex);
}

void M2BinaryWriterVisitor::visit(const ParticleEmitter& emitter) {
    writer.write(emitter.particleId);
    writer.write(emitter.flags);
    writer.write(emitter.position.x);
    writer.write(emitter.position.y);
    writer.write(emitter.position.z);
    writer.write(emitter.boneId);

    visit(emitter.geometryModelFilename);
    visit(emitter.recursionModelFilename);

    writer.write(emitter.blendingType);
    writer.write(emitter.emitterType);
    writer.write(emitter.particleColorIndex);

    visit(emitter.emissionSpeed);
    visit(emitter.speedVariation);
    visit(emitter.verticalRange);
    visit(emitter.horizontalRange);
    visit(emitter.gravity);
    visit(emitter.lifespan);

    writer.write(emitter.lifespanVary);

    visit(emitter.emissionRate);

    writer.write(emitter.emissionRateVary);
}

void M2BinaryWriterVisitor::visit(const Event& event) {
    writer.write(event.identifier);
    writer.write(event.data);
    writer.write(event.boneId);
    writer.write(event.position.x);
    writer.write(event.position.y);
    writer.write(event.position.z);
    visit(event.enabled);
}

void M2BinaryWriterVisitor::visit(const MD20Header& header) {
    version = header.version;

    writer.write(header.magic);
    writer.write(header.version);

    visit(header.modelName);
    visit(header.globalFlags);

    visit(header.globalLoops);
    visit(header.sequences);
    visit(header.sequenceIdxHashById);

    visit(header.bones);
    visit(header.keyBoneIds);

    visit(header.vertices);

    if (header.version >= M2_VERSION_WOTLK) {
        writer.write(header.numSkinProfiles);
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

    writer.write(header.bounding.minimum);
    writer.write(header.bounding.maximum);
    writer.write(header.bounding.sphereRadius);

    writer.write(header.collision.minimum);
    writer.write(header.collision.maximum);
    writer.write(header.collision.sphereRadius);

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

void M2BinaryWriterVisitor::visit(const AnimationTrackBase& track) {
    writer.write(track.interpolationType);
    writer.write(track.globalSequenceId);
    visit(track.timestamps);
}

void M2BinaryWriterVisitor::visit(const std::string& str) {
    writer.write<u32>(static_cast<u32>(str.size()));
    u32 offsetPos = writer.getPosition();
    writer.write<u32>(0); // placeholder for offset
    if (str.empty()) {
        return; // No data to write, so we can skip the deferred write
    }

    // Defer writing the string data until after we've written the count and reserved space for the offset
    deferredWrites.push_back([this, offsetPos, &str]() {
        u32 currentPos = writer.getPosition();
        writer.setPosition(offsetPos);
        writer.write(currentPos - baseOffset); // Write the actual offset (relative to base)
        writer.setPosition(currentPos);
        writer.writeString(str);
        writer.AlignTo(16); // Align to 16 bytes after writing the string
    });
}

} // namespace m2

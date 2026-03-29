
#include "../../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

#include <type_traits>

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

BinaryWriterVisitor::BinaryWriterVisitor(common::BinaryWriter& writer) : writer(writer) {}

void BinaryWriterVisitor::start() {
    baseOffset = writer.getPosition();
}

void BinaryWriterVisitor::visit(const GlobalFlags& flags) {
    writer.write(static_cast<u32>(flags.value));
}

void BinaryWriterVisitor::visit(const GlobalSequence& seq) {
    writer.write(seq.timestamp);
}

void BinaryWriterVisitor::visit(const Sequence& seq) {
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

    if (!hasFlag(seq.flags, SequenceFlag::StoredAnimated)) {
        animDataBuffers.push_back(AnimDataBuffer{seq.id, seq.variationIndex, {}});
        auto& animBuffer = animDataBuffers.back();
        animDataWriterStates.push_back({});
        auto& writerState = animDataWriterStates.back();
        writerState.streambuf = std::make_unique<common::vector_streambuf>(animBuffer.data);
        writerState.stream = std::make_unique<std::ostream>(writerState.streambuf.get());
        writerState.writer = std::make_unique<common::BinaryWriter>(*writerState.stream);
        animDataWriters.push_back(writerState.writer.get());
    } else {
        animDataWriters.push_back(&writer);
    }
}

void BinaryWriterVisitor::visit(const Vertex& vertex) {
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

void BinaryWriterVisitor::visit(const Bone& bone) {
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

void BinaryWriterVisitor::visit(const Texture& texture) {
    writer.write(texture.type);
    writer.write(texture.flags);
    visit(texture.filename);
}

void BinaryWriterVisitor::visit(const Material& material) {
    writer.write(material.flags);
    writer.write(material.blendingMode);
}

void BinaryWriterVisitor::visit(const TextureWeight& weight) {
    visit(weight.weight);
}

void BinaryWriterVisitor::visit(const TextureTransform& transform) {
    visit(transform.translation);
    visit(transform.rotation);
    visit(transform.scaling);
}

void BinaryWriterVisitor::visit(const ColorAnimation& color) {
    visit(color.color);
    visit(color.alpha);
}

void BinaryWriterVisitor::visit(const Light& light) {
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

void BinaryWriterVisitor::visit(const CameraSpline& spline) {
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

void BinaryWriterVisitor::visit(const Camera& camera) {
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

void BinaryWriterVisitor::visit(const Attachment& attachment) {
    writer.write(attachment.id);
    writer.write(attachment.boneId);
    writer.write(attachment.unknown);
    writer.write(attachment.position.x);
    writer.write(attachment.position.y);
    writer.write(attachment.position.z);
    visit(attachment.animate);
}

void BinaryWriterVisitor::visit(const RibbonEmitter& emitter) {
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

void BinaryWriterVisitor::visit(const ParticleEmitter& emitter) {
    writer.write(emitter.particleId);
    writer.write(emitter.flags);
    writer.write(emitter.position.x);
    writer.write(emitter.position.y);
    writer.write(emitter.position.z);
    writer.write(emitter.boneId);
    writer.write(emitter.textureId);

    visit(emitter.particleModelFilename);
    visit(emitter.childEmittersModelFilename);

    writer.write(emitter.blendingType);
    writer.write(emitter.emitterType);
    writer.write(emitter.particleColorIndex);

    writer.write(emitter.multiTexScale[0]);
    writer.write(emitter.multiTexScale[1]);
    writer.write(emitter.textureTilerotation);
    writer.write(emitter.rows);
    writer.write(emitter.columns);

    visit(emitter.emissionSpeed);
    visit(emitter.speedVariation);
    visit(emitter.verticalRange);
    visit(emitter.horizontalRange);
    visit(emitter.gravity);
    visit(emitter.lifespan);

    writer.write(emitter.lifespanVariation);

    visit(emitter.emissionRate);

    writer.write(emitter.emissionRateVariation);

    visit(emitter.emissionAreaWidth);
    visit(emitter.emissionAreaLength);
    visit(emitter.zSource);

    visit(emitter.colorTrack);
    visit(emitter.alphaTrack);
    visit(emitter.scaleTrack);
    writer.write(emitter.scaleVary);
    visit(emitter.headUVScroll);
    visit(emitter.tailUVScroll);

    writer.write(emitter.tailLength);
    writer.write(emitter.twinkleSpeed);
    writer.write(emitter.twinklePercent);
    writer.write(emitter.twinkleScale.x);
    writer.write(emitter.twinkleScale.y);
    writer.write(emitter.inheritVelocityScale);
    writer.write(emitter.drag);
    writer.write(emitter.baseSpin);
    writer.write(emitter.baseSpinVariation);
    writer.write(emitter.spinSpeed);
    writer.write(emitter.spinSpeedVariation);

    writer.write(emitter.tumble.minimum);
    writer.write(emitter.tumble.maximum);
    writer.write(emitter.windVector);
    writer.write(emitter.windTime);

    writer.write(emitter.followSpeed1);
    writer.write(emitter.followScale1);
    writer.write(emitter.followSpeed2);
    writer.write(emitter.followScale2);

    visit(emitter.splinePoints);
    visit(emitter.enabledIn);

    const bool extendedParticle =
        (version > 271) || hasFlag(globalFlags, GlobalFlag::NewParticleRecord);
    if (extendedParticle) {
        writer.write(emitter.multiTexScrollMid[0][0]);
        writer.write(emitter.multiTexScrollMid[0][1]);
        writer.write(emitter.multiTexScrollMid[1][0]);
        writer.write(emitter.multiTexScrollMid[1][1]);
        writer.write(emitter.multiTexScrollRange[0][0]);
        writer.write(emitter.multiTexScrollRange[0][1]);
        writer.write(emitter.multiTexScrollRange[1][0]);
        writer.write(emitter.multiTexScrollRange[1][1]);
    }
}

void BinaryWriterVisitor::visit(const Event& event) {
    writer.write(event.identifier);
    writer.write(event.data);
    writer.write(event.boneId);
    writer.write(event.position.x);
    writer.write(event.position.y);
    writer.write(event.position.z);
    visit(event.enabled);
}

void BinaryWriterVisitor::visit(const MD20Header& md20) {
    version = md20.version;
    const auto& header = md20.model;

    writer.write(md20.magic);
    writer.write(md20.version);

    visit(md20.model);
}

void BinaryWriterVisitor::visit(const Model& model) {
    visit(model.modelName);
    visit(model.globalFlags);
    globalFlags = model.globalFlags.value;

    visit(model.globalLoops);
    visit(model.sequences);
    visit(model.sequenceIdxHashById);

    visit(model.bones);
    visit(model.keyBoneIds);

    visit(model.vertices);

    if (version >= M2_VERSION_WOTLK) {
        writer.write(model.numSkinProfiles);
    }

    visit(model.colors);
    visit(model.textures);
    visit(model.textureWeights);
    visit(model.textureTransforms);
    visit(model.textureIndicesById);
    visit(model.materials);
    visit(model.boneCombos);
    visit(model.textureCombos);
    visit(model.textureCoordCombos);
    visit(model.textureWeightCombos);
    visit(model.textureTransformCombos);

    writer.write(model.bounding.minimum);
    writer.write(model.bounding.maximum);
    writer.write(model.bounding.sphereRadius);

    writer.write(model.collision.minimum);
    writer.write(model.collision.maximum);
    writer.write(model.collision.sphereRadius);

    visit(model.collisionTriangleIndices);
    visit(model.collisionVertices);
    visit(model.collisionFaceNormals);

    visit(model.attachments);
    visit(model.attachmentIndicesById);
    visit(model.events);
    visit(model.lights);
    visit(model.cameras);
    visit(model.cameraIndicesById);
    visit(model.ribbonEmitters);
    visit(model.particleEmitters);
    if (hasFlag(model.globalFlags.value, GlobalFlag::UseTextureCombinerCombos)) {
        visit(model.textureCombinerCombos);
    }
}

void BinaryWriterVisitor::visit(const AnimationTrackBase& track) {
    writer.write(track.interpolationType);
    writer.write(track.globalSequenceId);
    visit(track.timestamps);
}

void BinaryWriterVisitor::visit(const std::string& str) {
    writer.write<u32>(static_cast<u32>(str.size()));
    u32 offsetPos = writer.getPosition();
    writer.write<u32>(0);
    if (str.empty()) {
        return;
    }

    deferredWrites.push_back([this, offsetPos, &str]() {
        u32 currentPos = writer.getPosition();
        writer.setPosition(offsetPos);
        writer.write(currentPos - baseOffset);
        writer.setPosition(currentPos);
        writer.writeString(str);
        writer.AlignTo(16);
    });
}

} // namespace m2
} // namespace whiteout

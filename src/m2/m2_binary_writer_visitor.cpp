#include "m2_binary_writer_visitor.h"
#include "../include/common/binary_writer.h"

#include <type_traits>

namespace m2 {

M2BinaryWriterVisitor::M2BinaryWriterVisitor(common::BinaryWriter& writer) 
    : writer(writer) {
    }

using common::BinaryWriter;

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
    
    writer.write(seq.boundingBox.minimum);
    writer.write(seq.boundingBox.maximum);

    writer.write(seq.boundingSphereRadius);
    
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
    

    writer.write(header.boundingBox.minimum);
    writer.write(header.boundingBox.maximum);
    writer.write(header.boundingSphereRadius);
    
    writer.write(header.collisionBox.minimum);
    writer.write(header.collisionBox.maximum);
    writer.write(header.collisionSphereRadius);
    
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

template<typename T>
void M2BinaryWriterVisitor::visit(const AnimationTrack<T>& track) {
    writer.write(track.interpolationType);
    writer.write(track.globalSequenceId);
    visit(track.timestamps);
    visit(track.values);
}

template<typename T>
void M2BinaryWriterVisitor::visit(const std::vector<T>& array) {
    writer.write<u32>(static_cast<u32>(array.size()));
    u32 offsetPos = writer.getPosition();
    writer.write<u32>(0); // placeholder for offset
    if (array.empty()) {
        return; // No data to write, so we can skip the deferred write
    }

    // Defer writing the array data until after we've written the count and reserved space for the offset
    deferredWrites.push_back([this, offsetPos, &array]() {
        u32 currentPos = writer.getPosition();
        writer.setPosition(offsetPos);
        writer.write(currentPos - baseOffset); // Write the actual offset (relative to base)
        writer.setPosition(currentPos);
        if constexpr (std::is_trivially_copyable_v<T>) {
            writer.write(array);
        } else {
            for (const T& item : array) {
                visit(item);
            }
        }
        writer.AlignTo(16); // Align to 16 bytes after writing the array
    });
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

// Chunk structure visit implementations
void M2BinaryWriterVisitor::visit(const M2TXACChunk& chunk) {
    writer.write(chunk.unknown);
}

void M2BinaryWriterVisitor::visit(const M2PFIDChunk& chunk) {
    writer.write(chunk.physFileDataId);
}

void M2BinaryWriterVisitor::visit(const M2SFIDChunk& chunk) {
    writer.write(chunk.skinFileDataIds);
    writer.write(chunk.lodSkinFileDataIds);
}

void M2BinaryWriterVisitor::visit(const M2AFIDEntry& entry) {
    writer.write(entry.animId);
    writer.write(entry.subAnimId);
    writer.write(entry.fileDataId);
}

void M2BinaryWriterVisitor::visit(const M2AFIDChunk& chunk) {
    writer.write(chunk.animFileIds);
}

void M2BinaryWriterVisitor::visit(const M2BFIDChunk& chunk) {
    writer.write(chunk.boneFileDataIds);
}

void M2BinaryWriterVisitor::visit(const M2EXPTEntry& entry) {
    writer.write(entry.zSource);
    writer.write(entry.colorMult);
    writer.write(entry.alphaMult);
}

void M2BinaryWriterVisitor::visit(const M2EXPTChunk& chunk) {
    visit(chunk.extendedParticles);
}

void M2BinaryWriterVisitor::visit(const M2EXP2Particle& particle) {
    writer.write(particle.zSource);
    writer.write(particle.colorMult);
    writer.write(particle.alphaMult);
    visit(particle.alphaCutoff);
}

void M2BinaryWriterVisitor::visit(const M2EXP2Chunk& chunk) {
    visit(chunk.content);
}

void M2BinaryWriterVisitor::visit(const M2PABCChunk& chunk) {
    visit(chunk.replacementParentSequenceLookups);
}

void M2BinaryWriterVisitor::visit(const M2PADCChunk& chunk) {
    visit(chunk.textureWeights);
}

void M2BinaryWriterVisitor::visit(const M2SequenceBounds& bounds) {
    writer.write(bounds);
}

void M2BinaryWriterVisitor::visit(const M2PSBCChunk& chunk) {
    visit(chunk.parentSequenceBounds);
}

void M2BinaryWriterVisitor::visit(const M2PEDCChunk& chunk) {
    visit(chunk.parentEventData);
}

void M2BinaryWriterVisitor::visit(const M2SKIDChunk& chunk) {
    writer.write(chunk.skeletonFileDataId);
}

void M2BinaryWriterVisitor::visit(const M2TXIDEntry& entry) {
    writer.write(entry.fileDataId);
}

void M2BinaryWriterVisitor::visit(const M2TXIDChunk& chunk) {
    visit(chunk.textureIds);
}

void M2BinaryWriterVisitor::visit(const M2LDV1Chunk& chunk) {
    writer.write(chunk.unknown0);
    writer.write(chunk.lodCount);
    writer.write(chunk.unknown2);
    writer.write(chunk.particleBoneLod);
    writer.write(chunk.unknown4);
}

void M2BinaryWriterVisitor::visit(const M2RPIDEntry& entry) {
    writer.write(entry.fileDataId);
}

void M2BinaryWriterVisitor::visit(const M2RPIDChunk& chunk) {
    visit(chunk.recursiveParticleModels);
}

void M2BinaryWriterVisitor::visit(const M2GPIDEntry& entry) {
    writer.write(entry.fileDataId);
}

void M2BinaryWriterVisitor::visit(const M2GPIDChunk& chunk) {
    visit(chunk.geometryParticleModels);
}

void M2BinaryWriterVisitor::visit(const M2WFV1Chunk& chunk) {
    // Empty chunk
}

void M2BinaryWriterVisitor::visit(const M2WFV2Chunk& chunk) {
    // Empty chunk
}

void M2BinaryWriterVisitor::visit(const M2PGD1Entry& entry) {
    writer.write(entry.geoset);
}

void M2BinaryWriterVisitor::visit(const M2PGD1Chunk& chunk) {
    visit(chunk.particleGeosetData);
}

void M2BinaryWriterVisitor::visit(const M2WFV3Data& data) {
    writer.write(data.bumpScale);
    writer.write(data.value0_x);
    writer.write(data.value0_y);
    writer.write(data.value0_z);
    writer.write(data.value1_w);
    writer.write(data.value0_w);
    writer.write(data.value1_x);
    writer.write(data.value1_y);
    writer.write(data.value2_w);
    writer.write(data.value3_y);
    writer.write(data.value3_x);
    writer.write(data.baseColor);
    writer.write(data.flags);
    writer.write(data.unknown0);
    writer.write(data.value3_w);
    writer.write(data.value3_z);
    writer.write(data.value4_y);
    writer.write(data.unknown1);
    writer.write(data.unknown2);
    writer.write(data.unknown3);
    writer.write(data.unknown4);
}

void M2BinaryWriterVisitor::visit(const M2WFV3Chunk& chunk) {
    visit(chunk.data);
}

void M2BinaryWriterVisitor::visit(const M2PFDCChunk& chunk) {
    writer.write(chunk.physicsData);
}

void M2BinaryWriterVisitor::visit(const M2EDGFEntry& entry) {
    writer.write(entry.value0);
    writer.write(entry.value8);
    writer.write(entry.valueC);
}

void M2BinaryWriterVisitor::visit(const M2EDGFChunk& chunk) {
    visit(chunk.entries);
}

void M2BinaryWriterVisitor::visit(const M2NERFEntry& entry) {
    writer.write(entry.coefs);
}

void M2BinaryWriterVisitor::visit(const M2NERFChunk& chunk) {
    visit(chunk.entries);
}

void M2BinaryWriterVisitor::visit(const M2DETLEntry& entry) {
    writer.write(entry.flags);
    writer.write(entry.scale);
    writer.write(entry.diffuseColorMultiplier);
    writer.write(entry.unknown0);
    writer.write(entry.unknown1);
}

void M2BinaryWriterVisitor::visit(const M2DETLChunk& chunk) {
    visit(chunk.records);
}

void M2BinaryWriterVisitor::visit(const M2DBOCEntry& entry) {
    writer.write(entry.unknown1_1);
    writer.write(entry.unknown1_2);
    writer.write(entry.unknown1_3);
    writer.write(entry.unknown1_4);
}

void M2BinaryWriterVisitor::visit(const M2DBOCChunk& chunk) {
    visit(chunk.entries);
}

void M2BinaryWriterVisitor::visit(const M2AFRAChunk& chunk) {
    writer.write(chunk.data);
}

void M2BinaryWriterVisitor::visit(const M2PCOLChunk& chunk) {
    visit(chunk.vertexPositions);
    visit(chunk.faceNormals);
    visit(chunk.indices);
    visit(chunk.flags);
}

void M2BinaryWriterVisitor::visit(const M2DPIVChunk& chunk) {
    writer.write(chunk.data);
}

void M2BinaryWriterVisitor::visit(const M2TEXLEntry& entry) {
    writer.write(entry.unknown0);
    writer.write(entry.unknown1);
    writer.write(entry.textureLookup);
    writer.write(entry.unknown2);
}

void M2BinaryWriterVisitor::visit(const M2TEXLChunk& chunk) {
    visit(chunk.texturedLights);
}

} // namespace m2

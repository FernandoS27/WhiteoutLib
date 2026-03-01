#include "../m2_binary_writer_visitor.h"
#include "../include/common/binary_writer.h"

namespace m2 {

using common::BinaryWriter;

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
    writer.write(chunk.textureIds);
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
    writer.write(chunk.recursiveParticleModels);
}

void M2BinaryWriterVisitor::visit(const M2GPIDEntry& entry) {
    writer.write(entry.fileDataId);
}

void M2BinaryWriterVisitor::visit(const M2GPIDChunk& chunk) {
    writer.write(chunk.geometryParticleModels);
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

void M2BinaryWriterVisitor::visit(const M2SKL1Chunk& chunk) {
    writer.write(chunk.flags);
    visit(chunk.name);
    writer.write(chunk.reserved);
}

void M2BinaryWriterVisitor::visit(const M2SKA1Chunk& chunk) {
    visit(chunk.attachments);
    visit(chunk.attachmentLookupTable);
}

void M2BinaryWriterVisitor::visit(const M2SKB1Chunk& chunk) {
    visit(chunk.bones);
    visit(chunk.keyBoneLookup);
}

void M2BinaryWriterVisitor::visit(const M2SKS1Chunk& chunk) {
    visit(chunk.globalLoops);
    visit(chunk.sequences);
    visit(chunk.sequenceLookups);
    writer.write(chunk.reserved);
}

void M2BinaryWriterVisitor::visit(const M2SKPDChunk& chunk) {
    writer.write(chunk.reserved0);
    writer.write(chunk.parentSkeletonFileId);
    writer.write(chunk.reserved1);
}

} // namespace m2

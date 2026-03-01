// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

// Chunk structure visit implementations
void BinaryWriterVisitor::visit(const TXACChunk& chunk) {
    writer.write(chunk.unknown);
}

void BinaryWriterVisitor::visit(const PFIDChunk& chunk) {
    writer.write(chunk.physFileDataId);
}

void BinaryWriterVisitor::visit(const SFIDChunk& chunk) {
    writer.write(chunk.skinFileDataIds);
    writer.write(chunk.lodSkinFileDataIds);
}

void BinaryWriterVisitor::visit(const AFIDEntry& entry) {
    writer.write(entry.animId);
    writer.write(entry.subAnimId);
    writer.write(entry.fileDataId);
}

void BinaryWriterVisitor::visit(const AFIDChunk& chunk) {
    writer.write(chunk.animFileIds);
}

void BinaryWriterVisitor::visit(const BFIDChunk& chunk) {
    writer.write(chunk.boneFileDataIds);
}

void BinaryWriterVisitor::visit(const EXPTEntry& entry) {
    writer.write(entry.zSource);
    writer.write(entry.colorMult);
    writer.write(entry.alphaMult);
}

void BinaryWriterVisitor::visit(const EXPTChunk& chunk) {
    visit(chunk.extendedParticles);
}

void BinaryWriterVisitor::visit(const EXP2Particle& particle) {
    writer.write(particle.zSource);
    writer.write(particle.colorMult);
    writer.write(particle.alphaMult);
    visit(particle.alphaCutoff);
}

void BinaryWriterVisitor::visit(const EXP2Chunk& chunk) {
    visit(chunk.content);
}

void BinaryWriterVisitor::visit(const PABCChunk& chunk) {
    visit(chunk.replacementParentSequenceLookups);
}

void BinaryWriterVisitor::visit(const PADCChunk& chunk) {
    visit(chunk.textureWeights);
}

void BinaryWriterVisitor::visit(const SequenceBounds& bounds) {
    writer.write(bounds);
}

void BinaryWriterVisitor::visit(const PSBCChunk& chunk) {
    visit(chunk.parentSequenceBounds);
}

void BinaryWriterVisitor::visit(const PEDCChunk& chunk) {
    visit(chunk.parentEventData);
}

void BinaryWriterVisitor::visit(const SKIDChunk& chunk) {
    writer.write(chunk.skeletonFileDataId);
}

void BinaryWriterVisitor::visit(const TXIDEntry& entry) {
    writer.write(entry.fileDataId);
}

void BinaryWriterVisitor::visit(const TXIDChunk& chunk) {
    writer.write(chunk.textureIds);
}

void BinaryWriterVisitor::visit(const LDV1Chunk& chunk) {
    writer.write(chunk.unknown0);
    writer.write(chunk.lodCount);
    writer.write(chunk.unknown2);
    writer.write(chunk.particleBoneLod);
    writer.write(chunk.unknown4);
}

void BinaryWriterVisitor::visit(const M2RPIDEntry& entry) {
    writer.write(entry.fileDataId);
}

void BinaryWriterVisitor::visit(const M2RPIDChunk& chunk) {
    writer.write(chunk.recursiveParticleModels);
}

void BinaryWriterVisitor::visit(const GPIDEntry& entry) {
    writer.write(entry.fileDataId);
}

void BinaryWriterVisitor::visit(const GPIDChunk& chunk) {
    writer.write(chunk.geometryParticleModels);
}

void BinaryWriterVisitor::visit(const WFV1Chunk& chunk) {
    // Empty chunk
}

void BinaryWriterVisitor::visit(const WFV2Chunk& chunk) {
    // Empty chunk
}

void BinaryWriterVisitor::visit(const PGD1Entry& entry) {
    writer.write(entry.geoset);
}

void BinaryWriterVisitor::visit(const PGD1Chunk& chunk) {
    visit(chunk.particleGeosetData);
}

void BinaryWriterVisitor::visit(const WFV3Data& data) {
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

void BinaryWriterVisitor::visit(const WFV3Chunk& chunk) {
    visit(chunk.data);
}

void BinaryWriterVisitor::visit(const PFDCChunk& chunk) {
    writer.write(chunk.physicsData);
}

void BinaryWriterVisitor::visit(const EDGFEntry& entry) {
    writer.write(entry.value0);
    writer.write(entry.value8);
    writer.write(entry.valueC);
}

void BinaryWriterVisitor::visit(const EDGFChunk& chunk) {
    visit(chunk.entries);
}

void BinaryWriterVisitor::visit(const NERFEntry& entry) {
    writer.write(entry.coefs);
}

void BinaryWriterVisitor::visit(const NERFChunk& chunk) {
    visit(chunk.entries);
}

void BinaryWriterVisitor::visit(const DETLEntry& entry) {
    writer.write(entry.flags);
    writer.write(entry.scale);
    writer.write(entry.diffuseColorMultiplier);
    writer.write(entry.unknown0);
    writer.write(entry.unknown1);
}

void BinaryWriterVisitor::visit(const DETLChunk& chunk) {
    visit(chunk.records);
}

void BinaryWriterVisitor::visit(const DBOCEntry& entry) {
    writer.write(entry.unknown1_1);
    writer.write(entry.unknown1_2);
    writer.write(entry.unknown1_3);
    writer.write(entry.unknown1_4);
}

void BinaryWriterVisitor::visit(const DBOCChunk& chunk) {
    visit(chunk.entries);
}

void BinaryWriterVisitor::visit(const AFRAChunk& chunk) {
    writer.write(chunk.data);
}

void BinaryWriterVisitor::visit(const PCOLChunk& chunk) {
    visit(chunk.vertexPositions);
    visit(chunk.faceNormals);
    visit(chunk.indices);
    visit(chunk.flags);
}

void BinaryWriterVisitor::visit(const DPIVChunk& chunk) {
    writer.write(chunk.data);
}

void BinaryWriterVisitor::visit(const TEXLEntry& entry) {
    writer.write(entry.unknown0);
    writer.write(entry.unknown1);
    writer.write(entry.textureLookup);
    writer.write(entry.unknown2);
}

void BinaryWriterVisitor::visit(const TEXLChunk& chunk) {
    visit(chunk.texturedLights);
}

void BinaryWriterVisitor::visit(const SKL1Chunk& chunk) {
    writer.write(chunk.flags);
    visit(chunk.name);
    writer.write(chunk.reserved);
}

void BinaryWriterVisitor::visit(const SKA1Chunk& chunk) {
    visit(chunk.attachments);
    visit(chunk.attachmentLookupTable);
}

void BinaryWriterVisitor::visit(const SKB1Chunk& chunk) {
    visit(chunk.bones);
    visit(chunk.keyBoneLookup);
}

void BinaryWriterVisitor::visit(const SKS1Chunk& chunk) {
    visit(chunk.globalLoops);
    visit(chunk.sequences);
    visit(chunk.sequenceLookups);
    writer.write(chunk.reserved);
}

void BinaryWriterVisitor::visit(const SKPDChunk& chunk) {
    writer.write(chunk.reserved0);
    writer.write(chunk.parentSkeletonFileId);
    writer.write(chunk.reserved1);
}

} // namespace m2
} // namespace whiteout

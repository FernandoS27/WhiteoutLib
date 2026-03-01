// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

// Chunk structure visit implementations
void BinaryParseVisitor::visit(TXACChunk& chunk, BaseFile& file) {
    size_t num_entries = file.header.materials.size() + file.header.particleEmitters.size();
    chunk.unknown = reader.read<std::vector<std::array<u8, 2>>>(num_entries);
}

void BinaryParseVisitor::visit(PFIDChunk& chunk, BaseFile& file) {
    chunk.physFileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(SFIDChunk& chunk, BaseFile& file) {
    chunk.skinFileDataIds = reader.read<std::vector<u32>>(file.header.numSkinProfiles);
    u32 remainingBytes = maxSize - chunk.skinFileDataIds.size() * sizeof(u32);
    if (remainingBytes > 0) {
        chunk.lodSkinFileDataIds = reader.read<std::vector<u32>>(remainingBytes / sizeof(u32));
    }
}

void BinaryParseVisitor::visit(AFIDEntry& entry) {
    entry.animId = reader.read<u16>();
    entry.subAnimId = reader.read<u16>();
    entry.fileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(AFIDChunk& chunk) {
    parse_chunked_vector(chunk.animFileIds);
}

void BinaryParseVisitor::visit(BFIDChunk& chunk) {
    parse_chunked_vector(chunk.boneFileDataIds);
}

void BinaryParseVisitor::visit(EXPTEntry& entry) {
    entry.zSource = reader.read<f32>();
    entry.colorMult = reader.read<f32>();
    entry.alphaMult = reader.read<f32>();
}

void BinaryParseVisitor::visit(EXPTChunk& chunk) {
    visit(chunk.extendedParticles);
}

void BinaryParseVisitor::visit(EXP2Particle& particle) {
    particle.zSource = reader.read<f32>();
    particle.colorMult = reader.read<f32>();
    particle.alphaMult = reader.read<f32>();
    visit(particle.alphaCutoff);
}

void BinaryParseVisitor::visit(EXP2Chunk& chunk) {
    visit(chunk.content);
}

void BinaryParseVisitor::visit(PABCChunk& chunk) {
    visit(chunk.replacementParentSequenceLookups);
}

void BinaryParseVisitor::visit(PADCChunk& chunk) {
    visit(chunk.textureWeights);
}

void BinaryParseVisitor::visit(SequenceBounds& bounds) {
    bounds.extent.minimum = reader.read<Vector3f>();
    bounds.extent.maximum = reader.read<Vector3f>();
    bounds.radius = reader.read<f32>();
}

void BinaryParseVisitor::visit(PSBCChunk& chunk) {
    visit(chunk.parentSequenceBounds);
}

void BinaryParseVisitor::visit(PEDCChunk& chunk) {
    visit(chunk.parentEventData);
}

void BinaryParseVisitor::visit(SKIDChunk& chunk) {
    chunk.skeletonFileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(TXIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(TXIDChunk& chunk) {
    size_t entryCount = maxSize / sizeof(TXIDEntry);
    chunk.textureIds = reader.read<std::vector<TXIDEntry>>(entryCount);
}

void BinaryParseVisitor::visit(LDV1Chunk& chunk) {
    chunk.unknown0 = reader.read<u16>();
    chunk.lodCount = reader.read<u16>();
    chunk.unknown2 = reader.read<f32>();
    chunk.particleBoneLod = reader.readArray<u8, 4>();
    chunk.unknown4 = reader.read<u32>();
}

void BinaryParseVisitor::visit(M2RPIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(M2RPIDChunk& chunk) {
    size_t entryCount = maxSize / sizeof(M2RPIDEntry);
    chunk.recursiveParticleModels = reader.read<std::vector<M2RPIDEntry>>(entryCount);
}

void BinaryParseVisitor::visit(GPIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(GPIDChunk& chunk) {
    size_t entryCount = maxSize / sizeof(GPIDEntry);
    chunk.geometryParticleModels = reader.read<std::vector<GPIDEntry>>(entryCount);
}

void BinaryParseVisitor::visit(WFV1Chunk& chunk) {
    // Empty chunk
}

void BinaryParseVisitor::visit(WFV2Chunk& chunk) {
    // Empty chunk
}

void BinaryParseVisitor::visit(PGD1Entry& entry) {
    entry.geoset = reader.read<u16>();
}

void BinaryParseVisitor::visit(PGD1Chunk& chunk) {
    visit(chunk.particleGeosetData);
}

void BinaryParseVisitor::visit(WFV3Data& data) {
    data.bumpScale = reader.read<f32>();
    data.value0_x = reader.read<f32>();
    data.value0_y = reader.read<f32>();
    data.value0_z = reader.read<f32>();
    data.value1_w = reader.read<f32>();
    data.value0_w = reader.read<f32>();
    data.value1_x = reader.read<f32>();
    data.value1_y = reader.read<f32>();
    data.value2_w = reader.read<f32>();
    data.value3_y = reader.read<f32>();
    data.value3_x = reader.read<f32>();
    data.baseColor = reader.read<Vector4f>();
    data.flags = reader.read<u16>();
    data.unknown0 = reader.read<u16>();
    data.value3_w = reader.read<f32>();
    data.value3_z = reader.read<f32>();
    data.value4_y = reader.read<f32>();
    data.unknown1 = reader.read<f32>();
    data.unknown2 = reader.read<f32>();
    data.unknown3 = reader.read<f32>();
    data.unknown4 = reader.read<f32>();
}

void BinaryParseVisitor::visit(WFV3Chunk& chunk) {
    visit(chunk.data);
}

void BinaryParseVisitor::visit(PFDCChunk& chunk) {
    // Unimplemented chunk - just read raw data for now
}

void BinaryParseVisitor::visit(EDGFEntry& entry) {
    entry.value0 = reader.readArray<f32, 2>();
    entry.value8 = reader.read<f32>();
    entry.valueC = reader.readArray<u8, 0xC>();
}

void BinaryParseVisitor::visit(EDGFChunk& chunk) {
    visit(chunk.entries);
}

void BinaryParseVisitor::visit(NERFEntry& entry) {
    entry.coefs = reader.read<Vector2f>();
}

void BinaryParseVisitor::visit(NERFChunk& chunk) {
    visit(chunk.entries);
}

void BinaryParseVisitor::visit(DETLEntry& entry) {
    entry.flags = reader.read<u16>();
    entry.scale = reader.read<u16>();
    entry.diffuseColorMultiplier = reader.read<u16>();
    entry.unknown0 = reader.read<u16>();
    entry.unknown1 = reader.read<u32>();
}

void BinaryParseVisitor::visit(DETLChunk& chunk) {
    visit(chunk.records);
}

void BinaryParseVisitor::visit(DBOCEntry& entry) {
    entry.unknown1_1 = reader.read<f32>();
    entry.unknown1_2 = reader.read<f32>();
    entry.unknown1_3 = reader.read<u32>();
    entry.unknown1_4 = reader.read<u32>();
}

void BinaryParseVisitor::visit(DBOCChunk& chunk) {
    visit(chunk.entries);
}

void BinaryParseVisitor::visit(AFRAChunk& chunk) {
    chunk.data = reader.read<std::vector<u8>>(maxSize);
}

void BinaryParseVisitor::visit(PCOLChunk& chunk) {
    visit(chunk.vertexPositions);
    visit(chunk.faceNormals);
    visit(chunk.indices);
    visit(chunk.flags);
}

void BinaryParseVisitor::visit(DPIVChunk& chunk) {
    chunk.data = reader.readArray<u8, 32>();
}

void BinaryParseVisitor::visit(TEXLEntry& entry) {
    entry.unknown0 = reader.read<f32>();
    entry.unknown1 = reader.read<f32>();
    entry.textureLookup = reader.read<i32>();
    entry.unknown2 = reader.read<i32>();
}

void BinaryParseVisitor::visit(TEXLChunk& chunk) {
    visit(chunk.texturedLights);
}

void BinaryParseVisitor::visit(SKL1Chunk& chunk) {
    chunk.flags = reader.read<u32>();
    visit(chunk.name);
    chunk.reserved = reader.readArray<u8, 4>();
}

void BinaryParseVisitor::visit(SKA1Chunk& chunk) {
    visit(chunk.attachments);
    visit(chunk.attachmentLookupTable);
}

void BinaryParseVisitor::visit(SKB1Chunk& chunk) {
    visit(chunk.bones);
    visit(chunk.keyBoneLookup);
}

void BinaryParseVisitor::visit(SKS1Chunk& chunk) {
    visit(chunk.globalLoops);
    visit(chunk.sequences);
    visit(chunk.sequenceLookups);
    chunk.reserved = reader.readArray<u8, 8>();
}

void BinaryParseVisitor::visit(SKPDChunk& chunk) {
    chunk.reserved0 = reader.readArray<u8, 8>();
    chunk.parentSkeletonFileId = reader.read<u32>();
    chunk.reserved1 = reader.readArray<u8, 4>();
}

} // namespace m2
} // namespace whiteout

#include "../m2_binary_parse_visitor.h"
#include "common/binary_reader.h"

namespace m2 {

using common::BinaryReader;

// Chunk structure visit implementations
void M2BinaryParseVisitor::visit(M2TXACChunk& chunk, M2BaseFile& file) {
    size_t num_entries = file.header.materials.size() + file.header.particleEmitters.size();
    chunk.unknown = reader.read<std::vector<std::array<u8, 2>>>(num_entries);
}

void M2BinaryParseVisitor::visit(M2PFIDChunk& chunk, M2BaseFile& file) {
    chunk.physFileDataId = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2SFIDChunk& chunk, M2BaseFile& file) {
    chunk.skinFileDataIds = reader.read<std::vector<u32>>(file.header.numSkinProfiles);
    u32 remainingBytes = maxSize - chunk.skinFileDataIds.size() * sizeof(u32);
    if (remainingBytes > 0) {
        chunk.lodSkinFileDataIds = reader.read<std::vector<u32>>(remainingBytes / sizeof(u32));
    }
}

void M2BinaryParseVisitor::visit(M2AFIDEntry& entry) {
    entry.animId = reader.read<u16>();
    entry.subAnimId = reader.read<u16>();
    entry.fileDataId = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2AFIDChunk& chunk) {
    size_t entryCount = maxSize / sizeof(M2AFIDEntry);
    chunk.animFileIds = reader.read<std::vector<M2AFIDEntry>>(entryCount);
}

void M2BinaryParseVisitor::visit(M2BFIDChunk& chunk) {
    size_t entryCount = maxSize / sizeof(u32);
    chunk.boneFileDataIds = reader.read<std::vector<u32>>(entryCount);
}

void M2BinaryParseVisitor::visit(M2EXPTEntry& entry) {
    entry.zSource = reader.read<f32>();
    entry.colorMult = reader.read<f32>();
    entry.alphaMult = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(M2EXPTChunk& chunk) {
    visit(chunk.extendedParticles);
}

void M2BinaryParseVisitor::visit(M2EXP2Particle& particle) {
    particle.zSource = reader.read<f32>();
    particle.colorMult = reader.read<f32>();
    particle.alphaMult = reader.read<f32>();
    visit(particle.alphaCutoff);
}

void M2BinaryParseVisitor::visit(M2EXP2Chunk& chunk) {
    visit(chunk.content);
}

void M2BinaryParseVisitor::visit(M2PABCChunk& chunk) {
    visit(chunk.replacementParentSequenceLookups);
}

void M2BinaryParseVisitor::visit(M2PADCChunk& chunk) {
    visit(chunk.textureWeights);
}

void M2BinaryParseVisitor::visit(M2SequenceBounds& bounds) {
    bounds.extent.minimum = reader.read<Vector3f>();
    bounds.extent.maximum = reader.read<Vector3f>();
    bounds.radius = reader.read<f32>();
}

void M2BinaryParseVisitor::visit(M2PSBCChunk& chunk) {
    visit(chunk.parentSequenceBounds);
}

void M2BinaryParseVisitor::visit(M2PEDCChunk& chunk) {
    visit(chunk.parentEventData);
}

void M2BinaryParseVisitor::visit(M2SKIDChunk& chunk) {
    chunk.skeletonFileDataId = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2TXIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2TXIDChunk& chunk) {
    size_t entryCount = maxSize / sizeof(M2TXIDEntry);
    chunk.textureIds = reader.read<std::vector<M2TXIDEntry>>(entryCount);
}

void M2BinaryParseVisitor::visit(M2LDV1Chunk& chunk) {
    chunk.unknown0 = reader.read<u16>();
    chunk.lodCount = reader.read<u16>();
    chunk.unknown2 = reader.read<f32>();
    chunk.particleBoneLod = reader.readArray<u8, 4>();
    chunk.unknown4 = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2RPIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2RPIDChunk& chunk) {
    size_t entryCount = maxSize / sizeof(M2RPIDEntry);
    chunk.recursiveParticleModels = reader.read<std::vector<M2RPIDEntry>>(entryCount);
}

void M2BinaryParseVisitor::visit(M2GPIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2GPIDChunk& chunk) {
    size_t entryCount = maxSize / sizeof(M2GPIDEntry);
    chunk.geometryParticleModels = reader.read<std::vector<M2GPIDEntry>>(entryCount);
}

void M2BinaryParseVisitor::visit(M2WFV1Chunk& chunk) {
    // Empty chunk
}

void M2BinaryParseVisitor::visit(M2WFV2Chunk& chunk) {
    // Empty chunk
}

void M2BinaryParseVisitor::visit(M2PGD1Entry& entry) {
    entry.geoset = reader.read<u16>();
}

void M2BinaryParseVisitor::visit(M2PGD1Chunk& chunk) {
    visit(chunk.particleGeosetData);
}

void M2BinaryParseVisitor::visit(M2WFV3Data& data) {
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

void M2BinaryParseVisitor::visit(M2WFV3Chunk& chunk) {
    visit(chunk.data);
}

void M2BinaryParseVisitor::visit(M2PFDCChunk& chunk) {
	// Unimplemented chunk - just read raw data for now
}

void M2BinaryParseVisitor::visit(M2EDGFEntry& entry) {
    entry.value0 = reader.readArray<f32, 2>();
    entry.value8 = reader.read<f32>();
    entry.valueC = reader.readArray<u8, 0xC>();
}

void M2BinaryParseVisitor::visit(M2EDGFChunk& chunk) {
    visit(chunk.entries);
}

void M2BinaryParseVisitor::visit(M2NERFEntry& entry) {
    entry.coefs = reader.read<Vector2f>();
}

void M2BinaryParseVisitor::visit(M2NERFChunk& chunk) {
    visit(chunk.entries);
}

void M2BinaryParseVisitor::visit(M2DETLEntry& entry) {
    entry.flags = reader.read<u16>();
    entry.scale = reader.read<u16>();
    entry.diffuseColorMultiplier = reader.read<u16>();
    entry.unknown0 = reader.read<u16>();
    entry.unknown1 = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2DETLChunk& chunk) {
    visit(chunk.records);
}

void M2BinaryParseVisitor::visit(M2DBOCEntry& entry) {
    entry.unknown1_1 = reader.read<f32>();
    entry.unknown1_2 = reader.read<f32>();
    entry.unknown1_3 = reader.read<u32>();
    entry.unknown1_4 = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2DBOCChunk& chunk) {
    visit(chunk.entries);
}

void M2BinaryParseVisitor::visit(M2AFRAChunk& chunk) {
	chunk.data = reader.read<std::vector<u8>>(maxSize);
}

void M2BinaryParseVisitor::visit(M2PCOLChunk& chunk) {
    visit(chunk.vertexPositions);
    visit(chunk.faceNormals);
    visit(chunk.indices);
    visit(chunk.flags);
}

void M2BinaryParseVisitor::visit(M2DPIVChunk& chunk) {
    chunk.data = reader.readArray<u8, 32>();
}

void M2BinaryParseVisitor::visit(M2TEXLEntry& entry) {
    entry.unknown0 = reader.read<f32>();
    entry.unknown1 = reader.read<f32>();
    entry.textureLookup = reader.read<i32>();
    entry.unknown2 = reader.read<i32>();
}

void M2BinaryParseVisitor::visit(M2TEXLChunk& chunk) {
    visit(chunk.texturedLights);
}

void M2BinaryParseVisitor::visit(M2SKL1Chunk& chunk) {
    chunk.flags = reader.read<u32>();
    visit(chunk.name);
    chunk.reserved = reader.readArray<u8, 4>();
}

void M2BinaryParseVisitor::visit(M2SKA1Chunk& chunk) {
    visit(chunk.attachments);
    visit(chunk.attachmentLookupTable);
}

void M2BinaryParseVisitor::visit(M2SKB1Chunk& chunk) {
    visit(chunk.bones);
    visit(chunk.keyBoneLookup);
}

void M2BinaryParseVisitor::visit(M2SKS1Chunk& chunk) {
    visit(chunk.globalLoops);
    visit(chunk.sequences);
    visit(chunk.sequenceLookups);
    chunk.reserved = reader.readArray<u8, 8>();
}

void M2BinaryParseVisitor::visit(M2SKPDChunk& chunk) {
    chunk.reserved0 = reader.readArray<u8, 8>();
    chunk.parentSkeletonFileId = reader.read<u32>();
    chunk.reserved1 = reader.readArray<u8, 4>();
}

} // namespace m2

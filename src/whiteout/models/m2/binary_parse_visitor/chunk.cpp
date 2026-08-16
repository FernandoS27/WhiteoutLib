
#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

void BinaryParseVisitor::visit(TXACChunk& chunk, BaseFile& file) {
    size_t num_entries =
        file.header.model.materials.size() + file.header.model.particleEmitters.size();
    if (maxSize >= 0) {
        num_entries = std::min(num_entries, static_cast<size_t>(maxSize) / 2);
    }
    chunk.entries = reader.read<std::vector<std::array<u8, 2>>>(num_entries);
}

void BinaryParseVisitor::visit(PFIDChunk& chunk, BaseFile& /*file*/) {
    chunk.physFileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(SFIDChunk& chunk, BaseFile& file) {
    chunk.skinFileDataIds = reader.read<std::vector<u32>>(file.header.model.numSkinProfiles);
    u32 const remainingBytes =
        maxSize - static_cast<u32>(chunk.skinFileDataIds.size() * sizeof(u32));
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
    parse_chunked_vector(chunk.extendedParticles);
}

void BinaryParseVisitor::visit(EXP2Chunk& chunk) {
    visit(chunk.emitterExtensions);
    chunk.unknownSize = reader.read<u32>();
    chunk.unknownOffset = reader.read<u32>();
}

void BinaryParseVisitor::visit(PABCChunk& chunk) {
    visit(chunk.replacementParentSequenceLookups);
}

void BinaryParseVisitor::visit(PADCChunk& chunk) {
    visit(chunk.textureWeights);
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

void BinaryParseVisitor::visit(TXIDChunk& chunk) {
    parse_chunked_vector(chunk.textureIds);
}

void BinaryParseVisitor::visit(M2RPIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(M2RPIDChunk& chunk) {
    parse_chunked_vector(chunk.recursiveParticleModels);
}

void BinaryParseVisitor::visit(GPIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void BinaryParseVisitor::visit(GPIDChunk& chunk) {
    size_t const entryCount = maxSize / sizeof(GPIDEntry);
    chunk.geometryParticleModels = reader.read<std::vector<GPIDEntry>>(entryCount);
}

void BinaryParseVisitor::visit(ParticleGeosetData& entry) {
    entry.geoset = reader.read<u16>();
}

void BinaryParseVisitor::visit(PGD1Chunk& chunk) {
    visit(chunk.particleGeosetData);
}

void BinaryParseVisitor::visit(WaterfallData& data) {
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

void BinaryParseVisitor::visit(PFDCChunk& /*chunk*/) {}

void BinaryParseVisitor::visit(EdgeFadeData& entry) {
    entry.value0 = reader.readArray<f32, 2>();
    entry.value8 = reader.read<f32>();
    entry.valueC = reader.readArray<u8, 0xC>();
}

void BinaryParseVisitor::visit(EDGFChunk& chunk) {
    parse_chunked_vector(chunk.entries);
}

void BinaryParseVisitor::visit(DistanceFadeData& entry) {
    entry.squaredFarDist = reader.read<f32>();
    entry.squaredNearDist = reader.read<f32>();
    entry.reserved = reader.readArray<u32, 2>();
}

void BinaryParseVisitor::visit(NERFChunk& chunk) {
    parse_chunked_vector(chunk.entries);
}

void BinaryParseVisitor::visit(DetailedLightData& entry) {
    entry.flags = reader.read<u16>();
    entry.scale = reader.read<u16>();
    entry.diffuseColorMultiplier = reader.read<u16>();
    entry.unknown0 = reader.read<u16>();
    entry.unknown1 = reader.read<u32>();
}

void BinaryParseVisitor::visit(DETLChunk& chunk) {
    parse_chunked_vector(chunk.records);
}

void BinaryParseVisitor::visit(DebugOcclusionData& entry) {
    entry.unknown1_1 = reader.read<f32>();
    entry.unknown1_2 = reader.read<f32>();
    entry.unknown1_3 = reader.read<u32>();
    entry.unknown1_4 = reader.read<u32>();
}

void BinaryParseVisitor::visit(DBOCChunk& chunk) {
    parse_chunked_vector(chunk.entries);
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

void BinaryParseVisitor::visit(TexturedLightData& entry) {
    entry.unknown0 = reader.read<f32>();
    entry.unknown1 = reader.read<f32>();
    entry.textureLookup = reader.read<i32>();
    entry.unknown2 = reader.read<i32>();
}

void BinaryParseVisitor::visit(TEXLChunk& chunk) {
    parse_chunked_vector(chunk.texturedLights);
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
    globalFlags = GlobalFlag::ChunkedAnimFiles;
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

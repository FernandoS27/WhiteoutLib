#include "m2_binary_parse_visitor.h"

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
    
    seq.boundingBox.minimum = reader.read<Vector3f>();
    seq.boundingBox.maximum = reader.read<Vector3f>();
    seq.boundingSphereRadius = reader.read<f32>();
    
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
    header.boundingBox.minimum = reader.read<Vector3f>();
    header.boundingBox.maximum = reader.read<Vector3f>();
    header.boundingSphereRadius = reader.read<f32>();
    
    header.collisionBox.minimum = reader.read<Vector3f>();
    header.collisionBox.maximum = reader.read<Vector3f>();
    header.collisionSphereRadius = reader.read<f32>();
    
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

template<typename T>
void M2BinaryParseVisitor::visit(std::vector<T>& array) {
    // We read M2Arrays as std::vectors 
    const auto count = reader.read<u32>();
    const auto offset = reader.read<u32>();
    if (count == 0) {
        array.clear();
        return;
    }
    const auto currentPos = reader.getPosition();
    reader.setPosition(offset + baseOffset);

    if constexpr (std::is_trivially_copyable_v<T>) {
        array = reader.read<std::vector<T>>(count);
    } else {
        for (size_t i = 0; i < count; ++i) {
            T element;
            this->visit(element);
            array.push_back(std::move(element));
        }
    }
    reader.setPosition(currentPos);
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

template<typename T>
void M2BinaryParseVisitor::visit(AnimationTrack<T>& track) {
    track.interpolationType = reader.read<u16>();
    track.globalSequenceId = reader.read<u16>();
    visit(track.timestamps);
    visit(track.values);
}

void M2BinaryParseVisitor::visit(AnimationTrackBase& track) {
    track.interpolationType = reader.read<u16>();
    track.globalSequenceId = reader.read<u16>();
    visit(track.timestamps);
}

// Chunk structure visit implementations
void M2BinaryParseVisitor::visit(M2TXACChunk& chunk, M2File& file) {
    size_t num_entries = file.header.materials.size() + file.header.particleEmitters.size();
    chunk.unknown = reader.read<std::vector<std::array<u8, 2>>>(num_entries);
}

void M2BinaryParseVisitor::visit(M2PFIDChunk& chunk, M2File& file) {
    chunk.physFileDataId = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2SFIDChunk& chunk, M2File& file) {
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

void M2BinaryParseVisitor::visit(M2AFIDChunk& chunk, M2File& file) {
    size_t entryCount = maxSize / sizeof(M2AFIDEntry);
    chunk.animFileIds = reader.read<std::vector<M2AFIDEntry>>(entryCount);
}

void M2BinaryParseVisitor::visit(M2BFIDChunk& chunk, M2File& file) {
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
    visit(chunk.textureIds);
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
    visit(chunk.recursiveParticleModels);
}

void M2BinaryParseVisitor::visit(M2GPIDEntry& entry) {
    entry.fileDataId = reader.read<u32>();
}

void M2BinaryParseVisitor::visit(M2GPIDChunk& chunk) {
    visit(chunk.geometryParticleModels);
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

} // namespace m2

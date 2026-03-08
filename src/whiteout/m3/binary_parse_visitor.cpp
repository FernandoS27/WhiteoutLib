// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../common/binary_reader.h"
#include "binary_parse_visitor.h"
#include "chunk_traits.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace whiteout {
namespace m3 {

using common::BinaryReader;

namespace {

template <typename T, typename = void>
struct has_set_version : std::false_type {};

template <typename T>
struct has_set_version<T, std::void_t<decltype(std::declval<T&>().setVersion(std::declval<i32>()))>>
    : std::true_type {};

template <typename T>
void setStructureVersion(T& value, u32 version) {
    if constexpr (has_set_version<T>::value) {
        value.setVersion(static_cast<i32>(version));
    }
}

} // namespace

BinaryParseVisitor::BinaryParseVisitor(BinaryReader& reader, bool ismd33) : reader(reader) {
    (void)ismd33;
}

void BinaryParseVisitor::read(Model& model) {
    MD3Header header = reader.read<MD3Header>();

    if (header.magic == TAG_MD34) { // "MD34" or "MD33"
        readReferenceFunc = [this]() {
            Reference ref = this->reader.read<Reference>();
            return ref;
        };
    } else {
        readReferenceFunc = [this]() {
            Reference ref;
            ref.entries = this->reader.read<u32>();
            ref.index = this->reader.read<u32>();
            // MD33 doesn't have flags
            return ref;
        };
    }

    reader.setPosition(header.indexOffset);
    indexTable = reader.read<std::vector<IndexEntry>>(header.indexCount);
    indexUsed.resize(header.indexCount, false);

    reader.setPosition(indexTable[header.modelRef.index].offset);
    indexUsed[0] = true; // Mark the MD34 header as used
    indexUsed[header.modelRef.index] = true;
    visit(model, indexTable[header.modelRef.index].version);
    for (size_t i = 0; i < indexUsed.size(); ++i) {
        if (!indexUsed[i]) {
            issues.emplace_back("Index entry " + std::to_string(i) +
                                " was not used during parsing.");
            issues.emplace_back("  Tag: " + tagToString(indexTable[i].tag) +
                                ", Count: " + std::to_string(indexTable[i].count) +
                                ", Version: " + std::to_string(indexTable[i].version));
        }
    }
}

void BinaryParseVisitor::visit(Model& model, u32 version) {
    setStructureVersion(model, version);
    visit(model.name);
    model.flags = static_cast<ModelFlag>(reader.read<u32>());
    visit(model.sequences);
    visit(model.subTrackCollections);
    visit(model.animationGroups);
    visit(model.boneAnimationSets);
    model.animationSplitCount = reader.read<u32>();
    visit(model.animationStates);
    visit(model.bones);
    model.skinBoneCount = reader.read<u32>();
    visit(model.vertices, 0);
    visit(model.divisions);
    visit(model.boneLookup);
    model.bounds = reader.read<Extent>();
    model.collisionBounds = reader.read<Extent>();
    visit(model.collisionFaces);
    visit(model.collisionVerts);
    visit(model.collisionNormals);

    visit(model.attachmentPoints);
    visit(model.attachmentPointAddons);
    visit(model.lights);
    if (version >= 21) {
        visit(model.shadowBoxes);
    }
    visit(model.cameras);
    visit(model.camerasAddons);

    visit(model.materialMaps);
    visit(model.standardMaterials);
    visit(model.displacementMaterials);
    visit(model.compositeMaterials);
    visit(model.terrainMaterials);
    visit(model.volumeMaterials);
    visit(model.hairMaterials);
    visit(model.creepMaterials);

    if (version >= 25) {
        visit(model.volumeNoiseMaterials);
    }
    if (version >= 26) {
        visit(model.stbMaterials);
    }
    if (version >= 28) {
        visit(model.reflectionMaterials);
    }
    if (version >= 29) {
        visit(model.lensFlareMaterials);
    }
    if (version >= 30) {
        visit(model.materialAddData);
    }

    visit(model.particleEmitters);
    visit(model.particleEmitterCopies);
    visit(model.ribbonEmitters);
    visit(model.projections);
    visit(model.forces);
    visit(model.warps);
    visit(model.viewVolumes);

    visit(model.rigidBodies);
    visit(model.physicsConstraints);
    visit(model.physicsJoints);
    if (version >= 28) {
        visit(model.clothPhysics);
    }
    visit(model.ikTwoJoints);
    if (version >= 24) {
        visit(model.ikCCD);
    }
    visit(model.ikJoints);
    visit(model.oneBoneSolvers);

    visit(model.turretBehaviors);
    visit(model.triggerData);
    visit(model.initialReference);

    visit(model.tightHitTestObject, 1);
    visit(model.fuzzyHitTestObjects);
    visit(model.attachmentVolumes);
    if (version >= 23) {
        visit(model.attachmentVolumesAddon0);
        visit(model.attachmentVolumesAddon1);
    }
    visit(model.billboardBehaviors);

    if (version >= 21) {
        visit(model.trailingModels);
        model.m3aAnimHash = reader.read<u32>();
    }
    if (version >= 23) {
        visit(model.m3aAnimHashes);
    }
}

void BinaryParseVisitor::visit(VertexBuffer& value, u32 version) {
    (void)version;
    value.flags = reader.read<VertexFormatFlag>();
    visit(value.data);
    value.initialize();
}

void BinaryParseVisitor::visit(Sequence& value, u32 version) {
    value.id = reader.read<i32>();
    value.index = reader.read<i32>();
    visit(value.name);
    value.startFrame = reader.read<u32>();
    value.endFrame = reader.read<u32>();
    value.moveSpeed = reader.read<f32>();
    value.flags = reader.read<SequenceFlag>();
    value.frequency = reader.read<u32>();
    value.replayStart = reader.read<u32>();
    value.replayEnd = reader.read<u32>();
    value.blendTime = reader.read<u32>();
    if (version <= 1) {
        value.deprecated.unknown = reader.read<u32>();
    }
    value.bounds = reader.read<Extent>();
    visit(value.animationSets);
}

void BinaryParseVisitor::visit(SubTrackContainer& value, u32 version) {
    (void)version;

    visit(value.name);
    value.runsConcurrent = reader.read<u16>();
    value.animPriority = reader.read<u16>();
    value.animationStateIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    visit(value.animIds);
    visit(value.animRefs);
    value.unknown = reader.read<u32>();
    visit(value.sdev);
    visit(value.sd2v);
    visit(value.sd3v);
    visit(value.sd4q);
    visit(value.sdcc);
    visit(value.sdr3);
    visit(value.sdu8);
    visit(value.sds6);
    visit(value.sdu6);
    visit(value.sds3);
    visit(value.sdu3);
    visit(value.sdfg);
    visit(value.sdmb);
}

void BinaryParseVisitor::visit(AnimationGroup& value, u32 version) {
    (void)version;
    visit(value.name);
    visit(value.subtrackIndices);
}

void BinaryParseVisitor::visit(AnimationState& value, u32 version) {
    (void)version;
    visit(value.animIds);
    value.unknown = reader.readArray<u8, 16>();
}

void BinaryParseVisitor::visit(BoneAnimationSet& value, u32 version) {
    (void)version;
    value.flags = reader.read<Flag>();
    value.animationSequenceIndex = reader.read<u16>();
    value.fallbackSequenceIndex = reader.read<u16>();
    visit(value.name);
    visit(value.splitItems);
}

void BinaryParseVisitor::visit(Bone& value, u32 version) {
    (void)version;
    value.unknown = reader.read<u32>();
    visit(value.name);
    value.flags = reader.read<BoneFlag>();
    value.parentIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.position = reader.read<AnimRef<Vector3f>>();
    value.rotation = reader.read<AnimRef<Quaternion>>();
    value.scale = reader.read<AnimRef<Vector3f>>();
    value.visibility = reader.read<AnimRef<u32>>();
}

void BinaryParseVisitor::visit(Region& value, u32 version) {
    value.index = reader.read<u32>();
    if (version >= 3) {
        value.unknown = reader.read<u32>();
        value.firstVertex = reader.read<u32>();
        value.vertexCount = reader.read<u32>();
        value.firstIndex = reader.read<u32>();
        value.indexCount = reader.read<u32>();
    } else {
        value.firstVertex = reader.read<u16>();
        value.vertexCount = reader.read<u16>();
    }
    value.unknown2 = reader.read<u16>();
    value.firstBoneLookup = reader.read<u16>();
    value.boneLookupCount = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.boneWeightPairs = reader.read<u8>();
    value.boneIndexPairs = reader.read<u8>();
    value.rootBone = reader.read<u16>();
    if (version >= 4) {
        value.flags = reader.read<RegionFlag>();
    }
    if (version >= 5) {
        value.uvScale = reader.read<f32>();
        value.uvOffset = reader.read<f32>();
    }
}

void BinaryParseVisitor::visit(Batch& value, u32 version) {
    (void)version;
    value.unknown = reader.read<u32>();
    value.regionIndex = reader.read<u16>();
    value.unknown2 = reader.read<u32>();
    value.materialIndex = reader.read<u16>();
    value.boneCount = reader.read<u16>();
}

void BinaryParseVisitor::visit(MeshSection& value, u32 version) {
    (void)version;
    value.nodeIndex = reader.read<u32>();
    value.bounds = reader.read<AnimRef<Extent>>();
}

void BinaryParseVisitor::visit(MeshDivision& value, u32 version) {
    (void)version;
    visit(value.faces);
    visit(value.regions);
    visit(value.batches);
    visit(value.msec);
    value.instances = reader.read<u32>();
}

void BinaryParseVisitor::visit(InitialReference& value, u32 version) {
    (void)version;
    value.matrix = reader.read<Matrix44f>();
}

void BinaryParseVisitor::visit(AttachmentPoint& value, u32 version) {
    (void)version;
    value.unknown = reader.read<u32>();
    visit(value.name);
    value.boneIndex = reader.read<u32>();
}

void BinaryParseVisitor::visit(MaterialMap& value, u32 version) {
    (void)version;
    value.materialType = static_cast<MaterialType>(reader.read<u32>());
    value.materialIndex = reader.read<u32>();
}

void BinaryParseVisitor::visit(TextureLayer& value, u32 version) {
    value.id = reader.read<u32>();
    visit(value.texturePath);
    value.color = reader.read<AnimRef<ColorBGRA>>();
    value.flags = static_cast<TextureLayerFlag>(reader.read<u32>());
    value.uvMapping = static_cast<UVMappingMode>(reader.read<u32>());
    value.colorType = static_cast<ColorChannelSelect>(reader.read<u32>());
    value.rgbMultiply = reader.read<AnimRef<f32>>();
    value.rgbAdd = reader.read<AnimRef<f32>>();
    value.pocTexture = reader.read<u32>();
    if (version >= 24) {
        value.noiseAmplitude = reader.read<f32>();
        value.noiseFrequency = reader.read<f32>();
    }
    value.textureSource = reader.read<u32>();
    value.aviFrameRate = reader.read<u32>();
    value.aviStart = reader.read<u32>();
    value.aviStop = reader.read<u32>();
    value.aviLoop = reader.read<u32>();
    value.aviSync = reader.read<u32>();
    value.aviPlay = reader.read<AnimRef<u32>>();
    value.aviRestart = reader.read<AnimRef<u32>>();
    value.flipbookRows = reader.read<u32>();
    value.flipbookColumns = reader.read<u32>();
    value.currentFrame = reader.read<AnimRef<u16>>();
    value.uvOffset = reader.read<AnimRef<Vector2f>>();
    value.uvAngle = reader.read<AnimRef<Vector3f>>();
    value.uvTiling = reader.read<AnimRef<Vector2f>>();
    value.wOffset = reader.read<AnimRef<f32>>();
    value.wTiling = reader.read<AnimRef<f32>>();
    value.mapAlpha = reader.read<AnimRef<f32>>();
    if (version >= 23) {
        value.triplanarOffset = reader.read<AnimRef<Vector3f>>();
        value.triplanarScale = reader.read<AnimRef<Vector3f>>();
    }
    value.uvSourceRelated = reader.read<u32>();
    value.fresnelMode = static_cast<FresnelMode>(reader.read<u32>());
    value.fresnelExponent = reader.read<f32>();
    value.fresnelMin = reader.read<f32>();
    value.fresnelMax = reader.read<f32>();
    if (version >= 25) {
        value.fresnelTranslation = reader.read<Vector3f>();
        value.fresnelMask = reader.read<Vector3f>();
        value.fresnelRotation = reader.read<Vector2f>();
    }
    value.uvDensity = reader.read<u32>();
}

void BinaryParseVisitor::visit(StandardMaterial& value, u32 version) {
    visit(value.name);
    value.additionalFlags = static_cast<MaterialAdditionalFlag>(reader.read<u32>());
    value.flags = static_cast<MaterialFlag>(reader.read<u32>());
    value.blendMode = static_cast<BlendMode>(reader.read<u32>());
    value.priority = reader.read<i32>();
    value.rttChannels = reader.read<u32>();
    value.specularExponent = reader.read<f32>();
    value.depthBlendFalloff = reader.read<f32>();
    value.alphaTestThreshold = reader.read<u32>();
    value.hdrSpecularMultiplier = reader.read<f32>();
    value.hdrEmissiveMultiplier = reader.read<f32>();
    if (version >= 20) {
        value.hdrEnvironmentConstant = reader.read<f32>();
        value.hdrEnvironmentDiffuse = reader.read<f32>();
        value.hdrEnvironmentSpecular = reader.read<f32>();
    }

    visit(value.diffuseLayer);
    visit(value.decalLayer);
    visit(value.specularLayer);
    if (version >= 16) {
        visit(value.glossLayer);
    }
    visit(value.emissiveLayer1);
    visit(value.emissiveLayer2);
    visit(value.environmentLayer);
    visit(value.environmentMaskLayer);
    visit(value.alphaLayer1);
    visit(value.alphaLayer2);
    visit(value.normalLayer);
    visit(value.heightLayer);
    visit(value.lightMapLayer);
    visit(value.ambientOcclusionLayer);
    if (version >= 19) {
        visit(value.normalBlend1MaskLayer);
        visit(value.normalBlend2MaskLayer);
        visit(value.normalBlend1Layer);
        visit(value.normalBlend2Layer);
    }

    value.materialClass = static_cast<MaterialClass>(reader.read<u32>());
    value.layerBlendMode = static_cast<LayerBlendOp>(reader.read<u32>());
    value.emissiveBlendMode1 = static_cast<LayerBlendOp>(reader.read<u32>());
    value.emissiveBlendMode2 = static_cast<LayerBlendOp>(reader.read<u32>());
    value.specularMode = static_cast<SpecularMode>(reader.read<u32>());
    value.parallaxHeight = reader.read<AnimRef<f32>>();
    value.motionBlurAmount = reader.read<AnimRef<f32>>();
    if (version >= 19) {
        visit(value.normalBlendFactors);
    }
}

void BinaryParseVisitor::visit(DisplacementMaterial& value, u32 version) {
    (void)version;
    visit(value.name);
    value.unknown = reader.read<u32>();
    value.strength = reader.read<AnimRef<f32>>();
    visit(value.normalMap);
    visit(value.strengthMap);
    value.flags = reader.read<Flag>();
    value.priority = reader.read<u32>();
}

void BinaryParseVisitor::visit(CompositeSection& value, u32 version) {
    (void)version;
    value.materialIndex = reader.read<u32>();
    value.mapMultiplier = reader.read<AnimRef<f32>>();
}

void BinaryParseVisitor::visit(CompositeMaterial& value, u32 version) {
    (void)version;
    visit(value.name);
    value.priority = reader.read<u32>();
    visit(value.sections);
}

void BinaryParseVisitor::visit(TerrainMaterial& value, u32 version) {
    visit(value.name);
    visit(value.terrainMap);
    if (version >= 1) {
        value.unknown = reader.read<u32>();
    }
}

void BinaryParseVisitor::visit(VolumeMaterial& value, u32 version) {
    (void)version;
    visit(value.name);
    value.blendMode = reader.read<u32>();
    value.falloffType = static_cast<VolumeFalloffType>(reader.read<u32>());
    value.density = reader.read<AnimRef<f32>>();
    visit(value.colorMap);
    visit(value.noiseMap1);
    visit(value.noiseMap2);
    value.alphaThreshold = reader.read<u32>();
    value.flags = reader.read<Flag>();
}

void BinaryParseVisitor::visit(HairMaterial& value, u32 version) {
    (void)version;
    visit(value.name);
    visit(value.layerBase);
    visit(value.layerSpecShift);
    visit(value.layerSpecNoise);
    visit(value.layerAO);
    value.shiftPrimary = reader.read<f32>();
    value.shiftSecondary = reader.read<f32>();
    value.colorDiffuse = reader.read<AnimRef<ColorBGRA>>();
    value.colorSpec = reader.read<AnimRef<ColorBGRA>>();
    value.specExponent0 = reader.read<f32>();
    value.specExponent1 = reader.read<f32>();
}

void BinaryParseVisitor::visit(VolumeNoiseMaterial& value, u32 version) {
    (void)version;
    visit(value.name);
    value.falloffType = static_cast<VolumeFalloffType>(reader.read<u32>());
    value.drawTransparency = static_cast<VolumeNoiseCameraMode>(reader.read<u32>());
    value.density = reader.read<AnimRef<f32>>();
    value.nearPlane = reader.read<AnimRef<f32>>();
    value.falloff = reader.read<AnimRef<f32>>();
    visit(value.colorMap);
    visit(value.noiseMap1);
    visit(value.noiseMap2);
    value.scrollRate = reader.read<AnimRef<Vector3f>>();
    value.position = reader.read<AnimRef<Vector3f>>();
    value.scale = reader.read<AnimRef<Vector3f>>();
    value.rotation = reader.read<AnimRef<Vector3f>>();
    value.alphaThreshold = reader.read<u32>();
    value.flags = reader.read<VolumeNoiseMaterialFlag>();
}

void BinaryParseVisitor::visit(CreepMaterial& value, u32 version) {
    visit(value.name);
    visit(value.maskMap);
    if (version >= 1) {
        value.creepLow = reader.read<u32>();
    }
}

void BinaryParseVisitor::visit(STBMaterial& value, u32 version) {
    (void)version;
    visit(value.name);
    visit(value.diffuseMap);
    visit(value.normalMap);
    visit(value.specularMap);
}

void BinaryParseVisitor::visit(ReflectionMaterial& value, u32 version) {
    visit(value.name);
    value.unknown = reader.read<u32>();
    value.reflectionStrength = reader.read<AnimRef<f32>>();
    value.displacementStrength = reader.read<AnimRef<f32>>();
    if (version >= 2) {
        value.reflectionOffset = reader.read<AnimRef<f32>>();
        value.blurAngle = reader.read<AnimRef<f32>>();
        value.blurDistanceMax = reader.read<AnimRef<f32>>();
    }
    visit(value.reflectionMap);
    visit(value.displacementMap);
    if (version >= 2) {
        visit(value.blurMap);
    }
    value.flags = reader.read<ReflectionMaterialFlag>();
    if (version >= 3) {
        value.unknown2 = reader.read<u32>();
    }
}

void BinaryParseVisitor::visit(SubFlare& value, u32 version) {
    (void)version;
    value.index = reader.read<u32>();
    value.position = reader.read<f32>();
    value.sizeXY = reader.read<Vector2f>();
    value.scaleXY = reader.read<Vector2f>();
    value.fadeIn = reader.read<Vector2f>();
    value.fadeOut = reader.read<Vector2f>();
    value.colorAlpha = reader.read<ColorBGRA>();
    value.faceCenter = reader.read<u32>();
    value.offset = reader.read<Vector2f>();
}

void BinaryParseVisitor::visit(LensFlare& value, u32 version) {
    visit(value.name);
    visit(value.flareMap);
    visit(value.maskMap);
    visit(value.subFlares);
    value.columns = reader.read<u32>();
    value.rows = reader.read<u32>();
    value.distanceFade = reader.read<f32>();
    if (version >= 3) {
        visit(value.libName);
    }
    value.intensity = reader.read<AnimRef<f32>>();
    if (version >= 3) {
        value.color = reader.read<AnimRef<ColorBGRA>>();
        value.hdr = reader.read<AnimRef<f32>>();
        value.size = reader.read<AnimRef<f32>>();
    }
}

void BinaryParseVisitor::visit(MaterialAddData& value, u32 version) {
    visit(value.keyName);
    visit(value.keyHash);
    if (version >= 2) {
        visit(value.extraHash);
    }
    visit(value.valuePath);
    visit(value.valueData);
    for (auto& reservedRef : value.reserved) {
        reservedRef = readReferenceFunc();
    }
    value.frequency = reader.read<f32>();
    value.intensity = reader.read<f32>();
    value.holdTime = reader.read<f32>();
    value.randomHash = reader.read<u32>();
    value.animationType = reader.read<u32>();
    value.padding0 = reader.read<u32>();
    value.loopCount = reader.read<i32>();
    value.flags = reader.read<u32>();
    value.subType = reader.read<u32>();
    value.configA = reader.read<u32>();
    value.configB = reader.read<u32>();
    if (version >= 3) {
        value.extraId0 = reader.read<u32>();
        value.extraId1 = reader.read<u32>();
    }
}

void BinaryParseVisitor::visit(Light& value, u32 version) {
    (void)version;
    value.lightType = static_cast<LightType>(reader.read<u16>());
    value.boneIndex = reader.read<u16>();
    value.flags = static_cast<LightFlag>(reader.read<u32>());
    value.lodCut = reader.read<u32>();
    value.shadowLodCut = reader.read<u32>();
    value.diffuseColor = reader.read<AnimRef<Vector3f>>();
    value.intensityMultiplier = reader.read<AnimRef<f32>>();
    value.specularColor = reader.read<AnimRef<Vector3f>>();
    value.specularMultiplier = reader.read<AnimRef<f32>>();
    value.decay = reader.read<AnimRef<f32>>();
    value.attenuationEnd = reader.read<f32>();
    value.attenuationStart = reader.read<AnimRef<f32>>();
    value.hotSpot = reader.read<AnimRef<f32>>();
    value.falloff = reader.read<AnimRef<f32>>();
}

void BinaryParseVisitor::visit(Camera& value, u32 version) {
    value.boneIndex = reader.read<u32>();
    visit(value.name);

    if (version >= 2) {
        value.fieldOfView = reader.read<AnimRef<f32>>();
        value.useVerticalFOV = reader.read<u32>();
    }

    if (version == 2) {
        // Beta v2: farClip/nearClip are plain f32 (promoted to AnimRef for storage)
        f32 farClipStatic = reader.read<f32>();
        f32 nearClipStatic = reader.read<f32>();
        value.farClip = AnimRef<f32>{};
        value.farClip.initValue = farClipStatic;
        value.nearClip = AnimRef<f32>{};
        value.nearClip.initValue = nearClipStatic;
    }

    if (version >= 5) {
        value.dofType = reader.read<u32>();
    }

    if (version >= 3) {
        value.farClip = reader.read<AnimRef<f32>>();
        value.nearClip = reader.read<AnimRef<f32>>();
    }

    if (version >= 2) {
        value.shadowClipDistance = reader.read<AnimRef<f32>>();
        value.focusDistance = reader.read<AnimRef<f32>>();
        value.farFocusRange = reader.read<AnimRef<f32>>();
        value.nearFocusRange = reader.read<AnimRef<f32>>();
    }

    if (version >= 4) {
        value.nearFalloffStart = reader.read<AnimRef<f32>>();
        value.nearFalloffEnd = reader.read<AnimRef<f32>>();
    }

    if (version >= 2) {
        value.dofAmount = reader.read<AnimRef<f32>>();
    }

    if (version >= 5) {
        value.bokehFStop = reader.read<AnimRef<f32>>();
        value.bokehMaxCoCDiameter = reader.read<AnimRef<f32>>();
    }
}

void BinaryParseVisitor::visit(Event& value, u32 version) {
    visit(value.name);
    value.unknown = reader.read<u32>();
    value.boneIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.transform = reader.read<Matrix44f>();
    value.eventType = reader.read<u32>();
    visit(value.optionString);
    if (version >= 1) {
        value.rttChannelIndex = reader.read<u32>();
    }
    if (version >= 2) {
        value.extraParameter = reader.read<u32>();
    }
}

void BinaryParseVisitor::visit(ParticleEmitter& value, u32 version) {
    // Identification
    value.boneIndex = reader.read<u32>();
    value.materialIndex = reader.read<u32>();
    if (version >= 17) {
        value.additionalFlags = reader.read<ParticleAdditionalFlag>();
    }

    // Initial Velocity
    value.initialSpeed = reader.read<AnimRef<f32>>();
    value.initialSpeedRandom = reader.read<AnimRef<f32>>();
    if (version <= 14) {
        if (reader.read<u32>())
            value.additionalFlags |= ParticleAdditionalFlag::EmitSpeedRandomize;
    }
    value.initialYaw = reader.read<AnimRef<f32>>();
    value.initialPitch = reader.read<AnimRef<f32>>();
    value.initialHorizontal = reader.read<AnimRef<f32>>();
    value.initialVertical = reader.read<AnimRef<f32>>();

    // Lifetime
    value.lifetime = reader.read<AnimRef<f32>>();
    value.lifetimeRandom = reader.read<AnimRef<f32>>();
    if (version <= 14) {
        if (reader.read<u32>())
            value.additionalFlags |= ParticleAdditionalFlag::LifespanRandomize;
    }

    // Distance & Gravity
    value.killRadius = reader.read<f32>();
    value.gravityX = reader.read<u32>();
    value.gravityY = reader.read<u32>();
    value.gravity = reader.read<f32>();

    // Midpoint Timing (since v12)
    if (version >= 12) {
        value.sizeMidTime = reader.read<f32>();
        value.colorMidTime = reader.read<f32>();
        value.alphaMidTime = reader.read<f32>();
        value.rotationMidTime = reader.read<f32>();
    }

    // Hold Times (since v14)
    if (version >= 14) {
        value.sizeMidHoldTime = reader.read<f32>();
        value.colorMidHoldTime = reader.read<f32>();
        value.alphaMidHoldTime = reader.read<f32>();
        value.rotationMidHoldTime = reader.read<f32>();
    }

    // Per-Particle Curves
    value.sizeAnimation = reader.read<AnimRef<Vector3f>>();
    if (version <= 11) {
        value.sizeMidTime = reader.read<f32>();
    }
    value.rotationAnimation = reader.read<AnimRef<Vector3f>>();
    if (version <= 11) {
        value.rotationMidTime = reader.read<f32>();
    }
    value.colorStart = reader.read<AnimRef<ColorBGRA>>();
    value.colorMid = reader.read<AnimRef<ColorBGRA>>();
    value.colorEnd = reader.read<AnimRef<ColorBGRA>>();
    if (version <= 11) {
        value.colorMidTime = reader.read<f32>();
        value.alphaMidTime = reader.read<f32>();
    }

    // Physics
    value.drag = reader.read<f32>();
    if (version <= 14) {
        if (reader.read<u32>())
            value.additionalFlags |= ParticleAdditionalFlag::MassRandomize;
    }
    value.mass = reader.read<f32>();
    value.massRandom = reader.read<f32>();
    if (version >= 12) {
        value.massSizeMultiplier = reader.read<f32>();
    }
    if (version <= 14) {
        if (reader.read<u32>())
            value.additionalFlags |= ParticleAdditionalFlag::WorldSpace;
    }

    // Forces
    value.localForces = reader.read<u16>();
    value.worldForces = reader.read<u16>();
    value.localForcesFallback = reader.read<u16>();
    value.worldForcesFallback = reader.read<u16>();
    if (version >= 24) {
        value.worldForcesMassMultiplier = reader.read<f32>();
    }

    // Noise
    value.noiseAmplitude = reader.read<f32>();
    value.noiseFrequency = reader.read<f32>();
    value.noiseCoherence = reader.read<f32>();
    value.noiseEdge = reader.read<f32>();
    if (version <= 11) {
        value.deprecated.noiseSmoothness = reader.read<f32>();
    }
    if (version >= 11) {
        value.indexPlusLength = reader.read<u32>();
    }

    // Emission
    value.maxParticles = reader.read<u32>();
    value.emissionRate = reader.read<AnimRef<f32>>();
    value.emitterShape = reader.read<EmitterShape>();
    value.shapeOuter = reader.read<AnimRef<Vector3f>>();
    value.shapeInner = reader.read<AnimRef<Vector3f>>();
    value.outerRadius = reader.read<AnimRef<f32>>();
    value.innerRadius = reader.read<AnimRef<f32>>();
    if (version >= 14) {
        visit(value.shapeRegions);
    }

    // Randomization
    value.velocityType = reader.read<u32>();
    value.sizeRandomEnable = reader.read<u32>();
    value.sizeRandomAnimation = reader.read<AnimRef<Vector3f>>();
    value.rotationRandomEnable = reader.read<u32>();
    value.rotationRandomAnimation = reader.read<AnimRef<Vector3f>>();
    value.colorRandomEnable = reader.read<u32>();
    value.colorStartRandom = reader.read<AnimRef<ColorBGRA>>();
    value.colorMidRandom = reader.read<AnimRef<ColorBGRA>>();
    value.colorEndRandom = reader.read<AnimRef<ColorBGRA>>();
    value.alphaRandomEnable = reader.read<u32>();

    // Squirt & Flipbook
    value.squirtAmount = reader.read<AnimRef<u16>>();
    value.flipbookStartInitIndex = reader.read<u8>();
    value.flipbookStartStopIndex = reader.read<u8>();
    value.flipbookEndInitIndex = reader.read<u8>();
    value.flipbookEndStopIndex = reader.read<u8>();
    value.flipbookMidTime = reader.read<f32>();
    value.flipbookColumns = reader.read<u16>();
    value.flipbookRows = reader.read<u16>();
    if (version >= 12) {
        value.flipbookColumnFraction = reader.read<f32>();
        value.flipbookRowFraction = reader.read<f32>();
    }

    // Collision
    value.bounce = reader.read<f32>();
    value.friction = reader.read<f32>();
    value.collisionSpawnIndex = reader.read<i32>();
    value.collisionSpawnMin = reader.read<u32>();
    value.collisionSpawnMax = reader.read<u32>();
    value.collisionSpawnChance = reader.read<f32>();
    value.collisionSpawnEnergy = reader.read<f32>();
    value.collisionDieBounce = reader.read<u32>();

    // Instance
    value.instanceType = reader.read<ParticleInstanceType>();
    value.tailLength = reader.read<f32>();
    value.instanceAngle = reader.read<Vector3f>();
    if (version >= 17) {
        value.instanceDistance = reader.read<f32>();
    }

    // Variation Channels (order: pitch, yaw, speed, size, alpha, color, rotation, horizontal,
    // vertical)
    value.pitchType = reader.read<u32>();
    value.pitchAmplitude = reader.read<AnimRef<f32>>();
    value.pitchFrequency = reader.read<AnimRef<f32>>();
    value.yawType = reader.read<u32>();
    value.yawAmplitude = reader.read<AnimRef<f32>>();
    value.yawFrequency = reader.read<AnimRef<f32>>();
    value.speedType = reader.read<u32>();
    value.speedAmplitude = reader.read<AnimRef<f32>>();
    value.speedFrequency = reader.read<AnimRef<f32>>();
    value.sizeType = reader.read<u32>();
    value.sizeAmplitude = reader.read<AnimRef<f32>>();
    value.sizeFrequency = reader.read<AnimRef<f32>>();
    value.alphaType = reader.read<u32>();
    value.alphaAmplitude = reader.read<AnimRef<f32>>();
    value.alphaFrequency = reader.read<AnimRef<f32>>();
    value.colorType = reader.read<u32>();
    value.colorAmplitude = reader.read<AnimRef<f32>>();
    value.colorFrequency = reader.read<AnimRef<f32>>();
    value.rotationType = reader.read<u32>();
    value.rotationAmplitude = reader.read<AnimRef<f32>>();
    value.rotationFrequency = reader.read<AnimRef<f32>>();
    value.horizontalType = reader.read<u32>();
    value.horizontalAmplitude = reader.read<AnimRef<f32>>();
    value.horizontalFrequency = reader.read<AnimRef<f32>>();
    value.verticalType = reader.read<u32>();
    value.verticalAmplitude = reader.read<AnimRef<f32>>();
    value.verticalFrequency = reader.read<AnimRef<f32>>();

    // Parent Velocity & Phase
    value.particleVelocity = reader.read<AnimRef<f32>>();
    if (version >= 22) {
        value.phaseShift = reader.read<AnimRef<f32>>();
    }

    // Flags
    value.flags = reader.read<ParticleFlag>();
    if (version >= 18) {
        value.rotationFlags = reader.read<ParticleRotationFlag>();
    }

    // Smoothing (since v14)
    if (version >= 14) {
        value.colorSmoothing = reader.read<InterpolationMode>();
        value.sizeSmoothing = reader.read<InterpolationMode>();
        value.rotationSmoothing = reader.read<InterpolationMode>();
    }

    // UV Screen Space (since v17)
    if (version >= 17) {
        value.alphaThreshold = reader.read<AnimRef<f32>>();
        value.uvOffset = reader.read<AnimRef<Vector2f>>();
        value.uvAngle = reader.read<AnimRef<Vector3f>>();
        value.uvTiling = reader.read<AnimRef<Vector2f>>();
    }

    // Spline (always present)
    visit(value.splatLineData);

    // Wind & LOD
    value.windMultiplier = reader.read<f32>();
    value.lodReduce = reader.read<u32>();
    value.lodCut = reader.read<u32>();

    // Bounds
    value.lowerBound = reader.read<AnimRef<f32>>();
    value.upperBound = reader.read<AnimRef<f32>>();

    // Trails
    value.trailLinkIndex = reader.read<i32>();
    value.trailChance = reader.read<f32>();
    value.trailEmissionRate = reader.read<AnimRef<f32>>();

    // Splat
    value.splatProjectionIndex = reader.read<i32>();
    value.splatChance = reader.read<f32>();

    // References
    visit(value.modelPaths);
    visit(value.copyIndices);

    // v23+ unknowns
    if (version >= 23) {
        value.spawnRibbonOnBounceChance = reader.read<f32>();
        value.ribbonLinkIndex = reader.read<i32>();
    }
}

void BinaryParseVisitor::visit(ParticleEmitterCopy& value, u32 version) {
    (void)version;
    value.emissionRate = reader.read<AnimRef<f32>>();
    value.squirtAmount = reader.read<AnimRef<u16>>();
    value.boneIndex = reader.read<u32>();
}

void BinaryParseVisitor::visit(SplineRibbon& value, u32 version) {
    (void)version;
    value.emissionOffset = reader.read<Vector3f>();
    value.emissionVector = reader.read<Vector3f>();
    value.velocity = reader.read<AnimRef<f32>>();
    value.reserved = reader.read<u32>();
    value.boneIndex = reader.read<u32>();
    value.velocityBaseFactor = reader.read<AnimRef<f32>>();
    value.velocityEndFactor = reader.read<AnimRef<f32>>();
    value.yawType = reader.read<u32>();
    value.yawAmplitude = reader.read<AnimRef<f32>>();
    value.yawFrequency = reader.read<AnimRef<f32>>();
    value.pitchType = reader.read<u32>();
    value.pitchAmplitude = reader.read<AnimRef<f32>>();
    value.pitchFrequency = reader.read<AnimRef<f32>>();
    value.velocityType = reader.read<u32>();
    value.velocityAmplitude = reader.read<AnimRef<f32>>();
    value.velocityFrequency = reader.read<AnimRef<f32>>();
    value.yaw = reader.read<AnimRef<f32>>();
    value.pitch = reader.read<AnimRef<f32>>();
    value.emissionVectorNormFactor = reader.read<f32>();
    value.velocityNormFactor = reader.read<f32>();
}

void BinaryParseVisitor::visit(RibbonEmitter& value, u32 version) {
    // Identification
    value.boneIndex = reader.read<u16>();
    value.boneIndexFallback = reader.read<u16>();
    value.materialIndex = reader.read<u32>();
    if (version >= 8) {
        value.additionalFlags = static_cast<RibbonAdditionalFlag>(reader.read<u32>());
    }

    // Initial velocity
    value.initialSpeed = reader.read<AnimRef<f32>>();
    value.initialSpeedRandom = reader.read<AnimRef<f32>>();
    if (version <= 6) {
        if (reader.read<u32>())
            value.additionalFlags |= RibbonAdditionalFlag::SpeedRandomize;
    }

    // till v6: pitch then yaw, since v8: yaw then pitch
    if (version <= 6) {
        value.initialPitch = reader.read<AnimRef<f32>>();
        value.initialYaw = reader.read<AnimRef<f32>>();
    } else {
        value.initialYaw = reader.read<AnimRef<f32>>();
        value.initialPitch = reader.read<AnimRef<f32>>();
    }
    value.initialHorizontal = reader.read<AnimRef<f32>>();
    value.initialVertical = reader.read<AnimRef<f32>>();

    // Lifetime
    value.lifetime = reader.read<AnimRef<f32>>();
    value.lifetimeRandom = reader.read<AnimRef<f32>>();
    if (version <= 6) {
        if (reader.read<u32>())
            value.additionalFlags |= RibbonAdditionalFlag::LifespanRandomize;
    }

    // Distance & gravity
    value.killRadius = reader.read<u32>();
    value.gravityX = reader.read<f32>();
    value.gravityY = reader.read<f32>();
    value.gravity = reader.read<f32>();

    // Midpoint timing
    if (version >= 6) {
        value.sizeMidTime = reader.read<f32>();
        value.colorMidTime = reader.read<f32>();
        value.alphaMidTime = reader.read<f32>();
        value.rotationMidTime = reader.read<f32>();
    }
    if (version >= 8) {
        value.sizeMidHoldTime = reader.read<f32>();
        value.colorMidHoldTime = reader.read<f32>();
        value.alphaMidHoldTime = reader.read<f32>();
        value.rotationMidHoldTime = reader.read<f32>();
    }

    // Curves
    value.sizeAnimation = reader.read<AnimRef<Vector3f>>();
    if (version <= 5) {
        value.sizeMidTime = reader.read<f32>();
    }
    value.rotationAnimation = reader.read<AnimRef<Vector3f>>();
    if (version <= 5) {
        value.rotationMidTime = reader.read<f32>();
    }
    value.colorStart = reader.read<AnimRef<ColorBGRA>>();
    value.colorMid = reader.read<AnimRef<ColorBGRA>>();
    value.colorEnd = reader.read<AnimRef<ColorBGRA>>();
    if (version <= 5) {
        value.colorMidTime = reader.read<f32>();
        value.alphaMidTime = reader.read<f32>();
    }

    // Physics
    value.drag = reader.read<f32>();
    if (version <= 6) {
        if (reader.read<u32>())
            value.additionalFlags |= RibbonAdditionalFlag::MassRandomize;
    }
    value.mass = reader.read<f32>();
    value.massRandom = reader.read<f32>();
    value.massSizeMultiplier = reader.read<f32>();
    if (version <= 6) {
        if (reader.read<u32>())
            value.additionalFlags |= RibbonAdditionalFlag::WorldSpace;
    }

    // Forces
    value.localForces = reader.read<u16>();
    value.worldForces = reader.read<u16>();
    value.localForcesFallback = reader.read<u16>();
    value.worldForcesFallback = reader.read<u16>();
    if (version >= 9) {
        value.worldForcesMassMultiplier = reader.read<f32>();
    }

    // Noise
    value.noiseAmplitude = reader.read<f32>();
    value.noiseFrequency = reader.read<f32>();
    value.noiseCoherence = reader.read<f32>();
    value.noiseEdge = reader.read<f32>();
    if (version >= 5) {
        value.indexPlusLength = reader.read<u32>();
    }

    // Shape
    value.emitterShape = reader.read<u32>();
    value.ribbonType = reader.read<RibbonType>();
    value.divisions = reader.read<f32>();
    value.edges = reader.read<u32>();
    value.innerRadius = reader.read<f32>();
    value.maxLength = reader.read<AnimRef<f32>>();
    if (version <= 6) {
        value.deprecated.unknown3fbae7d6 = reader.read<i32>();
    }

    // References
    visit(value.splineRibbons);
    value.active = reader.read<AnimRef<u32>>();

    // Flags & smoothing
    value.flags = static_cast<RibbonFlag>(reader.read<u32>());
    if (version >= 8) {
        value.sizeSmoothing = reader.read<InterpolationMode>();
        value.colorSmoothing = reader.read<InterpolationMode>();
    }

    // Collision & LOD
    value.friction = reader.read<f32>();
    value.bounce = reader.read<f32>();
    value.lodReduce = reader.read<u32>();
    value.lodCut = reader.read<u32>();

    // Variation channels
    // till v6: pitch then yaw, since v8: yaw then pitch
    if (version <= 6) {
        value.pitchType = reader.read<u32>();
        value.pitchAmplitude = reader.read<AnimRef<f32>>();
        value.pitchFrequency = reader.read<AnimRef<f32>>();
        value.yawType = reader.read<u32>();
        value.yawAmplitude = reader.read<AnimRef<f32>>();
        value.yawFrequency = reader.read<AnimRef<f32>>();
    } else {
        value.yawType = reader.read<u32>();
        value.yawAmplitude = reader.read<AnimRef<f32>>();
        value.yawFrequency = reader.read<AnimRef<f32>>();
        value.pitchType = reader.read<u32>();
        value.pitchAmplitude = reader.read<AnimRef<f32>>();
        value.pitchFrequency = reader.read<AnimRef<f32>>();
    }

    // length, scale, alpha variation
    value.speedType = reader.read<u32>();
    value.speedAmplitude = reader.read<AnimRef<f32>>();
    value.speedFrequency = reader.read<AnimRef<f32>>();
    value.sizeType = reader.read<u32>();
    value.sizeAmplitude = reader.read<AnimRef<f32>>();
    value.sizeFrequency = reader.read<AnimRef<f32>>();
    value.alphaType = reader.read<u32>();
    value.alphaAmplitude = reader.read<AnimRef<f32>>();
    value.alphaFrequency = reader.read<AnimRef<f32>>();

    // Parent velocity & phase
    value.particleVelocity = reader.read<AnimRef<f32>>();
    value.overlay = reader.read<AnimRef<f32>>();
}

void BinaryParseVisitor::visit(Projector& value, u32 version) {
    (void)version;
    value.projectionType = static_cast<ProjectionType>(reader.read<u32>());
    value.bone = reader.read<u32>();
    value.materialReferenceIndex = reader.read<u32>();
    value.offset = reader.read<AnimRef<Vector3f>>();
    value.pitch = reader.read<AnimRef<f32>>();
    value.yaw = reader.read<AnimRef<f32>>();
    value.roll = reader.read<AnimRef<f32>>();
    value.fieldOfView = reader.read<AnimRef<f32>>();
    value.aspectRatio = reader.read<AnimRef<f32>>();
    value.near = reader.read<AnimRef<f32>>();
    value.far = reader.read<AnimRef<f32>>();
    value.boxOffsetZBottom = reader.read<AnimRef<f32>>();
    value.boxOffsetZTop = reader.read<AnimRef<f32>>();
    value.boxOffsetXLeft = reader.read<AnimRef<f32>>();
    value.boxOffsetXRight = reader.read<AnimRef<f32>>();
    value.boxOffsetYFront = reader.read<AnimRef<f32>>();
    value.boxOffsetYBack = reader.read<AnimRef<f32>>();
    value.falloff = reader.read<f32>();
    value.alphaInit = reader.read<f32>();
    value.alphaMid = reader.read<f32>();
    value.alphaEnd = reader.read<f32>();
    value.lifetimeAttack = reader.read<f32>();
    value.lifetimeAttackTo = reader.read<f32>();
    value.lifetimeHold = reader.read<f32>();
    value.lifetimeHoldTo = reader.read<f32>();
    value.lifetimeDecay = reader.read<f32>();
    value.lifetimeDecayTo = reader.read<f32>();
    value.attenuationDistance = reader.read<f32>();
    value.active = reader.read<AnimRef<u32>>();
    value.layer = reader.read<u32>();
    value.lodReduce = reader.read<u32>();
    value.lodCut = reader.read<u32>();
    value.flags = static_cast<ProjectorFlag>(reader.read<u32>());
}

void BinaryParseVisitor::visit(Force& value, u32 version) {
    (void)version;
    value.forceType = static_cast<ForceType>(reader.read<u32>());
    value.forceShape = static_cast<ForceShape>(reader.read<u32>());
    value.unknown = reader.read<u32>();
    value.boneIndex = reader.read<u32>();
    value.flags = static_cast<ForceFlag>(reader.read<u32>());
    value.localChannels = reader.read<u32>();
    value.strength = reader.read<AnimRef<f32>>();
    value.width = reader.read<AnimRef<f32>>();
    value.height = reader.read<AnimRef<f32>>();
    value.length = reader.read<AnimRef<f32>>();
}

void BinaryParseVisitor::visit(Warp& value, u32 version) {
    (void)version;
    value.warpType = reader.read<u32>();
    value.boneIndex = reader.read<u32>();
    value.unknown = reader.read<u32>();
    value.radius = reader.read<AnimRef<f32>>();
    value.height = reader.read<AnimRef<f32>>();
    value.strength = reader.read<AnimRef<f32>>();
    value.angular = reader.read<AnimRef<f32>>();
    value.axial = reader.read<AnimRef<f32>>();
    value.radial = reader.read<AnimRef<f32>>();
}

void BinaryParseVisitor::visit(ConvexHullHalfEdge& value, u32 version) {
    (void)version;
    value.type = reader.read<u8>();
    value.faceIndex = reader.read<u8>();
    value.vertexIndex = reader.read<u8>();
    value.nextAroundVertex = reader.read<u8>();
}

void BinaryParseVisitor::visit(PhysicsMeshNormal& value, u32 version) {
    (void)version;
    value.normal = reader.read<Vector3f>();
}

void BinaryParseVisitor::visit(PhysicsMeshTriangle& value, u32 version) {
    (void)version;
    value.vertexIndex0 = reader.read<u32>();
    value.vertexIndex1 = reader.read<u32>();
    value.vertexIndex2 = reader.read<u32>();
    value.edgeIndex0 = reader.read<u32>();
    value.edgeIndex1 = reader.read<u32>();
    value.edgeIndex2 = reader.read<u32>();
    value.reserved = reader.read<u16>();
    value.flags = reader.read<u16>();
}

void BinaryParseVisitor::visit(PhysicsMeshEdge& value, u32 version) {
    (void)version;
    value.edgeType = reader.read<u32>();
    value.vertexA = reader.read<u32>();
    value.vertexB = reader.read<u32>();
    value.faceA = reader.read<u32>();
    value.faceB = reader.read<u32>();
}

void BinaryParseVisitor::visit(PhysicsShape& value, u32 version) {
    (void)version;
    value.transform = reader.read<Matrix44f>();
    if (version <= 1) {
        value.collisionMargin = reader.read<f32>();
        value.shapeType = static_cast<PhysicsShapeType>(reader.read<u8>());
        reader.skip(3);
        visit(value.deprecated.v1.legacyVertices);
        visit(value.deprecated.v1.unknown0);
        visit(value.deprecated.v1.faceIndices);
        visit(value.deprecated.v1.planeEquations);
        value.deprecated.v1.halfExtents = reader.read<Vector3f>();
        return;
    }

    // Version 2+
    value.shapeType = static_cast<PhysicsShapeType>(reader.read<u8>());
    reader.skip(3);

    value.oldSizes = reader.read<Vector3f>();

    value.reserved0 = readReferenceFunc();
    value.shapeDimensions = reader.read<Vector3f>();
    visit(value.hullFaceNormals);
    visit(value.hullVertexPositions);
    visit(value.hullHalfEdges);
    visit(value.hullVertexFaceIndices);
    value.hullCenter = reader.read<Vector3f>();
    value.hullFaceNormalCount = reader.read<u32>();
    value.hullVertexCount = reader.read<u32>();
    value.hullHalfEdgeCount = reader.read<u32>();
    value.hullUnknown0 = reader.read<f32>();
    value.hullUnknown1 = reader.read<f32>();

    if (version >= 3) {
        visit(value.meshFaceNormals);
        visit(value.meshVertexPositions);
        visit(value.meshFaceIndices16);
        visit(value.meshFaceIndices32);
    }
    value.meshBoundsCenter = reader.read<Vector3f>();
    value.meshBoundsExtent = reader.read<Vector3f>();
    value.meshTolerance = reader.read<Vector3f>();
    if (version == 2) {
        visit(value.deprecated.v2.meshFaceNormals);
        visit(value.deprecated.v2.meshVertexPositions);
        visit(value.deprecated.v2.unknown);
        visit(value.deprecated.v2.unknown2);
    }
    value.meshNormalCount = reader.read<u32>();
    value.meshVertexCount = reader.read<u32>();
    value.meshFaceIndex16Count = reader.read<u32>();
    value.meshFaceIndex32Count = reader.read<u32>();
    value.meshUnknown1 = reader.read<u32>();
    value.meshReserved = reader.read<u32>();
    value.meshTreeDepth = reader.read<u32>();
    value.meshCollisionMargin = reader.read<f32>();
}

void BinaryParseVisitor::visit(RigidBody& value, u32 version) {
    if (version <= 2) {
        // Legacy Havok-era layout: 80 bytes base + Ref<PHSH> + 12 bytes post-ref
        value.density = reader.read<f32>();
        value.friction = reader.read<f32>();
        value.restitution = reader.read<f32>();
        value.linearDamping = reader.read<f32>();
        value.angularDamping = reader.read<f32>();
        value.gravityScale = reader.read<f32>();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                value.deprecated.inertiaTensor[r][c] = reader.read<f32>();
        value.parentBoneIndex = reader.read<u16>();
        value.deprecated.boneIndex = reader.read<u16>();
        value.deprecated.reserved = reader.readArray<u32, 4>();
    } else {
        value.simulationType = reader.read<u16>();
        value.parentBoneIndex = reader.read<u16>();
        value.physicsType = reader.read<u32>();
        value.density = reader.read<f32>();
        value.friction = reader.read<f32>();
        value.restitution = reader.read<f32>();
        value.linearDamping = reader.read<f32>();
        value.angularDamping = reader.read<f32>();
        value.gravityScale = reader.read<f32>();
        if (version >= 4) {
            value.dynamicState = reader.read<AnimRef<u32>>();
            value.dynamicBlendOut = reader.read<f32>();
        }
    }
    visit(value.rigidBodyShape);
    value.flags = static_cast<RigidBodyFlag>(reader.read<u32>());
    value.localForces = reader.read<u16>();
    value.worldForces = reader.read<u16>();
    value.priority = reader.read<u32>();
}

void BinaryParseVisitor::visit(PhysicsJoint& value, u32 version) {
    (void)version;
    value.jointType = reader.read<u32>();
    value.boneIndex1 = reader.read<u32>();
    value.boneIndex2 = reader.read<u32>();
    value.matrixBody1 = reader.read<Matrix44f>();
    value.matrixBody2 = reader.read<Matrix44f>();
    value.enableLimits = reader.read<u32>();
    value.limitMin = reader.read<f32>();
    value.limitMax = reader.read<f32>();
    value.coneAngle = reader.read<f32>();
    value.enableFriction = reader.read<u32>();
    value.friction = reader.read<f32>();
    value.dampingRatio = reader.read<f32>();
    value.angularFrequency = reader.read<f32>();
    value.breakThreshold = reader.read<f32>();
    value.enableShape = reader.read<u8>();
    reader.skip(3);
}

void BinaryParseVisitor::visit(PhysicsConstraint& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.rigidBody1 = reader.read<u16>();
    value.rigidBody2 = reader.read<u16>();
    value.flags = reader.read<Flag>();
    value.breakForce = reader.read<f32>();
}

void BinaryParseVisitor::visit(ClothCollider& value, u32 version) {
    (void)version;
    value.transform = reader.read<Matrix44f>();
    value.radius = reader.read<f32>();
    value.height = reader.read<f32>();
    value.padding = reader.read<u32>();
}

void BinaryParseVisitor::visit(ClothProxy& value, u32 version) {
    (void)version;
    value.proxyIndex = reader.read<u32>();
    value.clothIndex = reader.read<u32>();
    visit(value.proxyVertices);
    visit(value.proxyWeights);
}

void BinaryParseVisitor::visit(ClothPhysics& value, u32 version) {
    (void)version;
    value.clothMeshCount = reader.read<u32>();
    value.skinBoneCount = reader.read<u32>();
    visit(value.skinBones);
    visit(value.simEnabled);
    visit(value.vertexBones);
    visit(value.vertexWeights);
    visit(value.colliders);
    visit(value.proxies);
    value.density = reader.read<f32>();
    value.tracking = reader.read<f32>();
    value.stretchStiffness = reader.read<f32>();
    value.horizontalStiffness = reader.read<f32>();
    value.bendingStiffness = reader.read<f32>();
    value.damping = reader.read<f32>();
    value.friction = reader.read<f32>();
    value.gravity = reader.read<f32>();
    value.explosionScale = reader.read<f32>();
    value.windScale = reader.read<f32>();
    value.shearStiffness = reader.read<f32>();
    value.dragFactor = reader.read<f32>();
    value.liftFactor = reader.read<f32>();
    value.sphereStiffness = reader.read<f32>();
    if (version >= 4) {
        value.flatten = reader.read<u32>();
        value.active = reader.read<AnimRef<u32>>();
        value.useSkinCollision = reader.read<u32>();
        value.skinOffset = reader.read<f32>();
        value.skinExponent = reader.read<f32>();
        value.skinStiffness = reader.read<f32>();
        value.localChannels = reader.read<u32>();
        value.localWind = reader.read<Vector3f>();
    }
}

void BinaryParseVisitor::visit(HitTestShape& value, u32 version) {
    setStructureVersion(value, version);
    (void)version;
    value.shapeType = static_cast<HitTestShapeType>(reader.read<u32>());
    value.boneIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.transform = reader.read<Matrix44f>();
    visit(value.vertexPositions);
    visit(value.faceIndices);
    value.sizeX = reader.read<f32>();
    value.sizeY = reader.read<f32>();
    value.sizeZ = reader.read<f32>();
}

void BinaryParseVisitor::visit(AttachmentVolume& value, u32 version) {
    (void)version;
    value.bone1 = reader.read<u32>();
    value.bone2 = reader.read<u32>();
    value.shapeType = static_cast<HitTestShapeType>(reader.read<u32>());
    value.boneIndex = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.transform = reader.read<Matrix44f>();
    visit(value.vertexPositions);
    visit(value.faceIndices);
    value.sizeX = reader.read<f32>();
    value.sizeY = reader.read<f32>();
    value.sizeZ = reader.read<f32>();
}

void BinaryParseVisitor::visit(TriggerData& value, u32 version) {
    (void)version;
    visit(value.dataIndices);
    visit(value.name);
}

void BinaryParseVisitor::visit(TurretBehavior& value, u32 version) {
    value.transform = reader.read<Matrix44f>();
    if (version >= 4) {
        value.unknown1 = reader.read<Vector4f>();
        value.unknown2 = reader.read<Vector4f>();
    }
    value.boneIndex = reader.read<u16>();
    value.useAsMainTurret = reader.read<u8>();
    value.turretGroupId = reader.read<u8>();
    value.yawLimited = reader.read<u32>();
    value.yawMin = reader.read<f32>();
    value.yawMax = reader.read<f32>();
    if (version >= 4) {
        value.yawWeight = reader.read<f32>();
    }
    value.pitchLimited = reader.read<u32>();
    value.pitchMin = reader.read<f32>();
    value.pitchMax = reader.read<f32>();
    if (version >= 4) {
        value.pitchWeight = reader.read<f32>();
    }
    value.unknown3 = reader.read<f32>();
    value.unknown4 = reader.read<f32>();
    value.mainBoneOffset = reader.read<Vector3f>();
}

void BinaryParseVisitor::visit(BillboardBehavior& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.boneIndex = reader.read<u16>();
    value.billboardType = reader.read<u8>();
    value.cameraLookAt = reader.read<u8>();
    value.up = reader.read<Quaternion>();
    value.forward = reader.read<Quaternion>();
}

void BinaryParseVisitor::visit(IKJoint& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.boneIndex1 = reader.read<u16>();
    value.boneIndex2 = reader.read<u16>();
    value.raycastUp = reader.read<f32>();
    value.raycastDown = reader.read<f32>();
    value.maxSpeed = reader.read<f32>();
    value.goalThreshold = reader.read<f32>();
}

void BinaryParseVisitor::visit(IKTwoJoint& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.boneBase = reader.read<u16>();
    value.boneTarget = reader.read<u16>();
    value.boneEnd = reader.read<u16>();
    value.padding = reader.read<u16>();
    value.hingeAxis = reader.read<Vector3f>();
    value.maxAngleInner = reader.read<f32>();
    value.maxAngleOuter = reader.read<f32>();
    value.searchUp = reader.read<f32>();
    value.searchDown = reader.read<f32>();
}

void BinaryParseVisitor::visit(IKCCD& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.boneBase = reader.read<u16>();
    value.boneTarget = reader.read<u16>();
    value.searchUp = reader.read<f32>();
    value.searchDown = reader.read<f32>();
}

void BinaryParseVisitor::visit(OneBoneSolver& value, u32 version) {
    (void)version;
    visit(value.dependents);
    value.bone = reader.read<u16>();
    value.boneFallback = reader.read<u16>();
    value.flags = reader.read<Flag>();
    value.maxAngle = reader.read<f32>();
}

void BinaryParseVisitor::visit(ShadowBox& value, u32 version) {
    (void)version;
    value.matrix = reader.read<Matrix44f>();
}

void BinaryParseVisitor::visit(ViewVolume& value, u32 version) {
    (void)version;
    value.nodeIndex = reader.read<u32>();
    value.size = reader.read<AnimRef<Vector3f>>();
}

void BinaryParseVisitor::visit(TrailingModel& value, u32 version) {
    (void)version;
    visit(value.vectors);
    value.param0 = reader.read<f32>();
    value.param1 = reader.read<f32>();
    value.animFloat0 = reader.read<AnimRef<f32>>();
    value.animFloat1 = reader.read<AnimRef<f32>>();
    value.flag = reader.read<u32>();
    value.reserved0 = reader.read<u32>();
    value.reserved1 = reader.read<u32>();
}

template <typename T>
void BinaryParseVisitor::visit(AnimBlock<T>& block, u32 version) {
    (void)version;
    visit(block.timestamps);
    block.flags = reader.read<u32>();
    block.endFrame = reader.read<u32>();
    visit(block.keys);
}

template <typename T>
void BinaryParseVisitor::visit(std::vector<T>& container) {
    Reference ref = readReferenceFunc();
    if (ref.entries == 0) {
        container.clear();
        return;
    }

    indexUsed[ref.index] = true;
    assert(indexTable[ref.index].tag == ChunkTagTraits<T>::value);
    const auto currentPos = reader.getPosition();
    reader.setPosition(indexTable[ref.index].offset);
    const u32 version = indexTable[ref.index].version;
    if constexpr (ChunkTagTraits<T>::is_trivial) {
        container = reader.read<std::vector<T>>(ref.entries);
        if constexpr (has_set_version<T>::value) {
            for (auto& element : container) {
                setStructureVersion(element, version);
            }
        }
    } else {
        container.clear();
        container.reserve(ref.entries);
        for (size_t i = 0; i < ref.entries; ++i) {
            container.emplace_back();
            T& element = container.back();
            setStructureVersion(element, version);
            this->visit(element, version);
        }
    }
    reader.setPosition(currentPos);
}

template <typename T>
void BinaryParseVisitor::visit(std::optional<T>& container) {
    Reference ref = readReferenceFunc();
    if (ref.entries == 0) {
        container = std::nullopt;
        return;
    }

    assert(indexTable[ref.index].tag == ChunkTagTraits<T>::value);
    indexUsed[ref.index] = true;
    const auto currentPos = reader.getPosition();
    reader.setPosition(indexTable[ref.index].offset);
    if constexpr (std::is_trivially_copyable_v<T>) {
        container = reader.read<T>();
        if constexpr (has_set_version<T>::value) {
            setStructureVersion(*container, indexTable[ref.index].version);
        }
    } else {
        container.emplace();
        const u32 version = indexTable[ref.index].version;
        setStructureVersion(*container, version);
        this->visit(*container, version);
    }
    reader.setPosition(currentPos);
}

void BinaryParseVisitor::visit(std::string& str) {
    Reference ref = readReferenceFunc();
    if (ref.entries == 0) {
        str.clear();
        return;
    }

    const auto currentPos = reader.getPosition();
    indexUsed[ref.index] = true;
    reader.setPosition(indexTable[ref.index].offset);
    assert(indexTable[ref.index].tag == ChunkTagTraits<char>::value);
    str = reader.readString(ref.entries, false);
    reader.setPosition(currentPos);
}

void BinaryParseVisitor::visit(std::string& str, u32 version) {
    (void)version;
    visit(str);
}

} // namespace m3
} // namespace whiteout

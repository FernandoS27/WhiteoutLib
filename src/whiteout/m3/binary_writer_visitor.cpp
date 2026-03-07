// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../common/binary_writer.h"
#include "binary_writer_visitor.h"
#include "chunk_traits.h"

#include <algorithm>
#include <numeric>

namespace whiteout {
namespace m3 {

using common::BinaryWriter;

namespace {

template <typename T, typename = void>
struct has_get_version : std::false_type {};

template <typename T>
struct has_get_version<T, std::void_t<decltype(std::declval<T&>().getVersion())>> : std::true_type {
};

template <typename T>
u32 getStructureVersion(const T& value) {
    if constexpr (has_get_version<T>::value) {
        return value.getVersion();
    }
    return ChunkTagTraits<T>::max_version;
}

} // namespace

BinaryWriterVisitor::BinaryWriterVisitor(BinaryWriter& writer, bool ismd33)
    : writer(writer), ismd33(ismd33) {}

void BinaryWriterVisitor::write(const Model& model) {
    MD3Header header{};

    header.indexOffset = 0; // Placeholder, will be updated later
    header.indexCount = 0;  // Placeholder, will be updated later

    if (!ismd33) { // "MD34" or "MD33"
        writeReferenceFunc = [this](Reference& ref) { this->writer.write(ref); };
        header.magic = TAG_MD34;
    } else {
        writeReferenceFunc = [this](Reference& ref) {
            writer.write(ref.entries);
            writer.write(ref.index);
        };
        header.magic = TAG_MD33;
    }

    const auto write_header = [this, &header]() {
        writer.setPosition(0);
        writer.write(header.magic);
        writer.write(header.indexOffset);
        writer.write(header.indexCount);
        writeReferenceFunc(header.modelRef);
        writer.AlignTo(16, 0xAA);
    };

    indexTable.reserve(
        1024); // Avoid too many reallocations, as we will be adding entries one by one

    indexTable.emplace_back(header.magic, 0, 1, 0); // Placeholder for header reference entry
    auto modelRefIndex = indexTable.size();
    indexTable.emplace_back(ChunkTagTraits<Model>::value, 0, 1,
                            getStructureVersion(model)); // Placeholder for model reference entry
    header.modelRef.entries = 1;
    header.modelRef.index = modelRefIndex;

    write_header();
    deferredWrites.push_back([this, &model, modelRefIndex]() {
        const u32 modelRootOffset = writer.getPosition();
        indexTable[modelRefIndex].offset = modelRootOffset;
        visit(model, indexTable[modelRefIndex].version);
        writer.AlignTo(16, 0xAA);
        transferDeferredWrites(); // Use pre-prder only at root
    });

    while (!deferredWrites.empty()) {
        auto writeFunc = std::move(deferredWrites.front());
        deferredWrites.pop_front();
        writeFunc();
    }

    const u32 currentOffset = writer.getPosition();
    writer.setPosition(0);
    header.indexOffset = currentOffset;
    header.indexCount = indexTable.size();
    write_header();
    writer.setPosition(currentOffset);
    writer.write(indexTable);
}

void BinaryWriterVisitor::visit(const Model& model, u32 version) {
    visit(model.name);
    writer.write(model.flags);
    visit(model.sequences);
    visit(model.subTrackCollections);
    visit(model.animationGroups);
    visit(model.boneAnimationSets);
    writer.write(model.animationSplitCount);
    visit(model.animationStates);
    visit(model.bones);
    writer.write(model.skinBoneCount);
    visit(model.vertices, 0);
    visit(model.divisions);
    visit(model.boneLookup);
    writer.write(model.bounds);
    writer.write(model.collisionBounds);
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
        writer.write(model.m3aAnimHash);
    }
    if (version >= 23) {
        visit(model.m3aAnimHashes);
    }
}

void BinaryWriterVisitor::visit(const VertexBuffer& value, u32 version) {
    writer.write(value.flags);
    visit(value.data);
}

void BinaryWriterVisitor::visit(const Sequence& seq, u32 version) {
    writer.write(seq.id);
    writer.write(seq.index);
    visit(seq.name);
    writer.write(seq.startFrame);
    writer.write(seq.endFrame);
    writer.write(seq.moveSpeed);
    writer.write(seq.flags);
    writer.write(seq.frequency);
    writer.write(seq.replayStart);
    writer.write(seq.replayEnd);
    writer.write(seq.blendTime);
    if (version <= 1) {
        writer.write(seq.deprecated.unknown);
    }
    writer.write(seq.bounds);
    visit(seq.animationSets);
}

void BinaryWriterVisitor::visit(const SubTrackContainer& container, u32 version) {
    (void)version;
    visit(container.name);
    writer.write(container.runsConcurrent);
    writer.write(container.animPriority);
    writer.write(container.animationStateIndex);
    writer.write(container.padding);
    visit(container.animIds);
    visit(container.animRefs);
    writer.write(container.unknown);
    visit(container.sdev);
    visit(container.sd2v);
    visit(container.sd3v);
    visit(container.sd4q);
    visit(container.sdcc);
    visit(container.sdr3);
    visit(container.sdu8);
    visit(container.sds6);
    visit(container.sdu6);
    visit(container.sds3);
    visit(container.sdu3);
    visit(container.sdfg);
    visit(container.sdmb);
}

void BinaryWriterVisitor::visit(const AnimationGroup& group, u32 version) {
    (void)version;
    visit(group.name);
    visit(group.subtrackIndices);
}

void BinaryWriterVisitor::visit(const AnimationState& state, u32 version) {
    (void)version;
    visit(state.animIds);
    writer.write(state.unknown);
}

void BinaryWriterVisitor::visit(const BoneAnimationSet& set, u32 version) {
    (void)version;
    writer.write(set.flags);
    writer.write(set.animationSequenceIndex);
    writer.write(set.fallbackSequenceIndex);
    visit(set.name);
    visit(set.splitItems);
}

void BinaryWriterVisitor::visit(const Bone& bone, u32 version) {
    (void)version;
    writer.write(bone.unknown);
    visit(bone.name);
    writer.write(bone.flags);
    writer.write(bone.parentIndex);
    writer.write(bone.padding);
    writer.write(bone.position);
    writer.write(bone.rotation);
    writer.write(bone.scale);
    writer.write(bone.visibility);
}

void BinaryWriterVisitor::visit(const Region& region, u32 version) {
    writer.write(region.index);
    if (version >= 3) {
        writer.write(region.unknown);
        writer.write(region.firstVertex);
        writer.write(region.vertexCount);
        writer.write(region.firstIndex);
        writer.write(region.indexCount);
    } else {
        writer.write(static_cast<u16>(region.firstVertex));
        writer.write(static_cast<u16>(region.vertexCount));
    }
    writer.write(region.unknown2);
    writer.write(region.firstBoneLookup);
    writer.write(region.boneLookupCount);
    writer.write(region.padding);
    writer.write(region.boneWeightPairs);
    writer.write(region.boneIndexPairs);
    writer.write(region.rootBone);
    if (version >= 4) {
        writer.write(region.flags);
    }
    if (version >= 5) {
        writer.write(region.uvScale);
        writer.write(region.uvOffset);
    }
}

void BinaryWriterVisitor::visit(const Batch& batch, u32 version) {
    (void)version;
    writer.write(batch.unknown);
    writer.write(batch.regionIndex);
    writer.write(batch.unknown2);
    writer.write(batch.materialIndex);
    writer.write(batch.boneCount);
}

void BinaryWriterVisitor::visit(const MeshSection& section, u32 version) {
    (void)version;
    writer.write(section.nodeIndex);
    writer.write(section.bounds);
}

void BinaryWriterVisitor::visit(const MeshDivision& division, u32 version) {
    (void)version;
    visit(division.faces);
    visit(division.regions);
    visit(division.batches);
    visit(division.msec);
    writer.write(division.instances);
}

void BinaryWriterVisitor::visit(const InitialReference& ref, u32 version) {
    (void)version;
    writer.write(ref.matrix);
}

void BinaryWriterVisitor::visit(const AttachmentPoint& point, u32 version) {
    (void)version;
    writer.write(point.unknown);
    visit(point.name);
    writer.write(point.boneIndex);
}

void BinaryWriterVisitor::visit(const MaterialMap& map, u32 version) {
    (void)version;
    writer.write(map.materialType);
    writer.write(map.materialIndex);
}

void BinaryWriterVisitor::visit(const TextureLayer& layer, u32 version) {
    writer.write(layer.id);
    visit(layer.texturePath);
    writer.write(layer.color);
    writer.write(layer.flags);
    writer.write(layer.uvMapping);
    writer.write(layer.colorType);
    writer.write(layer.rgbMultiply);
    writer.write(layer.rgbAdd);
    writer.write(layer.pocTexture);
    if (version >= 24) {
        writer.write(layer.noiseAmplitude);
        writer.write(layer.noiseFrequency);
    }
    writer.write(layer.textureSource);
    writer.write(layer.aviFrameRate);
    writer.write(layer.aviStart);
    writer.write(layer.aviStop);
    writer.write(layer.aviLoop);
    writer.write(layer.aviSync);
    writer.write(layer.aviPlay);
    writer.write(layer.aviRestart);
    writer.write(layer.flipbookRows);
    writer.write(layer.flipbookColumns);
    writer.write(layer.currentFrame);
    writer.write(layer.uvOffset);
    writer.write(layer.uvAngle);
    writer.write(layer.uvTiling);
    writer.write(layer.wOffset);
    writer.write(layer.wTiling);
    writer.write(layer.mapAlpha);
    if (version >= 23) {
        writer.write(layer.triplanarOffset);
        writer.write(layer.triplanarScale);
    }
    writer.write(layer.uvSourceRelated);
    writer.write(layer.fresnelMode);
    writer.write(layer.fresnelExponent);
    writer.write(layer.fresnelMin);
    writer.write(layer.fresnelMax);
    if (version >= 25) {
        writer.write(layer.fresnelTranslation);
        writer.write(layer.fresnelMask);
        writer.write(layer.fresnelRotation);
    }
    writer.write(layer.uvDensity);
}

void BinaryWriterVisitor::visit(const StandardMaterial& material, u32 version) {
    visit(material.name);
    writer.write(material.additionalFlags);
    writer.write(material.flags);
    writer.write(material.blendMode);
    writer.write(material.priority);
    writer.write(material.rttChannels);
    writer.write(material.specularExponent);
    writer.write(material.depthBlendFalloff);
    writer.write(material.alphaTestThreshold);
    writer.write(material.hdrSpecularMultiplier);
    writer.write(material.hdrEmissiveMultiplier);
    if (version >= 20) {
        writer.write(material.hdrEnvironmentConstant);
        writer.write(material.hdrEnvironmentDiffuse);
        writer.write(material.hdrEnvironmentSpecular);
    }

    visit(material.diffuseLayer);
    visit(material.decalLayer);
    visit(material.specularLayer);
    if (version >= 16) {
        visit(material.glossLayer);
    }
    visit(material.emissiveLayer1);
    visit(material.emissiveLayer2);
    visit(material.environmentLayer);
    visit(material.environmentMaskLayer);
    visit(material.alphaLayer1);
    visit(material.alphaLayer2);
    visit(material.normalLayer);
    visit(material.heightLayer);
    visit(material.lightMapLayer);
    visit(material.ambientOcclusionLayer);
    if (version >= 19) {
        visit(material.normalBlend1MaskLayer);
        visit(material.normalBlend2MaskLayer);
        visit(material.normalBlend1Layer);
        visit(material.normalBlend2Layer);
    }

    writer.write(material.materialClass);
    writer.write(material.layerBlendMode);
    writer.write(material.emissiveBlendMode1);
    writer.write(material.emissiveBlendMode2);
    writer.write(material.specularMode);
    writer.write(material.parallaxHeight);
    writer.write(material.motionBlurAmount);
    if (version >= 19) {
        visit(material.normalBlendFactors);
    }
}

void BinaryWriterVisitor::visit(const DisplacementMaterial& material, u32 version) {
    (void)version;
    visit(material.name);
    writer.write(material.unknown);
    writer.write(material.strength);
    visit(material.normalMap);
    visit(material.strengthMap);
    writer.write(material.flags);
    writer.write(material.priority);
}

void BinaryWriterVisitor::visit(const CompositeSection& section, u32 version) {
    (void)version;
    writer.write(section.materialIndex);
    writer.write(section.mapMultiplier);
}

void BinaryWriterVisitor::visit(const CompositeMaterial& material, u32 version) {
    (void)version;
    visit(material.name);
    writer.write(material.priority);
    visit(material.sections);
}

void BinaryWriterVisitor::visit(const TerrainMaterial& material, u32 version) {
    visit(material.name);
    visit(material.terrainMap);
    if (version >= 1) {
        writer.write(material.unknown);
    }
}

void BinaryWriterVisitor::visit(const VolumeMaterial& material, u32 version) {
    (void)version;
    visit(material.name);
    writer.write(material.blendMode);
    writer.write(material.falloffType);
    writer.write(material.density);
    visit(material.colorMap);
    visit(material.noiseMap1);
    visit(material.noiseMap2);
    writer.write(material.alphaThreshold);
    writer.write(material.flags);
}

void BinaryWriterVisitor::visit(const HairMaterial& material, u32 version) {
    (void)version;
    visit(material.name);
    visit(material.layerBase);
    visit(material.layerSpecShift);
    visit(material.layerSpecNoise);
    visit(material.layerAO);
    writer.write(material.shiftPrimary);
    writer.write(material.shiftSecondary);
    writer.write(material.colorDiffuse);
    writer.write(material.colorSpec);
    writer.write(material.specExponent0);
    writer.write(material.specExponent1);
}

void BinaryWriterVisitor::visit(const VolumeNoiseMaterial& material, u32 version) {
    (void)version;
    visit(material.name);
    writer.write(material.falloffType);
    writer.write(material.drawTransparency);
    writer.write(material.density);
    writer.write(material.nearPlane);
    writer.write(material.falloff);
    visit(material.colorMap);
    visit(material.noiseMap1);
    visit(material.noiseMap2);
    writer.write(material.scrollRate);
    writer.write(material.position);
    writer.write(material.scale);
    writer.write(material.rotation);
    writer.write(material.alphaThreshold);
    writer.write(material.flags);
}

void BinaryWriterVisitor::visit(const CreepMaterial& material, u32 version) {
    visit(material.name);
    visit(material.maskMap);
    if (version >= 1) {
        writer.write(material.creepLow);
    }
}

void BinaryWriterVisitor::visit(const STBMaterial& material, u32 version) {
    (void)version;
    visit(material.name);
    visit(material.diffuseMap);
    visit(material.normalMap);
    visit(material.specularMap);
}

void BinaryWriterVisitor::visit(const ReflectionMaterial& material, u32 version) {
    visit(material.name);
    writer.write(material.unknown);
    writer.write(material.reflectionStrength);
    writer.write(material.displacementStrength);
    if (version >= 2) {
        writer.write(material.reflectionOffset);
        writer.write(material.blurAngle);
        writer.write(material.blurDistanceMax);
    }
    visit(material.reflectionMap);
    visit(material.displacementMap);
    if (version >= 2) {
        visit(material.blurMap);
    }
    writer.write(material.flags);
    if (version >= 3) {
        writer.write(material.unknown2);
    }
}

void BinaryWriterVisitor::visit(const SubFlare& flare, u32 version) {
    (void)version;
    writer.write(flare.index);
    writer.write(flare.position);
    writer.write(flare.sizeXY);
    writer.write(flare.scaleXY);
    writer.write(flare.fadeIn);
    writer.write(flare.fadeOut);
    writer.write(flare.colorAlpha);
    writer.write(flare.faceCenter);
    writer.write(flare.offset);
}

void BinaryWriterVisitor::visit(const SplineRibbon& ribbon, u32 version) {
    (void)version;
    writer.write(ribbon.emissionOffset);
    writer.write(ribbon.emissionVector);
    writer.write(ribbon.velocity);
    writer.write(ribbon.unknown01);
    writer.write(ribbon.boneIndex);
    writer.write(ribbon.velocityBaseFactor);
    writer.write(ribbon.velocityEndFactor);
    writer.write(ribbon.yawType);
    writer.write(ribbon.yawAmplitude);
    writer.write(ribbon.yawFrequency);
    writer.write(ribbon.pitchType);
    writer.write(ribbon.pitchAmplitude);
    writer.write(ribbon.pitchFrequency);
    writer.write(ribbon.velocityType);
    writer.write(ribbon.velocityAmplitude);
    writer.write(ribbon.velocityFrequency);
    writer.write(ribbon.yaw);
    writer.write(ribbon.pitch);
    writer.write(ribbon.unknown02);
    writer.write(ribbon.unknown03);
}

void BinaryWriterVisitor::visit(const LensFlare& flare, u32 version) {
    visit(flare.name);
    visit(flare.flareMap);
    visit(flare.maskMap);
    visit(flare.subFlares);
    writer.write(flare.columns);
    writer.write(flare.rows);
    writer.write(flare.distanceFade);
    if (version >= 3) {
        visit(flare.libName);
    }
    writer.write(flare.intensity);
    if (version >= 3) {
        writer.write(flare.color);
        writer.write(flare.hdr);
        writer.write(flare.size);
    }
}

void BinaryWriterVisitor::visit(const MaterialAddData& data, u32 version) {
    visit(data.keyName);
    visit(data.keyHash);
    if (version >= 2) {
        visit(data.extraHash);
    }
    visit(data.valuePath);
    visit(data.valueData);
    for (const auto& reservedRef : data.reserved) {
        Reference ref = reservedRef;
        writeReferenceFunc(ref);
    }
    writer.write(data.frequency);
    writer.write(data.intensity);
    writer.write(data.holdTime);
    writer.write(data.randomHash);
    writer.write(data.animationType);
    writer.write(data.padding0);
    writer.write(data.loopCount);
    writer.write(data.flags);
    writer.write(data.subType);
    writer.write(data.configA);
    writer.write(data.configB);
    if (version >= 3) {
        writer.write(data.extraId0);
        writer.write(data.extraId1);
    }
}

void BinaryWriterVisitor::visit(const Light& light, u32 version) {
    (void)version;
    writer.write(light.lightType);
    writer.write(light.boneIndex);
    writer.write(light.flags);
    writer.write(light.lodCut);
    writer.write(light.shadowLodCut);
    writer.write(light.diffuseColor);
    writer.write(light.intensityMultiplier);
    writer.write(light.specularColor);
    writer.write(light.specularMultiplier);
    writer.write(light.decay);
    writer.write(light.attenuationEnd);
    writer.write(light.attenuationStart);
    writer.write(light.hotSpot);
    writer.write(light.falloff);
}

void BinaryWriterVisitor::visit(const Camera& camera, u32 version) {
    writer.write(camera.boneIndex);
    visit(camera.name);

    if (version >= 2) {
        writer.write(camera.fieldOfView);
        writer.write(camera.useVerticalFOV);
    }

    if (version == 2) {
        // Beta v2: farClip/nearClip are plain f32 (written from AnimRef.initValue)
        writer.write(camera.farClip.initValue);
        writer.write(camera.nearClip.initValue);
    }

    if (version >= 5) {
        writer.write(camera.dofType);
    }

    if (version >= 3) {
        writer.write(camera.farClip);
        writer.write(camera.nearClip);
    }

    if (version >= 2) {
        writer.write(camera.shadowClipDistance);
        writer.write(camera.focusDistance);
        writer.write(camera.farFocusRange);
        writer.write(camera.nearFocusRange);
    }

    if (version >= 4) {
        writer.write(camera.nearFalloffStart);
        writer.write(camera.nearFalloffEnd);
    }

    if (version >= 2) {
        writer.write(camera.dofAmount);
    }

    if (version >= 5) {
        writer.write(camera.bokehFStop);
        writer.write(camera.bokehMaxCoCDiameter);
    }
}

void BinaryWriterVisitor::visit(const Event& event, u32 version) {
    visit(event.name);
    writer.write(event.unknown);
    writer.write(event.boneIndex);
    writer.write(event.padding);
    writer.write(event.transform);
    writer.write(event.eventType);
    visit(event.optionString);
    if (version >= 1) {
        writer.write(event.rttChannelIndex);
    }
    if (version >= 2) {
        writer.write(event.extraParameter);
    }
}

void BinaryWriterVisitor::visit(const ShadowBox& box, u32 version) {
    (void)version;
    writer.write(box.matrix);
}

void BinaryWriterVisitor::visit(const ParticleEmitter& emitter, u32 version) {
    writer.write(emitter.boneIndex);
    writer.write(emitter.materialIndex);
    if (version >= 17) {
        writer.write(emitter.additionalFlags);
    }

    writer.write(emitter.initialSpeed);
    writer.write(emitter.initialSpeedRandom);
    if (version <= 14) {
        writer.write(emitter.deprecated.emitSpeedRandomize);
    }
    writer.write(emitter.initialYaw);
    writer.write(emitter.initialPitch);
    writer.write(emitter.initialHorizontal);
    writer.write(emitter.initialVertical);

    writer.write(emitter.lifetime);
    writer.write(emitter.lifetimeRandom);
    if (version <= 14) {
        writer.write(emitter.deprecated.lifespanRandomize);
    }

    writer.write(emitter.killRadius);
    writer.write(emitter.gravityX);
    writer.write(emitter.gravityY);
    writer.write(emitter.gravity);

    if (version >= 12) {
        writer.write(emitter.sizeMidTime);
        writer.write(emitter.colorMidTime);
        writer.write(emitter.alphaMidTime);
        writer.write(emitter.rotationMidTime);
    }

    if (version >= 14) {
        writer.write(emitter.sizeMidHoldTime);
        writer.write(emitter.colorMidHoldTime);
        writer.write(emitter.alphaMidHoldTime);
        writer.write(emitter.rotationMidHoldTime);
    }

    writer.write(emitter.sizeAnimation);
    if (version <= 11) {
        writer.write(emitter.deprecated.sizeMidTimeLegacy);
    }
    writer.write(emitter.rotationAnimation);
    if (version <= 11) {
        writer.write(emitter.deprecated.rotationMidTimeLegacy);
    }
    writer.write(emitter.colorStart);
    writer.write(emitter.colorMid);
    writer.write(emitter.colorEnd);
    if (version <= 11) {
        writer.write(emitter.deprecated.colorMidTimeLegacy);
        writer.write(emitter.deprecated.alphaMidTimeLegacy);
    }

    writer.write(emitter.drag);
    if (version <= 14) {
        writer.write(emitter.deprecated.massRandomizeLegacy);
    }
    writer.write(emitter.mass);
    writer.write(emitter.massRandom);
    if (version >= 12) {
        writer.write(emitter.massSizeMultiplier);
    }
    if (version <= 14) {
        writer.write(emitter.deprecated.worldSpaceLegacy);
    }

    writer.write(emitter.localForces);
    writer.write(emitter.worldForces);
    writer.write(emitter.localForcesFallback);
    writer.write(emitter.worldForcesFallback);
    if (version >= 24) {
        writer.write(emitter.worldForcesMassMultiplier);
    }

    writer.write(emitter.noiseAmplitude);
    writer.write(emitter.noiseFrequency);
    writer.write(emitter.noiseCoherence);
    writer.write(emitter.noiseEdge);
    if (version <= 11) {
        writer.write(emitter.deprecated.unknown31f2da8);
    }
    if (version >= 11) {
        writer.write(emitter.indexPlusLength);
    }

    writer.write(emitter.maxParticles);
    writer.write(emitter.emissionRate);
    writer.write(emitter.emitterShape);
    writer.write(emitter.shapeOuter);
    writer.write(emitter.shapeInner);
    writer.write(emitter.outerRadius);
    writer.write(emitter.innerRadius);
    if (version >= 14) {
        visit(emitter.shapeRegions);
    }

    writer.write(emitter.velocityType);
    writer.write(emitter.sizeRandomEnable);
    writer.write(emitter.sizeRandomAnimation);
    writer.write(emitter.rotationRandomEnable);
    writer.write(emitter.rotationRandomAnimation);
    writer.write(emitter.colorRandomEnable);
    writer.write(emitter.colorStartRandom);
    writer.write(emitter.colorMidRandom);
    writer.write(emitter.colorEndRandom);
    writer.write(emitter.alphaRandomEnable);

    writer.write(emitter.squirtAmount);
    writer.write(emitter.flipbookStartInitIndex);
    writer.write(emitter.flipbookStartStopIndex);
    writer.write(emitter.flipbookEndInitIndex);
    writer.write(emitter.flipbookEndStopIndex);
    writer.write(emitter.flipbookMidTime);
    writer.write(emitter.flipbookColumns);
    writer.write(emitter.flipbookRows);
    if (version >= 12) {
        writer.write(emitter.flipbookColumnFraction);
        writer.write(emitter.flipbookRowFraction);
    }

    writer.write(emitter.bounce);
    writer.write(emitter.friction);
    writer.write(emitter.collisionSpawnIndex);
    writer.write(emitter.collisionSpawnMin);
    writer.write(emitter.collisionSpawnMax);
    writer.write(emitter.collisionSpawnChance);
    writer.write(emitter.collisionSpawnEnergy);
    writer.write(emitter.collisionDieBounce);

    writer.write(emitter.instanceType);
    writer.write(emitter.tailLength);
    writer.write(emitter.instanceAngle);
    if (version >= 17) {
        writer.write(emitter.instanceDistance);
    }

    writer.write(emitter.pitchType);
    writer.write(emitter.pitchAmplitude);
    writer.write(emitter.pitchFrequency);
    writer.write(emitter.yawType);
    writer.write(emitter.yawAmplitude);
    writer.write(emitter.yawFrequency);
    writer.write(emitter.speedType);
    writer.write(emitter.speedAmplitude);
    writer.write(emitter.speedFrequency);
    writer.write(emitter.sizeType);
    writer.write(emitter.sizeAmplitude);
    writer.write(emitter.sizeFrequency);
    writer.write(emitter.alphaType);
    writer.write(emitter.alphaAmplitude);
    writer.write(emitter.alphaFrequency);
    writer.write(emitter.colorType);
    writer.write(emitter.colorAmplitude);
    writer.write(emitter.colorFrequency);
    writer.write(emitter.rotationType);
    writer.write(emitter.rotationAmplitude);
    writer.write(emitter.rotationFrequency);
    writer.write(emitter.horizontalType);
    writer.write(emitter.horizontalAmplitude);
    writer.write(emitter.horizontalFrequency);
    writer.write(emitter.verticalType);
    writer.write(emitter.verticalAmplitude);
    writer.write(emitter.verticalFrequency);

    writer.write(emitter.particleVelocity);
    if (version >= 22) {
        writer.write(emitter.phaseShift);
    }

    writer.write(emitter.flags);
    if (version >= 18) {
        writer.write(emitter.rotationFlags);
    }

    if (version >= 14) {
        writer.write(emitter.colorSmoothing);
        writer.write(emitter.sizeSmoothing);
        writer.write(emitter.rotationSmoothing);
    }

    if (version >= 17) {
        writer.write(emitter.alphaThreshold);
        writer.write(emitter.uvOffset);
        writer.write(emitter.uvAngle);
        writer.write(emitter.uvTiling);
    }

    visit(emitter.splatLineData);

    writer.write(emitter.windMultiplier);
    writer.write(emitter.lodReduce);
    writer.write(emitter.lodCut);

    writer.write(emitter.lowerBound);
    writer.write(emitter.upperBound);

    writer.write(emitter.trailLinkIndex);
    writer.write(emitter.trailChance);
    writer.write(emitter.trailEmissionRate);

    writer.write(emitter.splatProjectionIndex);
    writer.write(emitter.splatChance);

    visit(emitter.modelPaths);
    visit(emitter.copyIndices);

    if (version >= 23) {
        writer.write(emitter.unknown9a7afdf2);
        writer.write(emitter.unknown87d57a7a);
    }
}

void BinaryWriterVisitor::visit(const ParticleEmitterCopy& copy, u32 version) {
    (void)version;
    writer.write(copy.emissionRate);
    writer.write(copy.squirtAmount);
    writer.write(copy.boneIndex);
}

void BinaryWriterVisitor::visit(const RibbonEmitter& emitter, u32 version) {
    writer.write(emitter.boneIndex);
    writer.write(emitter.boneIndexFallback);
    writer.write(emitter.materialIndex);
    if (version >= 8) {
        writer.write(emitter.additionalFlags);
    }

    writer.write(emitter.initialSpeed);
    writer.write(emitter.initialSpeedRandom);
    if (version <= 6) {
        writer.write(emitter.deprecated.speedRandomize);
    }

    // till v6: pitch then yaw, since v8: yaw then pitch
    if (version <= 6) {
        writer.write(emitter.initialPitch);
        writer.write(emitter.initialYaw);
    } else {
        writer.write(emitter.initialYaw);
        writer.write(emitter.initialPitch);
    }
    writer.write(emitter.initialHorizontal);
    writer.write(emitter.initialVertical);

    writer.write(emitter.lifetime);
    writer.write(emitter.lifetimeRandom);
    if (version <= 6) {
        writer.write(emitter.deprecated.lifespanRandomize);
    }

    writer.write(emitter.killRadius);
    writer.write(emitter.gravityX);
    writer.write(emitter.gravityY);
    writer.write(emitter.gravity);

    if (version >= 6) {
        writer.write(emitter.sizeMidTime);
        writer.write(emitter.colorMidTime);
        writer.write(emitter.alphaMidTime);
        writer.write(emitter.rotationMidTime);
    }
    if (version >= 8) {
        writer.write(emitter.sizeMidHoldTime);
        writer.write(emitter.colorMidHoldTime);
        writer.write(emitter.alphaMidHoldTime);
        writer.write(emitter.rotationMidHoldTime);
    }

    writer.write(emitter.sizeAnimation);
    if (version <= 5) {
        writer.write(emitter.deprecated.sizeMidTimeLegacy);
    }
    writer.write(emitter.rotationAnimation);
    if (version <= 5) {
        writer.write(emitter.deprecated.rotationMidTimeLegacy);
    }
    writer.write(emitter.colorStart);
    writer.write(emitter.colorMid);
    writer.write(emitter.colorEnd);
    if (version <= 5) {
        writer.write(emitter.deprecated.colorMidTimeLegacy);
        writer.write(emitter.deprecated.alphaMidTimeLegacy);
    }

    writer.write(emitter.drag);
    if (version <= 6) {
        writer.write(emitter.deprecated.massRandomize);
    }
    writer.write(emitter.mass);
    writer.write(emitter.massRandom);
    writer.write(emitter.massSizeMultiplier);
    if (version <= 6) {
        writer.write(emitter.deprecated.worldSpace);
    }

    writer.write(emitter.localForces);
    writer.write(emitter.worldForces);
    writer.write(emitter.localForcesFallback);
    writer.write(emitter.worldForcesFallback);
    if (version >= 9) {
        writer.write(emitter.worldForcesMassMultiplier);
    }

    writer.write(emitter.noiseAmplitude);
    writer.write(emitter.noiseFrequency);
    writer.write(emitter.noiseCoherence);
    writer.write(emitter.noiseEdge);
    if (version >= 5) {
        writer.write(emitter.indexPlusLength);
    }

    writer.write(emitter.emitterShape);
    writer.write(emitter.basedSource);
    writer.write(emitter.divisions);
    writer.write(emitter.edges);
    writer.write(emitter.innerRadius);
    writer.write(emitter.maxLength);
    if (version <= 6) {
        writer.write(emitter.deprecated.unknown3fbae7d6);
    }

    visit(emitter.splineRibbons);
    writer.write(emitter.active);

    writer.write(emitter.flags);
    if (version >= 8) {
        writer.write(emitter.sizeSmoothing);
        writer.write(emitter.colorSmoothing);
    }

    writer.write(emitter.friction);
    writer.write(emitter.bounce);
    writer.write(emitter.lodReduce);
    writer.write(emitter.lodCut);

    if (version <= 6) {
        writer.write(emitter.pitchType);
        writer.write(emitter.pitchAmplitude);
        writer.write(emitter.pitchFrequency);
        writer.write(emitter.yawType);
        writer.write(emitter.yawAmplitude);
        writer.write(emitter.yawFrequency);
    } else {
        writer.write(emitter.yawType);
        writer.write(emitter.yawAmplitude);
        writer.write(emitter.yawFrequency);
        writer.write(emitter.pitchType);
        writer.write(emitter.pitchAmplitude);
        writer.write(emitter.pitchFrequency);
    }

    writer.write(emitter.speedType);
    writer.write(emitter.speedAmplitude);
    writer.write(emitter.speedFrequency);
    writer.write(emitter.sizeType);
    writer.write(emitter.sizeAmplitude);
    writer.write(emitter.sizeFrequency);
    writer.write(emitter.alphaType);
    writer.write(emitter.alphaAmplitude);
    writer.write(emitter.alphaFrequency);

    writer.write(emitter.particleVelocity);
    writer.write(emitter.overlay);
}

void BinaryWriterVisitor::visit(const Projector& projector, u32 version) {
    (void)version;
    writer.write(projector.projectionType);
    writer.write(projector.bone);
    writer.write(projector.materialReferenceIndex);
    writer.write(projector.offset);
    writer.write(projector.pitch);
    writer.write(projector.yaw);
    writer.write(projector.roll);
    writer.write(projector.fieldOfView);
    writer.write(projector.aspectRatio);
    writer.write(projector.near);
    writer.write(projector.far);
    writer.write(projector.boxOffsetZBottom);
    writer.write(projector.boxOffsetZTop);
    writer.write(projector.boxOffsetXLeft);
    writer.write(projector.boxOffsetXRight);
    writer.write(projector.boxOffsetYFront);
    writer.write(projector.boxOffsetYBack);
    writer.write(projector.falloff);
    writer.write(projector.alphaInit);
    writer.write(projector.alphaMid);
    writer.write(projector.alphaEnd);
    writer.write(projector.lifetimeAttack);
    writer.write(projector.lifetimeAttackTo);
    writer.write(projector.lifetimeHold);
    writer.write(projector.lifetimeHoldTo);
    writer.write(projector.lifetimeDecay);
    writer.write(projector.lifetimeDecayTo);
    writer.write(projector.attenuationDistance);
    writer.write(projector.active);
    writer.write(projector.layer);
    writer.write(projector.lodReduce);
    writer.write(projector.lodCut);
    writer.write(projector.flags);
}

void BinaryWriterVisitor::visit(const Force& force, u32 version) {
    (void)version;
    writer.write(force.forceType);
    writer.write(force.forceShape);
    writer.write(force.unknown);
    writer.write(force.boneIndex);
    writer.write(force.flags);
    writer.write(force.localChannels);
    writer.write(force.strength);
    writer.write(force.width);
    writer.write(force.height);
    writer.write(force.length);
}

void BinaryWriterVisitor::visit(const Warp& warp, u32 version) {
    (void)version;
    writer.write(warp.warpType);
    writer.write(warp.boneIndex);
    writer.write(warp.unknown);
    writer.write(warp.radius);
    writer.write(warp.height);
    writer.write(warp.strength);
    writer.write(warp.angular);
    writer.write(warp.axial);
    writer.write(warp.radial);
}

void BinaryWriterVisitor::visit(const ViewVolume& volume, u32 version) {
    (void)version;
    writer.write(volume.nodeIndex);
    writer.write(volume.size);
}

void BinaryWriterVisitor::visit(const RigidBody& body, u32 version) {
    if (version <= 2) {
        // Legacy Havok-era layout: 80 bytes base + Ref<PHSH> + 12 bytes post-ref
        writer.write(body.density);
        writer.write(body.friction);
        writer.write(body.restitution);
        writer.write(body.linearDamping);
        writer.write(body.angularDamping);
        writer.write(body.gravityScale);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                writer.write(body.deprecated.inertiaTensor[r][c]);
        writer.write(body.parentBoneIndex);
        writer.write(body.deprecated.boneIndex);
        writer.write(body.deprecated.reserved);
    } else {
        writer.write(body.simulationType);
        writer.write(body.parentBoneIndex);
        writer.write(body.physicsType);
        writer.write(body.density);
        writer.write(body.friction);
        writer.write(body.restitution);
        writer.write(body.linearDamping);
        writer.write(body.angularDamping);
        writer.write(body.gravityScale);
        if (version >= 4) {
            writer.write(body.dynamicState);
            writer.write(body.dynamicBlendOut);
        }
    }
    visit(body.rigidBodyShape);
    writer.write(body.flags);
    writer.write(body.localForces);
    writer.write(body.worldForces);
    writer.write(body.priority);
}

void BinaryWriterVisitor::visit(const PhysicsConstraint& constraint, u32 version) {
    (void)version;
    visit(constraint.dependents);
    writer.write(constraint.rigidBody1);
    writer.write(constraint.rigidBody2);
    writer.write(constraint.flags);
    writer.write(constraint.breakForce);
}

void BinaryWriterVisitor::visit(const PhysicsJoint& joint, u32 version) {
    (void)version;
    writer.write(joint.jointType);
    writer.write(joint.boneIndex1);
    writer.write(joint.boneIndex2);
    writer.write(joint.matrixBody1);
    writer.write(joint.matrixBody2);
    writer.write(joint.enableLimits);
    writer.write(joint.limitMin);
    writer.write(joint.limitMax);
    writer.write(joint.coneAngle);
    writer.write(joint.enableFriction);
    writer.write(joint.friction);
    writer.write(joint.dampingRatio);
    writer.write(joint.angularFrequency);
    writer.write(joint.breakThreshold);
    writer.write(joint.enableShape);
}

void BinaryWriterVisitor::visit(const ClothPhysics& cloth, u32 version) {
    (void)version;
    writer.write(cloth.clothMeshCount);
    writer.write(cloth.skinBoneCount);
    visit(cloth.skinBones);
    visit(cloth.simEnabled);
    visit(cloth.vertexBones);
    visit(cloth.vertexWeights);
    visit(cloth.colliders);
    visit(cloth.proxies);
    writer.write(cloth.density);
    writer.write(cloth.tracking);
    writer.write(cloth.stretchStiffness);
    writer.write(cloth.horizontalStiffness);
    writer.write(cloth.bendingStiffness);
    writer.write(cloth.damping);
    writer.write(cloth.friction);
    writer.write(cloth.gravity);
    writer.write(cloth.explosionScale);
    writer.write(cloth.windScale);
    writer.write(cloth.shearStiffness);
    writer.write(cloth.dragFactor);
    writer.write(cloth.liftFactor);
    writer.write(cloth.sphereStiffness);
    if (version >= 4) {
        writer.write(cloth.flatten);
        writer.write(cloth.active);
        writer.write(cloth.useSkinCollision);
        writer.write(cloth.skinOffset);
        writer.write(cloth.skinExponent);
        writer.write(cloth.skinStiffness);
        writer.write(cloth.localChannels);
        writer.write(cloth.localWind);
    }
}

void BinaryWriterVisitor::visit(const IKTwoJoint& joint, u32 version) {
    (void)version;
    visit(joint.dependents);
    writer.write(joint.boneBase);
    writer.write(joint.boneTarget);
    writer.write(joint.boneEnd);
    writer.write(joint.padding);
    writer.write(joint.hingeAxis);
    writer.write(joint.maxAngleInner);
    writer.write(joint.maxAngleOuter);
    writer.write(joint.searchUp);
    writer.write(joint.searchDown);
}

void BinaryWriterVisitor::visit(const IKCCD& ccd, u32 version) {
    (void)version;
    visit(ccd.dependents);
    writer.write(ccd.boneBase);
    writer.write(ccd.boneTarget);
    writer.write(ccd.searchUp);
    writer.write(ccd.searchDown);
}

void BinaryWriterVisitor::visit(const IKJoint& joint, u32 version) {
    (void)version;
    visit(joint.dependents);
    writer.write(joint.boneIndex1);
    writer.write(joint.boneIndex2);
    writer.write(joint.raycastUp);
    writer.write(joint.raycastDown);
    writer.write(joint.maxSpeed);
    writer.write(joint.goalThreshold);
}

void BinaryWriterVisitor::visit(const OneBoneSolver& solver, u32 version) {
    (void)version;
    visit(solver.dependents);
    writer.write(solver.bone);
    writer.write(solver.boneFallback);
    writer.write(solver.flags);
    writer.write(solver.maxAngle);
}

void BinaryWriterVisitor::visit(const TurretBehavior& behavior, u32 version) {
    writer.write(behavior.transform);
    if (version >= 4) {
        writer.write(behavior.unknown1);
        writer.write(behavior.unknown2);
    }
    writer.write(behavior.boneIndex);
    writer.write(behavior.useAsMainTurret);
    writer.write(behavior.turretGroupId);
    writer.write(behavior.yawLimited);
    writer.write(behavior.yawMin);
    writer.write(behavior.yawMax);
    if (version >= 4) {
        writer.write(behavior.yawWeight);
    }
    writer.write(behavior.pitchLimited);
    writer.write(behavior.pitchMin);
    writer.write(behavior.pitchMax);
    if (version >= 4) {
        writer.write(behavior.pitchWeight);
    }
    writer.write(behavior.unknown3);
    writer.write(behavior.unknown4);
    writer.write(behavior.mainBoneOffset);
}

void BinaryWriterVisitor::visit(const TriggerData& trigger, u32 version) {
    (void)version;
    visit(trigger.dataIndices);
    visit(trigger.name);
}

void BinaryWriterVisitor::visit(const HitTestShape& shape, u32 version) {
    (void)version;
    writer.write(shape.shapeType);
    writer.write(shape.boneIndex);
    writer.write(shape.padding);
    writer.write(shape.transform);
    visit(shape.vertexPositions);
    visit(shape.faceIndices);
    writer.write(shape.sizeX);
    writer.write(shape.sizeY);
    writer.write(shape.sizeZ);
}

void BinaryWriterVisitor::visit(const AttachmentVolume& volume, u32 version) {
    (void)version;
    writer.write(volume.bone1);
    writer.write(volume.bone2);
    writer.write(volume.shapeType);
    writer.write(volume.boneIndex);
    writer.write(volume.padding);
    writer.write(volume.transform);
    visit(volume.vertexPositions);
    visit(volume.faceIndices);
    writer.write(volume.sizeX);
    writer.write(volume.sizeY);
    writer.write(volume.sizeZ);
}

void BinaryWriterVisitor::visit(const BillboardBehavior& behavior, u32 version) {
    (void)version;
    visit(behavior.dependents);
    writer.write(behavior.boneIndex);
    writer.write(behavior.billboardType);
    writer.write(behavior.cameraLookAt);
    writer.write(behavior.up);
    writer.write(behavior.forward);
}

void BinaryWriterVisitor::visit(const TrailingModel& model, u32 version) {
    (void)version;
    visit(model.vectors);
    writer.write(model.param0);
    writer.write(model.param1);
    writer.write(model.animFloat0);
    writer.write(model.animFloat1);
    writer.write(model.flag);
    writer.write(model.reserved0);
    writer.write(model.reserved1);
}

void BinaryWriterVisitor::visit(const ConvexHullHalfEdge& edge, u32 version) {
    (void)version;
    writer.write(edge.type);
    writer.write(edge.faceIndex);
    writer.write(edge.vertexIndex);
    writer.write(edge.nextAroundVertex);
}

void BinaryWriterVisitor::visit(const PhysicsMeshNormal& normal, u32 version) {
    (void)version;
    writer.write(normal.normal);
}

void BinaryWriterVisitor::visit(const PhysicsMeshTriangle& triangle, u32 version) {
    (void)version;
    writer.write(triangle.vertexIndex0);
    writer.write(triangle.vertexIndex1);
    writer.write(triangle.vertexIndex2);
    writer.write(triangle.edgeIndex0);
    writer.write(triangle.edgeIndex1);
    writer.write(triangle.edgeIndex2);
    writer.write(triangle.reserved);
    writer.write(triangle.flags);
}

void BinaryWriterVisitor::visit(const PhysicsMeshEdge& edge, u32 version) {
    (void)version;
    writer.write(edge.edgeType);
    writer.write(edge.vertexA);
    writer.write(edge.vertexB);
    writer.write(edge.faceA);
    writer.write(edge.faceB);
}

void BinaryWriterVisitor::visit(const PhysicsShape& shape, u32 version) {
    writer.write(shape.transform);
    if (version <= 1) {
        writer.write(shape.collisionMargin);
        writer.write(shape.shapeType);
        writer.write<u8>(0);
        writer.write<u8>(0);
        writer.write<u8>(0);
        visit(shape.deprecated.v1.legacyVertices);
        visit(shape.deprecated.v1.unknown0);
        visit(shape.deprecated.v1.faceIndices);
        visit(shape.deprecated.v1.planeEquations);
        writer.write(shape.deprecated.v1.halfExtents);
        return;
    }

    // Version 2+
    writer.write(shape.shapeType);
    writer.write<u8>(0);
    writer.write<u8>(0);
    writer.write<u8>(0);
    writer.write(shape.oldSizes);

    writer.write(shape.reserved0);
    writer.write(shape.shapeDimensions);
    visit(shape.hullFaceNormals);
    visit(shape.hullVertexPositions);
    visit(shape.hullHalfEdges);
    visit(shape.hullVertexFaceIndices);
    writer.write(shape.hullCenter);
    writer.write(shape.hullFaceNormalCount);
    writer.write(shape.hullVertexCount);
    writer.write(shape.hullHalfEdgeCount);
    writer.write(shape.hullUnknown0);
    writer.write(shape.hullUnknown1);

    if (version >= 3) {
        visit(shape.meshFaceNormals);
        visit(shape.meshVertexPositions);
        visit(shape.meshFaceIndices16);
        visit(shape.meshFaceIndices32);
    }
    writer.write(shape.meshBoundsCenter);
    writer.write(shape.meshBoundsExtent);
    writer.write(shape.meshTolerance);
    if (version == 2) {
        visit(shape.deprecated.v2.meshFaceNormals);
        visit(shape.deprecated.v2.meshVertexPositions);
        visit(shape.deprecated.v2.unknown);
        visit(shape.deprecated.v2.unknown2);
    }
    writer.write(shape.meshNormalCount);
    writer.write(shape.meshVertexCount);
    writer.write(shape.meshFaceIndex16Count);
    writer.write(shape.meshFaceIndex32Count);
    writer.write(shape.meshUnknown1);
    writer.write(shape.meshReserved);
    writer.write(shape.meshTreeDepth);
    writer.write(shape.meshCollisionMargin);
}

void BinaryWriterVisitor::visit(const ClothCollider& collider, u32 version) {
    (void)version;
    writer.write(collider.transform);
    writer.write(collider.radius);
    writer.write(collider.height);
    writer.write(collider.padding);
}

void BinaryWriterVisitor::visit(const ClothProxy& proxy, u32 version) {
    (void)version;
    writer.write(proxy.proxyIndex);
    writer.write(proxy.clothIndex);
    visit(proxy.proxyVertices);
    visit(proxy.proxyWeights);
}

void BinaryWriterVisitor::visit(const std::string& str, u32 version) {
    (void)version;
    visit(str);
}

template <typename T>
void BinaryWriterVisitor::visit(const AnimBlock<T>& block, u32 version) {
    (void)version;
    visit(block.timestamps);
    writer.write(block.flags);
    writer.write(block.endFrame);
    visit(block.keys);
}

template <typename T>
void BinaryWriterVisitor::visit(const std::vector<T>& container) {
    Reference ref = {};
    if (container.empty()) {
        writer.write(ref);
        return;
    }

    auto ref_position = writer.getPosition();
    writer.write(ref);
    currentLevelWrites.push_back([this, ref_position, &container]() {
        auto new_entry_index = indexTable.size();
        auto version = getStructureVersion(*container.begin());
        const auto currentOffset = writer.getPosition();
        indexTable.emplace_back(ChunkTagTraits<T>::value, currentOffset, container.size(), version);
        auto& entry = indexTable[new_entry_index];
        writer.setPosition(ref_position);
        Reference ref = {};
        ref.entries = container.size();
        ref.index = new_entry_index;
        writer.write(ref);
        writer.setPosition(entry.offset);
        if constexpr (ChunkTagTraits<T>::is_trivial) {
            writer.write(container);
        } else {
            for (auto& element : container) {
                visit(element, entry.version);
                transferDeferredWrites();
            }
        }
        writer.AlignTo(16, 0xAA);
    });
}

template <typename T>
void BinaryWriterVisitor::visit(const std::optional<T>& container) {
    Reference ref = {};
    if (!container.has_value()) {
        writer.write(ref);
        return;
    }

    auto ref_position = writer.getPosition();
    writer.write(ref);
    currentLevelWrites.push_back([this, ref_position, &container]() {
        auto new_entry_index = indexTable.size();
        auto version = getStructureVersion(*container);
        const auto currentOffset = writer.getPosition();
        indexTable.emplace_back(ChunkTagTraits<T>::value, currentOffset, 1, version);
        auto& entry = indexTable[new_entry_index];
        writer.setPosition(ref_position);
        Reference ref = {};
        ref.entries = 1;
        ref.index = new_entry_index;
        writer.write(ref);
        writer.setPosition(entry.offset);
        if constexpr (ChunkTagTraits<T>::is_trivial) {
            writer.write(*container);
        } else {
            visit(*container, entry.version);
            transferDeferredWrites();
        }
        writer.AlignTo(16, 0xAA);
    });
}

void BinaryWriterVisitor::visit(const std::string& str) {
    Reference ref = {};
    if (str.empty()) {
        writer.write(ref);
        return;
    }

    auto ref_position = writer.getPosition();
    writer.write(ref);
    currentLevelWrites.push_back([this, ref_position, &str]() {
        auto new_entry_index = indexTable.size();
        auto version = getStructureVersion(str);
        const auto currentOffset = writer.getPosition();
        indexTable.emplace_back(ChunkTagTraits<char>::value, currentOffset, str.size(), version);
        auto& entry = indexTable[new_entry_index];
        writer.setPosition(ref_position);
        Reference ref = {};
        ref.entries = str.size();
        ref.index = new_entry_index;
        writer.write(ref);
        writer.setPosition(entry.offset);
        writer.writeString(str);
        writer.AlignTo(16, 0xAA);
    });
}

void BinaryWriterVisitor::transferDeferredWrites(bool pre_order) {
    // Insert children at the FRONT of deferredWrites (before remaining siblings)
    // to achieve DFS pre-order. Reverse-iterate so first child ends up at front.
    while (!currentLevelWrites.empty()) {
        if (!pre_order) {
            deferredWrites.push_back(std::move(currentLevelWrites.back()));
        } else {
            deferredWrites.push_front(std::move(currentLevelWrites.back()));
        }
        currentLevelWrites.pop_back();
    }
}

} // namespace m3
} // namespace whiteout

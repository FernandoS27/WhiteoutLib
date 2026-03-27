// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

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
    if (version < 26) {
        writer.write(layer.uvDensity);
    }
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

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

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
    if (version < 26) {
        value.uvDensity = reader.read<u32>();
    }
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

void BinaryParseVisitor::visit(DataDrivenMaterial& value, u32 version) {
    visit(value.materialName);
    visit(value.fragmentHashes);
    if (version >= 2) {
        visit(value.extraHashes);
    }
    visitCharBlob(value.propertyBlob);
    visit(value.texturePaths);

    // 48 bytes the engine's loader never resolves as references, and that are
    // zero in all 2582 records of the HotS corpus across every version. Kept out
    // of the struct; flagged rather than silently dropped if that ever breaks.
    for (int i = 0; i < 4; ++i) {
        const Reference reserved = readReferenceFunc();
        if (reserved.entries != 0 || reserved.index != 0 || reserved.flags != 0) {
            issues.emplace_back("MADD reserved slot " + std::to_string(i) +
                                " is non-zero; data dropped");
        }
    }

    value.unknown108 = reader.read<f32>();
    value.unknown112 = reader.read<f32>();
    value.unknown116 = reader.read<f32>();
    value.effectNameHash = reader.read<u32>();
    value.unknown124 = reader.read<u32>();
    value.padding128 = reader.read<u32>();
    value.unknown132 = reader.read<i32>();
    value.unknown136 = reader.read<u32>();
    value.unknown140 = reader.read<u32>();
    value.unknown144 = reader.read<u32>();
    value.unknown148 = reader.read<u8>();
    value.alphaFresnelFlags = reader.read<u8>();
    value.shaderType = static_cast<MaterialShaderType>(reader.read<u8>());
    value.unknown151 = reader.read<u8>();
    if (version >= 3) {
        value.effectNameHash2 = reader.read<u32>();
        value.effectNameHash3 = reader.read<u32>();
    }
}

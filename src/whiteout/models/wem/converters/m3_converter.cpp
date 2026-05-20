// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/m3/parser.h"
#include "whiteout/models/m3/writer.h"
#include "whiteout/models/wem/converters.h"

#include <algorithm>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Helpers
// ============================================================================

namespace {

wem::Extent convertM3Extent(const m3::Extent& src) {
    wem::Extent dst;
    dst.minimum = src.min;
    dst.maximum = src.max;
    dst.sphereRadius = src.radius;
    return dst;
}

m3::Extent convertExtentToM3(const wem::Extent& src) {
    m3::Extent dst;
    dst.min = src.minimum;
    dst.max = src.maximum;
    dst.radius = src.sphereRadius;
    return dst;
}

BlendMode convertM3BlendMode(m3::BlendMode bm) {
    switch (bm) {
    case m3::BlendMode::Opaque:
        return BlendMode::Opaque;
    case m3::BlendMode::AlphaBlend:
        return BlendMode::AlphaBlend;
    case m3::BlendMode::Add:
        return BlendMode::Additive;
    case m3::BlendMode::AlphaAdd:
        return BlendMode::AdditiveAlpha;
    case m3::BlendMode::Mod:
        return BlendMode::Modulate;
    case m3::BlendMode::Mod2x:
        return BlendMode::Modulate2x;
    default:
        return BlendMode::Opaque;
    }
}

m3::BlendMode convertBlendModeToM3(BlendMode bm) {
    switch (bm) {
    case BlendMode::Opaque:
        return m3::BlendMode::Opaque;
    case BlendMode::AlphaBlend:
        return m3::BlendMode::AlphaBlend;
    case BlendMode::Additive:
        return m3::BlendMode::Add;
    case BlendMode::AdditiveAlpha:
        return m3::BlendMode::AlphaAdd;
    case BlendMode::Modulate:
        return m3::BlendMode::Mod;
    case BlendMode::Modulate2x:
        return m3::BlendMode::Mod2x;
    case BlendMode::AlphaKey:
        return m3::BlendMode::AlphaBlend;
    case BlendMode::BlendAdd:
        return m3::BlendMode::Add;
    case BlendMode::Transparent:
        return m3::BlendMode::AlphaBlend;
    default:
        return m3::BlendMode::Opaque;
    }
}

MaterialFlags convertM3MaterialFlags(m3::MaterialFlag mf) {
    auto flags = MaterialFlags::None;
    auto v = static_cast<u32>(mf);
    if (v & static_cast<u32>(m3::MaterialFlag::Unshaded))
        flags |= MaterialFlags::Unlit;
    if (v & static_cast<u32>(m3::MaterialFlag::TwoSided))
        flags |= MaterialFlags::TwoSided;
    if (v & static_cast<u32>(m3::MaterialFlag::Unfogged))
        flags |= MaterialFlags::Unfogged;
    return flags;
}

m3::MaterialFlag convertMaterialFlagsToM3(MaterialFlags mf) {
    auto result = m3::MaterialFlag::None;
    if (hasFlag(mf, MaterialFlags::Unlit))
        result = result | m3::MaterialFlag::Unshaded;
    if (hasFlag(mf, MaterialFlags::TwoSided))
        result = result | m3::MaterialFlag::TwoSided;
    if (hasFlag(mf, MaterialFlags::Unfogged))
        result = result | m3::MaterialFlag::Unfogged;
    return result;
}

MaterialClass convertM3MaterialClass(m3::MaterialClass mc) {
    switch (mc) {
    case m3::MaterialClass::Unit:
        return MaterialClass::Unit;
    case m3::MaterialClass::Building:
        return MaterialClass::Building;
    case m3::MaterialClass::Doodad:
        return MaterialClass::Doodad;
    case m3::MaterialClass::SpecialFX:
        return MaterialClass::SpecialFX;
    default:
        return MaterialClass::Unit;
    }
}

m3::MaterialClass convertMaterialClassToM3(MaterialClass mc) {
    switch (mc) {
    case MaterialClass::Unit:
        return m3::MaterialClass::Unit;
    case MaterialClass::Building:
        return m3::MaterialClass::Building;
    case MaterialClass::Doodad:
        return m3::MaterialClass::Doodad;
    case MaterialClass::SpecialFX:
        return m3::MaterialClass::SpecialFX;
    default:
        return m3::MaterialClass::Unit;
    }
}

LayerBlendOp convertM3LayerBlendOp(m3::LayerBlendOp op) {
    switch (op) {
    case m3::LayerBlendOp::Mod:
        return LayerBlendOp::Mod;
    case m3::LayerBlendOp::Mod2x:
        return LayerBlendOp::Mod2x;
    case m3::LayerBlendOp::Add:
        return LayerBlendOp::Add;
    case m3::LayerBlendOp::Lerp:
        return LayerBlendOp::Lerp;
    case m3::LayerBlendOp::TeamColorEmissiveAdd:
        return LayerBlendOp::TeamColorEmissiveAdd;
    case m3::LayerBlendOp::TeamColorDiffuseAdd:
        return LayerBlendOp::TeamColorDiffuse;
    case m3::LayerBlendOp::AddNoAlpha:
        return LayerBlendOp::OpNone;
    default:
        return LayerBlendOp::Mod;
    }
}

m3::LayerBlendOp convertLayerBlendOpToM3(LayerBlendOp op) {
    switch (op) {
    case LayerBlendOp::Mod:
        return m3::LayerBlendOp::Mod;
    case LayerBlendOp::Mod2x:
        return m3::LayerBlendOp::Mod2x;
    case LayerBlendOp::Add:
        return m3::LayerBlendOp::Add;
    case LayerBlendOp::Lerp:
        return m3::LayerBlendOp::Lerp;
    case LayerBlendOp::TeamColorEmissiveAdd:
        return m3::LayerBlendOp::TeamColorEmissiveAdd;
    case LayerBlendOp::TeamColorDiffuse:
        return m3::LayerBlendOp::TeamColorDiffuseAdd;
    case LayerBlendOp::OpNone:
        return m3::LayerBlendOp::AddNoAlpha;
    default:
        return m3::LayerBlendOp::Mod;
    }
}

SpecularMode convertM3SpecularMode(m3::SpecularMode sm) {
    switch (sm) {
    case m3::SpecularMode::RGB:
        return SpecularMode::RGB;
    case m3::SpecularMode::AlphaOnly:
        return SpecularMode::AlphaOnly;
    default:
        return SpecularMode::RGB;
    }
}

m3::SpecularMode convertSpecularModeToM3(SpecularMode sm) {
    return (sm == SpecularMode::AlphaOnly) ? m3::SpecularMode::AlphaOnly : m3::SpecularMode::RGB;
}

UVMappingMode convertM3UVMapping(m3::UVMappingMode uv) {
    switch (uv) {
    case m3::UVMappingMode::ExplicitUV0:
        return UVMappingMode::ExplicitUV0;
    case m3::UVMappingMode::ExplicitUV1:
        return UVMappingMode::ExplicitUV1;
    case m3::UVMappingMode::ExplicitUV2:
        return UVMappingMode::ExplicitUV2;
    case m3::UVMappingMode::ExplicitUV3:
        return UVMappingMode::ExplicitUV3;
    case m3::UVMappingMode::ReflectCubicEnvio:
        return UVMappingMode::ReflectCubicEnvironment;
    case m3::UVMappingMode::ReflectSphericalEnvio:
        return UVMappingMode::SphericalEnvironment;
    case m3::UVMappingMode::PlanarLocalX:
        return UVMappingMode::PlanarLocalX;
    case m3::UVMappingMode::PlanarLocalY:
        return UVMappingMode::PlanarLocalY;
    case m3::UVMappingMode::PlanarLocalZ:
        return UVMappingMode::PlanarLocalZ;
    case m3::UVMappingMode::PlanarWorldX:
        return UVMappingMode::PlanarWorldX;
    case m3::UVMappingMode::PlanarWorldY:
        return UVMappingMode::PlanarWorldY;
    case m3::UVMappingMode::PlanarWorldZ:
        return UVMappingMode::PlanarWorldZ;
    case m3::UVMappingMode::TriPlanarLocal:
        return UVMappingMode::TriPlanarLocal;
    case m3::UVMappingMode::TriPlanarWorld:
        return UVMappingMode::TriPlanarWorld;
    default:
        return UVMappingMode::ExplicitUV0;
    }
}

m3::UVMappingMode convertUVMappingToM3(UVMappingMode uv) {
    switch (uv) {
    case UVMappingMode::ExplicitUV0:
        return m3::UVMappingMode::ExplicitUV0;
    case UVMappingMode::ExplicitUV1:
        return m3::UVMappingMode::ExplicitUV1;
    case UVMappingMode::ExplicitUV2:
        return m3::UVMappingMode::ExplicitUV2;
    case UVMappingMode::ExplicitUV3:
        return m3::UVMappingMode::ExplicitUV3;
    case UVMappingMode::ReflectCubicEnvironment:
        return m3::UVMappingMode::ReflectCubicEnvio;
    case UVMappingMode::SphericalEnvironment:
        return m3::UVMappingMode::ReflectSphericalEnvio;
    case UVMappingMode::PlanarLocalX:
        return m3::UVMappingMode::PlanarLocalX;
    case UVMappingMode::PlanarLocalY:
        return m3::UVMappingMode::PlanarLocalY;
    case UVMappingMode::PlanarLocalZ:
        return m3::UVMappingMode::PlanarLocalZ;
    case UVMappingMode::PlanarWorldX:
        return m3::UVMappingMode::PlanarWorldX;
    case UVMappingMode::PlanarWorldY:
        return m3::UVMappingMode::PlanarWorldY;
    case UVMappingMode::PlanarWorldZ:
        return m3::UVMappingMode::PlanarWorldZ;
    case UVMappingMode::TriPlanarLocal:
        return m3::UVMappingMode::TriPlanarLocal;
    case UVMappingMode::TriPlanarWorld:
        return m3::UVMappingMode::TriPlanarWorld;
    default:
        return m3::UVMappingMode::ExplicitUV0;
    }
}

ColorChannelSelect convertM3ColorChannelSelect(m3::ColorChannelSelect cs) {
    switch (cs) {
    case m3::ColorChannelSelect::RGB:
        return ColorChannelSelect::RGB;
    case m3::ColorChannelSelect::RGBA:
        return ColorChannelSelect::RGBA;
    case m3::ColorChannelSelect::Alpha:
        return ColorChannelSelect::Alpha;
    case m3::ColorChannelSelect::Red:
        return ColorChannelSelect::Red;
    case m3::ColorChannelSelect::Green:
        return ColorChannelSelect::Green;
    case m3::ColorChannelSelect::Blue:
        return ColorChannelSelect::Blue;
    default:
        return ColorChannelSelect::RGBA;
    }
}

m3::ColorChannelSelect convertColorChannelToM3(ColorChannelSelect cs) {
    switch (cs) {
    case ColorChannelSelect::RGB:
        return m3::ColorChannelSelect::RGB;
    case ColorChannelSelect::RGBA:
        return m3::ColorChannelSelect::RGBA;
    case ColorChannelSelect::Alpha:
        return m3::ColorChannelSelect::Alpha;
    case ColorChannelSelect::Red:
        return m3::ColorChannelSelect::Red;
    case ColorChannelSelect::Green:
        return m3::ColorChannelSelect::Green;
    case ColorChannelSelect::Blue:
        return m3::ColorChannelSelect::Blue;
    default:
        return m3::ColorChannelSelect::RGBA;
    }
}

FresnelMode convertM3FresnelMode(m3::FresnelMode fm) {
    switch (fm) {
    case m3::FresnelMode::None:
        return FresnelMode::None;
    case m3::FresnelMode::Standard:
        return FresnelMode::Standard;
    case m3::FresnelMode::Inverted:
        return FresnelMode::Inverted;
    default:
        return FresnelMode::None;
    }
}

m3::FresnelMode convertFresnelModeToM3(FresnelMode fm) {
    switch (fm) {
    case FresnelMode::None:
        return m3::FresnelMode::None;
    case FresnelMode::Standard:
        return m3::FresnelMode::Standard;
    case FresnelMode::Inverted:
        return m3::FresnelMode::Inverted;
    default:
        return m3::FresnelMode::None;
    }
}

SubmeshFlags convertM3RegionFlags(m3::RegionFlag rf) {
    auto flags = SubmeshFlags::None;
    auto v = static_cast<u32>(rf);
    if (v & static_cast<u32>(m3::RegionFlag::Hidden))
        flags = flags | SubmeshFlags::Hidden;
    if (v & static_cast<u32>(m3::RegionFlag::ClothSimulated))
        flags = flags | SubmeshFlags::ClothSimulated;
    if (v & static_cast<u32>(m3::RegionFlag::ClothInfluenced))
        flags = flags | SubmeshFlags::ClothInfluenced;
    return flags;
}

m3::RegionFlag convertSubmeshFlagsToM3(SubmeshFlags sf) {
    auto result = m3::RegionFlag::None;
    if (hasFlag(sf, SubmeshFlags::Hidden))
        result = result | m3::RegionFlag::Hidden;
    if (hasFlag(sf, SubmeshFlags::ClothSimulated))
        result = result | m3::RegionFlag::ClothSimulated;
    if (hasFlag(sf, SubmeshFlags::ClothInfluenced))
        result = result | m3::RegionFlag::ClothInfluenced;
    return result;
}

// Convert an M3 TextureLayer to a WEM TextureSlot
TextureSlot convertTextureLayer(const m3::TextureLayer& layer, TextureSlotSemantic semantic,
                                u32 textureIndex, std::vector<std::string>& /*issues*/) {
    TextureSlot slot;
    slot.semantic = semantic;
    slot.textureIndex = textureIndex;
    slot.uvMapping = convertM3UVMapping(layer.uvMapping);
    slot.colorChannelSelect = convertM3ColorChannelSelect(layer.colorType);
    slot.rgbMultiply = layer.rgbMultiply.initValue;
    slot.rgbAdd = layer.rgbAdd.initValue;
    slot.flipbookRows = layer.flipbookRows;
    slot.flipbookColumns = layer.flipbookColumns;

    // Wrap flags from TextureLayerFlag
    auto layerFlags = static_cast<u32>(layer.flags);
    slot.wrapU = (layerFlags & static_cast<u32>(m3::TextureLayerFlag::UVWrapX)) != 0;
    slot.wrapV = (layerFlags & static_cast<u32>(m3::TextureLayerFlag::UVWrapY)) != 0;

    // Fresnel
    slot.fresnel.mode = convertM3FresnelMode(layer.fresnelMode);
    slot.fresnel.exponent = layer.fresnelExponent;
    slot.fresnel.min = layer.fresnelMin;
    slot.fresnel.max = layer.fresnelMax;
    slot.fresnel.translation = layer.fresnelTranslation;
    slot.fresnel.mask = layer.fresnelMask;
    slot.fresnel.rotation = layer.fresnelRotation;

    // Resolve UV set index from mapping mode
    switch (layer.uvMapping) {
    case m3::UVMappingMode::ExplicitUV0:
        slot.uvSetIndex = 0;
        break;
    case m3::UVMappingMode::ExplicitUV1:
        slot.uvSetIndex = 1;
        break;
    case m3::UVMappingMode::ExplicitUV2:
        slot.uvSetIndex = 2;
        break;
    case m3::UVMappingMode::ExplicitUV3:
        slot.uvSetIndex = 3;
        break;
    default:
        slot.uvSetIndex = 0;
        break;
    }

    return slot;
}

// Try to add a named texture layer from an M3 StandardMaterial to the material's texture slots
void tryAddLayer(const std::optional<m3::TextureLayer>& layer, TextureSlotSemantic semantic,
                 Material& wMat, std::vector<TextureRef>& textures,
                 std::vector<std::string>& issues) {
    if (!layer.has_value() || layer->texturePath.empty())
        return;

    // Find or add texture
    u32 texIdx = static_cast<u32>(textures.size());
    for (u32 i = 0; i < textures.size(); ++i) {
        if (textures[i].path == layer->texturePath) {
            texIdx = i;
            break;
        }
    }
    if (texIdx == static_cast<u32>(textures.size())) {
        TextureRef ref;
        ref.path = layer->texturePath;
        textures.push_back(std::move(ref));
    }

    wMat.textureSlots.push_back(convertTextureLayer(*layer, semantic, texIdx, issues));
}

} // anonymous namespace

// ============================================================================
// fromM3
// ============================================================================

ConvertResult M3Converter::fromM3(const m3::Model& m3Model) const {
    ConvertResult result;
    auto& model = result.model;
    auto& issues = result.issues;

    model.name = m3Model.name;
    model.bounds = convertM3Extent(m3Model.bounds);

    // ── Collect textures from materials (M3 stores paths inside layers) ────
    // We build the texture table on-the-fly while converting materials.

    // ── Materials ──────────────────────────────────────────────────
    // M3 uses MaterialMap (MATM) to dispatch material types. We process the
    // materialMaps array in order.
    model.materials.reserve(m3Model.materialMaps.size());
    for (const auto& matMap : m3Model.materialMaps) {
        if (matMap.materialType == m3::MaterialType::Standard) {
            if (matMap.materialIndex >= m3Model.standardMaterials.size()) {
                issues.push_back("MaterialMap references out-of-range StandardMaterial index " +
                                 std::to_string(matMap.materialIndex));
                Material const dummy{};
                model.materials.push_back(dummy);
                continue;
            }
            const auto& src = m3Model.standardMaterials[matMap.materialIndex];
            Material wMat;
            wMat.type = MaterialType::Standard;
            wMat.name = src.name;
            wMat.priorityPlane = src.priority;
            wMat.blendMode = convertM3BlendMode(src.blendMode);
            wMat.flags = convertM3MaterialFlags(src.flags);
            wMat.specularExponent = src.specularExponent;
            wMat.alphaTestThreshold = static_cast<f32>(src.alphaTestThreshold);
            wMat.depthBlendFalloff = src.depthBlendFalloff;
            wMat.hdrSpecularMultiplier = src.hdrSpecularMultiplier;
            wMat.hdrEmissiveMultiplier = src.hdrEmissiveMultiplier;
            wMat.hdrEnvironmentConstant = src.hdrEnvironmentConstant;
            wMat.hdrEnvironmentDiffuse = src.hdrEnvironmentDiffuse;
            wMat.hdrEnvironmentSpecular = src.hdrEnvironmentSpecular;
            wMat.materialClass = convertM3MaterialClass(src.materialClass);
            wMat.layerBlendMode = convertM3LayerBlendOp(src.layerBlendMode);
            wMat.emissiveBlendMode1 = convertM3LayerBlendOp(src.emissiveBlendMode1);
            wMat.emissiveBlendMode2 = convertM3LayerBlendOp(src.emissiveBlendMode2);
            wMat.specularMode = convertM3SpecularMode(src.specularMode);

            // Add named texture layers
            tryAddLayer(src.diffuseLayer, TextureSlotSemantic::Diffuse, wMat, model.textures,
                        issues);
            tryAddLayer(src.decalLayer, TextureSlotSemantic::Decal, wMat, model.textures, issues);
            tryAddLayer(src.specularLayer, TextureSlotSemantic::Specular, wMat, model.textures,
                        issues);
            tryAddLayer(src.glossLayer, TextureSlotSemantic::Gloss, wMat, model.textures, issues);
            tryAddLayer(src.emissiveLayer1, TextureSlotSemantic::Emissive1, wMat, model.textures,
                        issues);
            tryAddLayer(src.emissiveLayer2, TextureSlotSemantic::Emissive2, wMat, model.textures,
                        issues);
            tryAddLayer(src.environmentLayer, TextureSlotSemantic::Environment, wMat,
                        model.textures, issues);
            tryAddLayer(src.environmentMaskLayer, TextureSlotSemantic::EnvironmentMask, wMat,
                        model.textures, issues);
            tryAddLayer(src.alphaLayer1, TextureSlotSemantic::Alpha1, wMat, model.textures, issues);
            tryAddLayer(src.alphaLayer2, TextureSlotSemantic::Alpha2, wMat, model.textures, issues);
            tryAddLayer(src.normalLayer, TextureSlotSemantic::Normal, wMat, model.textures, issues);
            tryAddLayer(src.heightLayer, TextureSlotSemantic::Height, wMat, model.textures, issues);
            tryAddLayer(src.lightMapLayer, TextureSlotSemantic::LightMap, wMat, model.textures,
                        issues);
            tryAddLayer(src.ambientOcclusionLayer, TextureSlotSemantic::AmbientOcclusion, wMat,
                        model.textures, issues);
            tryAddLayer(src.normalBlend1MaskLayer, TextureSlotSemantic::NormalBlend1Mask, wMat,
                        model.textures, issues);
            tryAddLayer(src.normalBlend2MaskLayer, TextureSlotSemantic::NormalBlend2Mask, wMat,
                        model.textures, issues);
            tryAddLayer(src.normalBlend1Layer, TextureSlotSemantic::NormalBlend1, wMat,
                        model.textures, issues);
            tryAddLayer(src.normalBlend2Layer, TextureSlotSemantic::NormalBlend2, wMat,
                        model.textures, issues);

            model.materials.push_back(std::move(wMat));
        } else if (matMap.materialType == m3::MaterialType::Composite) {
            if (matMap.materialIndex >= m3Model.compositeMaterials.size()) {
                issues.push_back("MaterialMap references out-of-range CompositeMaterial index " +
                                 std::to_string(matMap.materialIndex));
                Material const dummy{};
                model.materials.push_back(dummy);
                continue;
            }
            const auto& src = m3Model.compositeMaterials[matMap.materialIndex];
            Material wMat;
            wMat.type = MaterialType::Composite;
            wMat.name = src.name;
            wMat.priorityPlane = static_cast<i32>(src.priority);

            for (const auto& sec : src.sections) {
                CompositeSection cs;
                // M3 CompositeSection.materialIndex indexes into MATM, not directly into
                // standardMaterials. We map it to the WEM material index (same as MATM order).
                cs.materialIndex = sec.materialIndex;
                cs.blendWeight = sec.mapMultiplier.initValue;
                wMat.sections.push_back(cs);
            }

            model.materials.push_back(std::move(wMat));
        } else {
            // Unsupported material type (displacement, terrain, volume, etc.)
            issues.push_back("Unsupported M3 material type " +
                             std::to_string(static_cast<u32>(matMap.materialType)) + " skipped");
            Material dummy;
            dummy.name = "unsupported_material_type_" +
                         std::to_string(static_cast<u32>(matMap.materialType));
            model.materials.push_back(std::move(dummy));
        }
    }

    // ── Vertex data + Divisions → Meshes ───────────────────────────
    const auto& vb = m3Model.vertices;
    auto positions = vb.getPositions();
    auto normals = vb.getNormals();
    auto tangents = vb.getTangents();
    auto boneIndices = vb.getBoneIndices();
    auto boneWeights = vb.getBoneWeights();
    size_t const numUVs = vb.UVsNum();
    bool const hasColors = vb.hasVertexColors();

    std::vector<std::vector<Vector2f>> allUVs(numUVs);
    for (size_t u = 0; u < numUVs; ++u) {
        allUVs[u] = vb.getUVs(u);
    }

    std::vector<std::array<u8, 4>> vertexColors;
    if (hasColors) {
        auto colors = vb.getColors();
        vertexColors.resize(colors.size());
        for (size_t i = 0; i < colors.size(); ++i) {
            // M3 stores BGRA, WEM stores RGBA
            vertexColors[i] = {colors[i].b, colors[i].g, colors[i].r, colors[i].a};
        }
    }

    for (size_t di = 0; di < m3Model.divisions.size(); ++di) {
        const auto& div = m3Model.divisions[di];
        Mesh mesh;
        mesh.name = m3Model.name + "_div" + std::to_string(di);

        // The division references a global vertex buffer; each region selects a range.
        // We copy the full vertex data used by this division.
        // Find the vertex range from regions
        u32 minVertex = UINT32_MAX, maxVertex = 0;
        for (const auto& reg : div.regions) {
            if (reg.firstVertex < minVertex)
                minVertex = reg.firstVertex;
            u32 const endV = reg.firstVertex + reg.vertexCount;
            if (endV > maxVertex)
                maxVertex = endV;
        }
        if (minVertex > maxVertex) {
            minVertex = 0;
            maxVertex = 0;
        }

        u32 const vertRange = maxVertex - minVertex;
        mesh.positions.resize(vertRange);
        mesh.normals.resize(vertRange);
        mesh.tangents.resize(vertRange);
        mesh.boneIndices.resize(vertRange);
        mesh.boneWeights.resize(vertRange);
        mesh.uvSets.resize(numUVs);
        for (size_t u = 0; u < numUVs; ++u) {
            mesh.uvSets[u].resize(vertRange);
        }
        if (hasColors)
            mesh.vertexColors.resize(vertRange);

        for (u32 v = 0; v < vertRange; ++v) {
            u32 const srcIdx = minVertex + v;
            if (srcIdx < positions.size())
                mesh.positions[v] = positions[srcIdx];
            if (srcIdx < normals.size())
                mesh.normals[v] = normals[srcIdx];
            if (srcIdx < tangents.size())
                mesh.tangents[v] = tangents[srcIdx];
            if (srcIdx < boneIndices.size())
                mesh.boneIndices[v] = boneIndices[srcIdx];
            if (srcIdx < boneWeights.size())
                mesh.boneWeights[v] = boneWeights[srcIdx];
            for (size_t u = 0; u < numUVs; ++u) {
                if (srcIdx < allUVs[u].size())
                    mesh.uvSets[u][v] = allUVs[u][srcIdx];
            }
            if (hasColors && srcIdx < vertexColors.size())
                mesh.vertexColors[v] = vertexColors[srcIdx];
        }

        // Indices: u16 → u32, rebase from global to mesh-local
        mesh.indices.reserve(div.faces.size());
        for (u16 const idx : div.faces) {
            u32 const rebased =
                static_cast<u32>(idx) >= minVertex ? static_cast<u32>(idx) - minVertex : 0;
            mesh.indices.push_back(rebased);
        }

        // Regions → Submeshes
        mesh.submeshes.reserve(div.regions.size());
        for (size_t ri = 0; ri < div.regions.size(); ++ri) {
            const auto& reg = div.regions[ri];
            Submesh sub;
            sub.name = "region_" + std::to_string(ri);
            sub.indexStart = reg.firstIndex;
            sub.indexCount = reg.indexCount;
            sub.vertexStart = reg.firstVertex - minVertex;
            sub.vertexCount = reg.vertexCount;
            sub.maxBoneInfluences = reg.boneWeightPairs;
            sub.rootBone = reg.rootBone;
            sub.flags = convertM3RegionFlags(reg.flags);

            // Find the batch that references this region to get material
            for (const auto& batch : div.batches) {
                if (batch.regionIndex == ri) {
                    // batch.materialIndex indexes into MATM (materialMaps),
                    // which is the same order as our model.materials[]
                    sub.materialIndex = batch.materialIndex;
                    break;
                }
            }

            mesh.submeshes.push_back(std::move(sub));
        }

        // Compute mesh bounds
        if (!mesh.positions.empty()) {
            Vector3f minP = mesh.positions[0];
            Vector3f maxP = mesh.positions[0];
            f32 maxDistSq = 0;
            for (const auto& p : mesh.positions) {
                minP =
                    Vector3f(std::min(minP.x, p.x), std::min(minP.y, p.y), std::min(minP.z, p.z));
                maxP =
                    Vector3f(std::max(maxP.x, p.x), std::max(maxP.y, p.y), std::max(maxP.z, p.z));
                f32 const distSq = p.x * p.x + p.y * p.y + p.z * p.z;
                if (distSq > maxDistSq)
                    maxDistSq = distSq;
            }
            mesh.bounds.minimum = minP;
            mesh.bounds.maximum = maxP;
            mesh.bounds.sphereRadius = std::sqrt(maxDistSq);
        }

        model.meshes.push_back(std::move(mesh));
    }

    return result;
}

// ============================================================================
// toM3
// ============================================================================

M3ConvertResult M3Converter::toM3(const Model& wemModel, u32 /*targetVersion*/) const {
    M3ConvertResult result;
    auto& m3 = result.model;
    auto& issues = result.issues;

    m3.name = wemModel.name;
    m3.bounds = convertExtentToM3(wemModel.bounds);

    // ── Materials ──────────────────────────────────────────────────
    for (size_t mi = 0; mi < wemModel.materials.size(); ++mi) {
        const auto& wMat = wemModel.materials[mi];

        m3::MaterialMap matMap;
        if (wMat.type == MaterialType::Standard) {
            matMap.materialType = m3::MaterialType::Standard;
            matMap.materialIndex = static_cast<u32>(m3.standardMaterials.size());

            m3::StandardMaterial mat;
            mat.name = wMat.name;
            mat.priority = wMat.priorityPlane;
            mat.blendMode = convertBlendModeToM3(wMat.blendMode);
            mat.flags = convertMaterialFlagsToM3(wMat.flags);
            mat.specularExponent = wMat.specularExponent;
            mat.alphaTestThreshold = static_cast<u32>(wMat.alphaTestThreshold);
            mat.depthBlendFalloff = wMat.depthBlendFalloff;
            mat.hdrSpecularMultiplier = wMat.hdrSpecularMultiplier;
            mat.hdrEmissiveMultiplier = wMat.hdrEmissiveMultiplier;
            mat.hdrEnvironmentConstant = wMat.hdrEnvironmentConstant;
            mat.hdrEnvironmentDiffuse = wMat.hdrEnvironmentDiffuse;
            mat.hdrEnvironmentSpecular = wMat.hdrEnvironmentSpecular;
            mat.materialClass = convertMaterialClassToM3(wMat.materialClass);
            mat.layerBlendMode = convertLayerBlendOpToM3(wMat.layerBlendMode);
            mat.emissiveBlendMode1 = convertLayerBlendOpToM3(wMat.emissiveBlendMode1);
            mat.emissiveBlendMode2 = convertLayerBlendOpToM3(wMat.emissiveBlendMode2);
            mat.specularMode = convertSpecularModeToM3(wMat.specularMode);

            // Texture slots → named layers
            for (const auto& slot : wMat.textureSlots) {
                m3::TextureLayer layer{};
                if (slot.textureIndex < wemModel.textures.size())
                    layer.texturePath = wemModel.textures[slot.textureIndex].path;
                layer.uvMapping = convertUVMappingToM3(slot.uvMapping);
                layer.colorType = convertColorChannelToM3(slot.colorChannelSelect);
                layer.rgbMultiply.initValue = slot.rgbMultiply;
                layer.rgbAdd.initValue = slot.rgbAdd;
                layer.flipbookRows = slot.flipbookRows;
                layer.flipbookColumns = slot.flipbookColumns;
                layer.fresnelMode = convertFresnelModeToM3(slot.fresnel.mode);
                layer.fresnelExponent = slot.fresnel.exponent;
                layer.fresnelMin = slot.fresnel.min;
                layer.fresnelMax = slot.fresnel.max;
                layer.fresnelTranslation = slot.fresnel.translation;
                layer.fresnelMask = slot.fresnel.mask;
                layer.fresnelRotation = slot.fresnel.rotation;

                // Set wrap flags
                u32 layerFlags = 0;
                if (slot.wrapU)
                    layerFlags |= static_cast<u32>(m3::TextureLayerFlag::UVWrapX);
                if (slot.wrapV)
                    layerFlags |= static_cast<u32>(m3::TextureLayerFlag::UVWrapY);
                layer.flags = static_cast<m3::TextureLayerFlag>(layerFlags);

                switch (slot.semantic) {
                case TextureSlotSemantic::Diffuse:
                    mat.diffuseLayer = layer;
                    break;
                case TextureSlotSemantic::Decal:
                    mat.decalLayer = layer;
                    break;
                case TextureSlotSemantic::Specular:
                    mat.specularLayer = layer;
                    break;
                case TextureSlotSemantic::Gloss:
                    mat.glossLayer = layer;
                    break;
                case TextureSlotSemantic::Emissive1:
                    mat.emissiveLayer1 = layer;
                    break;
                case TextureSlotSemantic::Emissive2:
                    mat.emissiveLayer2 = layer;
                    break;
                case TextureSlotSemantic::Environment:
                    mat.environmentLayer = layer;
                    break;
                case TextureSlotSemantic::EnvironmentMask:
                    mat.environmentMaskLayer = layer;
                    break;
                case TextureSlotSemantic::Alpha1:
                    mat.alphaLayer1 = layer;
                    break;
                case TextureSlotSemantic::Alpha2:
                    mat.alphaLayer2 = layer;
                    break;
                case TextureSlotSemantic::Normal:
                    mat.normalLayer = layer;
                    break;
                case TextureSlotSemantic::Height:
                    mat.heightLayer = layer;
                    break;
                case TextureSlotSemantic::LightMap:
                    mat.lightMapLayer = layer;
                    break;
                case TextureSlotSemantic::AmbientOcclusion:
                    mat.ambientOcclusionLayer = layer;
                    break;
                case TextureSlotSemantic::NormalBlend1Mask:
                    mat.normalBlend1MaskLayer = layer;
                    break;
                case TextureSlotSemantic::NormalBlend2Mask:
                    mat.normalBlend2MaskLayer = layer;
                    break;
                case TextureSlotSemantic::NormalBlend1:
                    mat.normalBlend1Layer = layer;
                    break;
                case TextureSlotSemantic::NormalBlend2:
                    mat.normalBlend2Layer = layer;
                    break;
                case TextureSlotSemantic::Custom:
                case TextureSlotSemantic::Roughness:
                case TextureSlotSemantic::Metalness:
                case TextureSlotSemantic::ORM:
                    // No matching named slot; try diffuse if empty
                    if (!mat.diffuseLayer.has_value()) {
                        mat.diffuseLayer = layer;
                    } else {
                        issues.push_back("Custom texture slot has no M3 equivalent, dropped: " +
                                         layer.texturePath);
                    }
                    break;
                }
            }

            m3.standardMaterials.push_back(std::move(mat));
        } else if (wMat.type == MaterialType::Composite) {
            matMap.materialType = m3::MaterialType::Composite;
            matMap.materialIndex = static_cast<u32>(m3.compositeMaterials.size());

            m3::CompositeMaterial cmat;
            cmat.name = wMat.name;
            cmat.priority = static_cast<u32>(wMat.priorityPlane);

            for (const auto& sec : wMat.sections) {
                m3::CompositeSection csec;
                csec.materialIndex = sec.materialIndex;
                csec.mapMultiplier.initValue = sec.blendWeight;
                cmat.sections.push_back(std::move(csec));
            }

            m3.compositeMaterials.push_back(std::move(cmat));
        } else {
            issues.push_back("Unsupported WEM material type for M3 export, skipped");
            matMap.materialType = m3::MaterialType::Standard;
            matMap.materialIndex = 0;
        }

        m3.materialMaps.push_back(matMap);
    }

    // ── Meshes → VertexBuffer + Divisions ──────────────────────────
    // M3 has a single global vertex buffer; we concatenate all mesh vertices.
    // VertexBuffer needs raw interleaved data — we can't easily produce that from WEM SoA
    // without the VertexBuffer encoder. Instead, we populate the accessors' source data
    // and rely on the M3 writer to re-encode.
    //
    // For now, build divisions from WEM meshes with correct region/batch layout.
    // The actual vertex encoding is handled by the M3 writer.

    // Determine vertex format flags
    bool anyColors = false;
    size_t maxUVs = 0;
    for (const auto& mesh : wemModel.meshes) {
        if (!mesh.vertexColors.empty())
            anyColors = true;
        if (mesh.uvSets.size() > maxUVs)
            maxUVs = mesh.uvSets.size();
    }

    auto vflags = m3::VertexFormatFlag::None;
    if (anyColors)
        vflags = vflags | m3::VertexFormatFlag::VertexColor;
    if (maxUVs >= 1)
        vflags = vflags | m3::VertexFormatFlag::UV1;
    if (maxUVs >= 2)
        vflags = vflags | m3::VertexFormatFlag::UV2;
    if (maxUVs >= 3)
        vflags = vflags | m3::VertexFormatFlag::UV3;
    if (maxUVs >= 4)
        vflags = vflags | m3::VertexFormatFlag::UV4;
    if (maxUVs >= 5)
        vflags = vflags | m3::VertexFormatFlag::UV5;
    m3.vertices.flags = vflags;

    // We must build the raw vertex data blob. M3 vertex layout:
    // position (12B) + boneWeights (4B) + boneIndices (4B) + normal (4B packed) +
    // [vertexColor 4B] + [UV layers, 4B each] + tangent (4B packed)
    // Total stride = 24 + (color ? 4 : 0) + (numUVs * 4) + 4
    size_t const stride = 24 + (anyColors ? 4 : 0) + (maxUVs * 4) + 4;
    size_t totalVerts = 0;
    for (const auto& mesh : wemModel.meshes)
        totalVerts += mesh.positions.size();

    m3.vertices.data.resize(totalVerts * stride, 0);

    u32 globalVertOffset = 0;
    for (size_t mi = 0; mi < wemModel.meshes.size(); ++mi) {
        const auto& mesh = wemModel.meshes[mi];
        size_t const vertCount = mesh.positions.size();

        m3::MeshDivision div;

        // Write vertex data for this mesh
        for (size_t v = 0; v < vertCount; ++v) {
            size_t const base = (globalVertOffset + v) * stride;
            u8* ptr = m3.vertices.data.data() + base;

            // Position (12B float3)
            if (v < mesh.positions.size()) {
                auto* fp = reinterpret_cast<f32*>(ptr);
                fp[0] = mesh.positions[v].x;
                fp[1] = mesh.positions[v].y;
                fp[2] = mesh.positions[v].z;
            }

            // Bone weights (4B at offset 12)
            if (v < mesh.boneWeights.size()) {
                ptr[12] = mesh.boneWeights[v][0];
                ptr[13] = mesh.boneWeights[v][1];
                ptr[14] = mesh.boneWeights[v][2];
                ptr[15] = mesh.boneWeights[v][3];
            }

            // Bone indices (4B at offset 16)
            if (v < mesh.boneIndices.size()) {
                ptr[16] = mesh.boneIndices[v][0];
                ptr[17] = mesh.boneIndices[v][1];
                ptr[18] = mesh.boneIndices[v][2];
                ptr[19] = mesh.boneIndices[v][3];
            }

            // Normal (packed 3xi8 + sign at offset 20)
            if (v < mesh.normals.size()) {
                auto n = mesh.normals[v];
                ptr[20] = static_cast<u8>(
                    static_cast<i8>(std::max(-127.0f, std::min(127.0f, n.x * 127.0f))));
                ptr[21] = static_cast<u8>(
                    static_cast<i8>(std::max(-127.0f, std::min(127.0f, n.y * 127.0f))));
                ptr[22] = static_cast<u8>(
                    static_cast<i8>(std::max(-127.0f, std::min(127.0f, n.z * 127.0f))));
                ptr[23] = 0; // sign byte
            }

            size_t off = 24;

            // Vertex color (4B BGRA if present)
            if (anyColors) {
                if (v < mesh.vertexColors.size()) {
                    // WEM stores RGBA, M3 stores BGRA
                    ptr[off + 0] = mesh.vertexColors[v][2]; // B
                    ptr[off + 1] = mesh.vertexColors[v][1]; // G
                    ptr[off + 2] = mesh.vertexColors[v][0]; // R
                    ptr[off + 3] = mesh.vertexColors[v][3]; // A
                } else {
                    ptr[off + 0] = 255;
                    ptr[off + 1] = 255;
                    ptr[off + 2] = 255;
                    ptr[off + 3] = 255;
                }
                off += 4;
            }

            // UV layers (2xi16 each, scaled by 2048)
            for (size_t u = 0; u < maxUVs; ++u) {
                if (u < mesh.uvSets.size() && v < mesh.uvSets[u].size()) {
                    Vector2f const uv = mesh.uvSets[u][v];
                    auto uvI16u =
                        static_cast<i16>(std::max(-32768.0f, std::min(32767.0f, uv.x * 2048.0f)));
                    auto uvI16v =
                        static_cast<i16>(std::max(-32768.0f, std::min(32767.0f, uv.y * 2048.0f)));
                    auto* ip = reinterpret_cast<i16*>(ptr + off);
                    ip[0] = uvI16u;
                    ip[1] = uvI16v;
                }
                off += 4;
            }

            // Tangent (packed 3xi8 + sign at end of vertex)
            if (v < mesh.tangents.size()) {
                auto t = mesh.tangents[v];
                ptr[off + 0] = static_cast<u8>(
                    static_cast<i8>(std::max(-127.0f, std::min(127.0f, t.x * 127.0f))));
                ptr[off + 1] = static_cast<u8>(
                    static_cast<i8>(std::max(-127.0f, std::min(127.0f, t.y * 127.0f))));
                ptr[off + 2] = static_cast<u8>(
                    static_cast<i8>(std::max(-127.0f, std::min(127.0f, t.z * 127.0f))));
                ptr[off + 3] = (t.w >= 0.0f) ? 0 : 0xFF; // bitangent sign
            }
        }

        // Faces: u32 → u16 (rebased to global vertex indices)
        div.faces.reserve(mesh.indices.size());
        for (u32 const idx : mesh.indices) {
            u32 globalIdx = idx + globalVertOffset;
            if (globalIdx > 0xFFFF) {
                issues.push_back("Mesh " + std::to_string(mi) + " global index > 65535");
                div.faces.push_back(0xFFFF);
            } else {
                div.faces.push_back(static_cast<u16>(globalIdx));
            }
        }

        // Regions from submeshes
        div.regions.reserve(mesh.submeshes.size());
        for (size_t si = 0; si < mesh.submeshes.size(); ++si) {
            const auto& sub = mesh.submeshes[si];
            m3::Region reg{};
            reg.index = static_cast<u32>(si);
            reg.firstVertex = sub.vertexStart + globalVertOffset;
            reg.vertexCount = sub.vertexCount;
            reg.firstIndex = sub.indexStart;
            reg.indexCount = sub.indexCount;
            reg.boneWeightPairs = static_cast<u8>(sub.maxBoneInfluences);
            reg.rootBone = sub.rootBone;
            reg.flags = convertSubmeshFlagsToM3(sub.flags);
            reg.uvScale = 1.0f;
            reg.uvOffset = 0.0f;
            div.regions.push_back(reg);

            // Batch
            m3::Batch batch{};
            batch.regionIndex = static_cast<u16>(si);
            batch.materialIndex = static_cast<u16>(sub.materialIndex);
            div.batches.push_back(batch);
        }

        m3.divisions.push_back(std::move(div));
        globalVertOffset += static_cast<u32>(vertCount);
    }

    m3.vertices.initialize();

    return result;
}

// ============================================================================
// M3Converter — FormatConverter interface
// ============================================================================

std::string M3Converter::formatId() const {
    return "m3";
}
std::string M3Converter::formatName() const {
    return "StarCraft II / HotS M3";
}
bool M3Converter::supportsImport() const {
    return true;
}
bool M3Converter::supportsExport() const {
    return true;
}
u32 M3Converter::defaultExportVersion() const {
    return 30;
}

ConvertResult M3Converter::importFromBytes(std::span<const u8> data) const {
    m3::Parser parser;
    auto m3Model = parser.parse(data);
    auto result = fromM3(m3Model);
    for (const auto& issue : parser.getIssues()) {
        result.issues.push_back(issue);
    }
    return result;
}

ExportResult M3Converter::exportToBytes(const Model& model, u32 version) const {
    auto m3Result = toM3(model, version == 0 ? defaultExportVersion() : version);
    ExportResult result;
    result.issues = std::move(m3Result.issues);
    m3::Writer writer;
    result.data = writer.write(m3Result.model);
    return result;
}

// ============================================================================
// Legacy free functions
// ============================================================================

ConvertResult fromM3(const m3::Model& m3Model) {
    static const M3Converter converter;
    return converter.fromM3(m3Model);
}

M3ConvertResult toM3(const Model& wemModel, u32 targetVersion) {
    static const M3Converter converter;
    return converter.toM3(wemModel, targetVersion);
}

} // namespace wem
} // namespace models
} // namespace whiteout

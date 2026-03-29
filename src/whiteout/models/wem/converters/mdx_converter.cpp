// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/mdx/parser.h"
#include "whiteout/models/mdx/writer.h"
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

wem::Extent convertExtent(const mdx::Extent& src) {
    wem::Extent dst;
    dst.minimum = src.minimum;
    dst.maximum = src.maximum;
    dst.sphereRadius = src.boundsRadius;
    return dst;
}

mdx::Extent convertExtentBack(const wem::Extent& src) {
    mdx::Extent dst;
    dst.minimum = src.minimum;
    dst.maximum = src.maximum;
    dst.boundsRadius = src.sphereRadius;
    return dst;
}

BlendMode convertFilterMode(mdx::Layer::FilterMode fm) {
    switch (fm) {
    case mdx::Layer::FilterMode::None:
        return BlendMode::Opaque;
    case mdx::Layer::FilterMode::Transparent:
        return BlendMode::Transparent;
    case mdx::Layer::FilterMode::Blend:
        return BlendMode::AlphaBlend;
    case mdx::Layer::FilterMode::Additive:
        return BlendMode::Additive;
    case mdx::Layer::FilterMode::AddAlpha:
        return BlendMode::AdditiveAlpha;
    case mdx::Layer::FilterMode::Modulate:
        return BlendMode::Modulate;
    case mdx::Layer::FilterMode::Modulate2x:
        return BlendMode::Modulate2x;
    default:
        return BlendMode::Opaque;
    }
}

mdx::Layer::FilterMode convertBlendModeToFilterMode(BlendMode bm) {
    switch (bm) {
    case BlendMode::Opaque:
        return mdx::Layer::FilterMode::None;
    case BlendMode::Transparent:
        return mdx::Layer::FilterMode::Transparent;
    case BlendMode::AlphaBlend:
        return mdx::Layer::FilterMode::Blend;
    case BlendMode::Additive:
        return mdx::Layer::FilterMode::Additive;
    case BlendMode::AdditiveAlpha:
        return mdx::Layer::FilterMode::AddAlpha;
    case BlendMode::Modulate:
        return mdx::Layer::FilterMode::Modulate;
    case BlendMode::Modulate2x:
        return mdx::Layer::FilterMode::Modulate2x;
    default:
        return mdx::Layer::FilterMode::None;
    }
}

MaterialFlags convertShadingFlags(mdx::Layer::ShadingFlag sf) {
    auto flags = MaterialFlags::None;
    auto sfval = static_cast<u32>(sf);
    if (sfval & static_cast<u32>(mdx::Layer::ShadingFlag::Unshaded))
        flags |= MaterialFlags::Unlit;
    if (sfval & static_cast<u32>(mdx::Layer::ShadingFlag::SphereEnvMap))
        flags |= MaterialFlags::SphereEnvMap;
    if (sfval & static_cast<u32>(mdx::Layer::ShadingFlag::TwoSided))
        flags |= MaterialFlags::TwoSided;
    if (sfval & static_cast<u32>(mdx::Layer::ShadingFlag::Unfogged))
        flags |= MaterialFlags::Unfogged;
    if (sfval & static_cast<u32>(mdx::Layer::ShadingFlag::NoDepthTest))
        flags |= MaterialFlags::NoDepthTest;
    if (sfval & static_cast<u32>(mdx::Layer::ShadingFlag::NoDepthSet))
        flags |= MaterialFlags::NoDepthWrite;
    return flags;
}

mdx::Layer::ShadingFlag convertMaterialFlagsToShading(MaterialFlags mf) {
    auto result = mdx::Layer::ShadingFlag::None;
    if (hasFlag(mf, MaterialFlags::Unlit))
        result = result | mdx::Layer::ShadingFlag::Unshaded;
    if (hasFlag(mf, MaterialFlags::SphereEnvMap))
        result = result | mdx::Layer::ShadingFlag::SphereEnvMap;
    if (hasFlag(mf, MaterialFlags::TwoSided))
        result = result | mdx::Layer::ShadingFlag::TwoSided;
    if (hasFlag(mf, MaterialFlags::Unfogged))
        result = result | mdx::Layer::ShadingFlag::Unfogged;
    if (hasFlag(mf, MaterialFlags::NoDepthTest))
        result = result | mdx::Layer::ShadingFlag::NoDepthTest;
    if (hasFlag(mf, MaterialFlags::NoDepthWrite))
        result = result | mdx::Layer::ShadingFlag::NoDepthSet;
    return result;
}

FresnelProperties convertLayerFresnel(const mdx::Layer& layer) {
    FresnelProperties fp;
    fp.color = layer.fresnelColor;
    fp.opacity = layer.fresnelOpacity;
    fp.teamColor = layer.fresnelTeamColor;
    return fp;
}

} // anonymous namespace

// ============================================================================
// fromMdx
// ============================================================================

ConvertResult MdxConverter::fromMdx(const mdx::Model& mdxModel) const {
    ConvertResult result;
    auto& model = result.model;
    auto& issues = result.issues;

    model.name = mdxModel.modelName;
    model.bounds = convertExtent(mdxModel.modelExtent);

    // ── Textures ───────────────────────────────────────────────────
    model.textures.reserve(mdxModel.textures.size());
    for (const auto& tex : mdxModel.textures) {
        TextureRef ref;
        ref.path = tex.fileName;
        ref.flags = tex.flags;
        ref.replaceableId = tex.replaceableId;
        model.textures.push_back(std::move(ref));
    }

    // ── Materials ──────────────────────────────────────────────────
    // MDX materials have 1+ layers. Single-layer → Standard, multi-layer → Composite.
    model.materials.reserve(mdxModel.materials.size());
    for (const auto& mat : mdxModel.materials) {
        if (mat.layers.size() == 1) {
            // Single layer → Standard material
            const auto& layer = mat.layers[0];
            Material wMat;
            wMat.type = MaterialType::Standard;
            wMat.priorityPlane = static_cast<i32>(mat.priorityPlane);
            wMat.blendMode = convertFilterMode(layer.filterMode);
            wMat.flags = convertShadingFlags(layer.shadingFlags);
            wMat.shader = mat.shader;
            wMat.emissiveGain = layer.emissiveGain;
            wMat.fresnel = convertLayerFresnel(layer);

            // Primary texture slot
            if (layer.is_hd && !layer.subTextures.empty()) {
                // Reforged HD: subTextures define named slots
                for (const auto& sub : layer.subTextures) {
                    TextureSlot slot;
                    slot.textureIndex = sub.textureId;
                    slot.uvSetIndex = layer.coordId;
                    slot.alpha = layer.alpha;
                    switch (sub.slot) {
                    case mdx::Layer::SlotType::DiffuseMap:
                        slot.semantic = TextureSlotSemantic::Diffuse;
                        break;
                    case mdx::Layer::SlotType::NormalMap:
                        slot.semantic = TextureSlotSemantic::Normal;
                        break;
                    case mdx::Layer::SlotType::ORMMap:
                        slot.semantic = TextureSlotSemantic::AmbientOcclusion;
                        break;
                    case mdx::Layer::SlotType::EmissiveMap:
                        slot.semantic = TextureSlotSemantic::Emissive1;
                        break;
                    case mdx::Layer::SlotType::TeamColor:
                        slot.semantic = TextureSlotSemantic::Custom;
                        break;
                    case mdx::Layer::SlotType::EnvironmentMap:
                        slot.semantic = TextureSlotSemantic::Environment;
                        break;
                    default:
                        slot.semantic = TextureSlotSemantic::Custom;
                        break;
                    }
                    wMat.textureSlots.push_back(std::move(slot));
                }
            } else {
                // Classic: single texture per layer
                TextureSlot slot;
                slot.textureIndex = layer.textureId;
                slot.uvSetIndex = layer.coordId;
                slot.alpha = layer.alpha;
                slot.semantic = TextureSlotSemantic::Diffuse;
                wMat.textureSlots.push_back(std::move(slot));
            }

            model.materials.push_back(std::move(wMat));
        } else {
            // Multi-layer → Composite material wrapping per-layer Standards
            // First, create one Standard per layer
            u32 baseIdx = static_cast<u32>(model.materials.size());
            std::vector<CompositeSection> sections;
            sections.reserve(mat.layers.size());

            for (const auto& layer : mat.layers) {
                Material layerMat;
                layerMat.type = MaterialType::Standard;
                layerMat.priorityPlane = static_cast<i32>(mat.priorityPlane);
                layerMat.blendMode = convertFilterMode(layer.filterMode);
                layerMat.flags = convertShadingFlags(layer.shadingFlags);
                layerMat.shader = mat.shader;
                layerMat.emissiveGain = layer.emissiveGain;
                layerMat.fresnel = convertLayerFresnel(layer);

                TextureSlot slot;
                slot.textureIndex = layer.textureId;
                slot.uvSetIndex = layer.coordId;
                slot.alpha = layer.alpha;
                slot.semantic = TextureSlotSemantic::Diffuse;
                layerMat.textureSlots.push_back(std::move(slot));

                CompositeSection section;
                section.materialIndex = static_cast<u32>(model.materials.size());
                section.blendWeight = 1.0f;
                section.blendMode = convertFilterMode(layer.filterMode);
                sections.push_back(section);

                model.materials.push_back(std::move(layerMat));
            }

            // Then the Composite that references them
            Material compMat;
            compMat.type = MaterialType::Composite;
            compMat.priorityPlane = static_cast<i32>(mat.priorityPlane);
            compMat.sections = std::move(sections);
            model.materials.push_back(std::move(compMat));
        }
    }

    // ── Geosets → Meshes ───────────────────────────────────────────
    model.meshes.reserve(mdxModel.geosets.size());
    for (size_t gi = 0; gi < mdxModel.geosets.size(); ++gi) {
        const auto& geo = mdxModel.geosets[gi];
        Mesh mesh;
        mesh.name = geo.lodName.empty() ? "geoset_" + std::to_string(gi) : geo.lodName;
        mesh.lodLevel = geo.lod;
        mesh.bounds = convertExtent(geo.extent);

        // Vertex data
        mesh.positions = geo.vertexPositions;
        mesh.normals = geo.vertexNormals;
        mesh.tangents = geo.tangents;

        // UV sets
        mesh.uvSets.reserve(geo.textureCoordinateSets.size());
        for (const auto& uvSet : geo.textureCoordinateSets) {
            mesh.uvSets.push_back(uvSet);
        }

        // Indices: u16 → u32
        mesh.indices.reserve(geo.faces.size());
        for (u16 idx : geo.faces) {
            mesh.indices.push_back(static_cast<u32>(idx));
        }

        // Skinning data: MDX stores as packed u8 array (4 bone indices + 4 bone weights per vertex)
        if (!geo.skinData.empty()) {
            size_t vertCount = geo.vertexPositions.size();
            mesh.boneIndices.resize(vertCount);
            mesh.boneWeights.resize(vertCount);
            for (size_t v = 0; v < vertCount && (v * 8 + 7) < geo.skinData.size(); ++v) {
                size_t offset = v * 8;
                mesh.boneIndices[v] = {geo.skinData[offset + 0], geo.skinData[offset + 1],
                                       geo.skinData[offset + 2], geo.skinData[offset + 3]};
                mesh.boneWeights[v] = {geo.skinData[offset + 4], geo.skinData[offset + 5],
                                       geo.skinData[offset + 6], geo.skinData[offset + 7]};
            }
        }

        // Single submesh per geoset (the whole geoset is one draw section)
        Submesh sub;
        sub.name = mesh.name;
        sub.indexStart = 0;
        sub.indexCount = static_cast<u32>(mesh.indices.size());
        sub.vertexStart = 0;
        sub.vertexCount = static_cast<u32>(mesh.positions.size());
        sub.selectionGroup = static_cast<u16>(geo.selectionGroup);
        sub.selectionFlags = static_cast<u16>(geo.selectionFlags);
        sub.bounds = mesh.bounds;

        // Resolve material index: MDX materialId indexes into mdxModel.materials[].
        // For single-layer materials this maps 1:1. For multi-layer materials,
        // the composite was appended after its per-layer standards.
        // We need to map this correctly.
        u32 mdxMatIdx = geo.materialId;
        if (mdxMatIdx < mdxModel.materials.size()) {
            // Walk through materials to find the correct WEM index
            u32 wemIdx = 0;
            for (u32 mi = 0; mi < mdxMatIdx; ++mi) {
                size_t layerCount = mdxModel.materials[mi].layers.size();
                if (layerCount <= 1) {
                    wemIdx += 1;
                } else {
                    wemIdx += static_cast<u32>(layerCount) + 1; // N standards + 1 composite
                }
            }
            // Point to the actual material for this geoset
            size_t layerCount = mdxModel.materials[mdxMatIdx].layers.size();
            if (layerCount <= 1) {
                sub.materialIndex = wemIdx;
            } else {
                // Use the composite (last one in the group)
                sub.materialIndex = wemIdx + static_cast<u32>(layerCount);
            }
        } else {
            sub.materialIndex = 0;
            issues.push_back("Geoset " + std::to_string(gi) + " has out-of-range materialId " +
                             std::to_string(mdxMatIdx));
        }

        mesh.submeshes.push_back(std::move(sub));
        model.meshes.push_back(std::move(mesh));
    }

    return result;
}

// ============================================================================
// toMdx
// ============================================================================

MdxConvertResult MdxConverter::toMdx(const Model& wemModel, u32 targetVersion) const {
    MdxConvertResult result;
    auto& mdx = result.model;
    auto& issues = result.issues;

    mdx.version = targetVersion;
    mdx.modelName = wemModel.name;
    mdx.modelExtent = convertExtentBack(wemModel.bounds);

    // ── Textures ───────────────────────────────────────────────────
    mdx.textures.reserve(wemModel.textures.size());
    for (const auto& ref : wemModel.textures) {
        mdx::Texture tex;
        tex.fileName = ref.path;
        tex.flags = ref.flags;
        tex.replaceableId = ref.replaceableId;
        mdx.textures.push_back(std::move(tex));
    }

    // ── Materials ──────────────────────────────────────────────────
    // Build a mapping from WEM material index → MDX material index
    // Standard → single-layer MDX material
    // Composite → multi-layer MDX material (layers from referenced standards)
    for (size_t mi = 0; mi < wemModel.materials.size(); ++mi) {
        const auto& wMat = wemModel.materials[mi];

        if (wMat.type == MaterialType::Composite) {
            mdx::Material mat;
            mat.priorityPlane = static_cast<u32>(wMat.priorityPlane);
            mat.shader = wMat.shader;

            for (const auto& section : wMat.sections) {
                mdx::Layer layer;
                layer.filterMode = convertBlendModeToFilterMode(section.blendMode);

                // Pull data from the referenced standard material
                if (section.materialIndex < wemModel.materials.size()) {
                    const auto& srcMat = wemModel.materials[section.materialIndex];
                    layer.shadingFlags = convertMaterialFlagsToShading(srcMat.flags);
                    layer.emissiveGain = srcMat.emissiveGain;
                    layer.fresnelColor = srcMat.fresnel.color;
                    layer.fresnelOpacity = srcMat.fresnel.opacity;
                    layer.fresnelTeamColor = srcMat.fresnel.teamColor;
                    if (!srcMat.textureSlots.empty()) {
                        layer.textureId = srcMat.textureSlots[0].textureIndex;
                        layer.coordId = srcMat.textureSlots[0].uvSetIndex;
                        layer.alpha = srcMat.textureSlots[0].alpha;
                    }
                }

                mat.layers.push_back(std::move(layer));
            }

            mdx.materials.push_back(std::move(mat));
        } else if (wMat.type == MaterialType::Standard) {
            // Check if this standard is referenced by a composite;
            // if so, skip it (it was folded into the composite's layers above)
            bool isChild = false;
            for (const auto& other : wemModel.materials) {
                if (other.type == MaterialType::Composite) {
                    for (const auto& sec : other.sections) {
                        if (sec.materialIndex == mi) {
                            isChild = true;
                            break;
                        }
                    }
                }
                if (isChild)
                    break;
            }
            if (isChild)
                continue;

            // Standalone Standard → single-layer MDX material
            mdx::Material mat;
            mat.priorityPlane = static_cast<u32>(wMat.priorityPlane);
            mat.shader = wMat.shader;

            mdx::Layer layer;
            layer.filterMode = convertBlendModeToFilterMode(wMat.blendMode);
            layer.shadingFlags = convertMaterialFlagsToShading(wMat.flags);
            layer.emissiveGain = wMat.emissiveGain;
            layer.fresnelColor = wMat.fresnel.color;
            layer.fresnelOpacity = wMat.fresnel.opacity;
            layer.fresnelTeamColor = wMat.fresnel.teamColor;
            if (!wMat.textureSlots.empty()) {
                layer.textureId = wMat.textureSlots[0].textureIndex;
                layer.coordId = wMat.textureSlots[0].uvSetIndex;
                layer.alpha = wMat.textureSlots[0].alpha;
            }

            mat.layers.push_back(std::move(layer));
            mdx.materials.push_back(std::move(mat));
        }
    }

    // Build a reverse map: WEM material idx → MDX material idx for geoset assignment
    std::vector<u32> wemToMdxMat(wemModel.materials.size(), 0);
    {
        u32 mdxIdx = 0;
        for (size_t mi = 0; mi < wemModel.materials.size(); ++mi) {
            const auto& wMat = wemModel.materials[mi];
            if (wMat.type == MaterialType::Composite) {
                wemToMdxMat[mi] = mdxIdx;
                mdxIdx++;
            } else if (wMat.type == MaterialType::Standard) {
                bool isChild = false;
                for (const auto& other : wemModel.materials) {
                    if (other.type == MaterialType::Composite) {
                        for (const auto& sec : other.sections) {
                            if (sec.materialIndex == mi) {
                                isChild = true;
                                break;
                            }
                        }
                    }
                    if (isChild)
                        break;
                }
                if (!isChild) {
                    wemToMdxMat[mi] = mdxIdx;
                    mdxIdx++;
                }
            }
        }
    }

    // ── Meshes → Geosets ───────────────────────────────────────────
    mdx.geosets.reserve(wemModel.meshes.size());
    for (size_t mi = 0; mi < wemModel.meshes.size(); ++mi) {
        const auto& mesh = wemModel.meshes[mi];
        mdx::Geoset geo;
        geo.lodName = mesh.name;
        geo.lod = mesh.lodLevel;
        geo.extent = convertExtentBack(mesh.bounds);
        geo.vertexPositions = mesh.positions;
        geo.vertexNormals = mesh.normals;
        geo.tangents = mesh.tangents;
        geo.textureCoordinateSets = mesh.uvSets;

        // Indices: u32 → u16 (MDX limit)
        geo.faces.reserve(mesh.indices.size());
        for (u32 idx : mesh.indices) {
            if (idx > 0xFFFF) {
                issues.push_back("Mesh " + std::to_string(mi) + " has index > 65535, clamped");
                geo.faces.push_back(0xFFFF);
            } else {
                geo.faces.push_back(static_cast<u16>(idx));
            }
        }

        // Skinning data: pack bone indices + weights into u8 array
        if (!mesh.boneIndices.empty() && !mesh.boneWeights.empty()) {
            size_t vertCount = mesh.positions.size();
            geo.skinData.resize(vertCount * 8);
            for (size_t v = 0; v < vertCount; ++v) {
                size_t offset = v * 8;
                const auto& bi = mesh.boneIndices[v];
                const auto& bw = mesh.boneWeights[v];
                geo.skinData[offset + 0] = bi[0];
                geo.skinData[offset + 1] = bi[1];
                geo.skinData[offset + 2] = bi[2];
                geo.skinData[offset + 3] = bi[3];
                geo.skinData[offset + 4] = bw[0];
                geo.skinData[offset + 5] = bw[1];
                geo.skinData[offset + 6] = bw[2];
                geo.skinData[offset + 7] = bw[3];
            }
        }

        // Use first submesh's material
        if (!mesh.submeshes.empty()) {
            const auto& sub = mesh.submeshes[0];
            geo.materialId = wemToMdxMat[sub.materialIndex];
            geo.selectionGroup = sub.selectionGroup;
            geo.selectionFlags = sub.selectionFlags;
        }

        mdx.geosets.push_back(std::move(geo));
    }

    return result;
}

// ============================================================================
// MdxConverter — FormatConverter interface
// ============================================================================

std::string MdxConverter::formatId() const {
    return "mdx";
}
std::string MdxConverter::formatName() const {
    return "Warcraft III MDX";
}
bool MdxConverter::supportsImport() const {
    return true;
}
bool MdxConverter::supportsExport() const {
    return true;
}
u32 MdxConverter::defaultExportVersion() const {
    return 800;
}

ConvertResult MdxConverter::importFromBytes(std::span<const u8> data) const {
    mdx::Parser parser;
    auto mdxModel = parser.parse(data);
    auto result = fromMdx(mdxModel);
    for (const auto& issue : parser.getIssues()) {
        result.issues.push_back(issue);
    }
    return result;
}

ExportResult MdxConverter::exportToBytes(const Model& model, u32 version) const {
    auto mdxResult = toMdx(model, version == 0 ? defaultExportVersion() : version);
    ExportResult result;
    result.issues = std::move(mdxResult.issues);
    mdx::Writer writer;
    result.data = writer.write(mdxResult.model);
    return result;
}

// ============================================================================
// Legacy free functions
// ============================================================================

ConvertResult fromMdx(const mdx::Model& mdxModel) {
    static const MdxConverter converter;
    return converter.fromMdx(mdxModel);
}

MdxConvertResult toMdx(const Model& wemModel, u32 targetVersion) {
    static const MdxConverter converter;
    return converter.toMdx(wemModel, targetVersion);
}

} // namespace wem
} // namespace models
} // namespace whiteout

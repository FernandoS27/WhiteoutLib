// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/converters.h"
#include "whiteout/models/m2/writer.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Helpers
// ============================================================================

namespace {

wem::Extent convertM2Extent(const m2::Extent& src) {
    wem::Extent dst;
    dst.minimum = src.minimum;
    dst.maximum = src.maximum;
    dst.sphereRadius = src.sphereRadius;
    return dst;
}

m2::Extent convertExtentToM2(const wem::Extent& src) {
    m2::Extent dst;
    dst.minimum = src.minimum;
    dst.maximum = src.maximum;
    dst.sphereRadius = src.sphereRadius;
    return dst;
}

BlendMode convertM2BlendMode(u16 bm) {
    switch (bm) {
    case 0: return BlendMode::Opaque;
    case 1: return BlendMode::AlphaKey;
    case 2: return BlendMode::AlphaBlend;
    case 3: return BlendMode::AdditiveAlpha;
    case 4: return BlendMode::Additive;
    case 5: return BlendMode::Modulate;
    case 6: return BlendMode::Modulate2x;
    case 7: return BlendMode::BlendAdd;
    default: return BlendMode::Opaque;
    }
}

u16 convertBlendModeToM2(BlendMode bm) {
    switch (bm) {
    case BlendMode::Opaque:       return 0;
    case BlendMode::AlphaKey:     return 1;
    case BlendMode::AlphaBlend:   return 2;
    case BlendMode::AdditiveAlpha:return 3;
    case BlendMode::Additive:     return 4;
    case BlendMode::Modulate:     return 5;
    case BlendMode::Modulate2x:   return 6;
    case BlendMode::BlendAdd:     return 7;
    case BlendMode::Transparent:  return 1; // closest M2 equivalent
    default:                      return 0;
    }
}

MaterialFlags convertM2MaterialFlags(u16 mf) {
    auto flags = MaterialFlags::None;
    if (mf & static_cast<u16>(m2::MaterialFlag::Unlit))
        flags |= MaterialFlags::Unlit;
    if (mf & static_cast<u16>(m2::MaterialFlag::Unfogged))
        flags |= MaterialFlags::Unfogged;
    if (mf & static_cast<u16>(m2::MaterialFlag::TwoSided))
        flags |= MaterialFlags::TwoSided;
    if (mf & static_cast<u16>(m2::MaterialFlag::DepthTest))
        flags |= MaterialFlags::DepthTest;
    if (mf & static_cast<u16>(m2::MaterialFlag::DepthWrite))
        flags |= MaterialFlags::DepthWrite;
    if (mf & static_cast<u16>(m2::MaterialFlag::NoAlphaComposite))
        flags |= MaterialFlags::NoAlphaComposite;
    return flags;
}

u16 convertMaterialFlagsToM2(MaterialFlags mf) {
    u16 result = 0;
    if (hasFlag(mf, MaterialFlags::Unlit))
        result |= static_cast<u16>(m2::MaterialFlag::Unlit);
    if (hasFlag(mf, MaterialFlags::Unfogged))
        result |= static_cast<u16>(m2::MaterialFlag::Unfogged);
    if (hasFlag(mf, MaterialFlags::TwoSided))
        result |= static_cast<u16>(m2::MaterialFlag::TwoSided);
    if (hasFlag(mf, MaterialFlags::DepthTest))
        result |= static_cast<u16>(m2::MaterialFlag::DepthTest);
    if (hasFlag(mf, MaterialFlags::DepthWrite))
        result |= static_cast<u16>(m2::MaterialFlag::DepthWrite);
    if (hasFlag(mf, MaterialFlags::NoAlphaComposite))
        result |= static_cast<u16>(m2::MaterialFlag::NoAlphaComposite);
    return result;
}

} // anonymous namespace

// ============================================================================
// fromM2
// ============================================================================

ConvertResult M2Converter::fromM2(const m2::Model& header) const {
    ConvertResult result;
    auto& model = result.model;
    auto& issues = result.issues;

    model.name = header.modelName;
    model.bounds = convertM2Extent(header.bounding);

    // ── Textures ───────────────────────────────────────────────────
    model.textures.reserve(header.textures.size());
    for (const auto& tex : header.textures) {
        TextureRef ref;
        ref.path = tex.filename;
        ref.flags = tex.flags;
        ref.type = tex.type;
        model.textures.push_back(std::move(ref));
    }

    // ── Materials ──────────────────────────────────────────────────
    // M2 materials are render-flag pairs (flags + blendMode).
    // Batches reference materials + textures independently, so we create
    // one WEM Standard material per unique (materialIndex, batch texture set) pair.
    // First pass: create base material entries from header.materials[]
    model.materials.reserve(header.materials.size());
    for (const auto& mat : header.materials) {
        Material wMat;
        wMat.type = MaterialType::Standard;
        wMat.blendMode = convertM2BlendMode(mat.blendingMode);
        wMat.flags = convertM2MaterialFlags(mat.flags);
        model.materials.push_back(std::move(wMat));
    }

    // ── Skin Profiles → Meshes ─────────────────────────────────────
    // Each skin profile becomes a WEM Mesh
    // Combine base skin profiles and LOD profiles
    struct SkinRef {
        const m2::SkinProfile* profile;
        int lodLevel;
        int index;
    };
    std::vector<SkinRef> allSkins;
    for (size_t i = 0; i < header.skinProfiles.size(); ++i) {
        allSkins.push_back({&header.skinProfiles[i], 0, static_cast<int>(i)});
    }
    for (size_t i = 0; i < header.lodProfiles.size(); ++i) {
        allSkins.push_back({&header.lodProfiles[i], static_cast<int>(i + 1), static_cast<int>(i)});
    }

    for (size_t si = 0; si < allSkins.size(); ++si) {
        const auto& skinRef = allSkins[si];
        const auto& skin = *skinRef.profile;
        Mesh mesh;
        mesh.name = header.modelName + "_skin" + std::to_string(si);
        mesh.lodLevel = static_cast<u32>(skinRef.lodLevel);

        // Resolve skin vertex indices → copy from global vertex array
        size_t vertCount = skin.vertices.size();
        mesh.positions.resize(vertCount);
        mesh.normals.resize(vertCount);
        mesh.boneIndices.resize(vertCount);
        mesh.boneWeights.resize(vertCount);
        mesh.uvSets.resize(2); // M2 always has 2 UV sets
        mesh.uvSets[0].resize(vertCount);
        mesh.uvSets[1].resize(vertCount);

        for (size_t v = 0; v < vertCount; ++v) {
            u32 globalIdx = static_cast<u32>(skin.vertices[v]) + skin.lodVertexBase;
            if (globalIdx < header.vertices.size()) {
                const auto& vert = header.vertices[globalIdx];
                mesh.positions[v] = vert.position;
                mesh.normals[v] = vert.normal;
                mesh.boneIndices[v] = vert.boneIndices;
                mesh.boneWeights[v] = vert.boneWeights;
                mesh.uvSets[0][v] = vert.texCoords[0];
                mesh.uvSets[1][v] = vert.texCoords[1];
            }
        }

        // Indices: skin.indices indexes into skin.vertices (already resolved above)
        mesh.indices.reserve(skin.indices.size());
        for (u16 idx : skin.indices) {
            mesh.indices.push_back(static_cast<u32>(idx));
        }

        // Submeshes from SkinSections
        mesh.submeshes.reserve(skin.submeshes.size());
        for (size_t ssi = 0; ssi < skin.submeshes.size(); ++ssi) {
            const auto& section = skin.submeshes[ssi];
            Submesh sub;
            sub.name = "section_" + std::to_string(ssi);
            sub.indexStart = section.indexStart;
            sub.indexCount = section.indexCount;
            sub.vertexStart = section.vertexStart;
            sub.vertexCount = section.vertexCount;
            sub.selectionGroup = section.skinSectionId;
            sub.centerPosition = section.centerPosition;
            sub.sortCenterPosition = section.sortCenterPosition;
            sub.sortRadius = section.sortRadius;
            sub.centerBoneIndex = section.centerBoneIndex;
            sub.maxBoneInfluences = section.boneInfluences;

            // Find the batch that references this skin section to get material info
            // Multiple batches can reference the same section for multi-pass;
            // we take the first (primary) batch
            bool batchFound = false;
            for (const auto& batch : skin.batches) {
                if (batch.skinSectionIndex == ssi) {
                    sub.materialIndex = batch.materialIndex;

                    // Resolve texture from combo table and attach to material
                    if (batch.textureComboIndex < header.textureCombos.size()) {
                        u16 texIdx = header.textureCombos[batch.textureComboIndex];
                        if (sub.materialIndex < model.materials.size()) {
                            auto& mat = model.materials[sub.materialIndex];
                            // Only add if not already populated
                            if (mat.textureSlots.empty()) {
                                mat.shaderId = batch.shaderId;
                                mat.priorityPlane = batch.priorityPlane;
                                for (u16 t = 0; t < batch.textureCount; ++t) {
                                    u16 comboIdx = batch.textureComboIndex + t;
                                    if (comboIdx < header.textureCombos.size()) {
                                        TextureSlot slot;
                                        slot.textureIndex = header.textureCombos[comboIdx];
                                        slot.semantic = (t == 0) ? TextureSlotSemantic::Diffuse
                                                                 : TextureSlotSemantic::Custom;
                                        // Resolve UV set from textureCoordCombos
                                        u16 uvComboIdx = batch.textureCoordComboIndex + t;
                                        if (uvComboIdx < header.textureCoordCombos.size()) {
                                            u16 uvSel = header.textureCoordCombos[uvComboIdx];
                                            slot.uvSetIndex = (uvSel <= 1) ? uvSel : 0;
                                        }
                                        mat.textureSlots.push_back(std::move(slot));
                                    }
                                }
                            }
                        }
                    }

                    batchFound = true;
                    break;
                }
            }
            if (!batchFound) {
                issues.push_back("Skin " + std::to_string(si) + " section " +
                                 std::to_string(ssi) + " has no matching batch");
            }

            mesh.submeshes.push_back(std::move(sub));
        }

        // Compute mesh bounds from vertex positions
        if (!mesh.positions.empty()) {
            Vector3f minP = mesh.positions[0];
            Vector3f maxP = mesh.positions[0];
            f32 maxDistSq = 0;
            for (const auto& p : mesh.positions) {
                minP = Vector3f(std::min(minP.x, p.x), std::min(minP.y, p.y), std::min(minP.z, p.z));
                maxP = Vector3f(std::max(maxP.x, p.x), std::max(maxP.y, p.y), std::max(maxP.z, p.z));
                f32 distSq = p.x * p.x + p.y * p.y + p.z * p.z;
                if (distSq > maxDistSq) maxDistSq = distSq;
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
// toM2
// ============================================================================

M2ConvertResult M2Converter::toM2(const Model& wemModel, u32 targetVersion) const {
    M2ConvertResult result;
    auto& header = result.model;
    auto& issues = result.issues;

    header.modelName = wemModel.name;
    header.bounding = convertExtentToM2(wemModel.bounds);

    // ── Textures ───────────────────────────────────────────────────
    header.textures.reserve(wemModel.textures.size());
    for (const auto& ref : wemModel.textures) {
        m2::Texture tex;
        tex.filename = ref.path;
        tex.flags = ref.flags;
        tex.type = ref.type;
        header.textures.push_back(std::move(tex));
    }

    // ── Materials ──────────────────────────────────────────────────
    header.materials.reserve(wemModel.materials.size());
    for (const auto& wMat : wemModel.materials) {
        if (wMat.type == MaterialType::Composite) {
            issues.push_back("Composite material '" + wMat.name +
                             "' not supported in M2, flattening to first section");
            // Use first section's properties
            if (!wMat.sections.empty() &&
                wMat.sections[0].materialIndex < wemModel.materials.size()) {
                const auto& srcMat = wemModel.materials[wMat.sections[0].materialIndex];
                m2::Material mat;
                mat.flags = convertMaterialFlagsToM2(srcMat.flags);
                mat.blendingMode = convertBlendModeToM2(srcMat.blendMode);
                header.materials.push_back(std::move(mat));
            } else {
                m2::Material mat;
                header.materials.push_back(std::move(mat));
            }
        } else {
            m2::Material mat;
            mat.flags = convertMaterialFlagsToM2(wMat.flags);
            mat.blendingMode = convertBlendModeToM2(wMat.blendMode);
            header.materials.push_back(std::move(mat));
        }
    }

    // ── Build global vertex array from all meshes ──────────────────
    // M2 has one global vertex array; skins reference subsets via index buffers
    u32 globalVertexOffset = 0;
    for (size_t mi = 0; mi < wemModel.meshes.size(); ++mi) {
        const auto& mesh = wemModel.meshes[mi];
        for (size_t v = 0; v < mesh.positions.size(); ++v) {
            m2::Vertex vert;
            vert.position = mesh.positions[v];
            if (v < mesh.normals.size()) vert.normal = mesh.normals[v];
            if (v < mesh.boneIndices.size()) vert.boneIndices = mesh.boneIndices[v];
            if (v < mesh.boneWeights.size()) vert.boneWeights = mesh.boneWeights[v];
            if (!mesh.uvSets.empty() && v < mesh.uvSets[0].size())
                vert.texCoords[0] = mesh.uvSets[0][v];
            if (mesh.uvSets.size() > 1 && v < mesh.uvSets[1].size())
                vert.texCoords[1] = mesh.uvSets[1][v];
            header.vertices.push_back(std::move(vert));
        }

        // Build a skin profile for each mesh
        m2::SkinProfile skin;
        skin.lodVertexBase = globalVertexOffset;

        // vertices[] is just the identity mapping for this mesh's range
        size_t meshVertCount = mesh.positions.size();
        skin.vertices.resize(meshVertCount);
        for (size_t v = 0; v < meshVertCount; ++v) {
            skin.vertices[v] = static_cast<u16>(v);
        }

        // Copy indices
        skin.indices.reserve(mesh.indices.size());
        for (u32 idx : mesh.indices) {
            if (idx > 0xFFFF) {
                issues.push_back("Mesh " + std::to_string(mi) + " index > 65535, clamped");
                skin.indices.push_back(0xFFFF);
            } else {
                skin.indices.push_back(static_cast<u16>(idx));
            }
        }

        // Submeshes → SkinSections + Batches
        for (size_t si = 0; si < mesh.submeshes.size(); ++si) {
            const auto& sub = mesh.submeshes[si];
            m2::SkinSection section;
            section.skinSectionId = sub.selectionGroup;
            section.level = static_cast<u16>(mesh.lodLevel);
            section.vertexStart = static_cast<u16>(sub.vertexStart);
            section.vertexCount = static_cast<u16>(sub.vertexCount);
            section.indexStart = static_cast<u16>(sub.indexStart);
            section.indexCount = static_cast<u16>(sub.indexCount);
            section.boneInfluences = sub.maxBoneInfluences;
            section.centerBoneIndex = sub.centerBoneIndex;
            section.centerPosition = sub.centerPosition;
            section.sortCenterPosition = sub.sortCenterPosition;
            section.sortRadius = sub.sortRadius;
            skin.submeshes.push_back(section);

            // Build a batch for each submesh
            m2::Batch batch;
            batch.skinSectionIndex = static_cast<u16>(si);
            batch.materialIndex = static_cast<u16>(sub.materialIndex);

            // Build texture combo entries
            if (sub.materialIndex < wemModel.materials.size()) {
                const auto& wMat = wemModel.materials[sub.materialIndex];
                batch.shaderId = wMat.shaderId;
                batch.priorityPlane = static_cast<i8>(wMat.priorityPlane);
                batch.textureComboIndex = static_cast<u16>(header.textureCombos.size());
                batch.textureCount = static_cast<u16>(wMat.textureSlots.size());
                batch.textureCoordComboIndex = static_cast<u16>(header.textureCoordCombos.size());
                for (const auto& slot : wMat.textureSlots) {
                    header.textureCombos.push_back(static_cast<u16>(slot.textureIndex));
                    header.textureCoordCombos.push_back(static_cast<u16>(slot.uvSetIndex));
                }
            }

            skin.batches.push_back(batch);
        }

        header.skinProfiles.push_back(std::move(skin));
        globalVertexOffset += static_cast<u32>(meshVertCount);
    }

    header.numSkinProfiles = static_cast<u32>(header.skinProfiles.size());

    return result;
}

// ============================================================================
// M2Converter — FormatConverter interface
// ============================================================================

std::string M2Converter::formatId() const { return "m2"; }
std::string M2Converter::formatName() const { return "World of Warcraft M2"; }
bool M2Converter::supportsImport() const { return true; }
bool M2Converter::supportsExport() const { return true; }
u32 M2Converter::defaultExportVersion() const { return 274; }

ExportResult M2Converter::exportToBytes(const Model& model, u32 version) const {
    auto m2Result = toM2(model, version == 0 ? defaultExportVersion() : version);
    ExportResult result;
    result.issues = std::move(m2Result.issues);

    m2::WriteOptions opts;
    opts.m2Version = version == 0 ? defaultExportVersion() : version;
    m2::Writer writer(opts);
    auto serResult = writer.write(m2Result.model);
    result.data = std::move(serResult.m2Data);

    result.issues.push_back(
        "M2 byte-level export writes only the base .m2 file. "
        "Use toM2() for the full Model including skin profiles.");
    return result;
}

// ============================================================================
// Legacy free functions
// ============================================================================

ConvertResult fromM2(const m2::Model& m2Model) {
    static const M2Converter converter;
    return converter.fromM2(m2Model);
}

M2ConvertResult toM2(const Model& wemModel, u32 targetVersion) {
    static const M2Converter converter;
    return converter.toM2(wemModel, targetVersion);
}

} // namespace wem
} // namespace models
} // namespace whiteout

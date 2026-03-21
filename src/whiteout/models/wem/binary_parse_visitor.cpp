// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "binary_parse_visitor.h"

#include <cassert>
#include <type_traits>

namespace whiteout {
namespace models {
namespace wem {

using common::BinaryReader;

// ============================================================================
// Constructor
// ============================================================================

BinaryParseVisitor::BinaryParseVisitor(BinaryReader& reader) : reader(reader) {}

// ============================================================================
// Entry point
// ============================================================================

void BinaryParseVisitor::read(Model& model) {
    WEMHeader header = reader.read<WEMHeader>();

    if (header.magic != kWoemMagic) {
        issues.push_back("Invalid WEM file: expected magic 'WOEM'");
        return;
    }

    if (header.version > kCurrentVersion) {
        issues.push_back("WEM version " + std::to_string(header.version) +
                          " is newer than supported version " +
                          std::to_string(kCurrentVersion));
    }

    // Read index table
    reader.setPosition(header.indexOffset);
    indexTable = reader.read<std::vector<IndexEntry>>(header.indexCount);

    // Resolve model reference
    if (header.modelRef.entries == 0) {
        issues.push_back("WEM file has no model data");
        return;
    }

    if (header.modelRef.index >= indexTable.size()) {
        issues.push_back("WEM model reference index out of bounds");
        return;
    }

    reader.setPosition(indexTable[header.modelRef.index].offset);
    visit(model);
}

// ============================================================================
// Container visitors (templates)
// ============================================================================

template <typename T>
void BinaryParseVisitor::visit(std::vector<T>& container) {
    Reference ref = reader.read<Reference>();
    if (ref.entries == 0) {
        container.clear();
        return;
    }

    if (ref.index >= indexTable.size()) {
        issues.push_back("Reference index out of bounds: " + std::to_string(ref.index));
        container.clear();
        return;
    }

    const auto& entry = indexTable[ref.index];
    if (entry.tag != ChunkTagTraits<T>::value) {
        issues.push_back("Tag mismatch in reference: expected " +
                         std::to_string(ChunkTagTraits<T>::value) + " got " +
                         std::to_string(entry.tag));
    }

    const auto savedPos = reader.getPosition();
    reader.setPosition(entry.offset);

    if constexpr (ChunkTagTraits<T>::is_trivial) {
        container = reader.read<std::vector<T>>(ref.entries);
    } else {
        container.clear();
        container.resize(ref.entries);
        for (u32 i = 0; i < ref.entries; ++i) {
            visit(container[i]);
        }
    }

    reader.setPosition(savedPos);
}

void BinaryParseVisitor::visit(std::string& str) {
    Reference ref = reader.read<Reference>();
    if (ref.entries == 0) {
        str.clear();
        return;
    }

    if (ref.index >= indexTable.size()) {
        issues.push_back("String reference index out of bounds: " +
                         std::to_string(ref.index));
        str.clear();
        return;
    }

    const auto savedPos = reader.getPosition();
    reader.setPosition(indexTable[ref.index].offset);
    str = reader.readString(ref.entries, false);
    reader.setPosition(savedPos);
}

// ============================================================================
// Inline helpers
// ============================================================================

void BinaryParseVisitor::visitFresnel(FresnelProperties& f) {
    f.mode = static_cast<FresnelMode>(reader.read<u32>());
    f.exponent = reader.read<f32>();
    f.min = reader.read<f32>();
    f.max = reader.read<f32>();
    f.translation = reader.read<Vector3f>();
    f.mask = reader.read<Vector3f>();
    f.rotation = reader.read<Vector2f>();
    f.color = reader.read<Vector3f>();
    f.opacity = reader.read<f32>();
    f.teamColor = reader.read<f32>();
}

// ============================================================================
// Structure visitors
// ============================================================================

void BinaryParseVisitor::visit(Model& model) {
    visit(model.name);
    model.bounds = reader.read<Extent>();
    visit(model.meshes);
    visit(model.materials);
    visit(model.textures);
}

void BinaryParseVisitor::visit(TextureRef& tex) {
    visit(tex.path);
    tex.flags = reader.read<u32>();
    tex.replaceableId = reader.read<u32>();
    tex.type = reader.read<u32>();
}

void BinaryParseVisitor::visit(Material& mat) {
    mat.type = static_cast<MaterialType>(reader.read<u32>());
    visit(mat.name);
    mat.priorityPlane = reader.read<i32>();

    if (mat.type == MaterialType::Standard) {
        mat.blendMode = static_cast<BlendMode>(reader.read<u32>());
        mat.flags = static_cast<MaterialFlags>(reader.read<u32>());
        visit(mat.shader);
        mat.shaderId = reader.read<u16>();
        mat.specularExponent = reader.read<f32>();
        mat.alphaTestThreshold = reader.read<f32>();
        mat.depthBlendFalloff = reader.read<f32>();
        mat.emissiveGain = reader.read<f32>();
        mat.hdrSpecularMultiplier = reader.read<f32>();
        mat.hdrEmissiveMultiplier = reader.read<f32>();
        mat.hdrEnvironmentConstant = reader.read<f32>();
        mat.hdrEnvironmentDiffuse = reader.read<f32>();
        mat.hdrEnvironmentSpecular = reader.read<f32>();
        visitFresnel(mat.fresnel);
        mat.materialClass = static_cast<MaterialClass>(reader.read<u32>());
        mat.layerBlendMode = static_cast<LayerBlendOp>(reader.read<u32>());
        mat.emissiveBlendMode1 = static_cast<LayerBlendOp>(reader.read<u32>());
        mat.emissiveBlendMode2 = static_cast<LayerBlendOp>(reader.read<u32>());
        mat.specularMode = static_cast<SpecularMode>(reader.read<u32>());
        visit(mat.textureSlots);
    } else if (mat.type == MaterialType::Composite) {
        visit(mat.sections);
    }
}

void BinaryParseVisitor::visit(TextureSlot& slot) {
    slot.semantic = static_cast<TextureSlotSemantic>(reader.read<u32>());
    slot.textureIndex = reader.read<u32>();
    slot.uvSetIndex = reader.read<u32>();
    slot.uvMapping = static_cast<UVMappingMode>(reader.read<u32>());
    slot.wrapU = reader.read<u8>() != 0;
    slot.wrapV = reader.read<u8>() != 0;
    slot.alpha = reader.read<f32>();
    slot.colorChannelSelect = static_cast<ColorChannelSelect>(reader.read<u32>());
    slot.rgbMultiply = reader.read<f32>();
    slot.rgbAdd = reader.read<f32>();
    slot.flipbookRows = reader.read<u32>();
    slot.flipbookColumns = reader.read<u32>();
    visitFresnel(slot.fresnel);
}

void BinaryParseVisitor::visit(CompositeSection& sec) {
    sec.materialIndex = reader.read<u32>();
    sec.blendWeight = reader.read<f32>();
    sec.blendMode = static_cast<BlendMode>(reader.read<u32>());
}

void BinaryParseVisitor::visit(Mesh& mesh) {
    visit(mesh.name);
    mesh.lodLevel = reader.read<u32>();
    mesh.bounds = reader.read<Extent>();
    visit(mesh.positions);
    visit(mesh.normals);
    visit(mesh.tangents);
    visit(mesh.boneIndices);
    visit(mesh.boneWeights);

    // UV sets: inline count followed by individual VEC2 references
    u32 uvSetCount = reader.read<u32>();
    mesh.uvSets.resize(uvSetCount);
    for (u32 i = 0; i < uvSetCount; ++i) {
        visit(mesh.uvSets[i]);
    }

    visit(mesh.vertexColors);
    visit(mesh.indices);
    visit(mesh.submeshes);
}

void BinaryParseVisitor::visit(Submesh& sub) {
    visit(sub.name);
    sub.indexStart = reader.read<u32>();
    sub.indexCount = reader.read<u32>();
    sub.vertexStart = reader.read<u32>();
    sub.vertexCount = reader.read<u32>();
    sub.materialIndex = reader.read<u32>();
    sub.selectionGroup = reader.read<u16>();
    sub.selectionFlags = reader.read<u16>();
    sub.centerPosition = reader.read<Vector3f>();
    sub.sortCenterPosition = reader.read<Vector3f>();
    sub.sortRadius = reader.read<f32>();
    sub.centerBoneIndex = reader.read<u16>();
    sub.maxBoneInfluences = reader.read<u16>();
    sub.rootBone = reader.read<u16>();
    sub.bounds = reader.read<Extent>();
    sub.flags = static_cast<SubmeshFlags>(reader.read<u32>());
}

} // namespace wem
} // namespace models
} // namespace whiteout

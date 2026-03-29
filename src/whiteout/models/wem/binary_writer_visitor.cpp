// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "binary_writer_visitor.h"

#include <type_traits>

namespace whiteout {
namespace models {
namespace wem {

using common::BinaryWriter;

// ============================================================================
// Constructor
// ============================================================================

BinaryWriterVisitor::BinaryWriterVisitor(BinaryWriter& writer) : writer(writer) {}

// ============================================================================
// Entry point
// ============================================================================

void BinaryWriterVisitor::write(const Model& model) {
    indexTable.clear();
    deferredWrites.clear();
    currentLevelWrites.clear();

    // Write header placeholder (32 bytes)
    WEMHeader header{};
    header.magic = kWoemMagic;
    header.version = kCurrentVersion;
    writer.write(header);

    // Reserve index slot 0 for header
    indexTable.emplace_back(IndexEntry{kWoemMagic, 0, 1, 0});

    // Reserve index slot 1 for model root
    u32 modelRefIdx = static_cast<u32>(indexTable.size());
    indexTable.emplace_back(IndexEntry{chunkTag<Model>, 0, 1, ChunkTagTraits<Model>::max_version});

    header.modelRef.entries = 1;
    header.modelRef.index = modelRefIdx;

    // Enqueue model root write
    deferredWrites.push_back([this, &model, modelRefIdx]() {
        indexTable[modelRefIdx].offset = writer.getPosition();
        visit(model);
        writer.AlignTo(16, 0xAA);
        transferDeferredWrites();
    });

    // Process all deferred writes
    while (!deferredWrites.empty()) {
        auto fn = std::move(deferredWrites.front());
        deferredWrites.pop_front();
        fn();
    }

    // Write index table at end
    const u32 indexOffset = writer.getPosition();
    writer.write(indexTable);

    // Backpatch header
    header.indexOffset = indexOffset;
    header.indexCount = static_cast<u32>(indexTable.size());
    writer.setPosition(0);
    writer.write(header);

    // Seek to end
    writer.setPosition(indexOffset + static_cast<u32>(indexTable.size()) * sizeof(IndexEntry));
}

// ============================================================================
// Deferred write pipeline
// ============================================================================

void BinaryWriterVisitor::transferDeferredWrites() {
    while (!currentLevelWrites.empty()) {
        deferredWrites.push_front(std::move(currentLevelWrites.back()));
        currentLevelWrites.pop_back();
    }
}

// ============================================================================
// Container visitors (templates)
// ============================================================================

template <typename T>
void BinaryWriterVisitor::visit(const std::vector<T>& container) {
    Reference ref{};
    if (container.empty()) {
        writer.write(ref);
        return;
    }

    auto refPos = writer.getPosition();
    writer.write(ref); // placeholder

    currentLevelWrites.push_back([this, refPos, &container]() {
        auto entryIdx = static_cast<u32>(indexTable.size());
        auto offset = writer.getPosition();
        indexTable.emplace_back(IndexEntry{ChunkTagTraits<T>::value, offset,
                                           static_cast<u32>(container.size()),
                                           ChunkTagTraits<T>::max_version});

        // Backpatch reference
        writer.setPosition(refPos);
        Reference ref{};
        ref.entries = static_cast<u32>(container.size());
        ref.index = entryIdx;
        writer.write(ref);
        writer.setPosition(offset);

        if constexpr (ChunkTagTraits<T>::is_trivial) {
            writer.write(container);
        } else {
            for (const auto& element : container) {
                visit(element);
                transferDeferredWrites();
            }
        }
        writer.AlignTo(16, 0xAA);
    });
}

void BinaryWriterVisitor::visit(const std::string& str) {
    Reference ref{};
    if (str.empty()) {
        writer.write(ref);
        return;
    }

    auto refPos = writer.getPosition();
    writer.write(ref); // placeholder

    currentLevelWrites.push_back([this, refPos, &str]() {
        auto entryIdx = static_cast<u32>(indexTable.size());
        auto offset = writer.getPosition();
        indexTable.emplace_back(IndexEntry{ChunkTagTraits<char>::value, offset,
                                           static_cast<u32>(str.size()),
                                           ChunkTagTraits<char>::max_version});

        // Backpatch reference
        writer.setPosition(refPos);
        Reference ref{};
        ref.entries = static_cast<u32>(str.size());
        ref.index = entryIdx;
        writer.write(ref);
        writer.setPosition(offset);

        writer.writeString(str);
        writer.AlignTo(16, 0xAA);
    });
}

// ============================================================================
// Inline helpers
// ============================================================================

void BinaryWriterVisitor::visitFresnel(const FresnelProperties& f) {
    writer.write<u32>(static_cast<u32>(f.mode));
    writer.write(f.exponent);
    writer.write(f.min);
    writer.write(f.max);
    writer.write(f.translation);
    writer.write(f.mask);
    writer.write(f.rotation);
    writer.write(f.color);
    writer.write(f.opacity);
    writer.write(f.teamColor);
}

// ============================================================================
// Structure visitors
// ============================================================================

void BinaryWriterVisitor::visit(const Model& model) {
    visit(model.name);
    writer.write(model.bounds);
    visit(model.meshes);
    visit(model.materials);
    visit(model.textures);
}

void BinaryWriterVisitor::visit(const TextureRef& tex) {
    visit(tex.path);
    writer.write(tex.flags);
    writer.write(tex.replaceableId);
    writer.write(tex.type);
}

void BinaryWriterVisitor::visit(const Material& mat) {
    writer.write<u32>(static_cast<u32>(mat.type));
    visit(mat.name);
    writer.write(mat.priorityPlane);

    if (mat.type == MaterialType::Standard) {
        writer.write<u32>(static_cast<u32>(mat.blendMode));
        writer.write<u32>(static_cast<u32>(mat.flags));
        visit(mat.shader);
        writer.write(mat.shaderId);
        writer.write(mat.specularExponent);
        writer.write(mat.alphaTestThreshold);
        writer.write(mat.depthBlendFalloff);
        writer.write(mat.emissiveGain);
        writer.write(mat.hdrSpecularMultiplier);
        writer.write(mat.hdrEmissiveMultiplier);
        writer.write(mat.hdrEnvironmentConstant);
        writer.write(mat.hdrEnvironmentDiffuse);
        writer.write(mat.hdrEnvironmentSpecular);
        visitFresnel(mat.fresnel);
        writer.write<u32>(static_cast<u32>(mat.materialClass));
        writer.write<u32>(static_cast<u32>(mat.layerBlendMode));
        writer.write<u32>(static_cast<u32>(mat.emissiveBlendMode1));
        writer.write<u32>(static_cast<u32>(mat.emissiveBlendMode2));
        writer.write<u32>(static_cast<u32>(mat.specularMode));
        visit(mat.textureSlots);
    } else if (mat.type == MaterialType::Composite) {
        visit(mat.sections);
    }
}

void BinaryWriterVisitor::visit(const TextureSlot& slot) {
    writer.write<u32>(static_cast<u32>(slot.semantic));
    writer.write(slot.textureIndex);
    writer.write(slot.uvSetIndex);
    writer.write<u32>(static_cast<u32>(slot.uvMapping));
    writer.write<u8>(slot.wrapU ? 1 : 0);
    writer.write<u8>(slot.wrapV ? 1 : 0);
    writer.write(slot.alpha);
    writer.write<u32>(static_cast<u32>(slot.colorChannelSelect));
    writer.write(slot.rgbMultiply);
    writer.write(slot.rgbAdd);
    writer.write(slot.flipbookRows);
    writer.write(slot.flipbookColumns);
    visitFresnel(slot.fresnel);
}

void BinaryWriterVisitor::visit(const CompositeSection& sec) {
    writer.write(sec.materialIndex);
    writer.write(sec.blendWeight);
    writer.write<u32>(static_cast<u32>(sec.blendMode));
}

void BinaryWriterVisitor::visit(const Mesh& mesh) {
    visit(mesh.name);
    writer.write(mesh.lodLevel);
    writer.write(mesh.bounds);
    visit(mesh.positions);
    visit(mesh.normals);
    visit(mesh.tangents);
    visit(mesh.boneIndices);
    visit(mesh.boneWeights);

    // UV sets: inline count followed by individual VEC2 references
    writer.write<u32>(static_cast<u32>(mesh.uvSets.size()));
    for (const auto& uvSet : mesh.uvSets) {
        visit(uvSet);
    }

    visit(mesh.vertexColors);
    visit(mesh.indices);
    visit(mesh.submeshes);
}

void BinaryWriterVisitor::visit(const Submesh& sub) {
    visit(sub.name);
    writer.write(sub.indexStart);
    writer.write(sub.indexCount);
    writer.write(sub.vertexStart);
    writer.write(sub.vertexCount);
    writer.write(sub.materialIndex);
    writer.write(sub.selectionGroup);
    writer.write(sub.selectionFlags);
    writer.write(sub.centerPosition);
    writer.write(sub.sortCenterPosition);
    writer.write(sub.sortRadius);
    writer.write(sub.centerBoneIndex);
    writer.write(sub.maxBoneInfluences);
    writer.write(sub.rootBone);
    writer.write(sub.bounds);
    writer.write<u32>(static_cast<u32>(sub.flags));
}

} // namespace wem
} // namespace models
} // namespace whiteout

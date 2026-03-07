// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

void BinaryParseVisitor::visit(SkinSection& section) {
    section.skinSectionId = reader.read<u16>();
    section.level = reader.read<u16>();
    section.vertexStart = reader.read<u16>();
    section.vertexCount = reader.read<u16>();
    section.indexStart = reader.read<u16>();
    section.indexCount = reader.read<u16>();
    section.boneCount = reader.read<u16>();
    section.boneComboIndex = reader.read<u16>();
    section.boneInfluences = reader.read<u16>();
    section.centerBoneIndex = reader.read<u16>();
    section.centerPosition = reader.read<Vector3f>();
    section.sortCenterPosition = reader.read<Vector3f>();
    section.sortRadius = reader.read<f32>();
}

void BinaryParseVisitor::visit(Batch& batch) {
    batch.flags = reader.read<u8>();
    batch.priorityPlane = reader.read<i8>();
    batch.shaderId = reader.read<u16>();
    batch.skinSectionIndex = reader.read<u16>();
    batch.geosetIndex = reader.read<u16>();
    batch.colorIndex = reader.read<i16>();
    batch.materialIndex = reader.read<u16>();
    batch.materialLayer = reader.read<u16>();
    batch.textureCount = reader.read<u16>();
    batch.textureComboIndex = reader.read<u16>();
    batch.textureCoordComboIndex = reader.read<u16>();
    batch.textureWeightComboIndex = reader.read<u16>();
    batch.textureTransformComboIndex = reader.read<u16>();
}

void BinaryParseVisitor::visit(ShadowBatch& batch) {
    batch.flags = reader.read<u8>();
    batch.flags2 = reader.read<u8>();
    batch.unknown0 = reader.read<u16>();
    batch.submeshId = reader.read<u16>();
    batch.textureId = reader.read<u16>();
    batch.colorId = reader.read<u16>();
    batch.transparencyId = reader.read<u16>();
}

void BinaryParseVisitor::visit(SkinProfile& profile) {
    profile.magic = reader.read<u32>();

    visit(profile.vertices);
    visit(profile.indices);
    visit(profile.bones);
    visit(profile.submeshes);
    visit(profile.batches);

    profile.lodVertexBase = reader.read<u32>();

    if (version >= M2_VERSION_CATA) {
        visit(profile.shadowBatches);
    } else {
        profile.shadowBatches.clear();
    }
}

void BinaryParseVisitor::visit(SkinFile& file) {
    visit(file.profile);
}

} // namespace m2
} // namespace whiteout

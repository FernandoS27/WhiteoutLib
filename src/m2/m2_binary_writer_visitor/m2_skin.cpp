#include "../m2_binary_writer_visitor.h"
#include "../../common/binary_writer.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

void M2BinaryWriterVisitor::visit(const M2SkinSection& section) {
    writer.write(section.skinSectionId);
    writer.write(section.level);
    writer.write(section.vertexStart);
    writer.write(section.vertexCount);
    writer.write(section.indexStart);
    writer.write(section.indexCount);
    writer.write(section.boneCount);
    writer.write(section.boneComboIndex);
    writer.write(section.boneInfluences);
    writer.write(section.centerBoneIndex);
    writer.write(section.centerPosition);
    writer.write(section.sortCenterPosition);
    writer.write(section.sortRadius);
}

void M2BinaryWriterVisitor::visit(const M2Batch& batch) {
    writer.write(batch.flags);
    writer.write(batch.priorityPlane);
    writer.write(batch.shaderId);
    writer.write(batch.skinSectionIndex);
    writer.write(batch.geosetIndex);
    writer.write(batch.colorIndex);
    writer.write(batch.materialIndex);
    writer.write(batch.materialLayer);
    writer.write(batch.textureCount);
    writer.write(batch.textureComboIndex);
    writer.write(batch.textureCoordComboIndex);
    writer.write(batch.textureWeightComboIndex);
    writer.write(batch.textureTransformComboIndex);
}

void M2BinaryWriterVisitor::visit(const M2ShadowBatch& batch) {
    writer.write(batch.flags);
    writer.write(batch.flags2);
    writer.write(batch.unknown0);
    writer.write(batch.submeshId);
    writer.write(batch.textureId);
    writer.write(batch.colorId);
    writer.write(batch.transparencyId);
}

void M2BinaryWriterVisitor::visit(const M2SkinProfile& profile) {
    writer.write(profile.magic);

    visit(profile.vertices);
    visit(profile.indices);
    visit(profile.bones);
    visit(profile.submeshes);
    visit(profile.batches);

    writer.write(profile.boneCountMax);
    writer.write(profile.lodVertexBase);

    if (version >= M2_VERSION_CATA) {
        visit(profile.shadowBatches);
    }
}

void M2BinaryWriterVisitor::visit(const M2SkinFile& file) {
    visit(file.profile);
}

} // namespace m2
} // namespace whiteout

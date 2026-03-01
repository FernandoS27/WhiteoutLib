#include "../m2_binary_writer_visitor.h"
#include "../include/common/binary_writer.h"

namespace m2 {

using common::BinaryWriter;

// ============================================================================
// BONE Header
// ============================================================================

void M2BinaryWriterVisitor::visit(const BONEHeader& header) {
    writer.write(header.unk);
}

// ============================================================================
// BIDA - Bone IDs
// ============================================================================

void M2BinaryWriterVisitor::visit(const BIDAChunk& chunk) {
    writer.write(chunk.boneIds);
}

// ============================================================================
// BOMT - Bone Offset Matrices
// ============================================================================

void M2BinaryWriterVisitor::visit(const Matrix4x4& matrix) {
    writer.write(matrix.data);
}

void M2BinaryWriterVisitor::visit(const BOMTChunk& chunk) {
    writer.write(chunk.boneOffsetMatrices);
}

} // namespace m2

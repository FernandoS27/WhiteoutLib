// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

// ============================================================================
// BONE Header
// ============================================================================

void BinaryWriterVisitor::visit(const BONEHeader& header) {
    writer.write(header.unk);
}

// ============================================================================
// BIDA - Bone IDs
// ============================================================================

void BinaryWriterVisitor::visit(const BIDAChunk& chunk) {
    writer.write(chunk.boneIds);
}

// ============================================================================
// BOMT - Bone Offset Matrices
// ============================================================================

void BinaryWriterVisitor::visit(const Matrix44f& matrix) {
    writer.write(matrix.data);
}

void BinaryWriterVisitor::visit(const BOMTChunk& chunk) {
    writer.write(chunk.boneOffsetMatrices);
}

} // namespace m2
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

// ============================================================================
// BONE Header
// ============================================================================

void BinaryParseVisitor::visit(BONEHeader& header) {
    header.unk = reader.read<u32>();
}

// ============================================================================
// BIDA - Bone IDs
// ============================================================================

void BinaryParseVisitor::visit(BIDAChunk& chunk) {
    parse_chunked_vector(chunk.boneIds);
}

// ============================================================================
// BOMT - Bone Offset Matrices
// ============================================================================

void BinaryParseVisitor::visit(Matrix44f& matrix) {
    for (auto& row : matrix.data) {
        row = reader.readArray<f32, 4>();
    }
}

void BinaryParseVisitor::visit(BOMTChunk& chunk) {
    parse_chunked_vector(chunk.boneOffsetMatrices);
}

} // namespace m2
} // namespace whiteout

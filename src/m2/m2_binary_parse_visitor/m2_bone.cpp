#include "../m2_binary_parse_visitor.h"
#include "../../common/binary_reader.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

// ============================================================================
// BONE Header
// ============================================================================

void M2BinaryParseVisitor::visit(BONEHeader& header) {
    header.unk = reader.read<u32>();
}

// ============================================================================
// BIDA - Bone IDs
// ============================================================================

void M2BinaryParseVisitor::visit(BIDAChunk& chunk) {
    parse_chunked_vector(chunk.boneIds);
}

// ============================================================================
// BOMT - Bone Offset Matrices
// ============================================================================

void M2BinaryParseVisitor::visit(Matrix4x4& matrix) {
    matrix.data = reader.readArray<f32, 16>();
}

void M2BinaryParseVisitor::visit(BOMTChunk& chunk) {
    parse_chunked_vector(chunk.boneOffsetMatrices);
}

} // namespace m2
} // namespace whiteout

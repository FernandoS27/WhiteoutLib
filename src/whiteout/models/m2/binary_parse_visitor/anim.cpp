// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

// ============================================================================
// AFM2 - Animation Data (Legion 24500+)
// ============================================================================

void BinaryParseVisitor::visit(AFM2Chunk& chunk) {
    parse_chunked_vector(chunk.animationData);
}

// ============================================================================
// AFSA - Attachment Skeleton Data (Legion 24500+)
// ============================================================================

void BinaryParseVisitor::visit(AFSAChunk& chunk) {
    parse_chunked_vector(chunk.attachmentData);
}

// ============================================================================
// AFSB - Bone Skeleton Data (Legion 24500+)
// ============================================================================

void BinaryParseVisitor::visit(AFSBChunk& chunk) {
    parse_chunked_vector(chunk.boneData);
}

} // namespace m2
} // namespace whiteout

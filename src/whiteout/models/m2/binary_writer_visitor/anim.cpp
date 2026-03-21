// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

// ============================================================================
// AFM2 - Animation Data (Legion 24500+)
// ============================================================================

void BinaryWriterVisitor::visit(const AFM2Chunk& chunk) {
    writer.write(chunk.animationData);
}

// ============================================================================
// AFSA - Attachment Skeleton Data (Legion 24500+)
// ============================================================================

void BinaryWriterVisitor::visit(const AFSAChunk& chunk) {
    writer.write(chunk.attachmentData);
}

// ============================================================================
// AFSB - Bone Skeleton Data (Legion 24500+)
// ============================================================================

void BinaryWriterVisitor::visit(const AFSBChunk& chunk) {
    writer.write(chunk.boneData);
}

} // namespace m2
} // namespace whiteout

#include "../m2_binary_writer_visitor.h"
#include "../include/common/binary_writer.h"

namespace m2 {

using common::BinaryWriter;

// ============================================================================
// AFM2 - Animation Data (Legion 24500+)
// ============================================================================

void M2BinaryWriterVisitor::visit(const AFM2Chunk& chunk) {
    writer.write(chunk.animationData);
}

// ============================================================================
// AFSA - Attachment Skeleton Data (Legion 24500+)
// ============================================================================

void M2BinaryWriterVisitor::visit(const AFSAChunk& chunk) {
    writer.write(chunk.attachmentData);
}

// ============================================================================
// AFSB - Bone Skeleton Data (Legion 24500+)
// ============================================================================

void M2BinaryWriterVisitor::visit(const AFSBChunk& chunk) {
    writer.write(chunk.boneData);
}

} // namespace m2

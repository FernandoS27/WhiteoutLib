
#include "../../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

void BinaryWriterVisitor::visit(const AFM2Chunk& chunk) {
    writer.write(chunk.animationData);
}

void BinaryWriterVisitor::visit(const AFSAChunk& chunk) {
    writer.write(chunk.attachmentData);
}

void BinaryWriterVisitor::visit(const AFSBChunk& chunk) {
    writer.write(chunk.boneData);
}

} // namespace m2
} // namespace whiteout

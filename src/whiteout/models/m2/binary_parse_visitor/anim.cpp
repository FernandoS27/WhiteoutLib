
#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

void BinaryParseVisitor::visit(AFM2Chunk& chunk) {
    parse_chunked_vector(chunk.animationData);
}

void BinaryParseVisitor::visit(AFSAChunk& chunk) {
    parse_chunked_vector(chunk.attachmentData);
}

void BinaryParseVisitor::visit(AFSBChunk& chunk) {
    parse_chunked_vector(chunk.boneData);
}

} // namespace m2
} // namespace whiteout

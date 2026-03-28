
#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

void BinaryParseVisitor::visit(BONEHeader& header) {
    header.unk = reader.read<u32>();
}

void BinaryParseVisitor::visit(BIDAChunk& chunk) {
    parse_chunked_vector(chunk.boneIds);
}

void BinaryParseVisitor::visit(Matrix44f& matrix) {
    for (auto& row : matrix.data) {
        row = reader.readArray<f32, 4>();
    }
}

void BinaryParseVisitor::visit(BOMTChunk& chunk) {
    parse_chunked_vector(chunk.boneOffsetMatrices);
}

}
}

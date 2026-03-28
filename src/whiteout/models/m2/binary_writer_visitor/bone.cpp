
#include "../../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

void BinaryWriterVisitor::visit(const BONEHeader& header) {
    writer.write(header.unk);
}

void BinaryWriterVisitor::visit(const BIDAChunk& chunk) {
    writer.write(chunk.boneIds);
}

void BinaryWriterVisitor::visit(const Matrix44f& matrix) {
    writer.write(matrix.data);
}

void BinaryWriterVisitor::visit(const BOMTChunk& chunk) {
    writer.write(chunk.boneOffsetMatrices);
}

}
}

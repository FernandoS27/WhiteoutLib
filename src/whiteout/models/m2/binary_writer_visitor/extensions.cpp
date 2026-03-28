
#include "../../../common/binary_writer.h"
#include "../binary_writer_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryWriter;

void BinaryWriterVisitor::visit(const LodProfile& chunk) {
    writer.write(chunk.flags);
    writer.write(chunk.numLodLevels);
    writer.write(chunk.lodDistance);
    writer.write(chunk.particleBoneLod);
    writer.write(chunk.reserved0);
    writer.write(chunk.lodFlags);
    writer.write(chunk.lodBatchCount);
    writer.write(chunk.reserved1);
}

}
}

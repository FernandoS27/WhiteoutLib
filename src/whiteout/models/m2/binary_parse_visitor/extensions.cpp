
#include "../../../common/binary_reader.h"
#include "../binary_parse_visitor.h"

namespace whiteout {
namespace m2 {

using common::BinaryReader;

void BinaryParseVisitor::visit(ParticleEmitterExtension& chunk) {
    chunk.zSource = reader.read<f32>();
    chunk.colorMult = reader.read<f32>();
    chunk.alphaMult = reader.read<f32>();
    visit(chunk.alphaCutoff);
}

void BinaryParseVisitor::visit(LodProfile& chunk) {
    chunk.flags = reader.read<u16>();
    chunk.numLodLevels = reader.read<u16>();
    chunk.lodDistance = reader.read<f32>();
    chunk.particleBoneLod = reader.readArray<u8, 4>();
    chunk.reserved0 = reader.read<u8>();
    chunk.lodFlags = reader.read<u8>();
    chunk.lodBatchCount = reader.read<u8>();
    chunk.reserved1 = reader.read<u8>();
}

} // namespace m2
} // namespace whiteout

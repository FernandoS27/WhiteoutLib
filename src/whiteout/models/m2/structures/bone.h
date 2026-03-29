
#pragma once

#include <vector>
#include <whiteout/models/m2/structures/base.h>

namespace whiteout {
namespace m2 {

constexpr u32 BIDA_TAG = makeTag("BIDA");
constexpr u32 BOMT_TAG = makeTag("BOMT");

struct BONEHeader {
    u32 unk = 1;
};

struct BIDAChunk {
    std::vector<u16> boneIds;
};

struct BOMTChunk {
    std::vector<Matrix44f> boneOffsetMatrices;
};

} // namespace m2
} // namespace whiteout

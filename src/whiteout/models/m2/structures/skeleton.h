
#pragma once

#include <whiteout/models/m2/structures/base.h>

namespace whiteout {
namespace m2 {

constexpr u32 SKL1_TAG = makeTag("SKL1");
constexpr u32 SKA1_TAG = makeTag("SKA1");
constexpr u32 SKB1_TAG = makeTag("SKB1");
constexpr u32 SKS1_TAG = makeTag("SKS1");
constexpr u32 SKPD_TAG = makeTag("SKPD");

struct SKL1Chunk {
    u32 flags = 0x100;
    std::string name;
    std::array<u8, 4> reserved = {};
};

struct SKA1Chunk {
    std::vector<Attachment> attachments;
    std::vector<u16> attachmentLookupTable;
};

struct SKB1Chunk {
    std::vector<Bone> bones;
    std::vector<u16> keyBoneLookup;
};

struct SKS1Chunk {
    std::vector<GlobalSequence> globalLoops;
    std::vector<Sequence> sequences;
    std::vector<u16> sequenceLookups;
    std::array<u8, 8> reserved = {};
};

struct SKPDChunk {
    std::array<u8, 8> reserved0 = {};
    u32 parentSkeletonFileId = 0;
    std::array<u8, 4> reserved1 = {};
};

} // namespace m2
} // namespace whiteout

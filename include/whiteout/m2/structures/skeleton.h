// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "base.h"

namespace whiteout {
namespace m2 {

// Skeleton file tags
constexpr u32 SKL1_TAG = makeTag("SKL1");
constexpr u32 SKA1_TAG = makeTag("SKA1");
constexpr u32 SKB1_TAG = makeTag("SKB1");
constexpr u32 SKS1_TAG = makeTag("SKS1");
constexpr u32 SKPD_TAG = makeTag("SKPD");

// SKL1: Skeleton label/header chunk
struct SKL1Chunk {
    u32 flags = 0x100;               // Always 0x100 in Legion (7.3.2.25079)
    std::string name;                // Skeleton name
    std::array<u8, 4> reserved = {};  // Reserved, always 0 in Legion
};

// SKA1: Skeleton attachments chunk
struct SKA1Chunk {
    std::vector<Attachment> attachments;
    std::vector<u16> attachmentLookupTable;
};

// SKB1: Skeleton bones chunk
struct SKB1Chunk {
    std::vector<Bone> bones;
    std::vector<u16> keyBoneLookup;
};

// SKS1: Skeleton sequences chunk
struct SKS1Chunk {
    std::vector<GlobalSequence> globalLoops;
    std::vector<Sequence> sequences;
    std::vector<u16> sequenceLookups;
    std::array<u8, 8> reserved = {};  // Reserved, always 0 in Legion
};

// SKPD: Skeleton parent data
struct SKPDChunk {
    std::array<u8, 8> reserved0 = {};   // Always 0 in Legion (7.3.2.25079)
    u32 parentSkeletonFileId = 0;       // File ID of parent skeleton (for de-duplication)
    std::array<u8, 4> reserved1 = {};   // Always 0 in Legion (7.3.2.25079)
};

} // namespace m2
} // namespace whiteout

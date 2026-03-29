
#pragma once

#include <vector>
#include <whiteout/compatibility.h>
#include <whiteout/models/m2/structures/base.h>

namespace whiteout {
namespace m2 {

constexpr u32 AFM2_TAG = makeTag("AFM2");
constexpr u32 AFSA_TAG = makeTag("AFSA");
constexpr u32 AFSB_TAG = makeTag("AFSB");

struct AFM2Chunk {
    std::vector<u8> animationData;
};

struct AFSAChunk {
    std::vector<u8> attachmentData;
};

struct AFSBChunk {
    std::vector<u8> boneData;
};

struct AnimProfile {
    std::optional<AFM2Chunk> afm2_chunk = std::nullopt;
    std::optional<AFSAChunk> afsa_chunk = std::nullopt;
    std::optional<AFSBChunk> afsb_chunk = std::nullopt;

    bool isChunked = false;
};

} // namespace m2
} // namespace whiteout

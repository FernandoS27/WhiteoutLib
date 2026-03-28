
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/models/m2/parser.h>
#include "internal_structures.h"

#include <string>
#include <vector>

namespace whiteout {

namespace common {
class BinaryReader;
}

namespace m2 {

class WoWFileSystem;

class ChunkParser {
public:
    using ParseMode = Parser::ParseMode;

    explicit ChunkParser(ParseMode mode);

    void parseChunkedBase(common::BinaryReader& reader, BaseFile& m2file, WoWFileSystem* wfs);
    void parseChunkedSkeleton(common::BinaryReader& reader, SkeletonFile& skeletonFile, WoWFileSystem* wfs);
    void parseChunkedBone(common::BinaryReader& reader, BoneFile& boneFile, WoWFileSystem* wfs);
    void parseChunkedAnim(common::BinaryReader& reader, AnimFile& animFile, WoWFileSystem* wfs, bool isChunked = false);

    void reportIssue(const std::string& message);

    bool hasIssues() const { return !issues.empty(); }
    const std::vector<std::string>& getIssues() const { return issues; }

    void drainIssues(std::vector<std::string>& target);

private:
    struct ChunkEntry {
        u32 tag;
        u32 size;
        u32 dataOffset;
    };

    std::vector<ChunkEntry> collectChunks(common::BinaryReader& reader);
    void skipUnknownChunk(common::BinaryReader& reader, u32 tag, u32 size);

    ParseMode parseMode;
    std::vector<std::string> issues;
};

}
}

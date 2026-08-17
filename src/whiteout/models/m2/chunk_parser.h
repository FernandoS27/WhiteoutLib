
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
    ChunkParser();

    void parseChunkedBase(common::BinaryReader& reader, BaseFile& m2file, WoWFileSystem* wfs);
    void parseChunkedSkeleton(common::BinaryReader& reader, SkeletonFile& skeletonFile,
                              WoWFileSystem* wfs);
    /// @brief Read a whole `.bone` payload. False when the two chunks
    ///        disagree on how many entries they hold.
    bool parseBoneOverrides(common::BinaryReader& reader, BoneOverrideSet& overrides);
    void parseChunkedAnim(common::BinaryReader& reader, AnimFile& animFile, WoWFileSystem* wfs,
                          bool isChunked = false);

    /// @brief Read a whole `.phys` payload — a standalone file or an M2's
    ///        inline PFDC chunk, which are the same bytes.
    void parsePhysics(common::BinaryReader& reader, PhysicsData& physics);

    void reportIssue(const std::string& message);

    bool hasIssues() const {
        return !issues.empty();
    }
    const std::vector<std::string>& getIssues() const {
        return issues;
    }

    void drainIssues(std::vector<std::string>& target);

private:
    struct ChunkEntry {
        u32 tag;
        u32 size;
        u32 dataOffset;
    };

    std::vector<ChunkEntry> collectChunks(common::BinaryReader& reader);
    std::vector<ChunkEntry> collectPhysChunks(common::BinaryReader& reader);
    void skipUnknownChunk(common::BinaryReader& reader, u32 tag, u32 size);

    std::vector<std::string> issues;
};

} // namespace m2
} // namespace whiteout

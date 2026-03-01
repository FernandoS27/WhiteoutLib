#pragma once

#include "structures.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <span>


namespace common {
    class BinaryReader;
}

namespace m2 {

enum class ParseMode {
    Strict,
    Lenient,
};

class M2Parser {
public:
    explicit M2Parser(ParseMode mode = ParseMode::Lenient);
    
    M2FileSystem parse(const std::string& filePath);
    
    void parse(std::span<const uint8_t> buffer, M2FileSystem& fileSystem, M2FileType fileType);
    
    const std::vector<std::string>& getIssues() const { return issues; }
    
    void clearIssues() { issues.clear(); }
    
private:
    friend class common::BinaryReader;
    
    ParseMode parseMode;
    std::vector<std::string> issues;

    void parseM2Base(common::BinaryReader& reader, M2BaseFile& file);
    void parseChunkedM2Base(common::BinaryReader& reader, M2BaseFile& m2file);

    void parseM2Skin(common::BinaryReader& reader, M2SkinFile& skinFile);
    
    void parseChunkedM2Skeleton(common::BinaryReader& reader, M2SkeletonFile& skeletonFile);
    void parseChunkedM2Bone(common::BinaryReader& reader, M2BoneFile& boneFile);
    void parseChunkedM2Anim(common::BinaryReader& reader, M2AnimFile& animFile);

    void reportIssue(const std::string& message);
    void skipUnknownChunk(common::BinaryReader& reader, uint32_t tag, uint32_t size);
};

} // namespace m2

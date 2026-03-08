// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <stdexcept>
#include <string>
#include <vector>
#include "../compatibility.h"
#include "structures.h"

namespace whiteout {
namespace common {
class BinaryReader;
}

namespace m2 {

enum class ParseMode {
    Strict,
    Lenient,
};

class Parser {
public:
    explicit Parser(ParseMode mode = ParseMode::Lenient);

    FileSystem parse(const std::string& filePath);

    void parse(std::span<const uint8_t> buffer, FileSystem& fileSystem, FileType fileType);

    const std::vector<std::string>& getIssues() const {
        return issues;
    }

    void clearIssues() {
        issues.clear();
    }

private:
    friend class common::BinaryReader;

    ParseMode parseMode;
    std::vector<std::string> issues;

    void parseBase(common::BinaryReader& reader, BaseFile& file);
    void parseChunkedBase(common::BinaryReader& reader, BaseFile& m2file);

    void parseSkin(common::BinaryReader& reader, SkinFile& skinFile);

    void parseChunkedSkeleton(common::BinaryReader& reader, SkeletonFile& skeletonFile);
    void parseChunkedBone(common::BinaryReader& reader, BoneFile& boneFile);
    void parseChunkedAnim(common::BinaryReader& reader, AnimFile& animFile);

    void reportIssue(const std::string& message);
    void skipUnknownChunk(common::BinaryReader& reader, uint32_t tag, uint32_t size);
};

} // namespace m2
} // namespace whiteout

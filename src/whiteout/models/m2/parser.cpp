// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/m2/parser.h>
#include "../../common/binary_reader.h"
#include "../../common/streams.h"
#include "binary_parse_visitor.h"
#include "chunk_parser.h"
#include "wow_file_system.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace whiteout {
namespace m2 {

using common::BinaryReader;

// ============================================================================
// ParserImpl - Implementation class using PImpl idiom
// ============================================================================

class Parser::Impl {
public:
    ParseMode parseMode;
    std::vector<std::string> issues;
    ChunkParser chunkParser;

    explicit Impl(ParseMode mode) : parseMode(mode), chunkParser(mode) {}

    void parseBase(BinaryReader& reader, BaseFile& file, WoWFileSystem* wfs);
    void parseSkin(BinaryReader& reader, SkinFile& skinFile, WoWFileSystem* wfs);

    void reportIssue(const std::string& message);
};

// ============================================================================
// Parser Public Interface (using PImpl)
// ============================================================================

Parser::Parser(ParseMode mode) : pImpl(std::make_unique<Impl>(mode)) {
}

Parser::~Parser() = default;

FileSystem Parser::parse(interfaces::VirtualPathFileSystem& fs, const std::string& filePath) {
    pImpl->issues.clear();

    WoWFileSystem wfs(fs, filePath);
    FileSystem fileSystem;
    wfs.exploratorySearch();

    // Parse base .m2
    {
        auto m2Data = wfs.getM2Base();
        common::span_streambuf sbuf(m2Data);
        std::istream in(&sbuf);
        BinaryReader reader(in);
        pImpl->parseBase(reader, fileSystem.base, &wfs);

        // Extract baseName from path
        std::filesystem::path p(filePath);
        fileSystem.baseName = p.stem().string();
    }

    // Feed chunk metadata to WoWFileSystem so it can resolve satellite files
    if (fileSystem.base.sfid_chunk) {
        wfs.setSkinChunk(*fileSystem.base.sfid_chunk);
    }
    if (fileSystem.base.afid_chunk) {
        wfs.setAnimChunk(*fileSystem.base.afid_chunk);
    }
    if (fileSystem.base.skid_chunk) {
        wfs.setSkeletonChunk(*fileSystem.base.skid_chunk);
    }

    // Parse skin files
    if (fileSystem.base.sfid_chunk) {
        const auto& sfid = *fileSystem.base.sfid_chunk;
        for (u32 i = 0; i < static_cast<u32>(sfid.skinFileDataIds.size()); ++i) {
            auto skinData = wfs.getSkin(i, false);
            if (skinData.empty()) continue;
            try {
                common::span_streambuf sbuf(skinData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                SkinFile skinFile;
                pImpl->parseSkin(reader, skinFile, &wfs);
                skinFile.isLodSkin = false;
                skinFile.index = static_cast<int>(i);
                fileSystem.skins.push_back(std::move(skinFile));
            } catch (const std::exception& e) {
                pImpl->reportIssue(std::string("Error parsing skin ") + std::to_string(i) + ": " + e.what());
            }
        }
        for (u32 i = 0; i < static_cast<u32>(sfid.lodSkinFileDataIds.size()); ++i) {
            auto skinData = wfs.getSkin(i, true);
            if (skinData.empty()) continue;
            try {
                common::span_streambuf sbuf(skinData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                SkinFile skinFile;
                pImpl->parseSkin(reader, skinFile, &wfs);
                skinFile.isLodSkin = true;
                skinFile.lodLevel = static_cast<int>(i);
                fileSystem.skins.push_back(std::move(skinFile));
            } catch (const std::exception& e) {
                pImpl->reportIssue(std::string("Error parsing LOD skin ") + std::to_string(i) + ": " + e.what());
            }
        }
    }

    // Parse skeleton
    {
        auto skelData = wfs.getSkeleton();
        if (!skelData.empty()) {
            try {
                common::span_streambuf sbuf(skelData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                fileSystem.skeleton.emplace();
                pImpl->chunkParser.parseChunkedSkeleton(reader, fileSystem.skeleton.value(), &wfs);
            } catch (const std::exception& e) {
                pImpl->reportIssue(std::string("Error parsing skeleton: ") + e.what());
                fileSystem.skeleton.reset();
            }
        }
    }

    // Parse anim files
    if (fileSystem.base.afid_chunk) {
        for (const auto& entry : fileSystem.base.afid_chunk->animFileIds) {
            auto animData = wfs.getAnimBuffer(entry.animId, entry.subAnimId);
            if (animData.empty()) continue;
            try {
                common::span_streambuf sbuf(animData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                AnimFile animFile;
                animFile.animId = entry.animId;
                animFile.variant = entry.subAnimId;
                pImpl->chunkParser.parseChunkedAnim(reader, animFile, &wfs);
                fileSystem.anims.push_back(std::move(animFile));
            } catch (const std::exception& e) {
                pImpl->reportIssue(std::string("Error parsing anim ") +
                                   std::to_string(entry.animId) + "-" +
                                   std::to_string(entry.subAnimId) + ": " + e.what());
            }
        }
    }

    pImpl->chunkParser.drainIssues(pImpl->issues);
    return fileSystem;
}

FileSystem Parser::parse(interfaces::CascFileSystem& cascFs,
                         std::span<const uint8_t> buffer) {
    pImpl->issues.clear();

    WoWFileSystem wfs(cascFs, buffer);
    FileSystem fileSystem;

    // Parse base .m2
    {
        auto m2Data = wfs.getM2Base();
        common::span_streambuf sbuf(m2Data);
        std::istream in(&sbuf);
        BinaryReader reader(in);
        pImpl->parseBase(reader, fileSystem.base, &wfs);
    }

    // Parse skin files
    if (fileSystem.base.sfid_chunk) {
        const auto& sfid = *fileSystem.base.sfid_chunk;
        for (u32 i = 0; i < static_cast<u32>(sfid.skinFileDataIds.size()); ++i) {
            auto skinData = wfs.getSkin(i, false);
            if (skinData.empty()) continue;
            try {
                common::span_streambuf sbuf(skinData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                SkinFile skinFile;
                pImpl->parseSkin(reader, skinFile, &wfs);
                skinFile.isLodSkin = false;
                skinFile.index = static_cast<int>(i);
                fileSystem.skins.push_back(std::move(skinFile));
            } catch (const std::exception& e) {
                pImpl->reportIssue(std::string("Error parsing skin ") + std::to_string(i) + ": " + e.what());
            }
        }
        for (u32 i = 0; i < static_cast<u32>(sfid.lodSkinFileDataIds.size()); ++i) {
            auto skinData = wfs.getSkin(i, true);
            if (skinData.empty()) continue;
            try {
                common::span_streambuf sbuf(skinData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                SkinFile skinFile;
                pImpl->parseSkin(reader, skinFile, &wfs);
                skinFile.isLodSkin = true;
                skinFile.lodLevel = static_cast<int>(i);
                fileSystem.skins.push_back(std::move(skinFile));
            } catch (const std::exception& e) {
                pImpl->reportIssue(std::string("Error parsing LOD skin ") + std::to_string(i) + ": " + e.what());
            }
        }
    }

    // Parse skeleton
    {
        auto skelData = wfs.getSkeleton();
        if (!skelData.empty()) {
            try {
                common::span_streambuf sbuf(skelData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                fileSystem.skeleton.emplace();
                pImpl->chunkParser.parseChunkedSkeleton(reader, fileSystem.skeleton.value(), &wfs);
            } catch (const std::exception& e) {
                pImpl->reportIssue(std::string("Error parsing skeleton: ") + e.what());
                fileSystem.skeleton.reset();
            }
        }
    }

    // Parse anim files
    if (fileSystem.base.afid_chunk) {
        for (const auto& entry : fileSystem.base.afid_chunk->animFileIds) {
            auto animData = wfs.getAnimBuffer(entry.animId, entry.subAnimId);
            if (animData.empty()) continue;
            try {
                common::span_streambuf sbuf(animData);
                std::istream in(&sbuf);
                BinaryReader reader(in);
                AnimFile animFile;
                animFile.animId = entry.animId;
                animFile.variant = entry.subAnimId;
                pImpl->chunkParser.parseChunkedAnim(reader, animFile, &wfs);
                fileSystem.anims.push_back(std::move(animFile));
            } catch (const std::exception& e) {
                pImpl->reportIssue(std::string("Error parsing anim ") +
                                   std::to_string(entry.animId) + "-" +
                                   std::to_string(entry.subAnimId) + ": " + e.what());
            }
        }
    }

    pImpl->chunkParser.drainIssues(pImpl->issues);
    return fileSystem;
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

// ============================================================================
// ParserImpl Implementation - Moved all private methods here
// ============================================================================

void Parser::Impl::reportIssue(const std::string& message) {
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(message);
    }
    issues.push_back(message);
}

void Parser::Impl::parseBase(BinaryReader& reader, BaseFile& file, WoWFileSystem* wfs) {
    u32 magic = reader.read<u32>();
    reader.setPosition(0);

    if (magic == MD20_TAG) {
        file.format = Format::ClassicMD20;
        wfs->exploratorySearch(); // Populate WoWFileSystem with any discoverable satellite files
        BinaryParseVisitor parser(reader, wfs);
        parser.read(file.header);
        return;
    } else if (magic == MD21_TAG) {
        file.format = Format::LegionMD21;
        chunkParser.parseChunkedBase(reader, file, wfs);
        return;
    }
    std::string error = "Invalid M2 magic: expected MD20 or MD21, got '" +
                        std::string(reinterpret_cast<char*>(&magic), 4) + "'";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
}

void Parser::Impl::parseSkin(BinaryReader& reader, SkinFile& skinFile, WoWFileSystem* wfs) {
    u32 magic = reader.read<u32>();
    reader.setPosition(0);

    if (magic == SKIN_TAG) {
        skinFile.profile = SkinProfile();
        BinaryParseVisitor parser(reader, wfs);
        parser.read(skinFile.profile);
        return;
    }
    std::string error = "Invalid M2 magic: expected SKIN, got '" +
                        std::string(reinterpret_cast<char*>(&magic), 4) + "'";
    if (parseMode == ParseMode::Strict) {
        throw std::runtime_error(error);
    }
    issues.push_back(error);
}

} // namespace m2
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_d3_root_test: Validates D3 root manifest parsing against corpus data.
/// Pipeline: .build.info → build config → index → archive read → BLTE decode
///           → encoding table → root CKey → root EKey → archive read → decode → parse root.

#include "../src/whiteout/storages/casc/roots/d3_root.h"
#include "../src/whiteout/storages/casc/roots/root.h"
#include "../src/whiteout/storages/casc/config.h"
#include "../src/whiteout/storages/casc/encoding.h"
#include "../src/whiteout/storages/casc/index.h"
#include "../src/whiteout/storages/casc/blte.h"
#include "../src/whiteout/storages/common/mapped_file.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;
using namespace whiteout::storages::common;


static std::string findCorpusBase() {
    for (auto& p : {"Corpus/CASC", "../Corpus/CASC", "../../Corpus/CASC",
                     "C:/Projects/WhiteoutLib/Corpus/CASC"}) {
        if (std::filesystem::exists(p))
            return p;
    }
    return "";
}

/// Read raw bytes from an archive file at given offset+size.
/// Skips the 30-byte local archive header (EKey + sizes + flags).
static std::vector<u8> readArchiveData(const std::string& dataDir,
                                       u32 archiveIndex, u32 offset, u32 size) {
    // Build archive path: data/data.NNN
    char archiveName[64];
    std::snprintf(archiveName, sizeof(archiveName), "data/data.%03u", archiveIndex);
    std::string archivePath = dataDir + "/" + archiveName;

    std::ifstream file(archivePath, std::ios::binary);
    if (!file) return {};

    // Skip the 30-byte archive entry header (EKey hash + sizes + flags).
    constexpr u32 kArchiveHeaderSize = 30;
    if (size <= kArchiveHeaderSize) return {};
    file.seekg(offset + kArchiveHeaderSize);
    u32 dataSize = size - kArchiveHeaderSize;

    std::vector<u8> buf(dataSize);
    file.read(reinterpret_cast<char*>(buf.data()), dataSize);
    if (!file) return {};

    return buf;
}

/// Full pipeline: resolve a CKey to decoded data using encoding + index + archives.
static std::vector<u8> resolveCKey(const std::string& dataDir,
                                   const EncodingTable& encoding,
                                   const IndexTable& index,
                                   std::span<const u8, 16> cKey) {
    auto encEntry = encoding.findByCKey(cKey);
    if (!encEntry) return {};

    auto idxEntry = index.find(std::span<const u8>(encEntry->eKey.data(), 9));
    if (!idxEntry) return {};

    auto blteData = readArchiveData(dataDir, idxEntry->archiveIndex,
                                    idxEntry->archiveOffset, idxEntry->encodedSize);
    if (blteData.empty()) return {};

    auto decoded = blteDecode(blteData);
    if (!decoded.success) return {};

    return std::move(decoded.data);
}

TEST_CASE("D3Root", "[casc][d3_root][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }
    std::string gameDir = corpus + "/Diablo III";
    std::string dataDir = gameDir + "/Data";

    // Step 1: Read .build.info (lives at game root, not inside Data/).
    auto buildInfoFile = MappedFile::open(gameDir + "/.build.info");
    REQUIRE(buildInfoFile.has_value());

    auto builds = parseBuildInfo(buildInfoFile->data());
    REQUIRE_FALSE(builds.empty());

    // Find active build.
    const BuildInfo* activeBuild = nullptr;
    for (auto& b : builds) {
        if (b.active) { activeBuild = &b; break; }
    }
    if (!activeBuild) activeBuild = &builds[0];
    std::cout << "  Build: " << activeBuild->version << "\n";

    // Step 2: Read build config.
    // Build config file is at data/config/XX/YY/<buildKey hex>
    char configPath[512];
    std::snprintf(configPath, sizeof(configPath),
        "%s/config/%02x/%02x/%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
        dataDir.c_str(),
        activeBuild->buildKey[0], activeBuild->buildKey[1],
        activeBuild->buildKey[0], activeBuild->buildKey[1],
        activeBuild->buildKey[2], activeBuild->buildKey[3],
        activeBuild->buildKey[4], activeBuild->buildKey[5],
        activeBuild->buildKey[6], activeBuild->buildKey[7],
        activeBuild->buildKey[8], activeBuild->buildKey[9],
        activeBuild->buildKey[10], activeBuild->buildKey[11],
        activeBuild->buildKey[12], activeBuild->buildKey[13],
        activeBuild->buildKey[14], activeBuild->buildKey[15]);

    auto buildConfigFile = MappedFile::open(configPath);
    CHECK(buildConfigFile.has_value());
    if (!buildConfigFile) {
        std::cout << "  Path tried: " << configPath << "\n";
        return;
    }

    auto buildConfig = parseBuildConfig(buildConfigFile->data());

    // Step 3: Load index table.
    auto indexTable = IndexTable::load(dataDir);
    CHECK(indexTable.entryCount() > 0);

    // Step 4: Resolve encoding file.
    auto encodingIdx = indexTable.find(std::span<const u8>(buildConfig.encodingEKey.data(), 9));
    REQUIRE(encodingIdx != nullptr);

    auto encodingBlte = readArchiveData(dataDir, encodingIdx->archiveIndex,
                                        encodingIdx->archiveOffset, encodingIdx->encodedSize);
    REQUIRE_FALSE(encodingBlte.empty());

    auto encodingDecoded = blteDecode(encodingBlte);
    CHECK(encodingDecoded.success);
    if (!encodingDecoded.success) {
        std::cout << "  Error: " << encodingDecoded.error << "\n";
        return;
    }

    auto encodingTable = EncodingTable::parse(encodingDecoded.data);
    CHECK(encodingTable.entryCount() > 0);

    // Step 5: Resolve root file.
    auto rootEncEntry = encodingTable.findByCKey(buildConfig.rootCKey);
    REQUIRE(rootEncEntry != nullptr);

    auto rootIdx = indexTable.find(std::span<const u8>(rootEncEntry->eKey.data(), 9));
    REQUIRE(rootIdx != nullptr);

    auto rootBlte = readArchiveData(dataDir, rootIdx->archiveIndex,
                                    rootIdx->archiveOffset, rootIdx->encodedSize);
    REQUIRE_FALSE(rootBlte.empty());

    auto rootDecoded = blteDecode(rootBlte);
    CHECK(rootDecoded.success);
    if (!rootDecoded.success) {
        std::cout << "  Error: " << rootDecoded.error << "\n";
        return;
    }

    // Step 6: Parse D3 root with sub-directory resolver.
    auto resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        return resolveCKey(dataDir, encodingTable, indexTable, cKey);
    };

    auto root = D3Root::parse(rootDecoded.data, resolver);
    REQUIRE(root != nullptr);

    std::cout << "  Root entries: " << root->entryCount() << "\n";
    CHECK(root->entryCount() > 0);
    CHECK(root->entryCount() > 100);
    CHECK(root->format() == RootFormat::Diablo3);

    // Step 7: Auto-detection test.
    auto autoRoot = RootManifest::parse(rootDecoded.data);
    CHECK(autoRoot != nullptr);
    if (autoRoot) {
        CHECK(autoRoot->format() == RootFormat::Diablo3);
    }

    // Step 8: Enumerate and count.
    size_t enumCount = 0;
    root->enumerate([&](const RootEntry& e) {
        ++enumCount;
        return true;
    });
    CHECK(enumCount == root->entryCount());

    // Step 9: Verify some entries have non-empty paths.
    size_t withPath = 0;
    size_t withNamedPath = 0; // paths that look CascLib-compatible (contain a '.')
    root->enumerate([&](const RootEntry& e) {
        if (!e.path.empty()) {
            ++withPath;
            // CascLib-compatible paths have extensions: Actor\SomeName.acr
            if (e.path.find('.') != std::string::npos)
                ++withNamedPath;
        }
        return true;
    });
    CHECK(withPath > 0);
    CHECK(withNamedPath > 0);
    std::cout << "  Entries with paths: " << withPath << "\n";
    std::cout << "  Entries with named paths (CoreTOC-resolved): " << withNamedPath << "\n";

    // Step 10: Verify CascLib-compatible path structure.
    // Asset paths should follow: [SubDir\]GroupDir\Name.ext
    // Note: enumerateUnder compares against raw e.path (case-sensitive).
    size_t actorCount = 0;
    root->enumerateUnder("Base\\Actor\\", [&](const RootEntry&) {
        ++actorCount;
        return true;
    });
    size_t texturesCount = 0;
    root->enumerateUnder("Base\\Textures\\", [&](const RootEntry&) {
        ++texturesCount;
        return true;
    });
    std::cout << "  Actor entries under Base\\Actor\\: " << actorCount << "\n";
    std::cout << "  Texture entries under Base\\Textures\\: " << texturesCount << "\n";
    // D3 should have many assets in these well-known directories.
    CHECK(actorCount > 0);
    CHECK(texturesCount > 0);

    // Print a few example paths for visual inspection.
    size_t printed = 0;
    std::cout << "  Sample asset paths:\n";
    root->enumerateUnder("Base\\Actor\\", [&](const RootEntry& e) {
        if (printed < 5) {
            std::cout << "    " << e.path << "\n";
            ++printed;
        }
        return printed < 5;
    });
    printed = 0;
    root->enumerateUnder("Base\\Textures\\", [&](const RootEntry& e) {
        if (printed < 5) {
            std::cout << "    " << e.path << "\n";
            ++printed;
        }
        return printed < 5;
    });
    printed = 0;
    root->enumerateUnder("Base\\SoundBank\\", [&](const RootEntry& e) {
        if (printed < 5) {
            std::cout << "    " << e.path << "\n";
            ++printed;
        }
        return printed < 5;
    });
}

// Synthetic D3 root test (no corpus needed).
TEST_CASE("Synthetic D3Root", "[casc][d3_root]") {
    // Build a minimal D3 root directory with subdirectory signature.
    std::vector<u8> buf;

    auto writeLE32 = [&](u32 v) {
        buf.push_back(u8(v));
        buf.push_back(u8(v >> 8));
        buf.push_back(u8(v >> 16));
        buf.push_back(u8(v >> 24));
    };

    // Subdirectory signature.
    writeLE32(0xEAF1FE87);

    // Asset entries: 2 entries.
    writeLE32(2);
    for (int i = 0; i < 2; ++i) {
        // CKey (16 bytes).
        for (int j = 0; j < 16; ++j) buf.push_back(u8(0x10 + i * 0x20 + j));
        // FileIndex (u32): upper 16 = assetType, lower 16 = index.
        writeLE32(u32(0x2C << 16) | u32(i)); // 0x2C = Textures
    }

    // AssetIdx entries: 1 entry.
    writeLE32(1);
    for (int j = 0; j < 16; ++j) buf.push_back(u8(0x50 + j));
    writeLE32(u32(0x01 << 16) | 5); // Actor, index 5
    writeLE32(3);                    // subIndex = 3

    // Named entries: 2 entries.
    writeLE32(2);
    // Entry 1: CKey + "CoreTOC.dat"
    for (int j = 0; j < 16; ++j) buf.push_back(u8(0x70 + j));
    {
        const char* name = "CoreTOC.dat";
        buf.insert(buf.end(), name, name + strlen(name));
        buf.push_back(0); // null terminator
    }
    // Entry 2: CKey + "Packages.dat"
    for (int j = 0; j < 16; ++j) buf.push_back(u8(0x90 + j));
    {
        const char* name = "Packages.dat";
        buf.insert(buf.end(), name, name + strlen(name));
        buf.push_back(0);
    }

    auto root = D3Root::parse(buf);
    REQUIRE(root != nullptr);

    // 3 entries: 2 asset + 1 assetIdx. Named entries are sub-directory
    // references and are NOT emitted as root entries (without a resolver
    // they can't be resolved for CoreTOC loading).
    CHECK(root->entryCount() == 3);
    CHECK(root->format() == RootFormat::Diablo3);

    // Verify asset paths — without a resolver/CoreTOC, paths use the numeric fallback:
    //   getGroupDir(fileIndex) + "\" + fileIndex
    // where getGroupDir extracts group from upper 16 bits of the fileIndex.
    // FileIndex = 0x2C << 16 | 0 = 2883584 → group 0x2C = "tex"
    auto texEntry = root->findByPath("tex/2883584");
    CHECK_FALSE(texEntry.empty());

    // FileIndex = 0x01 << 16 | 5 = 65541, subIndex = 3 → group 0x01 = "acr"
    auto actorEntry = root->findByPath("acr/65541.3");
    CHECK_FALSE(actorEntry.empty());

    // FileDataId lookup should find the asset entries by fileIndex.
    auto byFdid = root->findByFileDataId(u32(0x2C << 16) | 0);
    CHECK_FALSE(byFdid.empty());

    auto byFdid2 = root->findByFileDataId(100);
    CHECK(byFdid2.empty());
}

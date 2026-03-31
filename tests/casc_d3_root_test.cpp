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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;
using namespace whiteout::storages::common;

static int g_passed = 0;
static int g_failed = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        std::cout << "  PASS: " << name << "\n";
        ++g_passed;
    } else {
        std::cout << "  FAIL: " << name << "\n";
        ++g_failed;
    }
}

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

static void testD3Root(const std::string& corpus) {
    std::cout << "[Test: D3 Root Pipeline]\n";

    std::string gameDir = corpus + "/Diablo III";
    std::string dataDir = gameDir + "/Data";

    // Step 1: Read .build.info (lives at game root, not inside Data/).
    auto buildInfoFile = MappedFile::open(gameDir + "/.build.info");
    check(buildInfoFile.has_value(), "D3 .build.info opens");
    if (!buildInfoFile) return;

    auto builds = parseBuildInfo(buildInfoFile->data());
    check(!builds.empty(), "D3 has build info entries");
    if (builds.empty()) return;

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
    check(buildConfigFile.has_value(), "D3 build config opens");
    if (!buildConfigFile) {
        std::cout << "  Path tried: " << configPath << "\n";
        return;
    }

    auto buildConfig = parseBuildConfig(buildConfigFile->data());

    // Step 3: Load index table.
    auto indexTable = IndexTable::load(dataDir);
    check(indexTable.entryCount() > 0, "D3 index loaded");

    // Step 4: Resolve encoding file.
    auto encodingIdx = indexTable.find(std::span<const u8>(buildConfig.encodingEKey.data(), 9));
    check(encodingIdx != nullptr, "Encoding EKey found in index");
    if (!encodingIdx) return;

    auto encodingBlte = readArchiveData(dataDir, encodingIdx->archiveIndex,
                                        encodingIdx->archiveOffset, encodingIdx->encodedSize);
    check(!encodingBlte.empty(), "Encoding archive data read");
    if (encodingBlte.empty()) return;

    auto encodingDecoded = blteDecode(encodingBlte);
    check(encodingDecoded.success, "Encoding BLTE decoded");
    if (!encodingDecoded.success) {
        std::cout << "  Error: " << encodingDecoded.error << "\n";
        return;
    }

    auto encodingTable = EncodingTable::parse(encodingDecoded.data);
    check(encodingTable.entryCount() > 0, "Encoding table has entries");

    // Step 5: Resolve root file.
    auto rootEncEntry = encodingTable.findByCKey(buildConfig.rootCKey);
    check(rootEncEntry != nullptr, "Root CKey found in encoding table");
    if (!rootEncEntry) return;

    auto rootIdx = indexTable.find(std::span<const u8>(rootEncEntry->eKey.data(), 9));
    check(rootIdx != nullptr, "Root EKey found in index");
    if (!rootIdx) return;

    auto rootBlte = readArchiveData(dataDir, rootIdx->archiveIndex,
                                    rootIdx->archiveOffset, rootIdx->encodedSize);
    check(!rootBlte.empty(), "Root archive data read");
    if (rootBlte.empty()) return;

    auto rootDecoded = blteDecode(rootBlte);
    check(rootDecoded.success, "Root BLTE decoded");
    if (!rootDecoded.success) {
        std::cout << "  Error: " << rootDecoded.error << "\n";
        return;
    }

    // Step 6: Parse D3 root with sub-directory resolver.
    auto resolver = [&](std::span<const u8, 16> cKey) -> std::vector<u8> {
        return resolveCKey(dataDir, encodingTable, indexTable, cKey);
    };

    auto root = D3Root::parse(rootDecoded.data, resolver);
    check(root != nullptr, "D3 root parsed");
    if (!root) return;

    std::cout << "  Root entries: " << root->entryCount() << "\n";
    check(root->entryCount() > 0, "D3 root has entries");
    check(root->entryCount() > 100, "D3 root has >100 entries");
    check(root->format() == RootFormat::Diablo3, "Format is Diablo3");

    // Step 7: Auto-detection test.
    auto autoRoot = RootManifest::parse(rootDecoded.data);
    check(autoRoot != nullptr, "Auto-detection parsed D3 root");
    if (autoRoot) {
        check(autoRoot->format() == RootFormat::Diablo3, "Auto-detected as Diablo3");
    }

    // Step 8: Enumerate and count.
    size_t enumCount = 0;
    root->enumerate([&](const RootEntry& e) {
        ++enumCount;
        return true;
    });
    check(enumCount == root->entryCount(), "Enumerate count matches entryCount");

    // Step 9: Verify some entries have non-empty paths.
    size_t withPath = 0;
    root->enumerate([&](const RootEntry& e) {
        if (!e.path.empty()) ++withPath;
        return true;
    });
    check(withPath > 0, "Some entries have paths");
    std::cout << "  Entries with paths: " << withPath << "\n";
}

// Synthetic D3 root test (no corpus needed).
static void testSyntheticD3Root() {
    std::cout << "[Test: Synthetic D3 root]\n";

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
        writeLE32(u32(0x2D << 16) | u32(i)); // 0x2D = Textures
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
    check(root != nullptr, "Synthetic D3 root parsed");
    if (!root) return;

    check(root->entryCount() == 5, "Synthetic D3 has 5 entries (2 asset + 1 assetidx + 2 named)");
    check(root->format() == RootFormat::Diablo3, "Format is Diablo3");

    // Verify path lookup (case-insensitive).
    auto found = root->findByPath("coretoc.dat");
    check(!found.empty(), "CoreTOC.dat found by path (case-insensitive)");

    auto found2 = root->findByPath("Packages.dat");
    check(!found2.empty(), "Packages.dat found by path");

    // Verify asset path.
    auto texEntry = root->findByPath("tex/0");
    check(!texEntry.empty(), "tex/0 asset path found");

    auto actorEntry = root->findByPath("acr/5.3");
    check(!actorEntry.empty(), "acr/5.3 assetidx path found");

    // FileDataId should return empty for D3.
    auto byFdid = root->findByFileDataId(100);
    check(byFdid.empty(), "findByFileDataId returns empty for D3");
}

int main() {
    std::cout << "=== CASC D3 Root Tests ===\n\n";

    testSyntheticD3Root();

    auto corpus = findCorpusBase();
    if (!corpus.empty()) {
        testD3Root(corpus);
    } else {
        std::cout << "WARNING: Corpus not found, skipping corpus-dependent tests.\n";
    }

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}

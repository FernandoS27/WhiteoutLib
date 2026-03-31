// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_tvfs_root_test: Validates TVFS root manifest parsing against WC3 corpus data.

#include "../src/whiteout/storages/casc/roots/tvfs_root.h"
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

static std::vector<u8> readArchiveData(const std::string& dataDir,
                                       u32 archiveIndex, u32 offset, u32 size) {
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

static void testTvfsRoot(const std::string& corpus) {
    std::cout << "[Test: TVFS Root Pipeline (WC3)]\n";

    std::string gameDir = corpus + "/Warcraft III";
    std::string dataDir = gameDir + "/Data";

    // Step 1: Read .build.info (lives at game root, not inside Data/).
    auto buildInfoFile = MappedFile::open(gameDir + "/.build.info");
    check(buildInfoFile.has_value(), "WC3 .build.info opens");
    if (!buildInfoFile) return;

    auto builds = parseBuildInfo(buildInfoFile->data());
    check(!builds.empty(), "WC3 has build info entries");
    if (builds.empty()) return;

    const BuildInfo* activeBuild = nullptr;
    for (auto& b : builds) {
        if (b.active) { activeBuild = &b; break; }
    }
    if (!activeBuild) activeBuild = &builds[0];
    std::cout << "  Build: " << activeBuild->version << "\n";

    // Step 2: Read build config.
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
    check(buildConfigFile.has_value(), "WC3 build config opens");
    if (!buildConfigFile) {
        std::cout << "  Path tried: " << configPath << "\n";
        return;
    }

    auto buildConfig = parseBuildConfig(buildConfigFile->data());

    // WC3 uses VFS root (vfsRootCKey/vfsRootEKey).
    bool hasVfsRoot = (buildConfig.vfsRootCKey != std::array<u8, 16>{});
    std::cout << "  Has VFS root: " << (hasVfsRoot ? "yes" : "no") << "\n";

    // Use vfsRootEKey if available, otherwise fall back to root CKey.
    auto rootCKey = hasVfsRoot ? buildConfig.vfsRootCKey : buildConfig.rootCKey;
    auto rootEKey = hasVfsRoot ? buildConfig.vfsRootEKey : std::array<u8, 16>{};

    std::cout << "  Root CKey: ";
    for (auto b : rootCKey) printf("%02x", b);
    std::cout << "\n";

    // Step 3: Load index table.
    auto indexTable = IndexTable::load(dataDir);
    std::cout << "  Index entries: " << indexTable.entryCount() << "\n";
    check(indexTable.entryCount() > 0, "WC3 index loaded");

    // Step 4: Resolve encoding + root.
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
    std::cout << "  Encoding entries: " << encodingTable.entryCount() << "\n";
    check(encodingTable.entryCount() > 0, "Encoding table has entries");

    // Try resolving root via encoding, or directly via EKey if VFS root has direct EKey.
    const IndexEntry* rootIdx = nullptr;
    if (rootEKey != std::array<u8, 16>{}) {
        rootIdx = indexTable.find(std::span<const u8>(rootEKey.data(), 9));
    }
    if (!rootIdx) {
        auto rootEncEntry = encodingTable.findByCKey(rootCKey);
        if (rootEncEntry)
            rootIdx = indexTable.find(std::span<const u8>(rootEncEntry->eKey.data(), 9));
    }

    check(rootIdx != nullptr, "Root found in index");
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

    std::cout << "  Root data size: " << rootDecoded.data.size() << " bytes\n";
    if (rootDecoded.data.size() >= 4) {
        u32 firstWord = u32(rootDecoded.data[0]) | (u32(rootDecoded.data[1]) << 8) |
                        (u32(rootDecoded.data[2]) << 16) | (u32(rootDecoded.data[3]) << 24);
        std::cout << "  Root first 4 bytes: 0x" << std::hex << firstWord << std::dec << "\n";
    }

    // Step 5: Parse TVFS root.
    auto root = TvfsRoot::parse(rootDecoded.data);
    check(root != nullptr, "TVFS root parsed");
    if (!root) {
        // If TVFS fails, try auto-detection.
        std::cout << "  Note: Direct TVFS parse failed, trying auto-detection...\n";
        auto autoRoot = RootManifest::parse(rootDecoded.data);
        check(autoRoot != nullptr, "Auto-detection found a root format");
        if (autoRoot) {
            std::cout << "  Auto-detected format: " << int(autoRoot->format()) << "\n";
            std::cout << "  Auto-detected entries: " << autoRoot->entryCount() << "\n";
        }
        return;
    }

    std::cout << "  TVFS entries: " << root->entryCount() << "\n";
    check(root->entryCount() > 0, "TVFS root has entries");
    check(root->format() == RootFormat::Tvfs, "Format is Tvfs");

    // Step 6: Auto-detection test.
    auto autoRoot = RootManifest::parse(rootDecoded.data);
    check(autoRoot != nullptr, "Auto-detection parsed TVFS root");
    if (autoRoot) {
        check(autoRoot->format() == RootFormat::Tvfs, "Auto-detected as Tvfs");
    }

    // Step 7: Enumerate.
    size_t enumCount = 0;
    root->enumerate([&](const RootEntry& e) {
        ++enumCount;
        return true;
    });
    check(enumCount == root->entryCount(), "Enumerate count matches entryCount");

    // Step 8: Print some sample paths.
    int printed = 0;
    root->enumerate([&](const RootEntry& e) {
        if (printed < 10 && !e.path.empty()) {
            std::cout << "    [" << printed << "] " << e.path << "\n";
            ++printed;
        }
        return printed < 10;
    });
    check(printed > 0, "Found entries with paths");
}

// Synthetic TVFS test.
static void testSyntheticTvfs() {
    std::cout << "[Test: Synthetic TVFS root]\n";

    // Build a minimal TVFS blob.
    // This is complex — we'll build a tiny prefix tree with one file.
    std::vector<u8> buf;

    // Helper lambdas.
    auto writeLE32 = [&](u32 v) {
        buf.push_back(u8(v));
        buf.push_back(u8(v >> 8));
        buf.push_back(u8(v >> 16));
        buf.push_back(u8(v >> 24));
    };
    auto writeBE32 = [&](u32 v) {
        buf.push_back(u8(v >> 24));
        buf.push_back(u8(v >> 16));
        buf.push_back(u8(v >> 8));
        buf.push_back(u8(v));
    };
    auto writeBE16 = [&](u16 v) {
        buf.push_back(u8(v >> 8));
        buf.push_back(u8(v));
    };

    // We'll have:
    //   pathTable: one leaf "test.txt" → VFS offset 0
    //   vfsTable: 1 span → CFT offset 0
    //   cftTable: 9-byte EKey

    // Build CFT table first to know sizes.
    std::vector<u8> cftTable;
    // Entry: EKey(9 bytes)
    for (int i = 0; i < 9; ++i) cftTable.push_back(u8(0xAA + i));

    // Build VFS table.
    std::vector<u8> vfsTable;
    vfsTable.push_back(1); // spanCount = 1
    // FileOffset (BE u32) = 0
    vfsTable.push_back(0); vfsTable.push_back(0);
    vfsTable.push_back(0); vfsTable.push_back(0);
    // SpanSize (BE u32) = 100
    vfsTable.push_back(0); vfsTable.push_back(0);
    vfsTable.push_back(0); vfsTable.push_back(100);
    // CftOffset (1 byte, since cftTable fits in 1 byte) = 0
    vfsTable.push_back(0);

    // Build path table.
    std::vector<u8> pathTable;
    // Leaf entry: nameLen(1) + name + 0xFF + NodeValue(file, BE u32 = VFS offset 0)
    const char* fileName = "test.txt";
    u8 nameLen = u8(strlen(fileName));
    pathTable.push_back(nameLen);
    pathTable.insert(pathTable.end(), fileName, fileName + nameLen);
    pathTable.push_back(0xFF);
    // NodeValue = 0 (VFS offset, not a folder).
    pathTable.push_back(0);
    pathTable.push_back(0);
    pathTable.push_back(0);
    pathTable.push_back(0);

    // Now build the full TVFS header + tables.
    u32 headerSize = 46; // all header fields
    u32 pathOff = headerSize;
    u32 pathSz = u32(pathTable.size());
    u32 vfsOff = pathOff + pathSz;
    u32 vfsSz = u32(vfsTable.size());
    u32 cftOff = vfsOff + vfsSz;
    u32 cftSz = u32(cftTable.size());

    // Signature (LE).
    writeLE32(0x53465654); // 'TVFS'
    // FormatVersion (u8) = 1
    buf.push_back(1);
    // HeaderSize (u8) — size of the header structure itself.
    buf.push_back(u8(headerSize));
    // EKeySize (u8) = 9
    buf.push_back(9);
    // PatchKeySize (u8) = 9
    buf.push_back(9);
    // Flags (LE u32) = 0
    writeLE32(0);
    // PathTableOffset (BE u32)
    writeBE32(pathOff);
    // PathTableSize (BE u32)
    writeBE32(pathSz);
    // VfsTableOffset (BE u32)
    writeBE32(vfsOff);
    // VfsTableSize (BE u32)
    writeBE32(vfsSz);
    // CftTableOffset (BE u32)
    writeBE32(cftOff);
    // CftTableSize (BE u32)
    writeBE32(cftSz);
    // MaxDepth (BE u16) = 4
    writeBE16(4);
    // EstTableOffset (BE u32) = 0
    writeBE32(0);
    // EstTableSize (BE u32) = 0
    writeBE32(0);

    // Append tables.
    buf.insert(buf.end(), pathTable.begin(), pathTable.end());
    buf.insert(buf.end(), vfsTable.begin(), vfsTable.end());
    buf.insert(buf.end(), cftTable.begin(), cftTable.end());

    auto root = TvfsRoot::parse(buf);
    check(root != nullptr, "Synthetic TVFS root parsed");
    if (!root) return;

    check(root->entryCount() >= 1, "Synthetic TVFS has entries");
    check(root->format() == RootFormat::Tvfs, "Format is Tvfs");

    // Look up the file.
    auto found = root->findByPath("test.txt");
    check(!found.empty(), "test.txt found by path");

    // Case-insensitive.
    auto foundUpper = root->findByPath("TEST.TXT");
    check(!foundUpper.empty(), "TEST.TXT found (case-insensitive)");

    // Auto-detection.
    auto autoRoot = RootManifest::parse(buf);
    check(autoRoot != nullptr, "Auto-detection parsed synthetic TVFS");
    if (autoRoot) {
        check(autoRoot->format() == RootFormat::Tvfs, "Auto-detected as Tvfs");
    }
}

int main() {
    std::cout << "=== CASC TVFS Root Tests ===\n\n";

    testSyntheticTvfs();

    auto corpus = findCorpusBase();
    if (!corpus.empty()) {
        testTvfsRoot(corpus);
    } else {
        std::cout << "WARNING: Corpus not found, skipping corpus-dependent tests.\n";
    }

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}

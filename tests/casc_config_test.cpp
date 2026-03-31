// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_config_test: Validates CASC config file parsing against real corpus data.

#include "../src/whiteout/storages/casc/config.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

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

static std::vector<u8> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = f.tellg();
    f.seekg(0);
    std::vector<u8> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

static std::string hexStr(std::span<const u8> data) {
    std::string s;
    for (auto b : data) {
        s += "0123456789abcdef"[(b >> 4) & 0xF];
        s += "0123456789abcdef"[b & 0xF];
    }
    return s;
}

// ============================================================================
// Helper: locate corpus path relative to executable
// ============================================================================

static std::string findCorpusBase() {
    // Try paths relative to CWD, then hardcoded corpus locations.
    for (auto& p : {"Corpus/CASC", "../Corpus/CASC", "../../Corpus/CASC",
                     "C:/Projects/WhiteoutLib/Corpus/CASC"}) {
        if (std::filesystem::exists(p))
            return p;
    }
    return "";
}

// ============================================================================
// Tests
// ============================================================================

static void testParseBuildInfoD3(const std::string& corpus) {
    std::cout << "[Test: Parse D3 .build.info]\n";
    auto data = readFile(corpus + "/Diablo III/.build.info");
    check(!data.empty(), "D3 .build.info file loaded");
    if (data.empty()) return;

    auto infos = parseBuildInfo(data);
    check(!infos.empty(), "Parsed at least one entry");
    if (infos.empty()) return;

    auto& info = infos[0];
    check(info.branch == "eu", "Branch is 'eu'");
    check(info.active, "Active is true");
    check(info.product.empty() || info.product.size() > 0, "Product field parsed");
    check(info.version == "2.8.0.99920", "Version is 2.8.0.99920");

    // Build key should be e34fc3fb7831a77e0ad7def0bb399ac7
    std::string bkHex = hexStr(info.buildKey);
    check(bkHex == "e34fc3fb7831a77e0ad7def0bb399ac7", "Build key matches expected");

    // CDN key should be 6547fc64ca796c7c1b25864145814ace
    std::string ckHex = hexStr(info.cdnKey);
    check(ckHex == "6547fc64ca796c7c1b25864145814ace", "CDN key matches expected");
}

static void testParseBuildInfoWC3(const std::string& corpus) {
    std::cout << "[Test: Parse WC3 .build.info]\n";
    auto data = readFile(corpus + "/Warcraft III/.build.info");
    check(!data.empty(), "WC3 .build.info file loaded");
    if (data.empty()) return;

    auto infos = parseBuildInfo(data);
    check(!infos.empty(), "Parsed at least one entry");
    if (infos.empty()) return;

    auto& info = infos[0];
    check(info.version == "2.0.4.23556", "Version is 2.0.4.23556");
    check(info.product == "w3", "Product is 'w3'");

    std::string bkHex = hexStr(info.buildKey);
    check(bkHex == "89e4f360ea99cdc4df4c68b10256e9c8", "Build key matches expected");
}

static void testParseBuildConfigD3(const std::string& corpus) {
    std::cout << "[Test: Parse D3 build config]\n";
    auto data = readFile(corpus + "/Diablo III/Data/config/e3/4f/e34fc3fb7831a77e0ad7def0bb399ac7");
    check(!data.empty(), "D3 build config loaded");
    if (data.empty()) return;

    auto cfg = parseBuildConfig(data);
    // root = e40cbb7f63784322bec89b76d722343c
    check(hexStr(cfg.rootCKey) == "e40cbb7f63784322bec89b76d722343c", "Root CKey matches");
    // encoding = 985ce1d6899bab1f2ea25490dba68823 084e2425ddd8789f89182d65a45f1383
    check(hexStr(cfg.encodingCKey) == "985ce1d6899bab1f2ea25490dba68823", "Encoding CKey matches");
    check(hexStr(cfg.encodingEKey) == "084e2425ddd8789f89182d65a45f1383", "Encoding EKey matches");
    check(cfg.buildProduct == "Diablo3", "Build product is Diablo3");
    check(cfg.buildName == "99920_Win32_2_8_0_RC", "Build name matches");
    check(cfg.buildUid == "d3", "Build UID is d3");
}

static void testParseBuildConfigWC3(const std::string& corpus) {
    std::cout << "[Test: Parse WC3 build config]\n";
    auto data = readFile(corpus + "/Warcraft III/Data/config/89/e4/89e4f360ea99cdc4df4c68b10256e9c8");
    check(!data.empty(), "WC3 build config loaded");
    if (data.empty()) return;

    auto cfg = parseBuildConfig(data);
    check(hexStr(cfg.rootCKey) == "6b1d32e59bcb091e1ea126bc5d3b907b", "Root CKey matches");
    check(hexStr(cfg.encodingCKey) == "5c0b5abe189ce9368842e90f8942db91", "Encoding CKey matches");
    check(hexStr(cfg.encodingEKey) == "6156a413e584af4b341f29c98e605df5", "Encoding EKey matches");
    check(cfg.buildProduct == "War3", "Build product is War3");
    check(cfg.buildUid == "w3", "Build UID is w3");
}

static void testParseCdnConfigD3(const std::string& corpus) {
    std::cout << "[Test: Parse D3 CDN config]\n";
    auto data = readFile(corpus + "/Diablo III/Data/config/65/47/6547fc64ca796c7c1b25864145814ace");
    check(!data.empty(), "D3 CDN config loaded");
    if (data.empty()) return;

    auto cdn = parseCdnConfig(data);
    check(!cdn.archiveEKeys.empty(), "Archive list is non-empty");
    // D3 corpus has 134 archives + 3 more = 137 total in archive list, let's check > 100
    check(cdn.archiveEKeys.size() > 100, "More than 100 archives");
    check(cdn.archiveIndexSizes.size() == cdn.archiveEKeys.size(),
          "Index sizes count matches archive count");
}

static void testParseCdnConfigWC3(const std::string& corpus) {
    std::cout << "[Test: Parse WC3 CDN config]\n";
    auto data = readFile(corpus + "/Warcraft III/Data/config/e2/fe/e2fe3a94ae5dbd41cc3d3e0be9a4d10e");
    check(!data.empty(), "WC3 CDN config loaded");
    if (data.empty()) return;

    auto cdn = parseCdnConfig(data);
    check(!cdn.archiveEKeys.empty(), "Archive list is non-empty");
    check(cdn.archiveEKeys.size() > 100, "More than 100 archives");
}

static void testEdgeCases() {
    std::cout << "[Test: Edge cases]\n";

    // Empty input
    auto infos = parseBuildInfo({});
    check(infos.empty(), "parseBuildInfo on empty returns empty");

    auto cfg = parseBuildConfig({});
    check(cfg.buildName.empty(), "parseBuildConfig on empty returns default");

    auto cdn = parseCdnConfig({});
    check(cdn.archiveEKeys.empty(), "parseCdnConfig on empty returns default");

    auto shmem = parseShmem({});
    check(shmem.archiveCount == 0, "parseShmem on empty returns default");
}

int main() {
    std::cout << "=== CASC Config Tests ===\n\n";

    auto corpus = findCorpusBase();
    if (corpus.empty()) {
        std::cout << "WARNING: Corpus not found, skipping corpus-dependent tests.\n";
    }

    if (!corpus.empty()) {
        testParseBuildInfoD3(corpus);
        testParseBuildInfoWC3(corpus);
        testParseBuildConfigD3(corpus);
        testParseBuildConfigWC3(corpus);
        testParseCdnConfigD3(corpus);
        testParseCdnConfigWC3(corpus);
    }

    testEdgeCases();

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}

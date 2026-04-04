// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_config_test: Validates CASC config file parsing against real corpus data.

#include "../src/whiteout/storages/casc/tables/config.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

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

static std::string findCorpusBase() {
    for (auto& p : {"Corpus/CASC", "../Corpus/CASC", "../../Corpus/CASC",
                     "C:/Projects/WhiteoutLib/Corpus/CASC"}) {
        if (std::filesystem::exists(p))
            return p;
    }
    return "";
}

// ============================================================================
// Corpus-dependent tests
// ============================================================================

TEST_CASE("Parse D3 .build.info", "[casc][config][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto data = readFile(corpus + "/Diablo III/.build.info");
    REQUIRE_FALSE(data.empty());

    auto infos = parseBuildInfo(data);
    REQUIRE_FALSE(infos.empty());

    auto& info = infos[0];
    CHECK(info.branch == "eu");
    CHECK(info.active);
    CHECK(info.version == "2.8.0.99920");
    CHECK(hexStr(info.buildKey) == "e34fc3fb7831a77e0ad7def0bb399ac7");
    CHECK(hexStr(info.cdnKey) == "6547fc64ca796c7c1b25864145814ace");
}

TEST_CASE("Parse WC3 .build.info", "[casc][config][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto data = readFile(corpus + "/Warcraft III/.build.info");
    REQUIRE_FALSE(data.empty());

    auto infos = parseBuildInfo(data);
    REQUIRE_FALSE(infos.empty());

    auto& info = infos[0];
    CHECK(info.version == "2.0.4.23556");
    CHECK(info.product == "w3");
    CHECK(hexStr(info.buildKey) == "89e4f360ea99cdc4df4c68b10256e9c8");
}

TEST_CASE("Parse D3 build config", "[casc][config][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto data = readFile(corpus + "/Diablo III/Data/config/e3/4f/e34fc3fb7831a77e0ad7def0bb399ac7");
    REQUIRE_FALSE(data.empty());

    auto cfg = parseBuildConfig(data);
    CHECK(hexStr(cfg.rootCKey) == "e40cbb7f63784322bec89b76d722343c");
    CHECK(hexStr(cfg.encodingCKey) == "985ce1d6899bab1f2ea25490dba68823");
    CHECK(hexStr(cfg.encodingEKey) == "084e2425ddd8789f89182d65a45f1383");
    CHECK(cfg.buildProduct == "Diablo3");
    CHECK(cfg.buildName == "99920_Win32_2_8_0_RC");
    CHECK(cfg.buildUid == "d3");
}

TEST_CASE("Parse WC3 build config", "[casc][config][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto data = readFile(corpus + "/Warcraft III/Data/config/89/e4/89e4f360ea99cdc4df4c68b10256e9c8");
    REQUIRE_FALSE(data.empty());

    auto cfg = parseBuildConfig(data);
    CHECK(hexStr(cfg.rootCKey) == "6b1d32e59bcb091e1ea126bc5d3b907b");
    CHECK(hexStr(cfg.encodingCKey) == "5c0b5abe189ce9368842e90f8942db91");
    CHECK(hexStr(cfg.encodingEKey) == "6156a413e584af4b341f29c98e605df5");
    CHECK(cfg.buildProduct == "War3");
    CHECK(cfg.buildUid == "w3");
}

TEST_CASE("Parse D3 CDN config", "[casc][config][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto data = readFile(corpus + "/Diablo III/Data/config/65/47/6547fc64ca796c7c1b25864145814ace");
    REQUIRE_FALSE(data.empty());

    auto cdn = parseCdnConfig(data);
    CHECK_FALSE(cdn.archiveEKeys.empty());
    CHECK(cdn.archiveEKeys.size() > 100);
    CHECK(cdn.archiveIndexSizes.size() == cdn.archiveEKeys.size());
}

TEST_CASE("Parse WC3 CDN config", "[casc][config][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto data = readFile(corpus + "/Warcraft III/Data/config/e2/fe/e2fe3a94ae5dbd41cc3d3e0be9a4d10e");
    REQUIRE_FALSE(data.empty());

    auto cdn = parseCdnConfig(data);
    CHECK_FALSE(cdn.archiveEKeys.empty());
    CHECK(cdn.archiveEKeys.size() > 100);
}

// ============================================================================
// Edge cases (no corpus needed)
// ============================================================================

TEST_CASE("Config parse edge cases", "[casc][config]") {
    SECTION("Empty build info") {
        auto infos = parseBuildInfo({});
        CHECK(infos.empty());
    }
    SECTION("Empty build config") {
        auto cfg = parseBuildConfig({});
        CHECK(cfg.buildName.empty());
    }
    SECTION("Empty CDN config") {
        auto cdn = parseCdnConfig({});
        CHECK(cdn.archiveEKeys.empty());
    }
    SECTION("Empty shmem") {
        auto shmem = parseShmem({});
        CHECK(shmem.archiveCount == 0);
    }
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file blizzard_game_finder_test.cpp
/// @brief Install paths read out of Battle.net's own files, tested on bytes.

#include "../src/whiteout/utils/blizzard_game_finder_scan.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace whiteout::utils;
using game_finder_scan::Candidate;

namespace {

// "光模型", a directory a user installed Warcraft III under. Every byte of all
// three characters is >= 0x80 — negative, where char is signed.
const std::string kCjk = "\xE5\x85\x89\xE6\xA8\xA1\xE5\x9E\x8B";

std::string varint(size_t n) {
    std::string out;
    do {
        char byte = static_cast<char>(n & 0x7F);
        n >>= 7;
        if (n)
            byte = static_cast<char>(byte | 0x80);
        out += byte;
    } while (n);
    return out;
}

/// One product.db install record, laid out the way the Battle.net Agent writes
/// it: the product uid, then a nested message whose field 1 is the install
/// path, followed by field 2 (tag 0x12) holding the region.
std::string productRecord(const std::string& uid, const std::string& path) {
    std::string const region{'\x12', '\x02', 'e', 'u', '\x18', '\x02'};
    std::string const settings = "\x0a" + varint(path.size()) + path + region;
    return "\x0a" + varint(uid.size()) + uid + "\x1a" + varint(settings.size()) + settings;
}

std::vector<std::string> pathsFor(const std::vector<Candidate>& found, BlizzardGame game) {
    std::vector<std::string> out;
    for (auto& c : found)
        if (c.game == game)
            out.push_back(c.path);
    return out;
}

} // namespace

TEST_CASE("A product.db install path keeps its non-ASCII characters", "[game_finder][unicode]") {
    std::string const install = "D:/" + kCjk + "/Warcraft III";
    std::string const db =
        productRecord("s2", "D:/Programs/StarCraft II") + productRecord("w3", install);

    auto const found = game_finder_scan::productDbPaths(db, /*windowsPaths=*/true);
    CHECK(pathsFor(found, BlizzardGame::WarcraftIIIReforged) == std::vector{install});
    CHECK(pathsFor(found, BlizzardGame::StarCraftII) ==
          std::vector<std::string>{"D:/Programs/StarCraft II"});
}

TEST_CASE("A product.db install path is not cut at the search window", "[game_finder][unicode]") {
    // The window bounds where a path may START. A UTF-8 path spends three bytes
    // per CJK character, so one well inside MAX_PATH still runs past 512 bytes.
    std::string install = "D:/";
    for (int i = 0; i < 70; ++i)
        install += kCjk;
    install += "/Warcraft III";
    REQUIRE(install.size() > 512);

    auto const found = game_finder_scan::productDbPaths(productRecord("w3", install), true);
    CHECK(pathsFor(found, BlizzardGame::WarcraftIIIReforged) == std::vector{install});
}

TEST_CASE("A POSIX product.db install path keeps its non-ASCII characters",
          "[game_finder][unicode]") {
    std::string const install = "/Applications/" + kCjk + "/Warcraft III";
    auto const found =
        game_finder_scan::productDbPaths(productRecord("w3", install), /*windowsPaths=*/false);
    CHECK(pathsFor(found, BlizzardGame::WarcraftIIIReforged) == std::vector{install});
}

TEST_CASE("A Battle.net.config install path decodes either JSON spelling",
          "[game_finder][unicode]") {
    // JSON may carry a non-ASCII character raw or as a \u escape, and a
    // character outside the BMP as a surrogate pair; all mean the same path.
    std::string const config = R"({
    "Games": {
        "w3": {
            "InstallPath": "D:\/)" +
                               kCjk + R"(\/Warcraft III"
        },
        "s2": {
            "InstallPath": "D:\/\u5149\u6a21\u578b\/StarCraft II"
        },
        "d3": {
            "InstallPath": "E:\\Games\\\uD840\uDC0B"
        }
    }
})";

    auto const found = game_finder_scan::battleNetConfigPaths(config);
    CHECK(pathsFor(found, BlizzardGame::WarcraftIIIReforged) ==
          std::vector{"D:/" + kCjk + "/Warcraft III"});
    CHECK(pathsFor(found, BlizzardGame::StarCraftII) ==
          std::vector{"D:/" + kCjk + "/StarCraft II"});
    // U+2000B, four bytes of UTF-8.
    CHECK(pathsFor(found, BlizzardGame::DiabloIII) ==
          std::vector<std::string>{"E:\\Games\\\xF0\xA0\x80\x8B"});
}

TEST_CASE("A Battle.net.config \\u escape decodes exactly the code unit it names",
          "[game_finder][unicode]") {
    // Each row is a raw InstallPath value as it sits in the file, and the UTF-8
    // path that has to come out of it.
    struct Row {
        const char* json;
        std::string expected;
    };
    std::string const replacement = "\xEF\xBF\xBD"; // U+FFFD
    Row const rows[] = {
        {R"(\u0041)", "A"},
        {R"(\u00e9 \u00E9)", "\xC3\xA9 \xC3\xA9"}, // hex digits in either case
        {R"(\u5149)", "\xE5\x85\x89"},
        {R"(\uD83D\uDE00)", "\xF0\x9F\x98\x80"}, // a surrogate pair, one code point
        // Half a pair is not a character; it becomes U+FFFD and what follows it stays.
        {R"(\uD83D)", replacement},
        {R"(\uD83Dx)", replacement + "x"},
        {R"(\uD83D\u0041)", replacement + "A"},
        {R"(\uDE00\uD83D)", replacement + replacement}, // low before high is two halves
        // Not four hex digits: not an escape, so kept as written.
        {R"(\u12G4)", R"(\u12G4)"},
        {R"(\u-123)", R"(\u-123)"},
        {R"(D:\u12)", R"(D:\u12)"},
        {R"(\\u0041)", R"(\u0041)"}, // an escaped backslash, then plain text
    };

    for (auto const& row : rows) {
        std::string const config = std::string(R"({"w3": {"InstallPath": ")") + row.json + R"("}})";
        INFO(row.json);
        auto const found = game_finder_scan::battleNetConfigPaths(config);
        CHECK(pathsFor(found, BlizzardGame::WarcraftIIIReforged) == std::vector{row.expected});
    }
}

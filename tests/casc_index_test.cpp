// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_index_test: Validates CASC .idx file parsing against real corpus data.

#include "../src/whiteout/storages/casc/index.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace whiteout;
using namespace whiteout::storages::casc;

static std::string findCorpusBase() {
    for (auto& p : {"Corpus/CASC", "../Corpus/CASC", "../../Corpus/CASC",
                     "C:/Projects/WhiteoutLib/Corpus/CASC"}) {
        if (std::filesystem::exists(p))
            return p;
    }
    return "";
}

TEST_CASE("Load D3 .idx files", "[casc][index][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto table = IndexTable::load(corpus + "/Diablo III/Data");
    CHECK(table.entryCount() > 0);
    CHECK(table.entryCount() > 1000);
}

TEST_CASE("Load WC3 .idx files", "[casc][index][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto table = IndexTable::load(corpus + "/Warcraft III/Data");
    CHECK(table.entryCount() > 0);
    CHECK(table.entryCount() > 1000);
}

TEST_CASE("Index lookup nonexistent EKey", "[casc][index][corpus]") {
    auto corpus = findCorpusBase();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    auto table = IndexTable::load(corpus + "/Warcraft III/Data");
    std::array<u8, 9> fakeKey{};
    fakeKey.fill(0xFF);
    CHECK(table.find(fakeKey) == nullptr);
}

TEST_CASE("Index insert and find", "[casc][index]") {
    IndexTable table;
    IndexEntry entry;
    entry.eKey = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    entry.archiveIndex = 42;
    entry.archiveOffset = 12345;
    entry.encodedSize = 6789;
    table.insert(entry);

    CHECK(table.entryCount() == 1);

    auto found = table.find(std::span<const u8>(entry.eKey.data(), 9));
    REQUIRE(found != nullptr);
    CHECK(found->archiveIndex == 42);
    CHECK(found->archiveOffset == 12345);
    CHECK(found->encodedSize == 6789);
}

TEST_CASE("Index serialize round-trip", "[casc][index]") {
    IndexTable table;

    for (u32 i = 0; i < 10; ++i) {
        IndexEntry entry;
        entry.eKey[0] = u8(i);
        entry.eKey[1] = u8(i + 1);
        entry.eKey[2] = u8(i + 2);
        entry.archiveIndex = i;
        entry.archiveOffset = i * 1000;
        entry.encodedSize = i * 100 + 50;
        table.insert(entry);
    }

    CHECK(table.entryCount() == 10);

    auto serialized = table.serialize();
    CHECK_FALSE(serialized.empty());
}

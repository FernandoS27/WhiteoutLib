// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_index_test: Validates CASC .idx file parsing against real corpus data.

#include "../src/whiteout/storages/casc/index.h"

#include <filesystem>
#include <iostream>
#include <string>

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

static std::string findCorpusBase() {
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

static void testLoadD3(const std::string& corpus) {
    std::cout << "[Test: Load D3 .idx files]\n";
    auto table = IndexTable::load(corpus + "/Diablo III/Data");
    std::cout << "  D3 entry count: " << table.entryCount() << "\n";
    check(table.entryCount() > 0, "D3 index has entries");
    check(table.entryCount() > 1000, "D3 index has >1000 entries");
}

static void testLoadWC3(const std::string& corpus) {
    std::cout << "[Test: Load WC3 .idx files]\n";
    auto table = IndexTable::load(corpus + "/Warcraft III/Data");
    std::cout << "  WC3 entry count: " << table.entryCount() << "\n";
    check(table.entryCount() > 0, "WC3 index has entries");
    check(table.entryCount() > 1000, "WC3 index has >1000 entries");
}

static void testNotFoundLookup(const std::string& corpus) {
    std::cout << "[Test: Lookup nonexistent EKey]\n";
    auto table = IndexTable::load(corpus + "/Warcraft III/Data");
    std::array<u8, 9> fakeKey{};
    fakeKey.fill(0xFF);
    auto entry = table.find(fakeKey);
    check(entry == nullptr, "Nonexistent EKey returns nullptr");
}

static void testInsertAndFind() {
    std::cout << "[Test: Insert and find]\n";
    IndexTable table;
    IndexEntry entry;
    entry.eKey = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    entry.archiveIndex = 42;
    entry.archiveOffset = 12345;
    entry.encodedSize = 6789;
    table.insert(entry);

    check(table.entryCount() == 1, "Entry count is 1 after insert");

    auto found = table.find(std::span<const u8>(entry.eKey.data(), 9));
    check(found != nullptr, "Inserted entry found");
    if (found) {
        check(found->archiveIndex == 42, "Archive index matches");
        check(found->archiveOffset == 12345, "Archive offset matches");
        check(found->encodedSize == 6789, "Encoded size matches");
    }
}

static void testRoundTrip() {
    std::cout << "[Test: Serialize round-trip]\n";
    IndexTable table;

    // Insert several entries
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

    check(table.entryCount() == 10, "10 entries inserted");

    auto serialized = table.serialize();
    check(!serialized.empty(), "Serialization produces output");
}

int main() {
    std::cout << "=== CASC Index Tests ===\n\n";

    auto corpus = findCorpusBase();
    if (corpus.empty()) {
        std::cout << "WARNING: Corpus not found, skipping corpus-dependent tests.\n";
    }

    if (!corpus.empty()) {
        testLoadD3(corpus);
        testLoadWC3(corpus);
        testNotFoundLookup(corpus);
    }

    testInsertAndFind();
    testRoundTrip();

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}

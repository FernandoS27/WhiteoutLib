// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_encoding_test: Validates CASC ENCODING table parse/serialize round-trip.

#include "../src/whiteout/storages/casc/encoding.h"

#include <cstring>
#include <iostream>
#include <numeric>
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

static EncodingEntry makeEntry(u8 seed) {
    EncodingEntry e;
    for (int i = 0; i < 16; ++i) e.cKey[i] = u8(seed + i);
    for (int i = 0; i < 16; ++i) e.eKey[i] = u8(seed + i + 0x80);
    e.fileSize = u64(seed) * 1000 + 42;
    return e;
}

// ============================================================================
// Tests
// ============================================================================

static void testInsertAndLookupCKey() {
    std::cout << "[Test: Insert and lookup by CKey]\n";
    EncodingTable table;

    auto e1 = makeEntry(0x10);
    auto e2 = makeEntry(0x20);
    auto e3 = makeEntry(0x30);
    table.insert(e1);
    table.insert(e2);
    table.insert(e3);

    check(table.entryCount() == 3, "Entry count is 3");

    auto found = table.findByCKey(e2.cKey);
    check(found != nullptr, "CKey lookup succeeds");
    if (found) {
        check(found->fileSize == e2.fileSize, "File size matches");
        check(found->eKey == e2.eKey, "EKey matches");
    }
}

static void testInsertAndLookupEKey() {
    std::cout << "[Test: Insert and lookup by EKey]\n";
    EncodingTable table;

    auto e1 = makeEntry(0x40);
    table.insert(e1);

    auto found = table.findByEKey(e1.eKey);
    check(found != nullptr, "EKey lookup succeeds");
    if (found) {
        check(found->cKey == e1.cKey, "CKey matches");
    }
}

static void testNotFound() {
    std::cout << "[Test: Lookup not found]\n";
    EncodingTable table;
    table.insert(makeEntry(0x50));

    std::array<u8, 16> fakeCKey{};
    fakeCKey.fill(0xFF);
    check(table.findByCKey(fakeCKey) == nullptr, "CKey not found");

    std::array<u8, 16> fakeEKey{};
    fakeEKey.fill(0xEE);
    check(table.findByEKey(fakeEKey) == nullptr, "EKey not found");
}

static void testSerializeRoundTrip() {
    std::cout << "[Test: Serialize and re-parse round-trip]\n";
    EncodingTable original;

    // Insert 100 entries
    for (u8 i = 0; i < 100; ++i)
        original.insert(makeEntry(i));

    check(original.entryCount() == 100, "Original has 100 entries");

    auto serialized = original.serialize();
    check(!serialized.empty(), "Serialization produces output");

    auto reparsed = EncodingTable::parse(serialized);
    check(reparsed.entryCount() == 100, "Reparsed has 100 entries");

    // Spot-check a few entries.
    for (u8 i : {u8(0), u8(42), u8(99)}) {
        auto expected = makeEntry(i);
        auto found = reparsed.findByCKey(expected.cKey);
        check(found != nullptr, ("Reparsed lookup for seed " + std::to_string(i)).c_str());
        if (found) {
            check(found->eKey == expected.eKey,
                  ("EKey round-trip for seed " + std::to_string(i)).c_str());
            check(found->fileSize == expected.fileSize,
                  ("FileSize round-trip for seed " + std::to_string(i)).c_str());
        }
    }
}

static void testManyEntries() {
    std::cout << "[Test: Scale with many entries]\n";
    EncodingTable table;

    // Insert 10,000 entries.
    for (u32 i = 0; i < 10000; ++i) {
        EncodingEntry e;
        // Use i to generate unique keys.
        std::memcpy(e.cKey.data(), &i, 4);
        e.cKey[4] = 0xCC;
        std::memcpy(e.eKey.data(), &i, 4);
        e.eKey[4] = 0xEE;
        e.fileSize = u64(i) * 137;
        table.insert(e);
    }

    check(table.entryCount() == 10000, "10,000 entries inserted");

    // Lookup a few.
    u32 testId = 5000;
    std::array<u8, 16> searchKey{};
    std::memcpy(searchKey.data(), &testId, 4);
    searchKey[4] = 0xCC;
    auto found = table.findByCKey(searchKey);
    check(found != nullptr, "Lookup in 10k table succeeds");
    if (found)
        check(found->fileSize == 5000u * 137, "File size correct in 10k table");
}

static void testEmptyInput() {
    std::cout << "[Test: Parse empty input]\n";
    auto table = EncodingTable::parse({});
    check(table.entryCount() == 0, "Empty input gives empty table");
}

int main() {
    std::cout << "=== CASC Encoding Tests ===\n\n";

    testInsertAndLookupCKey();
    testInsertAndLookupEKey();
    testNotFound();
    testSerializeRoundTrip();
    testManyEntries();
    testEmptyInput();

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}

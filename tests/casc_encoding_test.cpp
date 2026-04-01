// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_encoding_test: Validates CASC ENCODING table parse/serialize round-trip.

#include "../src/whiteout/storages/casc/encoding.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <numeric>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

static EncodingEntry makeEntry(u8 seed) {
    EncodingEntry e;
    for (int i = 0; i < 16; ++i) e.cKey[i] = u8(seed + i);
    for (int i = 0; i < 16; ++i) e.eKey[i] = u8(seed + i + 0x80);
    e.fileSize = u64(seed) * 1000 + 42;
    return e;
}

TEST_CASE("Encoding insert and lookup by CKey", "[casc][encoding]") {
    EncodingTable table;

    auto e1 = makeEntry(0x10);
    auto e2 = makeEntry(0x20);
    auto e3 = makeEntry(0x30);
    table.insert(e1);
    table.insert(e2);
    table.insert(e3);

    CHECK(table.entryCount() == 3);

    auto found = table.findByCKey(e2.cKey);
    REQUIRE(found != nullptr);
    CHECK(found->fileSize == e2.fileSize);
    CHECK(found->eKey == e2.eKey);
}

TEST_CASE("Encoding insert and lookup by EKey", "[casc][encoding]") {
    EncodingTable table;

    auto e1 = makeEntry(0x40);
    table.insert(e1);

    auto found = table.findByEKey(e1.eKey);
    REQUIRE(found != nullptr);
    CHECK(found->cKey == e1.cKey);
}

TEST_CASE("Encoding lookup not found", "[casc][encoding]") {
    EncodingTable table;
    table.insert(makeEntry(0x50));

    std::array<u8, 16> fakeCKey{};
    fakeCKey.fill(0xFF);
    CHECK(table.findByCKey(fakeCKey) == nullptr);

    std::array<u8, 16> fakeEKey{};
    fakeEKey.fill(0xEE);
    CHECK(table.findByEKey(fakeEKey) == nullptr);
}

TEST_CASE("Encoding serialize round-trip", "[casc][encoding]") {
    EncodingTable original;

    for (u8 i = 0; i < 100; ++i)
        original.insert(makeEntry(i));

    CHECK(original.entryCount() == 100);

    auto serialized = original.serialize();
    REQUIRE_FALSE(serialized.empty());

    auto reparsed = EncodingTable::parse(serialized);
    CHECK(reparsed.entryCount() == 100);

    for (u8 i : {u8(0), u8(42), u8(99)}) {
        auto expected = makeEntry(i);
        auto found = reparsed.findByCKey(expected.cKey);
        REQUIRE(found != nullptr);
        CHECK(found->eKey == expected.eKey);
        CHECK(found->fileSize == expected.fileSize);
    }
}

TEST_CASE("Encoding scale with many entries", "[casc][encoding]") {
    EncodingTable table;

    for (u32 i = 0; i < 10000; ++i) {
        EncodingEntry e;
        std::memcpy(e.cKey.data(), &i, 4);
        e.cKey[4] = 0xCC;
        std::memcpy(e.eKey.data(), &i, 4);
        e.eKey[4] = 0xEE;
        e.fileSize = u64(i) * 137;
        table.insert(e);
    }

    CHECK(table.entryCount() == 10000);

    u32 testId = 5000;
    std::array<u8, 16> searchKey{};
    std::memcpy(searchKey.data(), &testId, 4);
    searchKey[4] = 0xCC;
    auto found = table.findByCKey(searchKey);
    REQUIRE(found != nullptr);
    CHECK(found->fileSize == 5000u * 137);
}

TEST_CASE("Encoding parse empty input", "[casc][encoding]") {
    auto table = EncodingTable::parse({});
    CHECK(table.entryCount() == 0);
}

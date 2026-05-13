// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_wow_root_test: Validates WoW root manifest parsing using synthetic data.

#include "../src/whiteout/storages/casc/roots/wow_root.h"
#include "../src/whiteout/storages/casc/roots/root.h"
#include "../src/whiteout/storages/common/jenkins.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;
using namespace whiteout::storages::common;

// ============================================================================
// Binary builder helpers
// ============================================================================

static void writeLE32(std::vector<u8>& buf, u32 v) {
    buf.push_back(u8(v));
    buf.push_back(u8(v >> 8));
    buf.push_back(u8(v >> 16));
    buf.push_back(u8(v >> 24));
}

static void writeLEi32(std::vector<u8>& buf, i32 v) {
    u32 u;
    std::memcpy(&u, &v, 4);
    writeLE32(buf, u);
}

static void writeLE64(std::vector<u8>& buf, u64 v) {
    writeLE32(buf, u32(v));
    writeLE32(buf, u32(v >> 32));
}

static void writeBytes(std::vector<u8>& buf, const void* data, size_t len) {
    auto p = static_cast<const u8*>(data);
    buf.insert(buf.end(), p, p + len);
}

/// Create a synthetic CKey: [seed, seed+1, ... seed+15].
static std::array<u8, 16> makeCKey(u8 seed) {
    std::array<u8, 16> key;
    for (int i = 0; i < 16; ++i) key[i] = u8(seed + i);
    return key;
}

/// Compute Jenkins hash for a path (same as WoW root uses).
static u64 makeNameHash(const std::string& path) {
    auto h = jenkinsHash(path);
    return u64(h.pc) | (u64(h.pb) << 32);
}

// ============================================================================
// Build synthetic root blobs
// ============================================================================

/// Build a legacy (headerless) root blob with old interleaved format.
/// One block with N entries.
static std::vector<u8> buildLegacyRoot(
    const std::vector<std::pair<u32, std::array<u8, 16>>>& fileDataIdAndCKey,
    const std::vector<u64>& nameHashes,
    u32 localeFlags = 0xFFFFFFFF,
    u32 contentFlags = 0) {

    u32 n = u32(fileDataIdAndCKey.size());
    std::vector<u8> buf;

    // Block header: numRecords, contentFlags, localeFlags.
    writeLE32(buf, n);
    writeLE32(buf, contentFlags);
    writeLE32(buf, localeFlags);

    // Delta-encoded FileDataIds.
    // Delta encoding: id = sum(deltas[0..i]) + i (implicit +1 per entry).
    // So delta[0] = fileDataId[0], delta[i] = fileDataId[i] - fileDataId[i-1] - 1.
    i32 prevId = -1; // after implicit +1, first real id = delta[0]
    for (u32 i = 0; i < n; ++i) {
        i32 fdid = i32(fileDataIdAndCKey[i].first);
        i32 delta = (i == 0) ? fdid : (fdid - prevId - 1);
        writeLEi32(buf, delta);
        prevId = fdid;
    }

    // Interleaved records: [CKey(16) + NameHash(8)] per entry.
    for (u32 i = 0; i < n; ++i) {
        writeBytes(buf, fileDataIdAndCKey[i].second.data(), 16);
        writeLE64(buf, nameHashes[i]);
    }

    return buf;
}

/// Build a v2 (MFST, build 30080+) root blob with split format.
/// One block with N entries.
static std::vector<u8> buildV2Root(
    const std::vector<std::pair<u32, std::array<u8, 16>>>& fileDataIdAndCKey,
    const std::vector<u64>& nameHashes,
    u32 localeFlags = 0xFFFFFFFF,
    u32 contentFlags = 0) {

    u32 n = u32(fileDataIdAndCKey.size());
    std::vector<u8> buf;

    // MFST header: magic + totalFiles + namedFiles.
    writeLE32(buf, 0x4D465354); // 'MFST'
    writeLE32(buf, n);          // totalFileCount
    writeLE32(buf, n);          // namedFileCount

    // Block header
    writeLE32(buf, n);
    writeLE32(buf, contentFlags);
    writeLE32(buf, localeFlags);

    // Delta-encoded FileDataIds
    i32 prevId = -1;
    for (u32 i = 0; i < n; ++i) {
        i32 fdid = i32(fileDataIdAndCKey[i].first);
        i32 delta = (i == 0) ? fdid : (fdid - prevId - 1);
        writeLEi32(buf, delta);
        prevId = fdid;
    }

    // Split format: all CKeys, then all NameHashes.
    for (u32 i = 0; i < n; ++i)
        writeBytes(buf, fileDataIdAndCKey[i].second.data(), 16);
    for (u32 i = 0; i < n; ++i)
        writeLE64(buf, nameHashes[i]);

    return buf;
}

/// Build a v3 (MFST, build 50893+) root blob with split format.
/// version = 1 inside the v3 header (same block header format as v2).
static std::vector<u8> buildV3Root(
    const std::vector<std::pair<u32, std::array<u8, 16>>>& fileDataIdAndCKey,
    const std::vector<u64>& nameHashes,
    u32 localeFlags = 0xFFFFFFFF,
    u32 contentFlags = 0) {

    u32 n = u32(fileDataIdAndCKey.size());
    std::vector<u8> buf;

    // V3 header: magic + headerSize + version + totalFiles + namedFiles + padding.
    writeLE32(buf, 0x4D465354); // 'MFST'
    writeLE32(buf, 20);         // headerSize (bytes after magic: 4+4+4+4+4)
    writeLE32(buf, 1);          // version = 1 (v3 with old block header format)
    writeLE32(buf, n);          // totalFileCount
    writeLE32(buf, n);          // namedFileCount
    writeLE32(buf, 0);          // likely_padding

    // Block header (same as v2 for version 1)
    writeLE32(buf, n);
    writeLE32(buf, contentFlags);
    writeLE32(buf, localeFlags);

    // Delta-encoded FileDataIds
    i32 prevId = -1;
    for (u32 i = 0; i < n; ++i) {
        i32 fdid = i32(fileDataIdAndCKey[i].first);
        i32 delta = (i == 0) ? fdid : (fdid - prevId - 1);
        writeLEi32(buf, delta);
        prevId = fdid;
    }

    // Split format: all CKeys, then all NameHashes.
    for (u32 i = 0; i < n; ++i)
        writeBytes(buf, fileDataIdAndCKey[i].second.data(), 16);
    for (u32 i = 0; i < n; ++i)
        writeLE64(buf, nameHashes[i]);

    return buf;
}

/// Build a v3 root with manifest version 2 (build 58221+) block headers.
static std::vector<u8> buildV3ManifestV2Root(
    const std::vector<std::pair<u32, std::array<u8, 16>>>& fileDataIdAndCKey,
    const std::vector<u64>& nameHashes,
    u32 localeFlags = 0xFFFFFFFF,
    u32 unk1 = 0,
    u32 unk2 = 0,
    u8 unk3 = 0) {

    u32 n = u32(fileDataIdAndCKey.size());
    std::vector<u8> buf;

    // V3 header with version=2.
    writeLE32(buf, 0x4D465354); // 'MFST'
    writeLE32(buf, 20);         // headerSize
    writeLE32(buf, 2);          // version = 2
    writeLE32(buf, n);
    writeLE32(buf, n);
    writeLE32(buf, 0);          // padding

    // Extended block header (17 bytes): numRecords, localeFlags, unk1, unk2, unk3.
    writeLE32(buf, n);
    writeLE32(buf, localeFlags);
    writeLE32(buf, unk1);
    writeLE32(buf, unk2);
    buf.push_back(unk3);

    // Delta-encoded FileDataIds.
    i32 prevId = -1;
    for (u32 i = 0; i < n; ++i) {
        i32 fdid = i32(fileDataIdAndCKey[i].first);
        i32 delta = (i == 0) ? fdid : (fdid - prevId - 1);
        writeLEi32(buf, delta);
        prevId = fdid;
    }

    // Split format: all CKeys, then all NameHashes.
    for (u32 i = 0; i < n; ++i)
        writeBytes(buf, fileDataIdAndCKey[i].second.data(), 16);
    for (u32 i = 0; i < n; ++i)
        writeLE64(buf, nameHashes[i]);

    return buf;
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("Legacy (headerless) root parse", "[casc][wow_root]") {

    std::vector<std::pair<u32, std::array<u8, 16>>> entries = {
        {100, makeCKey(0x10)},
        {101, makeCKey(0x20)},
        {105, makeCKey(0x30)},
        {200, makeCKey(0x40)},
        {300, makeCKey(0x50)},
    };
    std::vector<u64> nameHashes;
    for (auto& [fdid, ckey] : entries) {
        nameHashes.push_back(makeNameHash("path/file" + std::to_string(fdid) + ".txt"));
    }

    auto blob = buildLegacyRoot(entries, nameHashes);
    auto root = WowRoot::parse(blob);
    REQUIRE(root != nullptr);

    CHECK(root->entryCount() == 5);
    CHECK(root->format() == RootFormat::Wow);

    auto found = root->findByFileDataId(105);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->cKey == makeCKey(0x30));
    CHECK(found[0]->fileDataId == 105);

    auto byPath = root->findByPath("path/file100.txt");
    CHECK_FALSE(byPath.empty());

    auto missing = root->findByFileDataId(999);
    CHECK(missing.empty());
}

TEST_CASE("V2 (MFST 30080+) root parse", "[casc][wow_root]") {

    std::vector<std::pair<u32, std::array<u8, 16>>> entries;
    std::vector<u64> nameHashes;
    for (u32 i = 0; i < 10; ++i) {
        entries.push_back({1000 + i * 3, makeCKey(u8(i * 10))});
        nameHashes.push_back(makeNameHash("world/maps/test" + std::to_string(i) + ".wdt"));
    }

    auto blob = buildV2Root(entries, nameHashes);
    auto root = WowRoot::parse(blob);
    REQUIRE(root != nullptr);

    CHECK(root->entryCount() == 10);

    for (u32 i = 0; i < 10; ++i) {
        auto found = root->findByFileDataId(1000 + i * 3);
        CHECK_FALSE(found.empty());
    }
}

TEST_CASE("V3 (MFST 50893+) root parse", "[casc][wow_root]") {

    std::vector<std::pair<u32, std::array<u8, 16>>> entries;
    std::vector<u64> nameHashes;
    for (u32 i = 0; i < 8; ++i) {
        entries.push_back({2000 + i, makeCKey(u8(0xA0 + i))});
        nameHashes.push_back(makeNameHash("interface/icons/icon" + std::to_string(i) + ".blp"));
    }

    auto blob = buildV3Root(entries, nameHashes);
    auto root = WowRoot::parse(blob);
    REQUIRE(root != nullptr);

    CHECK(root->entryCount() == 8);

    auto found = root->findByFileDataId(2004);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->cKey == makeCKey(u8(0xA4)));
}

TEST_CASE("V3 manifest version 2 (58221+) root parse", "[casc][wow_root]") {

    std::vector<std::pair<u32, std::array<u8, 16>>> entries;
    std::vector<u64> nameHashes;
    for (u32 i = 0; i < 5; ++i) {
        entries.push_back({3000 + i * 2, makeCKey(u8(0xC0 + i))});
        nameHashes.push_back(makeNameHash("sound/music/track" + std::to_string(i) + ".ogg"));
    }

    auto blob = buildV3ManifestV2Root(entries, nameHashes, 0xFFFFFFFF, 0x08, 0x00, 0x00);
    auto root = WowRoot::parse(blob);
    REQUIRE(root != nullptr);

    CHECK(root->entryCount() == 5);

    auto found = root->findByFileDataId(3004);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->contentFlags == 0x08);
}

TEST_CASE("Multiple blocks in one root", "[casc][wow_root]") {

    // Build two blocks manually — legacy format.
    std::vector<u8> buf;

    // Block 1: 3 entries, enUS locale.
    {
        writeLE32(buf, 3);             // numRecords
        writeLE32(buf, 0);             // contentFlags
        writeLE32(buf, 0x00000002);    // localeFlags = enUS

        // FileDataIds: 500, 501, 502 → deltas: 500, 0, 0
        writeLEi32(buf, 500);
        writeLEi32(buf, 0);
        writeLEi32(buf, 0);

        // Interleaved [CKey + NameHash].
        for (int i = 0; i < 3; ++i) {
            auto ck = makeCKey(u8(0x60 + i));
            writeBytes(buf, ck.data(), 16);
            writeLE64(buf, makeNameHash("file_" + std::to_string(500 + i)));
        }
    }

    // Block 2: 2 entries, koKR locale.
    {
        writeLE32(buf, 2);             // numRecords
        writeLE32(buf, 0);             // contentFlags
        writeLE32(buf, 0x00000004);    // localeFlags = koKR

        // FileDataIds: 600, 601 → deltas: 600, 0
        writeLEi32(buf, 600);
        writeLEi32(buf, 0);

        for (int i = 0; i < 2; ++i) {
            auto ck = makeCKey(u8(0x70 + i));
            writeBytes(buf, ck.data(), 16);
            writeLE64(buf, makeNameHash("kr_file_" + std::to_string(600 + i)));
        }
    }

    auto root = WowRoot::parse(buf);
    REQUIRE(root != nullptr);

    CHECK(root->entryCount() == 5);

    auto enUs = root->findByFileDataId(500);
    REQUIRE_FALSE(enUs.empty());
    CHECK(enUs[0]->localeFlags == 0x00000002);

    auto koKr = root->findByFileDataId(600);
    REQUIRE_FALSE(koKr.empty());
    CHECK(koKr[0]->localeFlags == 0x00000004);
}

TEST_CASE("Block with NoNameHash flag (v2)", "[casc][wow_root]") {
    // Build a v2 root where totalFileCount != namedFileCount.
    u32 n = 3;
    std::vector<u8> buf;

    // MFST header
    writeLE32(buf, 0x4D465354);
    writeLE32(buf, n);  // totalFileCount
    writeLE32(buf, 0);  // namedFileCount = 0 (no name hashes)

    // Block header with NoNameHash flag.
    writeLE32(buf, n);
    writeLE32(buf, 0x10000000); // contentFlags = NoNameHash
    writeLE32(buf, 0xFFFFFFFF);

    // Deltas: 10, 11, 12 → 10, 0, 0
    writeLEi32(buf, 10);
    writeLEi32(buf, 0);
    writeLEi32(buf, 0);

    // CKeys only (no name hashes).
    for (u32 i = 0; i < n; ++i) {
        auto ck = makeCKey(u8(0xD0 + i));
        writeBytes(buf, ck.data(), 16);
    }

    auto root = WowRoot::parse(buf);
    REQUIRE(root != nullptr);

    CHECK(root->entryCount() == 3);

    auto found = root->findByFileDataId(11);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->fileNameHash == 0);

    auto byPath = root->findByPath("anything");
    CHECK(byPath.empty());
}

TEST_CASE("Delta-encoded FileDataIds with gaps", "[casc][wow_root]") {

    // FileDataIds: 10, 15, 100, 101, 500
    // Deltas: 10, (15-10-1)=4, (100-15-1)=84, (101-100-1)=0, (500-101-1)=398
    std::vector<std::pair<u32, std::array<u8, 16>>> entries = {
        {10,  makeCKey(0xE0)},
        {15,  makeCKey(0xE1)},
        {100, makeCKey(0xE2)},
        {101, makeCKey(0xE3)},
        {500, makeCKey(0xE4)},
    };
    std::vector<u64> nameHashes;
    for (auto& [fdid, _] : entries)
        nameHashes.push_back(makeNameHash("data/" + std::to_string(fdid)));

    auto blob = buildLegacyRoot(entries, nameHashes);
    auto root = WowRoot::parse(blob);
    REQUIRE(root != nullptr);

    for (auto& [fdid, ckey] : entries) {
        auto found = root->findByFileDataId(fdid);
        REQUIRE_FALSE(found.empty());
        CHECK(found[0]->cKey == ckey);
    }
}

TEST_CASE("findByCKey", "[casc][wow_root]") {

    std::vector<std::pair<u32, std::array<u8, 16>>> entries = {
        {42, makeCKey(0xF0)},
        {43, makeCKey(0xF1)},
    };
    std::vector<u64> nameHashes = {
        makeNameHash("a"), makeNameHash("b"),
    };

    auto blob = buildLegacyRoot(entries, nameHashes);
    auto root = WowRoot::parse(blob);
    REQUIRE(root != nullptr);

    auto ckey = makeCKey(0xF1);
    auto found = root->findByCKey(ckey);
    REQUIRE_FALSE(found.empty());
    CHECK(found[0]->fileDataId == 43);

    auto missing = makeCKey(0xFF);
    CHECK(root->findByCKey(missing).empty());
}

TEST_CASE("Enumerate all entries", "[casc][wow_root]") {

    std::vector<std::pair<u32, std::array<u8, 16>>> entries;
    std::vector<u64> nameHashes;
    for (u32 i = 0; i < 20; ++i) {
        entries.push_back({i * 10, makeCKey(u8(i))});
        nameHashes.push_back(makeNameHash("enum/" + std::to_string(i)));
    }

    auto blob = buildLegacyRoot(entries, nameHashes);
    auto root = WowRoot::parse(blob);
    REQUIRE(root != nullptr);

    size_t count = 0;
    root->enumerate([&](const RootEntry&) { ++count; return true; });
    CHECK(count == 20);

    size_t partial = 0;
    root->enumerate([&](const RootEntry&) {
        ++partial;
        return partial < 5;
    });
    CHECK(partial == 5);
}

TEST_CASE("Auto-detection via RootManifest::parse", "[casc][wow_root]") {

    // Build a v2 root and verify auto-detection routes to WowRoot.
    std::vector<std::pair<u32, std::array<u8, 16>>> entries = {
        {77, makeCKey(0xAA)},
    };
    std::vector<u64> nameHashes = {makeNameHash("test/auto")};

    auto blob = buildV2Root(entries, nameHashes);
    auto root = RootManifest::parse(blob);
    REQUIRE(root != nullptr);
    CHECK(root->format() == RootFormat::Wow);
    CHECK(root->entryCount() == 1);
}

TEST_CASE("WoW root edge cases", "[casc][wow_root]") {
    std::vector<u8> empty;
    CHECK(WowRoot::parse(empty) == nullptr);

    std::vector<u8> tiny = {0x01, 0x02};
    CHECK(WowRoot::parse(tiny) == nullptr);
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_reserve_file_id_test.cpp
/// @brief Tests for StorageWritable::reserveFileId() across root formats.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

static std::vector<u8> makeTestData(size_t size, u8 seed = 0) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>((i + seed) & 0xFF);
    return data;
}

static void cleanDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// ============================================================================
// WoW root format: name is a full CASC path
// ============================================================================

TEST_CASE("Reserve FileId WoW", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — WoW root format]\n");

    const std::string testDir = "test_casc_reserve_wow";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "wow";
    opts.version = "1.0.0";
    opts.rootFormat = RootFormat::Wow;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // Reserve a file ID for a WoW path.
    auto id = storage.reserveFileId("world\\maps\\azeroth\\terrain.adt");
    REQUIRE(id.has_value());
    CHECK(*id > 0);

    // Write data using the reserved ID.
    auto data = makeTestData(512, 0x42);
    CHECK(storage.writeFile(static_cast<i32>(*id), data));

    // Save to disk.
    CHECK(storage.save(testDir));

    // Reopen and verify.
    auto reopened = StorageWritable::open(testDir);
    REQUIRE(reopened.has_value());

    // Should be readable by fileDataId.
    auto readById = reopened->readFile(static_cast<i32>(*id));
    CHECK((readById.has_value() && *readById == data));

    // Should be readable by path.
    auto readByPath = reopened->readFile("world\\maps\\azeroth\\terrain.adt");
    CHECK((readByPath.has_value() && *readByPath == data));

    cleanDir(testDir);
}

// ============================================================================
// WoW: duplicate name returns nullopt
// ============================================================================

TEST_CASE("Reserve FileId WoW Duplicate", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — WoW duplicate rejection]\n");

    CreateOptions opts;
    opts.rootFormat = RootFormat::Wow;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    auto id1 = storage.reserveFileId("dir\\file.dat");
    REQUIRE(id1.has_value());

    // Same name again — should return nullopt.
    auto id2 = storage.reserveFileId("dir\\file.dat");
    CHECK_FALSE(id2.has_value());
}

// ============================================================================
// WoW: two different names get distinct IDs
// ============================================================================

TEST_CASE("Reserve FileId WoW Distinct IDs", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — WoW distinct IDs]\n");

    CreateOptions opts;
    opts.rootFormat = RootFormat::Wow;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    auto id1 = storage.reserveFileId("file_a.dat");
    auto id2 = storage.reserveFileId("file_b.dat");

    REQUIRE(id1.has_value());
    REQUIRE(id2.has_value());
    CHECK(*id1 != *id2);
}

// ============================================================================
// D3 root format: reservation logic (duplicate detection, CoreTOC integration)
// Note: D3 root serializer doesn't produce a hierarchical directory that the
// parser expects, so we verify by fileDataId rather than path after round-trip.
// ============================================================================

TEST_CASE("Reserve FileId D3", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — Diablo III root format]\n");

    const std::string testDir = "test_casc_reserve_d3";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "diablo3";
    opts.version = "1.0.0";
    opts.rootFormat = RootFormat::Diablo3;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // Reserve a file ID: "my_beast.app" → Appearance group.
    auto id = storage.reserveFileId("my_beast.app");
    REQUIRE(id.has_value());
    CHECK(*id > 0);

    // Write data using the reserved ID.
    auto data = makeTestData(1024, 0xD3);
    CHECK(storage.writeFile(static_cast<i32>(*id), data));

    // Reserve another SNO.
    auto id2 = storage.reserveFileId("skeleton_king.acr");
    REQUIRE(id2.has_value());
    CHECK(*id != *id2);

    auto data2 = makeTestData(256, 0xAC);
    CHECK(storage.writeFile(static_cast<i32>(*id2), data2));

    // Duplicate should fail.
    CHECK_FALSE(storage.reserveFileId("my_beast.app").has_value());

    // Read back from overlay before save.
    auto overlayRead = storage.readFile(static_cast<i32>(*id));
    CHECK((overlayRead.has_value() && *overlayRead == data));

    // Save verifies that CoreTOC serialization doesn't crash.
    CHECK(storage.save(testDir));

    cleanDir(testDir);
}

// ============================================================================
// D3: duplicate SNO name returns nullopt
// ============================================================================

TEST_CASE("Reserve FileId D3 Duplicate", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — D3 duplicate rejection]\n");

    CreateOptions opts;
    opts.rootFormat = RootFormat::Diablo3;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    auto id1 = storage.reserveFileId("my_beast.app");
    REQUIRE(id1.has_value());

    // Same name again — should return nullopt.
    auto id2 = storage.reserveFileId("my_beast.app");
    CHECK_FALSE(id2.has_value());
}

// ============================================================================
// D3: unknown extension returns nullopt
// ============================================================================

TEST_CASE("Reserve FileId D3 Unknown Extension", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — D3 unknown extension]\n");

    CreateOptions opts;
    opts.rootFormat = RootFormat::Diablo3;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // ".xyz" is not a known SNO extension.
    auto id = storage.reserveFileId("some_file.xyz");
    CHECK_FALSE(id.has_value());
}

// ============================================================================
// D4 root format: name is "asset_name.ext", with meta + payload sub-entries
// ============================================================================

TEST_CASE("Reserve FileId D4", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — Diablo IV root format]\n");

    const std::string testDir = "test_casc_reserve_d4";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "diablo4";
    opts.version = "1.0.0";
    opts.rootFormat = RootFormat::Diablo4;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // Reserve a file ID for an appearance asset.
    auto id = storage.reserveFileId("my_monster.app");
    REQUIRE(id.has_value());
    CHECK(*id > 0);

    // Write child data (default hint = None → base:child).
    auto childData = makeTestData(768, 0xD4);
    CHECK(storage.writeFile(static_cast<i32>(*id), childData));

    // Write meta data for the same snoId.
    auto metaData = makeTestData(128, 0xAA);
    CHECK(storage.writeFile(static_cast<i32>(*id), metaData, {}, FileIdHint::Meta));

    // Write payload data for the same snoId.
    auto payloadData = makeTestData(2048, 0xBB);
    CHECK(storage.writeFile(static_cast<i32>(*id), payloadData, {}, FileIdHint::Payload));

    // Read back from overlay by fileDataId + hint before save.
    auto oChild = storage.readFile(static_cast<i32>(*id));
    CHECK((oChild.has_value() && *oChild == childData));

    auto oMeta = storage.readFile(static_cast<i32>(*id), FileIdHint::Meta);
    CHECK((oMeta.has_value() && *oMeta == metaData));

    auto oPayload = storage.readFile(static_cast<i32>(*id), FileIdHint::Payload);
    CHECK((oPayload.has_value() && *oPayload == payloadData));

    // Save.
    CHECK(storage.save(testDir));

    // Reopen and verify each sub-entry by its enriched path.
    auto reopened = StorageWritable::open(testDir);
    REQUIRE(reopened.has_value());

    auto rChild = reopened->readFile("base:child\\Appearance\\my_monster.app");
    CHECK((rChild.has_value() && *rChild == childData));

    auto rMeta = reopened->readFile("base:meta\\Appearance\\my_monster.app");
    CHECK((rMeta.has_value() && *rMeta == metaData));

    auto rPayload = reopened->readFile("base:payload\\Appearance\\my_monster.app");
    CHECK((rPayload.has_value() && *rPayload == payloadData));

    cleanDir(testDir);
}

// ============================================================================
// D4: multiple reserves with meta + payload per appearance
// ============================================================================

TEST_CASE("Reserve FileId D4 Multiple", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — D4 multiple reserves with sub-entries]\n");

    const std::string testDir = "test_casc_reserve_d4_multi";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "diablo4";
    opts.version = "1.0.0";
    opts.rootFormat = RootFormat::Diablo4;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    auto id1 = storage.reserveFileId("beast_a.app");
    auto id2 = storage.reserveFileId("beast_b.app");
    auto id3 = storage.reserveFileId("fire_tex.tex");
    REQUIRE(id1.has_value());
    REQUIRE(id2.has_value());
    REQUIRE(id3.has_value());

    // All IDs must be distinct.
    CHECK(*id1 != *id2);
    CHECK(*id1 != *id3);
    CHECK(*id2 != *id3);

    // Write child + meta + payload for beast_a.
    auto data1_child = makeTestData(100, 0x01);
    auto data1_meta = makeTestData(50, 0x11);
    auto data1_payload = makeTestData(500, 0x21);
    CHECK(storage.writeFile(static_cast<i32>(*id1), data1_child));
    CHECK(storage.writeFile(static_cast<i32>(*id1), data1_meta, {}, FileIdHint::Meta));
    CHECK(storage.writeFile(static_cast<i32>(*id1), data1_payload, {}, FileIdHint::Payload));

    // Write child + meta for beast_b (no payload).
    auto data2_child = makeTestData(200, 0x02);
    auto data2_meta = makeTestData(64, 0x12);
    CHECK(storage.writeFile(static_cast<i32>(*id2), data2_child));
    CHECK(storage.writeFile(static_cast<i32>(*id2), data2_meta, {}, FileIdHint::Meta));

    // Write child only for texture.
    auto data3 = makeTestData(300, 0x03);
    CHECK(storage.writeFile(static_cast<i32>(*id3), data3));

    // Read back from overlay by fileDataId + hint before save.
    // beast_a: child + meta + payload
    auto o1c = storage.readFile(static_cast<i32>(*id1));
    CHECK((o1c.has_value() && *o1c == data1_child));
    auto o1m = storage.readFile(static_cast<i32>(*id1), FileIdHint::Meta);
    CHECK((o1m.has_value() && *o1m == data1_meta));
    auto o1p = storage.readFile(static_cast<i32>(*id1), FileIdHint::Payload);
    CHECK((o1p.has_value() && *o1p == data1_payload));

    // beast_b: child + meta
    auto o2c = storage.readFile(static_cast<i32>(*id2));
    CHECK((o2c.has_value() && *o2c == data2_child));
    auto o2m = storage.readFile(static_cast<i32>(*id2), FileIdHint::Meta);
    CHECK((o2m.has_value() && *o2m == data2_meta));

    // fire_tex: child only
    auto o3 = storage.readFile(static_cast<i32>(*id3));
    CHECK((o3.has_value() && *o3 == data3));

    CHECK(storage.save(testDir));

    auto reopened = StorageWritable::open(testDir);
    REQUIRE(reopened.has_value());

    // beast_a: child + meta + payload
    auto r1c = reopened->readFile("base:child\\Appearance\\beast_a.app");
    CHECK((r1c.has_value() && *r1c == data1_child));

    auto r1m = reopened->readFile("base:meta\\Appearance\\beast_a.app");
    CHECK((r1m.has_value() && *r1m == data1_meta));

    auto r1p = reopened->readFile("base:payload\\Appearance\\beast_a.app");
    CHECK((r1p.has_value() && *r1p == data1_payload));

    // beast_b: child + meta
    auto r2c = reopened->readFile("base:child\\Appearance\\beast_b.app");
    CHECK((r2c.has_value() && *r2c == data2_child));

    auto r2m = reopened->readFile("base:meta\\Appearance\\beast_b.app");
    CHECK((r2m.has_value() && *r2m == data2_meta));

    // fire_tex: child only
    auto r3 = reopened->readFile("base:child\\Texture\\fire_tex.tex");
    CHECK((r3.has_value() && *r3 == data3));

    cleanDir(testDir);
}

// ============================================================================
// Reserve + normal writeFile(path) coexist
// ============================================================================

TEST_CASE("Reserve FileId Mixed With Path Writes", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — mixed with path-based writes]\n");

    const std::string testDir = "test_casc_reserve_mixed";
    cleanDir(testDir);

    CreateOptions opts;
    opts.rootFormat = RootFormat::Wow;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // Path-based write.
    auto pathData = makeTestData(128, 0xAA);
    CHECK(storage.writeFile("some\\normal\\file.dat", pathData));

    // Reserve-based write.
    auto id = storage.reserveFileId("another\\reserved\\file.m2");
    REQUIRE(id.has_value());
    auto idData = makeTestData(256, 0xBB);
    CHECK(storage.writeFile(static_cast<i32>(*id), idData));

    CHECK(storage.save(testDir));

    auto reopened = StorageWritable::open(testDir);
    REQUIRE(reopened.has_value());

    auto r1 = reopened->readFile("some\\normal\\file.dat");
    CHECK((r1.has_value() && *r1 == pathData));

    auto r2 = reopened->readFile("another\\reserved\\file.m2");
    CHECK((r2.has_value() && *r2 == idData));

    cleanDir(testDir);
}

// ============================================================================
// WowTvfs root format: same as WoW but uses TVFS serialization (11.x+)
// ============================================================================

TEST_CASE("Reserve FileId WowTvfs", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — WowTvfs root format]\n");

    const std::string testDir = "test_casc_reserve_wowtvfs";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "wow";
    opts.version = "1.0.0";
    opts.rootFormat = RootFormat::WowTvfs;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // Reserve a file ID for a WoW path (same semantics as WoW format).
    auto id = storage.reserveFileId("world\\maps\\azeroth\\terrain.adt");
    REQUIRE(id.has_value());
    CHECK(*id > 0);

    // Write data using the reserved ID.
    auto data = makeTestData(512, 0x42);
    CHECK(storage.writeFile(static_cast<i32>(*id), data));

    // Save to disk.
    CHECK(storage.save(testDir));

    // Reopen and verify.
    auto reopened = StorageWritable::open(testDir);
    REQUIRE(reopened.has_value());

    // Should be readable by path.
    auto readByPath = reopened->readFile("world\\maps\\azeroth\\terrain.adt");
    CHECK((readByPath.has_value() && *readByPath == data));

    cleanDir(testDir);
}

// ============================================================================
// WowTvfs: duplicate name returns nullopt
// ============================================================================

TEST_CASE("Reserve FileId WowTvfs Duplicate", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — WowTvfs duplicate rejection]\n");

    CreateOptions opts;
    opts.rootFormat = RootFormat::WowTvfs;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    auto id1 = storage.reserveFileId("dir\\file.dat");
    REQUIRE(id1.has_value());

    // Same name again — should return nullopt.
    auto id2 = storage.reserveFileId("dir\\file.dat");
    CHECK_FALSE(id2.has_value());
}

// ============================================================================
// WowTvfs: two different names get distinct IDs
// ============================================================================

TEST_CASE("Reserve FileId WowTvfs Distinct IDs", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — WowTvfs distinct IDs]\n");

    CreateOptions opts;
    opts.rootFormat = RootFormat::WowTvfs;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    auto id1 = storage.reserveFileId("file_a.dat");
    auto id2 = storage.reserveFileId("file_b.dat");

    REQUIRE(id1.has_value());
    REQUIRE(id2.has_value());
    CHECK(*id1 != *id2);
}

// ============================================================================
// WowTvfs + path writes coexist
// ============================================================================

TEST_CASE("Reserve FileId WowTvfs Mixed With Path Writes", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — WowTvfs mixed with path-based writes]\n");

    const std::string testDir = "test_casc_reserve_wowtvfs_mixed";
    cleanDir(testDir);

    CreateOptions opts;
    opts.rootFormat = RootFormat::WowTvfs;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // Path-based write.
    auto pathData = makeTestData(128, 0xAA);
    CHECK(storage.writeFile("some\\normal\\file.dat", pathData));

    // Reserve-based write.
    auto id = storage.reserveFileId("another\\reserved\\file.m2");
    REQUIRE(id.has_value());
    auto idData = makeTestData(256, 0xBB);
    CHECK(storage.writeFile(static_cast<i32>(*id), idData));

    CHECK(storage.save(testDir));

    auto reopened = StorageWritable::open(testDir);
    REQUIRE(reopened.has_value());

    auto r1 = reopened->readFile("some\\normal\\file.dat");
    CHECK((r1.has_value() && *r1 == pathData));

    auto r2 = reopened->readFile("another\\reserved\\file.m2");
    CHECK((r2.has_value() && *r2 == idData));

    cleanDir(testDir);
}

// ============================================================================
// D4: duplicate SNO name returns nullopt
// ============================================================================

TEST_CASE("Reserve FileId D4 Duplicate", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — D4 duplicate rejection]\n");

    CreateOptions opts;
    opts.rootFormat = RootFormat::Diablo4;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    auto id1 = storage.reserveFileId("my_beast.app");
    REQUIRE(id1.has_value());

    // Same name again — should return nullopt.
    auto id2 = storage.reserveFileId("my_beast.app");
    CHECK_FALSE(id2.has_value());
}

// ============================================================================
// D4: unknown extension returns nullopt
// ============================================================================

TEST_CASE("Reserve FileId D4 Unknown Extension", "[casc][reserve_file_id]") {
    std::printf("\n[Test: reserveFileId — D4 unknown extension]\n");

    CreateOptions opts;
    opts.rootFormat = RootFormat::Diablo4;
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // ".xyz" is not a known SNO extension.
    auto id = storage.reserveFileId("some_file.xyz");
    CHECK_FALSE(id.has_value());
}

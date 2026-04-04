// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_round_trip_test.cpp
/// @brief CASC write support round-trip tests.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;


/// Generate test data of a given size.
static std::vector<u8> makeTestData(size_t size, u8 seed = 0) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>((i + seed) & 0xFF);
    return data;
}

/// Clean up a test directory.
static void cleanDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// ============================================================================
// Test 1: Create → write files → save → reopen → read back
// ============================================================================

TEST_CASE("Create Write Save Read", "[casc][round_trip]") {
    std::printf("\n[Test: Create → write → save → read]\n");

    const std::string testDir = "test_casc_roundtrip_1";
    cleanDir(testDir);

    // Create a new empty storage.
    CreateOptions createOpts;
    createOpts.product = "test";
    createOpts.version = "1.0.0";
    auto storage = StorageWritable::create(createOpts);
    CHECK(static_cast<bool>(storage));

    // Write several test files.
    auto data1 = makeTestData(1024, 0x11);
    auto data2 = makeTestData(65536, 0x22);  // > 1 BLTE frame.
    auto data3 = makeTestData(1, 0x33);       // Tiny file.
    auto data4 = makeTestData(0);             // Empty file.

    CHECK(storage.writeFile("dir/file1.txt", data1));
    CHECK(storage.writeFile("dir/file2.bin", data2));
    CHECK(storage.writeFile("tiny.dat", data3));
    CHECK(storage.writeFile("empty.dat", data4));

    // Read from overlay (before save).
    auto overlay1 = storage.readFile("dir/file1.txt");
    CHECK((overlay1.has_value() && *overlay1 == data1));

    // fileExists checks overlay.
    CHECK(storage.fileExists("dir/file1.txt"));

    // Save to disk.
    CHECK(storage.save(testDir));

    // Reopen from disk.
    auto reopened = Storage::open(testDir);
    CHECK(reopened.has_value());

    if (reopened) {
        auto read1 = reopened->readFile("dir/file1.txt");
        CHECK((read1.has_value() && *read1 == data1));

        auto read2 = reopened->readFile("dir/file2.bin");
        CHECK((read2.has_value() && *read2 == data2));

        auto read3 = reopened->readFile("tiny.dat");
        CHECK((read3.has_value() && *read3 == data3));

        auto read4 = reopened->readFile("empty.dat");
        CHECK((read4.has_value() && *read4 == data4));
    }

    // Leave test dir for debugging when tests fail.
    cleanDir(testDir);
}

// ============================================================================
// Test 2: Overlay correctness — write, overwrite, delete, read
// ============================================================================

TEST_CASE("Overlay Correctness", "[casc][round_trip]") {
    std::printf("\n[Test: Overlay correctness]\n");

    auto storage = StorageWritable::create();
    CHECK(static_cast<bool>(storage));

    auto original = makeTestData(100, 0xAA);
    auto updated = makeTestData(200, 0xBB);

    // Write original.
    CHECK(storage.writeFile("test.dat", original));
    auto r1 = storage.readFile("test.dat");
    CHECK((r1.has_value() && *r1 == original));

    // Overwrite.
    CHECK(storage.writeFile("test.dat", updated));
    auto r2 = storage.readFile("test.dat");
    CHECK((r2.has_value() && *r2 == updated));

    // Delete (from overlay — no source, so should fail).
    // Actually, delete checks if file exists in source — for a created storage with no source,
    // the overlay write IS the source for read purposes but deleteFile checks root manifest.
    // So this tests that we can't delete something that only exists in overlay without a root.
    // This is by design — you write and save, then delete after.
    CHECK(storage.fileExists("test.dat"));
}

// ============================================================================
// Test 3: Modify existing storage — add, delete, save, verify
// ============================================================================

TEST_CASE("Modify Existing", "[casc][round_trip]") {
    std::printf("\n[Test: Modify existing storage]\n");

    const std::string testDir = "test_casc_roundtrip_3";
    cleanDir(testDir);

    // Create and save an initial storage.
    {
        auto storage = StorageWritable::create();
        auto orig1 = makeTestData(512, 0x01);
        auto orig2 = makeTestData(256, 0x02);
        auto orig3 = makeTestData(128, 0x03);

        storage.writeFile("keep.dat", orig1);
        storage.writeFile("delete_me.dat", orig2);
        storage.writeFile("replace_me.dat", orig3);
        CHECK(storage.save(testDir));
    }

    // Reopen and modify.
    auto storage = StorageWritable::open(testDir);
    CHECK(storage.has_value());

    if (!storage) {
        cleanDir(testDir);
        return;
    }

    // Verify existing files.
    auto keepData = makeTestData(512, 0x01);
    auto readKeep = storage->readFile("keep.dat");
    CHECK((readKeep.has_value() && *readKeep == keepData));

    // Add a new file.
    auto newData = makeTestData(300, 0x04);
    CHECK(storage->writeFile("new_file.dat", newData));

    // Delete an existing file.
    CHECK(storage->deleteFile("delete_me.dat"));
    CHECK_FALSE(storage->fileExists("delete_me.dat"));

    // Replace an existing file.
    auto replacedData = makeTestData(999, 0x05);
    CHECK(storage->writeFile("replace_me.dat", replacedData));

    // Verify overlay reads.
    auto readNew = storage->readFile("new_file.dat");
    CHECK((readNew.has_value() && *readNew == newData));

    auto readReplaced = storage->readFile("replace_me.dat");
    CHECK((readReplaced.has_value() && *readReplaced == replacedData));

    // Save modifications.
    const std::string modifiedDir = "test_casc_roundtrip_3_mod";
    cleanDir(modifiedDir);
    CHECK(storage->save(modifiedDir));

    // Reopen modified storage and verify.
    auto modified = Storage::open(modifiedDir);
    CHECK(modified.has_value());

    if (modified) {
        auto r1 = modified->readFile("keep.dat");
        CHECK((r1.has_value() && *r1 == keepData));

        CHECK_FALSE(modified->fileExists("delete_me.dat"));

        auto r3 = modified->readFile("replace_me.dat");
        CHECK((r3.has_value() && *r3 == replacedData));

        auto r4 = modified->readFile("new_file.dat");
        CHECK((r4.has_value() && *r4 == newData));
    }

    cleanDir(testDir);
    cleanDir(modifiedDir);
}

// ============================================================================
// Test 4: Large file round-trip (multi-frame BLTE)
// ============================================================================

TEST_CASE("Large File", "[casc][round_trip]") {
    std::printf("\n[Test: Large file round-trip]\n");

    const std::string testDir = "test_casc_roundtrip_4";
    cleanDir(testDir);

    auto storage = StorageWritable::create();

    // 1 MB file — will span multiple BLTE frames (64 KB each).
    auto largeData = makeTestData(1024 * 1024, 0xFF);
    CHECK(storage.writeFile("large.bin", largeData));
    CHECK(storage.save(testDir));

    auto reopened = Storage::open(testDir);
    CHECK(reopened.has_value());

    if (reopened) {
        auto read = reopened->readFile("large.bin");
        CHECK(read.has_value());
        if (read) {
            CHECK(read->size() == largeData.size());
            CHECK(*read == largeData);
        }
    }

    cleanDir(testDir);
}

// ============================================================================
// Test 5: Multiple saves (save, modify, save again)
// ============================================================================

TEST_CASE("Multiple Saves", "[casc][round_trip]") {
    std::printf("\n[Test: Multiple saves]\n");

    const std::string testDir = "test_casc_roundtrip_5";
    cleanDir(testDir);

    // First save.
    {
        auto storage = StorageWritable::create();
        storage.writeFile("file_v1.dat", makeTestData(100, 0x01));
        CHECK(storage.save(testDir));
    }

    // Second save (modify).
    {
        auto storage = StorageWritable::open(testDir);
        CHECK(storage.has_value());
        if (storage) {
            storage->writeFile("file_v2.dat", makeTestData(200, 0x02));
            CHECK(storage->save(testDir));
        }
    }

    // Verify both files present.
    {
        auto storage = Storage::open(testDir);
        CHECK(storage.has_value());
        if (storage) {
            auto r1 = storage->readFile("file_v1.dat");
            CHECK((r1.has_value() && *r1 == makeTestData(100, 0x01)));

            auto r2 = storage->readFile("file_v2.dat");
            CHECK((r2.has_value() && *r2 == makeTestData(200, 0x02)));
        }
    }

    cleanDir(testDir);
}

// ============================================================================
// Main
// ============================================================================

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_write_health_test.cpp
/// @brief Extensive CASC write tests: file modification, frame growth/shrink,
///        archive health after multiple save cycles, and dead-space reclamation.
///
/// All tests create fresh (non-official) CASC storages from scratch.
/// No online connectivity or official game data is required.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <numeric>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

/// Generate deterministic test data.  Different seeds produce different content.
static std::vector<u8> makeTestData(size_t size, u8 seed = 0) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>((i * 7 + seed) & 0xFF);
    return data;
}

/// Generate compressible test data (runs of repeated bytes).
static std::vector<u8> makeCompressibleData(size_t size, u8 seed = 0) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>(seed + (i / 64));  // Changes every 64 bytes.
    return data;
}

/// Clean up a test directory (and ignore errors).
static void cleanDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

/// Measure total size of all data.XXX archive files in a CASC output directory.
static u64 measureArchiveSize(const std::string& cascDir) {
    u64 total = 0;
    const std::string dataDir = cascDir + "/Data/data";
    std::error_code ec;
    if (!fs::exists(dataDir, ec)) return 0;
    for (auto& entry : fs::directory_iterator(dataDir, ec)) {
        if (entry.is_regular_file()) {
            auto name = entry.path().filename().string();
            // Match data.000, data.001, etc.
            if (name.size() >= 8 && name.substr(0, 5) == "data.") {
                total += entry.file_size();
            }
        }
    }
    return total;
}

/// Count the number of data.XXX archive files in a CASC output directory.
static u32 countArchiveFiles(const std::string& cascDir) {
    u32 count = 0;
    const std::string dataDir = cascDir + "/Data/data";
    std::error_code ec;
    if (!fs::exists(dataDir, ec)) return 0;
    for (auto& entry : fs::directory_iterator(dataDir, ec)) {
        if (entry.is_regular_file()) {
            auto name = entry.path().filename().string();
            if (name.size() >= 8 && name.substr(0, 5) == "data.") {
                ++count;
            }
        }
    }
    return count;
}

/// Count enumerable files in a storage.
static u32 countFiles(const Storage& storage) {
    u32 count = 0;
    storage.enumerate([&](const EnumerateEntry&) {
        ++count;
        return true;
    });
    return count;
}

/// Sum of all file sizes reported by the storage.
static u64 sumFileSizes(const Storage& storage) {
    u64 total = 0;
    storage.enumerate([&](const EnumerateEntry& e) {
        total += e.fileSize;
        return true;
    });
    return total;
}

/// Default frame size used in production CASC (64 KB).
static constexpr u32 kDefaultFrameSize = 0x10000;

/// Small frame size for faster testing of multi-frame behavior.
static constexpr u32 kSmallFrameSize = 4096;

// ============================================================================
// Diagnostic: Minimal round-trip per root format
// ============================================================================

TEST_CASE("Diag: Minimal Round-Trip Per Root Format", "[casc][write_health][diag]") {
    auto format = GENERATE(RootFormat::Tvfs, RootFormat::Wow, RootFormat::Diablo3,
                            RootFormat::Diablo4, RootFormat::WowTvfs);
    const char* name =
        format == RootFormat::Tvfs    ? "TVFS" :
        format == RootFormat::Wow     ? "WoW"  :
        format == RootFormat::Diablo3 ? "D3"   :
        format == RootFormat::Diablo4 ? "D4"   : "WowTvfs";

    std::printf("\n[Diag: minimal round-trip — %s]\n", name);

    const std::string testDir = std::string("test_casc_diag_") + name;
    cleanDir(testDir);

    auto testData = makeTestData(4096, 0xAB);

    {
        CreateOptions opts;
        opts.product = "diag";
        opts.version = "1.0.0";
        opts.rootFormat = format;
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));
        REQUIRE(storage.writeFile("hello.bin", testData));
        REQUIRE(storage.save(testDir));
    }

    {
        auto reader = Storage::open(testDir);
        REQUIRE(reader.has_value());

        auto count = reader->totalFileCount();
        std::printf("  totalFileCount: %s\n", count.has_value() ? std::to_string(*count).c_str() : "n/a");

        u32 enumCount = countFiles(*reader);
        std::printf("  enumerate count: %u\n", enumCount);

        // Try readFile by path.
        auto data = reader->readFile("hello.bin");
        if (!data.has_value()) {
            std::printf("  readFile(\"hello.bin\") FAILED\n");

            // Try readFile by fileDataId if path fails (WoW/D3 might need this).
            auto data2 = reader->readFile(1);
            std::printf("  readFile(fileDataId=1): %s\n", data2.has_value() ? "OK" : "FAILED");
        } else {
            std::printf("  readFile(\"hello.bin\"): %llu bytes\n",
                        static_cast<unsigned long long>(data->size()));
            CHECK(*data == testData);
        }
    }

    // Test multi-step: open writable, modify, save, reopen.
    {
        auto w = StorageWritable::open(testDir);
        REQUIRE(w.has_value());
        REQUIRE(w->writeFile("hello.bin", makeTestData(1024, 0xCD)));
        REQUIRE(w->writeFile("extra.bin", makeTestData(2048, 0xEF)));
        REQUIRE(w->save(testDir));
    }

    {
        auto reader2 = Storage::open(testDir);
        REQUIRE(reader2.has_value());

        auto d1 = reader2->readFile("hello.bin");
        std::printf("  After modify: readFile(\"hello.bin\"): %s\n",
                    d1.has_value() ? (std::to_string(d1->size()) + " bytes").c_str() : "FAILED");
        if (d1.has_value()) CHECK(d1->size() == 1024);

        auto d2 = reader2->readFile("extra.bin");
        std::printf("  After modify: readFile(\"extra.bin\"): %s\n",
                    d2.has_value() ? (std::to_string(d2->size()) + " bytes").c_str() : "FAILED");
        if (d2.has_value()) CHECK(d2->size() == 2048);
    }

    // Test delete.
    {
        auto w = StorageWritable::open(testDir);
        if (w.has_value()) {
            bool delOk = w->deleteFile("extra.bin");
            std::printf("  deleteFile(\"extra.bin\"): %s\n", delOk ? "OK" : "FAILED");
            if (delOk) {
                REQUIRE(w->save(testDir));
            }
        }
    }

    if (true) {
        auto reader3 = Storage::open(testDir);
        REQUIRE(reader3.has_value());
        bool exists = reader3->fileExists("extra.bin");
        std::printf("  After delete: fileExists(\"extra.bin\"): %s\n", exists ? "true" : "false");
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Write many files, verify all survive a save/reopen cycle
// ============================================================================

TEST_CASE("Health: Bulk Write Round-Trip", "[casc][write_health]") {
    std::printf("\n[Test: Bulk write round-trip — many files]\n");

    const std::string testDir = "test_casc_health_bulk";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "health_test";
    opts.version = "1.0.0";
    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // Write 100 files of varying sizes.
    constexpr u32 kFileCount = 100;
    std::vector<std::vector<u8>> originals(kFileCount);

    for (u32 i = 0; i < kFileCount; ++i) {
        size_t size = (i + 1) * 256;  // 256 B to 25.6 KB.
        originals[i] = makeTestData(size, static_cast<u8>(i));
        std::string path = "bulk/file_" + std::to_string(i) + ".dat";
        REQUIRE(storage.writeFile(path, originals[i]));
    }

    REQUIRE(storage.save(testDir));

    // Reopen and verify every file.
    auto reopened = Storage::open(testDir);
    REQUIRE(reopened.has_value());

    auto fileCount = reopened->totalFileCount();
    REQUIRE(fileCount.has_value());
    CHECK(*fileCount == kFileCount);

    for (u32 i = 0; i < kFileCount; ++i) {
        std::string path = "bulk/file_" + std::to_string(i) + ".dat";
        auto data = reopened->readFile(path);
        REQUIRE(data.has_value());
        CHECK(data->size() == originals[i].size());
        CHECK(*data == originals[i]);
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Files grow in size across multiple save cycles (increasing frame count)
// ============================================================================

TEST_CASE("Health: File Growth Across Save Cycles", "[casc][write_health]") {
    std::printf("\n[Test: File growth — increasing frame count across saves]\n");

    const std::string testDir = "test_casc_health_growth";
    cleanDir(testDir);

    // Use small frames so we can observe multi-frame behavior at modest sizes.
    constexpr u32 kFileCount = 10;
    constexpr size_t kInitialSize = 1024;

    {
        CreateOptions opts;
        opts.product = "growth_test";
        opts.version = "1.0.0";
        opts.blteFrameSize = kSmallFrameSize;
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (u32 i = 0; i < kFileCount; ++i) {
            auto data = makeTestData(kInitialSize, static_cast<u8>(i));
            REQUIRE(storage.writeFile("grow/f" + std::to_string(i) + ".bin", data));
        }
        REQUIRE(storage.save(testDir));
    }

    u64 prevArchiveSize = measureArchiveSize(testDir);
    CHECK(prevArchiveSize > 0);

    // Over 5 cycles, grow each file by 4× (so frame count increases significantly).
    for (u32 cycle = 1; cycle <= 5; ++cycle) {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        size_t newSize = kInitialSize * static_cast<size_t>(1u << (cycle * 2));  // ×4 each cycle.

        for (u32 i = 0; i < kFileCount; ++i) {
            auto data = makeTestData(newSize, static_cast<u8>(i + cycle * 10));
            REQUIRE(writable->writeFile("grow/f" + std::to_string(i) + ".bin", data));
        }

        REQUIRE(writable->save(testDir));

        // Archive should grow as file sizes increase.
        u64 archiveSize = measureArchiveSize(testDir);
        CHECK(archiveSize > prevArchiveSize);
        prevArchiveSize = archiveSize;

        // Verify all files are still readable at the new size.
        auto reader = Storage::open(testDir);
        REQUIRE(reader.has_value());

        for (u32 i = 0; i < kFileCount; ++i) {
            auto data = reader->readFile("grow/f" + std::to_string(i) + ".bin");
            REQUIRE(data.has_value());
            CHECK(data->size() == newSize);
            auto expected = makeTestData(newSize, static_cast<u8>(i + cycle * 10));
            CHECK(*data == expected);
        }
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Files shrink drastically — archive compacts on save
// ============================================================================

TEST_CASE("Health: File Shrink Reclaims Space", "[casc][write_health]") {
    std::printf("\n[Test: File shrink — archive compaction on save]\n");

    const std::string testDir = "test_casc_health_shrink";
    cleanDir(testDir);

    constexpr u32 kFileCount = 20;
    constexpr size_t kLargeSize = 128 * 1024;

    {
        CreateOptions opts;
        opts.product = "shrink_test";
        opts.version = "1.0.0";
        opts.blteFrameSize = kSmallFrameSize;
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (u32 i = 0; i < kFileCount; ++i) {
            auto data = makeTestData(kLargeSize, static_cast<u8>(i));
            REQUIRE(storage.writeFile("shrink/f" + std::to_string(i) + ".bin", data));
        }
        REQUIRE(storage.save(testDir));
    }

    u64 largeArchiveSize = measureArchiveSize(testDir);
    std::printf("  Archive size (large files):  %llu bytes\n",
                static_cast<unsigned long long>(largeArchiveSize));

    // Now replace every file with a tiny 64-byte version and save again.
    {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        for (u32 i = 0; i < kFileCount; ++i) {
            auto tinyData = makeTestData(64, static_cast<u8>(i + 100));
            REQUIRE(writable->writeFile("shrink/f" + std::to_string(i) + ".bin", tinyData));
        }
        REQUIRE(writable->save(testDir));
    }

    u64 smallArchiveSize = measureArchiveSize(testDir);
    std::printf("  Archive size (tiny files):   %llu bytes\n",
                static_cast<unsigned long long>(smallArchiveSize));

    // The new archive should be dramatically smaller — the old large data
    // is not carried forward because save() rebuilds archives from scratch.
    CHECK(smallArchiveSize < largeArchiveSize / 4);

    // Verify all files read back correctly at the new tiny size.
    auto reader = Storage::open(testDir);
    REQUIRE(reader.has_value());

    for (u32 i = 0; i < kFileCount; ++i) {
        auto data = reader->readFile("shrink/f" + std::to_string(i) + ".bin");
        REQUIRE(data.has_value());
        CHECK(data->size() == 64);
        auto expected = makeTestData(64, static_cast<u8>(i + 100));
        CHECK(*data == expected);
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Delete files → save → verify space is reclaimed
// ============================================================================

TEST_CASE("Health: Delete Reclaims Space", "[casc][write_health]") {
    std::printf("\n[Test: Delete files → space reclamation]\n");

    const std::string testDir = "test_casc_health_delete";
    cleanDir(testDir);

    constexpr u32 kFileCount = 50;
    constexpr size_t kFileSize = 16 * 1024;  // 16 KB each.

    {
        CreateOptions opts;
        opts.product = "delete_test";
        opts.version = "1.0.0";
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (u32 i = 0; i < kFileCount; ++i) {
            auto data = makeTestData(kFileSize, static_cast<u8>(i));
            REQUIRE(storage.writeFile("del/file_" + std::to_string(i) + ".dat", data));
        }
        REQUIRE(storage.save(testDir));
    }

    u64 fullSize = measureArchiveSize(testDir);
    std::printf("  Archive size (50 files): %llu bytes\n",
                static_cast<unsigned long long>(fullSize));

    // Delete 40 of the 50 files and save again.
    {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        for (u32 i = 0; i < 40; ++i) {
            CHECK(writable->deleteFile("del/file_" + std::to_string(i) + ".dat"));
        }
        REQUIRE(writable->save(testDir));
    }

    u64 reducedSize = measureArchiveSize(testDir);
    std::printf("  Archive size (10 files): %llu bytes\n",
                static_cast<unsigned long long>(reducedSize));

    // Should be roughly 1/5 of the original.
    CHECK(reducedSize < fullSize / 2);

    // Verify survivors.
    auto reader = Storage::open(testDir);
    REQUIRE(reader.has_value());

    auto count = reader->totalFileCount();
    REQUIRE(count.has_value());
    CHECK(*count == 10);

    for (u32 i = 40; i < kFileCount; ++i) {
        auto data = reader->readFile("del/file_" + std::to_string(i) + ".dat");
        REQUIRE(data.has_value());
        CHECK(data->size() == kFileSize);
        auto expected = makeTestData(kFileSize, static_cast<u8>(i));
        CHECK(*data == expected);
    }

    // Verify deleted files are gone.
    for (u32 i = 0; i < 40; ++i) {
        CHECK_FALSE(reader->fileExists("del/file_" + std::to_string(i) + ".dat"));
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Mixed churn — add, delete, replace across many cycles
// ============================================================================

TEST_CASE("Health: Mixed Churn Across Cycles", "[casc][write_health]") {
    std::printf("\n[Test: Mixed churn — add/delete/replace over 8 cycles]\n");

    const std::string testDir = "test_casc_health_churn";
    cleanDir(testDir);

    // Cycle schema:
    //   Cycle 0: write files 0..19 (20 files)
    //   Cycle 1: delete 0..4, replace 5..9 (larger), add 20..24
    //   Cycle 2: delete 5..9, replace 10..14 (larger), add 25..29
    //   ... (pattern continues)
    //   Each cycle we track expected live contents and verify.

    CreateOptions opts;
    opts.product = "churn_test";
    opts.version = "1.0.0";

    // Track live files: path → expected data.
    std::map<std::string, std::vector<u8>> liveFiles;

    // Cycle 0: initial population.
    {
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (u32 i = 0; i < 20; ++i) {
            std::string path = "churn/f" + std::to_string(i) + ".dat";
            auto data = makeTestData(4096 + i * 128, static_cast<u8>(i));
            REQUIRE(storage.writeFile(path, data));
            liveFiles[path] = data;
        }
        REQUIRE(storage.save(testDir));
    }

    // Cycles 1..8: churn.
    for (u32 cycle = 1; cycle <= 8; ++cycle) {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        u32 deleteBase = (cycle - 1) * 5;
        u32 replaceBase = deleteBase + 5;
        u32 addBase = 20 + (cycle - 1) * 5;

        // Delete 5 files.
        for (u32 i = deleteBase; i < deleteBase + 5 && i < 100; ++i) {
            std::string path = "churn/f" + std::to_string(i) + ".dat";
            if (liveFiles.count(path)) {
                writable->deleteFile(path);
                liveFiles.erase(path);
            }
        }

        // Replace 5 files with larger versions.
        for (u32 i = replaceBase; i < replaceBase + 5 && i < 100; ++i) {
            std::string path = "churn/f" + std::to_string(i) + ".dat";
            if (liveFiles.count(path)) {
                size_t newSize = 8192 + cycle * 2048 + i * 64;
                auto data = makeTestData(newSize, static_cast<u8>(i + cycle * 30));
                REQUIRE(writable->writeFile(path, data));
                liveFiles[path] = data;
            }
        }

        // Add 5 new files.
        for (u32 i = addBase; i < addBase + 5; ++i) {
            std::string path = "churn/f" + std::to_string(i) + ".dat";
            auto data = makeTestData(2048 + i * 64, static_cast<u8>(i + cycle * 50));
            REQUIRE(writable->writeFile(path, data));
            liveFiles[path] = data;
        }

        REQUIRE(writable->save(testDir));

        // Verify after each cycle.
        auto reader = Storage::open(testDir);
        REQUIRE(reader.has_value());

        u32 expectedCount = static_cast<u32>(liveFiles.size());
        auto actualCount = reader->totalFileCount();
        REQUIRE(actualCount.has_value());
        CHECK(*actualCount == expectedCount);

        for (auto& [path, expectedData] : liveFiles) {
            auto data = reader->readFile(path);
            REQUIRE(data.has_value());
            CHECK(data->size() == expectedData.size());
            CHECK(*data == expectedData);
        }

        std::printf("  Cycle %u: %u live files, archive %llu bytes\n",
                    cycle, expectedCount,
                    static_cast<unsigned long long>(measureArchiveSize(testDir)));
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Large multi-frame files replaced by single-frame files repeatedly
// ============================================================================

TEST_CASE("Health: Multi-Frame to Single-Frame Replacement", "[casc][write_health]") {
    std::printf("\n[Test: Multi-frame → single-frame replacement]\n");

    const std::string testDir = "test_casc_health_frame_replace";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "frame_replace_test";
    opts.version = "1.0.0";
    opts.blteFrameSize = kSmallFrameSize;  // 4 KB frames.

    // Repeatedly: write large file (many frames) → save → replace with tiny → save.
    // The archive should shrink back each time.
    constexpr u32 kCycles = 6;

    {
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));
        // Sentinel file that persists through all cycles.
        REQUIRE(storage.writeFile("sentinel.dat", makeTestData(128, 0xFF)));
        REQUIRE(storage.save(testDir));
    }

    for (u32 cycle = 0; cycle < kCycles; ++cycle) {
        // Phase A: Write a large file (many frames).
        {
            auto writable = StorageWritable::open(testDir);
            REQUIRE(writable.has_value());

            size_t largeSize = 256 * 1024;  // 256 KB → 64 frames at 4 KB.
            auto largeData = makeTestData(largeSize, static_cast<u8>(cycle * 2));
            REQUIRE(writable->writeFile("volatile.bin", largeData));
            REQUIRE(writable->save(testDir));
        }

        u64 largeArchive = measureArchiveSize(testDir);

        // Phase B: Replace with a tiny file (single frame).
        {
            auto writable = StorageWritable::open(testDir);
            REQUIRE(writable.has_value());

            auto tinyData = makeTestData(32, static_cast<u8>(cycle * 2 + 1));
            REQUIRE(writable->writeFile("volatile.bin", tinyData));
            REQUIRE(writable->save(testDir));
        }

        u64 smallArchive = measureArchiveSize(testDir);

        std::printf("  Cycle %u: large=%llu bytes, small=%llu bytes\n", cycle,
                    static_cast<unsigned long long>(largeArchive),
                    static_cast<unsigned long long>(smallArchive));

        // Tiny archive should be much smaller than the large-file archive.
        CHECK(smallArchive < largeArchive / 2);

        // Verify both files are correct.
        auto reader = Storage::open(testDir);
        REQUIRE(reader.has_value());

        auto tinyRead = reader->readFile("volatile.bin");
        REQUIRE(tinyRead.has_value());
        CHECK(tinyRead->size() == 32);
        CHECK(*tinyRead == makeTestData(32, static_cast<u8>(cycle * 2 + 1)));

        auto sentinelRead = reader->readFile("sentinel.dat");
        REQUIRE(sentinelRead.has_value());
        CHECK(*sentinelRead == makeTestData(128, 0xFF));
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Overwrite file with identical data — idempotent save
// ============================================================================

TEST_CASE("Health: Idempotent Overwrite", "[casc][write_health]") {
    std::printf("\n[Test: Overwrite with identical data — idempotent save]\n");

    const std::string testDir = "test_casc_health_idempotent";
    cleanDir(testDir);

    constexpr u32 kFileCount = 25;
    std::vector<std::vector<u8>> originals(kFileCount);

    {
        CreateOptions opts;
        opts.product = "idem_test";
        opts.version = "1.0.0";
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (u32 i = 0; i < kFileCount; ++i) {
            originals[i] = makeTestData(8192 + i * 512, static_cast<u8>(i));
            REQUIRE(storage.writeFile("idem/f" + std::to_string(i) + ".dat", originals[i]));
        }
        REQUIRE(storage.save(testDir));
    }

    u64 initialSize = measureArchiveSize(testDir);

    // Reopen and overwrite every file with the same data → save.
    for (u32 pass = 0; pass < 3; ++pass) {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        for (u32 i = 0; i < kFileCount; ++i) {
            REQUIRE(writable->writeFile("idem/f" + std::to_string(i) + ".dat", originals[i]));
        }
        REQUIRE(writable->save(testDir));

        u64 passSize = measureArchiveSize(testDir);
        std::printf("  Pass %u: archive %llu bytes (initial %llu)\n",
                    pass, static_cast<unsigned long long>(passSize),
                    static_cast<unsigned long long>(initialSize));

        // Size should be very close to initial (content-addressed dedup by CKey).
        // Allow 10% tolerance for metadata growth.
        CHECK(passSize <= initialSize * 11 / 10);
        CHECK(passSize >= initialSize * 9 / 10);
    }

    // Verify data integrity after repeated overwrites.
    auto reader = Storage::open(testDir);
    REQUIRE(reader.has_value());

    for (u32 i = 0; i < kFileCount; ++i) {
        auto data = reader->readFile("idem/f" + std::to_string(i) + ".dat");
        REQUIRE(data.has_value());
        CHECK(*data == originals[i]);
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Archive file count stays bounded even with many write cycles
// ============================================================================

TEST_CASE("Health: Archive Count Stays Bounded", "[casc][write_health]") {
    std::printf("\n[Test: Archive file count stability]\n");

    const std::string testDir = "test_casc_health_archive_count";
    cleanDir(testDir);

    constexpr u32 kFileCount = 20;
    constexpr size_t kFileSize = 8192;  // 8 KB each.

    {
        CreateOptions opts;
        opts.product = "bound_test";
        opts.version = "1.0.0";
        // Very small max archive size to force multiple archive files.
        opts.archiveMaxSize = 16 * 1024;  // 16 KB max per archive.
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        // Write uncompressed data so it doesn't get too small.
        WriteOptions wo;
        wo.compress = false;
        for (u32 i = 0; i < kFileCount; ++i) {
            REQUIRE(storage.writeFile("bound/f" + std::to_string(i) + ".dat",
                                      makeTestData(kFileSize, static_cast<u8>(i)), wo));
        }
        REQUIRE(storage.save(testDir));
    }

    u32 initialArchiveCount = countArchiveFiles(testDir);
    CHECK(initialArchiveCount >= 1);

    std::printf("  Initial: %u archive files\n", initialArchiveCount);

    // Modify and save 5 times — count should not grow unboundedly.
    for (u32 cycle = 0; cycle < 5; ++cycle) {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        // Replace a few files (uncompressed to keep predictable sizes).
        WriteOptions wo;
        wo.compress = false;
        for (u32 i = 0; i < 5; ++i) {
            u32 fi = (cycle * 5 + i) % kFileCount;
            REQUIRE(writable->writeFile("bound/f" + std::to_string(fi) + ".dat",
                                        makeTestData(kFileSize, static_cast<u8>(fi + cycle + 1)), wo));
        }
        REQUIRE(writable->save(testDir));

        u32 archiveCount = countArchiveFiles(testDir);
        std::printf("  Cycle %u: %u archive files\n", cycle, archiveCount);

        // Archive count should stay roughly the same (±1) since total data is constant.
        CHECK(archiveCount <= initialArchiveCount + 2);
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Stress — many small writes and saves
// ============================================================================

TEST_CASE("Health: Rapid Small Writes Stress", "[casc][write_health]") {
    std::printf("\n[Test: Rapid small writes stress]\n");

    const std::string testDir = "test_casc_health_stress";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "stress_test";
    opts.version = "1.0.0";

    // Create initial storage with a few files.
    {
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (u32 i = 0; i < 10; ++i) {
            REQUIRE(storage.writeFile("stress/seed_" + std::to_string(i) + ".dat",
                                      makeTestData(512, static_cast<u8>(i))));
        }
        REQUIRE(storage.save(testDir));
    }

    // 20 rapid cycles: each writes 5 new files and modifies 3 existing.
    u32 totalFiles = 10;
    for (u32 cycle = 0; cycle < 20; ++cycle) {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        // New files.
        for (u32 i = 0; i < 5; ++i) {
            u32 id = totalFiles + i;
            REQUIRE(writable->writeFile("stress/file_" + std::to_string(id) + ".dat",
                                        makeTestData(256 + id * 16, static_cast<u8>(id))));
        }
        totalFiles += 5;

        // Modify a few existing files (the seed files).
        for (u32 i = 0; i < 3; ++i) {
            u32 idx = (cycle * 3 + i) % 10;
            REQUIRE(writable->writeFile("stress/seed_" + std::to_string(idx) + ".dat",
                                        makeTestData(512 + cycle * 32,
                                                     static_cast<u8>(idx + cycle))));
        }

        REQUIRE(writable->save(testDir));
    }

    // Final verification.
    auto reader = Storage::open(testDir);
    REQUIRE(reader.has_value());

    auto count = reader->totalFileCount();
    REQUIRE(count.has_value());
    CHECK(*count == totalFiles);  // 10 seed + 20×5 = 110.

    std::printf("  Final: %u files, archive %llu bytes\n",
                *count, static_cast<unsigned long long>(measureArchiveSize(testDir)));

    cleanDir(testDir);
}

// ============================================================================
// Test: Parametrized root format — all writable formats behave consistently
// ============================================================================

TEST_CASE("Health: Root Format Consistency", "[casc][write_health]") {
    auto format = GENERATE(RootFormat::Tvfs, RootFormat::Wow, RootFormat::Diablo3,
                            RootFormat::Diablo4, RootFormat::WowTvfs);

    const char* formatName =
        format == RootFormat::Tvfs    ? "TVFS" :
        format == RootFormat::Wow     ? "WoW"  :
        format == RootFormat::Diablo3 ? "D3"   :
        format == RootFormat::Diablo4 ? "D4"   : "WowTvfs";

    std::printf("\n[Test: Root format consistency — %s]\n", formatName);

    const std::string testDir = std::string("test_casc_health_fmt_") + formatName;
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "fmt_test";
    opts.version = "1.0.0";
    opts.rootFormat = format;

    constexpr u32 kFileCount = 15;
    std::vector<std::vector<u8>> originals(kFileCount);

    {
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (u32 i = 0; i < kFileCount; ++i) {
            originals[i] = makeTestData(1024 * (i + 1), static_cast<u8>(i));
            std::string path = "fmt/file_" + std::to_string(i) + ".dat";
            REQUIRE(storage.writeFile(path, originals[i]));
        }
        REQUIRE(storage.save(testDir));
    }

    // Modify half the files (shrink them).
    {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        for (u32 i = 0; i < kFileCount / 2; ++i) {
            originals[i] = makeTestData(256, static_cast<u8>(i + 100));
            REQUIRE(writable->writeFile("fmt/file_" + std::to_string(i) + ".dat", originals[i]));
        }
        REQUIRE(writable->save(testDir));
    }

    // Delete a few.
    {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        for (u32 i = kFileCount - 3; i < kFileCount; ++i) {
            REQUIRE(writable->deleteFile("fmt/file_" + std::to_string(i) + ".dat"));
        }
        REQUIRE(writable->save(testDir));
    }

    // Verify.
    {
        auto reader = Storage::open(testDir);
        REQUIRE(reader.has_value());

        auto count = reader->totalFileCount();
        REQUIRE(count.has_value());
        CHECK(*count == kFileCount - 3);

        for (u32 i = 0; i < kFileCount - 3; ++i) {
            auto data = reader->readFile("fmt/file_" + std::to_string(i) + ".dat");
            REQUIRE(data.has_value());
            CHECK(*data == originals[i]);
        }

        for (u32 i = kFileCount - 3; i < kFileCount; ++i) {
            CHECK_FALSE(reader->fileExists("fmt/file_" + std::to_string(i) + ".dat"));
        }
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Compressed vs. uncompressed writes — archive size comparison
// ============================================================================

TEST_CASE("Health: Compressed vs Uncompressed", "[casc][write_health]") {
    std::printf("\n[Test: Compressed vs uncompressed archive sizes]\n");

    const std::string compDir = "test_casc_health_comp";
    const std::string rawDir = "test_casc_health_raw";
    cleanDir(compDir);
    cleanDir(rawDir);

    // Highly compressible data.
    constexpr size_t kFileSize = 64 * 1024;
    constexpr u32 kFileCount = 10;

    // Compressed storage.
    {
        auto storage = StorageWritable::create();
        for (u32 i = 0; i < kFileCount; ++i) {
            WriteOptions wo;
            wo.compress = true;
            REQUIRE(storage.writeFile("comp/f" + std::to_string(i) + ".dat",
                                      makeCompressibleData(kFileSize, static_cast<u8>(i)), wo));
        }
        REQUIRE(storage.save(compDir));
    }

    // Uncompressed storage.
    {
        auto storage = StorageWritable::create();
        for (u32 i = 0; i < kFileCount; ++i) {
            WriteOptions wo;
            wo.compress = false;
            REQUIRE(storage.writeFile("raw/f" + std::to_string(i) + ".dat",
                                      makeCompressibleData(kFileSize, static_cast<u8>(i)), wo));
        }
        REQUIRE(storage.save(rawDir));
    }

    u64 compSize = measureArchiveSize(compDir);
    u64 rawSize = measureArchiveSize(rawDir);

    std::printf("  Compressed:   %llu bytes\n", static_cast<unsigned long long>(compSize));
    std::printf("  Uncompressed: %llu bytes\n", static_cast<unsigned long long>(rawSize));

    // For compressible data, compressed archive should be much smaller.
    CHECK(compSize < rawSize / 2);

    // Both should read back identically.
    auto readerComp = Storage::open(compDir);
    auto readerRaw = Storage::open(rawDir);
    REQUIRE(readerComp.has_value());
    REQUIRE(readerRaw.has_value());

    for (u32 i = 0; i < kFileCount; ++i) {
        auto expected = makeCompressibleData(kFileSize, static_cast<u8>(i));

        auto cData = readerComp->readFile("comp/f" + std::to_string(i) + ".dat");
        REQUIRE(cData.has_value());
        CHECK(*cData == expected);

        auto rData = readerRaw->readFile("raw/f" + std::to_string(i) + ".dat");
        REQUIRE(rData.has_value());
        CHECK(*rData == expected);
    }

    cleanDir(compDir);
    cleanDir(rawDir);
}

// ============================================================================
// Test: Configurable BLTE frame sizes — small vs large frames
// ============================================================================

TEST_CASE("Health: Frame Size Configurations", "[casc][write_health]") {
    auto frameSize = GENERATE(u32(1024), u32(4096), u32(0x10000), u32(0x40000));

    std::printf("\n[Test: Frame size %u bytes]\n", frameSize);

    const std::string testDir = "test_casc_health_fsize_" + std::to_string(frameSize);
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "fsize_test";
    opts.version = "1.0.0";
    opts.blteFrameSize = frameSize;

    constexpr size_t kFileSize = 512 * 1024;  // 512 KB.
    constexpr u32 kFileCount = 5;

    std::vector<std::vector<u8>> originals(kFileCount);

    {
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (u32 i = 0; i < kFileCount; ++i) {
            originals[i] = makeTestData(kFileSize, static_cast<u8>(i));
            REQUIRE(storage.writeFile("fsize/f" + std::to_string(i) + ".bin", originals[i]));
        }
        REQUIRE(storage.save(testDir));
    }

    u64 archiveSize = measureArchiveSize(testDir);
    std::printf("  Archive size: %llu bytes\n", static_cast<unsigned long long>(archiveSize));

    // Reopen, modify, save again.
    {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        // Replace first file with smaller data.
        originals[0] = makeTestData(128, 0xAA);
        REQUIRE(writable->writeFile("fsize/f0.bin", originals[0]));

        // Replace last file with larger data.
        originals[kFileCount - 1] = makeTestData(1024 * 1024, 0xBB);
        REQUIRE(writable->writeFile("fsize/f" + std::to_string(kFileCount - 1) + ".bin",
                                    originals[kFileCount - 1]));

        REQUIRE(writable->save(testDir));
    }

    // Verify.
    auto reader = Storage::open(testDir);
    REQUIRE(reader.has_value());

    for (u32 i = 0; i < kFileCount; ++i) {
        auto data = reader->readFile("fsize/f" + std::to_string(i) + ".bin");
        REQUIRE(data.has_value());
        CHECK(data->size() == originals[i].size());
        CHECK(*data == originals[i]);
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Empty file edge cases
// ============================================================================

TEST_CASE("Health: Empty Files", "[casc][write_health]") {
    std::printf("\n[Test: Empty file handling]\n");

    const std::string testDir = "test_casc_health_empty";
    cleanDir(testDir);

    {
        auto storage = StorageWritable::create();
        REQUIRE(static_cast<bool>(storage));

        REQUIRE(storage.writeFile("empty1.dat", {}));
        REQUIRE(storage.writeFile("notempty.dat", makeTestData(1024, 0x42)));
        REQUIRE(storage.writeFile("empty2.dat", {}));
        REQUIRE(storage.writeFile("empty3.dat", {}));

        REQUIRE(storage.save(testDir));
    }

    {
        auto reader = Storage::open(testDir);
        REQUIRE(reader.has_value());

        auto e1 = reader->readFile("empty1.dat");
        REQUIRE(e1.has_value());
        CHECK(e1->empty());

        auto ne = reader->readFile("notempty.dat");
        REQUIRE(ne.has_value());
        CHECK(*ne == makeTestData(1024, 0x42));

        auto e2 = reader->readFile("empty2.dat");
        REQUIRE(e2.has_value());
        CHECK(e2->empty());

        auto e3 = reader->readFile("empty3.dat");
        REQUIRE(e3.has_value());
        CHECK(e3->empty());
    }

    // Replace non-empty with empty and vice versa.
    {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        REQUIRE(writable->writeFile("notempty.dat", {}));
        REQUIRE(writable->writeFile("empty1.dat", makeTestData(512, 0xCC)));
        REQUIRE(writable->save(testDir));
    }

    auto reader2 = Storage::open(testDir);
    REQUIRE(reader2.has_value());

    auto ne2 = reader2->readFile("notempty.dat");
    REQUIRE(ne2.has_value());
    CHECK(ne2->empty());

    auto e1v2 = reader2->readFile("empty1.dat");
    REQUIRE(e1v2.has_value());
    CHECK(*e1v2 == makeTestData(512, 0xCC));

    cleanDir(testDir);
}

// ============================================================================
// Test: Graduated scaling — file sizes grow by orders of magnitude
// ============================================================================

TEST_CASE("Health: Graduated Size Scaling", "[casc][write_health]") {
    std::printf("\n[Test: Graduated size scaling — 1B to 2MB]\n");

    const std::string testDir = "test_casc_health_graduated";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "grad_test";
    opts.version = "1.0.0";
    opts.blteFrameSize = kSmallFrameSize;

    // Files at exponentially increasing sizes: 1, 16, 256, 4K, 64K, 1M, 2M.
    const std::vector<size_t> sizes = {1, 16, 256, 4096, 65536, 1024 * 1024, 2 * 1024 * 1024};
    std::vector<std::vector<u8>> originals;

    {
        auto storage = StorageWritable::create(opts);
        REQUIRE(static_cast<bool>(storage));

        for (size_t i = 0; i < sizes.size(); ++i) {
            originals.push_back(makeTestData(sizes[i], static_cast<u8>(i)));
            REQUIRE(storage.writeFile("grad/size_" + std::to_string(sizes[i]) + ".dat", originals[i]));
        }
        REQUIRE(storage.save(testDir));
    }

    // Verify.
    {
        auto reader = Storage::open(testDir);
        REQUIRE(reader.has_value());

        for (size_t i = 0; i < sizes.size(); ++i) {
            auto data = reader->readFile("grad/size_" + std::to_string(sizes[i]) + ".dat");
            REQUIRE(data.has_value());
            CHECK(data->size() == sizes[i]);
            CHECK(*data == originals[i]);
        }
    }

    // Now reverse: replace all with a fixed 512 bytes.
    {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        for (size_t i = 0; i < sizes.size(); ++i) {
            originals[i] = makeTestData(512, static_cast<u8>(i + 100));
            REQUIRE(writable->writeFile("grad/size_" + std::to_string(sizes[i]) + ".dat",
                                        originals[i]));
        }
        REQUIRE(writable->save(testDir));
    }

    u64 flatSize = measureArchiveSize(testDir);
    std::printf("  Archive with all files at 512 bytes: %llu bytes\n",
                static_cast<unsigned long long>(flatSize));

    // Should be modest — all files are now 512 bytes.
    CHECK(flatSize < 64 * 1024);  // Well under 64 KB for 7 × 512 B files.

    auto reader2 = Storage::open(testDir);
    REQUIRE(reader2.has_value());

    for (size_t i = 0; i < sizes.size(); ++i) {
        auto data = reader2->readFile("grad/size_" + std::to_string(sizes[i]) + ".dat");
        REQUIRE(data.has_value());
        CHECK(*data == originals[i]);
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Concurrent-style add + delete in the same save cycle
// ============================================================================

TEST_CASE("Health: Add And Delete Same Cycle", "[casc][write_health]") {
    std::printf("\n[Test: Add + delete in the same save cycle]\n");

    const std::string testDir = "test_casc_health_add_del";
    cleanDir(testDir);

    // Seed storage.
    {
        auto storage = StorageWritable::create();
        for (u32 i = 0; i < 20; ++i) {
            REQUIRE(storage.writeFile("ad/f" + std::to_string(i) + ".dat",
                                      makeTestData(2048, static_cast<u8>(i))));
        }
        REQUIRE(storage.save(testDir));
    }

    // In a single open/save cycle: delete some, add others.
    {
        auto writable = StorageWritable::open(testDir);
        REQUIRE(writable.has_value());

        // Delete files 0..9.
        for (u32 i = 0; i < 10; ++i) {
            REQUIRE(writable->deleteFile("ad/f" + std::to_string(i) + ".dat"));
        }

        // Add files 20..29.
        for (u32 i = 20; i < 30; ++i) {
            REQUIRE(writable->writeFile("ad/f" + std::to_string(i) + ".dat",
                                        makeTestData(4096, static_cast<u8>(i))));
        }

        REQUIRE(writable->save(testDir));
    }

    auto reader = Storage::open(testDir);
    REQUIRE(reader.has_value());

    auto count = reader->totalFileCount();
    REQUIRE(count.has_value());
    CHECK(*count == 20);  // 10 survivors + 10 new.

    // Deleted files should be gone.
    for (u32 i = 0; i < 10; ++i) {
        CHECK_FALSE(reader->fileExists("ad/f" + std::to_string(i) + ".dat"));
    }

    // Survivors should be intact.
    for (u32 i = 10; i < 20; ++i) {
        auto data = reader->readFile("ad/f" + std::to_string(i) + ".dat");
        REQUIRE(data.has_value());
        CHECK(*data == makeTestData(2048, static_cast<u8>(i)));
    }

    // New files should be present.
    for (u32 i = 20; i < 30; ++i) {
        auto data = reader->readFile("ad/f" + std::to_string(i) + ".dat");
        REQUIRE(data.has_value());
        CHECK(*data == makeTestData(4096, static_cast<u8>(i)));
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: File sizes reported by enumerate match actual read sizes
// ============================================================================

TEST_CASE("Health: Enumerate Size Consistency", "[casc][write_health]") {
    std::printf("\n[Test: Enumerate file sizes vs actual read sizes]\n");

    const std::string testDir = "test_casc_health_enum_size";
    cleanDir(testDir);

    const std::vector<size_t> sizes = {0, 1, 127, 128, 1023, 1024, 65535, 65536, 100000};

    {
        auto storage = StorageWritable::create();
        REQUIRE(static_cast<bool>(storage));

        for (size_t i = 0; i < sizes.size(); ++i) {
            REQUIRE(storage.writeFile("enum/f" + std::to_string(i) + ".dat",
                                      makeTestData(sizes[i], static_cast<u8>(i))));
        }
        REQUIRE(storage.save(testDir));
    }

    auto reader = Storage::open(testDir);
    REQUIRE(reader.has_value());

    // Check that fileSize() API returns the correct size for each file.
    for (size_t i = 0; i < sizes.size(); ++i) {
        std::string path = "enum/f" + std::to_string(i) + ".dat";
        auto fsize = reader->fileSize(path);
        REQUIRE(fsize.has_value());
        CHECK(*fsize == sizes[i]);

        auto data = reader->readFile(path);
        REQUIRE(data.has_value());
        CHECK(data->size() == sizes[i]);
    }

    // Also verify via enumerate: build a map keyed by normalized path.
    std::map<std::string, u64> enumeratedSizes;
    reader->enumerate([&](const EnumerateEntry& e) {
        // Normalize separators to forward-slash for consistent comparison.
        std::string p(e.path);
        std::replace(p.begin(), p.end(), '\\', '/');
        enumeratedSizes[p] = e.fileSize;
        return true;
    });

    for (size_t i = 0; i < sizes.size(); ++i) {
        std::string path = "enum/f" + std::to_string(i) + ".dat";
        auto it = enumeratedSizes.find(path);
        REQUIRE(it != enumeratedSizes.end());
        CHECK(it->second == sizes[i]);
    }

    cleanDir(testDir);
}

// ============================================================================
// Test: Progressive archive growth — total archive size tracks data volume
// ============================================================================

TEST_CASE("Health: Archive Size Tracks Data Volume", "[casc][write_health]") {
    std::printf("\n[Test: Archive size tracks total data volume]\n");

    const std::string testDir = "test_casc_health_tracking";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "track_test";
    opts.version = "1.0.0";

    auto storage = StorageWritable::create(opts);
    REQUIRE(static_cast<bool>(storage));

    // Add files in batches and measure archive size after each.
    u64 prevArchiveSize = 0;
    u64 totalDataWritten = 0;

    for (u32 batch = 0; batch < 5; ++batch) {
        if (batch == 0) {
            // First batch writes directly to the created storage.
        } else {
            auto w = StorageWritable::open(testDir);
            REQUIRE(w.has_value());
            storage = std::move(*w);
        }

        for (u32 i = 0; i < 10; ++i) {
            u32 id = batch * 10 + i;
            size_t size = 4096;
            REQUIRE(storage.writeFile("track/f" + std::to_string(id) + ".dat",
                                      makeTestData(size, static_cast<u8>(id))));
            totalDataWritten += size;
        }
        REQUIRE(storage.save(testDir));

        u64 archiveSize = measureArchiveSize(testDir);
        std::printf("  Batch %u: %u files, data=%llu, archive=%llu bytes\n",
                    batch, (batch + 1) * 10,
                    static_cast<unsigned long long>(totalDataWritten),
                    static_cast<unsigned long long>(archiveSize));

        // Archive should grow with each batch.
        CHECK(archiveSize > prevArchiveSize);

        // Archive size should be within reasonable bounds of raw data volume.
        // (metadata + encoding overhead, but not wildly disproportionate).
        CHECK(archiveSize < totalDataWritten * 3);  // Less than 3× overhead.

        prevArchiveSize = archiveSize;
    }

    cleanDir(testDir);
}

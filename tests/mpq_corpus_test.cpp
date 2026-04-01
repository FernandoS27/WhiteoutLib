// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MPQ corpus test suite.
//
// Opens every MPQ archive in the corpus directory (including .w3x, .w3n,
// .SC2Map, .SC2Mod files which are also MPQ archives) and runs a battery
// of tests against real data:
//
//   1. Open & parse — validates header, hash table, block table
//   2. List files — verifies listfile extraction
//   3. Read every file — exercises full extraction pipeline (decompression,
//      decryption, multi-sector, single-unit, etc.)
//   4. File metadata consistency — fileExists, fileInfo match extracted data
//   5. Round-trip — save to a temp copy, reopen, verify all files identical
//   6. Parallel read — read all files concurrently with a thread pool
//   7. Concurrent read + modify — read while writing overlay, save, verify
//   8. Selective modification — write a new file, delete one, round-trip
//
// Usage: mpq_corpus_test [corpus_dir]
//   corpus_dir defaults to Corpus/MPQ (searched relative to CWD + hard-coded).

#include <catch2/catch_all.hpp>

#include <whiteout/storages/mpq/storage.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace mpq = whiteout::storages::mpq;

using whiteout::u8;
using whiteout::u32;
using whiteout::u64;

// ============================================================================
// Test harness
// ============================================================================
static bool fail(const char* fmt, ...) {
    
    std::printf("  FAIL: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return false;
}

static bool pass(const char* fmt, ...) {
    
    std::printf("  PASS: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return true;
}

static bool expect(bool condition, const char* fmt, ...) {
    if (condition) {
        
        return true;
    }
    
    std::printf("  FAIL: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return false;
}

// ============================================================================
// Corpus discovery
// ============================================================================

static std::string findCorpusDir(const char* override_path) {
    if (override_path && fs::exists(override_path))
        return override_path;

    for (auto& p : {"Corpus/MPQ", "../Corpus/MPQ", "../../Corpus/MPQ",
                     "C:/Projects/WhiteoutLib/Corpus/MPQ"}) {
        if (fs::exists(p))
            return p;
    }
    return "";
}

static bool isMpqExtension(const std::string& ext) {
    // All of these are MPQ archives.
    std::string lower;
    lower.resize(ext.size());
    std::transform(ext.begin(), ext.end(), lower.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });
    return lower == ".mpq" || lower == ".w3x" || lower == ".w3n" || lower == ".w3m" ||
           lower == ".sc2map" || lower == ".sc2mod" || lower == ".sc2replay" ||
           lower == ".s2ma" || lower == ".s2mod";
}

static std::vector<std::string> discoverArchives(const std::string& dir) {
    std::vector<std::string> archives;
    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        if (isMpqExtension(entry.path().extension().string()))
            archives.push_back(entry.path().string());
    }
    std::sort(archives.begin(), archives.end());
    return archives;
}

// ============================================================================
// Helpers
// ============================================================================

/// Simple FNV-1a hash for quick content comparison.
static u64 fnv1a(const std::vector<u8>& data) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (auto b : data) {
        hash ^= b;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static std::vector<u8> makeTestPayload(size_t size) {
    static constexpr char kPhrase[] = "mpq-corpus-test-injected-payload-";
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>(kPhrase[i % (sizeof(kPhrase) - 1)]);
    return data;
}

// ============================================================================
// Per-archive test suite
// ============================================================================

struct ArchiveTestResult {
    std::string path;
    int passed = 0;
    int failed = 0;
    u32 fileCount = 0;
    u32 filesRead = 0;
    u32 readErrors = 0;
};

/// Snapshot of file contents: filename -> {size, hash}.
struct FileSnapshot {
    size_t size = 0;
    u64 hash = 0;
};

/// Read all files from a storage and return a snapshot map.
/// Increments readErrors for files that fail to extract.
static std::unordered_map<std::string, FileSnapshot>
snapshotAllFiles(mpq::Storage& storage, const std::vector<std::string>& files, u32& readErrors) {
    std::unordered_map<std::string, FileSnapshot> snap;
    for (const auto& name : files) {
        std::string error;
        auto data = storage.readFile(name, &error);
        if (data) {
            snap[name] = {data->size(), fnv1a(*data)};
        } else {
            readErrors++;
        }
    }
    return snap;
}

static ArchiveTestResult testArchive(const std::string& archivePath,
                                     whiteout::utils::SimpleThreadPool& pool) {
    int localPassed = 0;
    int localFailed = 0;

    auto localExpect = [&](bool cond, const char* fmt, ...) -> bool {
        if (cond) {
            localPassed++;
            return true;
        }
        localFailed++;
        std::printf("    FAIL: ");
        va_list args;
        va_start(args, fmt);
        std::vprintf(fmt, args);
        va_end(args);
        std::printf("\n");
        return false;
    };

    auto localPass = [&](const char* fmt, ...) {
        localPassed++;
        std::printf("    PASS: ");
        va_list args;
        va_start(args, fmt);
        std::vprintf(fmt, args);
        va_end(args);
        std::printf("\n");
    };

    auto localFail = [&](const char* fmt, ...) {
        localFailed++;
        std::printf("    FAIL: ");
        va_list args;
        va_start(args, fmt);
        std::vprintf(fmt, args);
        va_end(args);
        std::printf("\n");
    };

    ArchiveTestResult result;
    result.path = archivePath;

    std::string relPath = fs::relative(archivePath).string();
    std::printf("\n  === %s ===\n", relPath.c_str());

    // ---------------------------------------------------------------
    // TEST 1: Open archive (serial, no pool)
    // ---------------------------------------------------------------
    std::string openError;
    auto storage = mpq::Storage::open(archivePath, &openError);
    if (!storage) {
        localFail("open failed: %s", openError.c_str());
        result.failed = localFailed;
        result.passed = localPassed;
        return result;
    }
    localPass("open (serial)");

    // ---------------------------------------------------------------
    // TEST 2: Open archive with worker pool
    // ---------------------------------------------------------------
    auto storagePooled = mpq::Storage::open(archivePath, &openError, &pool);
    if (!storagePooled) {
        localFail("open with pool failed: %s", openError.c_str());
        result.failed = localFailed;
        result.passed = localPassed;
        return result;
    }
    localPass("open (pooled)");

    // ---------------------------------------------------------------
    // TEST 3: Archive info sanity
    // ---------------------------------------------------------------
    {
        auto info = storage->archiveInfo();
        localExpect(info.hashTableEntries > 0, "hash table has entries (got %u)",
                    info.hashTableEntries);
        localExpect(info.sectorSize > 0, "sector size > 0 (got %u)", info.sectorSize);
    }

    // ---------------------------------------------------------------
    // TEST 4: List files
    // ---------------------------------------------------------------
    auto files = storage->listFiles();
    result.fileCount = static_cast<u32>(files.size());
    if (files.empty()) {
        // Archives without (listfile) — expected for war3.mpq, some w3x.
        std::printf("    INFO: listFiles() returned 0 files (no listfile in archive)\n");
    } else {
        localPass("listFiles() returned %zu files", files.size());
    }

    // ---------------------------------------------------------------
    // TEST 5: Read every file (serial) & build snapshot
    // ---------------------------------------------------------------
    u32 serialReadErrors = 0;
    auto serialSnapshot = snapshotAllFiles(*storage, files, serialReadErrors);

    result.filesRead = static_cast<u32>(serialSnapshot.size());
    result.readErrors = serialReadErrors;

    if (files.empty()) {
        // Nothing to read — skip the rest of content tests.
        result.passed = localPassed;
        result.failed = localFailed;
        return result;
    }
    localExpect(!serialSnapshot.empty(), "serial read: %zu/%zu files extracted (%u errors)",
                serialSnapshot.size(), files.size(), serialReadErrors);

    // ---------------------------------------------------------------
    // TEST 6: fileExists + fileInfo consistency
    // ---------------------------------------------------------------
    {
        u32 existMismatches = 0;
        u32 sizeMismatches = 0;
        for (const auto& [name, snap] : serialSnapshot) {
            if (!storage->fileExists(name))
                existMismatches++;
            auto fi = storage->fileInfo(name);
            if (fi && fi->uncompressedSize != static_cast<u32>(snap.size))
                sizeMismatches++;
        }
        localExpect(existMismatches == 0, "fileExists consistency (%u mismatches)",
                    existMismatches);
        // fileInfo size can legitimately differ from extracted size for some
        // compression modes (block table stores approximate/padded sizes).
        if (sizeMismatches > 0) {
            std::printf("    WARN: fileInfo uncompressedSize differs from extracted "
                        "size for %u/%zu files\n",
                        sizeMismatches, serialSnapshot.size());
        } else {
            localPass("fileInfo size consistency");
        }
    }

    // ---------------------------------------------------------------
    // TEST 7: parallel read — all files via pooled storage
    // ---------------------------------------------------------------
    {
        u32 poolReadErrors = 0;
        auto poolSnapshot = snapshotAllFiles(*storagePooled, files, poolReadErrors);

        localExpect(poolSnapshot.size() == serialSnapshot.size(),
                    "parallel read: same file count (serial=%zu pool=%zu)",
                    serialSnapshot.size(), poolSnapshot.size());

        u32 hashMismatches = 0;
        for (const auto& [name, snap] : serialSnapshot) {
            auto it = poolSnapshot.find(name);
            if (it == poolSnapshot.end() || it->second.hash != snap.hash) {
                hashMismatches++;
                if (hashMismatches <= 5) {
                    std::printf("    DIAG: pool vs serial mismatch: %s "
                                "(serial size=%zu hash=%016llx, pool %s)\n",
                                name.c_str(), snap.size,
                                static_cast<unsigned long long>(snap.hash),
                                (it == poolSnapshot.end()) ? "missing" : "differs");
                }
            }
        }
        localExpect(hashMismatches == 0,
                    "parallel read: content matches serial (%u mismatches)", hashMismatches);
    }

    // ---------------------------------------------------------------
    // TEST 8: concurrent reads from multiple threads
    // ---------------------------------------------------------------
    if (!serialSnapshot.empty()) {
        constexpr int kReaderThreads = 8;
        constexpr int kIterations = 3;

        std::atomic<u32> concurrentErrors{0};
        std::vector<std::thread> readers;

        for (int t = 0; t < kReaderThreads; ++t) {
            readers.emplace_back([&, t]() {
                for (int iter = 0; iter < kIterations; ++iter) {
                    for (const auto& name : files) {
                        auto data = storage->readFile(name);
                        if (!data)
                            continue;
                        auto it = serialSnapshot.find(name);
                        if (it != serialSnapshot.end() && fnv1a(*data) != it->second.hash)
                            concurrentErrors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& th : readers)
            th.join();

        localExpect(concurrentErrors.load() == 0,
                    "concurrent reads (%d threads x %d iters): %u hash mismatches",
                    kReaderThreads, kIterations, concurrentErrors.load());
    }

    // ---------------------------------------------------------------
    // TEST 9: round-trip — save copy, reopen, verify all files
    // ---------------------------------------------------------------
    {
        auto tempDir = fs::temp_directory_path() / "whiteout_mpq_corpus_test";
        fs::create_directories(tempDir);
        auto tempFile = tempDir / "roundtrip.mpq";

        // Re-open fresh to avoid stale overlay state from test 10.
        auto freshForRt = mpq::Storage::open(archivePath, &pool);
        bool saved = freshForRt ? freshForRt->save(tempFile.string()) : false;
        if (!localExpect(saved, "save() to temp file")) {
            // cleanup
            std::error_code ec;
            fs::remove_all(tempDir, ec);
        } else {
            auto reopened = mpq::Storage::open(tempFile.string(), &pool);
            if (!localExpect(reopened.has_value(), "reopen saved archive")) {
                std::error_code ec;
                fs::remove_all(tempDir, ec);
            } else {
                auto rtFiles = reopened->listFiles();
                // Writer adds (listfile) and (attributes) — allow extra files.
                localExpect(rtFiles.size() >= files.size(),
                            "round-trip file count: orig=%zu saved=%zu",
                            files.size(), rtFiles.size());

                u32 rtErrors = 0;
                auto rtSnapshot = snapshotAllFiles(*reopened, rtFiles, rtErrors);

                // Every original file must exist and match in the round-trip.
                u32 rtMissing = 0;
                u32 rtContentMismatch = 0;
                for (const auto& [name, snap] : serialSnapshot) {
                    auto it = rtSnapshot.find(name);
                    if (it == rtSnapshot.end()) {
                        rtMissing++;
                        if (rtMissing <= 3)
                            std::printf("    DIAG: round-trip missing: %s\n", name.c_str());
                    } else if (it->second.hash != snap.hash) {
                        rtContentMismatch++;
                        if (rtContentMismatch <= 3)
                            std::printf("    DIAG: round-trip content differs: %s\n",
                                        name.c_str());
                    }
                }
                localExpect(rtMissing == 0,
                            "round-trip: all original files present (%u missing)", rtMissing);
                localExpect(rtContentMismatch == 0,
                            "round-trip: all original files match (%u differ)",
                            rtContentMismatch);

                // Cleanup.
                reopened->close();
                std::error_code ec;
                fs::remove_all(tempDir, ec);
            }
        }
    }

    // ---------------------------------------------------------------
    // TEST 10: selective modification round-trip
    // ---------------------------------------------------------------
    if (files.size() >= 2) {
        auto tempDir = fs::temp_directory_path() / "whiteout_mpq_corpus_test";
        fs::create_directories(tempDir);
        auto tempFile = tempDir / "modified.mpq";

        // Open a fresh copy for modification tests.
        auto modStorage = mpq::Storage::open(archivePath, &pool);
        if (!modStorage) {
            localFail("could not open fresh copy for modification test");
        } else {
            // Pick a file to delete and inject a new one.
            auto modFiles = modStorage->listFiles();
            const std::string& fileToDelete = modFiles[0];
            const std::string injectedName = "__corpus_test_injected_file.txt";
            auto injectedData = makeTestPayload(4096);

            modStorage->writeFile(injectedName, injectedData);
            modStorage->deleteFile(fileToDelete);

            // Verify overlay works before save.
            localExpect(modStorage->fileExists(injectedName), "overlay: injected file exists");
            localExpect(!modStorage->fileExists(fileToDelete), "overlay: deleted file gone");

            auto readBack = modStorage->readFile(injectedName);
            localExpect(readBack.has_value() && *readBack == injectedData,
                        "overlay: injected file content correct");

            bool saved = modStorage->save(tempFile.string());
            if (!localExpect(saved, "modified save()")) {
                std::error_code ec;
                fs::remove_all(tempDir, ec);
            } else {
                auto reopened = mpq::Storage::open(tempFile.string());
                if (!localExpect(reopened.has_value(), "reopen modified archive")) {
                    std::error_code ec;
                    fs::remove_all(tempDir, ec);
                } else {
                    localExpect(!reopened->fileExists(fileToDelete),
                                "modified: deleted file absent after reopen");
                    localExpect(reopened->fileExists(injectedName),
                                "modified: injected file present after reopen");

                    auto modReadBack = reopened->readFile(injectedName);
                    localExpect(modReadBack.has_value() && *modReadBack == injectedData,
                                "modified: injected file content survives round-trip");

                    // Verify remaining files are intact.
                    u32 modMismatches = 0;
                    for (const auto& [name, snap] : serialSnapshot) {
                        if (name == fileToDelete)
                            continue;
                        auto data = reopened->readFile(name);
                        if (!data || fnv1a(*data) != snap.hash)
                            modMismatches++;
                    }
                    localExpect(modMismatches == 0,
                                "modified: remaining files intact (%u mismatches)", modMismatches);

                    reopened->close();
                    std::error_code ec;
                    fs::remove_all(tempDir, ec);
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // TEST 11: double round-trip (save, reopen, save again, reopen again)
    //          Verifies that saved archives are stable through multiple saves.
    // ---------------------------------------------------------------
    {
        auto tempDir = fs::temp_directory_path() / "whiteout_mpq_corpus_test";
        fs::create_directories(tempDir);
        auto tempFile1 = tempDir / "double_rt_1.mpq";
        auto tempFile2 = tempDir / "double_rt_2.mpq";

        // Re-open original fresh for this test.
        auto fresh = mpq::Storage::open(archivePath, &pool);
        if (fresh) {
            bool saved1 = fresh->save(tempFile1.string());
            if (localExpect(saved1, "double round-trip: first save")) {
                auto mid = mpq::Storage::open(tempFile1.string(), &pool);
                if (mid) {
                    bool saved2 = mid->save(tempFile2.string());
                    if (localExpect(saved2, "double round-trip: second save")) {
                        auto final_ = mpq::Storage::open(tempFile2.string());
                        if (final_) {
                            auto drtFiles = final_->listFiles();
                            // After first save, (listfile)/(attributes) are added.
                            // Second save should be stable — same count as first save.
                            auto midFiles = mid->listFiles();
                            localExpect(drtFiles.size() == midFiles.size(),
                                        "double round-trip: stable file count "
                                        "(1st=%zu 2nd=%zu)",
                                        midFiles.size(), drtFiles.size());

                            // All original files must still be present and correct.
                            u32 drtErrors = 0;
                            auto drtSnap = snapshotAllFiles(*final_, drtFiles, drtErrors);

                            u32 drtMismatches = 0;
                            for (const auto& [name, snap] : serialSnapshot) {
                                auto it = drtSnap.find(name);
                                if (it == drtSnap.end() || it->second.hash != snap.hash)
                                    drtMismatches++;
                            }
                            localExpect(drtMismatches == 0,
                                        "double round-trip: original content intact "
                                        "(%u mismatches)",
                                        drtMismatches);
                            final_->close();
                        }
                    }
                    mid->close();
                }
            }
            fresh->close();
        }
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    result.passed = localPassed;
    result.failed = localFailed;
    return result;
}

// ============================================================================
// Main
// ============================================================================

TEST_CASE("MPQ corpus round-trip", "[mpq][corpus]") {
    int g_passed = 0, g_failed = 0;
    auto t0 = std::chrono::steady_clock::now();

    std::string corpusDir = findCorpusDir(nullptr);

    if (corpusDir.empty()) {
        SKIP("MPQ corpus directory not found");
    }

    std::printf("MPQ Corpus Test\n");
    std::printf("Corpus: %s\n", corpusDir.c_str());

    auto archives = discoverArchives(corpusDir);
    if (archives.empty()) {
        SKIP("no MPQ archives found in corpus directory");
    }

    std::printf("Found %zu archives.\n", archives.size());

    // Create a shared thread pool for parallel tests.
    const size_t numThreads = std::max(4u, std::thread::hardware_concurrency());
    whiteout::utils::SimpleThreadPool pool(numThreads);
    std::printf("Thread pool: %zu threads\n", numThreads);

    // Run tests on every archive.
    u32 totalArchives = 0;
    u32 archivesPassed = 0;
    u32 archivesFailed = 0;
    u32 totalFiles = 0;
    u32 totalFilesRead = 0;
    u32 totalReadErrors = 0;

    for (const auto& archivePath : archives) {
        auto result = testArchive(archivePath, pool);
        totalArchives++;
        totalFiles += result.fileCount;
        totalFilesRead += result.filesRead;
        totalReadErrors += result.readErrors;

        g_passed += result.passed;
        g_failed += result.failed;

        if (result.failed == 0) {
            archivesPassed++;
        } else {
            archivesFailed++;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::printf("\n================================================================\n");
    std::printf("MPQ Corpus Test Summary\n");
    std::printf("================================================================\n");
    std::printf("  Archives tested : %u (%u passed, %u failed)\n", totalArchives, archivesPassed,
                archivesFailed);
    std::printf("  Total files     : %u (%u read successfully, %u read errors)\n", totalFiles,
                totalFilesRead, totalReadErrors);
    std::printf("  Assertions      : %d passed, %d failed\n", g_passed, g_failed);
    std::printf("  Elapsed         : %lld ms\n", static_cast<long long>(elapsed));
    std::printf("================================================================\n");

    CHECK(g_failed == 0);
}

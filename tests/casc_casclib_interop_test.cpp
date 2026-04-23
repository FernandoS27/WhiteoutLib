// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_casclib_interop_test.cpp
/// @brief Interoperability test: write CASC storages with WhiteoutLib,
///        read them back with CascLib's C API.
///
/// Validates that storages produced by our writer are accepted by CascLib
/// for every root format that CascLib can parse:
///   - TVFS (Warcraft III Reforged / general purpose)
///   - WoW MFST (World of Warcraft legacy root)
///   - Diablo III (hierarchical directory root)
///
/// Requires -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON to build with CascLib.
/// Without that flag, all tests are SKIP'd.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
#include <CascLib.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>

#include <string>
#include <unordered_map>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

static std::vector<u8> makeTestData(size_t size, u8 seed = 0) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>((i * 7 + seed) & 0xFF);
    return data;
}

static void cleanDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

// ============================================================================
// Test payload — a set of named files with known content.
// ============================================================================

struct TestFile {
    std::string path;
    std::vector<u8> data;
};

/// Build a set of test files with varying sizes.
static std::vector<TestFile> buildTestFiles() {
    return {
        {"alpha.txt",                makeTestData(100, 0xAA)},
        {"data/config.bin",          makeTestData(4096, 0xBB)},
        {"data/textures/grass.dds",  makeTestData(16384, 0xCC)},
        {"models/tree.m2",           makeTestData(8192, 0xDD)},
        {"sounds/wind.ogg",          makeTestData(2048, 0xEE)},
        {"readme.md",               makeTestData(512, 0x11)},
        {"empty_section/marker.bin", makeTestData(64, 0x22)},
    };
}

// ============================================================================
// CascLib helpers (only when cross-val is enabled)
// ============================================================================

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)

/// RAII wrapper for a CascLib storage handle.
struct ScopedCascStorage {
    HANDLE h = nullptr;
    ~ScopedCascStorage() { if (h) CascCloseStorage(h); }
    explicit operator bool() const { return h != nullptr; }
};

/// RAII wrapper for a CascLib file handle.
struct ScopedCascFile {
    HANDLE h = nullptr;
    ~ScopedCascFile() { if (h) CascCloseFile(h); }
    explicit operator bool() const { return h != nullptr; }
};

/// RAII wrapper for a CascLib find handle.
struct ScopedCascFind {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~ScopedCascFind() { if (h != INVALID_HANDLE_VALUE) CascFindClose(h); }
    explicit operator bool() const { return h != INVALID_HANDLE_VALUE; }
};

/// Read an entire file from CascLib by path. Returns empty on failure.
static std::vector<u8> cascReadFile(HANDLE hStorage, const std::string& path) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, path.c_str(), 0, CASC_OPEN_BY_NAME, &file.h))
        return {};

    ULONGLONG fileSize64 = 0;
    if (!CascGetFileSize64(file.h, &fileSize64))
        return {};

    auto fileSize = static_cast<size_t>(fileSize64);
    std::vector<u8> buffer(fileSize);
    size_t totalRead = 0;

    while (totalRead < fileSize) {
        auto chunk = static_cast<DWORD>(std::min<size_t>(fileSize - totalRead, 0x10000000u));
        DWORD bytesRead = 0;
        if (!CascReadFile(file.h, buffer.data() + totalRead, chunk, &bytesRead)) {
            DWORD err = GetCascError();
            if (err != ERROR_HANDLE_EOF)
                return {};
            totalRead += bytesRead;
            break;
        }
        totalRead += bytesRead;
        if (bytesRead == 0) break;
    }
    buffer.resize(totalRead);
    return buffer;
}

/// Read a file from CascLib by FileDataId. Returns empty on failure.
static std::vector<u8> cascReadFileById(HANDLE hStorage, u32 fileId) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, CASC_FILE_DATA_ID(fileId), 0, CASC_OPEN_BY_FILEID, &file.h))
        return {};

    ULONGLONG fileSize64 = 0;
    if (!CascGetFileSize64(file.h, &fileSize64))
        return {};

    auto fileSize = static_cast<size_t>(fileSize64);
    std::vector<u8> buffer(fileSize);
    size_t totalRead = 0;

    while (totalRead < fileSize) {
        auto chunk = static_cast<DWORD>(std::min<size_t>(fileSize - totalRead, 0x10000000u));
        DWORD bytesRead = 0;
        if (!CascReadFile(file.h, buffer.data() + totalRead, chunk, &bytesRead)) {
            DWORD err = GetCascError();
            if (err != ERROR_HANDLE_EOF)
                return {};
            totalRead += bytesRead;
            break;
        }
        totalRead += bytesRead;
        if (bytesRead == 0) break;
    }
    buffer.resize(totalRead);
    return buffer;
}

/// Enumerate all files from CascLib, returning path → FileDataId map.
/// Paths are normalized to use forward slashes for cross-platform comparison.
static std::unordered_map<std::string, DWORD> cascEnumerateFiles(HANDLE hStorage) {
    std::unordered_map<std::string, DWORD> result;
    CASC_FIND_DATA findData{};
    ScopedCascFind hFind;
    hFind.h = CascFindFirstFile(hStorage, "*", &findData, nullptr);
    if (!hFind) return result;
    do {
        if (findData.szFileName[0] != '\0' && findData.NameType == CascNameFull) {
            std::string name = findData.szFileName;
            std::replace(name.begin(), name.end(), '\\', '/');
            result.emplace(std::move(name), findData.dwFileDataId);
        }
    } while (CascFindNextFile(hFind.h, &findData));
    return result;
}

/// Core validation: write a storage with the given root format, open it with
/// CascLib, and verify every file can be enumerated and read back identically.
static void validateWithCascLib(const std::string& testDir,
                                RootFormat rootFormat,
                                const std::vector<TestFile>& files,
                                bool useFileDataIds) {
    cleanDir(testDir);

    // --- Write storage with WhiteoutLib ---
    CreateOptions opts;
    opts.product = "interop_test";
    opts.version = "1.0.0";
    opts.rootFormat = rootFormat;
    auto storage = StorageWritable::create(opts);

    // Track assigned FileDataIds for formats that support ID-based lookup.
    std::unordered_map<u32, size_t> idToFileIndex;

    for (size_t i = 0; i < files.size(); ++i) {
        REQUIRE(storage.writeFile(files[i].path, files[i].data));
    }

    REQUIRE(storage.save(testDir));

    // --- Open with CascLib ---
    ScopedCascStorage cascStorage;

    // Use CascOpenStorageEx with a progress callback to diagnose which step fails. 
    CASC_OPEN_STORAGE_ARGS openArgs{};
    openArgs.Size = sizeof(CASC_OPEN_STORAGE_ARGS);
    openArgs.dwLocaleMask = CASC_LOCALE_ALL;

    std::vector<std::string> progressLog;
    openArgs.PfnProgressCallback = [](void* param, CASC_PROGRESS_MSG msg,
                                      LPCSTR szObject, DWORD current, DWORD total) -> bool {
        auto* log = static_cast<std::vector<std::string>*>(param);
        log->push_back(std::string(szObject ? szObject : "(null)") +
                       " [msg=" + std::to_string(msg) +
                       " cur=" + std::to_string(current) +
                       " total=" + std::to_string(total) + "]");
        return false; // don't cancel
    };
    openArgs.PtrProgressParam = &progressLog;

    bool opened = CascOpenStorageEx(testDir.c_str(), &openArgs, false, &cascStorage.h);
    if (!opened) {
        DWORD err = GetCascError();
        std::string listing;
        for (auto& e : fs::recursive_directory_iterator(testDir))
            listing += "  " + e.path().string() + "\n";
        std::string allProgress;
        for (auto& p : progressLog)
            allProgress += "  " + p + "\n";
        FAIL("CascLib failed to open storage at '" << testDir
             << "', error code: " << err
             << "\nProgress log:\n" << allProgress
             << "Directory listing:\n" << listing);
    }

    // --- Enumerate files with CascLib ---
    auto cascFiles = cascEnumerateFiles(cascStorage.h);
    INFO("CascLib enumerated " << cascFiles.size() << " files");

    // Every test file must appear in the CascLib enumeration.
    for (auto& tf : files) {
        auto it = cascFiles.find(tf.path);
        INFO("CascLib did not find file: " << tf.path);
        CHECK(it != cascFiles.end());
    }

    // --- Read each file via CascLib and compare content ---
    for (auto& tf : files) {
        DYNAMIC_SECTION("Read " << tf.path) {
            auto cascData = cascReadFile(cascStorage.h, tf.path);
            REQUIRE(cascData.size() == tf.data.size());
            CHECK(cascData == tf.data);
        }
    }

    // --- Also verify our own reader can round-trip ---
    {
        auto readStorage = Storage::open(testDir);
        REQUIRE(readStorage.has_value());
        for (auto& tf : files) {
            auto data = readStorage->readFile(tf.path);
            REQUIRE(data.has_value());
            CHECK(data->size() == tf.data.size());
            CHECK(*data == tf.data);
        }
    }

    cleanDir(testDir);
}

#endif // WHITEOUT_HAS_CASCLIB_CROSSVAL

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("CascLib reads TVFS storage", "[casc][casclib_interop]") {
#if !defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    SKIP("CascLib cross-validation not enabled (pass -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON)");
#else
    auto files = buildTestFiles();
    validateWithCascLib("test_casclib_tvfs", RootFormat::Tvfs, files, false);
#endif
}

TEST_CASE("CascLib reads WoW MFST storage", "[casc][casclib_interop]") {
#if !defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    SKIP("CascLib cross-validation not enabled (pass -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON)");
#else
    auto files = buildTestFiles();
    const std::string testDir = "test_casclib_wow";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "wow_interop";
    opts.version = "1.0.0";
    opts.rootFormat = RootFormat::Wow;
    auto storage = StorageWritable::create(opts);

    // Write files using explicit FileDataIds so we know the mapping.
    for (size_t i = 0; i < files.size(); ++i) {
        i32 fdi = static_cast<i32>(1000 + i);
        REQUIRE(storage.writeFile(fdi, files[i].data));
    }

    REQUIRE(storage.save(testDir));

    // --- Open with CascLib ---
    ScopedCascStorage cascStorage;
    bool opened = CascOpenStorage(testDir.c_str(), CASC_LOCALE_ALL, &cascStorage.h);
    if (!opened) {
        DWORD err = GetCascError();
        std::string listing;
        for (auto& e : fs::recursive_directory_iterator(testDir))
            listing += "  " + e.path().string() + "\n";
        FAIL("CascLib failed to open WoW storage at '" << testDir
             << "', error code: " << err << "\nDirectory listing:\n" << listing);
    }

    // --- Verify every file via CascLib by FileDataId ---
    for (size_t i = 0; i < files.size(); ++i) {
        DWORD fdi = static_cast<DWORD>(1000 + i);
        DYNAMIC_SECTION("Read by FileDataId " << fdi) {
            auto cascData = cascReadFileById(cascStorage.h, fdi);
            REQUIRE(cascData.size() == files[i].data.size());
            CHECK(cascData == files[i].data);
        }
    }

    cleanDir(testDir);
#endif
}

TEST_CASE("CascLib reads Diablo 3 storage", "[casc][casclib_interop]") {
#if !defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    SKIP("CascLib cross-validation not enabled (pass -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON)");
#else
    // CascLib's D3 root handler requires "Base\CoreTOC.dat" to exist.
    // Create a minimal valid CoreTOC: 844 bytes of zeros (DIABLO3_CORE_TOC_HEADER
    // has 3 arrays of 70 DWORDs + 1 Alignment DWORD — all zero = no asset entries).
    std::vector<u8> coreTocData(844, 0);

    auto files = buildTestFiles();
    const std::string testDir = "test_casclib_d3";
    cleanDir(testDir);

    CreateOptions opts;
    opts.product = "d3_interop";
    opts.version = "1.0.0";
    opts.rootFormat = RootFormat::Diablo3;
    auto storage = StorageWritable::create(opts);

    // Write CoreTOC.dat first (CascLib requires this for D3 root).
    REQUIRE(storage.writeFile("Base\\CoreTOC.dat", coreTocData));

    for (auto& tf : files)
        REQUIRE(storage.writeFile(tf.path, tf.data));

    REQUIRE(storage.save(testDir));

    // --- Verify our own reader works first (diagnostics) ---
    {
        auto readStorage = Storage::open(testDir);
        REQUIRE(readStorage.has_value());
        auto ourFiles = readStorage->listFiles();
        INFO("Our reader lists " << ourFiles.size() << " files");
        for (auto& tf : files) {
            auto data = readStorage->readFile(tf.path);
            INFO("Our reader: " << tf.path << " -> " << (data ? "OK" : "MISSING"));
            REQUIRE(data.has_value());
            CHECK(*data == tf.data);
        }
        // Explicitly check CoreTOC.dat is readable via our reader.
        auto tocData = readStorage->readFile("Base\\CoreTOC.dat");
        INFO("Our reader: Base\\CoreTOC.dat -> " << (tocData ? "OK" : "MISSING"));
        REQUIRE(tocData.has_value());
    }

    // --- Open with CascLib ---
    ScopedCascStorage cascStorage;

    CASC_OPEN_STORAGE_ARGS openArgs{};
    openArgs.Size = sizeof(CASC_OPEN_STORAGE_ARGS);
    openArgs.dwLocaleMask = CASC_LOCALE_ALL;

    bool opened = CascOpenStorageEx(testDir.c_str(), &openArgs, false, &cascStorage.h);
    if (!opened) {
        DWORD err = GetCascError();
        std::string listing;
        for (auto& e : fs::recursive_directory_iterator(testDir))
            listing += "  " + e.path().string() + "\n";
        FAIL("CascLib failed to open D3 storage at '" << testDir
             << "', error code: " << err
             << "\nDirectory listing:\n" << listing);
    }

    auto cascFiles = cascEnumerateFiles(cascStorage.h);
    INFO("CascLib enumerated " << cascFiles.size() << " files");

    // Read each test file via CascLib by name.
    for (auto& tf : files) {
        DYNAMIC_SECTION("Read " << tf.path) {
            auto cascData = cascReadFile(cascStorage.h, tf.path);
            REQUIRE(cascData.size() == tf.data.size());
            CHECK(cascData == tf.data);
        }
    }

    cleanDir(testDir);
#endif
}

TEST_CASE("CascLib reads storage with many files", "[casc][casclib_interop]") {
#if !defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    SKIP("CascLib cross-validation not enabled (pass -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON)");
#else
    // Stress test: 100 files across different directories.
    const std::string testDir = "test_casclib_stress";
    cleanDir(testDir);

    std::vector<TestFile> files;
    files.reserve(100);
    for (int i = 0; i < 100; ++i) {
        std::string dir = "dir" + std::to_string(i / 10);
        std::string name = "file" + std::to_string(i) + ".bin";
        files.push_back({dir + "/" + name, makeTestData(256 + i * 50, u8(i))});
    }

    validateWithCascLib(testDir, RootFormat::Tvfs, files, false);
#endif
}

TEST_CASE("CascLib reads storage with empty file", "[casc][casclib_interop]") {
#if !defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    SKIP("CascLib cross-validation not enabled (pass -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON)");
#else
    const std::string testDir = "test_casclib_empty";
    cleanDir(testDir);

    std::vector<TestFile> files = {
        {"nonempty.bin", makeTestData(128, 0x42)},
        {"empty.bin",    {}},
        {"also_data.txt", makeTestData(64, 0x99)},
    };

    validateWithCascLib(testDir, RootFormat::Tvfs, files, false);
#endif
}

TEST_CASE("CascLib reads storage with large file", "[casc][casclib_interop]") {
#if !defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    SKIP("CascLib cross-validation not enabled (pass -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON)");
#else
    // A file larger than the BLTE frame size (64KB) to test multi-frame BLTE.
    const std::string testDir = "test_casclib_large";
    cleanDir(testDir);

    std::vector<TestFile> files = {
        {"small.bin",  makeTestData(100, 0x01)},
        {"large.bin",  makeTestData(200000, 0x02)}, // ~200 KB, multiple BLTE frames
        {"medium.bin", makeTestData(70000, 0x03)},   // ~70 KB, just above frame size
    };

    validateWithCascLib(testDir, RootFormat::Tvfs, files, false);
#endif
}

TEST_CASE("CascLib reads Overwatch storage", "[casc][casclib_interop]") {
#if !defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    SKIP("CascLib cross-validation not enabled (pass -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON)");
#else
    // Overwatch uses a text-based CSV root file.  CascLib's
    // RootHandler_CreateOverwatch parses pipe-delimited CSV with FILENAME and
    // MD5 columns, then scans for .cmf/.apm files in Manifest/ or
    // TactManifest/ directories.
    //
    // We test:
    //  1. Basic file paths are enumerable and readable via CascLib.
    //  2. Paths with subdirectories resolve correctly.
    //  3. File content round-trips exactly.
    //
    // File paths must NOT start with "Manifest/" or "TactManifest/" to avoid
    // triggering CascLib's CMF/APM sub-parsing on our synthetic test data.
    auto files = buildTestFiles();
    validateWithCascLib("test_casclib_ow", RootFormat::Overwatch, files, false);
#endif
}

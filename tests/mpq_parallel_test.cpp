// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MPQ parallel (WorkerPool) test suite — TDD tests for the parallelism plan.
//
// Every test runs twice: once without a pool (serial, pool = nullptr) and once
// with a real 4-thread pool (parallel).  Results must be correct in both modes.
//
// Tests cover:
//   1.  Plumbing:      open/create accept WorkerPool* without changing behavior
//   2.  Encode:        encodeFileData with pool produces identical results to serial
//   3.  Decode:        extractFileData with pool produces identical results to serial
//   4.  Writer:        writeArchive with pool produces a valid, intact archive
//   5.  End-to-end:    Full Storage round-trip with pool (save + reopen + readFile)
//   6.  Edge cases:    Thresholds, single-unit, encrypted, small files
//   7.  Correctness:   Pool actually executes tasks on worker threads (parallel run only)

#include <whiteout/storages/mpq/storage.h>
#include <whiteout/interfaces.h>

// Internal headers — test exercises internals directly.
#include "whiteout/storages/mpq/file_data.h"
#include "whiteout/storages/mpq/writer.h"

#include <fstream>
#include "whiteout/storages/mpq/tables/header.h"
#include "whiteout/storages/mpq/tables/block_table.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace mpq = whiteout::storages::mpq;

using whiteout::u8;
using whiteout::u16;
using whiteout::u32;
using whiteout::u64;

// ============================================================================
// Test WorkerPool Implementation
// ============================================================================

/// A real multi-threaded WorkerPool for testing parallel code paths.
/// Tracks submitted task count and which thread IDs executed them.
class TestWorkerPool : public whiteout::interfaces::WorkerPool {
public:
    explicit TestWorkerPool(size_t numThreads = 4) {
        for (size_t i = 0; i < numThreads; ++i) {
            m_threads.emplace_back([this] { workerLoop(); });
        }
    }

    ~TestWorkerPool() override {
        {
            std::lock_guard lock(m_mutex);
            m_shutdown = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads) {
            t.join();
        }
    }

    void submit(const whiteout::interfaces::WorkerTask& task) override {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push(task.fn);
            m_totalSubmitted.fetch_add(1, std::memory_order_relaxed);
        }
        m_cv.notify_one();
    }

    void waitIdle() override {
        std::unique_lock lock(m_mutex);
        m_idleCv.wait(lock, [this] {
            return m_queue.empty() && m_activeCount == 0;
        });
    }

    size_t threadCount() const noexcept override {
        return m_threads.size();
    }

    /// Number of tasks submitted since construction (or last reset).
    size_t totalSubmitted() const {
        return m_totalSubmitted.load(std::memory_order_relaxed);
    }

    /// Number of unique thread IDs that executed tasks.
    size_t uniqueWorkerThreads() const {
        std::lock_guard lock(m_threadIdMutex);
        return m_workerThreadIds.size();
    }

    /// Reset counters for a new test.
    void resetCounters() {
        m_totalSubmitted.store(0, std::memory_order_relaxed);
        std::lock_guard lock(m_threadIdMutex);
        m_workerThreadIds.clear();
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return !m_queue.empty() || m_shutdown; });
                if (m_shutdown && m_queue.empty()) return;
                task = std::move(m_queue.front());
                m_queue.pop();
                ++m_activeCount;
            }

            // Track which threads do work.
            {
                std::lock_guard lock(m_threadIdMutex);
                m_workerThreadIds.insert(std::this_thread::get_id());
            }

            task();

            {
                std::lock_guard lock(m_mutex);
                --m_activeCount;
            }
            m_idleCv.notify_all();
        }
    }

    std::vector<std::thread> m_threads;
    std::queue<std::function<void()>> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    int m_activeCount = 0;
    bool m_shutdown = false;

    std::atomic<size_t> m_totalSubmitted{0};
    mutable std::mutex m_threadIdMutex;
    std::set<std::thread::id> m_workerThreadIds;
};

// ============================================================================
// Test Helpers
// ============================================================================

namespace {

int g_passed = 0;
int g_failed = 0;
const char* g_runLabel = "serial";

bool fail(int testNumber, const char* fmt, ...) {
    std::printf("[%s] TEST %d FAIL: ", g_runLabel, testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    ++g_failed;
    return false;
}

bool pass(int testNumber, const char* fmt, ...) {
    std::printf("[%s] TEST %d PASS: ", g_runLabel, testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    ++g_passed;
    return true;
}

bool expect(bool condition, int testNumber, const char* fmt, ...) {
    if (condition) return true;
    std::printf("[%s] TEST %d FAIL: ", g_runLabel, testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return false;
}

/// Generate a data pattern that is compressible (repeating phrase).
std::vector<u8> makeCompressibleData(size_t size) {
    static constexpr char kPhrase[] = "whiteout-mpq-parallel-test-data-block-";
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>(kPhrase[i % (sizeof(kPhrase) - 1)]);
    }
    return data;
}

/// Generate a deterministic pseudo-random data pattern (less compressible).
std::vector<u8> makePatternData(size_t size, u8 seed) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>((i * 13 + (i / 17) * 7 + seed) & 0xFF);
    }
    return data;
}

/// Compare two byte vectors for equality.
bool vectorsEqual(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

void cleanup(const fs::path& tempDir) {
    std::error_code ec;
    fs::remove_all(tempDir, ec);
}

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

/// Test 1: open(path, pool) and create(opts, pool) compile and work correctly.
/// The pool parameter plumbing doesn't alter existing behavior.
bool test1_plumbing_open_create(const fs::path& tempDir, TestWorkerPool* pool) {
    constexpr int T = 1;

    // create() with pool should produce a valid storage.
    auto storage = mpq::Storage::create({}, pool);
    if (!expect(static_cast<bool>(storage), T, "create(opts, pool) returned invalid")) return false;

    // Write a file and save.
    auto data = makeCompressibleData(1000);
    storage.writeFile("test.txt", data);
    auto path = (tempDir / "plumbing.mpq").string();
    if (!expect(storage.save(path), T, "save failed")) return false;

    // open() with pool should produce a valid storage.
    std::string err;
    auto opened = mpq::Storage::open(path, &err, pool);
    if (!expect(opened.has_value(), T, "open(path, err, pool) failed: %s", err.c_str())) return false;

    // Read back should match.
    auto readBack = opened->readFile("test.txt");
    if (!expect(readBack.has_value(), T, "readFile after open(pool) failed")) return false;
    if (!expect(vectorsEqual(*readBack, data), T, "data mismatch after open(pool)")) return false;

    // open() single-arg convenience should still work.
    auto opened2 = mpq::Storage::open(path);
    if (!expect(opened2.has_value(), T, "open(path) convenience overload broken")) return false;

    return pass(T, "plumbing: open/create with WorkerPool* work correctly");
}

/// Test 2: encodeFileData with pool produces identical output to serial (multi-sector).
/// This verifies the parallel compression path matches byte-for-byte.
bool test2_encode_serial_equivalence(TestWorkerPool* pool) {
    constexpr int T = 2;

    // Create data that spans many sectors (4096-byte sectors, ~25 sectors).
    auto rawData = makeCompressibleData(100000);

    mpq::EncodeOptions opts;
    opts.compression = mpq::CompressionFlag::kZlib;
    opts.encrypt = false;
    opts.singleUnit = false;
    opts.sectorSize = 4096;
    opts.filename = "bigfile.dat";

    // Encode serially (pool = nullptr).
    auto serialResult = mpq::encodeFileData(std::span<const u8>(rawData), opts, nullptr);

    // Encode with pool (or serially if pool == nullptr).
    if (pool) pool->resetCounters();
    auto parallelResult = mpq::encodeFileData(std::span<const u8>(rawData), opts, pool);

    // Results should be identical.
    if (!expect(serialResult.compressedSize == parallelResult.compressedSize, T,
                "compressed size mismatch: serial=%u parallel=%u",
                serialResult.compressedSize, parallelResult.compressedSize)) return false;
    if (!expect(serialResult.flags == parallelResult.flags, T,
                "flags mismatch: serial=0x%08X parallel=0x%08X",
                static_cast<u32>(serialResult.flags), static_cast<u32>(parallelResult.flags))) return false;
    if (!expect(vectorsEqual(serialResult.data, parallelResult.data), T,
                "encoded data differs between serial and parallel")) return false;

    // Pool should have been used (numSectors >= 2).
    if (pool) {
        if (!expect(pool->totalSubmitted() > 0, T,
                    "pool was not used for multi-sector encode (submitted=%zu)",
                    pool->totalSubmitted())) return false;
    }

    return pass(T, "encodeFileData: parallel == serial for %zu-byte file (%zu tasks)",
                rawData.size(), pool ? pool->totalSubmitted() : size_t{0});
}

/// Test 3: encodeFileData with pool + encryption produces identical output to serial.
bool test3_encode_encrypted_equivalence(TestWorkerPool* pool) {
    constexpr int T = 3;

    auto rawData = makeCompressibleData(50000);

    mpq::EncodeOptions opts;
    opts.compression = mpq::CompressionFlag::kZlib;
    opts.encrypt = true;
    opts.singleUnit = false;
    opts.sectorSize = 4096;
    opts.filename = "encrypted\\secret.dat";

    auto serialResult = mpq::encodeFileData(std::span<const u8>(rawData), opts, nullptr);

    if (pool) pool->resetCounters();
    auto parallelResult = mpq::encodeFileData(std::span<const u8>(rawData), opts, pool);

    if (!expect(vectorsEqual(serialResult.data, parallelResult.data), T,
                "encrypted encoded data differs between serial and parallel")) return false;
    if (!expect(serialResult.flags == parallelResult.flags, T,
                "flags mismatch for encrypted encode")) return false;

    return pass(T, "encodeFileData: encrypted parallel == serial");
}

/// Test 4: encodeFileData for single-unit files stays serial (threshold bypass).
bool test4_encode_single_unit_serial(TestWorkerPool* pool) {
    constexpr int T = 4;

    auto rawData = makeCompressibleData(5000);

    mpq::EncodeOptions opts;
    opts.compression = mpq::CompressionFlag::kZlib;
    opts.encrypt = false;
    opts.singleUnit = true;
    opts.sectorSize = 4096;
    opts.filename = "single.dat";

    if (pool) pool->resetCounters();
    auto result = mpq::encodeFileData(std::span<const u8>(rawData), opts, pool);

    // Single-unit should not use the pool.
    if (pool) {
        if (!expect(pool->totalSubmitted() == 0, T,
                    "pool was used for single-unit file (submitted=%zu)",
                    pool->totalSubmitted())) return false;
    }
    if (!expect(!result.data.empty(), T, "single-unit encode produced empty data")) return false;

    return pass(T, "encodeFileData: single-unit bypasses pool (0 tasks)");
}

/// Test 5: encodeFileData for tiny files (< 2 sectors) stays serial.
bool test5_encode_below_threshold(TestWorkerPool* pool) {
    constexpr int T = 5;

    // 1 sector exactly (sectorSize = 4096, data size < 4096).
    auto rawData = makeCompressibleData(3000);

    mpq::EncodeOptions opts;
    opts.compression = mpq::CompressionFlag::kZlib;
    opts.encrypt = false;
    opts.singleUnit = false;
    opts.sectorSize = 4096;
    opts.filename = "tiny.dat";

    if (pool) pool->resetCounters();
    auto result = mpq::encodeFileData(std::span<const u8>(rawData), opts, pool);

    // < 2 sectors → serial path.
    if (pool) {
        if (!expect(pool->totalSubmitted() == 0, T,
                    "pool was used for 1-sector file (submitted=%zu)",
                    pool->totalSubmitted())) return false;
    }

    // Should still produce valid output.
    if (!expect(!result.data.empty(), T, "1-sector encode produced empty data")) return false;

    return pass(T, "encodeFileData: below-threshold (<2 sectors) bypasses pool");
}

/// Test 6: extractFileData with pool produces identical output to serial.
/// Round-trips: encode → extract.
bool test6_extract_parallel_roundtrip(TestWorkerPool* pool) {
    constexpr int T = 6;

    auto rawData = makeCompressibleData(80000);

    // Encode the data (serially — this is the "ground truth").
    mpq::EncodeOptions encOpts;
    encOpts.compression = mpq::CompressionFlag::kZlib;
    encOpts.encrypt = false;
    encOpts.singleUnit = false;
    encOpts.sectorSize = 4096;
    encOpts.filename = "roundtrip.dat";

    auto encoded = mpq::encodeFileData(std::span<const u8>(rawData), encOpts, nullptr);
    if (!expect(!encoded.data.empty(), T, "encode failed")) return false;

    // Build a mock block entry for extraction.
    mpq::BlockEntry block;
    block.fileOffset = 0;
    block.compressedSize = encoded.compressedSize;
    block.uncompressedSize = static_cast<u32>(rawData.size());
    block.flags = encoded.flags;

    // Extract serially.
    std::string serialErr;
    auto serialExtract = mpq::extractFileData(
        std::span<const u8>(encoded.data), 0, block, encOpts.sectorSize, 0, &serialErr, nullptr);
    if (!expect(!serialExtract.empty(), T, "serial extract failed: %s", serialErr.c_str())) return false;
    if (!expect(vectorsEqual(serialExtract, rawData), T, "serial extract data mismatch")) return false;

    // Extract with pool (or serially if pool == nullptr).
    if (pool) pool->resetCounters();
    std::string parallelErr;
    auto parallelExtract = mpq::extractFileData(
        std::span<const u8>(encoded.data), 0, block, encOpts.sectorSize, 0, &parallelErr, pool);
    if (!expect(!parallelExtract.empty(), T, "parallel extract failed: %s", parallelErr.c_str())) return false;
    if (!expect(vectorsEqual(parallelExtract, rawData), T, "parallel extract data mismatch")) return false;

    // Pool should have been used (file has ~20 sectors, threshold is 4).
    if (pool) {
        if (!expect(pool->totalSubmitted() > 0, T,
                    "pool was not used for multi-sector extract (submitted=%zu)",
                    pool->totalSubmitted())) return false;
    }

    return pass(T, "extractFileData: parallel round-trip correct (%zu tasks)",
                pool ? pool->totalSubmitted() : size_t{0});
}

/// Test 7: extractFileData with pool for encrypted+compressed round-trip.
bool test7_extract_encrypted_roundtrip(TestWorkerPool* pool) {
    constexpr int T = 7;

    auto rawData = makeCompressibleData(60000);

    mpq::EncodeOptions encOpts;
    encOpts.compression = mpq::CompressionFlag::kZlib;
    encOpts.encrypt = true;
    encOpts.singleUnit = false;
    encOpts.sectorSize = 4096;
    encOpts.filename = "encrypted\\data.bin";

    auto encoded = mpq::encodeFileData(std::span<const u8>(rawData), encOpts, nullptr);
    if (!expect(!encoded.data.empty(), T, "encrypted encode failed")) return false;

    mpq::BlockEntry block;
    block.fileOffset = 0;
    block.compressedSize = encoded.compressedSize;
    block.uncompressedSize = static_cast<u32>(rawData.size());
    block.flags = encoded.flags;

    u32 fileKey = mpq::deriveFileKey(encOpts.filename, block);

    // Serial extract.
    std::string serialErr;
    auto serialExtract = mpq::extractFileData(
        std::span<const u8>(encoded.data), 0, block, encOpts.sectorSize, fileKey, &serialErr, nullptr);
    if (!expect(!serialExtract.empty(), T, "serial encrypted extract failed: %s", serialErr.c_str())) return false;
    if (!expect(vectorsEqual(serialExtract, rawData), T, "serial encrypted extract mismatch")) return false;

    // Parallel extract.
    if (pool) pool->resetCounters();
    std::string parallelErr;
    auto parallelExtract = mpq::extractFileData(
        std::span<const u8>(encoded.data), 0, block, encOpts.sectorSize, fileKey, &parallelErr, pool);
    if (!expect(!parallelExtract.empty(), T, "parallel encrypted extract failed: %s", parallelErr.c_str())) return false;
    if (!expect(vectorsEqual(parallelExtract, rawData), T, "parallel encrypted extract mismatch")) return false;

    return pass(T, "extractFileData: encrypted parallel round-trip correct");
}

/// Test 8: extractFileData with pool for small file (< 4 sectors) stays serial.
bool test8_extract_below_threshold(TestWorkerPool* pool) {
    constexpr int T = 8;

    // 3 sectors: 3 * 4096 = 12288, we use 12000.
    auto rawData = makeCompressibleData(12000);

    mpq::EncodeOptions encOpts;
    encOpts.compression = mpq::CompressionFlag::kZlib;
    encOpts.encrypt = false;
    encOpts.singleUnit = false;
    encOpts.sectorSize = 4096;
    encOpts.filename = "small.dat";

    auto encoded = mpq::encodeFileData(std::span<const u8>(rawData), encOpts, nullptr);

    mpq::BlockEntry block;
    block.fileOffset = 0;
    block.compressedSize = encoded.compressedSize;
    block.uncompressedSize = static_cast<u32>(rawData.size());
    block.flags = encoded.flags;

    if (pool) pool->resetCounters();
    auto result = mpq::extractFileData(
        std::span<const u8>(encoded.data), 0, block, encOpts.sectorSize, 0, nullptr, pool);
    if (!expect(!result.empty(), T, "extract failed for small file")) return false;
    if (!expect(vectorsEqual(result, rawData), T, "data mismatch for small file")) return false;

    // < 4 sectors → serial path.
    if (pool) {
        if (!expect(pool->totalSubmitted() == 0, T,
                    "pool was used for <4 sectors (submitted=%zu, sectors=3)",
                    pool->totalSubmitted())) return false;
    }

    return pass(T, "extractFileData: below-threshold (<4 sectors) bypasses pool");
}

/// Test 9: writeArchive with pool produces a valid archive with identical file contents.
/// NOTE: Raw archive bytes may differ due to (attributes) timestamps, so we compare
/// by opening both archives and verifying file-level data equivalence.
bool test9_write_archive_parallel(const fs::path& tempDir, TestWorkerPool* pool) {
    constexpr int T = 9;

    // Build header.
    mpq::MpqHeader header;
    header.magic = 0x1A51504D;
    header.headerSize = 32;
    header.formatVersion = 0;
    header.sectorSizeShift = 3; // 4096
    header.hashTableEntries = 64;

    // Create multiple overlay entries (>= 2 to trigger file-level parallelism).
    std::vector<mpq::WriteEntry> entries;
    std::vector<std::vector<u8>> fileData;

    for (int i = 0; i < 10; ++i) {
        auto data = makeCompressibleData(20000 + i * 3000);
        mpq::WriteEntry we;
        we.filename = "file" + std::to_string(i) + ".dat";
        we.rawData = data;
        we.compression = 0x02;
        we.encrypt = false;
        we.singleUnit = false;
        entries.push_back(std::move(we));
        fileData.push_back(std::move(data));
    }

    // Write with pool (or serially if pool == nullptr).
    if (pool) pool->resetCounters();
    auto parallelArchive = mpq::writeArchive(header, entries, 64, pool);
    if (!expect(!parallelArchive.empty(), T, "parallel writeArchive produced empty output")) return false;

    // Save to disk and reopen to verify all files are extractable.
    auto archPath = (tempDir / "writer_parallel.mpq").string();
    {
        std::ofstream out(archPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(parallelArchive.data()),
                  static_cast<std::streamsize>(parallelArchive.size()));
        if (!expect(static_cast<bool>(out), T, "write to disk failed")) return false;
    }

    auto reopened = mpq::Storage::open(archPath);
    if (!expect(reopened.has_value(), T, "reopen parallel archive failed")) return false;

    for (int i = 0; i < 10; ++i) {
        std::string name = "file" + std::to_string(i) + ".dat";
        auto readBack = reopened->readFile(name);
        if (!expect(readBack.has_value(), T, "readFile failed for %s", name.c_str())) return false;
        if (!expect(vectorsEqual(*readBack, fileData[i]), T,
                    "data mismatch for %s (expected %zu, got %zu)",
                    name.c_str(), fileData[i].size(), readBack->size())) return false;
    }

    // Pool should have been used for multi-file parallelism.
    if (pool) {
        if (!expect(pool->totalSubmitted() > 0, T,
                    "pool was not used for multi-file writeArchive")) return false;
    }

    return pass(T, "writeArchive: parallel produces valid archive with %d files (%zu tasks)",
                (int)entries.size(), pool ? pool->totalSubmitted() : size_t{0});
}

/// Test 10: writeArchive with < 2 overlay entries uses sector-level parallelism instead.
bool test10_write_archive_single_file_fallback(const fs::path& tempDir, TestWorkerPool* pool) {
    constexpr int T = 10;

    mpq::MpqHeader header;
    header.magic = 0x1A51504D;
    header.headerSize = 32;
    header.formatVersion = 0;
    header.sectorSizeShift = 3;
    header.hashTableEntries = 16;

    // Only 1 overlay entry — should fall back to sector-level parallelism.
    auto largeData = makeCompressibleData(100000);
    mpq::WriteEntry we;
    we.filename = "onlyone.dat";
    we.rawData = largeData;
    we.compression = 0x02;
    we.singleUnit = false;
    std::vector<mpq::WriteEntry> entries = {std::move(we)};

    if (pool) pool->resetCounters();
    auto parallelArchive = mpq::writeArchive(header, entries, 16, pool);
    if (!expect(!parallelArchive.empty(), T, "parallel writeArchive produced empty output")) return false;

    // Save to disk and reopen to verify.
    auto archPath = (tempDir / "writer_single_fallback.mpq").string();
    {
        std::ofstream out(archPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(parallelArchive.data()),
                  static_cast<std::streamsize>(parallelArchive.size()));
    }

    auto reopened = mpq::Storage::open(archPath);
    if (!expect(reopened.has_value(), T, "reopen failed")) return false;

    auto readBack = reopened->readFile("onlyone.dat");
    if (!expect(readBack.has_value(), T, "readFile failed")) return false;
    if (!expect(vectorsEqual(*readBack, largeData), T, "data mismatch")) return false;

    // Pool should still have been used (sector-level for the one large file).
    if (pool) {
        if (!expect(pool->totalSubmitted() > 0, T,
                    "pool was not used even for sector-level parallelism")) return false;
    }

    return pass(T, "writeArchive: single overlay file uses sector-level parallelism (%zu tasks)",
                pool ? pool->totalSubmitted() : size_t{0});
}

/// Test 11: Full end-to-end Storage round-trip with pool.
/// create(pool) → writeFile (many files) → save(path) → open(path, pool) → readFile.
bool test11_end_to_end_with_pool(const fs::path& tempDir, TestWorkerPool* pool) {
    constexpr int T = 11;

    auto path = (tempDir / "e2e_parallel.mpq").string();

    // Create with pool.
    auto storage = mpq::Storage::create({}, pool);
    if (!expect(static_cast<bool>(storage), T, "create with pool failed")) return false;

    // Write a variety of files.
    struct TestFile {
        std::string name;
        std::vector<u8> data;
        mpq::WriteOptions opts;
    };

    std::vector<TestFile> files;

    // 1. Large compressible file (many sectors).
    {
        mpq::WriteOptions opts;
        opts.compression = mpq::Compression::Zlib;
        files.push_back({"maps\\large.bin", makeCompressibleData(120000), opts});
    }
    // 2. Medium file.
    {
        mpq::WriteOptions opts;
        opts.compression = mpq::Compression::Zlib;
        files.push_back({"data\\medium.dat", makeCompressibleData(30000), opts});
    }
    // 3. Single-unit file.
    {
        mpq::WriteOptions opts;
        opts.compression = mpq::Compression::Zlib;
        opts.singleUnit = true;
        files.push_back({"scripts\\small.txt", makePatternData(800, 0x42), opts});
    }
    // 4. Encrypted file.
    {
        mpq::WriteOptions opts;
        opts.compression = mpq::Compression::Zlib;
        opts.encrypt = true;
        files.push_back({"secure\\key.bin", makePatternData(2048, 0x77), opts});
    }
    // 5. Tiny uncompressed file.
    {
        mpq::WriteOptions opts;
        opts.compression = mpq::Compression::None;
        files.push_back({"readme.txt", {'H', 'e', 'l', 'l', 'o'}, opts});
    }
    // 6-10. Multiple medium files to trigger file-level parallelism on save.
    for (int i = 0; i < 5; ++i) {
        mpq::WriteOptions opts;
        opts.compression = mpq::Compression::Zlib;
        files.push_back({"batch\\file" + std::to_string(i) + ".dat",
                         makeCompressibleData(25000 + i * 5000), opts});
    }

    // Write all files.
    for (const auto& f : files) {
        if (!expect(storage.writeFile(f.name, f.data, f.opts), T,
                    "writeFile failed for %s", f.name.c_str())) return false;
    }

    // Save (this triggers writeArchive with pool).
    if (pool) pool->resetCounters();
    if (!expect(storage.save(path), T, "save with pool failed")) return false;

    size_t saveTasks = pool ? pool->totalSubmitted() : size_t{0};

    // Reopen with pool.
    std::string openErr;
    auto reopened = mpq::Storage::open(path, &openErr, pool);
    if (!expect(reopened.has_value(), T, "reopen with pool failed: %s", openErr.c_str())) return false;

    // Read back every file and verify contents.
    if (pool) pool->resetCounters();
    for (const auto& f : files) {
        auto readBack = reopened->readFile(f.name);
        if (!expect(readBack.has_value(), T,
                    "readFile failed for %s after reopen", f.name.c_str())) return false;
        if (!expect(vectorsEqual(*readBack, f.data), T,
                    "data mismatch for %s (expected %zu bytes, got %zu)",
                    f.name.c_str(), f.data.size(), readBack->size())) return false;
    }

    size_t readTasks = pool ? pool->totalSubmitted() : size_t{0};

    return pass(T, "end-to-end: %zu files round-tripped (save: %zu tasks, reads: %zu tasks)",
                files.size(), saveTasks, readTasks);
}

/// Test 12: Parallel encode + extract with PKware compression.
bool test12_pkware_compression_parallel(TestWorkerPool* pool) {
    constexpr int T = 12;

    auto rawData = makeCompressibleData(40000);

    mpq::EncodeOptions opts;
    opts.compression = 0x08; // PKware
    opts.encrypt = false;
    opts.singleUnit = false;
    opts.sectorSize = 4096;
    opts.filename = "pkware.dat";

    auto serialResult = mpq::encodeFileData(std::span<const u8>(rawData), opts, nullptr);

    if (pool) pool->resetCounters();
    auto parallelResult = mpq::encodeFileData(std::span<const u8>(rawData), opts, pool);

    if (!expect(vectorsEqual(serialResult.data, parallelResult.data), T,
                "PKware encoded data differs")) return false;

    // Now extract with pool.
    mpq::BlockEntry block;
    block.fileOffset = 0;
    block.compressedSize = parallelResult.compressedSize;
    block.uncompressedSize = static_cast<u32>(rawData.size());
    block.flags = parallelResult.flags;

    auto extracted = mpq::extractFileData(
        std::span<const u8>(parallelResult.data), 0, block, opts.sectorSize, 0, nullptr, pool);
    if (!expect(!extracted.empty(), T, "PKware parallel extract failed")) return false;
    if (!expect(vectorsEqual(extracted, rawData), T, "PKware parallel extract data mismatch")) return false;

    return pass(T, "PKware compression: parallel encode + extract correct");
}

/// Test 13: Verify pool tasks actually execute on worker threads (not main thread).
bool test13_tasks_run_on_worker_threads(TestWorkerPool* pool) {
    constexpr int T = 13;
    if (!pool) return pass(T, "tasks run on worker threads: skipped (no pool)");

    // Use a large enough file to get multiple sectors.
    auto rawData = makeCompressibleData(200000);

    mpq::EncodeOptions opts;
    opts.compression = 0x02;
    opts.encrypt = false;
    opts.singleUnit = false;
    opts.sectorSize = 4096;
    opts.filename = "threadcheck.dat";

    pool->resetCounters();
    auto result = mpq::encodeFileData(std::span<const u8>(rawData), opts, pool);

    if (!expect(!result.data.empty(), T, "encode failed")) return false;

    size_t submitted = pool->totalSubmitted();
    size_t uniqueThreads = pool->uniqueWorkerThreads();

    if (!expect(submitted > 1, T, "expected multiple tasks, got %zu", submitted)) return false;
    if (!expect(uniqueThreads >= 1, T, "no worker threads executed tasks")) return false;

    return pass(T, "pool tasks ran on %zu worker threads (%zu tasks submitted)",
                uniqueThreads, submitted);
}

/// Test 14: Overwrite-save round-trip preserves data with pool.
bool test14_overwrite_save_with_pool(const fs::path& tempDir, TestWorkerPool* pool) {
    constexpr int T = 14;

    auto path = (tempDir / "overwrite_parallel.mpq").string();

    // Create initial archive.
    auto storage = mpq::Storage::create({}, pool);
    auto dataA = makeCompressibleData(50000);
    auto dataB = makePatternData(1000, 0xAA);
    storage.writeFile("fileA.dat", dataA);
    storage.writeFile("fileB.dat", dataB);
    if (!expect(storage.save(path), T, "initial save failed")) return false;

    // Close the initial storage so it releases its file mapping.
    storage = mpq::Storage::create({});

    // Reopen, modify, save to same path.
    auto reopened = mpq::Storage::open(path, nullptr, pool);
    if (!expect(reopened.has_value(), T, "reopen failed")) return false;

    auto dataC = makeCompressibleData(70000);
    reopened->writeFile("fileC.dat", dataC);
    reopened->deleteFile("fileB.dat");

    // Overwrite-save.
    if (!expect(reopened->save(), T, "overwrite save failed")) return false;

    // Reopen again and verify.
    auto final_ = mpq::Storage::open(path, nullptr, pool);
    if (!expect(final_.has_value(), T, "final reopen failed")) return false;

    auto readA = final_->readFile("fileA.dat");
    if (!expect(readA.has_value() && vectorsEqual(*readA, dataA), T, "fileA mismatch")) return false;
    if (!expect(!final_->fileExists("fileB.dat"), T, "deleted fileB still exists")) return false;
    auto readC = final_->readFile("fileC.dat");
    if (!expect(readC.has_value() && vectorsEqual(*readC, dataC), T, "fileC mismatch")) return false;

    return pass(T, "overwrite-save with pool preserves data correctly");
}

/// Test 15: Stress test — many small files encoded in parallel.
bool test15_many_small_files(const fs::path& tempDir, TestWorkerPool* pool) {
    constexpr int T = 15;

    std::vector<std::string> filenames;
    std::vector<std::vector<u8>> origData;

    // Create an archive with 50 small-medium files.
    auto storage = mpq::Storage::create({}, pool);
    for (int i = 0; i < 50; ++i) {
        auto data = makeCompressibleData(5000 + i * 200);
        std::string name = "stress\\file" + std::to_string(i) + ".dat";
        mpq::WriteOptions wo;
        wo.compression = mpq::Compression::Zlib;
        wo.singleUnit = (i % 5 == 0); // Every 5th is single-unit.
        storage.writeFile(name, data, wo);
        filenames.push_back(std::move(name));
        origData.push_back(std::move(data));
    }

    auto archPath = (tempDir / "stress50.mpq").string();
    if (pool) pool->resetCounters();
    if (!expect(storage.save(archPath), T, "save failed")) return false;
    size_t saveTasks = pool ? pool->totalSubmitted() : size_t{0};

    // Reopen and verify every file.
    auto reopened = mpq::Storage::open(archPath, nullptr, pool);
    if (!expect(reopened.has_value(), T, "reopen failed")) return false;

    for (int i = 0; i < 50; ++i) {
        auto readBack = reopened->readFile(filenames[i]);
        if (!expect(readBack.has_value(), T, "readFile failed for %s", filenames[i].c_str())) return false;
        if (!expect(vectorsEqual(*readBack, origData[i]), T,
                    "data mismatch for %s", filenames[i].c_str())) return false;
    }

    return pass(T, "stress: 50 files round-tripped (%zu save tasks)", saveTasks);
}

/// Test 16: Incompressible data (compression fallback) with pool.
bool test16_incompressible_data(TestWorkerPool* pool) {
    constexpr int T = 16;

    // High-entropy data that won't compress well.
    auto rawData = makePatternData(30000, 0xFF);

    mpq::EncodeOptions opts;
    opts.compression = 0x02;
    opts.encrypt = false;
    opts.singleUnit = false;
    opts.sectorSize = 4096;
    opts.filename = "random.bin";

    auto serialResult = mpq::encodeFileData(std::span<const u8>(rawData), opts, nullptr);

    if (pool) pool->resetCounters();
    auto parallelResult = mpq::encodeFileData(std::span<const u8>(rawData), opts, pool);

    if (!expect(vectorsEqual(serialResult.data, parallelResult.data), T,
                "incompressible data: encoded output differs")) return false;

    // Verify round-trip.
    mpq::BlockEntry block;
    block.fileOffset = 0;
    block.compressedSize = parallelResult.compressedSize;
    block.uncompressedSize = static_cast<u32>(rawData.size());
    block.flags = parallelResult.flags;

    auto extracted = mpq::extractFileData(
        std::span<const u8>(parallelResult.data), 0, block, opts.sectorSize, 0, nullptr, pool);
    if (!expect(vectorsEqual(extracted, rawData), T,
                "incompressible data: extract mismatch")) return false;

    return pass(T, "incompressible data: parallel encode + extract correct");
}

// ============================================================================
// Test Runner
// ============================================================================

static void runSuite(const fs::path& tempDir, TestWorkerPool* pool) {
    fs::create_directories(tempDir);

    // Plumbing tests.
    if (!test1_plumbing_open_create(tempDir, pool)) ++g_failed;

    // Encode tests (encodeFileData).
    if (!test2_encode_serial_equivalence(pool)) ++g_failed;
    if (!test3_encode_encrypted_equivalence(pool)) ++g_failed;
    if (!test4_encode_single_unit_serial(pool)) ++g_failed;
    if (!test5_encode_below_threshold(pool)) ++g_failed;

    // Extract tests (extractFileData).
    if (!test6_extract_parallel_roundtrip(pool)) ++g_failed;
    if (!test7_extract_encrypted_roundtrip(pool)) ++g_failed;
    if (!test8_extract_below_threshold(pool)) ++g_failed;

    // Writer tests (writeArchive).
    if (!test9_write_archive_parallel(tempDir, pool)) ++g_failed;
    if (!test10_write_archive_single_file_fallback(tempDir, pool)) ++g_failed;

    // End-to-end tests.
    if (!test11_end_to_end_with_pool(tempDir, pool)) ++g_failed;
    if (!test14_overwrite_save_with_pool(tempDir, pool)) ++g_failed;

    // Algorithm / edge case tests.
    if (!test12_pkware_compression_parallel(pool)) ++g_failed;
    if (!test13_tasks_run_on_worker_threads(pool)) ++g_failed;
    if (!test15_many_small_files(tempDir, pool)) ++g_failed;
    if (!test16_incompressible_data(pool)) ++g_failed;
}

int main() {
    fs::path baseDir = fs::temp_directory_path() / "whiteout_mpq_parallel_test";
    cleanup(baseDir);

    std::printf("=== Serial run (pool = nullptr) ===\n\n");
    g_runLabel = "serial";
    runSuite(baseDir / "serial", nullptr);

    std::printf("\n=== Parallel run (pool = 4 threads) ===\n\n");
    TestWorkerPool pool(4);
    g_runLabel = "parallel";
    runSuite(baseDir / "parallel", &pool);

    cleanup(baseDir);
    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

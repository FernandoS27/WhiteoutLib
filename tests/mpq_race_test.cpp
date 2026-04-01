// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MPQ pipeline race condition test suite.
//
// Tests the flattened encodeBatch() / extractBatch() pipeline for data races,
// deadlocks, and correctness under concurrent execution. Every test runs
// against three pool configurations:
//
//   1. No pool (nullptr)            — serial baseline
//   2. Pool WITHOUT semaphores      — fallback path (parallel files XOR sectors)
//   3. Pool WITH semaphores         — flattened timeline-semaphore pipeline
//
// Additionally, stress tests hammer the pipeline at varying thread counts
// (1, 2, 4, 8) to surface races that only appear under specific scheduling.
//
// Key race scenarios tested:
//   - Concurrent sector compression writing to adjacent SectorResult slots
//   - Assembly task reading sector data written by compression tasks
//   - std::atomic<bool> failed sentinel visibility across threads
//   - signalOnComplete firing before/after all tasks complete
//   - Mixed single-unit / multi-sector / empty files in a single batch
//   - Per-file error isolation in extractBatch
//   - encodeBatch + extractBatch round-trip at scale
//   - 1-thread pool (maximum contention for semaphore flattened path)
//   - threadCount == 0 graceful fallback

#include <catch2/catch_all.hpp>

#include <whiteout/interfaces.h>
#include <whiteout/storages/mpq/storage.h>
#include <whiteout/utils/simple_thread_pool.h>
#include <whiteout/utils/timeline_semaphore.h>

// Internal headers — test exercises internals directly.
#include "whiteout/storages/mpq/file_data.h"
#include "whiteout/storages/mpq/writer.h"
#include "whiteout/storages/mpq/tables/header.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace mpq = whiteout::storages::mpq;

using whiteout::u8;
using whiteout::u32;
using whiteout::u64;

// ============================================================================
// Pool Implementations
// ============================================================================

/// A pool that ignores semaphore fields — forces the fallback path in
/// encodeBatch/extractBatch. Uses real threads for actual concurrency.
class NoSemaphorePool : public whiteout::interfaces::WorkerPool {
public:
    explicit NoSemaphorePool(size_t numThreads) : m_pool(numThreads) {}

    void submit(const whiteout::interfaces::WorkerTask& task) override {
        // Strip semaphore fields — submit the bare function.
        whiteout::interfaces::WorkerTask stripped;
        stripped.fn = task.fn;
        m_pool.submit(stripped);
    }

    void waitIdle() override { m_pool.waitIdle(); }
    size_t threadCount() const noexcept override { return m_pool.threadCount(); }

    // Returns nullptr — no semaphore support.
    std::unique_ptr<whiteout::interfaces::TimelineSemaphore> createTimelineSemaphore() override {
        return nullptr;
    }

private:
    whiteout::utils::SimpleThreadPool m_pool;
};

/// A pool with zero threads — tasks submitted but never executed.
/// Used to test the threadCount() == 0 guard.
class ZeroThreadPool : public whiteout::interfaces::WorkerPool {
public:
    void submit(const whiteout::interfaces::WorkerTask&) override {
        // Never executes — just counts.
        m_submitted.fetch_add(1, std::memory_order_relaxed);
    }
    void waitIdle() override {}
    size_t threadCount() const noexcept override { return 0; }
    size_t submitted() const { return m_submitted.load(std::memory_order_relaxed); }

private:
    std::atomic<size_t> m_submitted{0};
};

// ============================================================================
// Test Harness
// ============================================================================

namespace {
bool fail(int testNumber, const char* fmt, ...) {
    std::printf("TEST %d FAIL: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    
    return false;
}

bool pass(int testNumber, const char* fmt, ...) {
    std::printf("TEST %d PASS: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    
    return true;
}

bool expect(bool condition, int testNumber, const char* fmt, ...) {
    if (condition) return true;
    std::printf("TEST %d FAIL: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return false;
}

std::vector<u8> makeCompressibleData(size_t size) {
    static constexpr char kPhrase[] = "whiteout-mpq-race-test-data-block-pattern-";
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>(kPhrase[i % (sizeof(kPhrase) - 1)]);
    }
    return data;
}

std::vector<u8> makePatternData(size_t size, u8 seed) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>((i * 13 + (i / 17) * 7 + seed) & 0xFF);
    }
    return data;
}

/// High-entropy pseudo-random data (very hard to compress).
std::vector<u8> makeRandomData(size_t size, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>(dist(rng));
    return data;
}

bool vectorsEqual(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

} // anonymous namespace

// ============================================================================
// Test 1: encodeBatch — serial baseline (nullptr pool)
//
// Verifies encodeBatch with no pool produces correct results and that
// every file round-trips through extractFileData.
// ============================================================================
bool test1_encode_batch_serial() {
    constexpr int T = 1;

    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;
    std::vector<std::vector<u8>> rawDatas;

    for (int i = 0; i < 20; ++i) {
        rawDatas.push_back(makeCompressibleData(5000 + i * 2000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "batch_serial_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    auto result = mpq::encodeBatch(items, nullptr);
    if (!expect(result.files.size() == 20, T, "expected 20 files, got %zu", result.files.size()))
        return false;

    // Verify each file round-trips.
    for (int i = 0; i < 20; ++i) {
        auto& ef = result.files[i];
        if (!expect(!ef.data.empty(), T, "file %d encoded empty", i)) return false;

        mpq::BlockEntry block{};
        block.fileOffset = 0;
        block.compressedSize = ef.compressedSize;
        block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        block.flags = ef.flags;

        auto extracted = mpq::extractFileData(std::span<const u8>(ef.data), 0, block,
                                              items[i].second.sectorSize, 0);
        if (!expect(vectorsEqual(extracted, rawDatas[i]), T,
                    "file %d round-trip mismatch (expected %zu, got %zu)", i,
                    rawDatas[i].size(), extracted.size()))
            return false;
    }

    return pass(T, "encodeBatch serial: 20 files round-trip correctly");
}

// ============================================================================
// Test 2: encodeBatch — fallback path (pool without semaphores)
//
// Exercises the parallel-files XOR parallel-sectors fallback with multiple
// thread counts to surface races in shared result vector writes.
// ============================================================================
bool test2_encode_batch_no_semaphore(size_t numThreads) {
    constexpr int T = 2;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    // Mix of file sizes: some multi-sector, some single-sector, some single-unit.
    for (int i = 0; i < 30; ++i) {
        size_t sz = (i % 3 == 0) ? 60000 : (i % 3 == 1) ? 3000 : 500;
        rawDatas.push_back(makeCompressibleData(sz));

        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.singleUnit = (i % 3 == 2);
        opts.sectorSize = 4096;
        opts.filename = "nosem_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    // Serial baseline.
    auto serialResult = mpq::encodeBatch(items, nullptr);

    // Parallel (no semaphores).
    NoSemaphorePool pool(numThreads);
    auto parallelResult = mpq::encodeBatch(items, &pool);

    if (!expect(serialResult.files.size() == parallelResult.files.size(), T,
                "file count mismatch: serial=%zu parallel=%zu",
                serialResult.files.size(), parallelResult.files.size()))
        return false;

    for (size_t i = 0; i < serialResult.files.size(); ++i) {
        if (!expect(serialResult.files[i].compressedSize == parallelResult.files[i].compressedSize,
                    T, "file %zu compressed size mismatch: serial=%u parallel=%u", i,
                    serialResult.files[i].compressedSize, parallelResult.files[i].compressedSize))
            return false;
        if (!expect(vectorsEqual(serialResult.files[i].data, parallelResult.files[i].data), T,
                    "file %zu encoded data differs (serial vs no-semaphore pool)", i))
            return false;
    }

    // Verify round-trip for every file.
    for (size_t i = 0; i < items.size(); ++i) {
        auto& ef = parallelResult.files[i];
        if (ef.data.empty()) continue; // empty input
        mpq::BlockEntry block{};
        block.fileOffset = 0;
        block.compressedSize = ef.compressedSize;
        block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        block.flags = ef.flags;

        auto extracted = mpq::extractFileData(std::span<const u8>(ef.data), 0, block,
                                              items[i].second.sectorSize, 0);
        if (!expect(vectorsEqual(extracted, rawDatas[i]), T,
                    "file %zu round-trip mismatch (no-semaphore pool)", i))
            return false;
    }

    return pass(T, "encodeBatch no-semaphore (%zu threads): 30 mixed files correct", numThreads);
}

// ============================================================================
// Test 3: encodeBatch — flattened pipeline (pool WITH semaphores)
//
// The main race-condition target: sector compression tasks write to
// SectorResult slots concurrently, then the assembly task reads them.
// ============================================================================
bool test3_encode_batch_with_semaphores(size_t numThreads) {
    constexpr int T = 3;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    // Create files with many sectors to maximize concurrent sector tasks.
    for (int i = 0; i < 20; ++i) {
        rawDatas.push_back(makeCompressibleData(40000 + i * 5000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "sem_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    // Serial baseline.
    auto serialResult = mpq::encodeBatch(items, nullptr);

    // Parallel with semaphores (SimpleThreadPool supports them).
    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto parallelResult = mpq::encodeBatch(items, &pool);

    for (size_t i = 0; i < serialResult.files.size(); ++i) {
        if (!expect(serialResult.files[i].compressedSize == parallelResult.files[i].compressedSize,
                    T, "file %zu compressed size mismatch: serial=%u parallel=%u", i,
                    serialResult.files[i].compressedSize, parallelResult.files[i].compressedSize))
            return false;
        if (!expect(vectorsEqual(serialResult.files[i].data, parallelResult.files[i].data), T,
                    "file %zu encoded data differs (serial vs semaphore pool)", i))
            return false;
    }

    return pass(T, "encodeBatch semaphore (%zu threads): 20 multi-sector files correct",
                numThreads);
}

// ============================================================================
// Test 4: encodeBatch — mixed file types in a single batch
//
// Tests: empty files, single-unit, 1-sector, multi-sector, encrypted,
// incompressible — all in the same batch. Exercises every fast-path and
// the main pipeline simultaneously.
// ============================================================================
bool test4_encode_batch_mixed(whiteout::interfaces::WorkerPool* pool) {
    constexpr int T = 4;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    auto addItem = [&](std::vector<u8> data, mpq::EncodeOptions opts) {
        rawDatas.push_back(std::move(data));
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    };

    // 1) Empty file.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "empty.dat";
        addItem({}, opts);
    }

    // 2) Single-unit small file.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.singleUnit = true;
        opts.sectorSize = 4096;
        opts.filename = "single_unit.dat";
        addItem(makeCompressibleData(800), opts);
    }

    // 3) 1-sector file (below multi-sector threshold).
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "one_sector.dat";
        addItem(makeCompressibleData(3000), opts);
    }

    // 4) Exactly 1 sector boundary.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "exact_sector.dat";
        addItem(makeCompressibleData(4096), opts);
    }

    // 5) 2 sectors (just over 1 sector).
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "two_sectors.dat";
        addItem(makeCompressibleData(4097), opts);
    }

    // 6) Large multi-sector file.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "large.dat";
        addItem(makeCompressibleData(100000), opts);
    }

    // 7) Encrypted multi-sector file.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.encrypt = true;
        opts.sectorSize = 4096;
        opts.filename = "encrypted\\secret.dat";
        addItem(makeCompressibleData(50000), opts);
    }

    // 8) Incompressible data (random bytes).
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "random.dat";
        addItem(makeRandomData(30000, 42), opts);
    }

    // 9) No compression.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::None;
        opts.sectorSize = 4096;
        opts.filename = "uncompressed.dat";
        addItem(makeCompressibleData(20000), opts);
    }

    // 10-15) More multi-sector files to create pool contention.
    for (int i = 0; i < 6; ++i) {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "extra_" + std::to_string(i) + ".dat";
        addItem(makeCompressibleData(25000 + i * 8000), opts);
    }

    auto result = mpq::encodeBatch(items, pool);
    if (!expect(result.files.size() == items.size(), T,
                "file count mismatch: expected %zu, got %zu", items.size(), result.files.size()))
        return false;

    // Verify every non-empty file round-trips.
    for (size_t i = 0; i < items.size(); ++i) {
        auto& ef = result.files[i];
        if (rawDatas[i].empty()) {
            // Empty input → empty output is acceptable.
            continue;
        }
        if (!expect(!ef.data.empty(), T, "file %zu encoded empty (input was %zu bytes)", i,
                    rawDatas[i].size()))
            return false;

        mpq::BlockEntry block{};
        block.fileOffset = 0;
        block.compressedSize = ef.compressedSize;
        block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        block.flags = ef.flags;

        u32 fileKey = 0;
        if (items[i].second.encrypt && !items[i].second.filename.empty()) {
            fileKey = mpq::deriveFileKey(items[i].second.filename, block);
        }

        std::string err;
        auto extracted = mpq::extractFileData(std::span<const u8>(ef.data), 0, block,
                                              items[i].second.sectorSize, fileKey, &err);
        if (!expect(vectorsEqual(extracted, rawDatas[i]), T,
                    "file %zu ('%s') round-trip mismatch: %s", i,
                    items[i].second.filename.c_str(), err.c_str()))
            return false;
    }

    return pass(T, "encodeBatch mixed: %zu files correct", items.size());
}

// ============================================================================
// Test 5: extractBatch — flattened pipeline round-trip
//
// Encodes files, then extracts them all via extractBatch. Tests concurrent
// sector decompression across multiple files with per-file semaphores.
// ============================================================================
bool test5_extract_batch_with_semaphores(size_t numThreads) {
    constexpr int T = 5;

    // Encode many files serially first.
    std::vector<std::vector<u8>> rawDatas;
    std::vector<mpq::EncodedFile> encodedFiles;
    constexpr u32 kSectorSize = 4096;

    for (int i = 0; i < 25; ++i) {
        rawDatas.push_back(makeCompressibleData(10000 + i * 4000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = kSectorSize;
        opts.filename = "extract_" + std::to_string(i) + ".dat";
        encodedFiles.push_back(mpq::encodeFileData(std::span<const u8>(rawDatas.back()), opts));
    }

    // Build a mock archive: concatenate all encoded files.
    std::vector<u8> archiveData;
    std::vector<mpq::ExtractFileInfo> fileInfos;

    for (size_t i = 0; i < encodedFiles.size(); ++i) {
        u32 offset = static_cast<u32>(archiveData.size());
        archiveData.insert(archiveData.end(), encodedFiles[i].data.begin(),
                           encodedFiles[i].data.end());

        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = offset;
        fi.block.compressedSize = encodedFiles[i].compressedSize;
        fi.block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        fi.block.flags = encodedFiles[i].flags;
        fi.fileKey = 0;
        fileInfos.push_back(fi);
    }

    // Extract all at once with semaphore-capable pool.
    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto results = mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize,
                                     &pool);

    if (!expect(results.size() == rawDatas.size(), T,
                "result count mismatch: expected %zu, got %zu", rawDatas.size(), results.size()))
        return false;

    for (size_t i = 0; i < rawDatas.size(); ++i) {
        if (!expect(results[i].has_value(), T, "file %zu extraction returned nullopt", i))
            return false;
        if (!expect(vectorsEqual(*results[i], rawDatas[i]), T,
                    "file %zu extract mismatch (expected %zu, got %zu)", i, rawDatas[i].size(),
                    results[i]->size()))
            return false;
    }

    return pass(T, "extractBatch semaphore (%zu threads): 25 files correct", numThreads);
}

// ============================================================================
// Test 6: extractBatch — fallback path (no semaphores)
// ============================================================================
bool test6_extract_batch_no_semaphore(size_t numThreads) {
    constexpr int T = 6;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<mpq::EncodedFile> encodedFiles;
    constexpr u32 kSectorSize = 4096;

    for (int i = 0; i < 15; ++i) {
        rawDatas.push_back(makeCompressibleData(20000 + i * 3000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = kSectorSize;
        opts.filename = "nosem_extract_" + std::to_string(i) + ".dat";
        encodedFiles.push_back(mpq::encodeFileData(std::span<const u8>(rawDatas.back()), opts));
    }

    std::vector<u8> archiveData;
    std::vector<mpq::ExtractFileInfo> fileInfos;

    for (size_t i = 0; i < encodedFiles.size(); ++i) {
        u32 offset = static_cast<u32>(archiveData.size());
        archiveData.insert(archiveData.end(), encodedFiles[i].data.begin(),
                           encodedFiles[i].data.end());

        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = offset;
        fi.block.compressedSize = encodedFiles[i].compressedSize;
        fi.block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        fi.block.flags = encodedFiles[i].flags;
        fi.fileKey = 0;
        fileInfos.push_back(fi);
    }

    NoSemaphorePool pool(numThreads);
    auto results = mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize,
                                     &pool);

    for (size_t i = 0; i < rawDatas.size(); ++i) {
        if (!expect(results[i].has_value(), T, "file %zu returned nullopt", i)) return false;
        if (!expect(vectorsEqual(*results[i], rawDatas[i]), T, "file %zu mismatch", i))
            return false;
    }

    return pass(T, "extractBatch no-semaphore (%zu threads): 15 files correct", numThreads);
}

// ============================================================================
// Test 7: extractBatch — mixed (empty, single-unit, invalid, encrypted)
//
// Per-file error isolation: invalid files should return nullopt without
// corrupting other files' results.
// ============================================================================
bool test7_extract_batch_mixed(whiteout::interfaces::WorkerPool* pool) {
    constexpr int T = 7;
    constexpr u32 kSectorSize = 4096;

    std::vector<u8> archiveData;
    std::vector<mpq::ExtractFileInfo> fileInfos;
    // Track expected results: -1 = should be nullopt, otherwise index into rawDatas.
    std::vector<int> expectedIdx;
    std::vector<std::vector<u8>> rawDatas;

    // Helper to add a valid encoded file.
    auto addValid = [&](const std::vector<u8>& rawData, const mpq::EncodeOptions& opts) {
        auto encoded = mpq::encodeFileData(std::span<const u8>(rawData), opts);
        u32 offset = static_cast<u32>(archiveData.size());
        archiveData.insert(archiveData.end(), encoded.data.begin(), encoded.data.end());

        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = offset;
        fi.block.compressedSize = encoded.compressedSize;
        fi.block.uncompressedSize = static_cast<u32>(rawData.size());
        fi.block.flags = encoded.flags;

        if (opts.encrypt && !opts.filename.empty())
            fi.fileKey = mpq::deriveFileKey(opts.filename, fi.block);

        fileInfos.push_back(fi);
        rawDatas.push_back(rawData);
        expectedIdx.push_back(static_cast<int>(rawDatas.size()) - 1);
    };

    // 1) Normal multi-sector file.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = kSectorSize;
        opts.filename = "normal.dat";
        addValid(makeCompressibleData(50000), opts);
    }

    // 2) Zero-byte file (block with uncompressedSize == 0).
    {
        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = 0;
        fi.block.compressedSize = 0;
        fi.block.uncompressedSize = 0;
        fi.block.flags = mpq::FileFlag::kExists;
        fi.fileKey = 0;
        fileInfos.push_back(fi);
        expectedIdx.push_back(-2); // -2 = empty vec expected
    }

    // 3) Single-unit file.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.singleUnit = true;
        opts.sectorSize = kSectorSize;
        opts.filename = "single.dat";
        addValid(makeCompressibleData(2000), opts);
    }

    // 4) Another multi-sector file (interleaved with bad entries).
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = kSectorSize;
        opts.filename = "multi2.dat";
        addValid(makeCompressibleData(80000), opts);
    }

    // 5) Non-existent block (exists flag not set).
    {
        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = 0;
        fi.block.compressedSize = 100;
        fi.block.uncompressedSize = 200;
        fi.block.flags = mpq::FileFlag::None;
        fi.fileKey = 0;
        fileInfos.push_back(fi);
        expectedIdx.push_back(-1); // nullopt expected
    }

    // 6) Encrypted multi-sector file.
    {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.encrypt = true;
        opts.sectorSize = kSectorSize;
        opts.filename = "enc\\data.bin";
        addValid(makeCompressibleData(40000), opts);
    }

    // 7-10) More valid files for contention.
    for (int i = 0; i < 4; ++i) {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = kSectorSize;
        opts.filename = "extra_" + std::to_string(i) + ".dat";
        addValid(makeCompressibleData(30000 + i * 10000), opts);
    }

    auto results =
        mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize, pool);

    if (!expect(results.size() == fileInfos.size(), T, "result count mismatch")) return false;

    for (size_t i = 0; i < expectedIdx.size(); ++i) {
        int idx = expectedIdx[i];
        if (idx == -1) {
            // Should be nullopt (non-existent block).
            if (!expect(!results[i].has_value(), T, "file %zu should be nullopt but has value", i))
                return false;
        } else if (idx == -2) {
            // Should be empty vector (zero-byte file).
            if (!expect(results[i].has_value(), T, "file %zu (0-byte) should have value", i))
                return false;
            if (!expect(results[i]->empty(), T, "file %zu (0-byte) should be empty", i))
                return false;
        } else {
            if (!expect(results[i].has_value(), T, "file %zu returned nullopt unexpectedly", i))
                return false;
            if (!expect(vectorsEqual(*results[i], rawDatas[idx]), T, "file %zu data mismatch", i))
                return false;
        }
    }

    return pass(T, "extractBatch mixed: %zu files with error isolation correct",
                fileInfos.size());
}

// ============================================================================
// Test 8: threadCount == 0 graceful fallback
//
// Neither encodeBatch nor extractBatch should submit any tasks or deadlock.
// ============================================================================
bool test8_zero_thread_pool() {
    constexpr int T = 8;

    ZeroThreadPool pool;

    // encodeBatch should fall through to serial.
    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    for (int i = 0; i < 5; ++i) {
        rawDatas.push_back(makeCompressibleData(20000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "zero_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    auto encResult = mpq::encodeBatch(items, &pool);
    if (!expect(encResult.files.size() == 5, T, "encodeBatch produced wrong count")) return false;
    if (!expect(pool.submitted() == 0, T, "encodeBatch submitted %zu tasks to 0-thread pool",
                pool.submitted()))
        return false;

    // extractBatch should also fall through to serial.
    constexpr u32 kSectorSize = 4096;
    std::vector<u8> archiveData;
    std::vector<mpq::ExtractFileInfo> fileInfos;

    for (size_t i = 0; i < encResult.files.size(); ++i) {
        auto& ef = encResult.files[i];
        u32 offset = static_cast<u32>(archiveData.size());
        archiveData.insert(archiveData.end(), ef.data.begin(), ef.data.end());

        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = offset;
        fi.block.compressedSize = ef.compressedSize;
        fi.block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        fi.block.flags = ef.flags;
        fi.fileKey = 0;
        fileInfos.push_back(fi);
    }

    auto extResults =
        mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize, &pool);

    for (size_t i = 0; i < rawDatas.size(); ++i) {
        if (!expect(extResults[i].has_value(), T, "file %zu nullopt with 0-thread pool", i))
            return false;
        if (!expect(vectorsEqual(*extResults[i], rawDatas[i]), T, "file %zu mismatch", i))
            return false;
    }

    return pass(T, "zero-thread pool: no tasks submitted, serial fallback correct");
}

// ============================================================================
// Test 9: 1-thread pool stress (maximum contention)
//
// With 1 thread, all tasks are serialized through a single worker. This
// tests that semaphore waits/signals don't deadlock when there's zero
// parallel capacity for interleaving.
// ============================================================================
bool test9_single_thread_stress() {
    constexpr int T = 9;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    for (int i = 0; i < 15; ++i) {
        rawDatas.push_back(makeCompressibleData(30000 + i * 5000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "single_thread_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    // 1-thread pool with semaphores.
    whiteout::utils::SimpleThreadPool pool(1);
    auto result = mpq::encodeBatch(items, &pool);

    // Verify against serial baseline.
    auto serialResult = mpq::encodeBatch(items, nullptr);

    for (size_t i = 0; i < items.size(); ++i) {
        if (!expect(vectorsEqual(serialResult.files[i].data, result.files[i].data), T,
                    "file %zu differs between serial and 1-thread pool", i))
            return false;
    }

    return pass(T, "1-thread pool stress: 15 files, no deadlock, correct output");
}

// ============================================================================
// Test 10: Repeated batch encode/extract iteration (race accumulation)
//
// Runs encodeBatch + extractBatch in a tight loop to catch intermittent
// races that only manifest under specific thread scheduling.
// ============================================================================
bool test10_iteration_stress(size_t numThreads) {
    constexpr int T = 10;
    constexpr int kIterations = 30;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> encItems;
    constexpr u32 kSectorSize = 4096;

    for (int i = 0; i < 10; ++i) {
        rawDatas.push_back(makeCompressibleData(15000 + i * 3000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = kSectorSize;
        opts.filename = "iter_" + std::to_string(i) + ".dat";
        encItems.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    whiteout::utils::SimpleThreadPool pool(numThreads);

    for (int iter = 0; iter < kIterations; ++iter) {
        // Encode.
        auto encResult = mpq::encodeBatch(encItems, &pool);

        // Build mock archive for extraction.
        std::vector<u8> archiveData;
        std::vector<mpq::ExtractFileInfo> fileInfos;

        for (size_t i = 0; i < encResult.files.size(); ++i) {
            u32 offset = static_cast<u32>(archiveData.size());
            archiveData.insert(archiveData.end(), encResult.files[i].data.begin(),
                               encResult.files[i].data.end());

            mpq::ExtractFileInfo fi;
            fi.block.fileOffset = offset;
            fi.block.compressedSize = encResult.files[i].compressedSize;
            fi.block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
            fi.block.flags = encResult.files[i].flags;
            fi.fileKey = 0;
            fileInfos.push_back(fi);
        }

        // Extract.
        auto extResults =
            mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize, &pool);

        // Verify.
        for (size_t i = 0; i < rawDatas.size(); ++i) {
            if (!expect(extResults[i].has_value(), T, "iter %d file %zu nullopt", iter, i))
                return false;
            if (!expect(vectorsEqual(*extResults[i], rawDatas[i]), T,
                        "iter %d file %zu mismatch", iter, i))
                return false;
        }
    }

    return pass(T, "iteration stress (%zu threads): %d encode+extract cycles correct", numThreads,
                kIterations);
}

// ============================================================================
// Test 11: encodeBatch + writeArchive integration
//
// Verifies that writeArchive (which now calls encodeBatch internally)
// produces valid archives under all pool modes.
// ============================================================================
bool test11_write_archive_integration(whiteout::interfaces::WorkerPool* pool) {
    constexpr int T = 11;

    mpq::MpqHeader header;
    header.magic = 0x1A51504D;
    header.headerSize = 32;
    header.formatVersion = 0;
    header.sectorSizeShift = 3; // 4096
    header.hashTableEntries = 64;

    std::vector<mpq::WriteEntry> entries;
    std::vector<std::vector<u8>> origData;

    // Mix of file types.
    for (int i = 0; i < 15; ++i) {
        auto data = makeCompressibleData(8000 + i * 4000);
        mpq::WriteEntry we;
        we.filename = "archive_file" + std::to_string(i) + ".dat";
        we.rawData = data;
        we.compression = mpq::CompressionFlag::kZlib;
        we.encrypt = (i % 5 == 3);
        we.singleUnit = (i % 7 == 0);
        entries.push_back(std::move(we));
        origData.push_back(std::move(data));
    }

    auto archive = mpq::writeArchive(header, entries, 64, pool);
    if (!expect(!archive.empty(), T, "writeArchive produced empty output")) return false;

    // Write to tmp file, reopen via Storage, verify every file.
    auto tmpDir = fs::temp_directory_path() / "whiteout_race_test11";
    fs::create_directories(tmpDir);
    auto archPath = (tmpDir / "integration.mpq").string();
    {
        std::ofstream out(archPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(archive.data()),
                  static_cast<std::streamsize>(archive.size()));
    }

    auto storage = mpq::Storage::open(archPath);
    if (!expect(storage.has_value(), T, "failed to open generated archive")) return false;

    for (int i = 0; i < 15; ++i) {
        std::string name = "archive_file" + std::to_string(i) + ".dat";
        auto readBack = storage->readFile(name);
        if (!expect(readBack.has_value(), T, "readFile failed for %s", name.c_str())) return false;
        if (!expect(vectorsEqual(*readBack, origData[i]), T, "data mismatch for %s", name.c_str()))
            return false;
    }
    std::error_code ec;
    fs::remove_all(tmpDir, ec);

    return pass(T, "writeArchive integration: 15 files round-trip correct");
}

// ============================================================================
// Test 12: Massive batch stress (many files × many sectors)
//
// Maximum contention: many files each with many sectors, all processed
// concurrently via the flattened pipeline.
// ============================================================================
bool test12_massive_batch(size_t numThreads) {
    constexpr int T = 12;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    // 50 files × ~25 sectors each = ~1250 compression tasks.
    for (int i = 0; i < 50; ++i) {
        rawDatas.push_back(makeCompressibleData(80000 + (i % 10) * 10000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "massive_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto result = mpq::encodeBatch(items, &pool);

    if (!expect(result.files.size() == 50, T, "expected 50 files")) return false;

    // Spot-check every file round-trips.
    for (size_t i = 0; i < items.size(); ++i) {
        auto& ef = result.files[i];
        mpq::BlockEntry block{};
        block.fileOffset = 0;
        block.compressedSize = ef.compressedSize;
        block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        block.flags = ef.flags;

        auto extracted = mpq::extractFileData(std::span<const u8>(ef.data), 0, block,
                                              items[i].second.sectorSize, 0);
        if (!expect(vectorsEqual(extracted, rawDatas[i]), T,
                    "massive batch file %zu round-trip mismatch", i))
            return false;
    }

    return pass(T, "massive batch (%zu threads): 50 files × ~25 sectors correct", numThreads);
}

// ============================================================================
// Test 13: extractBatch — encrypted files batch
//
// Tests that per-file encryption keys are correctly applied in parallel
// decompression without cross-contamination.
// ============================================================================
bool test13_extract_batch_encrypted(size_t numThreads) {
    constexpr int T = 13;
    constexpr u32 kSectorSize = 4096;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<mpq::EncodedFile> encodedFiles;
    std::vector<mpq::EncodeOptions> optsList;

    for (int i = 0; i < 12; ++i) {
        rawDatas.push_back(makeCompressibleData(20000 + i * 5000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.encrypt = true;
        opts.sectorSize = kSectorSize;
        opts.filename = "enc\\batch_" + std::to_string(i) + ".dat";
        optsList.push_back(opts);
        encodedFiles.push_back(mpq::encodeFileData(std::span<const u8>(rawDatas.back()), opts));
    }

    // Build mock archive.
    std::vector<u8> archiveData;
    std::vector<mpq::ExtractFileInfo> fileInfos;

    for (size_t i = 0; i < encodedFiles.size(); ++i) {
        u32 offset = static_cast<u32>(archiveData.size());
        archiveData.insert(archiveData.end(), encodedFiles[i].data.begin(),
                           encodedFiles[i].data.end());

        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = offset;
        fi.block.compressedSize = encodedFiles[i].compressedSize;
        fi.block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        fi.block.flags = encodedFiles[i].flags;
        fi.fileKey = mpq::deriveFileKey(optsList[i].filename, fi.block);
        fileInfos.push_back(fi);
    }

    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto results = mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize,
                                     &pool);

    for (size_t i = 0; i < rawDatas.size(); ++i) {
        if (!expect(results[i].has_value(), T, "encrypted file %zu returned nullopt", i))
            return false;
        if (!expect(vectorsEqual(*results[i], rawDatas[i]), T, "encrypted file %zu mismatch", i))
            return false;
    }

    return pass(T, "extractBatch encrypted (%zu threads): 12 files correct", numThreads);
}

// ============================================================================
// Test 14: encodeBatch serial == parallel byte-equality
//
// Verifies that the flattened semaphore path produces bit-identical output
// to the serial path. This catches races where sector results are written
// to wrong indices or assembly reads stale data.
// ============================================================================
bool test14_serial_parallel_byte_equality(size_t numThreads) {
    constexpr int T = 14;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    for (int i = 0; i < 20; ++i) {
        rawDatas.push_back(makeCompressibleData(30000 + i * 2000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.encrypt = (i % 3 == 0);
        opts.singleUnit = (i % 7 == 0);
        opts.sectorSize = 4096;
        opts.filename = "eq_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    auto serialResult = mpq::encodeBatch(items, nullptr);

    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto parallelResult = mpq::encodeBatch(items, &pool);

    for (size_t i = 0; i < items.size(); ++i) {
        if (!expect(serialResult.files[i].compressedSize == parallelResult.files[i].compressedSize,
                    T, "file %zu compressed size: serial=%u parallel=%u", i,
                    serialResult.files[i].compressedSize, parallelResult.files[i].compressedSize))
            return false;
        if (!expect(serialResult.files[i].flags == parallelResult.files[i].flags, T,
                    "file %zu flags: serial=0x%X parallel=0x%X", i,
                    static_cast<u32>(serialResult.files[i].flags),
                    static_cast<u32>(parallelResult.files[i].flags)))
            return false;
        if (!expect(vectorsEqual(serialResult.files[i].data, parallelResult.files[i].data), T,
                    "file %zu encoded bytes differ (serial vs %zu-thread semaphore pool)", i,
                    numThreads))
            return false;
    }

    return pass(T, "serial==parallel byte equality (%zu threads): 20 files match", numThreads);
}

// ============================================================================
// Test 15: Concurrent encodeBatch calls from multiple threads
//
// Multiple threads call encodeBatch simultaneously with different data.
// This tests that there are no static/global mutable state races.
// ============================================================================
bool test15_concurrent_batch_calls() {
    constexpr int T = 15;
    constexpr int kConcurrentBatches = 4;
    constexpr int kFilesPerBatch = 8;

    whiteout::utils::SimpleThreadPool pool(4);

    // Each thread gets its own batch items (fully independent data).
    struct BatchWork {
        std::vector<std::vector<u8>> rawDatas;
        std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;
        mpq::BatchEncodeResult result;
        bool ok = false;
    };

    std::vector<BatchWork> batches(kConcurrentBatches);
    for (int b = 0; b < kConcurrentBatches; ++b) {
        for (int f = 0; f < kFilesPerBatch; ++f) {
            batches[b].rawDatas.push_back(
                makeCompressibleData(20000 + b * 1000 + f * 3000));
            mpq::EncodeOptions opts;
            opts.compression = mpq::CompressionFlag::kZlib;
            opts.sectorSize = 4096;
            opts.filename =
                "concurrent_b" + std::to_string(b) + "_f" + std::to_string(f) + ".dat";
            batches[b].items.emplace_back(
                std::span<const u8>(batches[b].rawDatas.back()), opts);
        }
    }

    // Run all batches concurrently using separate OS threads (not the pool).
    std::vector<std::thread> batchThreads;
    for (int b = 0; b < kConcurrentBatches; ++b) {
        batchThreads.emplace_back([b, &batches, &pool]() {
            batches[b].result = mpq::encodeBatch(batches[b].items, &pool);
            // Verify round-trip inline.
            batches[b].ok = true;
            for (int f = 0; f < kFilesPerBatch; ++f) {
                auto& ef = batches[b].result.files[f];
                mpq::BlockEntry block{};
                block.fileOffset = 0;
                block.compressedSize = ef.compressedSize;
                block.uncompressedSize = static_cast<u32>(batches[b].rawDatas[f].size());
                block.flags = ef.flags;

                auto extracted = mpq::extractFileData(
                    std::span<const u8>(ef.data), 0, block, 4096, 0);
                if (!vectorsEqual(extracted, batches[b].rawDatas[f])) {
                    batches[b].ok = false;
                }
            }
        });
    }

    for (auto& t : batchThreads)
        t.join();

    for (int b = 0; b < kConcurrentBatches; ++b) {
        if (!expect(batches[b].ok, T, "concurrent batch %d failed round-trip", b))
            return false;
    }

    return pass(T, "concurrent encodeBatch: %d batches × %d files all correct",
                kConcurrentBatches, kFilesPerBatch);
}

// ============================================================================
// Test 16: encodeBatch + extractBatch with PKware compression
//
// Different codec path — ensures the flattened pipeline isn't coupled
// to zlib internals.
// ============================================================================
bool test16_pkware_batch(whiteout::interfaces::WorkerPool* pool) {
    constexpr int T = 16;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;
    constexpr u32 kSectorSize = 4096;

    for (int i = 0; i < 10; ++i) {
        rawDatas.push_back(makeCompressibleData(20000 + i * 5000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kPKware;
        opts.sectorSize = kSectorSize;
        opts.filename = "pkware_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    auto encResult = mpq::encodeBatch(items, pool);

    // Build archive and extract.
    std::vector<u8> archiveData;
    std::vector<mpq::ExtractFileInfo> fileInfos;

    for (size_t i = 0; i < encResult.files.size(); ++i) {
        u32 offset = static_cast<u32>(archiveData.size());
        archiveData.insert(archiveData.end(), encResult.files[i].data.begin(),
                           encResult.files[i].data.end());

        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = offset;
        fi.block.compressedSize = encResult.files[i].compressedSize;
        fi.block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        fi.block.flags = encResult.files[i].flags;
        fi.fileKey = 0;
        fileInfos.push_back(fi);
    }

    auto extResults =
        mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize, pool);

    for (size_t i = 0; i < rawDatas.size(); ++i) {
        if (!expect(extResults[i].has_value(), T, "PKware file %zu nullopt", i)) return false;
        if (!expect(vectorsEqual(*extResults[i], rawDatas[i]), T, "PKware file %zu mismatch", i))
            return false;
    }

    return pass(T, "PKware batch: 10 files encode+extract correct");
}

// ============================================================================
// Test 17: extractBatch serial == parallel byte equality
//
// Verifies extracted data is identical regardless of pool configuration.
// ============================================================================
bool test17_extract_serial_parallel_equality(size_t numThreads) {
    constexpr int T = 17;
    constexpr u32 kSectorSize = 4096;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<mpq::EncodedFile> encodedFiles;

    for (int i = 0; i < 15; ++i) {
        rawDatas.push_back(makeCompressibleData(25000 + i * 4000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = kSectorSize;
        opts.filename = "xeq_" + std::to_string(i) + ".dat";
        encodedFiles.push_back(mpq::encodeFileData(std::span<const u8>(rawDatas.back()), opts));
    }

    std::vector<u8> archiveData;
    std::vector<mpq::ExtractFileInfo> fileInfos;

    for (size_t i = 0; i < encodedFiles.size(); ++i) {
        u32 offset = static_cast<u32>(archiveData.size());
        archiveData.insert(archiveData.end(), encodedFiles[i].data.begin(),
                           encodedFiles[i].data.end());

        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = offset;
        fi.block.compressedSize = encodedFiles[i].compressedSize;
        fi.block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        fi.block.flags = encodedFiles[i].flags;
        fi.fileKey = 0;
        fileInfos.push_back(fi);
    }

    // Serial extraction.
    auto serialResults = mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos,
                                           kSectorSize, nullptr);

    // Parallel extraction.
    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto parallelResults =
        mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize, &pool);

    for (size_t i = 0; i < rawDatas.size(); ++i) {
        if (!expect(serialResults[i].has_value() && parallelResults[i].has_value(), T,
                    "file %zu: serial=%d parallel=%d", i, serialResults[i].has_value(),
                    parallelResults[i].has_value()))
            return false;
        if (!expect(vectorsEqual(*serialResults[i], *parallelResults[i]), T,
                    "file %zu: serial != parallel extracted data", i))
            return false;
    }

    return pass(T, "extractBatch serial==parallel (%zu threads): 15 files match", numThreads);
}

// ============================================================================
// Test 18: Encode batch with all-empty files
//
// Edge case: batch where every file is empty. Should not deadlock or crash.
// ============================================================================
bool test18_all_empty_batch(whiteout::interfaces::WorkerPool* pool) {
    constexpr int T = 18;

    std::vector<std::vector<u8>> rawDatas(10); // all empty
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    for (int i = 0; i < 10; ++i) {
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 4096;
        opts.filename = "empty_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas[i]), opts);
    }

    auto result = mpq::encodeBatch(items, pool);
    if (!expect(result.files.size() == 10, T, "expected 10 results")) return false;

    return pass(T, "all-empty batch: no deadlock, %zu results", result.files.size());
}

// ============================================================================
// Test 19: encodeBatch with tiny sector size (extreme sector count)
//
// Uses a 512-byte sector size, creating many sectors per file and thus
// many concurrent tasks. Maximizes race window.
// ============================================================================
bool test19_tiny_sector_size(size_t numThreads) {
    constexpr int T = 19;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::pair<std::span<const u8>, mpq::EncodeOptions>> items;

    // 5 files × 50KB / 512 = ~100 sectors each = ~500 tasks.
    for (int i = 0; i < 5; ++i) {
        rawDatas.push_back(makeCompressibleData(50000));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.sectorSize = 512;
        opts.filename = "tiny_sector_" + std::to_string(i) + ".dat";
        items.emplace_back(std::span<const u8>(rawDatas.back()), opts);
    }

    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto result = mpq::encodeBatch(items, &pool);

    // Verify round-trip.
    for (size_t i = 0; i < items.size(); ++i) {
        auto& ef = result.files[i];
        mpq::BlockEntry block{};
        block.fileOffset = 0;
        block.compressedSize = ef.compressedSize;
        block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        block.flags = ef.flags;

        auto extracted = mpq::extractFileData(std::span<const u8>(ef.data), 0, block, 512, 0);
        if (!expect(vectorsEqual(extracted, rawDatas[i]), T,
                    "file %zu round-trip mismatch (512-byte sectors)", i))
            return false;
    }

    return pass(T, "tiny sector size (%zu threads): 5 files × ~100 sectors correct", numThreads);
}

// ============================================================================
// Test 20: extractBatch with all single-unit files
//
// Single-unit files bypass the sector pipeline entirely. Verifies the
// single-task fast path doesn't race with other files' multi-sector tasks.
// ============================================================================
bool test20_extract_all_single_unit(whiteout::interfaces::WorkerPool* pool) {
    constexpr int T = 20;
    constexpr u32 kSectorSize = 4096;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<mpq::EncodedFile> encodedFiles;

    for (int i = 0; i < 20; ++i) {
        rawDatas.push_back(makeCompressibleData(1000 + i * 500));
        mpq::EncodeOptions opts;
        opts.compression = mpq::CompressionFlag::kZlib;
        opts.singleUnit = true;
        opts.sectorSize = kSectorSize;
        opts.filename = "su_" + std::to_string(i) + ".dat";
        encodedFiles.push_back(mpq::encodeFileData(std::span<const u8>(rawDatas.back()), opts));
    }

    std::vector<u8> archiveData;
    std::vector<mpq::ExtractFileInfo> fileInfos;

    for (size_t i = 0; i < encodedFiles.size(); ++i) {
        u32 offset = static_cast<u32>(archiveData.size());
        archiveData.insert(archiveData.end(), encodedFiles[i].data.begin(),
                           encodedFiles[i].data.end());

        mpq::ExtractFileInfo fi;
        fi.block.fileOffset = offset;
        fi.block.compressedSize = encodedFiles[i].compressedSize;
        fi.block.uncompressedSize = static_cast<u32>(rawDatas[i].size());
        fi.block.flags = encodedFiles[i].flags;
        fi.fileKey = 0;
        fileInfos.push_back(fi);
    }

    auto results =
        mpq::extractBatch(std::span<const u8>(archiveData), 0, fileInfos, kSectorSize, pool);

    for (size_t i = 0; i < rawDatas.size(); ++i) {
        if (!expect(results[i].has_value(), T, "single-unit file %zu nullopt", i)) return false;
        if (!expect(vectorsEqual(*results[i], rawDatas[i]), T, "single-unit file %zu mismatch", i))
            return false;
    }

    return pass(T, "extractBatch all single-unit: 20 files correct");
}

// ============================================================================
// Test Runner
// ============================================================================


// Catch2 test cases

TEST_CASE("test1 encode batch serial", "[mpq][race]") {
    REQUIRE(test1_encode_batch_serial());
}

TEST_CASE("test2 encode batch no semaphore", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test2_encode_batch_no_semaphore(n));
}

TEST_CASE("test3 encode batch with semaphores", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test3_encode_batch_with_semaphores(n));
}

TEST_CASE("test4 encode batch mixed", "[mpq][race]") {
    REQUIRE(test4_encode_batch_mixed(nullptr));
}

TEST_CASE("test5 extract batch with semaphores", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test5_extract_batch_with_semaphores(n));
}

TEST_CASE("test6 extract batch no semaphore", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test6_extract_batch_no_semaphore(n));
}

TEST_CASE("test7 extract batch mixed", "[mpq][race]") {
    REQUIRE(test7_extract_batch_mixed(nullptr));
}

TEST_CASE("test8 zero thread pool", "[mpq][race]") {
    REQUIRE(test8_zero_thread_pool());
}

TEST_CASE("test9 single thread stress", "[mpq][race]") {
    REQUIRE(test9_single_thread_stress());
}

TEST_CASE("test10 iteration stress", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test10_iteration_stress(n));
}

TEST_CASE("test11 write archive integration", "[mpq][race]") {
    REQUIRE(test11_write_archive_integration(nullptr));
}

TEST_CASE("test12 massive batch", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test12_massive_batch(n));
}

TEST_CASE("test13 extract batch encrypted", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test13_extract_batch_encrypted(n));
}

TEST_CASE("test14 serial parallel byte equality", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test14_serial_parallel_byte_equality(n));
}

TEST_CASE("test15 concurrent batch calls", "[mpq][race]") {
    REQUIRE(test15_concurrent_batch_calls());
}

TEST_CASE("test16 pkware batch", "[mpq][race]") {
    REQUIRE(test16_pkware_batch(nullptr));
}

TEST_CASE("test17 extract serial parallel equality", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test17_extract_serial_parallel_equality(n));
}

TEST_CASE("test18 all empty batch", "[mpq][race]") {
    REQUIRE(test18_all_empty_batch(nullptr));
}

TEST_CASE("test19 tiny sector size", "[mpq][race]") {
    auto n = GENERATE(1, 2, 4);
    REQUIRE(test19_tiny_sector_size(n));
}

TEST_CASE("test20 extract all single unit", "[mpq][race]") {
    REQUIRE(test20_extract_all_single_unit(nullptr));
}


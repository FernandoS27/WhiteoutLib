// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// CASC batch & DAG pipeline race condition test suite.
//
// Tests blteDecodeBatch() and readBatch() for data races, deadlocks, and
// correctness under concurrent execution. Every test runs against three pool
// configurations:
//
//   1. No pool (nullptr)            — serial baseline
//   2. Pool WITHOUT semaphores      — fallback path (per-file blteDecode)
//   3. Pool WITH semaphores         — flattened timeline-semaphore DAG pipeline
//
// Additionally, stress tests hammer the pipeline at varying thread counts
// (1, 2, 4, 8) to surface races that only appear under specific scheduling.

#include <catch2/catch_all.hpp>

#include <whiteout/interfaces.h>
#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_thread_pool.h>
#include <whiteout/utils/timeline_semaphore.h>

// Internal headers — test exercises internals directly.
#include "whiteout/storages/casc/blte.h"
#include "whiteout/storages/casc/crypto.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace casc = whiteout::storages::casc;

using whiteout::u8;
using whiteout::u32;
using whiteout::u64;
using whiteout::i32;

/// Locale mask for corpus files — enUS + locale-less entries.
static constexpr u32 kCorpusLocale = casc::LocaleMasks::enUS;

// ============================================================================
// Pool Implementations
// ============================================================================

/// A pool that ignores semaphore fields — forces the fallback path.
class NoSemaphorePool : public whiteout::interfaces::WorkerPool {
public:
    explicit NoSemaphorePool(size_t numThreads) : m_pool(numThreads) {}

    void submit(const whiteout::interfaces::WorkerTask& task) override {
        whiteout::interfaces::WorkerTask stripped;
        stripped.fn = task.fn;
        m_pool.submit(stripped);
    }

    void waitIdle() override { m_pool.waitIdle(); }
    size_t threadCount() const noexcept override { return m_pool.threadCount(); }

    std::unique_ptr<whiteout::interfaces::TimelineSemaphore> createTimelineSemaphore() override {
        return nullptr;
    }

private:
    whiteout::utils::SimpleThreadPool m_pool;
};

/// A pool with zero threads — tasks submitted but never executed.
class ZeroThreadPool : public whiteout::interfaces::WorkerPool {
public:
    void submit(const whiteout::interfaces::WorkerTask&) override {
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
int g_skipped = 0;
bool fail(int testNumber, const char* fmt, ...) {
    std::printf("[test] TEST %d FAIL: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    
    return false;
}

bool pass(int testNumber, const char* fmt, ...) {
    std::printf("[test] TEST %d PASS: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    
    return true;
}

bool skip(int testNumber, const char* fmt, ...) {
    std::printf("[test] TEST %d SKIP: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    ++g_skipped;
    return true;
}

bool expect(bool condition, int testNumber, const char* fmt, ...) {
    if (condition) return true;
    std::printf("[test] TEST %d FAIL: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return false;
}

bool vectorsEqual(const std::vector<u8>& a, const std::vector<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// ============================================================================
// Data Generation
// ============================================================================

std::vector<u8> makePatternData(size_t size, u8 seed) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>((i * 13 + (i / 17) * 7 + seed) & 0xFF);
    return data;
}

std::vector<u8> makeCompressibleData(size_t size) {
    static constexpr char kPhrase[] = "whiteout-casc-race-test-data-block-pattern-";
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>(kPhrase[i % (sizeof(kPhrase) - 1)]);
    return data;
}

/// Create a BLTE blob from raw data using blteEncode.
std::vector<u8> makeBlteSingleFrame(const std::vector<u8>& raw) {
    casc::BlteEncodeOptions opts;
    opts.frameSize = static_cast<u32>(raw.size() + 1); // Single frame.
    opts.compress = false;
    return casc::blteEncode(raw, opts, nullptr);
}

/// Create a multi-frame BLTE blob with the given frame size.
std::vector<u8> makeBlteMultiFrame(const std::vector<u8>& raw, u32 frameSize = 1024) {
    casc::BlteEncodeOptions opts;
    opts.frameSize = frameSize;
    opts.compress = true;
    return casc::blteEncode(raw, opts, nullptr);
}

// ============================================================================
// Test 1: blteDecodeBatch — serial baseline (nullptr pool)
// ============================================================================
bool test1_blte_batch_serial() {
    constexpr int T = 1;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    for (int i = 0; i < 10; ++i) {
        rawDatas.push_back(makePatternData(5000 + i * 1000, static_cast<u8>(i)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 1024));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    auto results = casc::blteDecodeBatch(entries, nullptr, nullptr);

    if (!expect(results.size() == 10, T, "expected 10 results, got %zu", results.size()))
        return fail(T, "wrong result count");

    for (int i = 0; i < 10; ++i) {
        if (!expect(results[i].success, T, "file %d decode failed: %s",
                    i, results[i].error.c_str()))
            return fail(T, "decode failure");
        if (!expect(vectorsEqual(results[i].data, rawDatas[i]), T,
                    "file %d data mismatch (expected %zu bytes, got %zu)",
                    i, rawDatas[i].size(), results[i].data.size()))
            return fail(T, "data mismatch");
    }

    return pass(T, "blteDecodeBatch serial: 10 files decoded correctly");
}

// ============================================================================
// Test 2: blteDecodeBatch — no-semaphore pool (fallback path)
// ============================================================================
bool test2_blte_batch_no_semaphore(size_t numThreads) {
    constexpr int T = 2;

    // Generate baseline with serial decode.
    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    for (int i = 0; i < 15; ++i) {
        rawDatas.push_back(makePatternData(3000 + i * 800, static_cast<u8>(i + 10)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 512));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    auto serialResults = casc::blteDecodeBatch(entries, nullptr, nullptr);

    // Decode with no-semaphore pool.
    NoSemaphorePool pool(numThreads);
    auto parallelResults = casc::blteDecodeBatch(entries, nullptr, &pool);

    if (!expect(parallelResults.size() == serialResults.size(), T,
                "result count mismatch"))
        return fail(T, "count mismatch");

    for (size_t i = 0; i < serialResults.size(); ++i) {
        if (!expect(parallelResults[i].success == serialResults[i].success, T,
                    "file %zu success mismatch", i))
            return fail(T, "success mismatch");
        if (!expect(vectorsEqual(parallelResults[i].data, serialResults[i].data), T,
                    "file %zu data mismatch", i))
            return fail(T, "data mismatch");
    }

    return pass(T, "blteDecodeBatch no-semaphore pool (%zu threads): byte-identical",
                numThreads);
}

// ============================================================================
// Test 3: blteDecodeBatch — full DAG (semaphore pool)
// ============================================================================
bool test3_blte_batch_full_dag(size_t numThreads) {
    constexpr int T = 3;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    // Create files with many frames to maximize signalOnComplete vs assembly race.
    for (int i = 0; i < 10; ++i) {
        size_t size = 20000 + i * 5000; // 20–65KB → many 512-byte frames (40–130 frames)
        rawDatas.push_back(makePatternData(size, static_cast<u8>(i + 50)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 512));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    auto serialResults = casc::blteDecodeBatch(entries, nullptr, nullptr);

    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto dagResults = casc::blteDecodeBatch(entries, nullptr, &pool);

    if (!expect(dagResults.size() == serialResults.size(), T,
                "result count mismatch"))
        return fail(T, "count mismatch");

    for (size_t i = 0; i < serialResults.size(); ++i) {
        if (!expect(dagResults[i].success == serialResults[i].success, T,
                    "file %zu success mismatch", i))
            return fail(T, "success mismatch");
        if (!expect(vectorsEqual(dagResults[i].data, serialResults[i].data), T,
                    "file %zu data mismatch (expected %zu, got %zu)", i,
                    serialResults[i].data.size(), dagResults[i].data.size()))
            return fail(T, "data mismatch");
    }

    return pass(T, "blteDecodeBatch full DAG (%zu threads): byte-identical", numThreads);
}

// ============================================================================
// Test 4: blteDecodeBatch — mixed inputs (single/multi/empty/invalid)
// ============================================================================
bool test4_blte_batch_mixed() {
    constexpr int T = 4;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;

    // File 0: multi-frame
    rawDatas.push_back(makePatternData(10000, 1));
    blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 1024));

    // File 1: single-frame
    rawDatas.push_back(makePatternData(500, 2));
    blteBlobs.push_back(makeBlteSingleFrame(rawDatas.back()));

    // File 2: empty data
    rawDatas.push_back(std::vector<u8>{});
    blteBlobs.push_back(makeBlteSingleFrame(rawDatas.back()));

    // File 3: multi-frame
    rawDatas.push_back(makePatternData(8000, 3));
    blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 512));

    // File 4: invalid (truncated)
    rawDatas.push_back({}); // placeholder
    blteBlobs.push_back({'B', 'L', 'T'}); // too short

    // File 5: multi-frame
    rawDatas.push_back(makeCompressibleData(15000));
    blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 2048));

    std::vector<casc::BlteBatchEntry> entries;
    for (auto& b : blteBlobs)
        entries.push_back({b});

    whiteout::utils::SimpleThreadPool pool(4);
    auto results = casc::blteDecodeBatch(entries, nullptr, &pool);

    if (!expect(results.size() == 6, T, "expected 6 results"))
        return fail(T, "count mismatch");

    // File 0: success
    if (!expect(results[0].success, T, "file 0 should succeed"))
        return fail(T, "file 0 failed: %s", results[0].error.c_str());
    if (!expect(vectorsEqual(results[0].data, rawDatas[0]), T, "file 0 data mismatch"))
        return fail(T, "file 0 data mismatch");

    // File 1: success (single frame)
    if (!expect(results[1].success, T, "file 1 should succeed"))
        return fail(T, "file 1 failed");
    if (!expect(vectorsEqual(results[1].data, rawDatas[1]), T, "file 1 data mismatch"))
        return fail(T, "file 1 data mismatch");

    // File 2: success (empty)
    if (!expect(results[2].success, T, "file 2 (empty) should succeed"))
        return fail(T, "file 2 failed");

    // File 3: success
    if (!expect(results[3].success, T, "file 3 should succeed"))
        return fail(T, "file 3 failed: %s", results[3].error.c_str());
    if (!expect(vectorsEqual(results[3].data, rawDatas[3]), T, "file 3 data mismatch"))
        return fail(T, "file 3 data mismatch");

    // File 4: should fail (invalid)
    if (!expect(!results[4].success, T, "file 4 (invalid) should fail"))
        return fail(T, "file 4 unexpectedly succeeded");
    if (!expect(!results[4].error.empty(), T, "file 4 should have error message"))
        return fail(T, "file 4 missing error");

    // File 5: success
    if (!expect(results[5].success, T, "file 5 should succeed"))
        return fail(T, "file 5 failed: %s", results[5].error.c_str());
    if (!expect(vectorsEqual(results[5].data, rawDatas[5]), T, "file 5 data mismatch"))
        return fail(T, "file 5 data mismatch");

    return pass(T, "blteDecodeBatch mixed inputs: per-file error isolation works");
}

// ============================================================================
// Test 5: blteDecodeBatch — single-thread stress (deadlock detection)
// ============================================================================
bool test5_blte_batch_single_thread() {
    constexpr int T = 5;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    // 20 multi-frame files through 1-thread pool.
    for (int i = 0; i < 20; ++i) {
        rawDatas.push_back(makePatternData(8000 + i * 1000, static_cast<u8>(i)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 512));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    auto serialResults = casc::blteDecodeBatch(entries, nullptr, nullptr);

    whiteout::utils::SimpleThreadPool pool(1);
    auto results = casc::blteDecodeBatch(entries, nullptr, &pool);

    for (size_t i = 0; i < serialResults.size(); ++i) {
        if (!expect(results[i].success == serialResults[i].success, T,
                    "file %zu success mismatch", i))
            return fail(T, "success mismatch");
        if (!expect(vectorsEqual(results[i].data, serialResults[i].data), T,
                    "file %zu data mismatch", i))
            return fail(T, "data mismatch");
    }

    return pass(T, "blteDecodeBatch 1-thread stress (20 multi-frame files): no deadlock");
}

// ============================================================================
// Test 6: readBatch — serial baseline (no pool, uses real CASC corpus)
// ============================================================================
bool test6_readBatch_serial(const std::string& corpusPath) {
    constexpr int T = 6;

    auto storage = casc::Storage::open(corpusPath, kCorpusLocale);
    if (!storage) return skip(T, "corpus not found at %s", corpusPath.c_str());

    // Collect some file paths.
    std::vector<std::string> paths;
    storage->enumerate([&](const casc::FindEntry& fe) {
        if (paths.size() >= 20) return false;
        if (!fe.path.empty()) paths.push_back(fe.path);
        return true;
    });

    if (paths.empty()) return skip(T, "no files in corpus");

    // Read individually.
    std::vector<std::optional<std::vector<u8>>> individual;
    for (auto& p : paths) {
        individual.push_back(storage->readFile(p));
    }

    // Read via batch.
    std::vector<casc::BatchReadRequest> requests;
    for (auto& p : paths) {
        casc::BatchReadRequest req;
        req.path = p;
        requests.push_back(req);
    }

    auto batchResults = storage->readBatch(requests);

    if (!expect(batchResults.size() == paths.size(), T, "result count mismatch"))
        return fail(T, "count mismatch");

    int matched = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        bool indivOk = individual[i].has_value();
        bool batchOk = batchResults[i].success;
        if (!expect(indivOk == batchOk, T,
                    "file %zu: individual=%s batch=%s",
                    i, indivOk ? "ok" : "fail", batchOk ? "ok" : "fail"))
            return fail(T, "success/fail mismatch");
        if (indivOk && batchOk) {
            if (!expect(vectorsEqual(batchResults[i].data, *individual[i]), T,
                        "file %zu data mismatch (individual=%zu vs batch=%zu)",
                        i, individual[i]->size(), batchResults[i].data.size()))
                return fail(T, "data mismatch");
            ++matched;
        }
    }

    return pass(T, "readBatch serial: %d/%zu files byte-identical to individual reads", matched, paths.size());
}

// ============================================================================
// Test 7: readBatch — no-semaphore pool
// ============================================================================
bool test7_readBatch_no_semaphore(const std::string& corpusPath, size_t numThreads) {
    constexpr int T = 7;

    NoSemaphorePool pool(numThreads);
    auto storage = casc::Storage::open(corpusPath, kCorpusLocale, &pool);
    if (!storage) return skip(T, "corpus not found at %s", corpusPath.c_str());

    // Also open serial for comparison.
    auto serial = casc::Storage::open(corpusPath, kCorpusLocale);
    if (!serial) return skip(T, "serial open failed");

    std::vector<std::string> paths;
    storage->enumerate([&](const casc::FindEntry& fe) {
        if (paths.size() >= 30) return false;
        if (!fe.path.empty()) paths.push_back(fe.path);
        return true;
    });
    if (paths.empty()) return skip(T, "no files in corpus");

    std::vector<casc::BatchReadRequest> requests;
    for (auto& p : paths) {
        casc::BatchReadRequest req;
        req.path = p;
        requests.push_back(req);
    }

    // Serial baseline.
    auto serialResults = serial->readBatch(requests);
    // Parallel.
    auto parallelResults = storage->readBatch(requests);

    for (size_t i = 0; i < paths.size(); ++i) {
        if (!expect(parallelResults[i].success == serialResults[i].success, T,
                    "file %zu success mismatch", i))
            return fail(T, "success mismatch");
        if (serialResults[i].success) {
            if (!expect(vectorsEqual(parallelResults[i].data, serialResults[i].data), T,
                        "file %zu data mismatch", i))
                return fail(T, "data mismatch");
        }
    }

    return pass(T, "readBatch no-semaphore pool (%zu threads): byte-identical", numThreads);
}

// ============================================================================
// Test 8: readBatch — full DAG (semaphore pool)
// ============================================================================
bool test8_readBatch_full_dag(const std::string& corpusPath, size_t numThreads) {
    constexpr int T = 8;

    whiteout::utils::SimpleThreadPool pool(numThreads);
    auto storage = casc::Storage::open(corpusPath, kCorpusLocale, &pool);
    if (!storage) return skip(T, "corpus not found at %s", corpusPath.c_str());

    auto serial = casc::Storage::open(corpusPath, kCorpusLocale);
    if (!serial) return skip(T, "serial open failed");

    std::vector<std::string> paths;
    storage->enumerate([&](const casc::FindEntry& fe) {
        if (paths.size() >= 50) return false;
        if (!fe.path.empty()) paths.push_back(fe.path);
        return true;
    });
    if (paths.empty()) return skip(T, "no files in corpus");

    std::vector<casc::BatchReadRequest> requests;
    for (auto& p : paths) {
        casc::BatchReadRequest req;
        req.path = p;
        requests.push_back(req);
    }

    auto serialResults = serial->readBatch(requests);

    auto dagResults = storage->readBatch(requests);

    for (size_t i = 0; i < paths.size(); ++i) {
        if (!expect(dagResults[i].success == serialResults[i].success, T,
                    "file %zu success mismatch", i))
            return fail(T, "success mismatch");
        if (serialResults[i].success) {
            if (!expect(vectorsEqual(dagResults[i].data, serialResults[i].data), T,
                        "file %zu data mismatch", i))
                return fail(T, "data mismatch");
        }
    }

    return pass(T, "readBatch full DAG (%zu threads): byte-identical", numThreads);
}

// ============================================================================
// Test 9: readBatch — mixed valid/invalid paths
// ============================================================================
bool test9_readBatch_mixed_valid_invalid(const std::string& corpusPath) {
    constexpr int T = 9;

    whiteout::utils::SimpleThreadPool pool(4);
    auto storage = casc::Storage::open(corpusPath, kCorpusLocale, &pool);
    if (!storage) return skip(T, "corpus not found at %s", corpusPath.c_str());

    // Collect files that are actually readable (not just enumerated).
    std::vector<std::string> validPaths;
    {
        std::vector<std::string> candidates;
        storage->enumerate([&](const casc::FindEntry& fe) {
            if (candidates.size() >= 30) return false;
            if (!fe.path.empty()) candidates.push_back(fe.path);
            return true;
        });
        for (auto& p : candidates) {
            if (validPaths.size() >= 5) break;
            auto data = storage->readFile(p);
            if (data) validPaths.push_back(p);
        }
    }
    if (validPaths.empty()) return skip(T, "no readable files in corpus");

    // Build requests: valid, invalid, valid, invalid, valid...
    std::vector<casc::BatchReadRequest> requests;
    for (size_t i = 0; i < validPaths.size(); ++i) {
        casc::BatchReadRequest good;
        good.path = validPaths[i];
        requests.push_back(good);

        casc::BatchReadRequest bad;
        bad.path = "nonexistent/path/file_" + std::to_string(i) + ".dat";
        requests.push_back(bad);
    }

    auto results = storage->readBatch(requests);

    if (!expect(results.size() == requests.size(), T, "result count mismatch"))
        return fail(T, "count mismatch");

    for (size_t i = 0; i < requests.size(); ++i) {
        bool isValid = (i % 2 == 0);
        if (isValid) {
            if (!expect(results[i].success, T, "valid file %zu should succeed: %s",
                        i, results[i].error.c_str()))
                return fail(T, "valid file failed");
        } else {
            if (!expect(!results[i].success, T, "invalid file %zu should fail", i))
                return fail(T, "invalid file succeeded");
        }
    }

    return pass(T, "readBatch mixed valid/invalid: per-file error isolation works");
}

// ============================================================================
// Test 10: readBatch — empty request list
// ============================================================================
bool test10_readBatch_empty() {
    constexpr int T = 10;

    auto storage = casc::Storage::create();
    std::vector<casc::BatchReadRequest> requests;
    auto results = storage.readBatch(requests);

    if (!expect(results.empty(), T, "expected empty results"))
        return fail(T, "non-empty results from empty request");

    return pass(T, "readBatch empty request list: returns empty");
}

// ============================================================================
// Test 11: zero-thread pool fallback
// ============================================================================
bool test11_zero_thread_pool() {
    constexpr int T = 11;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    for (int i = 0; i < 5; ++i) {
        rawDatas.push_back(makePatternData(4000, static_cast<u8>(i)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 1024));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    ZeroThreadPool pool;
    auto results = casc::blteDecodeBatch(entries, nullptr, &pool);

    // Should fall back to serial (threadCount == 0).
    if (!expect(pool.submitted() == 0, T, "no tasks should be submitted to 0-thread pool"))
        return fail(T, "%zu tasks submitted", pool.submitted());

    for (int i = 0; i < 5; ++i) {
        if (!expect(results[i].success, T, "file %d should decode successfully", i))
            return fail(T, "file %d failed: %s", i, results[i].error.c_str());
        if (!expect(vectorsEqual(results[i].data, rawDatas[i]), T,
                    "file %d data mismatch", i))
            return fail(T, "data mismatch");
    }

    return pass(T, "zero-thread pool: serial fallback, no tasks submitted");
}

// ============================================================================
// Test 12: 1-thread pool stress
// ============================================================================
bool test12_single_thread_stress() {
    constexpr int T = 12;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    for (int i = 0; i < 25; ++i) {
        rawDatas.push_back(makePatternData(6000 + i * 500, static_cast<u8>(i)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 256));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    auto serialResults = casc::blteDecodeBatch(entries, nullptr, nullptr);

    whiteout::utils::SimpleThreadPool pool(1);
    auto results = casc::blteDecodeBatch(entries, nullptr, &pool);

    for (size_t i = 0; i < serialResults.size(); ++i) {
        if (!expect(results[i].success == serialResults[i].success, T,
                    "file %zu success mismatch", i))
            return fail(T, "success mismatch");
        if (!expect(vectorsEqual(results[i].data, serialResults[i].data), T,
                    "file %zu data mismatch", i))
            return fail(T, "data mismatch");
    }

    return pass(T, "1-thread pool stress (25 files, tiny frames): no deadlock");
}

// ============================================================================
// Test 13: iteration stress
// ============================================================================
bool test13_iteration_stress() {
    constexpr int T = 13;
    constexpr int kIterations = 30;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    for (int i = 0; i < 10; ++i) {
        rawDatas.push_back(makePatternData(5000 + i * 1000, static_cast<u8>(i)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 512));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    whiteout::utils::SimpleThreadPool pool(4);

    for (int iter = 0; iter < kIterations; ++iter) {
        auto results = casc::blteDecodeBatch(entries, nullptr, &pool);
        for (size_t i = 0; i < rawDatas.size(); ++i) {
            if (!expect(results[i].success, T,
                        "iter %d file %zu failed: %s", iter, i, results[i].error.c_str()))
                return fail(T, "failure at iteration %d", iter);
            if (!expect(vectorsEqual(results[i].data, rawDatas[i]), T,
                        "iter %d file %zu data mismatch", iter, i))
                return fail(T, "mismatch at iteration %d", iter);
        }
    }

    return pass(T, "iteration stress: %d iterations, no stale semaphore races", kIterations);
}

// ============================================================================
// Test 14: serial == parallel byte equality sweep
// ============================================================================
bool test14_serial_parallel_equality() {
    constexpr int T = 14;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    for (int i = 0; i < 15; ++i) {
        rawDatas.push_back(makePatternData(4000 + i * 2000, static_cast<u8>(i)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 512));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    auto serialResults = casc::blteDecodeBatch(entries, nullptr, nullptr);

    for (size_t tc : {1, 2, 4, 8}) {
        whiteout::utils::SimpleThreadPool pool(tc);
        auto results = casc::blteDecodeBatch(entries, nullptr, &pool);

        for (size_t i = 0; i < serialResults.size(); ++i) {
            if (!expect(vectorsEqual(results[i].data, serialResults[i].data), T,
                        "threads=%zu file %zu data mismatch", tc, i))
                return fail(T, "mismatch at thread count %zu", tc);
        }
    }

    return pass(T, "serial==parallel byte equality at thread counts 1,2,4,8");
}

// ============================================================================
// Test 15: concurrent readBatch on shared pool
// ============================================================================
bool test15_concurrent_readBatch(const std::string& corpusPath) {
    constexpr int T = 15;

    whiteout::utils::SimpleThreadPool pool(4);
    auto serial = casc::Storage::open(corpusPath, kCorpusLocale);
    if (!serial) return skip(T, "corpus not found at %s", corpusPath.c_str());

    // Collect paths.
    std::vector<std::string> allPaths;
    serial->enumerate([&](const casc::FindEntry& fe) {
        if (allPaths.size() >= 40) return false;
        if (!fe.path.empty()) allPaths.push_back(fe.path);
        return true;
    });
    if (allPaths.size() < 16)
        return skip(T, "not enough files in corpus (%zu)", allPaths.size());

    // Split into 4 non-overlapping sets.
    size_t perThread = allPaths.size() / 4;

    auto storage = casc::Storage::open(corpusPath, kCorpusLocale, &pool);
    if (!storage) return skip(T, "parallel open failed");

    // Compute serial baseline for all.
    std::vector<casc::BatchReadRequest> allReqs;
    for (auto& p : allPaths) {
        casc::BatchReadRequest req;
        req.path = p;
        allReqs.push_back(req);
    }
    auto serialResults = serial->readBatch(allReqs);

    // Launch 4 OS threads, each calling readBatch.
    std::vector<std::vector<casc::BatchReadResult>> threadResults(4);
    std::vector<std::thread> threads;
    std::atomic<bool> anyFail{false};

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            size_t start = t * perThread;
            size_t end = (t == 3) ? allPaths.size() : start + perThread;

            std::vector<casc::BatchReadRequest> reqs;
            for (size_t i = start; i < end; ++i) {
                casc::BatchReadRequest req;
                req.path = allPaths[i];
                reqs.push_back(req);
            }

            threadResults[t] = storage->readBatch(reqs);
        });
    }

    for (auto& t : threads) t.join();

    // Verify each thread's results match the serial baseline.
    for (int t = 0; t < 4; ++t) {
        size_t start = t * perThread;
        size_t end = (t == 3) ? allPaths.size() : start + perThread;
        size_t count = end - start;

        if (!expect(threadResults[t].size() == count, T,
                    "thread %d: result count mismatch (%zu vs %zu)",
                    t, threadResults[t].size(), count))
            return fail(T, "count mismatch");

        for (size_t i = 0; i < count; ++i) {
            size_t si = start + i;
            if (serialResults[si].success) {
                if (!expect(threadResults[t][i].success, T,
                            "thread %d file %zu: should succeed", t, i))
                    return fail(T, "concurrent batch failure");
                if (!expect(vectorsEqual(threadResults[t][i].data, serialResults[si].data), T,
                            "thread %d file %zu: data mismatch", t, i))
                    return fail(T, "concurrent data mismatch");
            }
        }
    }

    return pass(T, "concurrent readBatch (4 OS threads on shared pool): no cross-contamination");
}

// ============================================================================
// Test 16: massive batch — 100+ files
// ============================================================================
bool test16_massive_batch() {
    constexpr int T = 16;
    constexpr int kFileCount = 100;

    std::vector<std::vector<u8>> rawDatas;
    std::vector<std::vector<u8>> blteBlobs;
    std::vector<casc::BlteBatchEntry> entries;

    for (int i = 0; i < kFileCount; ++i) {
        rawDatas.push_back(makePatternData(3000 + (i % 20) * 500, static_cast<u8>(i & 0xFF)));
        blteBlobs.push_back(makeBlteMultiFrame(rawDatas.back(), 512));
    }
    for (auto& b : blteBlobs)
        entries.push_back({b});

    auto serialResults = casc::blteDecodeBatch(entries, nullptr, nullptr);

    whiteout::utils::SimpleThreadPool pool(8);
    auto results = casc::blteDecodeBatch(entries, nullptr, &pool);

    for (int i = 0; i < kFileCount; ++i) {
        if (!expect(results[i].success == serialResults[i].success, T,
                    "file %d success mismatch", i))
            return fail(T, "success mismatch");
        if (!expect(vectorsEqual(results[i].data, serialResults[i].data), T,
                    "file %d data mismatch", i))
            return fail(T, "data mismatch at file %d", i);
    }

    return pass(T, "massive batch (%d files, 8 threads): all correct", kFileCount);
}

// ============================================================================
// Test 17: encrypted BLTE batch (KeyRing concurrent findKey)
// ============================================================================
bool test17_encrypted_batch() {
    constexpr int T = 17;

    // We test KeyRing thread safety by calling findKey concurrently
    // from multiple threads. We don't have actual encrypted BLTE data,
    // so we just verify the atomic m_firstMissing works correctly.
    casc::KeyRing ring;
    ring.addKey(0x1234, "00112233445566778899AABBCCDDEEFF");

    constexpr size_t kThreads = 8;
    constexpr size_t kIterations = 10000;

    std::vector<std::thread> threads;
    std::atomic<bool> anyRace{false};

    for (size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (size_t i = 0; i < kIterations; ++i) {
                // Alternate between found and missing keys.
                auto found = ring.findKey(0x1234);
                if (!found) {
                    anyRace.store(true, std::memory_order_relaxed);
                }
                // Different missing key per thread.
                ring.findKey(0xDEAD0000 + t);
            }
        });
    }

    for (auto& t : threads) t.join();

    if (!expect(!anyRace.load(), T, "existing key lookup returned nullptr"))
        return fail(T, "race condition in findKey");

    auto missing = ring.firstMissingKey();
    if (!expect(missing.has_value(), T, "should have a first missing key"))
        return fail(T, "no missing key recorded");

    // The first missing key should be one of the 0xDEAD0000+t values.
    u64 val = *missing;
    bool validMissing = (val >= 0xDEAD0000 && val < 0xDEAD0000 + kThreads);
    if (!expect(validMissing, T, "first missing key 0x%llX not in expected range",
                (unsigned long long)val))
        return fail(T, "unexpected missing key value");

    return pass(T, "KeyRing concurrent findKey: atomic m_firstMissing works");
}

// ============================================================================
// Test 18: readBatch vs individual readFile
// ============================================================================
bool test18_readBatch_vs_individual(const std::string& corpusPath) {
    constexpr int T = 18;

    whiteout::utils::SimpleThreadPool pool(4);
    auto storage = casc::Storage::open(corpusPath, kCorpusLocale, &pool);
    if (!storage) return skip(T, "corpus not found at %s", corpusPath.c_str());

    std::vector<std::string> paths;
    storage->enumerate([&](const casc::FindEntry& fe) {
        if (paths.size() >= 30) return false;
        if (!fe.path.empty()) paths.push_back(fe.path);
        return true;
    });
    if (paths.empty()) return skip(T, "no files in corpus");

    // Individual reads.
    std::vector<std::optional<std::vector<u8>>> individual;
    for (auto& p : paths) {
        individual.push_back(storage->readFile(p));
    }

    // Batch read.
    std::vector<casc::BatchReadRequest> requests;
    for (auto& p : paths) {
        casc::BatchReadRequest req;
        req.path = p;
        requests.push_back(req);
    }
    auto batchResults = storage->readBatch(requests);

    int matched = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        bool indivOk = individual[i].has_value();
        bool batchOk = batchResults[i].success;
        if (!expect(indivOk == batchOk, T,
                    "file %zu: individual=%s batch=%s",
                    i, indivOk ? "ok" : "fail", batchOk ? "ok" : "fail"))
            return fail(T, "success/fail mismatch");
        if (indivOk && batchOk) {
            if (!expect(vectorsEqual(batchResults[i].data, *individual[i]), T,
                        "file %zu data mismatch (individual=%zu batch=%zu)",
                        i, individual[i]->size(), batchResults[i].data.size()))
                return fail(T, "data mismatch");
            ++matched;
        }
    }

    return pass(T, "readBatch vs individual readFile: byte-identical (%d/%zu files)", matched, paths.size());
}

// ============================================================================
// Test 19/20: real corpus tests
// ============================================================================
bool test19_real_corpus_d3(const std::string& corpusBase) {
    constexpr int T = 19;
    std::string d3Path = corpusBase + "/Diablo III";
    if (!fs::exists(d3Path))
        d3Path = corpusBase + "/D3";
    if (!fs::exists(d3Path))
        d3Path = corpusBase + "/d3";
    if (!fs::exists(d3Path))
        return skip(T, "D3 CASC corpus not found");

    whiteout::utils::SimpleThreadPool pool(4);
    auto storage = casc::Storage::open(d3Path, kCorpusLocale, &pool);
    if (!storage) return skip(T, "D3 corpus open failed");

    auto serial = casc::Storage::open(d3Path, kCorpusLocale);
    if (!serial) return skip(T, "D3 serial open failed");

    // Enumerate candidates, then filter to actually-readable files.
    std::vector<std::string> candidates;
    serial->enumerate([&](const casc::FindEntry& fe) {
        if (candidates.size() >= 200) return false;
        if (!fe.path.empty()) candidates.push_back(fe.path);
        return true;
    });
    std::vector<std::string> paths;
    for (auto& p : candidates) {
        if (paths.size() >= 50) break;
        if (serial->readFile(p)) paths.push_back(p);
    }
    if (paths.empty()) return skip(T, "no readable files in D3 corpus");

    std::vector<casc::BatchReadRequest> requests;
    for (auto& p : paths) {
        casc::BatchReadRequest req;
        req.path = p;
        requests.push_back(req);
    }

    auto serialResults = serial->readBatch(requests);
    auto dagResults = storage->readBatch(requests);

    int matches = 0, bothFailed = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        bool sOk = serialResults[i].success;
        bool dOk = dagResults[i].success;
        if (!expect(sOk == dOk, T,
                    "D3 file %zu: serial=%s dag=%s",
                    i, sOk ? "ok" : "fail", dOk ? "ok" : "fail"))
            return fail(T, "D3 success/fail disagreement");
        if (sOk && dOk) {
            if (!expect(vectorsEqual(dagResults[i].data, serialResults[i].data), T,
                        "D3 file %zu data mismatch", i))
                return fail(T, "D3 data mismatch");
            ++matches;
        } else {
            ++bothFailed;
        }
    }

    return pass(T, "real corpus D3: %d/%zu readable, %d unreadable (0 disagreements)",
                matches, paths.size(), bothFailed);
}

bool test20_real_corpus_wc3(const std::string& corpusBase) {
    constexpr int T = 20;
    std::string wc3Path = corpusBase + "/Warcraft III";
    if (!fs::exists(wc3Path))
        wc3Path = corpusBase + "/WC3";
    if (!fs::exists(wc3Path))
        wc3Path = corpusBase + "/wc3";
    if (!fs::exists(wc3Path))
        return skip(T, "WC3 CASC corpus not found");

    whiteout::utils::SimpleThreadPool pool(4);
    auto storage = casc::Storage::open(wc3Path, kCorpusLocale, &pool);
    if (!storage) return skip(T, "WC3 corpus open failed");

    auto serial = casc::Storage::open(wc3Path, kCorpusLocale);
    if (!serial) return skip(T, "WC3 serial open failed");

    // Enumerate candidates, then filter to actually-readable files.
    std::vector<std::string> candidates;
    serial->enumerate([&](const casc::FindEntry& fe) {
        if (candidates.size() >= 200) return false;
        if (!fe.path.empty()) candidates.push_back(fe.path);
        return true;
    });
    std::vector<std::string> paths;
    for (auto& p : candidates) {
        if (paths.size() >= 50) break;
        if (serial->readFile(p)) paths.push_back(p);
    }
    if (paths.empty()) return skip(T, "no readable files in WC3 corpus");

    std::vector<casc::BatchReadRequest> requests;
    for (auto& p : paths) {
        casc::BatchReadRequest req;
        req.path = p;
        requests.push_back(req);
    }

    auto serialResults = serial->readBatch(requests);
    auto dagResults = storage->readBatch(requests);

    int matches = 0, bothFailed = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        bool sOk = serialResults[i].success;
        bool dOk = dagResults[i].success;
        if (!expect(sOk == dOk, T,
                    "WC3 file %zu: serial=%s dag=%s",
                    i, sOk ? "ok" : "fail", dOk ? "ok" : "fail"))
            return fail(T, "WC3 success/fail disagreement");
        if (sOk && dOk) {
            if (!expect(vectorsEqual(dagResults[i].data, serialResults[i].data), T,
                        "WC3 file %zu data mismatch", i))
                return fail(T, "WC3 data mismatch");
            ++matches;
        } else {
            ++bothFailed;
        }
    }

    return pass(T, "real corpus WC3: %d/%zu readable, %d unreadable (0 disagreements)",
                matches, paths.size(), bothFailed);
}

} // anonymous namespace

// ============================================================================
// Main
// ============================================================================


// ============================================================================
// Catch2 test cases
// ============================================================================

static std::string findCascCorpus() {
    for (auto& p : {"Corpus/CASC/D3", "../../Corpus/CASC/D3", "../Corpus/CASC/D3"})
        if (std::filesystem::exists(p)) return p;
    return "";
}

TEST_CASE("test1 blte batch serial", "[casc][race]") {
    REQUIRE(test1_blte_batch_serial());
}

TEST_CASE("test2 blte batch no semaphore", "[casc][race]") {
    auto threads = GENERATE(1, 2, 4);
    REQUIRE(test2_blte_batch_no_semaphore(threads));
}

TEST_CASE("test3 blte batch full dag", "[casc][race]") {
    auto threads = GENERATE(1, 2, 4);
    REQUIRE(test3_blte_batch_full_dag(threads));
}

TEST_CASE("test4 blte batch mixed", "[casc][race]") {
    REQUIRE(test4_blte_batch_mixed());
}

TEST_CASE("test5 blte batch single thread", "[casc][race]") {
    REQUIRE(test5_blte_batch_single_thread());
}

TEST_CASE("test6 readBatch serial", "[casc][race][corpus]") {
    auto corpus = findCascCorpus();
    if (corpus.empty()) SKIP("CASC corpus not found");
    REQUIRE(test6_readBatch_serial(corpus));
}

TEST_CASE("test7 readBatch no semaphore", "[casc][race][corpus]") {
    auto corpus = findCascCorpus();
    if (corpus.empty()) SKIP("CASC corpus not found");
    auto threads = GENERATE(1, 2, 4);
    REQUIRE(test7_readBatch_no_semaphore(corpus, threads));
}

TEST_CASE("test8 readBatch full dag", "[casc][race][corpus]") {
    auto corpus = findCascCorpus();
    if (corpus.empty()) SKIP("CASC corpus not found");
    auto threads = GENERATE(1, 2, 4);
    REQUIRE(test8_readBatch_full_dag(corpus, threads));
}

TEST_CASE("test9 readBatch mixed valid invalid", "[casc][race][corpus]") {
    auto corpus = findCascCorpus();
    if (corpus.empty()) SKIP("CASC corpus not found");
    REQUIRE(test9_readBatch_mixed_valid_invalid(corpus));
}

TEST_CASE("test10 readBatch empty", "[casc][race]") {
    REQUIRE(test10_readBatch_empty());
}

TEST_CASE("test11 zero thread pool", "[casc][race]") {
    REQUIRE(test11_zero_thread_pool());
}

TEST_CASE("test12 single thread stress", "[casc][race]") {
    REQUIRE(test12_single_thread_stress());
}

TEST_CASE("test13 iteration stress", "[casc][race]") {
    REQUIRE(test13_iteration_stress());
}

TEST_CASE("test14 serial parallel equality", "[casc][race]") {
    REQUIRE(test14_serial_parallel_equality());
}

TEST_CASE("test15 concurrent readBatch", "[casc][race][corpus]") {
    auto corpus = findCascCorpus();
    if (corpus.empty()) SKIP("CASC corpus not found");
    REQUIRE(test15_concurrent_readBatch(corpus));
}

TEST_CASE("test16 massive batch", "[casc][race]") {
    REQUIRE(test16_massive_batch());
}

TEST_CASE("test17 encrypted batch", "[casc][race]") {
    REQUIRE(test17_encrypted_batch());
}

TEST_CASE("test18 readBatch vs individual", "[casc][race][corpus]") {
    auto corpus = findCascCorpus();
    if (corpus.empty()) SKIP("CASC corpus not found");
    REQUIRE(test18_readBatch_vs_individual(corpus));
}

TEST_CASE("test19 real corpus d3", "[casc][race][corpus]") {
    auto corpus = findCascCorpus();
    if (corpus.empty()) SKIP("CASC corpus not found");
    REQUIRE(test19_real_corpus_d3(corpus));
}

TEST_CASE("test20 real corpus wc3", "[casc][race][corpus]") {
    auto corpus = findCascCorpus();
    if (corpus.empty()) SKIP("CASC corpus not found");
    REQUIRE(test20_real_corpus_wc3(corpus));
}


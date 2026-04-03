// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Online CASC performance cross-reference test.
///
/// Benchmarks OnlineStorage (CDN-backed) against CascLib's C API for every
/// measurable read-only operation, reporting per-operation timings and
/// speedup ratios.  Network latency is the dominant factor; the test
/// measures end-to-end wall time including HTTP round-trips.
///
/// Build with -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON to enable the CascLib
/// comparison path.  Without it, only WhiteoutLib benchmarks are collected.
///
/// Usage:
///   online_perf_crossref_test.exe <product> [options]
///
///   product      — Blizzard product code ("w3", "s1", "wow", "hero", etc.)
///
/// Options:
///   --region <r>       CDN region (default: us)
///   --csv <path>       CSV output path (default: online_perf_crossref.csv)
///   --sample <n>       Max sample size for per-file benchmarks (default: 200)
///   --http-threads <n> Number of HTTP worker threads (default: 4)
///   --pool-threads <n> Number of WorkerPool threads (default: hardware_concurrency)
///   --cache-dir <d>    Local cache directory for OnlineStorage
///   --cache-size <mb>  In-memory cache size in MB (default: 256)
///   --help, -h         Show this help
///
/// Operations benchmarked:
///   1.  Storage open (cold)
///   2.  Storage open (warm, with cache)
///   3.  Prefetch
///   4.  Enumerate (list all entries)
///   5.  File-exists lookup (by path)
///   6.  File-exists lookup (by FileDataId)
///   7.  File-size query (by path)
///   8.  File-size query (by FileDataId)
///   9.  File-info query (by path)
///  10.  Single file read (by path) — per-file latency
///  11.  Single file read (by FileDataId) — per-file latency
///  12.  Read by size category — small (<4KB), medium (4KB-256KB), large (>256KB)
///  13.  Batch sequential read — total throughput
///  14.  readBatch (parallel)
///  15.  Repeated read (same file, hot-path / cache-hit benchmark)
///  16.  List all file paths
///  17.  Total file count
///  18.  Cache flush

#include <whiteout/storages/casc/online_storage.h>
#include <whiteout/utils/simple_http_handler.h>
#include <whiteout/utils/simple_thread_pool.h>

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
#include <CascLib.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ============================================================================
// RAII helpers for CascLib
// ============================================================================

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)

struct ScopedCascStorage {
    HANDLE h = nullptr;
    ~ScopedCascStorage() { if (h) CascCloseStorage(h); }
    explicit operator bool() const { return h != nullptr; }
};

struct ScopedCascFile {
    HANDLE h = nullptr;
    ~ScopedCascFile() { if (h) CascCloseFile(h); }
    explicit operator bool() const { return h != nullptr; }
};

struct ScopedCascFind {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~ScopedCascFind() { if (h != INVALID_HANDLE_VALUE) CascFindClose(h); }
    explicit operator bool() const { return h != INVALID_HANDLE_VALUE; }
};

#endif // WHITEOUT_HAS_CASCLIB_CROSSVAL

// ============================================================================
// Benchmark result types
// ============================================================================

struct BenchResult {
    std::string operation;
    std::string sizeCategory;     // "" or "small"/"medium"/"large"
    double whiteoutUs  = 0.0;     // total microseconds (WhiteoutLib)
    double cascLibUs   = 0.0;     // total microseconds (CascLib), 0 if unavailable
    u32    iterations  = 0;       // how many times the operation ran
    u64    bytesTotal  = 0;       // total bytes processed (for throughput ops)
    bool   hasCascLib  = false;   // whether CascLib data is present
};

// ============================================================================
// Statistics helpers
// ============================================================================

struct LatencyStats {
    u32    count = 0;
    double totalUs = 0.0;
    double minUs = 0.0;
    double maxUs = 0.0;
    double avgUs = 0.0;
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
};

static double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p / 100.0 * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static LatencyStats computeLatencyStats(std::vector<double>& latencies) {
    LatencyStats s;
    if (latencies.empty()) return s;
    s.count = static_cast<u32>(latencies.size());
    s.totalUs = 0.0;
    for (auto v : latencies) s.totalUs += v;
    std::sort(latencies.begin(), latencies.end());
    s.minUs = latencies.front();
    s.maxUs = latencies.back();
    s.avgUs = s.totalUs / static_cast<double>(s.count);
    s.p50Us = percentile(latencies, 50.0);
    s.p95Us = percentile(latencies, 95.0);
    s.p99Us = percentile(latencies, 99.0);
    return s;
}

// ============================================================================
// Timing macro
// ============================================================================

#define BENCH_TIME_US(varName, code)                                 \
    double varName;                                                  \
    do {                                                             \
        auto _bench_t0 = Clock::now();                               \
        code;                                                        \
        auto _bench_t1 = Clock::now();                               \
        varName = std::chrono::duration<double, std::micro>(         \
                      _bench_t1 - _bench_t0).count();                \
    } while (0)

// ============================================================================
// Result reporting
// ============================================================================

static void printResultsTable(const std::vector<BenchResult>& results) {
    std::cout << "\n";
    std::cout << std::left << std::setw(40) << "Operation"
              << std::right << std::setw(8) << "Iters"
              << std::setw(14) << "Whiteout(us)"
              << std::setw(14) << "CascLib(us)"
              << std::setw(12) << "Speedup"
              << std::setw(14) << "WO MB/s"
              << std::setw(14) << "CL MB/s"
              << "\n";
    std::cout << std::string(116, '-') << "\n";

    for (auto& r : results) {
        std::string label = r.operation;
        if (!r.sizeCategory.empty())
            label += " [" + r.sizeCategory + "]";

        double woPerIter = (r.iterations > 0)
            ? r.whiteoutUs / static_cast<double>(r.iterations)
            : r.whiteoutUs;
        double clPerIter = (r.iterations > 0 && r.hasCascLib)
            ? r.cascLibUs / static_cast<double>(r.iterations)
            : 0.0;

        double speedup = (r.hasCascLib && r.cascLibUs > 0.0)
            ? r.cascLibUs / r.whiteoutUs
            : 0.0;

        // Throughput in MB/s
        double woThroughput = 0.0, clThroughput = 0.0;
        if (r.bytesTotal > 0 && r.whiteoutUs > 0.0)
            woThroughput = (static_cast<double>(r.bytesTotal) / (1024.0 * 1024.0))
                         / (r.whiteoutUs / 1e6);
        if (r.bytesTotal > 0 && r.hasCascLib && r.cascLibUs > 0.0)
            clThroughput = (static_cast<double>(r.bytesTotal) / (1024.0 * 1024.0))
                         / (r.cascLibUs / 1e6);

        std::cout << std::left << std::setw(40) << label
                  << std::right << std::setw(8) << r.iterations
                  << std::fixed << std::setprecision(1)
                  << std::setw(14) << woPerIter;

        if (r.hasCascLib)
            std::cout << std::setw(14) << clPerIter;
        else
            std::cout << std::setw(14) << "n/a";

        if (speedup > 0.0) {
            std::cout << std::setw(11) << std::setprecision(2) << speedup << "x";
            if (speedup >= 1.0)
                std::cout << " ";  // we're faster or equal
            else
                std::cout << "!";  // CascLib is faster — attention needed
        } else {
            std::cout << std::setw(12) << "n/a";
        }

        if (woThroughput > 0.0)
            std::cout << std::setw(14) << std::setprecision(1) << woThroughput;
        else
            std::cout << std::setw(14) << "";

        if (clThroughput > 0.0)
            std::cout << std::setw(14) << std::setprecision(1) << clThroughput;
        else
            std::cout << std::setw(14) << "";

        std::cout << "\n";
    }
    std::cout << "\n";
}

static void writeResultsCsv(const std::string& path, const std::vector<BenchResult>& results) {
    std::ofstream f(path);
    if (!f) {
        std::cerr << "WARNING: could not write " << path << "\n";
        return;
    }
    f << "operation,sizeCategory,iterations,whiteoutTotalUs,cascLibTotalUs,"
         "whiteoutPerIterUs,cascLibPerIterUs,speedup,bytesTotal,"
         "whiteoutMBs,cascLibMBs\n";

    for (auto& r : results) {
        double woPerIter = (r.iterations > 0)
            ? r.whiteoutUs / static_cast<double>(r.iterations)
            : r.whiteoutUs;
        double clPerIter = (r.iterations > 0 && r.hasCascLib)
            ? r.cascLibUs / static_cast<double>(r.iterations)
            : 0.0;
        double speedup = (r.hasCascLib && r.cascLibUs > 0.0)
            ? r.cascLibUs / r.whiteoutUs : 0.0;
        double woMBs = 0.0, clMBs = 0.0;
        if (r.bytesTotal > 0 && r.whiteoutUs > 0.0)
            woMBs = (static_cast<double>(r.bytesTotal) / (1024.0 * 1024.0))
                  / (r.whiteoutUs / 1e6);
        if (r.bytesTotal > 0 && r.hasCascLib && r.cascLibUs > 0.0)
            clMBs = (static_cast<double>(r.bytesTotal) / (1024.0 * 1024.0))
                  / (r.cascLibUs / 1e6);

        f << r.operation << ","
          << r.sizeCategory << ","
          << r.iterations << ","
          << std::fixed << std::setprecision(1)
          << r.whiteoutUs << ","
          << (r.hasCascLib ? r.cascLibUs : 0.0) << ","
          << woPerIter << ","
          << clPerIter << ","
          << std::setprecision(3) << speedup << ","
          << r.bytesTotal << ","
          << std::setprecision(1) << woMBs << ","
          << clMBs << "\n";
    }

    std::cout << "Wrote CSV results: " << path << "\n";
}

// ============================================================================
// Detailed latency report for read operations
// ============================================================================

static void printLatencyComparison(const std::string& label,
                                   std::vector<double>& woLatencies,
                                   std::vector<double>& clLatencies) {
    auto wo = computeLatencyStats(woLatencies);
    std::cout << "\n  " << label << " — Per-Read Latency Distribution\n";
    std::cout << "  " << std::string(70, '-') << "\n";
    std::cout << "  " << std::setw(20) << "" << std::setw(14) << "Whiteout"
              << std::setw(14) << "CascLib" << std::setw(12) << "Speedup" << "\n";
    std::cout << "  " << std::string(60, '-') << "\n";

    auto row = [&](const char* metric, double woVal, double clVal) {
        std::cout << "  " << std::setw(20) << metric
                  << std::fixed << std::setprecision(1)
                  << std::setw(14) << woVal;
        if (clVal > 0.0) {
            std::cout << std::setw(14) << clVal
                      << std::setprecision(2) << std::setw(11)
                      << (clVal / woVal) << "x";
        } else {
            std::cout << std::setw(14) << "n/a" << std::setw(12) << "";
        }
        std::cout << "\n";
    };

    LatencyStats cl{};
    if (!clLatencies.empty())
        cl = computeLatencyStats(clLatencies);

    row("Count", static_cast<double>(wo.count), static_cast<double>(cl.count));
    row("Min (us)", wo.minUs, cl.minUs);
    row("Avg (us)", wo.avgUs, cl.avgUs);
    row("P50 (us)", wo.p50Us, cl.p50Us);
    row("P95 (us)", wo.p95Us, cl.p95Us);
    row("P99 (us)", wo.p99Us, cl.p99Us);
    row("Max (us)", wo.maxUs, cl.maxUs);
    row("Total (us)", wo.totalUs, cl.totalUs);
}

// ============================================================================
// Shared entry type
// ============================================================================

struct TestEntry {
    std::string path;
    i32         fileDataId = 0;
    u64         fileSize = 0;
};

// ============================================================================
// CascLib online benchmark helpers
// ============================================================================

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)

static std::optional<std::vector<u8>> cascLibReadFile(HANDLE hStorage,
                                                      const std::string& path) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, path.c_str(), 0, CASC_OPEN_BY_NAME, &file.h))
        return std::nullopt;

    ULONGLONG fileSize64 = 0;
    if (!CascGetFileSize64(file.h, &fileSize64))
        return std::nullopt;

    if (fileSize64 > static_cast<ULONGLONG>(1u << 30))
        return std::nullopt;

    auto fileSize = static_cast<size_t>(fileSize64);
    std::vector<u8> buffer(fileSize);
    size_t totalRead = 0;

    while (totalRead < fileSize) {
        auto chunk = static_cast<DWORD>(
            std::min<size_t>(fileSize - totalRead, 0x10000000u));
        DWORD bytesRead = 0;
        if (!CascReadFile(file.h, buffer.data() + totalRead, chunk, &bytesRead)) {
            DWORD err = GetCascError();
            if (err != ERROR_HANDLE_EOF) return std::nullopt;
            totalRead += bytesRead;
            break;
        }
        totalRead += bytesRead;
        if (bytesRead == 0) break;
    }
    buffer.resize(totalRead);
    return buffer;
}

static std::optional<std::vector<u8>> cascLibReadFileById(HANDLE hStorage,
                                                          i32 fileDataId) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, CASC_FILE_DATA_ID(fileDataId), 0,
                      CASC_OPEN_BY_FILEID, &file.h))
        return std::nullopt;

    ULONGLONG fileSize64 = 0;
    if (!CascGetFileSize64(file.h, &fileSize64))
        return std::nullopt;

    if (fileSize64 > static_cast<ULONGLONG>(1u << 30))
        return std::nullopt;

    auto fileSize = static_cast<size_t>(fileSize64);
    std::vector<u8> buffer(fileSize);
    size_t totalRead = 0;

    while (totalRead < fileSize) {
        auto chunk = static_cast<DWORD>(
            std::min<size_t>(fileSize - totalRead, 0x10000000u));
        DWORD bytesRead = 0;
        if (!CascReadFile(file.h, buffer.data() + totalRead, chunk, &bytesRead)) {
            DWORD err = GetCascError();
            if (err != ERROR_HANDLE_EOF) return std::nullopt;
            totalRead += bytesRead;
            break;
        }
        totalRead += bytesRead;
        if (bytesRead == 0) break;
    }
    buffer.resize(totalRead);
    return buffer;
}

static bool cascLibFileExists(HANDLE hStorage, const std::string& path) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, path.c_str(), 0, CASC_OPEN_BY_NAME, &file.h))
        return false;
    return true;
}

static bool cascLibFileExistsById(HANDLE hStorage, i32 fileDataId) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, CASC_FILE_DATA_ID(fileDataId), 0,
                      CASC_OPEN_BY_FILEID, &file.h))
        return false;
    return true;
}

static std::optional<u64> cascLibFileSize(HANDLE hStorage, const std::string& path) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, path.c_str(), 0, CASC_OPEN_BY_NAME, &file.h))
        return std::nullopt;
    ULONGLONG sz = 0;
    if (!CascGetFileSize64(file.h, &sz))
        return std::nullopt;
    return static_cast<u64>(sz);
}

static std::optional<u64> cascLibFileSizeById(HANDLE hStorage, i32 fileDataId) {
    ScopedCascFile file;
    if (!CascOpenFile(hStorage, CASC_FILE_DATA_ID(fileDataId), 0,
                      CASC_OPEN_BY_FILEID, &file.h))
        return std::nullopt;
    ULONGLONG sz = 0;
    if (!CascGetFileSize64(file.h, &sz))
        return std::nullopt;
    return static_cast<u64>(sz);
}

#endif // WHITEOUT_HAS_CASCLIB_CROSSVAL

// ============================================================================
// Size category helpers
// ============================================================================

enum class SizeCategory { Small, Medium, Large };

static SizeCategory categorize(u64 size) {
    if (size < 4096) return SizeCategory::Small;
    if (size < 262144) return SizeCategory::Medium;
    return SizeCategory::Large;
}

static const char* categoryLabel(SizeCategory c) {
    switch (c) {
        case SizeCategory::Small:  return "small (<4KB)";
        case SizeCategory::Medium: return "medium (4-256KB)";
        case SizeCategory::Large:  return "large (>256KB)";
    }
    return "?";
}

// ============================================================================
// Configuration
// ============================================================================

struct PerfConfig {
    std::string product = "w3";
    std::string region = "us";
    std::string csvPath = "online_perf_crossref.csv";
    std::string cacheDir;
    size_t      maxSample = 200;
    size_t      httpThreads = 8;
    size_t      poolThreads = 0;  // 0 = hardware_concurrency
    size_t      cacheSizeMb = 256;
};

// ============================================================================
// Main benchmark
// ============================================================================

static void runOnlinePerfCrossRef(const PerfConfig& cfg) {
    std::vector<BenchResult> results;

    size_t nPoolThreads = cfg.poolThreads > 0
        ? cfg.poolThreads
        : std::max(2u, std::thread::hardware_concurrency());

    std::cout << "\n================================================================\n";
    std::cout << "Online CASC Performance Cross-Reference Test\n";
    std::cout << "  Product:      " << cfg.product << "\n";
    std::cout << "  Region:       " << cfg.region << "\n";
    std::cout << "  Output:       " << cfg.csvPath << "\n";
    std::cout << "  HTTP threads: " << cfg.httpThreads << "\n";
    std::cout << "  Pool threads: " << nPoolThreads << "\n";
    std::cout << "  Cache size:   " << cfg.cacheSizeMb << " MB\n";
    if (!cfg.cacheDir.empty())
        std::cout << "  Cache dir:    " << cfg.cacheDir << "\n";
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    std::cout << "  CascLib:      ENABLED\n";
#else
    std::cout << "  CascLib:      DISABLED (WhiteoutLib-only benchmarks)\n";
#endif
    std::cout << "================================================================\n";

    // Shared HTTP handler and thread pool for all WhiteoutLib benchmarks.
    utils::SimpleHttpHandler http(cfg.httpThreads);
    utils::SimpleThreadPool pool(nPoolThreads);

    auto makeOpts = [&]() {
        OnlineOpenOptions opts;
        opts.product = cfg.product;
        opts.region = cfg.region;
        opts.http = &http;
        opts.pool = &pool;
        opts.localeMask = LocaleMasks::enUS;
        opts.memoryCacheSize = cfg.cacheSizeMb * 1024 * 1024;
        if (!cfg.cacheDir.empty())
            opts.cacheDir = cfg.cacheDir;
        return opts;
    };

    // ==================================================================
    // 1. Storage Open (cold — no cache)
    // ==================================================================
    std::cout << "\n--- Benchmark 1: OnlineStorage Open (cold) ---\n";

    constexpr int kOpenTrials = 3;
    double woOpenBestUs = 1e18;

    for (int trial = 0; trial < kOpenTrials; ++trial) {
        auto opts = makeOpts();
        // Disable disk cache for cold open
        opts.cacheDir.clear();

        auto t0 = Clock::now();
        auto s = OnlineStorage::open(opts);
        auto t1 = Clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        if (s) woOpenBestUs = std::min(woOpenBestUs, us);
        std::cout << "  [WO] Trial " << trial << ": "
                  << std::fixed << std::setprecision(1) << (us / 1000.0)
                  << " ms" << (s ? "" : " FAIL") << "\n";
    }

    {
        BenchResult r;
        r.operation = "OnlineStorage::open (cold)";
        r.whiteoutUs = woOpenBestUs;
        r.iterations = 1;
        results.push_back(r);
    }

    // ==================================================================
    // 1b. Storage Open (warm — with cache dir, if configured)
    // ==================================================================
    if (!cfg.cacheDir.empty()) {
        std::cout << "\n--- Benchmark 1b: OnlineStorage Open (warm, cached) ---\n";

        double woWarmBestUs = 1e18;
        for (int trial = 0; trial < kOpenTrials; ++trial) {
            auto opts = makeOpts();

            auto t0 = Clock::now();
            auto s = OnlineStorage::open(opts);
            auto t1 = Clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (s) woWarmBestUs = std::min(woWarmBestUs, us);
            std::cout << "  [WO+cache] Trial " << trial << ": "
                      << std::fixed << std::setprecision(1) << (us / 1000.0)
                      << " ms" << (s ? "" : " FAIL") << "\n";
        }

        BenchResult r;
        r.operation = "OnlineStorage::open (warm)";
        r.whiteoutUs = woWarmBestUs;
        r.iterations = 1;
        results.push_back(r);
    }

    // Open persistent handle for the remaining benchmarks.
    auto woStorage = OnlineStorage::open(makeOpts());
    if (!woStorage) {
        std::cerr << "FATAL: could not open OnlineStorage (error "
                  << OnlineStorage::lastError() << ")\n";
        return;
    }

    // Print product info.
    if (auto prod = woStorage->product()) {
        std::cout << "\n  Product: " << prod->name
                  << "  Version: " << prod->version
                  << "  Build: " << prod->buildId << "\n";
    }

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    // CascLib doesn't have a native online/CDN mode — it opens local storages.
    // For cross-reference, we'd need a local CASC corpus of the same product.
    // We skip CascLib benchmarks here; they're covered by casc_perf_crossref_test.
    // This test focuses on OnlineStorage-specific CDN performance.
    std::cout << "\n  NOTE: CascLib comparison is limited for online mode.\n"
              << "  CascLib operates on local storage; direct CDN comparison\n"
              << "  is not apples-to-apples. Use casc_perf_crossref_test for\n"
              << "  local CASC comparison.\n";
#endif

    // ==================================================================
    // 2. Prefetch
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 2: Prefetch ---\n";

        BENCH_TIME_US(woUs, {
            bool ok = woStorage->prefetch();
            std::cout << "  [WO] prefetch: " << (ok ? "OK" : "FAILED") << "\n";
        });
        std::cout << "  [WO] " << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms\n";

        BenchResult r;
        r.operation = "prefetch";
        r.whiteoutUs = woUs;
        r.iterations = 1;
        results.push_back(r);
    }

    // ==================================================================
    // 3. Enumerate All Entries
    // ==================================================================
    std::vector<TestEntry> allEntries;

    {
        std::cout << "\n--- Benchmark 3: Enumerate All Entries ---\n";

        BENCH_TIME_US(woEnumUs, {
            woStorage->enumerate([&](const EnumerateEntry& fe) {
                TestEntry e;
                e.path = std::string(fe.path);
                e.fileDataId = fe.fileDataId;
                e.fileSize = fe.fileSize;
                allEntries.push_back(std::move(e));
                return true;
            });
        });
        std::cout << "  [WO] Enumerated " << allEntries.size() << " entries in "
                  << std::fixed << std::setprecision(1) << (woEnumUs / 1000.0)
                  << " ms\n";

        BenchResult r;
        r.operation = "enumerate";
        r.whiteoutUs = woEnumUs;
        r.iterations = static_cast<u32>(allEntries.size());
        results.push_back(r);
    }

    // Build a representative sample: deterministic stride.
    std::vector<TestEntry> sample;
    {
        size_t stride = std::max<size_t>(1, allEntries.size() / cfg.maxSample);
        for (size_t i = 0; i < allEntries.size() && sample.size() < cfg.maxSample; i += stride)
            sample.push_back(allEntries[i]);
    }
    std::cout << "  Sample size for remaining benchmarks: " << sample.size() << "\n";

    // Separate entries with paths vs FileDataIds.
    std::vector<TestEntry> pathEntries, idEntries;
    for (auto& e : sample) {
        if (!e.path.empty()) pathEntries.push_back(e);
        if (e.fileDataId > 0) idEntries.push_back(e);
    }
    std::cout << "  Entries with paths: " << pathEntries.size()
              << "  with FileDataId: " << idEntries.size() << "\n";

    // ==================================================================
    // 4. File Exists (by path)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 4: File Exists (by path) ---\n";

        u32 woHits = 0;
        BENCH_TIME_US(woUs, {
            for (auto& e : pathEntries) {
                if (woStorage->fileExists(e.path)) ++woHits;
            }
        });
        std::cout << "  [WO] " << woHits << "/" << pathEntries.size()
                  << " exist, " << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms\n";

        BenchResult r;
        r.operation = "fileExists(path)";
        r.whiteoutUs = woUs;
        r.iterations = static_cast<u32>(pathEntries.size());
        results.push_back(r);
    }

    // ==================================================================
    // 5. File Exists (by FileDataId)
    // ==================================================================
    if (!idEntries.empty()) {
        std::cout << "\n--- Benchmark 5: File Exists (by FileDataId) ---\n";

        u32 woHits = 0;
        BENCH_TIME_US(woUs, {
            for (auto& e : idEntries) {
                if (woStorage->fileExists(e.fileDataId)) ++woHits;
            }
        });
        std::cout << "  [WO] " << woHits << "/" << idEntries.size()
                  << " exist, " << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms\n";

        BenchResult r;
        r.operation = "fileExists(fileDataId)";
        r.whiteoutUs = woUs;
        r.iterations = static_cast<u32>(idEntries.size());
        results.push_back(r);
    }

    // ==================================================================
    // 6. File Size (by path)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 6: File Size (by path) ---\n";

        u32 woHits = 0;
        BENCH_TIME_US(woUs, {
            for (auto& e : pathEntries) {
                if (woStorage->fileSize(e.path)) ++woHits;
            }
        });
        std::cout << "  [WO] " << woHits << "/" << pathEntries.size()
                  << " resolved, " << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms\n";

        BenchResult r;
        r.operation = "fileSize(path)";
        r.whiteoutUs = woUs;
        r.iterations = static_cast<u32>(pathEntries.size());
        results.push_back(r);
    }

    // ==================================================================
    // 7. File Size (by FileDataId)
    // ==================================================================
    if (!idEntries.empty()) {
        std::cout << "\n--- Benchmark 7: File Size (by FileDataId) ---\n";

        u32 woHits = 0;
        BENCH_TIME_US(woUs, {
            for (auto& e : idEntries) {
                if (woStorage->fileSize(e.fileDataId)) ++woHits;
            }
        });
        std::cout << "  [WO] " << woHits << "/" << idEntries.size()
                  << " resolved, " << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms\n";

        BenchResult r;
        r.operation = "fileSize(fileDataId)";
        r.whiteoutUs = woUs;
        r.iterations = static_cast<u32>(idEntries.size());
        results.push_back(r);
    }

    // ==================================================================
    // 8. File Info (by path)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 8: File Info (by path) ---\n";

        u32 woHits = 0;
        BENCH_TIME_US(woUs, {
            for (auto& e : pathEntries) {
                if (woStorage->fileInfo(e.path)) ++woHits;
            }
        });
        std::cout << "  [WO] " << woHits << "/" << pathEntries.size()
                  << " resolved, " << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms\n";

        BenchResult r;
        r.operation = "fileInfo(path)";
        r.whiteoutUs = woUs;
        r.iterations = static_cast<u32>(pathEntries.size());
        results.push_back(r);
    }

    // ==================================================================
    // 9. Single File Read (by path) — per-file latency
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 9: Single File Read (by path) ---\n";

        // Warmup: read a few files to prime HTTP connections and memory cache.
        size_t warmupCount = std::min<size_t>(5, pathEntries.size());
        for (size_t i = 0; i < warmupCount; ++i) {
            (void)woStorage->readFile(pathEntries[i].path);
        }

        std::vector<double> woLatencies;
        u64 woBytes = 0;
        u32 woOk = 0;

        for (auto& e : pathEntries) {
            auto t0 = Clock::now();
            auto data = woStorage->readFile(e.path);
            auto t1 = Clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (data) {
                woLatencies.push_back(us);
                woBytes += data->size();
                ++woOk;
            }
        }

        double woTotalUs = 0.0;
        for (auto v : woLatencies) woTotalUs += v;

        std::cout << "  [WO] " << woOk << " reads, "
                  << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                  << " ms total, " << (woBytes / (1024 * 1024)) << " MB\n";

        std::vector<double> emptyLatencies;
        printLatencyComparison("readFile(path)", woLatencies, emptyLatencies);

        BenchResult r;
        r.operation = "readFile(path)";
        r.whiteoutUs = woTotalUs;
        r.iterations = woOk;
        r.bytesTotal = woBytes;
        results.push_back(r);
    }

    // ==================================================================
    // 10. Single File Read (by FileDataId) — per-file latency
    // ==================================================================
    if (!idEntries.empty()) {
        std::cout << "\n--- Benchmark 10: Single File Read (by FileDataId) ---\n";

        // Warmup.
        size_t warmupCount = std::min<size_t>(5, idEntries.size());
        for (size_t i = 0; i < warmupCount; ++i) {
            (void)woStorage->readFile(idEntries[i].fileDataId);
        }

        std::vector<double> woLatencies;
        u64 woBytes = 0;
        u32 woOk = 0;

        for (auto& e : idEntries) {
            auto t0 = Clock::now();
            auto data = woStorage->readFile(e.fileDataId);
            auto t1 = Clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (data) {
                woLatencies.push_back(us);
                woBytes += data->size();
                ++woOk;
            }
        }

        double woTotalUs = 0.0;
        for (auto v : woLatencies) woTotalUs += v;

        std::cout << "  [WO] " << woOk << " reads, "
                  << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                  << " ms total, " << (woBytes / (1024 * 1024)) << " MB\n";

        std::vector<double> emptyLatencies;
        printLatencyComparison("readFile(fileDataId)", woLatencies, emptyLatencies);

        BenchResult r;
        r.operation = "readFile(fileDataId)";
        r.whiteoutUs = woTotalUs;
        r.iterations = woOk;
        r.bytesTotal = woBytes;
        results.push_back(r);
    }

    // ==================================================================
    // 11. Read by Size Category
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 11: Read by Size Category ---\n";

        std::vector<TestEntry> smallEntries, medEntries, largeEntries;
        for (auto& e : pathEntries) {
            switch (categorize(e.fileSize)) {
                case SizeCategory::Small:  smallEntries.push_back(e); break;
                case SizeCategory::Medium: medEntries.push_back(e);   break;
                case SizeCategory::Large:  largeEntries.push_back(e); break;
            }
        }
        // If enumerate didn't provide sizes, do a discovery pass.
        if (smallEntries.empty() && medEntries.empty() && largeEntries.empty()) {
            for (auto& e : pathEntries) {
                auto sz = woStorage->fileSize(e.path);
                if (!sz) continue;
                TestEntry te = e;
                te.fileSize = *sz;
                switch (categorize(te.fileSize)) {
                    case SizeCategory::Small:  smallEntries.push_back(te); break;
                    case SizeCategory::Medium: medEntries.push_back(te);   break;
                    case SizeCategory::Large:  largeEntries.push_back(te); break;
                }
            }
        }

        auto benchCategory = [&](const char* label, std::vector<TestEntry>& entries, SizeCategory cat) {
            if (entries.empty()) {
                std::cout << "  " << label << ": no entries, skipping.\n";
                return;
            }
            // Cap per category to limit test duration over network.
            constexpr size_t kCategoryCap = 100;
            if (entries.size() > kCategoryCap)
                entries.resize(kCategoryCap);

            std::vector<double> woLatencies;
            u64 woBytes = 0;

            for (auto& e : entries) {
                auto t0 = Clock::now();
                auto data = woStorage->readFile(e.path);
                auto t1 = Clock::now();
                if (data) {
                    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                    woLatencies.push_back(us);
                    woBytes += data->size();
                }
            }

            double woTotalUs = 0.0;
            for (auto v : woLatencies) woTotalUs += v;

            std::cout << "  " << label << ":\n";
            std::cout << "    [WO] " << woLatencies.size() << " reads, "
                      << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                      << " ms, " << (woBytes / 1024) << " KB\n";

            std::vector<double> emptyLatencies;
            printLatencyComparison(label, woLatencies, emptyLatencies);

            BenchResult r;
            r.operation = "readFile(path)";
            r.sizeCategory = categoryLabel(cat);
            r.whiteoutUs = woTotalUs;
            r.iterations = static_cast<u32>(woLatencies.size());
            r.bytesTotal = woBytes;
            results.push_back(r);
        };

        benchCategory("Small (<4KB)", smallEntries, SizeCategory::Small);
        benchCategory("Medium (4-256KB)", medEntries, SizeCategory::Medium);
        benchCategory("Large (>256KB)", largeEntries, SizeCategory::Large);
    }

    // ==================================================================
    // 12. Batch Sequential Read — Total Throughput
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 12: Batch Sequential Read (all samples) ---\n";

        u64 woBytes = 0;
        u32 woOk = 0;
        BENCH_TIME_US(woUs, {
            for (auto& e : sample) {
                std::optional<std::vector<u8>> data;
                if (!e.path.empty())
                    data = woStorage->readFile(e.path);
                else if (e.fileDataId > 0)
                    data = woStorage->readFile(e.fileDataId);
                if (data) { woBytes += data->size(); ++woOk; }
            }
        });
        std::cout << "  [WO] " << woOk << "/" << sample.size() << " reads, "
                  << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms, "
                  << (woBytes / (1024 * 1024)) << " MB\n";

        BenchResult r;
        r.operation = "batch_sequential_read";
        r.whiteoutUs = woUs;
        r.iterations = woOk;
        r.bytesTotal = woBytes;
        results.push_back(r);
    }

    // ==================================================================
    // 13. readBatch (parallel)
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 13: readBatch (parallel, "
                  << nPoolThreads << " threads) ---\n";

        std::vector<BatchReadRequest> requests;
        requests.reserve(sample.size());
        for (auto& e : sample) {
            BatchReadRequest req;
            if (!e.path.empty())
                req.path = e.path;
            else if (e.fileDataId > 0)
                req.fileDataId = e.fileDataId;
            requests.push_back(std::move(req));
        }

        u64 woBytes = 0;
        u32 woOk = 0;
        BENCH_TIME_US(woUs, {
            auto batchResults = woStorage->readBatch(requests);
            for (auto& br : batchResults) {
                if (br.success) {
                    woBytes += br.data.size();
                    ++woOk;
                }
            }
        });
        std::cout << "  [WO+batch] " << woOk << "/" << sample.size() << " reads, "
                  << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms, "
                  << (woBytes / (1024 * 1024)) << " MB\n";

        BenchResult r;
        r.operation = "readBatch (parallel)";
        r.whiteoutUs = woUs;
        r.iterations = woOk;
        r.bytesTotal = woBytes;
        results.push_back(r);
    }

    // ==================================================================
    // 14. Repeated Read (same file, hot-path / cache-hit benchmark)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 14: Repeated Read (same file, hot path) ---\n";

        // Find a medium-sized file for this test.
        std::string targetPath;
        for (auto& e : pathEntries) {
            if (e.fileSize >= 4096 && e.fileSize <= 262144) {
                targetPath = e.path;
                break;
            }
        }
        // Fallback: if no medium file found by size metadata, try reading.
        if (targetPath.empty()) {
            for (auto& e : pathEntries) {
                auto data = woStorage->readFile(e.path);
                if (data && data->size() >= 4096 && data->size() <= 262144) {
                    targetPath = e.path;
                    break;
                }
            }
        }

        if (!targetPath.empty()) {
            // Warmup: prime the cache.
            (void)woStorage->readFile(targetPath);

            constexpr int kRepeats = 100;
            u64 woBytes = 0;
            std::vector<double> woLatencies;

            for (int i = 0; i < kRepeats; ++i) {
                auto t0 = Clock::now();
                auto data = woStorage->readFile(targetPath);
                auto t1 = Clock::now();
                double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                if (data) {
                    woLatencies.push_back(us);
                    woBytes += data->size();
                }
            }

            double woTotalUs = 0.0;
            for (auto v : woLatencies) woTotalUs += v;

            std::cout << "  Target: " << targetPath << "\n";
            std::cout << "  [WO] " << woLatencies.size() << "x reads, "
                      << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                      << " ms total\n";

            std::vector<double> emptyLatencies;
            printLatencyComparison("repeated_read (hot path)", woLatencies, emptyLatencies);

            BenchResult r;
            r.operation = "repeated_read (hot)";
            r.whiteoutUs = woTotalUs;
            r.iterations = static_cast<u32>(woLatencies.size());
            r.bytesTotal = woBytes;
            results.push_back(r);
        } else {
            std::cout << "  Skipped: no suitable medium-sized file found.\n";
        }
    }

    // ==================================================================
    // 15. List All File Paths
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 15: List All File Paths ---\n";

        std::vector<std::string> woFiles;
        BENCH_TIME_US(woUs, {
            woFiles = woStorage->listFiles();
        });
        std::cout << "  [WO] " << woFiles.size() << " paths in "
                  << std::fixed << std::setprecision(1) << (woUs / 1000.0)
                  << " ms\n";

        BenchResult r;
        r.operation = "listFiles";
        r.whiteoutUs = woUs;
        r.iterations = static_cast<u32>(woFiles.size());
        results.push_back(r);
    }

    // ==================================================================
    // 16. Total File Count
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 16: Total File Count ---\n";

        std::optional<u32> woCount;
        BENCH_TIME_US(woUs, {
            woCount = woStorage->totalFileCount();
        });
        std::cout << "  [WO] totalFileCount = "
                  << (woCount ? std::to_string(*woCount) : "n/a")
                  << " in " << std::fixed << std::setprecision(1) << woUs << " us\n";

        BenchResult r;
        r.operation = "totalFileCount";
        r.whiteoutUs = woUs;
        r.iterations = 1;
        results.push_back(r);
    }

    // ==================================================================
    // 17. Cache Flush
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 17: Cache Flush ---\n";

        BENCH_TIME_US(woUs, {
            woStorage->flushCache();
        });
        std::cout << "  [WO] flushCache in "
                  << std::fixed << std::setprecision(1) << (woUs / 1000.0)
                  << " ms\n";

        BenchResult r;
        r.operation = "flushCache";
        r.whiteoutUs = woUs;
        r.iterations = 1;
        results.push_back(r);
    }

    // ==================================================================
    // 18. Cold Read vs Warm Read (cache effectiveness)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 18: Cold vs Warm Read (cache effectiveness) ---\n";

        // Pick up to 20 files for this test.
        size_t nCacheTest = std::min<size_t>(20, pathEntries.size());

        // Cold pass: flush cache, then read.
        woStorage->flushCache();

        std::vector<double> coldLatencies;
        u64 coldBytes = 0;
        for (size_t i = 0; i < nCacheTest; ++i) {
            auto t0 = Clock::now();
            auto data = woStorage->readFile(pathEntries[i].path);
            auto t1 = Clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (data) {
                coldLatencies.push_back(us);
                coldBytes += data->size();
            }
        }

        // Warm pass: same files, now in memory cache.
        std::vector<double> warmLatencies;
        u64 warmBytes = 0;
        for (size_t i = 0; i < nCacheTest; ++i) {
            auto t0 = Clock::now();
            auto data = woStorage->readFile(pathEntries[i].path);
            auto t1 = Clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (data) {
                warmLatencies.push_back(us);
                warmBytes += data->size();
            }
        }

        double coldTotal = 0.0, warmTotal = 0.0;
        for (auto v : coldLatencies) coldTotal += v;
        for (auto v : warmLatencies) warmTotal += v;

        std::cout << "  Cold: " << coldLatencies.size() << " reads, "
                  << std::fixed << std::setprecision(1) << (coldTotal / 1000.0)
                  << " ms, " << (coldBytes / 1024) << " KB\n";
        std::cout << "  Warm: " << warmLatencies.size() << " reads, "
                  << std::fixed << std::setprecision(1) << (warmTotal / 1000.0)
                  << " ms, " << (warmBytes / 1024) << " KB\n";
        if (warmTotal > 0.0 && coldTotal > 0.0) {
            std::cout << "  Cache speedup: " << std::setprecision(2)
                      << (coldTotal / warmTotal) << "x\n";
        }

        printLatencyComparison("cold read", coldLatencies, warmLatencies);

        {
            BenchResult r;
            r.operation = "readFile (cold)";
            r.whiteoutUs = coldTotal;
            r.iterations = static_cast<u32>(coldLatencies.size());
            r.bytesTotal = coldBytes;
            results.push_back(r);
        }
        {
            BenchResult r;
            r.operation = "readFile (warm/cached)";
            r.whiteoutUs = warmTotal;
            r.iterations = static_cast<u32>(warmLatencies.size());
            r.bytesTotal = warmBytes;
            results.push_back(r);
        }
    }

    // ==================================================================
    // 19. HTTP Thread Sweep (readBatch throughput vs thread count)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n================================================================\n";
        std::cout << "Benchmark 19: HTTP Thread Sweep — readBatch Throughput\n";
        std::cout << "================================================================\n";

        // Collect large files for max-bandwidth test (>64KB preferred).
        std::vector<TestEntry> largeForSweep;
        for (auto& e : allEntries) {
            if (e.fileSize >= 65536 && !e.path.empty()) {
                largeForSweep.push_back(e);
                if (largeForSweep.size() >= 200) break;
            }
        }
        // Fallback: use whatever we have.
        if (largeForSweep.size() < 20) {
            largeForSweep.clear();
            for (auto& e : pathEntries) {
                largeForSweep.push_back(e);
                if (largeForSweep.size() >= 100) break;
            }
        }

        std::vector<BatchReadRequest> sweepRequests;
        for (auto& e : largeForSweep) {
            BatchReadRequest req;
            req.path = e.path;
            sweepRequests.push_back(std::move(req));
        }

        std::cout << "  Files in sweep: " << sweepRequests.size() << "\n\n";

        const std::array<size_t, 5> httpThreadCounts = {2, 4, 8, 16, 32};

        std::cout << std::left << std::setw(16) << "HTTP Threads"
                  << std::right << std::setw(10) << "Files"
                  << std::setw(12) << "Time(ms)"
                  << std::setw(12) << "MB read"
                  << std::setw(14) << "Throughput"
                  << "\n";
        std::cout << std::string(64, '-') << "\n";

        for (auto nHttp : httpThreadCounts) {
            // Open a fresh storage with this HTTP thread count.
            utils::SimpleHttpHandler sweepHttp(nHttp);
            utils::SimpleThreadPool sweepPool(nPoolThreads);

            OnlineOpenOptions sweepOpts;
            sweepOpts.product = cfg.product;
            sweepOpts.region = cfg.region;
            sweepOpts.http = &sweepHttp;
            sweepOpts.pool = &sweepPool;
            sweepOpts.localeMask = LocaleMasks::enUS;
            sweepOpts.memoryCacheSize = cfg.cacheSizeMb * 1024 * 1024;
            if (!cfg.cacheDir.empty())
                sweepOpts.cacheDir = cfg.cacheDir;

            auto sweepStorage = OnlineStorage::open(sweepOpts);
            if (!sweepStorage) {
                std::cout << std::left << std::setw(16) << nHttp
                          << "  OPEN FAILED\n";
                continue;
            }

            // Prefetch to ensure encoding/root are loaded.
            sweepStorage->prefetch();

            u64 sweepBytes = 0;
            u32 sweepOk = 0;

            BENCH_TIME_US(sweepUs, {
                auto batchRes = sweepStorage->readBatch(sweepRequests);
                for (auto& br : batchRes) {
                    if (br.success) {
                        sweepBytes += br.data.size();
                        ++sweepOk;
                    }
                }
            });

            double mb = static_cast<double>(sweepBytes) / (1024.0 * 1024.0);
            double secs = sweepUs / 1e6;
            double mbps = (secs > 0.0) ? mb / secs : 0.0;

            std::cout << std::left << std::setw(16) << nHttp
                      << std::right << std::setw(10) << sweepOk
                      << std::fixed << std::setprecision(0)
                      << std::setw(12) << (sweepUs / 1000.0)
                      << std::setprecision(1)
                      << std::setw(12) << mb
                      << std::setprecision(2)
                      << std::setw(12) << mbps << " MB/s"
                      << "\n";

            BenchResult r;
            r.operation = "readBatch (http=" + std::to_string(nHttp) + "t)";
            r.whiteoutUs = sweepUs;
            r.iterations = sweepOk;
            r.bytesTotal = sweepBytes;
            results.push_back(r);
        }
        std::cout << "\n";
    }

    // ==================================================================
    // Summary
    // ==================================================================
    std::cout << "\n================================================================\n";
    std::cout << "RESULTS SUMMARY\n";
    std::cout << "================================================================\n";
    printResultsTable(results);

    // Write CSV
    if (!cfg.csvPath.empty()) {
        writeResultsCsv(cfg.csvPath, results);
    }
}

// ============================================================================
// Entry point
// ============================================================================

int main(int argc, char* argv[]) {
    PerfConfig cfg;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " <product> [options]\n\n"
                      << "  product — Blizzard product code (w3, s1, wow, hero, etc.)\n\n"
                      << "Options:\n"
                      << "  --region <r>        CDN region (default: us)\n"
                      << "  --csv <path>        CSV output path (default: online_perf_crossref.csv)\n"
                      << "  --sample <n>        Max sample size for per-file benchmarks (default: 200)\n"
                      << "  --http-threads <n>  Number of HTTP worker threads (default: 4)\n"
                      << "  --pool-threads <n>  Number of WorkerPool threads (default: hardware_concurrency)\n"
                      << "  --cache-dir <d>     Local cache directory for OnlineStorage\n"
                      << "  --cache-size <mb>   In-memory cache size in MB (default: 256)\n"
                      << "\n"
                      << "Operations benchmarked:\n"
                      << "  Storage open (cold/warm), prefetch, enumerate, fileExists,\n"
                      << "  fileSize, fileInfo, readFile (path/id), batch read,\n"
                      << "  readBatch, size-category read, repeated read, listFiles,\n"
                      << "  totalFileCount, flushCache, cold-vs-warm cache comparison\n";
            return 0;
        }
        if (a == "--region" && i + 1 < argc) { cfg.region = argv[++i]; continue; }
        if (a == "--csv" && i + 1 < argc) { cfg.csvPath = argv[++i]; continue; }
        if (a == "--sample" && i + 1 < argc) { cfg.maxSample = static_cast<size_t>(std::atoi(argv[++i])); continue; }
        if (a == "--http-threads" && i + 1 < argc) { cfg.httpThreads = static_cast<size_t>(std::atoi(argv[++i])); continue; }
        if (a == "--pool-threads" && i + 1 < argc) { cfg.poolThreads = static_cast<size_t>(std::atoi(argv[++i])); continue; }
        if (a == "--cache-dir" && i + 1 < argc) { cfg.cacheDir = argv[++i]; continue; }
        if (a == "--cache-size" && i + 1 < argc) { cfg.cacheSizeMb = static_cast<size_t>(std::atoi(argv[++i])); continue; }
        if (a.size() >= 2 && a[0] == '-' && a[1] == '-') continue;
        if (a.size() >= 1 && a[0] == '-') continue;
        positional.push_back(a);
    }

    if (positional.empty()) {
        std::cerr << "Error: no product code provided.\n"
                  << "Usage: " << argv[0] << " <product> [options]\n"
                  << "  e.g.: " << argv[0] << " w3\n"
                  << "  e.g.: " << argv[0] << " wow --region eu --sample 500\n";
        return 1;
    }

    cfg.product = positional[0];

    runOnlinePerfCrossRef(cfg);
    return 0;
}

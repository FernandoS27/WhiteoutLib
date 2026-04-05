// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// CASC performance cross-reference test.
///
/// Compares the pure-C++ WhiteoutLib CASC implementation against CascLib's
/// C API for every measurable operation, reporting per-operation timings and
/// speedup ratios.
///
/// Build with -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON to enable the CascLib
/// comparison path.  Without it, only WhiteoutLib benchmarks are collected.
///
/// Usage:
///   casc_perf_crossref_test.exe <casc_path> [output_csv]
///
///   casc_path  — path to a CASC storage (directory containing .build.info)
///   output_csv — optional path for CSV results (default: casc_perf_crossref.csv)
///
/// Operations benchmarked:
///   1. Storage open
///   2. Enumerate (list all entries)
///   3. File-exists lookup (by path)
///   4. File-exists lookup (by FileDataId)
///   5. File-size query (by path)
///   6. File-size query (by FileDataId)
///   7. File-info query (by path)
///   8. Single file read (by path) — sequential, representative sample
///   9. Single file read (by FileDataId) — sequential
///  10. Batch sequential read — total throughput for N files
///  11. Read by size category — small (<4KB), medium (4KB-256KB), large (>256KB)
///  12. List all file paths

#include <whiteout/storages/casc/storage.h>
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
    u32    iterations  = 0;       // how many times WhiteoutLib operation succeeded
    u32    clIterations = 0;      // how many times CascLib operation succeeded (0 = same as iterations)
    u64    bytesTotal  = 0;       // total bytes processed by WhiteoutLib
    u64    clBytesTotal = 0;      // total bytes processed by CascLib (0 = same as bytesTotal)
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
    std::cout << std::left << std::setw(36) << "Operation"
              << std::right << std::setw(8) << "Iters"
              << std::setw(14) << "Whiteout(us)"
              << std::setw(14) << "CascLib(us)"
              << std::setw(12) << "Speedup"
              << std::setw(14) << "WO MB/s"
              << std::setw(14) << "CL MB/s"
              << "\n";
    std::cout << std::string(112, '-') << "\n";

    for (auto& r : results) {
        std::string label = r.operation;
        if (!r.sizeCategory.empty())
            label += " [" + r.sizeCategory + "]";

        double woPerIter = (r.iterations > 0)
            ? r.whiteoutUs / static_cast<double>(r.iterations)
            : r.whiteoutUs;
        u32 clIters = (r.clIterations > 0) ? r.clIterations : r.iterations;
        double clPerIter = (clIters > 0 && r.hasCascLib)
            ? r.cascLibUs / static_cast<double>(clIters)
            : 0.0;

        double speedup = (r.hasCascLib && clPerIter > 0.0 && woPerIter > 0.0)
            ? clPerIter / woPerIter
            : 0.0;

        // Throughput in MB/s (each side uses its own byte count)
        u64 clBytes = (r.clBytesTotal > 0) ? r.clBytesTotal : r.bytesTotal;
        double woThroughput = 0.0, clThroughput = 0.0;
        if (r.bytesTotal > 0 && r.whiteoutUs > 0.0)
            woThroughput = (static_cast<double>(r.bytesTotal) / (1024.0 * 1024.0))
                         / (r.whiteoutUs / 1e6);
        if (clBytes > 0 && r.hasCascLib && r.cascLibUs > 0.0)
            clThroughput = (static_cast<double>(clBytes) / (1024.0 * 1024.0))
                         / (r.cascLibUs / 1e6);

        std::cout << std::left << std::setw(36) << label
                  << std::right << std::setw(8) << r.iterations
                  << std::fixed << std::setprecision(1)
                  << std::setw(14) << woPerIter;

        if (r.hasCascLib)
            std::cout << std::setw(14) << clPerIter;
        else
            std::cout << std::setw(14) << "n/a";

        if (speedup > 0.0) {
            std::cout << std::setw(11) << std::setprecision(2) << speedup << "x";
            // Visual indicator
            if (speedup >= 1.0)
                std::cout << " ";  // we're faster or equal — good
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
    f << "operation,sizeCategory,woIterations,clIterations,whiteoutTotalUs,cascLibTotalUs,"
         "whiteoutPerIterUs,cascLibPerIterUs,speedup,woBytesTotal,clBytesTotal,"
         "whiteoutMBs,cascLibMBs\n";

    for (auto& r : results) {
        double woPerIter = (r.iterations > 0)
            ? r.whiteoutUs / static_cast<double>(r.iterations)
            : r.whiteoutUs;
        u32 clIters = (r.clIterations > 0) ? r.clIterations : r.iterations;
        double clPerIter = (clIters > 0 && r.hasCascLib)
            ? r.cascLibUs / static_cast<double>(clIters)
            : 0.0;
        double speedup = (r.hasCascLib && clPerIter > 0.0 && woPerIter > 0.0)
            ? clPerIter / woPerIter : 0.0;
        u64 clBytes = (r.clBytesTotal > 0) ? r.clBytesTotal : r.bytesTotal;
        double woMBs = 0.0, clMBs = 0.0;
        if (r.bytesTotal > 0 && r.whiteoutUs > 0.0)
            woMBs = (static_cast<double>(r.bytesTotal) / (1024.0 * 1024.0))
                  / (r.whiteoutUs / 1e6);
        if (clBytes > 0 && r.hasCascLib && r.cascLibUs > 0.0)
            clMBs = (static_cast<double>(clBytes) / (1024.0 * 1024.0))
                  / (r.cascLibUs / 1e6);

        f << r.operation << ","
          << r.sizeCategory << ","
          << r.iterations << ","
          << clIters << ","
          << std::fixed << std::setprecision(1)
          << r.whiteoutUs << ","
          << (r.hasCascLib ? r.cascLibUs : 0.0) << ","
          << woPerIter << ","
          << clPerIter << ","
          << std::setprecision(3) << speedup << ","
          << r.bytesTotal << ","
          << clBytes << ","
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
// CascLib benchmark helpers
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

    if (fileSize64 > static_cast<ULONGLONG>(1u << 30))  // skip >1GB
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
// Main benchmark
// ============================================================================

static void runPerfCrossRef(const std::string& cascPath,
                            const std::string& csvPath) {
    std::vector<BenchResult> results;

    std::cout << "\n================================================================\n";
    std::cout << "CASC Performance Cross-Reference Test\n";
    std::cout << "  Storage:   " << cascPath << "\n";
    std::cout << "  Output:    " << csvPath << "\n";
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    std::cout << "  CascLib:   ENABLED\n";
#else
    std::cout << "  CascLib:   DISABLED (WhiteoutLib-only benchmarks)\n";
#endif
    std::cout << "================================================================\n";

    // ==================================================================
    // 1. Storage Open (single-threaded)
    // ==================================================================
    std::cout << "\n--- Benchmark 1a: Storage Open (no WorkerPool) ---\n";

    constexpr int kOpenTrials = 3;
    double woOpenBestUs = 1e18;
    double clOpenBestUs = 1e18;

    for (int trial = 0; trial < kOpenTrials; ++trial) {
        {
            auto t0 = Clock::now();
            auto s = Storage::open(cascPath);
            auto t1 = Clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (s) woOpenBestUs = std::min(woOpenBestUs, us);
            std::cout << "  [WO] Trial " << trial << ": "
                      << std::fixed << std::setprecision(1) << (us / 1000.0)
                      << " ms" << (s ? "" : " FAIL") << "\n";
        }

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        {
            ScopedCascStorage cs;
            auto t0 = Clock::now();
            bool ok = CascOpenStorage(cascPath.c_str(), CASC_LOCALE_ALL, &cs.h);
            auto t1 = Clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (ok) clOpenBestUs = std::min(clOpenBestUs, us);
            std::cout << "  [CL] Trial " << trial << ": "
                      << std::fixed << std::setprecision(1) << (us / 1000.0)
                      << " ms" << (ok ? "" : " FAIL") << "\n";
        }
#endif
    }

    {
        BenchResult r;
        r.operation = "Storage::open";
        r.whiteoutUs = woOpenBestUs;
        r.iterations = 1;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        r.cascLibUs = clOpenBestUs;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 1b. Storage Open (with WorkerPool)
    // ==================================================================
    {
        unsigned nThreads = std::max(2u, std::thread::hardware_concurrency());
        std::cout << "\n--- Benchmark 1b: Storage Open (WorkerPool, "
                  << nThreads << " threads) ---\n";

        double woPoolBestUs = 1e18;

        for (int trial = 0; trial < kOpenTrials; ++trial) {
            auto pool = std::make_unique<utils::SimpleThreadPool>(nThreads);
            auto t0 = Clock::now();
            auto s = Storage::open(cascPath, pool.get());
            auto t1 = Clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (s) woPoolBestUs = std::min(woPoolBestUs, us);
            std::cout << "  [WO+pool] Trial " << trial << ": "
                      << std::fixed << std::setprecision(1) << (us / 1000.0)
                      << " ms" << (s ? "" : " FAIL") << "\n";
        }

        BenchResult r;
        r.operation = "Storage::open (pool)";
        r.whiteoutUs = woPoolBestUs;
        r.iterations = 1;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        r.cascLibUs = clOpenBestUs;  // same CascLib baseline
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // Open persistent handles for the remaining benchmarks.
    auto woStorage = Storage::open(cascPath);
    if (!woStorage) {
        std::cerr << "FATAL: could not open WhiteoutLib storage (error "
                  << Storage::lastError() << ")\n";
        return;
    }

    // Pooled storage for parallel benchmarks.
    unsigned nPoolThreads = std::max(2u, std::thread::hardware_concurrency());
    auto threadPool = std::make_unique<utils::SimpleThreadPool>(nPoolThreads);
    auto woPoolStorage = Storage::open(cascPath, threadPool.get());
    if (!woPoolStorage) {
        std::cerr << "WARNING: could not open pooled WhiteoutLib storage, "
                     "pool benchmarks will be skipped.\n";
    }

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    ScopedCascStorage clStorage;
    if (!CascOpenStorage(cascPath.c_str(), CASC_LOCALE_ALL, &clStorage.h)) {
        std::cerr << "FATAL: could not open CascLib storage\n";
        return;
    }
#endif

    // ==================================================================
    // 2. Enumerate All Entries
    // ==================================================================
    std::cout << "\n--- Benchmark 2: Enumerate All Entries ---\n";

    std::vector<TestEntry> allEntries;

    {
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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        u32 clCount = 0;
        BENCH_TIME_US(clEnumUs, {
            CASC_FIND_DATA fd{};
            ScopedCascFind hFind;
            hFind.h = CascFindFirstFile(clStorage.h, "*", &fd, nullptr);
            if (hFind) {
                do { ++clCount; } while (CascFindNextFile(hFind.h, &fd));
            }
        });
        std::cout << "  [CL] Enumerated " << clCount << " entries in "
                  << std::fixed << std::setprecision(1) << (clEnumUs / 1000.0)
                  << " ms\n";
        r.cascLibUs = clEnumUs;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 2b. Enumerate with Wildcard Mask
    // ==================================================================
    {
        // Determine representative wildcard patterns based on extensions present.
        // We always test: a broad extension mask, a narrow directory prefix mask,
        // and a pattern that matches nothing.
        struct WildcardTest {
            const char* mask;
            const char* label;
        };

        // Collect a few common extensions from the enumerated entries.
        std::string anyExt;
        {
            std::unordered_map<std::string, u32> extCounts;
            for (auto& e : allEntries) {
                auto dot = e.path.rfind('.');
                if (dot != std::string::npos && dot + 1 < e.path.size()) {
                    std::string ext = e.path.substr(dot); // includes '.'
                    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    ++extCounts[ext];
                }
            }
            // Pick the most common extension (for a representative broad filter).
            u32 best = 0;
            for (auto& [ext, cnt] : extCounts) {
                if (cnt > best) { best = cnt; anyExt = ext; }
            }
        }

        std::string extMask  = anyExt.empty() ? std::string("*.dds") : ("*" + anyExt);

        // Find a directory prefix that exists (first path component of the first entry with a '/').
        std::string prefixMask;
        for (auto& e : allEntries) {
            auto sep = e.path.find_first_of("\\/");
            if (sep != std::string::npos && sep > 0) {
                prefixMask = e.path.substr(0, sep) + "\\*";
                break;
            }
        }
        if (prefixMask.empty()) prefixMask = "data\\*"; // fallback

        std::vector<WildcardTest> wildcardTests = {
            { extMask.c_str(),     "extension"      },
            { prefixMask.c_str(),  "directory prefix" },
            { "*.ZZZNONEXIST",     "no match"       },
        };

        for (auto& wt : wildcardTests) {
            std::cout << "\n--- Benchmark 2b: Enumerate Wildcard (" << wt.label
                      << ": \"" << wt.mask << "\") ---\n";

            u32 woCount = 0;
            BENCH_TIME_US(woUs, {
                woStorage->enumerate(wt.mask, [&](const EnumerateEntry&) {
                    ++woCount;
                    return true;
                });
            });
            std::cout << "  [WO] " << woCount << " matches in "
                      << std::fixed << std::setprecision(1) << (woUs / 1000.0) << " ms\n";

            BenchResult r;
            r.operation = std::string("enumerate(\"") + wt.mask + "\")";
            r.whiteoutUs = woUs;
            r.iterations = woCount;

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            u32 clCount = 0;
            BENCH_TIME_US(clUs, {
                CASC_FIND_DATA fd{};
                ScopedCascFind hFind;
                hFind.h = CascFindFirstFile(clStorage.h, wt.mask, &fd, nullptr);
                if (hFind) {
                    do { ++clCount; } while (CascFindNextFile(hFind.h, &fd));
                }
            });
            std::cout << "  [CL] " << clCount << " matches in "
                      << std::fixed << std::setprecision(1) << (clUs / 1000.0) << " ms\n";
            r.cascLibUs = clUs;
            r.hasCascLib = true;
#endif
            results.push_back(r);
        }
    }

    // Build a representative sample: deterministic stride.
    constexpr size_t kMaxSample = 2000;
    std::vector<TestEntry> sample;
    {
        size_t stride = std::max<size_t>(1, allEntries.size() / kMaxSample);
        for (size_t i = 0; i < allEntries.size() && sample.size() < kMaxSample; i += stride)
            sample.push_back(allEntries[i]);
    }
    std::cout << "  Sample size for remaining benchmarks: " << sample.size() << "\n";

    // Separate entries with paths vs FileDataIds for targeted benchmarks.
    std::vector<TestEntry> pathEntries, idEntries;
    for (auto& e : sample) {
        if (!e.path.empty()) pathEntries.push_back(e);
        if (e.fileDataId > 0) idEntries.push_back(e);
    }
    std::cout << "  Entries with paths: " << pathEntries.size()
              << "  with FileDataId: " << idEntries.size() << "\n";

    // ==================================================================
    // 3. File Exists (by path)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 3: File Exists (by path) ---\n";

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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        u32 clHits = 0;
        BENCH_TIME_US(clUs, {
            for (auto& e : pathEntries) {
                if (cascLibFileExists(clStorage.h, e.path)) ++clHits;
            }
        });
        std::cout << "  [CL] " << clHits << "/" << pathEntries.size()
                  << " exist, " << std::fixed << std::setprecision(1)
                  << (clUs / 1000.0) << " ms\n";
        r.cascLibUs = clUs;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 4. File Exists (by FileDataId)
    // ==================================================================
    if (!idEntries.empty()) {
        std::cout << "\n--- Benchmark 4: File Exists (by FileDataId) ---\n";

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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        u32 clHits = 0;
        BENCH_TIME_US(clUs, {
            for (auto& e : idEntries) {
                if (cascLibFileExistsById(clStorage.h, e.fileDataId)) ++clHits;
            }
        });
        std::cout << "  [CL] " << clHits << "/" << idEntries.size()
                  << " exist, " << std::fixed << std::setprecision(1)
                  << (clUs / 1000.0) << " ms";
        if (clHits == 0) {
            std::cout << " (not supported by CascLib for this root — skipping comparison)";
        } else {
            r.cascLibUs = clUs;
            r.hasCascLib = true;
        }
        std::cout << "\n";
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 5. File Size (by path)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 5: File Size (by path) ---\n";

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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        u32 clHits = 0;
        BENCH_TIME_US(clUs, {
            for (auto& e : pathEntries) {
                if (cascLibFileSize(clStorage.h, e.path)) ++clHits;
            }
        });
        std::cout << "  [CL] " << clHits << "/" << pathEntries.size()
                  << " resolved, " << std::fixed << std::setprecision(1)
                  << (clUs / 1000.0) << " ms\n";
        r.cascLibUs = clUs;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 6. File Size (by FileDataId)
    // ==================================================================
    if (!idEntries.empty()) {
        std::cout << "\n--- Benchmark 6: File Size (by FileDataId) ---\n";

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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        u32 clHits = 0;
        BENCH_TIME_US(clUs, {
            for (auto& e : idEntries) {
                if (cascLibFileSizeById(clStorage.h, e.fileDataId)) ++clHits;
            }
        });
        std::cout << "  [CL] " << clHits << "/" << idEntries.size()
                  << " resolved, " << std::fixed << std::setprecision(1)
                  << (clUs / 1000.0) << " ms";
        if (clHits == 0) {
            std::cout << " (not supported by CascLib for this root — skipping comparison)";
        } else {
            r.cascLibUs = clUs;
            r.hasCascLib = true;
        }
        std::cout << "\n";
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 7. File Info (by path) — WhiteoutLib only (no CascLib equivalent)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 7: File Info (by path) ---\n";

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
        // CascLib doesn't have a direct fileInfo equivalent returning all metadata
        // at once without an open+query sequence, so we skip it here.
        results.push_back(r);
    }

    // ==================================================================
    // 8. Single File Read (by path) — per-file latency comparison
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 8: Single File Read (by path) ---\n";

        // Warmup pass: read all sample files once (untimed) so the OS page
        // cache is populated.  Without this, whichever API reads a file first
        // pays the cold-disk penalty while the second reader gets warm pages.
        for (auto& e : pathEntries) {
            (void)woStorage->readFile(e.path);
        }

        std::vector<double> woLatencies, clLatencies;
        u64 woBytes = 0, clBytes = 0;
        u32 woOk = 0, clOk = 0;

        for (auto& e : pathEntries) {
            {
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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            {
                auto t0 = Clock::now();
                auto data = cascLibReadFile(clStorage.h, e.path);
                auto t1 = Clock::now();
                double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                if (data) {
                    clLatencies.push_back(us);
                    clBytes += data->size();
                    ++clOk;
                }
            }
#endif
        }

        double woTotalUs = 0.0, clTotalUs = 0.0;
        for (auto v : woLatencies) woTotalUs += v;
        for (auto v : clLatencies) clTotalUs += v;

        std::cout << "  [WO] " << woOk << " reads, "
                  << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                  << " ms total, " << (woBytes / (1024 * 1024)) << " MB\n";
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        std::cout << "  [CL] " << clOk << " reads, "
                  << std::fixed << std::setprecision(1) << (clTotalUs / 1000.0)
                  << " ms total, " << (clBytes / (1024 * 1024)) << " MB\n";
#endif

        printLatencyComparison("readFile(path)", woLatencies, clLatencies);

        BenchResult r;
        r.operation = "readFile(path)";
        r.whiteoutUs = woTotalUs;
        r.iterations = woOk;
        r.bytesTotal = woBytes;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        r.cascLibUs = clTotalUs;
        r.clIterations = clOk;
        r.clBytesTotal = clBytes;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 9. Single File Read (by FileDataId) — per-file latency comparison
    // ==================================================================
    if (!idEntries.empty()) {
        std::cout << "\n--- Benchmark 9: Single File Read (by FileDataId) ---\n";

        // Warmup: populate OS page cache.
        for (auto& e : idEntries) {
            (void)woStorage->readFile(e.fileDataId);
        }

        std::vector<double> woLatencies, clLatencies;
        u64 woBytes = 0, clBytes = 0;
        u32 woOk = 0, clOk = 0;

        for (auto& e : idEntries) {
            {
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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            {
                auto t0 = Clock::now();
                auto data = cascLibReadFileById(clStorage.h, e.fileDataId);
                auto t1 = Clock::now();
                double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                if (data) {
                    clLatencies.push_back(us);
                    clBytes += data->size();
                    ++clOk;
                }
            }
#endif
        }

        double woTotalUs = 0.0, clTotalUs = 0.0;
        for (auto v : woLatencies) woTotalUs += v;
        for (auto v : clLatencies) clTotalUs += v;

        std::cout << "  [WO] " << woOk << " reads, "
                  << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                  << " ms total, " << (woBytes / (1024 * 1024)) << " MB\n";
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        std::cout << "  [CL] " << clOk << " reads, "
                  << std::fixed << std::setprecision(1) << (clTotalUs / 1000.0)
                  << " ms total, " << (clBytes / (1024 * 1024)) << " MB\n";
#endif

        printLatencyComparison("readFile(fileDataId)", woLatencies, clLatencies);

        BenchResult r;
        r.operation = "readFile(fileDataId)";
        r.whiteoutUs = woTotalUs;
        r.iterations = woOk;
        r.bytesTotal = woBytes;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        r.cascLibUs = clTotalUs;
        r.clIterations = clOk;
        r.clBytesTotal = clBytes;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 10. Read by Size Category
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 10: Read by Size Category ---\n";

        // Categorize entries that have known sizes.
        std::vector<TestEntry> smallEntries, medEntries, largeEntries;
        for (auto& e : pathEntries) {
            switch (categorize(e.fileSize)) {
                case SizeCategory::Small:  smallEntries.push_back(e); break;
                case SizeCategory::Medium: medEntries.push_back(e);   break;
                case SizeCategory::Large:  largeEntries.push_back(e); break;
            }
        }
        // If enumerate didn't provide sizes, do a discovery read.
        if (smallEntries.empty() && medEntries.empty() && largeEntries.empty()) {
            for (auto& e : pathEntries) {
                auto data = woStorage->readFile(e.path);
                if (!data) continue;
                TestEntry te = e;
                te.fileSize = data->size();
                switch (categorize(te.fileSize)) {
                    case SizeCategory::Small:  if (smallEntries.size() < 300) smallEntries.push_back(te); break;
                    case SizeCategory::Medium: if (medEntries.size() < 300)   medEntries.push_back(te);   break;
                    case SizeCategory::Large:  if (largeEntries.size() < 300) largeEntries.push_back(te); break;
                }
                if (smallEntries.size() >= 300 && medEntries.size() >= 300 && largeEntries.size() >= 300)
                    break;
            }
        }

        auto benchCategory = [&](const char* label, std::vector<TestEntry>& entries, SizeCategory cat) {
            if (entries.empty()) {
                std::cout << "  " << label << ": no entries, skipping.\n";
                return;
            }
            // Cap per category.
            constexpr size_t kCategoryCap = 500;
            if (entries.size() > kCategoryCap)
                entries.resize(kCategoryCap);

            std::vector<double> woLatencies, clLatencies;
            u64 woBytes = 0, clBytes = 0;

            for (auto& e : entries) {
                {
                    auto t0 = Clock::now();
                    auto data = woStorage->readFile(e.path);
                    auto t1 = Clock::now();
                    if (data) {
                        woLatencies.push_back(
                            std::chrono::duration<double, std::micro>(t1 - t0).count());
                        woBytes += data->size();
                    }
                }
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
                {
                    auto t0 = Clock::now();
                    auto data = cascLibReadFile(clStorage.h, e.path);
                    auto t1 = Clock::now();
                    if (data) {
                        clLatencies.push_back(
                            std::chrono::duration<double, std::micro>(t1 - t0).count());
                        clBytes += data->size();
                    }
                }
#endif
            }

            double woTotalUs = 0.0, clTotalUs = 0.0;
            for (auto v : woLatencies) woTotalUs += v;
            for (auto v : clLatencies) clTotalUs += v;

            std::cout << "  " << label << ":\n";
            std::cout << "    [WO] " << woLatencies.size() << " reads, "
                      << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                      << " ms, " << (woBytes / 1024) << " KB\n";
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            std::cout << "    [CL] " << clLatencies.size() << " reads, "
                      << std::fixed << std::setprecision(1) << (clTotalUs / 1000.0)
                      << " ms, " << (clBytes / 1024) << " KB\n";
#endif
            printLatencyComparison(label, woLatencies, clLatencies);

            BenchResult r;
            r.operation = "readFile(path)";
            r.sizeCategory = categoryLabel(cat);
            r.whiteoutUs = woTotalUs;
            r.iterations = static_cast<u32>(woLatencies.size());
            r.bytesTotal = woBytes;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            r.cascLibUs = clTotalUs;
            r.clIterations = static_cast<u32>(clLatencies.size());
            r.clBytesTotal = clBytes;
            r.hasCascLib = true;
#endif
            results.push_back(r);
        };

        benchCategory("Small (<4KB)", smallEntries, SizeCategory::Small);
        benchCategory("Medium (4-256KB)", medEntries, SizeCategory::Medium);
        benchCategory("Large (>256KB)", largeEntries, SizeCategory::Large);
    }

    // ==================================================================
    // 11. List All File Paths
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 11: List All File Paths ---\n";

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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        // CascLib equivalent: enumerate and collect paths.
        std::vector<std::string> clFiles;
        BENCH_TIME_US(clUs, {
            CASC_FIND_DATA fd{};
            ScopedCascFind hFind;
            hFind.h = CascFindFirstFile(clStorage.h, "*", &fd, nullptr);
            if (hFind) {
                do {
                    if (fd.szFileName[0] != '\0')
                        clFiles.emplace_back(fd.szFileName);
                } while (CascFindNextFile(hFind.h, &fd));
            }
        });
        std::cout << "  [CL] " << clFiles.size() << " paths in "
                  << std::fixed << std::setprecision(1) << (clUs / 1000.0)
                  << " ms\n";
        r.cascLibUs = clUs;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 12. Batch Sequential Read — Total Throughput
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 12: Batch Sequential Read (all samples) ---\n";

        // Warmup: populate OS page cache.
        for (auto& e : sample) {
            if (!e.path.empty())
                (void)woStorage->readFile(e.path);
            else if (e.fileDataId > 0)
                (void)woStorage->readFile(e.fileDataId);
        }

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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        u64 clBytes = 0;
        u32 clOk = 0;
        BENCH_TIME_US(clUs, {
            for (auto& e : sample) {
                std::optional<std::vector<u8>> data;
                if (!e.path.empty())
                    data = cascLibReadFile(clStorage.h, e.path);
                else if (e.fileDataId > 0)
                    data = cascLibReadFileById(clStorage.h, e.fileDataId);
                if (data) { clBytes += data->size(); ++clOk; }
            }
        });
        std::cout << "  [CL] " << clOk << "/" << sample.size() << " reads, "
                  << std::fixed << std::setprecision(1)
                  << (clUs / 1000.0) << " ms, "
                  << (clBytes / (1024 * 1024)) << " MB\n";
        r.cascLibUs = clUs;
        r.clIterations = clOk;
        r.clBytesTotal = clBytes;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 12b. Batch Sequential Read (with WorkerPool)
    // ==================================================================
    if (woPoolStorage) {
        std::cout << "\n--- Benchmark 12b: Batch Sequential Read (pool, "
                  << nPoolThreads << " threads) ---\n";

        u64 woBytes = 0;
        u32 woOk = 0;
        BENCH_TIME_US(woUs, {
            for (auto& e : sample) {
                std::optional<std::vector<u8>> data;
                if (!e.path.empty())
                    data = woPoolStorage->readFile(e.path);
                else if (e.fileDataId > 0)
                    data = woPoolStorage->readFile(e.fileDataId);
                if (data) { woBytes += data->size(); ++woOk; }
            }
        });
        std::cout << "  [WO+pool] " << woOk << "/" << sample.size() << " reads, "
                  << std::fixed << std::setprecision(1)
                  << (woUs / 1000.0) << " ms, "
                  << (woBytes / (1024 * 1024)) << " MB\n";

        BenchResult r;
        r.operation = "batch_sequential_read (pool)";
        r.whiteoutUs = woUs;
        r.iterations = woOk;
        r.bytesTotal = woBytes;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        r.cascLibUs = clOpenBestUs > 0.0 ? results.back().cascLibUs : 0.0;
        // Reuse CascLib batch_sequential_read result for comparison.
        for (auto& prev : results) {
            if (prev.operation == "batch_sequential_read" && prev.hasCascLib) {
                r.cascLibUs = prev.cascLibUs;
                r.clIterations = prev.clIterations;
                r.clBytesTotal = prev.clBytesTotal;
                r.hasCascLib = true;
                break;
            }
        }
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 12c. readBatch (parallel, with WorkerPool)
    // ==================================================================
    if (woPoolStorage) {
        std::cout << "\n--- Benchmark 12c: readBatch (pool, "
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
            auto batchResults = woPoolStorage->readBatch(requests);
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
        r.operation = "readBatch (pool)";
        r.whiteoutUs = woUs;
        r.iterations = woOk;
        r.bytesTotal = woBytes;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        for (auto& prev : results) {
            if (prev.operation == "batch_sequential_read" && prev.hasCascLib) {
                r.cascLibUs = prev.cascLibUs;
                r.clIterations = prev.clIterations;
                r.clBytesTotal = prev.clBytesTotal;
                r.hasCascLib = true;
                break;
            }
        }
#endif
        results.push_back(r);
    }

    // ==================================================================
    // 13. Repeated Read (same file, hot-path benchmark)
    // ==================================================================
    if (!pathEntries.empty()) {
        std::cout << "\n--- Benchmark 13: Repeated Read (same file, hot path) ---\n";

        // Find a medium-sized file for this test.
        std::string targetPath;
        for (auto& e : pathEntries) {
            auto data = woStorage->readFile(e.path);
            if (data && data->size() >= 4096 && data->size() <= 262144) {
                targetPath = e.path;
                break;
            }
        }

        if (!targetPath.empty()) {
            constexpr int kRepeats = 200;
            u64 woBytes = 0;
            std::vector<double> woLatencies, clLatencies;

            for (int i = 0; i < kRepeats; ++i) {
                auto t0 = Clock::now();
                auto data = woStorage->readFile(targetPath);
                auto t1 = Clock::now();
                if (data) {
                    woLatencies.push_back(
                        std::chrono::duration<double, std::micro>(t1 - t0).count());
                    woBytes += data->size();
                }
            }

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            u64 clBytes = 0;
            for (int i = 0; i < kRepeats; ++i) {
                auto t0 = Clock::now();
                auto data = cascLibReadFile(clStorage.h, targetPath);
                auto t1 = Clock::now();
                if (data) {
                    clLatencies.push_back(
                        std::chrono::duration<double, std::micro>(t1 - t0).count());
                    clBytes += data->size();
                }
            }
#endif

            double woTotalUs = 0.0, clTotalUs = 0.0;
            for (auto v : woLatencies) woTotalUs += v;
            for (auto v : clLatencies) clTotalUs += v;

            std::cout << "  Target: " << targetPath << "\n";
            std::cout << "  [WO] " << woLatencies.size() << "x reads, "
                      << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                      << " ms total\n";
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            std::cout << "  [CL] " << clLatencies.size() << "x reads, "
                      << std::fixed << std::setprecision(1) << (clTotalUs / 1000.0)
                      << " ms total\n";
#endif

            printLatencyComparison("repeated_read (hot path)", woLatencies, clLatencies);

            BenchResult r;
            r.operation = "repeated_read (hot)";
            r.whiteoutUs = woTotalUs;
            r.iterations = static_cast<u32>(woLatencies.size());
            r.bytesTotal = woBytes;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            r.cascLibUs = clTotalUs;
            r.clIterations = static_cast<u32>(clLatencies.size());
            r.clBytesTotal = clBytes;
            r.hasCascLib = true;
#endif
            results.push_back(r);
        } else {
            std::cout << "  Skipped: no suitable medium-sized file found.\n";
        }
    }

    // ==================================================================
    // 13b. Repeated Read (with WorkerPool)
    // ==================================================================
    if (!pathEntries.empty() && woPoolStorage) {
        std::cout << "\n--- Benchmark 13b: Repeated Read (pool, "
                  << nPoolThreads << " threads) ---\n";

        std::string targetPath;
        for (auto& e : pathEntries) {
            auto data = woPoolStorage->readFile(e.path);
            if (data && data->size() >= 4096 && data->size() <= 262144) {
                targetPath = e.path;
                break;
            }
        }

        if (!targetPath.empty()) {
            constexpr int kRepeats = 200;
            u64 woBytes = 0;
            std::vector<double> woLatencies;

            for (int i = 0; i < kRepeats; ++i) {
                auto t0 = Clock::now();
                auto data = woPoolStorage->readFile(targetPath);
                auto t1 = Clock::now();
                if (data) {
                    woLatencies.push_back(
                        std::chrono::duration<double, std::micro>(t1 - t0).count());
                    woBytes += data->size();
                }
            }

            double woTotalUs = 0.0;
            for (auto v : woLatencies) woTotalUs += v;

            std::cout << "  Target: " << targetPath << "\n";
            std::cout << "  [WO+pool] " << woLatencies.size() << "x reads, "
                      << std::fixed << std::setprecision(1) << (woTotalUs / 1000.0)
                      << " ms total\n";

            // Reuse CascLib baseline from 13.
            std::vector<double> emptyVec;
            printLatencyComparison("repeated_read (pool)", woLatencies, emptyVec);

            BenchResult r;
            r.operation = "repeated_read (pool)";
            r.whiteoutUs = woTotalUs;
            r.iterations = static_cast<u32>(woLatencies.size());
            r.bytesTotal = woBytes;
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
            for (auto& prev : results) {
                if (prev.operation == "repeated_read (hot)" && prev.hasCascLib) {
                    r.cascLibUs = prev.cascLibUs;
                    r.clIterations = prev.clIterations;
                    r.clBytesTotal = prev.clBytesTotal;
                    r.hasCascLib = true;
                    break;
                }
            }
#endif
            results.push_back(r);
        } else {
            std::cout << "  Skipped: no suitable medium-sized file found.\n";
        }
    }

    // ==================================================================
    // 14. Total File Count
    // ==================================================================
    {
        std::cout << "\n--- Benchmark 14: Total File Count ---\n";

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

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
        // CascLib: CascGetStorageInfo with CascStorageTotalFileCount.
        DWORD clCount = 0;
        size_t cbLength = sizeof(clCount);
        BENCH_TIME_US(clUs, {
            CascGetStorageInfo(clStorage.h, CascStorageTotalFileCount,
                               &clCount, cbLength, &cbLength);
        });
        std::cout << "  [CL] totalFileCount = " << clCount
                  << " in " << std::fixed << std::setprecision(1) << clUs << " us\n";
        r.cascLibUs = clUs;
        r.hasCascLib = true;
#endif
        results.push_back(r);
    }

    // ==================================================================
    // Summary
    // ==================================================================
    std::cout << "\n================================================================\n";
    std::cout << "RESULTS SUMMARY\n";
    std::cout << "================================================================\n";
    printResultsTable(results);

    // Verdict
#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    {
        int faster = 0, slower = 0, comparable = 0;
        for (auto& r : results) {
            if (!r.hasCascLib || r.cascLibUs <= 0.0 || r.whiteoutUs <= 0.0) continue;
            u32 clIters = (r.clIterations > 0) ? r.clIterations : r.iterations;
            double woPerIter = (r.iterations > 0)
                ? r.whiteoutUs / static_cast<double>(r.iterations) : r.whiteoutUs;
            double clPerIter = (clIters > 0)
                ? r.cascLibUs / static_cast<double>(clIters) : r.cascLibUs;
            if (woPerIter <= 0.0 || clPerIter <= 0.0) continue;
            double speedup = clPerIter / woPerIter;
            if (speedup >= 1.05) ++faster;
            else if (speedup <= 0.95) ++slower;
            else ++comparable;
        }
        std::cout << "Verdict: WhiteoutLib is faster in " << faster
                  << " ops, comparable in " << comparable
                  << " ops, slower in " << slower << " ops.\n";
        if (slower > 0) {
            std::cout << "\nOperations where CascLib is faster (>5%):\n";
            for (auto& r : results) {
                if (!r.hasCascLib || r.cascLibUs <= 0.0 || r.whiteoutUs <= 0.0) continue;
                u32 clIters = (r.clIterations > 0) ? r.clIterations : r.iterations;
                double woPerIter = (r.iterations > 0)
                    ? r.whiteoutUs / static_cast<double>(r.iterations) : r.whiteoutUs;
                double clPerIter = (clIters > 0)
                    ? r.cascLibUs / static_cast<double>(clIters) : r.cascLibUs;
                if (woPerIter <= 0.0 || clPerIter <= 0.0) continue;
                double speedup = clPerIter / woPerIter;
                if (speedup <= 0.95) {
                    std::string label = r.operation;
                    if (!r.sizeCategory.empty()) label += " [" + r.sizeCategory + "]";
                    std::cout << "  " << label << ": "
                              << std::fixed << std::setprecision(2)
                              << (1.0 / speedup) << "x slower\n";
                }
            }
        }
    }
#endif

    std::cout << "\n";

    // Write CSV
    if (!csvPath.empty()) {
        writeResultsCsv(csvPath, results);
    }
}

// ============================================================================
// Entry point
// ============================================================================

int main(int argc, char* argv[]) {
    std::string cascPath;
    std::string csvPath = "casc_perf_crossref.csv";

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " <casc_path> [output_csv]\n\n"
                      << "  casc_path  — path to a CASC storage directory\n"
                      << "  output_csv — CSV output path (default: casc_perf_crossref.csv)\n\n"
                      << "Operations benchmarked:\n"
                      << "  Storage open, enumerate, fileExists, fileSize, fileInfo,\n"
                      << "  readFile (path/id), batch read, size-category read,\n"
                      << "  listFiles, repeated read, totalFileCount\n";
            return 0;
        }
        if (a.size() >= 2 && a[0] == '-' && a[1] == '-') continue;
        if (a.size() >= 1 && a[0] == '-') continue;
        positional.push_back(a);
    }

    if (positional.empty()) {
        std::cerr << "Error: no CASC storage path provided.\n"
                  << "Usage: " << argv[0] << " <casc_path> [output_csv]\n";
        return 1;
    }

    cascPath = positional[0];
    if (positional.size() >= 2)
        csvPath = positional[1];

    if (!fs::exists(cascPath)) {
        std::cerr << "Error: path does not exist: " << cascPath << "\n";
        return 1;
    }

    runPerfCrossRef(cascPath, csvPath);
    return 0;
}

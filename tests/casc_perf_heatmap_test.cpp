// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// CASC performance heatmap test: opens a CASC storage, reads every file,
/// records per-file timings, and emits a heatmap CSV plus summary statistics.
///
/// Usage:
///   casc_perf_heatmap_test.exe <casc_path> [output_dir]
///
///   casc_path  — path to a CASC storage (directory containing .build.info or Data/)
///   output_dir — optional directory for CSV output (default: current directory)
///
/// Outputs:
///   <output_dir>/casc_perf_raw.csv       — per-file read timing data
///   <output_dir>/casc_perf_heatmap.csv   — 2D frequency matrix (size bucket × latency bucket)
///   <output_dir>/casc_perf_summary.csv   — per-size-bucket summary statistics
///   Console                              — human-readable summary + heatmap preview
///
/// The raw CSV can be loaded into Python/Excel for custom heatmap visualization.
/// Example Python one-liner:
///   import pandas as pd; import seaborn as sns; import matplotlib.pyplot as plt
///   h = pd.read_csv("casc_perf_heatmap.csv", index_col=0)
///   sns.heatmap(h, annot=True, fmt="d", cmap="YlOrRd"); plt.show()

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ============================================================================
// Timing record for a single file read
// ============================================================================

struct ReadSample {
    std::string path;
    i32 fileDataId = 0;
    u64 decodedSize = 0;
    double readUs = 0.0; // microseconds
};

// ============================================================================
// Size & latency bucket definitions
// ============================================================================

struct Bucket {
    const char* label;
    u64 lo; // inclusive
    u64 hi; // exclusive (0 = unlimited)
};

static const Bucket kSizeBuckets[] = {
    {"0-1KB",     0,              1024},
    {"1-4KB",     1024,           4096},
    {"4-16KB",    4096,           16384},
    {"16-64KB",   16384,          65536},
    {"64-256KB",  65536,          262144},
    {"256KB-1MB", 262144,         1048576},
    {"1-4MB",     1048576,        4194304},
    {"4-16MB",    4194304,        16777216},
    {"16MB+",     16777216,       0},
};
static constexpr size_t kNumSizeBuckets = sizeof(kSizeBuckets) / sizeof(kSizeBuckets[0]);

static const Bucket kLatencyBuckets[] = {
    {"0-50us",      0,        50},
    {"50-100us",    50,       100},
    {"100-250us",   100,      250},
    {"250-500us",   250,      500},
    {"500us-1ms",   500,      1000},
    {"1-2ms",       1000,     2000},
    {"2-5ms",       2000,     5000},
    {"5-10ms",      5000,     10000},
    {"10-50ms",     10000,    50000},
    {"50-100ms",    50000,    100000},
    {"100ms+",      100000,   0},
};
static constexpr size_t kNumLatencyBuckets = sizeof(kLatencyBuckets) / sizeof(kLatencyBuckets[0]);

static size_t sizeBucketIndex(u64 size) {
    for (size_t i = 0; i < kNumSizeBuckets; ++i) {
        if (kSizeBuckets[i].hi == 0 || size < kSizeBuckets[i].hi)
            return i;
    }
    return kNumSizeBuckets - 1;
}

static size_t latencyBucketIndex(double us) {
    for (size_t i = 0; i < kNumLatencyBuckets; ++i) {
        if (kLatencyBuckets[i].hi == 0 || us < static_cast<double>(kLatencyBuckets[i].hi))
            return i;
    }
    return kNumLatencyBuckets - 1;
}

// ============================================================================
// Statistics helpers
// ============================================================================

struct BucketStats {
    u32 count = 0;
    double minUs = 0.0;
    double maxUs = 0.0;
    double avgUs = 0.0;
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double totalMB = 0.0;
    double throughputMBs = 0.0; // MB/s
};

static double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p / 100.0 * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static BucketStats computeStats(const std::vector<ReadSample>& samples) {
    BucketStats s;
    if (samples.empty()) return s;

    s.count = static_cast<u32>(samples.size());

    std::vector<double> latencies;
    latencies.reserve(samples.size());
    double totalUs = 0.0;
    u64 totalBytes = 0;

    for (auto& r : samples) {
        latencies.push_back(r.readUs);
        totalUs += r.readUs;
        totalBytes += r.decodedSize;
    }

    std::sort(latencies.begin(), latencies.end());

    s.minUs = latencies.front();
    s.maxUs = latencies.back();
    s.avgUs = totalUs / static_cast<double>(s.count);
    s.p50Us = percentile(latencies, 50.0);
    s.p95Us = percentile(latencies, 95.0);
    s.p99Us = percentile(latencies, 99.0);
    s.totalMB = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
    s.throughputMBs = (totalUs > 0.0)
        ? (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / (totalUs / 1e6)
        : 0.0;

    return s;
}

// ============================================================================
// CSV output
// ============================================================================

static void writeRawCsv(const std::string& path, const std::vector<ReadSample>& samples) {
    std::ofstream f(path);
    if (!f) {
        std::cerr << "  WARNING: could not write " << path << "\n";
        return;
    }
    f << "path,fileDataId,decodedSize,readUs,sizeBucket,latencyBucket\n";
    for (auto& s : samples) {
        // Escape commas in path
        std::string escapedPath = s.path;
        bool needsQuote = escapedPath.find(',') != std::string::npos
                       || escapedPath.find('"') != std::string::npos;
        if (needsQuote) {
            // Double any quotes
            std::string tmp;
            for (char c : escapedPath) {
                if (c == '"') tmp += '"';
                tmp += c;
            }
            escapedPath = "\"" + tmp + "\"";
        }

        f << escapedPath << ","
          << s.fileDataId << ","
          << s.decodedSize << ","
          << std::fixed << std::setprecision(1) << s.readUs << ","
          << kSizeBuckets[sizeBucketIndex(s.decodedSize)].label << ","
          << kLatencyBuckets[latencyBucketIndex(s.readUs)].label << "\n";
    }
    std::cout << "  Wrote raw CSV: " << path << " (" << samples.size() << " rows)\n";
}

static void writeHeatmapCsv(const std::string& path, const std::vector<ReadSample>& samples) {
    // Build 2D frequency matrix: rows = size buckets, cols = latency buckets
    std::vector<std::vector<u32>> matrix(kNumSizeBuckets, std::vector<u32>(kNumLatencyBuckets, 0));

    for (auto& s : samples) {
        size_t si = sizeBucketIndex(s.decodedSize);
        size_t li = latencyBucketIndex(s.readUs);
        matrix[si][li]++;
    }

    std::ofstream f(path);
    if (!f) {
        std::cerr << "  WARNING: could not write " << path << "\n";
        return;
    }

    // Header row: latency bucket labels
    f << "size_bucket";
    for (size_t l = 0; l < kNumLatencyBuckets; ++l) {
        f << "," << kLatencyBuckets[l].label;
    }
    f << "\n";

    // Data rows
    for (size_t s = 0; s < kNumSizeBuckets; ++s) {
        f << kSizeBuckets[s].label;
        for (size_t l = 0; l < kNumLatencyBuckets; ++l) {
            f << "," << matrix[s][l];
        }
        f << "\n";
    }

    std::cout << "  Wrote heatmap CSV: " << path << "\n";
}

static void writeSummaryCsv(const std::string& path, const std::vector<ReadSample>& samples) {
    // Group samples by size bucket
    std::vector<std::vector<ReadSample>> groups(kNumSizeBuckets);
    for (auto& s : samples) {
        groups[sizeBucketIndex(s.decodedSize)].push_back(s);
    }

    std::ofstream f(path);
    if (!f) {
        std::cerr << "  WARNING: could not write " << path << "\n";
        return;
    }

    f << "sizeBucket,count,minUs,avgUs,p50Us,p95Us,p99Us,maxUs,totalMB,throughputMBs\n";
    for (size_t i = 0; i < kNumSizeBuckets; ++i) {
        auto st = computeStats(groups[i]);
        f << kSizeBuckets[i].label << ","
          << st.count << ","
          << std::fixed << std::setprecision(1)
          << st.minUs << ","
          << st.avgUs << ","
          << st.p50Us << ","
          << st.p95Us << ","
          << st.p99Us << ","
          << st.maxUs << ","
          << std::setprecision(2) << st.totalMB << ","
          << std::setprecision(1) << st.throughputMBs << "\n";
    }

    std::cout << "  Wrote summary CSV: " << path << "\n";
}

// ============================================================================
// Console heatmap rendering
// ============================================================================

static void printHeatmapToConsole(const std::vector<ReadSample>& samples) {
    // Build 2D frequency matrix
    std::vector<std::vector<u32>> matrix(kNumSizeBuckets, std::vector<u32>(kNumLatencyBuckets, 0));
    u32 maxCount = 0;

    for (auto& s : samples) {
        size_t si = sizeBucketIndex(s.decodedSize);
        size_t li = latencyBucketIndex(s.readUs);
        matrix[si][li]++;
        maxCount = std::max(maxCount, matrix[si][li]);
    }

    // ANSI heat colors: dark → bright (8 levels)
    // Using background colors for a visual heatmap effect
    static const char* kHeatColors[] = {
        "\033[48;5;232m",  // near-black (0)
        "\033[48;5;52m",   // dark red
        "\033[48;5;88m",   // medium red
        "\033[48;5;124m",  // red
        "\033[48;5;166m",  // orange-red
        "\033[48;5;208m",  // orange
        "\033[48;5;220m",  // yellow
        "\033[48;5;229m",  // bright yellow
    };
    static constexpr size_t kNumHeatColors = sizeof(kHeatColors) / sizeof(kHeatColors[0]);
    static const char* kReset = "\033[0m";

    std::cout << "\n  Heatmap (file reads: size bucket x latency bucket)\n";
    std::cout << "  " << std::setw(12) << " ";
    for (size_t l = 0; l < kNumLatencyBuckets; ++l) {
        std::cout << std::setw(10) << kLatencyBuckets[l].label;
    }
    std::cout << "\n";

    for (size_t s = 0; s < kNumSizeBuckets; ++s) {
        std::cout << "  " << std::setw(12) << kSizeBuckets[s].label;
        for (size_t l = 0; l < kNumLatencyBuckets; ++l) {
            u32 val = matrix[s][l];
            size_t colorIdx = 0;
            if (val > 0 && maxCount > 0) {
                double normalized = static_cast<double>(val) / static_cast<double>(maxCount);
                colorIdx = static_cast<size_t>(normalized * static_cast<double>(kNumHeatColors - 1));
                colorIdx = std::min(colorIdx, kNumHeatColors - 1);
            }
            std::cout << kHeatColors[colorIdx] << std::setw(10) << val << kReset;
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

static void printSummaryToConsole(const std::vector<ReadSample>& samples) {
    // Group samples by size bucket
    std::vector<std::vector<ReadSample>> groups(kNumSizeBuckets);
    for (auto& s : samples) {
        groups[sizeBucketIndex(s.decodedSize)].push_back(s);
    }

    std::cout << "\n  Per-bucket summary:\n";
    std::cout << "  " << std::setw(12) << "Size"
              << std::setw(8) << "Count"
              << std::setw(10) << "Min(us)"
              << std::setw(10) << "Avg(us)"
              << std::setw(10) << "P50(us)"
              << std::setw(10) << "P95(us)"
              << std::setw(10) << "P99(us)"
              << std::setw(10) << "Max(us)"
              << std::setw(10) << "MB"
              << std::setw(12) << "MB/s"
              << "\n";
    std::cout << "  " << std::string(112, '-') << "\n";

    for (size_t i = 0; i < kNumSizeBuckets; ++i) {
        if (groups[i].empty()) continue;
        auto st = computeStats(groups[i]);
        std::cout << "  " << std::setw(12) << kSizeBuckets[i].label
                  << std::setw(8) << st.count
                  << std::fixed
                  << std::setw(10) << std::setprecision(1) << st.minUs
                  << std::setw(10) << std::setprecision(1) << st.avgUs
                  << std::setw(10) << std::setprecision(1) << st.p50Us
                  << std::setw(10) << std::setprecision(1) << st.p95Us
                  << std::setw(10) << std::setprecision(1) << st.p99Us
                  << std::setw(10) << std::setprecision(1) << st.maxUs
                  << std::setw(10) << std::setprecision(2) << st.totalMB
                  << std::setw(12) << std::setprecision(1) << st.throughputMBs
                  << "\n";
    }
}

// ============================================================================
// Main test logic
// ============================================================================

static void runPerfTest(const std::string& cascPath, const std::string& outputDir) {
    std::cout << "\n================================================================\n";
    std::cout << "CASC Performance Heatmap Test\n";
    std::cout << "  Storage path: " << cascPath << "\n";
    std::cout << "  Output dir:   " << outputDir << "\n";
    std::cout << "================================================================\n";

    // ------------------------------------------------------------------
    // Phase 1: Open storage
    // ------------------------------------------------------------------
    std::cout << "\n--- Phase 1: Opening storage ---\n";
    auto tOpen0 = Clock::now();

    const size_t numThreads = std::max<size_t>(1, std::thread::hardware_concurrency());
    whiteout::utils::SimpleThreadPool pool(numThreads);
    std::cout << "  Worker threads: " << numThreads << "\n";

    // Use progress callback to time each phase of Storage::open()
    auto tStepStart = Clock::now();
    auto progressCb = [&](ProgressStep step, u32 current, u32 total) -> bool {
        auto now = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - tStepStart).count();

        static const char* stepNames[] = {
            "LoadingBuildConfig", "LoadingCdnConfig", "LoadingIndexFiles",
            "MappingArchives", "LoadingEncodingTable", "LoadingRootManifest", "Ready"
        };
        auto name = (static_cast<int>(step) < 7) ? stepNames[static_cast<int>(step)] : "?";
        std::cout << "    [" << std::fixed << std::setprecision(1) << ms << " ms] "
                  << name;
        if (total > 0)
            std::cout << " (" << current << "/" << total << ")";
        std::cout << "\n";

        tStepStart = now;
        return true;
    };

    OpenOptions openOpts;
    openOpts.path = cascPath;
    openOpts.pool = &pool;
    openOpts.progressCallback = progressCb;

    std::string openError;
    auto storage = Storage::open(openOpts);
    if (!storage) {
        std::cerr << "  FATAL: could not open storage"
                  << " (error code: " << Storage::lastError() << ")\n";
        return;
    }

    auto tOpen1 = Clock::now();
    double openMs = std::chrono::duration<double, std::milli>(tOpen1 - tOpen0).count();
    std::cout << "  Storage opened in " << std::fixed << std::setprecision(1)
              << openMs << " ms\n";

    auto product = storage->product();
    if (product) {
        std::cout << "  Product: " << product->name
                  << " v" << product->version
                  << " (build " << product->buildId << ")\n";
    }

    auto totalCount = storage->totalFileCount();
    if (totalCount) {
        std::cout << "  Total files in root: " << *totalCount << "\n";
    }

    // ------------------------------------------------------------------
    // Phase 2: Enumerate all entries
    // ------------------------------------------------------------------
    std::cout << "\n--- Phase 2: Enumerating entries ---\n";
    auto tEnum0 = Clock::now();

    struct Entry {
        std::string path;
        i32 fileDataId;
    };
    std::vector<Entry> entries;
    storage->enumerate([&](const FindEntry& fe) {
        entries.push_back({fe.path, fe.fileDataId});
        return true;
    });

    auto tEnum1 = Clock::now();
    double enumMs = std::chrono::duration<double, std::milli>(tEnum1 - tEnum0).count();
    std::cout << "  Enumerated " << entries.size() << " entries in "
              << std::fixed << std::setprecision(1) << enumMs << " ms\n";

    // Sample a representative subset — reading 1M+ files is overkill.
    // Use a deterministic stride to get an evenly-spaced sample.
    constexpr size_t kMaxSampleSize = 5000;
    if (entries.size() > kMaxSampleSize) {
        size_t stride = entries.size() / kMaxSampleSize;
        std::vector<Entry> sampled;
        sampled.reserve(kMaxSampleSize);
        for (size_t i = 0; i < entries.size() && sampled.size() < kMaxSampleSize; i += stride) {
            sampled.push_back(std::move(entries[i]));
        }
        entries = std::move(sampled);
    }
    std::cout << "  Sample size: " << entries.size() << " files\n";

    // ------------------------------------------------------------------
    // Phase 3: Read sampled files and record timing
    // ------------------------------------------------------------------
    std::cout << "\n--- Phase 3: Reading sampled files ---\n";
    auto tRead0 = Clock::now();

    std::vector<ReadSample> samples;
    samples.reserve(entries.size());
    u32 readFailed = 0;
    u64 totalBytesRead = 0;

    u32 progressInterval = std::max<u32>(1, static_cast<u32>(entries.size()) / 20);

    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];

        if (i % progressInterval == 0) {
            double pct = 100.0 * static_cast<double>(i) / static_cast<double>(entries.size());
            std::cout << "  [" << std::fixed << std::setprecision(0) << pct << "%] "
                      << i << "/" << entries.size() << " files read, "
                      << (totalBytesRead / (1024 * 1024)) << " MB decoded\r" << std::flush;
        }

        // Time the individual read
        auto t0 = Clock::now();
        std::optional<std::vector<u8>> data;
        if (!e.path.empty()) {
            data = storage->readFile(e.path);
        } else if (e.fileDataId > 0) {
            data = storage->readFile(e.fileDataId);
        }
        auto t1 = Clock::now();

        if (!data) {
            ++readFailed;
            continue;
        }

        double readUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
        totalBytesRead += data->size();

        samples.push_back({
            e.path,
            e.fileDataId,
            static_cast<u64>(data->size()),
            readUs,
        });
    }

    auto tRead1 = Clock::now();
    double readMs = std::chrono::duration<double, std::milli>(tRead1 - tRead0).count();

    std::cout << "  [100%] " << entries.size() << "/" << entries.size() << " files read"
              << std::string(40, ' ') << "\n";
    std::cout << "  Successfully read: " << samples.size() << " files\n";
    std::cout << "  Failed/skipped:    " << readFailed << " files\n";
    std::cout << "  Total decoded:     " << std::fixed << std::setprecision(2)
              << (static_cast<double>(totalBytesRead) / (1024.0 * 1024.0)) << " MB\n";
    std::cout << "  Total read time:   " << std::setprecision(1) << readMs << " ms\n";
    std::cout << "  Avg throughput:    " << std::setprecision(1)
              << (readMs > 0.0
                  ? (static_cast<double>(totalBytesRead) / (1024.0 * 1024.0)) / (readMs / 1000.0)
                  : 0.0)
              << " MB/s\n";

    // ------------------------------------------------------------------
    // Phase 4: Output heatmap and statistics
    // ------------------------------------------------------------------
    std::cout << "\n--- Phase 4: Generating reports ---\n";

    fs::create_directories(outputDir);

    writeRawCsv(outputDir + "/casc_perf_raw.csv", samples);
    writeHeatmapCsv(outputDir + "/casc_perf_heatmap.csv", samples);
    writeSummaryCsv(outputDir + "/casc_perf_summary.csv", samples);

    // Also write a metadata file for reproducibility
    {
        std::ofstream f(outputDir + "/casc_perf_meta.txt");
        if (f) {
            f << "casc_path=" << cascPath << "\n";
            if (product) {
                f << "product=" << product->name << "\n";
                f << "version=" << product->version << "\n";
                f << "build=" << product->buildId << "\n";
            }
            f << "total_entries=" << entries.size() << "\n";
            f << "files_read=" << samples.size() << "\n";
            f << "files_failed=" << readFailed << "\n";
            f << "total_bytes=" << totalBytesRead << "\n";
            f << "open_ms=" << std::fixed << std::setprecision(1) << openMs << "\n";
            f << "enumerate_ms=" << enumMs << "\n";
            f << "read_all_ms=" << readMs << "\n";
            std::cout << "  Wrote metadata: " << outputDir << "/casc_perf_meta.txt\n";
        }
    }

    // Console heatmap
    printHeatmapToConsole(samples);
    printSummaryToConsole(samples);

    // ------------------------------------------------------------------
    // Overall timing summary
    // ------------------------------------------------------------------
    std::cout << "\n================================================================\n";
    std::cout << "Timing Summary\n";
    std::cout << "  Open:      " << std::fixed << std::setprecision(1) << openMs << " ms\n";
    std::cout << "  Enumerate: " << enumMs << " ms\n";
    std::cout << "  Read all:  " << readMs << " ms\n";

    auto overall = computeStats(samples);
    std::cout << "\nOverall Read Latency (across all files):\n";
    std::cout << "  Count: " << overall.count << "\n";
    std::cout << "  Min:   " << std::setprecision(1) << overall.minUs << " us\n";
    std::cout << "  Avg:   " << overall.avgUs << " us\n";
    std::cout << "  P50:   " << overall.p50Us << " us\n";
    std::cout << "  P95:   " << overall.p95Us << " us\n";
    std::cout << "  P99:   " << overall.p99Us << " us\n";
    std::cout << "  Max:   " << overall.maxUs << " us\n";
    std::cout << "================================================================\n";
}

// ============================================================================
// Entry point — custom main to accept positional CLI arguments
// ============================================================================

int main(int argc, char* argv[]) {
    // Parse our custom arguments before any test framework arguments.
    // Usage: <exe> <casc_path> [output_dir] [-- catch2 args...]
    std::string cascPath;
    std::string outputDir = ".";

    // Collect non-Catch2 arguments (everything before "--" or that doesn't start with "-")
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    // Extract positional args
    std::vector<std::string> positional;
    for (auto& a : args) {
        if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " <casc_path> [output_dir]\n\n"
                      << "  casc_path  — path to a CASC storage directory\n"
                      << "  output_dir — directory for CSV output files (default: current dir)\n\n"
                      << "Outputs:\n"
                      << "  casc_perf_raw.csv     — per-file read timing data\n"
                      << "  casc_perf_heatmap.csv — 2D frequency matrix (size × latency)\n"
                      << "  casc_perf_summary.csv — per-size-bucket summary statistics\n"
                      << "  casc_perf_meta.txt    — run metadata for reproducibility\n";
            return 0;
        }
        // Skip Catch2-style flags
        if (a.size() >= 2 && a[0] == '-' && a[1] == '-') continue;
        if (a.size() >= 1 && a[0] == '-') continue;
        positional.push_back(a);
    }

    if (positional.empty()) {
        std::cerr << "Error: no CASC storage path provided.\n"
                  << "Usage: " << argv[0] << " <casc_path> [output_dir]\n";
        return 1;
    }

    cascPath = positional[0];
    if (positional.size() >= 2) {
        outputDir = positional[1];
    }

    if (!fs::exists(cascPath)) {
        std::cerr << "Error: path does not exist: " << cascPath << "\n";
        return 1;
    }

    runPerfTest(cascPath, outputDir);
    return 0;
}

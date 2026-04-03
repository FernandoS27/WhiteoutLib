// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// CASC parallel test (Phase 4): verify that reading files with a WorkerPool
/// yields byte-identical results to serial reads, and optionally log timing.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/interfaces.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

// ============================================================================
// Test Framework
// ============================================================================


// ============================================================================
// TestWorkerPool — real multi-threaded pool
// ============================================================================

class TestWorkerPool : public interfaces::WorkerPool {
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
        for (auto& t : m_threads) t.join();
    }

    void submit(const interfaces::WorkerTask& task) override {
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

    size_t threadCount() const noexcept override { return m_threads.size(); }

    size_t totalSubmitted() const {
        return m_totalSubmitted.load(std::memory_order_relaxed);
    }

    void resetCounters() {
        m_totalSubmitted.store(0, std::memory_order_relaxed);
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
};

// ============================================================================
// Helpers
// ============================================================================

static std::string findCorpus() {
    for (auto& p : {"Corpus/CASC", "../Corpus/CASC", "../../Corpus/CASC",
                     "C:/Projects/WhiteoutLib/Corpus/CASC"}) {
        if (std::filesystem::exists(p))
            return p;
    }
    return "";
}

/// Collect the first N readable file paths from storage.
static std::vector<std::string> collectPaths(Storage& storage, size_t maxCount) {
    std::vector<std::string> paths;
    storage.enumerate([&](const EnumerateEntry& fe) {
        if (paths.size() >= maxCount) return false;
        if (!fe.path.empty())
            paths.push_back(std::string(fe.path));
        return true;
    });
    return paths;
}

/// Read a set of files and record results.
struct ReadResult {
    std::string path;
    std::vector<u8> data;
    bool success = false;
};

static std::vector<ReadResult> readFiles(Storage& storage,
                                         const std::vector<std::string>& paths) {
    std::vector<ReadResult> results;
    results.reserve(paths.size());
    for (auto& p : paths) {
        ReadResult r;
        r.path = p;
        auto data = storage.readFile(p);
        if (data) {
            r.data = std::move(*data);
            r.success = true;
        }
        results.push_back(std::move(r));
    }
    return results;
}

// ============================================================================
// Test: serial vs parallel produces identical results
// ============================================================================

TEST_CASE("Serial Vs Parallel", "[casc][parallel][corpus]") {
    auto corpus = findCorpus();
    if (corpus.empty()) { SKIP("Corpus not found"); }

    struct CorpusEntry { std::string label; std::string path; };
    std::vector<CorpusEntry> entries;
    if (std::filesystem::exists(corpus + "/Diablo III"))
        entries.push_back({"Diablo III", corpus + "/Diablo III"});
    if (std::filesystem::exists(corpus + "/Warcraft III"))
        entries.push_back({"Warcraft III Reforged", corpus + "/Warcraft III"});
    if (entries.empty()) SKIP("No corpus subdirectories found");

    for (auto& [label, path] : entries) {
    DYNAMIC_SECTION("Parallel: " << label) {
    constexpr size_t kFileCount = 50; // Number of files to compare.

    std::cout << "\n[Test: " << label << " serial vs parallel]\n";
    std::cout << "  Path: " << path << "\n";

    // --- Serial open & read ---
    auto t0 = std::chrono::steady_clock::now();
    auto serial = Storage::open(path);
    REQUIRE(serial.has_value());

    auto paths = collectPaths(*serial, kFileCount);
    std::cout << "  Testing " << paths.size() << " files.\n";
    if (paths.empty()) {
        std::cout << "  SKIP: no files found.\n";
        return;
    }

    auto serialResults = readFiles(*serial, paths);
    auto t1 = std::chrono::steady_clock::now();
    auto serialMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // Count successful serial reads.
    size_t serialSuccess = 0;
    for (auto& r : serialResults)
        if (r.success) ++serialSuccess;
    CHECK(serialSuccess > 0);

    serial->close();

    // --- Parallel open & read ---
    TestWorkerPool pool(4);

    auto t2 = std::chrono::steady_clock::now();
    auto parallel = Storage::open(path, &pool);
    REQUIRE(parallel.has_value());

    auto parallelResults = readFiles(*parallel, paths);
    auto t3 = std::chrono::steady_clock::now();
    auto parallelMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();

    // Count successful parallel reads.
    size_t parallelSuccess = 0;
    for (auto& r : parallelResults)
        if (r.success) ++parallelSuccess;
    CHECK(parallelSuccess > 0);

    // --- Compare results ---
    CHECK(serialResults.size() == parallelResults.size());

    size_t identical = 0;
    size_t mismatches = 0;
    size_t bothSkipped = 0;
    for (size_t i = 0; i < serialResults.size(); ++i) {
        auto& s = serialResults[i];
        auto& p = parallelResults[i];

        if (!s.success && !p.success) {
            ++bothSkipped;
            continue;
        }
        if (s.success != p.success) {
            ++mismatches;
            if (mismatches <= 3) {
                std::cout << "    DIFF: " << s.path
                          << " serial=" << (s.success ? "OK" : "FAIL")
                          << " parallel=" << (p.success ? "OK" : "FAIL") << "\n";
            }
            continue;
        }
        if (s.data.size() != p.data.size() ||
            std::memcmp(s.data.data(), p.data.data(), s.data.size()) != 0) {
            ++mismatches;
            if (mismatches <= 3) {
                std::cout << "    DIFF: " << s.path
                          << " serial=" << s.data.size()
                          << "B parallel=" << p.data.size() << "B\n";
            }
            continue;
        }
        ++identical;
    }

    CHECK(mismatches == 0);

    std::cout << "  Timing: serial=" << serialMs << "ms  parallel=" << parallelMs << "ms\n";
    std::cout << "  Pool submitted " << pool.totalSubmitted() << " tasks.\n";

    parallel->close();
} // DYNAMIC_SECTION
} // for entries
}

// ============================================================================
// main
// ============================================================================

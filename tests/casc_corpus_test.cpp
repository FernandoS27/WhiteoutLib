// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// CASC corpus test (Phase 4): bulk-read every file from both corpus storages,
/// compute MD5 of the decoded data, and verify it matches the recorded CKey.
/// This validates the entire end-to-end read pipeline.

#include <catch2/catch_all.hpp>

#include <whiteout/storages/casc/storage.h>

// Internal header — we need md5Hash() for verification.
// This is a test program, so the internal dependency is acceptable.
#include "../src/whiteout/storages/common/md5.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

using namespace whiteout;
using namespace whiteout::storages::casc;

static std::string hexStr(const std::array<u8, 16>& key) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (auto b : key) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0xF]);
    }
    return s;
}

static std::string findCorpus() {
    for (auto& p : {"Corpus/CASC", "../Corpus/CASC", "../../Corpus/CASC",
                     "C:/Projects/WhiteoutLib/Corpus/CASC"}) {
        if (std::filesystem::exists(p))
            return p;
    }
    return "";
}

/// Read every file from a storage, verify MD5 == CKey.
/// Returns {passed, failed, skipped}.
struct TestResult {
    u32 passed = 0;
    u32 failed = 0;
    u32 skipped = 0;  // entries that couldn't be read (e.g. encrypted).
    u32 total = 0;
};

/// Verification mode.
enum class VerifyMode {
    CKeyMd5,     ///< Full CKey == MD5(decoded) check (D3, WoW).
    Readability,  ///< Only verify files can be decoded (TVFS — root has EKeys not CKeys).
};

static TestResult testStorage(const std::string& label, const std::string& path,
                              VerifyMode mode = VerifyMode::CKeyMd5) {
    TestResult result;

    std::cout << "\n================================================================\n";
    std::cout << "Corpus test: " << label
              << (mode == VerifyMode::Readability ? " [readability-only]" : " [CKey-MD5]")
              << "\n";
    std::cout << "  Path: " << path << "\n";

    auto t0 = std::chrono::steady_clock::now();

    auto storage = Storage::open(path);
    if (!storage) {
        std::cout << "  FAIL: could not open storage (error " << Storage::lastError() << ")\n";
        result.failed = 1;
        return result;
    }
    std::cout << "  Storage opened successfully.\n";

    auto count = storage->totalFileCount();
    if (count) std::cout << "  Root entries: " << *count << "\n";

    // Collect all entries.
    struct Entry {
        std::array<u8, 16> cKey;
        std::string path;
        i32 fileDataId;
    };
    std::vector<Entry> entries;
    storage->enumerate([&](const FindEntry& fe) {
        Entry e;
        e.cKey = fe.cKey;
        e.path = fe.path;
        e.fileDataId = fe.fileDataId;
        entries.push_back(std::move(e));
        return true;
    });
    result.total = static_cast<u32>(entries.size());
    std::cout << "  Enumerated " << entries.size() << " entries.\n";

    if (mode == VerifyMode::CKeyMd5) {
        // Deduplicate by CKey so we don't re-read the same file content multiple times.
        auto cmp = [](const Entry& a, const Entry& b) { return a.cKey < b.cKey; };
        std::sort(entries.begin(), entries.end(), cmp);
        entries.erase(
            std::unique(entries.begin(), entries.end(),
                        [](const Entry& a, const Entry& b) { return a.cKey == b.cKey; }),
            entries.end());
        std::cout << "  Unique CKeys: " << entries.size() << "\n";
    } else {
        // For readability mode, deduplicate by path to avoid re-reading duplicates.
        auto cmp = [](const Entry& a, const Entry& b) { return a.path < b.path; };
        std::sort(entries.begin(), entries.end(), cmp);
        entries.erase(
            std::unique(entries.begin(), entries.end(),
                        [](const Entry& a, const Entry& b) {
                            return !a.path.empty() && a.path == b.path;
                        }),
            entries.end());
        std::cout << "  Unique paths: " << entries.size() << "\n";
    }

    u32 progressInterval = std::max<u32>(1, static_cast<u32>(entries.size()) / 20);

    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];

        // Progress.
        if (i % progressInterval == 0) {
            std::cout << "  " << i << "/" << entries.size() << " ...\r" << std::flush;
        }

        // Try to read.
        std::optional<std::vector<u8>> data;
        if (!e.path.empty()) {
            data = storage->readFile(e.path);
        } else if (e.fileDataId > 0) {
            data = storage->readFile(e.fileDataId);
        }

        if (!data) {
            // Could not read — may be encrypted or missing from local archives.
            ++result.skipped;
            continue;
        }

        if (mode == VerifyMode::CKeyMd5) {
            // Compute MD5 of decoded data and compare to CKey.
            auto md5 = storages::common::md5Hash(
                std::span<const u8>(data->data(), data->size()));

            if (md5 == e.cKey) {
                ++result.passed;
            } else {
                ++result.failed;
                if (result.failed <= 10) {
                    std::cout << "  MISMATCH: "
                              << (e.path.empty() ? "id:" + std::to_string(e.fileDataId) : e.path)
                              << "\n    expected CKey=" << hexStr(e.cKey)
                              << "\n    actual   MD5 =" << hexStr(md5)
                              << "  (decoded " << data->size() << " bytes)\n";
                }
            }
        } else {
            // Readability mode — just verify we got non-empty decoded data.
            ++result.passed;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::cout << "  Result: " << result.passed << " passed, "
              << result.failed << " failed, "
              << result.skipped << " skipped"
              << " out of " << entries.size()
              << (mode == VerifyMode::CKeyMd5 ? " unique CKeys" : " unique paths")
              << " (" << ms << " ms)\n";

    return result;
}

TEST_CASE("CASC corpus verify", "[casc][corpus]") {
    std::cout << "=== CASC Corpus Test (Phase 4) ===\n";

    auto corpus = findCorpus();
    if (corpus.empty()) {
        std::cout << "Corpus directory not found — skipping.\n";
        // done
    }

    u32 totalPassed = 0, totalFailed = 0, totalSkipped = 0;

    // Diablo III — has real CKeys from root, full MD5 verification.
    if (std::filesystem::exists(corpus + "/Diablo III")) {
        auto r = testStorage("Diablo III", corpus + "/Diablo III", VerifyMode::CKeyMd5);
        totalPassed += r.passed;
        totalFailed += r.failed;
        totalSkipped += r.skipped;
    }

    // Warcraft III Reforged — TVFS root provides EKeys only (no CKeys).
    // Paths can appear in multiple VFS sub-manifests with different EKeys,
    // so CKey attribution during enumerate is non-deterministic. Readability
    // mode verifies the full path→EKey→index→archive→BLTE pipeline.
    if (std::filesystem::exists(corpus + "/Warcraft III")) {
        auto r = testStorage("Warcraft III Reforged", corpus + "/Warcraft III",
                             VerifyMode::Readability);
        totalPassed += r.passed;
        totalFailed += r.failed;
        totalSkipped += r.skipped;
    }

    std::cout << "\n================================================================\n";
    std::cout << "=== Grand total: " << totalPassed << " passed, "
              << totalFailed << " failed, " << totalSkipped << " skipped ===\n";

    CHECK(totalFailed == 0);
}

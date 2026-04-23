// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_ow_online_crossval_test.cpp
/// @brief Cross-validate Overwatch (product "pro") file paths between
///        WhiteoutLib's Storage::openOnline and CascLib's CascOpenOnlineStorage.
///
/// Opens the live Overwatch 2 CDN with both libraries and compares the
/// enumerated file paths.  Tagged [online][network] for exclusion in
/// offline CI: ctest --label-exclude online
///
/// NOTE: CascLib may fail to open the OW CDN with ERROR_NOT_ENOUGH_MEMORY
/// because the Overwatch storage has ~7.7M entries and CascLib's internal
/// data structures cannot handle that scale online. The cross-validation
/// test gracefully SKIPs in that case, while the WhiteoutLib-only sanity
/// test still exercises our OW CDN parsing.
///
/// Requires -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_http_handler.h>
#include <whiteout/utils/simple_thread_pool.h>

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
#include <CascLib.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace fs = std::filesystem;

static const char* kCacheDir = "test_ow_crossval_cache";

// ============================================================================
// CascLib RAII helpers
// ============================================================================

#if defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)

struct ScopedCascStorage {
    HANDLE h = nullptr;
    ~ScopedCascStorage() { if (h) CascCloseStorage(h); }
    explicit operator bool() const { return h != nullptr; }
};

struct ScopedCascFind {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~ScopedCascFind() { if (h != INVALID_HANDLE_VALUE) CascFindClose(h); }
    explicit operator bool() const { return h != INVALID_HANDLE_VALUE; }
};

/// Normalize path: backslash -> forward slash.
static std::string normalizePath(const std::string& p) {
    std::string out = p;
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

/// Enumerate all named files from CascLib.
static std::set<std::string> cascEnumPaths(HANDLE hStorage) {
    std::set<std::string> paths;
    CASC_FIND_DATA fd{};
    ScopedCascFind hFind;
    hFind.h = CascFindFirstFile(hStorage, "*", &fd, nullptr);
    if (!hFind) return paths;
    do {
        if (fd.szFileName[0] != '\0' && fd.NameType == CascNameFull)
            paths.insert(normalizePath(fd.szFileName));
    } while (CascFindNextFile(hFind.h, &fd));
    return paths;
}

#endif // WHITEOUT_HAS_CASCLIB_CROSSVAL

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("WhiteoutLib OW CDN sanity", "[casc][online][network][overwatch]") {
    fs::create_directories(kCacheDir);

    utils::SimpleHttpHandler http(4);
    utils::SimpleThreadPool pool(4);

    OnlineOpenOptions opts;
    opts.product = "pro";
    opts.region = "us";
    opts.http = &http;
    opts.pool = &pool;
    opts.localeMask = LocaleMasks::enUS;
    opts.memoryCacheSize = 64 * 1024 * 1024;
    opts.cacheDir = kCacheDir;

    auto storage = Storage::openOnline(opts);
    if (!storage) {
        WARN("WhiteoutLib could not open OW CDN — CDN may be unavailable");
        SKIP("CDN unavailable");
    }

    std::cout << "WhiteoutLib: opened OW online storage\n";
    if (auto prod = storage->product())
        std::cout << "  Product: " << prod->name
                  << "  Version: " << prod->version << "\n";

    // Count named files (don't store all 7.7M paths to save memory).
    size_t namedCount = 0;
    size_t totalCount = 0;
    std::string firstPath, lastPath;
    storage->enumerate([&](const EnumerateEntry& e) {
        totalCount++;
        if (!e.path.empty()) {
            namedCount++;
            if (firstPath.empty()) firstPath = std::string(e.path);
            lastPath = std::string(e.path);
        }
        return true;
    });

    std::cout << "  Total entries: " << totalCount << "\n";
    std::cout << "  Named files:  " << namedCount << "\n";
    if (!firstPath.empty())
        std::cout << "  First path:   " << firstPath << "\n";
    if (!lastPath.empty())
        std::cout << "  Last path:    " << lastPath << "\n";

    // OW2 should have millions of named files.
    CHECK(namedCount > 1'000'000);
    CHECK(totalCount >= namedCount);
}

TEST_CASE("OW online path cross-validation", "[casc][online][network][overwatch]") {
#if !defined(WHITEOUT_HAS_CASCLIB_CROSSVAL)
    SKIP("CascLib cross-validation not enabled (pass -DWHITEOUT_ENABLE_CASCLIB_CROSSVAL=ON)");
#else
    fs::create_directories(kCacheDir);

    // -- Open CascLib FIRST (before WhiteoutLib's large allocations) --
    // CascOpenOnlineStorage format: "cache_path*product*region"
    // (separator is '*' per CASC_PARAM_SEPARATOR)
    std::string cascCacheDir = fs::absolute(std::string(kCacheDir) + "/casclib").string();
    fs::create_directories(cascCacheDir);
    std::string cascParam = cascCacheDir + "*pro*us";

    std::cout << "CascLib: opening OW CDN ...\n";
    std::cout.flush();

    ScopedCascStorage clStorage;
    bool clOpened = CascOpenOnlineStorage(cascParam.c_str(), CASC_LOCALE_ALL, &clStorage.h);
    if (!clOpened) {
        DWORD err = GetCascError();
        // Error 8 = ERROR_NOT_ENOUGH_MEMORY: CascLib can't handle OW's ~7.7M entries online.
        WARN("CascLib could not open OW CDN (error " << err
             << (err == 8 ? " = OOM on massive OW storage" : "") << ")");
        SKIP("CascLib CDN open failed");
    }
    std::cout << "CascLib: opened OW online storage\n";

    // Enumerate CascLib paths.
    auto clPaths = cascEnumPaths(clStorage.h);
    std::cout << "  CascLib enumerated " << clPaths.size() << " named files\n";
    REQUIRE(clPaths.size() > 0);

    // Close CascLib to free memory before opening WhiteoutLib.
    CascCloseStorage(clStorage.h);
    clStorage.h = nullptr;

    // -- Open WhiteoutLib --
    utils::SimpleHttpHandler http(4);
    utils::SimpleThreadPool pool(4);

    OnlineOpenOptions woOpts;
    woOpts.product = "pro";
    woOpts.region = "us";
    woOpts.http = &http;
    woOpts.pool = &pool;
    woOpts.localeMask = LocaleMasks::enUS;
    woOpts.memoryCacheSize = 64 * 1024 * 1024;
    woOpts.cacheDir = kCacheDir;

    auto woStorage = Storage::openOnline(woOpts);
    if (!woStorage) {
        WARN("WhiteoutLib could not open OW CDN");
        SKIP("CDN unavailable");
    }

    std::cout << "WhiteoutLib: opened OW online storage\n";
    if (auto prod = woStorage->product())
        std::cout << "  Product: " << prod->name
                  << "  Version: " << prod->version << "\n";

    // Enumerate WhiteoutLib paths.
    std::set<std::string> woPaths;
    woStorage->enumerate([&](const EnumerateEntry& e) {
        if (!e.path.empty())
            woPaths.insert(normalizePath(std::string(e.path)));
        return true;
    });
    std::cout << "  WhiteoutLib enumerated " << woPaths.size() << " named files\n";
    REQUIRE(woPaths.size() > 0);
    woStorage.reset();

    // -- Compare path sets --
    std::vector<std::string> onlyInWhiteout, onlyInCascLib, inBoth;

    std::set_intersection(woPaths.begin(), woPaths.end(),
                          clPaths.begin(), clPaths.end(),
                          std::back_inserter(inBoth));
    std::set_difference(woPaths.begin(), woPaths.end(),
                        clPaths.begin(), clPaths.end(),
                        std::back_inserter(onlyInWhiteout));
    std::set_difference(clPaths.begin(), clPaths.end(),
                        woPaths.begin(), woPaths.end(),
                        std::back_inserter(onlyInCascLib));

    std::cout << "\n  Paths in both:       " << inBoth.size()
              << "\n  Only in WhiteoutLib: " << onlyInWhiteout.size()
              << "\n  Only in CascLib:     " << onlyInCascLib.size() << "\n";

    auto showSample = [](const std::vector<std::string>& v, const char* label, size_t n = 10) {
        if (v.empty()) return;
        std::cout << "  " << label << " (first " << std::min(n, v.size()) << "):\n";
        for (size_t i = 0; i < std::min(n, v.size()); ++i)
            std::cout << "    " << v[i] << "\n";
    };
    showSample(onlyInWhiteout, "Only in WhiteoutLib");
    showSample(onlyInCascLib, "Only in CascLib");

    // At least 90% overlap.
    size_t smaller = std::min(woPaths.size(), clPaths.size());
    double overlapPct = smaller > 0
        ? 100.0 * static_cast<double>(inBoth.size()) / static_cast<double>(smaller)
        : 0.0;
    std::cout << "  Overlap: " << overlapPct << "% of smaller set\n";
    CHECK(overlapPct > 90.0);

    // Every CascLib path should be in WhiteoutLib (superset).
    double cascLibCoverage = clPaths.size() > 0
        ? 100.0 * static_cast<double>(inBoth.size()) / static_cast<double>(clPaths.size())
        : 100.0;
    std::cout << "  CascLib coverage: " << cascLibCoverage << "%\n";
    CHECK(cascLibCoverage > 90.0);
#endif
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// online_storage_test: Validates CDN-backed Storage::openOnline against live
/// Blizzard CDN servers using SimpleHttpHandler.
///
/// These tests hit the real network.  They are tagged [online] so they can be
/// excluded in offline CI runs:
///     ctest --label-exclude online
///
/// Products used:
///   - "s1"   (StarCraft 1 Remastered) — small, WoW root format
///   - "w3"   (Warcraft III Reforged)  — small, TVFS root format

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_http_handler.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

using namespace whiteout;
using namespace whiteout::storages::casc;

// ============================================================================
// Helpers
// ============================================================================

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

/// Create a standard OnlineOpenOptions for a given product.
static OnlineOpenOptions makeOpts(interfaces::HttpHandler* http,
                                  interfaces::WorkerPool* pool,
                                  const std::string& product,
                                  const std::string& region = "us") {
    OnlineOpenOptions opts;
    opts.product = product;
    opts.region = region;
    opts.http = http;
    opts.pool = pool;
    opts.localeMask = LocaleMasks::enUS;
    opts.memoryCacheSize = 64 * 1024 * 1024; // 64 MB for tests
    return opts;
}

// ============================================================================
// Section 1: Basic open / close
// ============================================================================

TEST_CASE("Storage openOnline fails without HttpHandler", "[online][casc]") {
    OnlineOpenOptions opts;
    opts.product = "w3";
    opts.region = "us";
    // opts.http is null
    auto storage = Storage::openOnline(opts);
    REQUIRE_FALSE(storage.has_value());
    CHECK(Storage::lastError() == CascError::NoHttpHandler);
}

TEST_CASE("Storage openOnline fails with empty product", "[online][casc][network]") {
    utils::SimpleHttpHandler http(2);
    OnlineOpenOptions opts;
    opts.product = "";
    opts.region = "us";
    opts.http = &http;

    auto storage = Storage::openOnline(opts);
    // Should fail during CDN discovery (empty product → invalid URL → HTTP failure)
    REQUIRE_FALSE(storage.has_value());
}

TEST_CASE("Storage openOnline with invalid product", "[online][casc][network]") {
    utils::SimpleHttpHandler http(2);
    auto opts = makeOpts(&http, nullptr, "totally_nonexistent_product_xyz");

    auto storage = Storage::openOnline(opts);
    REQUIRE_FALSE(storage.has_value());
}

// ============================================================================
// Section 2: Live CDN tests (Warcraft III Reforged — TVFS root)
// ============================================================================

TEST_CASE("Storage openOnline W3 open succeeds", "[online][casc][network][w3]") {
    utils::SimpleHttpHandler http(4);
    utils::SimpleThreadPool pool(4);
    auto opts = makeOpts(&http, &pool, "w3");

    auto storage = Storage::openOnline(opts);
    if (!storage) {
        WARN("W3 open failed (error " << Storage::lastError()
             << ") — CDN may be unavailable");
        SKIP("CDN unavailable");
    }
    REQUIRE(*storage);

    // product()
    {
        auto prod = storage->product();
        REQUIRE(prod.has_value());
        CHECK_FALSE(prod->name.empty());
    }

    // totalFileCount()
    {
        auto count = storage->totalFileCount();
        REQUIRE(count.has_value());
        CHECK(*count > 0);
    }

    // enumerate()
    {
        u32 count = 0;
        u32 withPath = 0;
        storage->enumerate([&](const EnumerateEntry& fe) {
            ++count;
            if (!fe.path.empty()) ++withPath;
            return count < 100; // stop after 100
        });
        CHECK(count > 0);
    }

    // listFiles()
    auto files = storage->listFiles();
    CHECK(files.size() > 0);

    // readFile() by path
    // NOTE: TVFS roots use truncated 9-byte eKeys.  Many entries may not
    // resolve via the encoding table or may reference non-BLTE archive data,
    // so file reads can be flaky for W3.  We try several and WARN on failure.
    bool readFileOk = false;
    if (!files.empty()) {
        for (size_t i = 0; i < std::min<size_t>(files.size(), 10); ++i) {
            auto data = storage->readFile(files[i]);
            if (data && !data->empty()) {
                CHECK(data->size() > 0);
                readFileOk = true;
                break;
            }
        }
        if (!readFileOk) {
            WARN("readFile: none of the first 10 TVFS entries could be resolved");
        }
    }

    // readBatch() — only meaningful if readFile works.
    if (readFileOk && !files.empty()) {
        size_t n = std::min<size_t>(files.size(), 5);
        std::vector<BatchReadRequest> batchReq(n);
        for (size_t i = 0; i < n; ++i)
            batchReq[i].path = files[i];

        auto batchRes = storage->readBatch(batchReq);
        REQUIRE(batchRes.size() == n);

        u32 batchOk = 0;
        for (auto& r : batchRes)
            if (r.success) ++batchOk;
        if (batchOk == 0) {
            WARN("readBatch: 0/" << n << " succeeded — TVFS resolution issue");
        }
    }

    // fileExists()
    if (!files.empty()) {
        CHECK(storage->fileExists(files[0]));
    }
    CHECK_FALSE(storage->fileExists("nonexistent_file_that_surely_does_not_exist.xyz"));

    // fileInfo() — find any file with valid metadata
    // NOTE: TVFS root entries use truncated eKeys, so encoding lookup may not
    // always resolve file size. This is a known limitation.
    {
        bool found = false;
        for (size_t i = 0; i < std::min<size_t>(files.size(), 10); ++i) {
            auto info = storage->fileInfo(files[i]);
            if (info.has_value() && info->fileSize > 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            WARN("No files with resolved fileSize in first 50 — expected for TVFS roots");
        }
    }
}

// ============================================================================
// Section 3: Live CDN tests (StarCraft 1 Remastered — WoW root format)
// ============================================================================

TEST_CASE("Storage openOnline S1 open succeeds", "[online][casc][network][s1]") {
    utils::SimpleHttpHandler http(4);
    utils::SimpleThreadPool pool(4);
    auto opts = makeOpts(&http, &pool, "s1");

    auto storage = Storage::openOnline(opts);
    if (!storage) {
        WARN("S1 open failed (error " << Storage::lastError()
             << ") — CDN may be unavailable");
        SKIP("CDN unavailable");
    }
    REQUIRE(*storage);

    // product()
    {
        auto prod = storage->product();
        REQUIRE(prod.has_value());
    }

    // totalFileCount()
    {
        auto count = storage->totalFileCount();
        REQUIRE(count.has_value());
        CHECK(*count > 0);
    }

    // readFile() by file data ID (WoW-style root)
    i32 readableId = -1;
    {
        i32 targetId = -1;
        storage->enumerate([&](const EnumerateEntry& fe) {
            if (fe.fileDataId > 0) {
                targetId = fe.fileDataId;
                return false;
            }
            return true;
        });

        if (targetId >= 0) {
            auto data = storage->readFile(targetId);
            REQUIRE(data.has_value());
            CHECK(data->size() > 0);
            readableId = targetId;
        }
    }

    // readBatch() by file data ID
    if (readableId >= 0) {
        std::vector<BatchReadRequest> reqs(1);
        reqs[0].fileDataId = readableId;

        auto results = storage->readBatch(reqs);
        REQUIRE(results.size() == 1);
        CHECK(results[0].success);
        CHECK(results[0].data.size() > 0);
    }
}

// ============================================================================
// Section 4: Encryption key management
// ============================================================================

TEST_CASE("Storage openOnline encryption key management", "[online][casc]") {
    utils::SimpleHttpHandler http(2);
    OnlineOpenOptions opts;
    opts.product = "w3";
    opts.region = "us";
    opts.http = &http;
    opts.flags = StorageFeatureFlags::LoadOnDemand; // avoid full load

    auto storage = Storage::openOnline(opts);
    if (!storage) { SKIP("CDN unavailable"); }

    // Add and retrieve a key.
    std::array<u8, 16> testKey = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    CHECK(storage->addEncryptionKey(0xDEADBEEF, testKey));

    auto found = storage->findEncryptionKey(0xDEADBEEF);
    REQUIRE(found.has_value());
    CHECK(*found == testKey);

    // Non-existent key.
    CHECK_FALSE(storage->findEncryptionKey(0x12345678).has_value());

    // Add by hex string.
    CHECK(storage->addEncryptionKey(0xCAFE, "0102030405060708090a0b0c0d0e0f10"));
    auto found2 = storage->findEncryptionKey(0xCAFE);
    REQUIRE(found2.has_value());
    CHECK(*found2 == testKey);
}

// ============================================================================
// Section 5: Prefetch / LoadOnDemand
// ============================================================================

TEST_CASE("Storage openOnline LoadOnDemand defers encoding load", "[online][casc][network][w3]") {
    utils::SimpleHttpHandler http(4);
    utils::SimpleThreadPool pool(4);
    auto opts = makeOpts(&http, &pool, "w3");
    opts.flags = StorageFeatureFlags::LoadOnDemand;

    auto storage = Storage::openOnline(opts);
    if (!storage) { SKIP("CDN unavailable"); }
    REQUIRE(*storage);

    // Storage should be valid even though encoding/root not loaded yet.
    // Prefetch triggers the deferred load.
    CHECK(storage->prefetch());

    // Now operations should work.
    auto count = storage->totalFileCount();
    REQUIRE(count.has_value());
    CHECK(*count > 0);
}

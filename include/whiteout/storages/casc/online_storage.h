// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file online_storage.h
 * @brief CDN-backed read-only CASC storage.
 *
 * Presents the same public read API as @ref Storage but fetches data from
 * Blizzard CDN servers on demand, optionally caching to a local directory.
 * HTTP transport is not implemented by the library — the user supplies an
 * @ref interfaces::HttpHandler implementation.
 */

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include <whiteout/storages/casc/types.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

class OnlineStorage {
public:
    OnlineStorage();
    ~OnlineStorage();
    OnlineStorage(OnlineStorage&&) noexcept;
    OnlineStorage& operator=(OnlineStorage&&) noexcept;

    // Non-copyable.
    OnlineStorage(const OnlineStorage&) = delete;
    OnlineStorage& operator=(const OnlineStorage&) = delete;

    // ── Construction ─────────────────────────────────────────────────

    /// Open an online CASC storage backed by CDN.
    /// Returns std::nullopt on failure (check lastError()).
    static std::optional<OnlineStorage> open(const OnlineOpenOptions& opts);

    void close();
    explicit operator bool() const noexcept;

    // ── Read operations (same surface as Storage) ────────────────────

    std::optional<std::vector<u8>> readFile(const std::string& cascPath) const;
    std::optional<std::vector<u8>> readFile(const std::string& cascPath,
                                            u32 localeFlags, u32 openFlags = 0) const;
    std::optional<std::vector<u8>> readFile(i32 fileId) const;
    std::optional<std::vector<u8>> readFile(i32 fileId,
                                            u32 localeFlags, u32 openFlags = 0) const;

    bool fileExists(const std::string& cascPath) const;
    bool fileExists(i32 fileId) const;

    std::optional<u64> fileSize(const std::string& cascPath) const;
    std::optional<u64> fileSize(i32 fileId) const;

    std::optional<FileFullInfo> fileInfo(const std::string& cascPath) const;
    std::optional<FileFullInfo> fileInfo(i32 fileId) const;

    void enumerate(std::function<bool(const EnumerateEntry&)> callback) const;
    std::vector<std::string> listFiles() const;
    std::vector<FindEntry> listEntries() const;
    std::optional<u32> totalFileCount() const;

    std::vector<BatchReadResult> readBatch(
        std::span<const BatchReadRequest> requests) const;

    std::optional<StorageProduct> product() const;

    // ── Encryption ───────────────────────────────────────────────────

    bool addEncryptionKey(u64 keyName, const std::array<u8, 16>& key);
    bool addEncryptionKey(u64 keyName, const std::string& keyHex);
    bool importKeysFromString(const std::string& keyList);
    bool importKeysFromFile(const std::string& keyFilePath);
    std::optional<std::array<u8, 16>> findEncryptionKey(u64 keyName) const;

    // ── Cache management ─────────────────────────────────────────────

    /// Flush pending cache writes to disk (no-op if no cacheDir).
    void flushCache();

    /// Prefetch the encoding table and root manifest into cache/memory.
    bool prefetch();

    static u32 lastError() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::storages::casc

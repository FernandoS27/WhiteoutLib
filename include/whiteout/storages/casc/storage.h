// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file storage.h
 * @brief CASC archive storage access and manipulation
 *
 * This file defines:
 * - Storage class (PImpl) for opening, reading, and writing CASC archives
 *   used by Blizzard games (WoW, Diablo III, WC3 Reforged, and others)
 * - File read / query / enumerate operations (thread-safe, shared lock)
 * - TACT encryption key management
 * - Batch read interface with optional parallel I/O via WorkerPool
 * - Write overlay with deferred persist-to-disk
 *
 * @example Basic usage
 * @code
 * auto storage = casc::Storage::open("C:/Games/Diablo III/Data");
 * if (storage) {
 *     auto data = storage->readFile("data/global/excel/items.txt");
 *     storage->enumerate([](const FindEntry& e) { return true; });
 * }
 * @endcode
 */

#pragma once

#if !defined(WHITEOUT_HAS_CASC)
#error \
    "<whiteout/storages/casc/storage.h> requires CASC support. Configure with -DWHITEOUT_ENABLE_CASC=ON and link against the whiteout_casc target."
#endif

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "types.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
} // namespace whiteout::interfaces

namespace whiteout::storages::casc {

/**
 * @brief RAII wrapper for CASC archive access
 *
 * Storage is the primary entry point for reading and writing CASC archives.
 * Use the static factory methods (`open`, `create`) to obtain an instance,
 * then read files by path or FileDataId, enumerate entries, or stage writes.
 *
 * All public methods are thread-safe: read operations acquire a shared
 * lock; write and persist operations acquire an exclusive lock.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide internals.
 */
class Storage {
public:
    /// Default constructor — creates an empty (invalid) storage.
    Storage();
    /// Destructor (defined in .cpp for incomplete type).
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    /// Move constructor.
    Storage(Storage&& other) noexcept;
    /// Move assignment.
    Storage& operator=(Storage&& other) noexcept;

    // ── Construction ─────────────────────────────────────────────────

    /**
     * @brief Open an existing local CASC storage.
     * @param path Path to the game's top-level directory (containing
     *             .build.info) or its Data subdirectory.
     * @param pool Optional WorkerPool for parallel I/O (non-owning).
     * @return A valid Storage, or std::nullopt on failure.
     */
    static std::optional<Storage> open(const std::string& path,
                                       interfaces::WorkerPool* pool = nullptr);

    /// @overload Open with diagnostic error message output.
    static std::optional<Storage> open(const std::string& path, std::string* error,
                                       interfaces::WorkerPool* pool = nullptr);

    /// @overload Open with locale mask.
    static std::optional<Storage> open(const std::string& path, u32 localeMask,
                                       interfaces::WorkerPool* pool = nullptr);

    /// @overload Open with full options.
    static std::optional<Storage> open(const OpenOptions& opts);

    /**
     * @brief Create a new empty storage in memory.
     *
     * No file is written to disk until save() is called.
     *
     * @param opts Creation options (product name, version, root format).
     * @param pool Optional WorkerPool for parallel I/O.
     * @return A valid empty Storage ready for writeFile() calls.
     */
    static Storage create(CreateOptions opts = {},
                          interfaces::WorkerPool* pool = nullptr);

    /// Release all resources and invalidate the storage.
    void close();

    /// @return True if the storage is open and valid.
    explicit operator bool() const noexcept;

    // ── Read operations ──────────────────────────────────────────────

    /// @return File contents, or std::nullopt if the path is not found.
    std::optional<std::vector<u8>> readFile(const std::string& cascPath) const;

    /// @overload Read a file by path with locale and open flags.
    std::optional<std::vector<u8>> readFile(const std::string& cascPath,
                                            u32 localeFlags, u32 openFlags = 0) const;

    /// @overload Read a file by WoW-style FileDataId.
    std::optional<std::vector<u8>> readFile(i32 fileId) const;

    /// @overload Read a file by FileDataId with locale and open flags.
    std::optional<std::vector<u8>> readFile(i32 fileId,
                                            u32 localeFlags, u32 openFlags = 0) const;

    /// @return True if the path resolves to a known file.
    bool fileExists(const std::string& cascPath) const;

    /// @overload Check existence by FileDataId.
    bool fileExists(i32 fileId) const;

    /// @return Uncompressed file size, or std::nullopt if not found.
    std::optional<u64> fileSize(const std::string& cascPath) const;
    /// @overload
    std::optional<u64> fileSize(i32 fileId) const;

    /// @return Full metadata for the file, or std::nullopt if not found.
    std::optional<FileFullInfo> fileInfo(const std::string& cascPath) const;
    /// @overload
    std::optional<FileFullInfo> fileInfo(i32 fileId) const;

    /**
     * @brief Enumerate all entries in the root manifest.
     * @param callback Invoked for each entry; return false to stop.
     *        The EnumerateEntry is a view — its path field is valid only
     *        during the callback invocation.
     */
    void enumerate(std::function<bool(const EnumerateEntry&)> callback) const;

    /**
     * @brief Enumerate entries whose path matches a wildcard mask.
     *
     * Supports CascLib-compatible wildcard patterns:
     * - `*` matches zero or more characters (including path separators)
     * - `?` matches exactly one character
     * - All other characters are matched literally (case-insensitive)
     *
     * The mask `"*"` is equivalent to the no-mask overload.
     *
     * @param mask    Wildcard pattern (e.g. `"*.dds"`, `"data\\global\\*"`).
     * @param callback Invoked for each matching entry; return false to stop.
     */
    void enumerate(const std::string& mask,
                   std::function<bool(const EnumerateEntry&)> callback) const;

    /// @return All known file paths.
    std::vector<std::string> listFiles() const;

    /// @return All entries with metadata.
    std::vector<FindEntry> listEntries() const;

    /// @return Total number of files in the root manifest.
    std::optional<u32> totalFileCount() const;

    /**
     * @brief Read multiple files in a single call.
     *
     * When a WorkerPool is configured, resolution, raw reads, and
     * BLTE decoding are overlapped across files via DAG scheduling.
     * Individual file failures do not affect other files in the batch.
     *
     * @param requests Array of file read requests.
     * @return One result per request, in the same order.
     */
    std::vector<BatchReadResult> readBatch(
        std::span<const BatchReadRequest> requests) const;

    /// @return Product identification info, or std::nullopt if unavailable.
    std::optional<StorageProduct> product() const;

    // ── Encryption ───────────────────────────────────────────────────

    /// Add a TACT encryption key (raw 16-byte array).
    bool addEncryptionKey(u64 keyName, const std::array<u8, 16>& key);

    /// @overload Add a TACT encryption key from hex string.
    bool addEncryptionKey(u64 keyName, const std::string& keyHex);

    /// Import encryption keys from a formatted string (one per line).
    bool importKeysFromString(const std::string& keyList);

    /// Import encryption keys from a file.
    bool importKeysFromFile(const std::string& keyFilePath);

    /// @return The encryption key for @p keyName, or std::nullopt if not found.
    std::optional<std::array<u8, 16>> findEncryptionKey(u64 keyName) const;

    // ── Write operations ─────────────────────────────────────────────

    /**
     * @brief Write a file by path.
     *
     * Data is stored in an in-memory overlay until save() is called.
     *
     * @param path CASC path for the new or updated file.
     * @param data File contents.
     * @param opts Write options (locale, content flags, compression).
     * @return True on success.
     */
    bool writeFile(const std::string& path, const std::vector<u8>& data,
                   WriteOptions opts = {});

    /// @overload Write a file by FileDataId.
    bool writeFile(i32 fileId, const std::vector<u8>& data,
                   WriteOptions opts = {});

    /// Mark a file for deletion (effective on next save).
    bool deleteFile(const std::string& path);
    /// @overload
    bool deleteFile(i32 fileId);

    /// Persist all pending changes to disk.
    bool save();
    /// @overload Persist to a specific output path.
    bool save(const std::string& path);

    /// Clear the in-memory decoded-data cache (container cache).
    /// Has no effect if no cache was configured via OpenOptions::memoryCacheSize.
    void flushCache();

    /// @return Last error code (thread-local).
    static u32 lastError() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::storages::casc

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file storage.h
 * @brief Unified CASC archive read access (local & online)
 *
 * This file defines:
 * - Storage class (PImpl) for reading CASC archives from local disk or CDN
 * - File read / query / enumerate operations (thread-safe, shared lock)
 * - TACT encryption key management
 * - Batch read interface with optional parallel I/O via WorkerPool
 *
 * For write operations (writeFile, deleteFile, save), see StorageWritable
 * in <whiteout/storages/casc/storage_writable.h>.
 *
 * @example Basic usage
 * @code
 * auto storage = casc::Storage::open("C:/Games/Diablo III/Data");
 * if (storage) {
 *     auto data = storage->readFile("data/global/excel/items.txt");
 *     storage->enumerate([](const EnumerateEntry& e) { return true; });
 * }
 * @endcode
 */

#pragma once

#if !defined(WHITEOUT_HAS_CASC)
#error                                                                                             \
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

class StorageWritable;

/**
 * @brief Unified read-only CASC storage (local disk or CDN)
 *
 * Storage is the primary entry point for reading CASC archives.
 * Use `open()` for local disk, `openOnline()` for CDN-backed access.
 * The same public read API works identically regardless of backing store.
 *
 * All public methods are thread-safe: read operations acquire a shared lock.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide internals.
 *
 * @see StorageWritable for write + persist operations.
 */
/// @bind methods, move_only, no_default_ctor, js_name=CascStorage
class Storage {
public:
    /// Destructor (defined in .cpp for incomplete type).
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    /// Move constructor.
    Storage(Storage&& other) noexcept;
    /// Move assignment.
    Storage& operator=(Storage&& other) noexcept;

    // ── Local construction ───────────────────────────────────────────

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
    /// @bind skip — `std::string* error` out-param needs a value-return shape.
    static std::optional<Storage> open(const std::string& path, std::string* error,
                                       interfaces::WorkerPool* pool = nullptr);

    /// @overload Open with locale mask.
    static std::optional<Storage> open(const std::string& path, u32 localeMask,
                                       interfaces::WorkerPool* pool = nullptr);

    /// @overload Open with full options.
    /// @bind skip — OpenOptions has unsupported field shapes (std::span,
    /// std::function). Use the (path, localeMask, pool) overloads for now.
    static std::optional<Storage> open(const OpenOptions& opts);

    // ── Online construction ──────────────────────────────────────────

    /**
     * @brief Open a CDN-backed CASC storage.
     *
     * Fetches data from Blizzard CDN servers on demand.
     * HTTP transport is user-supplied via OnlineOpenOptions::http.
     *
     * @param opts Online open options (product, region, http handler, etc.).
     * @return A valid Storage, or std::nullopt on failure.
     */
    /// @bind skip — OnlineOpenOptions has unsupported field shapes
    /// (HttpHandler*, std::function progress callback).
    static std::optional<Storage> openOnline(const OnlineOpenOptions& opts);

    /// Release all resources and invalidate the storage.
    void close();

    /// @return True if the storage is open and valid.
    explicit operator bool() const noexcept;

    // ── Capability queries ───────────────────────────────────────────

    /// @return True if this storage reads from local disk.
    bool isLocal() const noexcept;

    /// @return True if this storage reads from CDN.
    bool isOnline() const noexcept;

    /// @return True if this storage has a write overlay (StorageWritable).
    bool isWritable() const noexcept;

    /// @return The root manifest format, or RootFormat::Unknown.
    RootFormat rootFormat() const noexcept;

    // ── Read operations ──────────────────────────────────────────────

    /// @return File contents, or std::nullopt if the path is not found.
    std::optional<std::vector<u8>> readFile(const std::string& cascPath) const;

    /// @overload Read a file by path with locale and open flags.
    std::optional<std::vector<u8>> readFile(const std::string& cascPath, u32 localeFlags,
                                            u32 openFlags = 0) const;

    /// @overload Read a file by WoW-style FileDataId.
    std::optional<std::vector<u8>> readFile(i32 fileId, FileIdHint hint = FileIdHint::None) const;

    /// @overload Read a file by FileDataId with locale and open flags.
    std::optional<std::vector<u8>> readFile(i32 fileId, u32 localeFlags, u32 openFlags = 0,
                                            FileIdHint hint = FileIdHint::None) const;

    /// @return True if the path resolves to a known file.
    bool fileExists(const std::string& cascPath) const;

    /// @overload Check existence by FileDataId.
    bool fileExists(i32 fileId, FileIdHint hint = FileIdHint::None) const;

    /// @return Uncompressed file size, or std::nullopt if not found.
    std::optional<u64> fileSize(const std::string& cascPath) const;
    /// @overload
    std::optional<u64> fileSize(i32 fileId, FileIdHint hint = FileIdHint::None) const;

    /// @return Full metadata for the file, or std::nullopt if not found.
    /// @bind skip — FileFullInfo not yet bound (has std::array<u8,16> fields).
    std::optional<FileFullInfo> fileInfo(const std::string& cascPath) const;
    /// @overload
    /// @bind skip — see above.
    std::optional<FileFullInfo> fileInfo(i32 fileId, FileIdHint hint = FileIdHint::None) const;

    /**
     * @brief Enumerate all entries in the root manifest.
     * @param callback Invoked for each entry; return false to stop.
     *        The EnumerateEntry is a view — its path field is valid only
     *        during the callback invocation.
     */
    /// @bind skip — std::function param not auto-marshalled; use listFiles().
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
    /// @bind skip — std::function param not auto-marshalled.
    void enumerate(const std::string& mask,
                   std::function<bool(const EnumerateEntry&)> callback) const;

    /// @return All known file paths.
    std::vector<std::string> listFiles() const;

    /// @return All entries with metadata.
    /// @bind skip — returns vector<FindEntry>; vector<value_object>
    /// returns need codegen marshalling not yet implemented.
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
    /// @bind skip — vector<BatchReadRequest>/<BatchReadResult> need
    /// vector<value_object> marshalling.
    std::vector<BatchReadResult> readBatch(std::span<const BatchReadRequest> requests) const;

    /// @return Product identification info, or std::nullopt if unavailable.
    /// @bind skip — StorageProduct not yet bound.
    std::optional<StorageProduct> product() const;

    // ── Encryption ───────────────────────────────────────────────────

    /// Add a TACT encryption key (raw 16-byte array).
    /// @bind skip — std::array<u8,16> param not yet auto-marshalled;
    /// use the hex-string overload below.
    bool addEncryptionKey(u64 keyName, const std::array<u8, 16>& key);

    /// @overload Add a TACT encryption key from hex string.
    bool addEncryptionKey(u64 keyName, const std::string& keyHex);

    /// Import encryption keys from a formatted string (one per line).
    bool importKeysFromString(const std::string& keyList);

    /// Import encryption keys from a file.
    bool importKeysFromFile(const std::string& keyFilePath);

    /// @return The encryption key for @p keyName, or std::nullopt if not found.
    std::optional<std::array<u8, 16>> findEncryptionKey(u64 keyName) const;

    // ── Cache ────────────────────────────────────────────────────────

    /// Clear the in-memory decoded-data cache (container cache).
    void flushCache();

    /// Force every deferred load (encoding, root, VFS, index files,
    /// orphan bitvector) to resolve. Idempotent.
    bool prefetch();

    /// @return Last error code (thread-local).
    static u32 lastError() noexcept;

    /// @cond INTERNAL
    struct Impl; // defined in storage_impl.h (internal)
    /// @endcond

protected:
    friend class StorageWritable;

    /// Default constructor — creates an empty (invalid) storage.
    Storage();

    /// Protected constructor for use by StorageWritable and factories.
    explicit Storage(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::storages::casc

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file storage_writable.h
 * @brief Writable CASC storage with in-memory overlay and persist-to-disk.
 *
 * StorageWritable extends Storage with write operations:
 * - writeFile() stages data in an in-memory overlay
 * - deleteFile() marks files for removal
 * - save() persists the overlay + source to a new CASC storage on disk
 *
 * All read operations from Storage work identically, with overlay entries
 * taking priority over source data.
 *
 * @example
 * @code
 * auto storage = casc::StorageWritable::open(opts);
 * storage->writeFile("data/test.txt", testData);
 * storage->save("C:/Output");
 * @endcode
 */

#pragma once

#include <whiteout/storages/casc/storage.h>

namespace whiteout::storages::casc {

/**
 * @brief Writable CASC storage (read + write + save)
 *
 * Inherits all read operations from Storage.
 * Adds write overlay and persist-to-disk support.
 *
 * Only local-backed storages can be writable (CDN is read-only).
 */
/// @bind methods, move_only, no_default_ctor, js_name=CascStorageWritable,
/// extends=whiteout::storages::casc::Storage
class StorageWritable : public Storage {
public:
    StorageWritable();
    ~StorageWritable();

    StorageWritable(StorageWritable&&) noexcept;
    StorageWritable& operator=(StorageWritable&&) noexcept;

    StorageWritable(const StorageWritable&) = delete;
    StorageWritable& operator=(const StorageWritable&) = delete;

    // ── Construction ─────────────────────────────────────────────────

    /**
     * @brief Open an existing local CASC storage for reading and writing.
     * @param opts Open options (same as Storage::open).
     * @return A valid StorageWritable, or std::nullopt on failure.
     */
    /// @bind skip — OpenOptions unbindable (std::span, std::function fields).
    static std::optional<StorageWritable> open(const OpenOptions& opts);

    /// @overload Convenience: open by base path.
    static std::optional<StorageWritable> open(const std::string& basePath);

    /// @overload Convenience: open with error string and optional pool.
    /// @bind skip — out-param `std::string* errorString`.
    static std::optional<StorageWritable> open(const std::string& basePath,
                                               std::string* errorString,
                                               interfaces::WorkerPool* pool = nullptr);

    /**
     * @brief Create a new empty storage in memory.
     *
     * No file is written to disk until save() is called.
     *
     * @param opts Creation options (product name, version, root format).
     * @param pool Optional WorkerPool for parallel I/O.
     * @return A valid empty StorageWritable ready for writeFile() calls.
     */
    static StorageWritable create(CreateOptions opts = {}, interfaces::WorkerPool* pool = nullptr);

    // ── Write operations ─────────────────────────────────────────────

    /**
     * @brief Reserve a file-data-ID for a named asset.
     *
     * Allocates the next available file-data-ID and associates it with
     * @p name.  The interpretation of @p name depends on the root format:
     *
     * - **WoW / WoWTvfs**: @p name is a full CASC path
     *   (e.g. `"Base\\creatures\\beast\\beast.m2"`).
     * - **Diablo 3 / Diablo 4 / TVFS**: @p name is `"asset_name.ext"`, where
     *   the extension determines the SNO group.  A CoreTOC entry is created
     *   automatically.
     *
     * Returns @c std::nullopt if the name already exists in the root or in
     * a previous reservation.
     *
     * @code
     * auto id = storage.reserveFileId("my_beast.app");
     * if (id) storage.writeFile(*id, data);
     * @endcode
     */
    std::optional<u32> reserveFileId(const std::string& name);

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
    bool writeFile(const std::string& path, const std::vector<u8>& data, WriteOptions opts = {});

    /// @overload Write a file by FileDataId.
    bool writeFile(i32 fileId, const std::vector<u8>& data, WriteOptions opts = {},
                   FileIdHint hint = FileIdHint::None);

    /// Mark a file for deletion (effective on next save).
    bool deleteFile(const std::string& path);
    /// @overload
    bool deleteFile(i32 fileId, FileIdHint hint = FileIdHint::None);

    /// Persist all pending changes to disk (writes to the original location).
    bool save();
    /// @overload Persist to a specific output path.
    bool save(const std::string& path);
};

} // namespace whiteout::storages::casc

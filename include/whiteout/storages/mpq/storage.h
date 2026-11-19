// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file storage.h
 * @brief Public API for reading, writing, and creating MPQ archives
 *
 * Provides the Storage class — the primary entry point for all MPQ archive
 * operations.  Archives can be opened from disk, created in memory, queried,
 * modified, and persisted atomically back to disk.
 *
 * @example Basic usage
 * @code
 * auto storage = mpq::Storage::open("War3.mpq");
 * if (storage) {
 *     auto data = storage->readFile("war3map.j");
 *     storage->writeFile("patch.txt", bytes);
 *     storage->save();
 * }
 * @endcode
 */

#pragma once

#if !defined(WHITEOUT_HAS_MPQ)
#error                                                                                             \
    "<whiteout/storages/mpq/storage.h> requires MPQ support. Configure with -DWHITEOUT_ENABLE_MPQ=ON and link against " \
    "the whiteout_mpq target."
#endif

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
} // namespace whiteout::interfaces

namespace whiteout::storages::mpq {

/**
 * @brief RAII wrapper for MPQ archive access
 *
 * Provides full CRUD operations on MPQ archives.  Modifications are held
 * in a virtual overlay until save() is called, which writes a complete
 * new archive atomically (write to temp file, then rename).
 *
 * All public methods are thread-safe: read operations acquire a shared
 * lock; write and persist operations acquire an exclusive lock.
 *
 * @bind methods, no_default_ctor
 */
class Storage {
public:
    Storage();
    ~Storage();

    // Non-copyable.
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Movable.
    Storage(Storage&& other) noexcept;
    Storage& operator=(Storage&& other) noexcept;

    // -- Construction --

    /// Open an existing MPQ archive. Memory-maps the file and parses tables.
    /// A Warcraft III map (an HM3W block, or a .w3x or .w3m name) is read as
    /// the game reads one, so a protected map opens.
    /// @param path  Path to the .mpq file.
    /// @param pool  Optional WorkerPool for parallel compress/decompress (non-owning).
    /// @return A valid Storage, or std::nullopt on failure.
    static std::optional<Storage> open(const std::string& path,
                                       interfaces::WorkerPool* pool = nullptr);

    /// Open an existing MPQ archive with diagnostic output on failure.
    /// @param path  Path to the .mpq file.
    /// @param error If non-null, receives a description of the failure reason.
    /// @param pool  Optional WorkerPool for parallel compress/decompress (non-owning).
    /// @return A valid Storage, or std::nullopt on failure.
    static std::optional<Storage> open(const std::string& path, std::string* error,
                                       interfaces::WorkerPool* pool = nullptr);

    /// Create a new empty archive in memory. No file on disk until save(path).
    static Storage create(CreateOptions opts = {}, interfaces::WorkerPool* pool = nullptr);

    /// Release all resources. Same effect as letting the Storage go out of scope.
    void close();

    /// Check if the storage is valid (has been opened or created).
    explicit operator bool() const noexcept;

    // -- Read Operations (thread-safe, shared lock) --

    /// Read a file from the archive: its neutral-locale copy, as Storm and
    /// StormLib read a name (the last one, when a protected map lists the
    /// name twice). A name with '/' is looked for as Storm hashes it, slash
    /// kept, then with '\\'. A patch file reads as its patch data. Pending
    /// writes and deletes count as already saved.
    /// @return File contents, or std::nullopt if not found, deleted, or not
    ///         decodable to its full size.
    std::optional<std::vector<u8>> readFile(const std::string& name) const;

    /// Read a file, with optional error output for diagnosing failures.
    std::optional<std::vector<u8>> readFile(const std::string& name, std::string* error) const;

    /// Read a file's copy in @p locale, else its neutral copy.
    std::optional<std::vector<u8>> readFile(const std::string& name, u16 locale) const;

    /// Check if readFile(name) finds a file (including overlay).
    bool fileExists(const std::string& name) const;

    /// Get information about the file readFile(name) reads.
    std::optional<FileInfo> fileInfo(const std::string& name) const;

    /// Get summary information about the archive.
    ArchiveInfo archiveInfo() const;

    /// List the files (listfile) names that the archive holds, plus overlay
    /// additions, minus deletions. A file no (listfile) names is not listed.
    std::vector<std::string> listFiles() const;

    /// Enumerate all known filenames via callback.
    /// Callback returns true to continue, false to stop.
    ///
    /// @bind skip — std::function callbacks aren't auto-bindable across
    /// backends. Use listFiles() and iterate.
    void enumerate(std::function<bool(const std::string&)> callback) const;

    // -- Write Operations (thread-safe, exclusive lock) --

    /// Write or overwrite a file. Data is held in overlay until save(), which
    /// replaces every locale of the name with the ones written.
    /// @return true on success, false if the storage is not open or the data
    ///         is over 4 GiB (an MPQ file size is 32-bit).
    bool writeFile(const std::string& name, std::span<const u8> data, WriteOptions opts = {});

    /// Delete a file from the archive, every locale of it.
    /// @return true if the file was found (in source or overlay), false otherwise.
    bool deleteFile(const std::string& name);

    // -- Persist (thread-safe, exclusive lock) --

    /// Save the archive to its original path.
    /// @return false if this Storage was created via create() with no prior save(path).
    bool save();

    /// Save the archive to a specific path. After saving, the new file becomes
    /// the source archive and the overlay is cleared.
    ///
    /// The archive is written to an unused name beside @p path, then renamed
    /// over it, so a failed save leaves @p path as it was; past 128 MiB it is
    /// streamed rather than held in memory. Bytes before the MPQ header (a
    /// Warcraft III map's HM3W block) are kept, as are files no (listfile)
    /// names, and a v4 archive keeps its HET/BET tables and MD5s. A copy no
    /// lookup can reach (a protected map's decoy) is dropped. When every file
    /// has a name, the hash table is built anew, at most three quarters full.
    /// A save that cannot keep every file refuses instead: see lastError().
    bool save(const std::string& path);

    /// Why the last save() failed; empty after one that succeeded.
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace whiteout::storages::mpq

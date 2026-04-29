// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file storage_impl.h
/// @brief Unified Storage::Impl struct with composed local/online/write state.
///
/// Internal header — shared by storage_core.cpp, storage_local.cpp,
/// storage_online.cpp, and storage_writable.cpp.
#pragma once

#include "../codec/blte.h"
#include "../tables/config.h"
#include "../codec/crypto.h"
#include "data_source.h"
#include "storage_backend.h"
#include "../tables/encoding.h"
#include "../tables/index.h"
#include "key_utils.h"
#include "../cdn/memory_cache.h"
#include "../cdn/online_index.h"
#include "../cdn/cdn_cache.h"
#include "../cdn/cdn_fetcher.h"
#include "../cdn/online_data_source.h"
#include "local_data_source.h"
#include "../roots/root.h"
#include "writer.h"
#include "../../common/mapped_file.h"
#include "../../common/hex.h"
#include "../../common/string_utils.h"

#include <whiteout/storages/casc/storage.h>
#include <whiteout/interfaces.h>
#include <whiteout/sno/core_toc.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace whiteout::storages::casc {

// ============================================================================
// Thread-local error (defined in storage_core.cpp, used across all TUs)
// ============================================================================

extern thread_local u32 s_lastError;

enum ErrorCode : u32 {
    kOk = 0,
    kInvalidPath = 1,
    kBuildInfoNotFound = 2,
    kBuildConfigNotFound = 3,
    kCdnConfigNotFound = 4,
    kIndexLoadFailed = 5,
    kEncodingNotFound = 6,
    kEncodingDecodeFailed = 7,
    kRootNotFound = 8,
    kRootDecodeFailed = 9,
    kRootParseFailed = 10,
    kFileNotFound = 11,
    kFileDecodeFailed = 12,
    kNotValid = 13,
    kArchiveReadFailed = 14,
    kWriteFailed = 15,
    kSharingViolation = 16,

    // Online-specific (aligned with CascError public constants).
    kHttpRequestFailed    = CascError::HttpRequestFailed,    // 0x10
    kCdnServerUnavailable = CascError::CdnServerUnavailable, // 0x11
    kRemoteFileNotFound   = CascError::RemoteFileNotFound,   // 0x12
    kVersionInfoNotFound  = CascError::VersionInfoNotFound,  // 0x13
    kCdnInfoNotFound      = CascError::CdnInfoNotFound,     // 0x14
    kNoHttpHandler        = CascError::NoHttpHandler,        // 0x15

    kSaveFailed = 0x20,
};

// ============================================================================
// Overlay types (write support)
// ============================================================================

struct OverlayKey {
    std::string path;               ///< Normalized, lowercase, forward-slash.
    std::optional<u32> fileDataId;  ///< For WoW-style writes.
    FileIdHint hint = FileIdHint::None; ///< Sub-type hint for FileDataId lookups.

    bool operator==(const OverlayKey& o) const {
        return path == o.path && fileDataId == o.fileDataId && hint == o.hint;
    }
};

struct OverlayKeyHash {
    size_t operator()(const OverlayKey& k) const {
        size_t h = std::hash<std::string>{}(k.path);
        if (k.fileDataId)
            h ^= std::hash<u32>{}(*k.fileDataId) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<u8>{}(static_cast<u8>(k.hint)) + 0x517cc1b7 + (h << 6) + (h >> 2);
        return h;
    }
};

struct OverlayEntry {
    std::vector<u8> rawData;
    WriteOptions writeOptions;
};

// ============================================================================
// Composed state structs
// ============================================================================

/// Local-only state: disk paths, index table, memory-mapped archives.
struct LocalState {
    std::string basePath;   ///< Game root dir (parent of Data/).
    std::string dataPath;   ///< basePath + "/Data".
    IndexTable indexTable;
    std::vector<storages::common::MappedFile> dataArchives;
    std::unique_ptr<LocalDataSource> dataSource;

    /// Build a config file path from a 16-byte key.
    std::string configPath(std::span<const u8, 16> key) const {
        std::array<u8, 16> arr;
        std::memcpy(arr.data(), key.data(), 16);
        return configFilePath(dataPath, arr);
    }

    /// Discover and memory-map all data.XXX archives.
    bool mapArchives(std::string* error);
};

/// Online-only state: CDN infrastructure.
struct OnlineState {
    interfaces::HttpHandler* http = nullptr;
    std::vector<CdnServer> cdnServers;
    StorageProduct productInfo;
    OnlineIndexTable onlineIndex;
    OnlineIndexTable looseIndex;
    std::unique_ptr<CdnCache> cache;
    std::unique_ptr<CdnFetcher> fetcher;
    std::unique_ptr<OnlineDataSource> dataSource;
};

/// Write overlay (pending writes/deletes, flushed on save).
/// A reserved file-ID entry, created by reserveFileId().
struct FileIdReservation {
    std::string name;               ///< Original name passed by the caller.
    std::string enrichedPath;       ///< Full root path for the entry.
    sno::SnoGroup snoGroup = sno::SnoGroup::None;  ///< D3/D4: SNO group.
    std::string snoName;            ///< D3/D4: asset name (without extension).
    i32 snoId = 0;                  ///< D3/D4: allocated SNO ID.
};

struct WriteOverlay {
    std::unordered_map<OverlayKey, OverlayEntry, OverlayKeyHash> pendingWrites;
    std::unordered_set<OverlayKey, OverlayKeyHash> pendingDeletes;
    RootFormat requestedRootFormat = RootFormat::Tvfs;

    /// File-ID reservations: fileDataId → reservation details.
    std::unordered_map<u32, FileIdReservation> reservedFileIds;

    /// CoreTOC state for D3/D4 formats (lazily initialised on first reservation).
    std::unique_ptr<sno::CoreToc> coreToc;

    /// Next file-data-ID to allocate (0 = not yet initialised).
    u32 nextFileDataId = 0;
};

// ============================================================================
// Storage::Impl — unified implementation
// ============================================================================

struct Storage::Impl {
    interfaces::WorkerPool* pool = nullptr;
    u32 localeMask = 0;

    /// External listfile data (caller-owned, must outlive Storage).
    std::span<const u8> listfileData;

    BuildConfig buildConfig;
    CdnConfig cdnConfig;
    mutable EncodingTable encodingTable;
    mutable std::unique_ptr<RootManifest> root;
    KeyRing keyRing;

    /// Active data source — points into localState or onlineState.
    DataSource* dataSource = nullptr;

    /// Specialised resolution backend (replaces dataSource + memCache for hot paths).
    mutable std::unique_ptr<StorageBackend> backend;

    /// Composed state — exactly one of local/online is non-null after open.
    std::unique_ptr<LocalState> localState;
    std::unique_ptr<OnlineState> onlineState;
    std::unique_ptr<WriteOverlay> writeOverlay;

    mutable std::shared_mutex mutex;
    bool isValid = false;

    /// Optional decoded-data cache for container sub-entries.
    mutable std::unique_ptr<MemoryCache> memCache;

    /// LoadOnDemand: defer encoding + root loading until first access.
    bool deferMode = false;
    mutable std::once_flag deferOnce;
    mutable bool deferLoadOk = true;

    /// Pre-built bitvector: m_encodingReferenced[i] = true iff encoding entry i
    /// is referenced by at least one root entry. Used by enumerate() for orphans.
    mutable std::vector<bool> m_encodingReferenced;

    // ── State queries ────────────────────────────────────────────

    bool isLocal() const noexcept { return localState != nullptr; }
    bool isOnline() const noexcept { return onlineState != nullptr; }

    // ── Resolution helpers ───────────────────────────────────────

    /// CKey → encoding → index → BLTE decode → raw file data.
    std::vector<u8> resolveCKey(std::span<const u8, 16> cKey,
                                interfaces::WorkerPool* poolToUse = nullptr) const;

    /// EKey → index → BLTE decode → raw file data (skip encoding table).
    std::vector<u8> resolveEKey(std::span<const u8, 16> eKey,
                                interfaces::WorkerPool* poolToUse = nullptr) const;

    /// RootEntry → decoded file data (locale filter + container slicing).
    std::optional<std::vector<u8>> resolveRootEntry(
        const std::vector<const RootEntry*>& entries,
        u32 localeFlags) const;

    /// Resolve encoding entry from a RootEntry (tries CKey first, then EKey).
    const EncodingEntry* resolveEncoding(const RootEntry& re) const;

    /// Read a file (overlay check + root resolution).
    std::optional<std::vector<u8>> readFileResolved(
        const OverlayKey& key,
        const std::vector<const RootEntry*>& entries,
        u32 localeFlags) const;

    /// File info from pre-resolved entries.
    std::optional<FileFullInfo> fileInfoResolved(
        const std::vector<const RootEntry*>& entries) const;

    /// Lazily load encoding + root (thread-safe via call_once).
    bool ensureLoaded() const;

    /// Load encoding table and root manifest.
    bool loadEncodingAndRoot(std::span<const u8> prefetchedEncodingBlte = {}) const;
};

// ── VFS prefetch helpers (defined in storage_local.cpp / storage_online.cpp) ─

/// Parallel-resolve VFS sub-manifests using local archives + JobGroup.
std::unordered_map<u64, std::vector<u8>> prefetchVfsLocal(
    const Storage::Impl& impl,
    const std::vector<std::array<u8, 16>>& vfsEKeys,
    const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey);

/// Parallel-resolve VFS sub-manifests using async HTTP + WaitState.
std::unordered_map<u64, std::vector<u8>> prefetchVfsOnline(
    const Storage::Impl& impl,
    const std::vector<std::array<u8, 16>>& vfsEKeys,
    const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey);

} // namespace whiteout::storages::casc

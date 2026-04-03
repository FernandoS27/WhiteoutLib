// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/storages/casc/storage.h>

// Internal headers.
#include "config.h"
#include "crypto.h"
#include "encoding.h"
#include "index.h"
#include "blte.h"
#include "constants.h"
#include "key_utils.h"
#include "roots/root.h"
#include "roots/wow_root.h"
#include "roots/d3_root.h"
#include "roots/tvfs_root.h"
#include "roots/mndx_root.h"
#include "writer.h"
#include "../common/hex.h"
#include "../common/mapped_file.h"
#include "../common/md5.h"
#include "../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

namespace whiteout::storages::casc {

// ============================================================================
// Thread-local error
// ============================================================================

static thread_local u32 s_lastError = 0;

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
    kSaveFailed = 16,
};

// ============================================================================
// Overlay types
// ============================================================================

struct OverlayKey {
    std::string path;               // Normalized, lowercase, forward-slash.
    std::optional<u32> fileDataId;  // For WoW-style writes.

    bool operator==(const OverlayKey& o) const {
        return path == o.path && fileDataId == o.fileDataId;
    }
};

struct OverlayKeyHash {
    size_t operator()(const OverlayKey& k) const {
        size_t h = std::hash<std::string>{}(k.path);
        if (k.fileDataId)
            h ^= std::hash<u32>{}(*k.fileDataId) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct OverlayEntry {
    std::vector<u8> rawData;
    WriteOptions writeOptions;
};

// ============================================================================
// Impl
// ============================================================================

using storages::common::normalizeCascPath;

struct Storage::Impl {
    std::string basePath;        // Game root dir (parent of Data/).
    std::string dataPath;        // basePath + "/Data".
    interfaces::WorkerPool* pool = nullptr;
    u32 localeMask = 0;
    RootFormat requestedRootFormat = RootFormat::Tvfs; // From CreateOptions.

    BuildConfig buildConfig;
    CdnConfig cdnConfig;
    IndexTable indexTable;
    mutable EncodingTable encodingTable;
    mutable std::unique_ptr<RootManifest> root;
    KeyRing keyRing;

    // Memory-mapped data archives: data.000, data.001, etc.
    std::vector<storages::common::MappedFile> dataArchives;

    // Overlay (pending writes/deletes, flushed on save).
    std::unordered_map<OverlayKey, OverlayEntry, OverlayKeyHash> pendingWrites;
    std::unordered_set<OverlayKey, OverlayKeyHash> pendingDeletes;

    mutable std::shared_mutex mutex;
    bool isValid = false;

    // LoadOnDemand: defer encoding + root loading until first access.
    bool deferMode = false;
    mutable std::once_flag deferOnce;
    mutable bool deferLoadOk = true;

    // Pre-built bitvector: m_encodingReferenced[i] = true iff encoding entry i
    // is referenced by at least one root entry.  Built during loadEncodingAndRoot().
    // Used by enumerate() for orphan detection without a per-call hash set.
    mutable std::vector<bool> m_encodingReferenced;

    // ---- helpers ----

    /// Read raw BLTE data from an archive at a given offset.
    /// Returns a span into the memory-mapped archive (zero-copy).
    /// Handles the 30-byte archive entry header (EKey hash + encoded/decoded sizes + flags).
    std::span<const u8> readRawBlte(u32 archiveIndex, u32 offset, u32 encodedSize) const;

    /// Read raw BLTE data without the archive entry header (.index entries).
    /// Returns a span into the memory-mapped archive (zero-copy).
    std::span<const u8> readRawBlteDirect(u32 archiveIndex, u32 offset, u32 encodedSize) const;

    /// Read raw BLTE data for an index entry, with automatic fallback
    /// if the directBLTE flag turns out to be wrong.
    /// Returns a span into the memory-mapped archive (zero-copy).
    std::span<const u8> readBlteFromIndex(const IndexEntry& idx) const;

    /// Full pipeline: CKey → decoded file data.
    std::vector<u8> resolveCKey(std::span<const u8, 16> cKey, interfaces::WorkerPool* poolToUse = nullptr) const;

    /// Full pipeline: EKey → decoded file data (skips encoding lookup).
    std::vector<u8> resolveEKey(std::span<const u8, 16> eKey, interfaces::WorkerPool* poolToUse = nullptr) const;

    /// Full pipeline: RootEntry → decoded file data (applies locale filter).
    std::optional<std::vector<u8>> resolveRootEntry(const std::vector<const RootEntry*>& entries,
                                                     u32 localeFlags) const;

    /// Resolve encoding entry from a RootEntry (tries CKey first, then EKey).
    const EncodingEntry* resolveEncoding(const RootEntry& re) const;

    /// Discover and memory-map all data.XXX archives.
    bool mapArchives(std::string* error);

    /// Build the config path from a 16-byte key.
    std::string configPath(std::span<const u8, 16> key) const {
        std::array<u8, 16> arr;
        std::memcpy(arr.data(), key.data(), 16);
        return configFilePath(dataPath, arr);
    }

    /// Shared implementation for readFile by path or file ID.
    /// Must be called with the shared lock already held.
    std::optional<std::vector<u8>> readFileResolved(
        const OverlayKey& key,
        const std::vector<const RootEntry*>& entries,
        u32 localeFlags) const;

    /// Shared implementation for fileInfo by path or file ID.
    /// Must be called with the shared lock already held.
    std::optional<FileFullInfo> fileInfoResolved(const std::vector<const RootEntry*>& entries) const;

    /// Lazily load encoding table and root manifest (LoadOnDemand).
    /// Thread-safe, called before any operation that needs encoding/root.
    /// Returns true if data is ready, false if deferred load failed.
    bool ensureLoaded() const;

    /// Load encoding table and root manifest.
    /// Returns true on success.
    bool loadEncodingAndRoot() const;
};

// ============================================================================
// Impl helpers
// ============================================================================

std::span<const u8> Storage::Impl::readRawBlte(u32 archiveIndex, u32 offset, u32 encodedSize) const {
    if (archiveIndex >= dataArchives.size())
        return {};

    auto& archive = dataArchives[archiveIndex];
    if (!archive)
        return {};

    // Default: .idx entries have a 30-byte per-entry header before BLTE data.
    u64 dataOffset = u64(offset) + kArchiveEntryHeaderSize;
    if (encodedSize <= kArchiveEntryHeaderSize)
        return {};
    u32 dataSize = encodedSize - kArchiveEntryHeaderSize;

    if (dataOffset + dataSize > archive.size())
        return {};

    auto ptr = archive.ptr() + dataOffset;
    return std::span<const u8>(ptr, dataSize);
}

/// Read raw BLTE data from a data archive (.index variant: no per-entry header).
std::span<const u8> Storage::Impl::readRawBlteDirect(u32 archiveIndex, u32 offset, u32 encodedSize) const {
    if (archiveIndex >= dataArchives.size())
        return {};

    auto& archive = dataArchives[archiveIndex];
    if (!archive)
        return {};

    if (u64(offset) + encodedSize > archive.size())
        return {};

    auto ptr = archive.ptr() + offset;
    return std::span<const u8>(ptr, encodedSize);
}

std::span<const u8> Storage::Impl::readBlteFromIndex(const IndexEntry& idx) const {
    auto blteData = idx.directBLTE
        ? readRawBlteDirect(idx.archiveIndex, idx.archiveOffset, idx.encodedSize)
        : readRawBlte(idx.archiveIndex, idx.archiveOffset, idx.encodedSize);
    if (!blteData.empty()) {
        // Verify BLTE magic; retry with opposite method if wrong.
        static constexpr u8 kBlteMagicBytes[4] = {'B','L','T','E'};
        if (blteData.size() >= 4 &&
            std::memcmp(blteData.data(), kBlteMagicBytes, 4) != 0) {
            auto retry = idx.directBLTE
                ? readRawBlte(idx.archiveIndex, idx.archiveOffset, idx.encodedSize)
                : readRawBlteDirect(idx.archiveIndex, idx.archiveOffset, idx.encodedSize);
            if (!retry.empty()) blteData = retry;
        }
    }
    return blteData;
}

std::vector<u8> Storage::Impl::resolveCKey(std::span<const u8, 16> cKey, interfaces::WorkerPool* poolToUse) const {
    // Use 9-byte matching: TVFS roots truncate cKeys to eKeySize (9) bytes.
    auto encEntry = encodingTable.findByCKey(cKey, 9);
    if (!encEntry) return {};

    auto idxEntry = indexTable.find(eKeyTrunc(encEntry->eKey));
    if (!idxEntry) return {};

    auto blteData = readBlteFromIndex(*idxEntry);
    if (blteData.empty()) return {};

    // Decode without pool — resolveCKey may be called from within pool tasks
    // (VFS sub-manifest resolution, D3 sub-directory resolution), so passing
    // the pool here would cause nested-pool deadlock when all threads block
    // on inner BLTE frame-decode jobs that can never run.
    auto decoded = blteDecode(blteData, &keyRing, poolToUse);
    if (!decoded.success) return {};

    return std::move(decoded.data);
}

/// Resolve directly from an EKey (skip encoding table, used by TVFS entries).
std::vector<u8> Storage::Impl::resolveEKey(std::span<const u8, 16> eKey, interfaces::WorkerPool* poolToUse) const {
    auto idxEntry = indexTable.find(eKeyTrunc(eKey));
    if (!idxEntry) return {};

    auto blteData = readBlteFromIndex(*idxEntry);
    if (blteData.empty()) return {};

    // Decode without pool — see resolveCKey comment about nested-pool deadlock.
    auto decoded = blteDecode(blteData, &keyRing, poolToUse);
    if (!decoded.success) return {};

    return std::move(decoded.data);
}

std::optional<std::vector<u8>> Storage::Impl::resolveRootEntry(
    const std::vector<const RootEntry*>& entries, u32 localeFlags) const {
    if (entries.empty()) return std::nullopt;

    const RootEntry* best = selectBestEntry(entries, localeFlags);
    if (!best) return std::nullopt;

    // Resolve CKey or EKey → index entry in a single pass (no duplicate lookups).
    const IndexEntry* idxEntry = nullptr;
    if (!isZeroKey(best->cKey)) {
        auto encEntry = encodingTable.findByCKey(best->cKey, 9);
        if (encEntry)
            idxEntry = indexTable.find(eKeyTrunc(encEntry->eKey));
    }
    if (!idxEntry && !isZeroKey(best->eKey)) {
        idxEntry = indexTable.find(eKeyTrunc(best->eKey));
    }
    if (!idxEntry) return std::nullopt;

    // Read and decode BLTE data directly from the already-resolved index entry.
    auto blteData = readBlteFromIndex(*idxEntry);
    if (blteData.empty()) return std::nullopt;

    auto decoded = blteDecode(blteData, &keyRing, pool);
    if (!decoded.success) return std::nullopt;

    return std::move(decoded.data);
}

const EncodingEntry* Storage::Impl::resolveEncoding(const RootEntry& re) const {
    // TVFS roots truncate both cKey and eKey to eKeySize (9) bytes, zero-
    // padded to 16. Use truncated matching for both paths.
    if (!isZeroKey(re.cKey)) {
        auto enc = encodingTable.findByCKey(re.cKey, 9);
        if (enc) return enc;
    }
    if (!isZeroKey(re.eKey)) {
        return encodingTable.findByEKey(re.eKey, 9);
    }
    return nullptr;
}

bool Storage::Impl::mapArchives(std::string* error) {
    namespace fs = std::filesystem;

    // Find all data.XXX files.
    std::string dataSubdir = dataPath + "/data";
    if (!fs::exists(dataSubdir)) {
        if (error) *error = "Data subdirectory not found: " + dataSubdir;
        return false;
    }

    // Discover the max archive index.
    u32 maxIndex = 0;
    for (auto& entry : fs::directory_iterator(dataSubdir)) {
        auto name = entry.path().filename().string();
        if (name.size() >= 8 && name.substr(0, 5) == "data.") {
            try {
                u32 idx = std::stoul(name.substr(5));
                if (idx > maxIndex) maxIndex = idx;
            } catch (...) {}
        }
    }

    dataArchives.resize(maxIndex + 1);
    for (u32 i = 0; i <= maxIndex; ++i) {
        char archiveName[32];
        std::snprintf(archiveName, sizeof(archiveName), "data/data.%03u", i);
        std::string path = dataPath + "/" + archiveName;
        if (fs::exists(path)) {
            auto mapped = storages::common::MappedFile::open(path,
                storages::common::AccessHint::Random);
            if (mapped) {
                dataArchives[i] = std::move(*mapped);
            }
        }
    }

    return true;
}

std::optional<std::vector<u8>> Storage::Impl::readFileResolved(
    const OverlayKey& key,
    const std::vector<const RootEntry*>& entries,
    u32 localeFlags) const {

    // Check overlay first.
    if (auto it = pendingWrites.find(key); it != pendingWrites.end())
        return it->second.rawData;

    // Check if deleted.
    if (pendingDeletes.count(key)) {
        s_lastError = kFileNotFound;
        return std::nullopt;
    }

    if (!root) {
        s_lastError = kFileNotFound;
        return std::nullopt;
    }

    auto result = resolveRootEntry(entries, localeFlags != 0 ? localeFlags : localeMask);
    if (!result) {
        s_lastError = kFileNotFound;
        return std::nullopt;
    }
    return result;
}

std::optional<FileFullInfo> Storage::Impl::fileInfoResolved(
    const std::vector<const RootEntry*>& entries) const {

    if (entries.empty()) return std::nullopt;

    auto* re = entries[0];
    FileFullInfo info;
    info.cKey = re->cKey;
    info.localeFlags = re->localeFlags;
    info.contentFlags = re->contentFlags;
    info.fileDataId = static_cast<i32>(re->fileDataId);
    info.path = re->path;
    info.fileSize = re->fileSize; // pre-resolved during load

    auto encEntry = resolveEncoding(*re);
    if (encEntry) {
        info.eKey = encEntry->eKey;
        if (info.fileSize == 0)
            info.fileSize = encEntry->fileSize;
        if (isZeroKey(info.cKey))
            info.cKey = encEntry->cKey;
    } else {
        info.eKey = re->eKey;
    }
    return info;
}

// ============================================================================
// Deferred loading (LoadOnDemand)
// ============================================================================

bool Storage::Impl::ensureLoaded() const {
    if (!deferMode) return true;
    std::call_once(deferOnce, [this]() {
        deferLoadOk = loadEncodingAndRoot();
    });
    return deferLoadOk;
}

bool Storage::Impl::loadEncodingAndRoot() const {
    // Step 6: Resolve and parse encoding table.
    auto encodingIdx = indexTable.find(eKeyTrunc(buildConfig.encodingEKey));
    if (!encodingIdx) {
        s_lastError = kEncodingNotFound;
        return false;
    }

    auto encodingBlte = readRawBlte(encodingIdx->archiveIndex,
                                     encodingIdx->archiveOffset,
                                     encodingIdx->encodedSize);
    if (encodingBlte.empty()) {
        s_lastError = kEncodingNotFound;
        return false;
    }

    auto encodingDecoded = blteDecode(encodingBlte, &keyRing, pool);
    if (!encodingDecoded.success) {
        s_lastError = kEncodingDecodeFailed;
        return false;
    }

    encodingTable = EncodingTable::parse(encodingDecoded.data, pool);
    if (encodingTable.entryCount() == 0) {
        s_lastError = kEncodingDecodeFailed;
        return false;
    }

    // Step 7: Resolve and parse root manifest.
    CKeyResolver ckeyResolver = [this](std::span<const u8, 16> cKey) -> std::vector<u8> {
        return resolveCKey(cKey, pool);
    };

    bool hasVfs = false;
    for (auto b : buildConfig.vfsRootCKey) {
        if (b != 0) { hasVfs = true; break; }
    }
    if (hasVfs) {
        std::vector<std::array<u8, 16>> vfsEKeys;
        std::unordered_map<u64, std::array<u8, 16>> vfsEKeyToCKey;
        for (auto& sub : buildConfig.vfsSubManifests) {
            if (sub.cKey == buildConfig.vfsRootCKey) continue;
            vfsEKeys.push_back(sub.eKey);
            vfsEKeyToCKey[keyHash64(sub.eKey)] = sub.cKey;
        }

        // Pre-fetch all VFS sub-manifests in parallel so the resolver
        // during tree traversal is a cheap cache lookup instead of I/O.
        std::unordered_map<u64, std::vector<u8>> vfsCache;
        auto vfsData = resolveCKey(buildConfig.vfsRootCKey, pool);

        // Resolve sub-manifests in parallel if pool available.
        if (pool && vfsEKeys.size() > 1) {
            struct SubResult {
                u64 hash;
                std::vector<u8> data;
            };
            std::vector<SubResult> results(vfsEKeys.size());
            utils::JobGroup jobGroup;
            jobGroup.add(vfsEKeys.size());
            for (size_t i = 0; i < vfsEKeys.size(); ++i) {
                interfaces::WorkerTask task;
                task.fn = [&, i]() {
                    std::array<u8, 16> eKey16{};
                    std::memcpy(eKey16.data(), vfsEKeys[i].data(),
                                std::min(size_t(16), vfsEKeys[i].size()));
                    results[i].hash = keyHash64(eKey16);
                    results[i].data = resolveEKey(eKey16);
                    if (results[i].data.empty()) {
                        auto cIt = vfsEKeyToCKey.find(results[i].hash);
                        if (cIt != vfsEKeyToCKey.end())
                            results[i].data = resolveCKey(cIt->second);
                    }
                    jobGroup.done();
                };
                pool->submit(task);
            }
            jobGroup.wait();
            for (auto& r : results) {
                if (!r.data.empty())
                    vfsCache.emplace(r.hash, std::move(r.data));
            }
        } else {
            for (auto& ek : vfsEKeys) {
                std::array<u8, 16> eKey16{};
                std::memcpy(eKey16.data(), ek.data(), std::min(size_t(16), ek.size()));
                u64 h = keyHash64(eKey16);
                auto data = resolveEKey(eKey16);
                if (data.empty()) {
                    auto cIt = vfsEKeyToCKey.find(h);
                    if (cIt != vfsEKeyToCKey.end())
                        data = resolveCKey(cIt->second);
                }
                if (!data.empty())
                    vfsCache.emplace(h, std::move(data));
            }
        }

        // Resolver that returns from the pre-fetched cache.
        VfsResolver vfsResolver = [&vfsCache](std::span<const u8> eKey) -> std::vector<u8> {
            u64 h = keyHash64(eKey.data());
            auto it = vfsCache.find(h);
            if (it != vfsCache.end())
                return it->second;  // copy — sub-manifest may be needed multiple times
            return {};
        };

        std::unique_ptr<TvfsRoot> tvfsRoot;
        if (!vfsData.empty())
            tvfsRoot = TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, pool);
        if (tvfsRoot)
            root = std::move(tvfsRoot);
    }

    if (!root) {
        auto rootData = resolveCKey(buildConfig.rootCKey);
        if (rootData.empty()) {
            s_lastError = kRootNotFound;
            return false;
        }

        if (rootData.size() >= 4) {
            u32 magic = u32(rootData[0]) | (u32(rootData[1]) << 8) |
                        (u32(rootData[2]) << 16) | (u32(rootData[3]) << 24);

            if (magic == RootSignature::kTVFS) {
                root = TvfsRoot::parse(rootData, pool);
            } else if (magic == RootSignature::kMFST) {
                root = WowRoot::parse(rootData, pool);
            } else if (magic == RootSignature::kD3Root || magic == RootSignature::kD3Dir) {
                root = D3Root::parse(rootData, ckeyResolver, pool);
            } else if (magic == RootSignature::kMNDX) {
                root = MndxRoot::parse(rootData, pool);
            }
        }

        if (!root)
            root = WowRoot::parse(rootData, pool);
    }

    if (!root) {
        s_lastError = kRootParseFailed;
        return false;
    }

    // Pre-resolve file sizes (and fill in zero CKeys) from the encoding table.
    // Also build a bitvector marking which encoding entries are referenced by root,
    // so enumerate() can detect orphans without a per-call hash set.
    m_encodingReferenced.assign(encodingTable.entryCount(), false);
    const auto* encBase = encodingTable.entries().data();
    root->resolveEntries([this, encBase](RootEntry& e) {
        const EncodingEntry* enc = nullptr;
        if (!isZeroKey(e.cKey))
            enc = encodingTable.findByCKey(e.cKey, 9);
        if (!enc && !isZeroKey(e.eKey))
            enc = encodingTable.findByEKey(e.eKey, 9);
        if (enc) {
            e.fileSize = enc->fileSize;
            if (isZeroKey(e.cKey))
                e.cKey = enc->cKey;
            m_encodingReferenced[static_cast<size_t>(enc - encBase)] = true;
        }
    });

    return true;
}

// ============================================================================
// Storage construction / destruction
// ============================================================================

Storage::Storage() : m_impl(std::make_unique<Impl>()) {}
Storage::~Storage() = default;
Storage::Storage(Storage&&) noexcept = default;
Storage& Storage::operator=(Storage&&) noexcept = default;

void Storage::close() {
    if (m_impl) {
        std::unique_lock lock(m_impl->mutex);
        m_impl->isValid = false;
        m_impl->dataArchives.clear();
        m_impl->root.reset();
    }
}

Storage::operator bool() const noexcept {
    return m_impl && m_impl->isValid;
}

// ============================================================================
// open()
// ============================================================================

std::optional<Storage> Storage::open(const std::string& path,
                                     interfaces::WorkerPool* pool) {
    return open(path, static_cast<std::string*>(nullptr), pool);
}

std::optional<Storage> Storage::open(const std::string& path, u32 localeMask,
                                     interfaces::WorkerPool* pool) {
    OpenOptions opts;
    opts.path = path;
    opts.localeMask = localeMask;
    opts.pool = pool;
    return open(opts);
}

std::optional<Storage> Storage::open(const std::string& path, std::string* error,
                                     interfaces::WorkerPool* pool) {
    OpenOptions opts;
    opts.path = path;
    opts.pool = pool;
    return open(opts);
}

std::optional<Storage> Storage::open(const OpenOptions& opts) {
    namespace fs = std::filesystem;

    std::string error;
    auto setError = [&](const std::string& msg, ErrorCode code) -> std::optional<Storage> {
        s_lastError = code;
        // If caller provided callback, we can't pass error string through it.
        // Just set thread-local code.
        return std::nullopt;
    };

    // Determine basePath and dataPath.
    std::string basePath = opts.path;
    std::string dataPath;

    // If path points at a data directory directly, go up one level.
    auto leaf = fs::path(basePath).filename().string();
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), ::tolower);
    if (leaf == "data" || leaf == "sc2data" || leaf == "heroesdata") {
        dataPath = basePath;
        basePath = fs::path(basePath).parent_path().string();
    } else if (fs::exists(basePath + "/Data")) {
        dataPath = basePath + "/Data";
    } else if (fs::exists(basePath + "/data")) {
        dataPath = basePath + "/data";
    } else if (fs::exists(basePath + "/SC2Data")) {
        dataPath = basePath + "/SC2Data";
    } else if (fs::exists(basePath + "/HeroesData")) {
        dataPath = basePath + "/HeroesData";
    } else {
        // Maybe basePath IS the data dir (no separate "Data" subdir).
        dataPath = basePath;
    }

    // Normalize paths.
    basePath = fs::path(basePath).lexically_normal().string();
    dataPath = fs::path(dataPath).lexically_normal().string();

    // Progress callback helper.
    auto progress = [&](ProgressStep step, u32 current = 0, u32 total = 0) -> bool {
        if (opts.progressCallback) return opts.progressCallback(step, current, total);
        return true;
    };

    // Step 1: Parse .build.info.
    progress(ProgressStep::LoadingBuildConfig);

    std::string buildInfoPath;
    if (fs::exists(basePath + "/.build.info"))
        buildInfoPath = basePath + "/.build.info";
    else if (fs::exists(dataPath + "/.build.info"))
        buildInfoPath = dataPath + "/.build.info";

    if (buildInfoPath.empty())
        return setError(".build.info not found", kBuildInfoNotFound);

    auto buildInfoFile = storages::common::MappedFile::open(buildInfoPath);
    if (!buildInfoFile)
        return setError("Failed to open .build.info", kBuildInfoNotFound);

    auto builds = parseBuildInfo(buildInfoFile->data());
    if (builds.empty())
        return setError("No builds in .build.info", kBuildInfoNotFound);

    // Select active build (or by buildKey).
    const BuildInfo* activeBuild = nullptr;
    if (!opts.buildKey.empty()) {
        for (auto& b : builds) {
            if (storages::common::hexEncode16(b.buildKey) == opts.buildKey) {
                activeBuild = &b;
                break;
            }
        }
    }
    if (!activeBuild) {
        for (auto& b : builds) {
            if (b.active) { activeBuild = &b; break; }
        }
    }
    if (!activeBuild) activeBuild = &builds[0];

    // Step 2: Parse build config.
    Storage storage;
    auto& impl = *storage.m_impl;
    impl.basePath = basePath;
    impl.dataPath = dataPath;
    impl.pool = opts.pool;
    impl.localeMask = opts.localeMask;

    auto buildConfigPath = impl.configPath(activeBuild->buildKey);
    auto buildConfigFile = storages::common::MappedFile::open(buildConfigPath);
    if (!buildConfigFile)
        return setError("Build config not found: " + buildConfigPath, kBuildConfigNotFound);

    impl.buildConfig = parseBuildConfig(buildConfigFile->data());

    // Step 3: Parse CDN config (optional, not all storages have it).
    auto cdnConfigPath = impl.configPath(activeBuild->cdnKey);
    auto cdnConfigFile = storages::common::MappedFile::open(cdnConfigPath);
    if (cdnConfigFile) {
        impl.cdnConfig = parseCdnConfig(cdnConfigFile->data());
    }

    // Step 4: Load index table (.idx bucket files + per-archive .index files).
    progress(ProgressStep::LoadingIndexFiles);
    impl.indexTable = IndexTable::load(dataPath, opts.pool);

    // Also load per-archive .index files if CDN config has archive keys.
    if (!impl.cdnConfig.archiveEKeys.empty())
        impl.indexTable.loadArchiveIndices(dataPath, impl.cdnConfig.archiveEKeys, opts.pool);

    if (impl.indexTable.entryCount() == 0)
        return setError("No index entries found", kIndexLoadFailed);

    // Step 5: Memory-map data archives.
    progress(ProgressStep::MappingArchives);
    std::string mapError;
    if (!impl.mapArchives(&mapError))
        return setError(mapError, kIndexLoadFailed);

    // Step 6+7: Load encoding table and root manifest.
    // When LoadOnDemand is set, defer these expensive steps until first use.
    if (opts.flags & StorageFeatureFlags::LoadOnDemand) {
        impl.deferMode = true;
        impl.isValid = true;
        progress(ProgressStep::Ready);
        return storage;
    }

    progress(ProgressStep::LoadingEncodingTable);
    progress(ProgressStep::LoadingRootManifest);

    if (!impl.loadEncodingAndRoot())
        return setError("Failed to load encoding/root", static_cast<ErrorCode>(s_lastError));

    // Done.
    progress(ProgressStep::Ready);
    impl.isValid = true;

    return storage;
}

Storage Storage::create(CreateOptions opts, interfaces::WorkerPool* pool) {
    Storage storage;
    auto& impl = *storage.m_impl;
    impl.pool = pool;
    impl.buildConfig.buildProduct = opts.product;
    impl.buildConfig.buildName = opts.version;
    impl.requestedRootFormat = opts.rootFormat;
    impl.isValid = true; // Valid but empty — writes go to overlay.
    return storage;
}

// ============================================================================
// Read operations
// ============================================================================

std::optional<std::vector<u8>> Storage::readFile(const std::string& cascPath) const {
    return readFile(cascPath, 0, 0);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& cascPath,
                                                  u32 localeFlags, u32 /*openFlags*/) const {
    if (!m_impl || !m_impl->isValid) {
        s_lastError = kNotValid;
        return std::nullopt;
    }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return std::nullopt; }

    std::shared_lock lock(m_impl->mutex);
    auto normalized = normalizeCascPath(cascPath);
    OverlayKey key{normalized, std::nullopt};
    auto entries = m_impl->root ? m_impl->root->findByNormalizedPath(normalized) : std::vector<const RootEntry*>{};
    return m_impl->readFileResolved(key, entries, localeFlags);
}

std::optional<std::vector<u8>> Storage::readFile(i32 fileId) const {
    return readFile(fileId, 0, 0);
}

std::optional<std::vector<u8>> Storage::readFile(i32 fileId,
                                                  u32 localeFlags, u32 /*openFlags*/) const {
    if (!m_impl || !m_impl->isValid) {
        s_lastError = kNotValid;
        return std::nullopt;
    }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return std::nullopt; }

    std::shared_lock lock(m_impl->mutex);
    OverlayKey key{"", static_cast<u32>(fileId)};
    auto entries = m_impl->root ? m_impl->root->findByFileDataId(static_cast<u32>(fileId))
                                : std::vector<const RootEntry*>{};
    return m_impl->readFileResolved(key, entries, localeFlags);
}

// ============================================================================
// readBatch
// ============================================================================

std::vector<BatchReadResult> Storage::readBatch(
    std::span<const BatchReadRequest> requests) const {

    std::vector<BatchReadResult> results(requests.size());

    if (requests.empty())
        return results;

    if (!m_impl || !m_impl->isValid) {
        for (auto& r : results) r.error = "storage not valid";
        return results;
    }
    if (!m_impl->ensureLoaded()) {
        for (auto& r : results) r.error = "deferred load failed";
        return results;
    }

    std::shared_lock lock(m_impl->mutex);

    // Phase 0: Overlay check + root lookup for each request.
    // Determine which requests need BLTE resolution.
    struct ResolveWork {
        size_t requestIndex;         // Index into requests/results arrays.
        std::vector<const RootEntry*> rootEntries;
        u32 localeFlags;
    };
    std::vector<ResolveWork> toResolve;
    toResolve.reserve(requests.size());

    for (size_t i = 0; i < requests.size(); ++i) {
        auto& req = requests[i];
        bool byPath = !req.path.empty();
        bool byId = req.fileDataId != kInvalidId;

        if (!byPath && !byId) {
            results[i].error = "empty request (no path or fileDataId)";
            continue;
        }

        // Build overlay key.
        OverlayKey oKey;
        if (byPath)
            oKey = {normalizeCascPath(req.path), std::nullopt};
        else
            oKey = {"", static_cast<u32>(req.fileDataId)};

        // Check pending writes overlay.
        if (auto it = m_impl->pendingWrites.find(oKey); it != m_impl->pendingWrites.end()) {
            results[i].data = it->second.rawData;
            results[i].success = true;
            continue;
        }

        // Check pending deletes.
        if (m_impl->pendingDeletes.count(oKey)) {
            results[i].error = "file is pending deletion";
            continue;
        }

        // Root lookup.
        std::vector<const RootEntry*> entries;
        if (m_impl->root) {
            if (byPath)
                entries = m_impl->root->findByNormalizedPath(normalizeCascPath(req.path));
            else
                entries = m_impl->root->findByFileDataId(static_cast<u32>(req.fileDataId));
        }

        if (entries.empty()) {
            results[i].error = "file not found in root manifest";
            continue;
        }

        u32 locale = req.localeFlags != 0 ? req.localeFlags : m_impl->localeMask;
        toResolve.push_back({i, std::move(entries), locale});
    }

    if (toResolve.empty())
        return results;

    // Phase 1: Resolution — for each pending request, resolve root entry
    // through encoding + index tables, read raw BLTE blob.
    // Each slot writes to its own resolvedBlob — no cross-slot sharing.
    struct ResolvedBlob {
        std::span<const u8> blteData;
        bool resolved = false;
        std::string error;
    };
    std::vector<ResolvedBlob> resolvedBlobs(toResolve.size());

    auto resolveOne = [&](size_t idx) {
        auto& work = toResolve[idx];
        auto& blob = resolvedBlobs[idx];

        // Select best locale match.
        const RootEntry* best = selectBestEntry(work.rootEntries, work.localeFlags);
        if (!best) { blob.error = "no matching root entry"; return; }

        // Resolve CKey or EKey → index → raw BLTE.
        const IndexEntry* idxEntry = nullptr;
        if (!isZeroKey(best->cKey)) {
            auto encEntry = m_impl->encodingTable.findByCKey(best->cKey, 9);
            if (encEntry)
                idxEntry = m_impl->indexTable.find(eKeyTrunc(encEntry->eKey));
        }
        if (!idxEntry && !isZeroKey(best->eKey)) {
            idxEntry = m_impl->indexTable.find(eKeyTrunc(best->eKey));
        }

        if (!idxEntry) {
            blob.error = "file not found in index";
            return;
        }

        blob.blteData = m_impl->readBlteFromIndex(*idxEntry);
        if (blob.blteData.empty()) {
            blob.error = "failed to read raw BLTE data from archive";
            return;
        }
        blob.resolved = true;
    };

    // Parallel resolution when pool available and enough work.
    constexpr size_t kMinBatchParallel = 4;
    if (m_impl->pool && toResolve.size() >= kMinBatchParallel &&
        m_impl->pool->threadCount() > 0) {
        utils::JobGroup resolveGroup;
        resolveGroup.add(toResolve.size());
        for (size_t idx = 0; idx < toResolve.size(); ++idx) {
            interfaces::WorkerTask task;
            task.fn = [&, idx]() {
                resolveOne(idx);
                resolveGroup.done();
            };
            m_impl->pool->submit(task);
        }
        resolveGroup.wait();
    } else {
        for (size_t idx = 0; idx < toResolve.size(); ++idx)
            resolveOne(idx);
    }

    // Phase 2: Decode — collect resolved blobs and batch decode.
    // Build batch entries, mapping resolved indices to decode indices.
    std::vector<BlteBatchEntry> batchEntries;
    std::vector<size_t> batchToResolveIdx; // maps batch slot → toResolve slot
    batchEntries.reserve(toResolve.size());
    batchToResolveIdx.reserve(toResolve.size());

    for (size_t idx = 0; idx < toResolve.size(); ++idx) {
        if (!resolvedBlobs[idx].resolved) {
            // Resolution failed — propagate error to result.
            results[toResolve[idx].requestIndex].error = std::move(resolvedBlobs[idx].error);
            continue;
        }
        BlteBatchEntry entry;
        entry.blteData = resolvedBlobs[idx].blteData;
        batchEntries.push_back(entry);
        batchToResolveIdx.push_back(idx);
    }

    if (!batchEntries.empty()) {
        auto decoded = blteDecodeBatch(batchEntries, &m_impl->keyRing, m_impl->pool);

        // Phase 3: Map decode results back to output.
        for (size_t d = 0; d < decoded.size(); ++d) {
            size_t resolveIdx = batchToResolveIdx[d];
            size_t requestIdx = toResolve[resolveIdx].requestIndex;
            results[requestIdx].data = std::move(decoded[d].data);
            results[requestIdx].success = decoded[d].success;
            if (!decoded[d].success && !decoded[d].error.empty())
                results[requestIdx].error = std::move(decoded[d].error);
        }
    }

    return results;
}

bool Storage::fileExists(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return false;
    if (!m_impl->ensureLoaded()) return false;
    std::shared_lock lock(m_impl->mutex);

    auto normalized = normalizeCascPath(cascPath);

    // Fast path: skip overlay checks when there are no pending writes/deletes.
    if (!m_impl->pendingWrites.empty() || !m_impl->pendingDeletes.empty()) {
        OverlayKey key{normalized, std::nullopt};
        if (m_impl->pendingWrites.count(key)) return true;
        if (m_impl->pendingDeletes.count(key)) return false;
    }

    return m_impl->root && m_impl->root->hasPath(normalized);
}

bool Storage::fileExists(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return false;
    if (!m_impl->ensureLoaded()) return false;
    std::shared_lock lock(m_impl->mutex);

    // Fast path: skip overlay checks when there are no pending writes/deletes.
    if (!m_impl->pendingWrites.empty() || !m_impl->pendingDeletes.empty()) {
        OverlayKey key{"", static_cast<u32>(fileId)};
        if (m_impl->pendingWrites.count(key)) return true;
        if (m_impl->pendingDeletes.count(key)) return false;
    }

    return m_impl->root && m_impl->root->hasFileDataId(static_cast<u32>(fileId));
}

std::optional<u64> Storage::fileSize(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByNormalizedPath(normalizeCascPath(cascPath));
    if (entries.empty()) return std::nullopt;

    // fileSize is pre-resolved during load.
    if (entries[0]->fileSize > 0)
        return entries[0]->fileSize;

    // Fallback for entries not resolved at load time.
    auto encEntry = m_impl->resolveEncoding(*entries[0]);
    if (!encEntry) return std::nullopt;
    return encEntry->fileSize;
}

std::optional<u64> Storage::fileSize(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByFileDataId(static_cast<u32>(fileId));
    if (entries.empty()) return std::nullopt;

    // fileSize is pre-resolved during load.
    if (entries[0]->fileSize > 0)
        return entries[0]->fileSize;

    // Fallback for entries not resolved at load time.
    auto encEntry = m_impl->resolveEncoding(*entries[0]);
    if (!encEntry) return std::nullopt;
    return encEntry->fileSize;
}

std::optional<FileFullInfo> Storage::fileInfo(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->fileInfoResolved(m_impl->root->findByNormalizedPath(normalizeCascPath(cascPath)));
}

std::optional<FileFullInfo> Storage::fileInfo(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->fileInfoResolved(
        m_impl->root->findByFileDataId(static_cast<u32>(fileId)));
}

void Storage::enumerate(std::function<bool(const EnumerateEntry&)> callback) const {
    if (!m_impl || !m_impl->isValid || !callback) return;
    if (!m_impl->ensureLoaded()) return;
    std::shared_lock lock(m_impl->mutex);

    EnumerateEntry fe;
    m_impl->root->enumerate([&](const RootEntry& re) -> bool {
        fe.cKey = re.cKey;           // pre-resolved during load (zero CKeys filled in)
        fe.fileSize = re.fileSize;   // pre-resolved during load
        fe.localeFlags = re.localeFlags;
        fe.contentFlags = re.contentFlags;
        fe.fileDataId = static_cast<i32>(re.fileDataId);
        fe.path = re.path;          // string_view — zero copy

        return callback(fe);
    });

    // Emit encoding-table orphans (entries not represented in the root manifest).
    // Uses pre-built bitvector from loadEncodingAndRoot() instead of per-call hash set.
    static constexpr char kHex[] = "0123456789abcdef";
    const auto& encEntries = m_impl->encodingTable.entries();
    char hexBuf[33];
    for (size_t i = 0; i < encEntries.size(); ++i) {
        auto& enc = encEntries[i];
        if (isZeroKey(enc.cKey))
            continue;
        if (m_impl->m_encodingReferenced[i])
            continue;

        // Format CKey as 32-character hex string.
        for (int j = 0; j < 16; ++j) {
            hexBuf[j * 2]     = kHex[enc.cKey[j] >> 4];
            hexBuf[j * 2 + 1] = kHex[enc.cKey[j] & 0xF];
        }
        hexBuf[32] = '\0';

        fe.cKey = enc.cKey;
        fe.fileSize = enc.fileSize;
        fe.localeFlags = 0;
        fe.contentFlags = 0;
        fe.fileDataId = kInvalidId;
        fe.path = std::string_view(hexBuf, 32);

        if (!callback(fe))
            break;
    }
}

// ── Wildcard matcher (CascLib-compatible: * and ? only, case-insensitive) ──

namespace {

/// Lowercase + normalize slashes for a single character.
inline char normChar(char ch) noexcept {
    if (ch == '/') return '\\';
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch;
}

/// Pre-normalize a mask string: lowercase, '/' → '\\'.
static std::string normalizeMask(const std::string& mask) {
    std::string r;
    r.reserve(mask.size());
    for (char c : mask) r.push_back(normChar(c));
    return r;
}

/// Match @p pat (already normalized) against @p str (may have mixed case/slashes)
/// where `*` matches zero-or-more characters and `?` matches exactly one character.
static bool wildcardMatch(std::string_view pat, std::string_view str) noexcept {
    size_t pi = 0, si = 0;
    size_t starPat = std::string_view::npos;
    size_t starStr = 0;

    while (si < str.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == normChar(str[si]))) {
            ++pi;
            ++si;
        } else if (pi < pat.size() && pat[pi] == '*') {
            starPat = pi++;
            starStr = si;
        } else if (starPat != std::string_view::npos) {
            pi = starPat + 1;
            si = ++starStr;
        } else {
            return false;
        }
    }

    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

/// Return true if @p mask is the trivial "match-all" pattern.
static bool isTrivialMask(std::string_view mask) noexcept {
    for (char ch : mask) {
        if (ch != '*') return false;
    }
    return true; // empty or all-stars = match everything
}

/// Extract the literal prefix from a mask — the portion before the first
/// wildcard ('*' or '?'). Returns an empty view if the mask starts with a
/// wildcard. The prefix is truncated to the last path separator so it
/// represents a complete directory path suitable for enumerateUnder().
static std::string_view extractPrefix(std::string_view mask) noexcept {
    // Find first wildcard character.
    size_t wild = 0;
    while (wild < mask.size() && mask[wild] != '*' && mask[wild] != '?')
        ++wild;
    if (wild == 0) return {};
    // Truncate to last path separator (we need a complete directory component).
    size_t lastSep = mask.rfind('\\', wild - 1);
    if (lastSep == std::string_view::npos) return {};
    return mask.substr(0, lastSep + 1); // includes trailing backslash
}

/// Check if @p mask is a pure suffix pattern: exactly one leading '*' followed
/// by a literal suffix (no other wildcards). If so, returns the suffix
/// (including the leading dot if present). Otherwise returns an empty view.
static std::string_view extractPureSuffix(std::string_view mask) noexcept {
    if (mask.size() < 2 || mask[0] != '*') return {};
    // Check that the rest has no wildcards.
    for (size_t i = 1; i < mask.size(); ++i) {
        if (mask[i] == '*' || mask[i] == '?') return {};
    }
    return mask.substr(1); // e.g. ".dds"
}

/// Case-insensitive suffix check (characters already normalized).
static bool endsWithNorm(std::string_view str, std::string_view suffix) noexcept {
    if (str.size() < suffix.size()) return false;
    size_t off = str.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (normChar(str[off + i]) != suffix[i]) return false;
    }
    return true;
}

} // anonymous namespace

void Storage::enumerate(const std::string& mask,
                        std::function<bool(const EnumerateEntry&)> callback) const {
    if (!callback) return;
    if (!m_impl || !m_impl->isValid) return;
    if (!m_impl->ensureLoaded()) return;

    // Fast path: "*" or empty mask → delegate to the unfiltered overload.
    if (mask.empty() || isTrivialMask(mask)) {
        enumerate(std::move(callback));
        return;
    }

    // Pre-normalize the mask once (lowercase + slash normalization).
    std::string normMask = normalizeMask(mask);

    std::shared_lock lock(m_impl->mutex);

    // ── Strategy selection ────────────────────────────────────────────
    //
    // 1. Pure suffix pattern (e.g. "*.dds"): fast endsWith check.
    // 2. Has literal prefix (e.g. "campaigns\\*.ogg"): narrow via
    //    root->enumerateUnder(prefix), then apply wildcard/suffix on
    //    the narrowed set.
    // 3. General pattern: full scan + wildcardMatch.

    std::string_view pureSuffix = extractPureSuffix(normMask);
    std::string_view prefix = extractPrefix(normMask);
    // Strip trailing backslash for enumerateUnder (it expects directory name
    // without trailing separator).
    std::string prefixDir;
    if (!prefix.empty())
        prefixDir = std::string(prefix.substr(0, prefix.size() - 1));

    // Build the enumerate-and-filter lambda that applies the chosen match
    // strategy to each root entry.
    EnumerateEntry fe;
    auto rootCallback = [&](const RootEntry& re) -> bool {
        // Quick prefix rejection: if the pattern has a literal prefix, entries
        // that don't start with it can be skipped without full wildcardMatch.
        // Both prefix (from mask) and path may use mixed '/' and '\\', so
        // compare via normChar() for separator equivalence.
        if (!prefix.empty()) {
            if (re.path.size() < prefix.size()) return true;
            for (size_t i = 0; i < prefix.size(); ++i) {
                if (normChar(re.path[i]) != prefix[i]) return true;
            }
        }

        // Apply wildcard filter.
        bool match;
        if (!pureSuffix.empty()) {
            match = endsWithNorm(re.path, pureSuffix);
        } else {
            match = wildcardMatch(normMask, re.path);
        }
        if (!match) return true;

        fe.cKey = re.cKey;
        fe.fileSize = re.fileSize;
        fe.localeFlags = re.localeFlags;
        fe.contentFlags = re.contentFlags;
        fe.fileDataId = static_cast<i32>(re.fileDataId);
        fe.path = re.path;
        return callback(fe);
    };

    // Full scan — prefix rejection + wildcard/suffix is already fast.
    m_impl->root->enumerate(rootCallback);

    // Orphan entries (encoding-table entries without root paths) have
    // 32-char hex-string paths. Skip the orphan phase entirely when the
    // pattern cannot possibly match a hex string (suffix patterns with a
    // dot, prefix patterns, etc.). For general patterns, still check.
    bool canMatchOrphan = pureSuffix.empty() && prefix.empty();
    if (!canMatchOrphan) return;

    static constexpr char kHex[] = "0123456789abcdef";
    const auto& encEntries = m_impl->encodingTable.entries();
    char hexBuf[33];
    for (size_t i = 0; i < encEntries.size(); ++i) {
        auto& enc = encEntries[i];
        if (isZeroKey(enc.cKey)) continue;
        if (m_impl->m_encodingReferenced[i]) continue;

        for (int j = 0; j < 16; ++j) {
            hexBuf[j * 2]     = kHex[enc.cKey[j] >> 4];
            hexBuf[j * 2 + 1] = kHex[enc.cKey[j] & 0xF];
        }
        hexBuf[32] = '\0';

        std::string_view hexPath(hexBuf, 32);
        if (!wildcardMatch(normMask, hexPath)) continue;

        fe.cKey = enc.cKey;
        fe.fileSize = enc.fileSize;
        fe.localeFlags = 0;
        fe.contentFlags = 0;
        fe.fileDataId = kInvalidId;
        fe.path = hexPath;
        if (!callback(fe)) break;
    }
}

std::vector<std::string> Storage::listFiles() const {
    std::vector<std::string> result;
    if (!m_impl || !m_impl->isValid) return result;
    if (!m_impl->ensureLoaded()) return result;
    std::shared_lock lock(m_impl->mutex);

    m_impl->root->enumerate([&](const RootEntry& re) -> bool {
        if (!re.path.empty())
            result.push_back(re.path);
        return true;
    });
    return result;
}

std::vector<FindEntry> Storage::listEntries() const {
    std::vector<FindEntry> result;
    enumerate([&](const EnumerateEntry& fe) {
        FindEntry& out = result.emplace_back();
        out.cKey = fe.cKey;
        out.fileSize = fe.fileSize;
        out.localeFlags = fe.localeFlags;
        out.contentFlags = fe.contentFlags;
        out.fileDataId = fe.fileDataId;
        out.path = std::string(fe.path);
        return true;
    });
    return result;
}

std::optional<u32> Storage::totalFileCount() const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    return static_cast<u32>(m_impl->root->entryCount());
}

std::optional<StorageProduct> Storage::product() const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    StorageProduct prod;
    prod.name = m_impl->buildConfig.buildProduct;
    prod.version = m_impl->buildConfig.buildName;
    prod.buildId = m_impl->buildConfig.buildUid;
    return prod;
}

// ============================================================================
// Encryption key management
// ============================================================================

bool Storage::addEncryptionKey(u64 keyName, const std::array<u8, 16>& key) {
    if (!m_impl) return false;
    m_impl->keyRing.addKey(keyName, key);
    return true;
}

bool Storage::addEncryptionKey(u64 keyName, const std::string& keyHex) {
    if (!m_impl) return false;
    m_impl->keyRing.addKey(keyName, keyHex);
    return true;
}

bool Storage::importKeysFromString(const std::string& keyList) {
    if (!m_impl) return false;
    return m_impl->keyRing.importFromString(keyList);
}

bool Storage::importKeysFromFile(const std::string& keyFilePath) {
    if (!m_impl) return false;
    return m_impl->keyRing.importFromFile(keyFilePath);
}

std::optional<std::array<u8, 16>> Storage::findEncryptionKey(u64 keyName) const {
    if (!m_impl) return std::nullopt;
    auto key = m_impl->keyRing.findKey(keyName);
    if (key) return *key;
    return std::nullopt;
}

// ============================================================================
// Write operations
// ============================================================================

bool Storage::writeFile(const std::string& path, const std::vector<u8>& data,
                        WriteOptions opts) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);

    auto normalized = normalizeCascPath(path);
    OverlayKey key{normalized, std::nullopt};

    // Remove from pending deletes if present.
    m_impl->pendingDeletes.erase(key);

    // Insert or replace in pending writes.
    m_impl->pendingWrites[key] = OverlayEntry{data, opts};
    return true;
}

bool Storage::writeFile(i32 fileId, const std::vector<u8>& data,
                        WriteOptions opts) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);

    OverlayKey key{"", static_cast<u32>(fileId)};

    m_impl->pendingDeletes.erase(key);
    m_impl->pendingWrites[key] = OverlayEntry{data, opts};
    return true;
}

bool Storage::deleteFile(const std::string& path) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);

    auto normalized = normalizeCascPath(path);
    OverlayKey key{normalized, std::nullopt};

    // Remove from pending writes if present.
    m_impl->pendingWrites.erase(key);

    // Check if the file exists in source or overlay.
    bool existsInSource = m_impl->root && m_impl->root->hasPath(normalized);
    if (!existsInSource) {
        s_lastError = kFileNotFound;
        return false;
    }

    m_impl->pendingDeletes.insert(key);
    return true;
}

bool Storage::deleteFile(i32 fileId) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);

    OverlayKey key{"", static_cast<u32>(fileId)};

    m_impl->pendingWrites.erase(key);

    bool existsInSource = m_impl->root &&
                          m_impl->root->hasFileDataId(static_cast<u32>(fileId));
    if (!existsInSource) {
        s_lastError = kFileNotFound;
        return false;
    }

    m_impl->pendingDeletes.insert(key);
    return true;
}

// ============================================================================
// Save
// ============================================================================

bool Storage::save() {
    if (!m_impl || !m_impl->isValid) { s_lastError = kNotValid; return false; }
    return save(m_impl->basePath);
}

bool Storage::save(const std::string& outputPath) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);

    namespace fs = std::filesystem;

    // Build merged file list.
    std::vector<WriteEntry> entries;

    // 1. Enumerate source entries (skip deletes, skip entries overwritten by overlay).
    //    Two-pass approach: collect metadata first, then read BLTE blobs in parallel.
    struct PendingRead {
        size_t entryIndex;
        u32 archiveIndex;
        u32 archiveOffset;
        u32 encodedSize;
        bool directBLTE;
    };
    std::vector<PendingRead> pendingReads;

    if (m_impl->root) {
        m_impl->root->enumerate([&](const RootEntry& re) -> bool {
            // Check if this entry is in pendingDeletes.
            OverlayKey pathKey{normalizeCascPath(re.path), std::nullopt};
            if (m_impl->pendingDeletes.count(pathKey))
                return true; // skip deleted

            if (re.fileDataId != kInvalidFileDataId) {
                OverlayKey idKey{"", re.fileDataId};
                if (m_impl->pendingDeletes.count(idKey))
                    return true; // skip deleted by ID
            }

            // Check if overwritten by overlay (by path).
            if (!re.path.empty() && m_impl->pendingWrites.count(pathKey))
                return true; // skip — overlay entry takes precedence

            // Check if overwritten by overlay (by file data ID).
            if (re.fileDataId != kInvalidFileDataId) {
                OverlayKey idKey{"", re.fileDataId};
                if (m_impl->pendingWrites.count(idKey))
                    return true;
            }

            WriteEntry we;
            we.path = re.path;
            we.cKey = re.cKey;
            we.fileDataId = re.fileDataId;
            we.localeFlags = re.localeFlags;
            we.contentFlags = re.contentFlags;

            // Look up encoding to get EKey + index to get archive location.
            auto encEntry = m_impl->resolveEncoding(re);
            if (encEntry) {
                we.eKey = encEntry->eKey;
                we.fileSize = encEntry->fileSize;

                // Record archive location for deferred parallel read.
                auto idxEntry = m_impl->indexTable.find(eKeyTrunc(encEntry->eKey));
                if (idxEntry) {
                    pendingReads.push_back({entries.size(),
                        idxEntry->archiveIndex, idxEntry->archiveOffset,
                        idxEntry->encodedSize, idxEntry->directBLTE});
                }
            }

            entries.push_back(std::move(we));
            return true;
        });

        // Phase 2: Read pre-encoded BLTE blobs in parallel.
        if (m_impl->pool && pendingReads.size() > 1) {
            utils::JobGroup jobGroup;
            jobGroup.add(pendingReads.size());
            for (auto& pr : pendingReads) {
                interfaces::WorkerTask task;
                task.fn = [&, idx = pr.entryIndex, ai = pr.archiveIndex,
                           ao = pr.archiveOffset, es = pr.encodedSize,
                           direct = pr.directBLTE]() {
                    auto span = direct
                        ? m_impl->readRawBlteDirect(ai, ao, es)
                        : m_impl->readRawBlte(ai, ao, es);
                    entries[idx].encodedBlob.assign(span.begin(), span.end());
                    entries[idx].hasPreEncoded = true;
                    jobGroup.done();
                };
                m_impl->pool->submit(task);
            }
            jobGroup.wait();
        } else {
            for (auto& pr : pendingReads) {
                auto span = pr.directBLTE
                    ? m_impl->readRawBlteDirect(pr.archiveIndex, pr.archiveOffset, pr.encodedSize)
                    : m_impl->readRawBlte(pr.archiveIndex, pr.archiveOffset, pr.encodedSize);
                entries[pr.entryIndex].encodedBlob.assign(span.begin(), span.end());
                entries[pr.entryIndex].hasPreEncoded = true;
            }
        }
    }

    // 2. Add overlay entries (new/modified files).
    for (auto& [key, overlay] : m_impl->pendingWrites) {
        WriteEntry we;
        we.path = key.path;
        if (key.fileDataId) we.fileDataId = *key.fileDataId;
        we.rawData = overlay.rawData;
        we.localeFlags = overlay.writeOptions.localeFlags;
        we.contentFlags = overlay.writeOptions.contentFlags;
        we.compress = overlay.writeOptions.compress;
        we.hasPreEncoded = false;
        entries.push_back(std::move(we));
    }

    // 3. Determine the root format.
    RootFormat rootFmt = m_impl->requestedRootFormat;
    if (m_impl->root)
        rootFmt = m_impl->root->format();

    // Auto-assign fileDataIds for new entries when format uses them.
    if (rootFmt == RootFormat::Wow || rootFmt == RootFormat::Diablo3) {
        u32 maxId = 0;
        for (auto& e : entries) {
            if (e.fileDataId != kInvalidFileDataId && e.fileDataId > maxId)
                maxId = e.fileDataId;
        }
        for (auto& e : entries) {
            if (e.fileDataId == kInvalidFileDataId) {
                e.fileDataId = ++maxId;
            }
        }
    }

    // 4. Determine writer options.
    WriterOptions writerOpts;
    // Use build config product info if available.
    writerOpts.product = m_impl->buildConfig.buildProduct.empty()
                             ? "custom" : m_impl->buildConfig.buildProduct;
    writerOpts.version = m_impl->buildConfig.buildName.empty()
                             ? "1.0.0" : m_impl->buildConfig.buildName;
    writerOpts.rootFormat = rootFmt;

    // 5. Write to a temporary directory, then rename.
    std::string tempDir = outputPath + ".tmp_save";

    // Clean up any leftover temp dir.
    std::error_code ec;
    fs::remove_all(tempDir, ec);

    bool ok = writeStorage(tempDir, entries, writerOpts, m_impl->pool);
    if (!ok) {
        fs::remove_all(tempDir, ec);
        s_lastError = kSaveFailed;
        return false;
    }

    // 6. Atomic replace: rename temp → output.
    // On Windows, can't atomically rename over existing dir, so remove first.
    // Must release memory-mapped files before renaming the directory.
    m_impl->dataArchives.clear();
    m_impl->root.reset();
    m_impl->isValid = false;

    if (outputPath != m_impl->basePath) {
        // Saving to a new location — just rename.
        fs::rename(tempDir, outputPath, ec);
        if (ec) {
            fs::remove_all(tempDir, ec);
            s_lastError = kSaveFailed;
            return false;
        }
    } else {
        // Saving in-place: rename current to .old, rename temp to current, remove .old.
        std::string oldDir = outputPath + ".old_save";
        fs::remove_all(oldDir, ec);
        fs::rename(outputPath, oldDir, ec);
        if (ec) {
            // Can't rename — try direct copy fallback.
            fs::remove_all(tempDir, ec);
            s_lastError = kSaveFailed;
            return false;
        }
        fs::rename(tempDir, outputPath, ec);
        if (ec) {
            // Restore original.
            fs::rename(oldDir, outputPath, ec);
            s_lastError = kSaveFailed;
            return false;
        }
        fs::remove_all(oldDir, ec);
    }

    // 7. Clear overlay and reload.
    m_impl->pendingWrites.clear();
    m_impl->pendingDeletes.clear();

    // Re-open from the output path.
    auto reopened = Storage::open(outputPath, m_impl->pool);
    if (reopened) {
        // Transfer state (can't move into self, so swap Impl internals).
        m_impl->basePath = reopened->m_impl->basePath;
        m_impl->dataPath = reopened->m_impl->dataPath;
        m_impl->buildConfig = std::move(reopened->m_impl->buildConfig);
        m_impl->cdnConfig = std::move(reopened->m_impl->cdnConfig);
        m_impl->indexTable = std::move(reopened->m_impl->indexTable);
        m_impl->encodingTable = std::move(reopened->m_impl->encodingTable);
        m_impl->root = std::move(reopened->m_impl->root);
        m_impl->dataArchives = std::move(reopened->m_impl->dataArchives);
        m_impl->isValid = true;
    }

    return true;
}

u32 Storage::lastError() noexcept {
    return s_lastError;
}

} // namespace whiteout::storages::casc

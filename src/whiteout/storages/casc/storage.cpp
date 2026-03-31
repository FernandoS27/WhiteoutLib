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
#include "roots/root.h"
#include "roots/wow_root.h"
#include "roots/d3_root.h"
#include "roots/d4_root.h"
#include "roots/tvfs_root.h"
#include "writer.h"
#include "../common/hex.h"
#include "../common/mapped_file.h"
#include "../common/md5.h"

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

/// Normalize a CASC path: lowercase, backslashes (CascLib convention), strip leading/trailing separators.
static std::string normalizePath(const std::string& input) {
    std::string result = input;
    for (auto& c : result) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // Strip leading separators.
    while (!result.empty() && result.front() == '\\') result.erase(result.begin());
    // Strip trailing separators.
    while (!result.empty() && result.back() == '\\') result.pop_back();
    return result;
}

/// 16-byte zero sentinel — used to detect EKey-only TVFS entries.
static constexpr std::array<u8, 16> kZeroKey{};

struct Storage::Impl {
    std::string basePath;        // Game root dir (parent of Data/).
    std::string dataPath;        // basePath + "/Data".
    interfaces::WorkerPool* pool = nullptr;
    u32 localeMask = 0;
    RootFormat requestedRootFormat = RootFormat::Tvfs; // From CreateOptions.

    BuildConfig buildConfig;
    CdnConfig cdnConfig;
    IndexTable indexTable;
    EncodingTable encodingTable;
    std::unique_ptr<RootManifest> root;
    KeyRing keyRing;

    // Memory-mapped data archives: data.000, data.001, etc.
    std::vector<storages::common::MappedFile> dataArchives;

    // Overlay (pending writes/deletes, flushed on save).
    std::unordered_map<OverlayKey, OverlayEntry, OverlayKeyHash> pendingWrites;
    std::unordered_set<OverlayKey, OverlayKeyHash> pendingDeletes;

    mutable std::shared_mutex mutex;
    bool isValid = false;

    // ---- helpers ----

    /// Read raw BLTE data from an archive at a given offset.
    /// Handles the 30-byte archive entry header (EKey hash + encoded/decoded sizes + flags).
    std::vector<u8> readRawBlte(u32 archiveIndex, u32 offset, u32 encodedSize) const;

    /// Read raw BLTE data without the archive entry header (.index entries).
    std::vector<u8> readRawBlteDirect(u32 archiveIndex, u32 offset, u32 encodedSize) const;

    /// Full pipeline: CKey → decoded file data.
    std::vector<u8> resolveCKey(std::span<const u8, 16> cKey) const;

    /// Full pipeline: EKey → decoded file data (skips encoding lookup).
    std::vector<u8> resolveEKey(std::span<const u8, 16> eKey) const;

    /// Full pipeline: RootEntry → decoded file data (applies locale filter).
    std::optional<std::vector<u8>> resolveRootEntry(const std::vector<RootEntry>& entries,
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
        const std::vector<RootEntry>& entries,
        u32 localeFlags) const;

    /// Shared implementation for fileInfo by path or file ID.
    /// Must be called with the shared lock already held.
    std::optional<FileFullInfo> fileInfoResolved(const std::vector<RootEntry>& entries) const;
};

// ============================================================================
// Impl helpers
// ============================================================================

std::vector<u8> Storage::Impl::readRawBlte(u32 archiveIndex, u32 offset, u32 encodedSize) const {
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
    return std::vector<u8>(ptr, ptr + dataSize);
}

/// Read raw BLTE data from a data archive (.index variant: no per-entry header).
std::vector<u8> Storage::Impl::readRawBlteDirect(u32 archiveIndex, u32 offset, u32 encodedSize) const {
    if (archiveIndex >= dataArchives.size())
        return {};

    auto& archive = dataArchives[archiveIndex];
    if (!archive)
        return {};

    if (u64(offset) + encodedSize > archive.size())
        return {};

    auto ptr = archive.ptr() + offset;
    return std::vector<u8>(ptr, ptr + encodedSize);
}

std::vector<u8> Storage::Impl::resolveCKey(std::span<const u8, 16> cKey) const {
    auto encEntry = encodingTable.findByCKey(cKey);
    if (!encEntry) return {};

    auto idxEntry = indexTable.find(std::span<const u8>(encEntry->eKey.data(), 9));
    if (!idxEntry) return {};

    auto blteData = idxEntry->directBLTE
        ? readRawBlteDirect(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize)
        : readRawBlte(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize);
    if (blteData.empty()) return {};

    auto decoded = blteDecode(blteData, &keyRing, pool);
    if (!decoded.success) {
        // Fallback: try the other read method (see resolveEKey comment).
        blteData = idxEntry->directBLTE
            ? readRawBlte(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize)
            : readRawBlteDirect(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize);
        if (blteData.empty()) return {};
        decoded = blteDecode(blteData, &keyRing, pool);
        if (!decoded.success) return {};
    }

    return std::move(decoded.data);
}

/// Resolve directly from an EKey (skip encoding table, used by TVFS entries).
std::vector<u8> Storage::Impl::resolveEKey(std::span<const u8, 16> eKey) const {
    auto idxEntry = indexTable.find(std::span<const u8>(eKey.data(), 9));
    if (!idxEntry) return {};

    auto blteData = idxEntry->directBLTE
        ? readRawBlteDirect(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize)
        : readRawBlte(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize);
    if (blteData.empty()) return {};

    auto decoded = blteDecode(blteData, &keyRing, pool);
    if (!decoded.success) {
        // Fallback: try the other read method.  D4 .idx entries sometimes
        // reference data without the 30-byte archive entry header, so the
        // directBLTE flag from the first index that stored the entry may be
        // wrong.  Retry with the opposite method before giving up.
        blteData = idxEntry->directBLTE
            ? readRawBlte(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize)
            : readRawBlteDirect(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize);
        if (blteData.empty()) return {};
        decoded = blteDecode(blteData, &keyRing, pool);
        if (!decoded.success) return {};
    }

    return std::move(decoded.data);
}

std::optional<std::vector<u8>> Storage::Impl::resolveRootEntry(
    const std::vector<RootEntry>& entries, u32 localeFlags) const {
    if (entries.empty()) return std::nullopt;

    // Select best matching entry based on locale.
    const RootEntry* best = nullptr;
    for (auto& e : entries) {
        // Skip entries marked "do not load".
        if (e.contentFlags & ContentFlags::DoNotLoad) continue;
        // If locale filter is set, check match.
        if (localeFlags != 0 && e.localeFlags != 0 && (e.localeFlags & localeFlags) == 0)
            continue;
        best = &e;
        break;
    }
    if (!best) best = &entries[0]; // fallback

    // First verify the entry can be located in the index.
    const IndexEntry* idxEntry = nullptr;
    if (best->cKey != kZeroKey) {
        auto encEntry = encodingTable.findByCKey(best->cKey);
        if (encEntry)
            idxEntry = indexTable.find(std::span<const u8>(encEntry->eKey.data(), 9));
    } else if (best->eKey != kZeroKey) {
        idxEntry = indexTable.find(std::span<const u8>(best->eKey.data(), 9));
    }
    if (!idxEntry) return std::nullopt;

    // Resolve the data. TVFS entries may have only an EKey (cKey is zeroed).
    std::vector<u8> data;
    if (best->cKey != kZeroKey) {
        data = resolveCKey(best->cKey);
    } else {
        data = resolveEKey(best->eKey);
    }
    // Return data even if empty (valid empty file).
    return data;
}

const EncodingEntry* Storage::Impl::resolveEncoding(const RootEntry& re) const {
    if (re.cKey != kZeroKey) {
        auto enc = encodingTable.findByCKey(re.cKey);
        if (enc) return enc;
    }
    if (re.eKey != kZeroKey) {
        // EKey-only entries (TVFS) use 9-byte truncated keys.
        // Always compare exactly 9 bytes to avoid false matches
        // when the key contains embedded zeros.
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
    const std::vector<RootEntry>& entries,
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
        // For D4Root, combined meta entries have zero CKeys — their data is
        // stored in a synthetic cache inside the root itself.
        if (auto* d4 = dynamic_cast<D4Root*>(root.get())) {
            for (auto& e : entries) {
                if (e.fileDataId == kInvalidFileDataId) continue;
                auto* cached = d4->findCombinedMeta(static_cast<i32>(e.fileDataId));
                if (cached)
                    return std::vector<u8>(*cached);
            }
        }
        s_lastError = kFileNotFound;
        return std::nullopt;
    }
    return result;
}

std::optional<FileFullInfo> Storage::Impl::fileInfoResolved(
    const std::vector<RootEntry>& entries) const {

    if (entries.empty()) return std::nullopt;

    auto& re = entries[0];
    FileFullInfo info;
    info.cKey = re.cKey;
    info.localeFlags = re.localeFlags;
    info.contentFlags = re.contentFlags;
    info.fileDataId = static_cast<i32>(re.fileDataId);
    info.path = re.path;

    auto encEntry = resolveEncoding(re);
    if (encEntry) {
        info.eKey = encEntry->eKey;
        info.fileSize = encEntry->fileSize;
        info.cKey = encEntry->cKey;
    } else {
        info.eKey = re.eKey;
    }
    return info;
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

    // If path points at Data/ directly, go up one level.
    auto leaf = fs::path(basePath).filename().string();
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), ::tolower);
    if (leaf == "data") {
        dataPath = basePath;
        basePath = fs::path(basePath).parent_path().string();
    } else if (fs::exists(basePath + "/Data")) {
        dataPath = basePath + "/Data";
    } else if (fs::exists(basePath + "/data")) {
        dataPath = basePath + "/data";
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

    // Step 6: Resolve and parse encoding table.
    progress(ProgressStep::LoadingEncodingTable);

    auto encodingIdx = impl.indexTable.find(
        std::span<const u8>(impl.buildConfig.encodingEKey.data(), 9));
    if (!encodingIdx)
        return setError("Encoding EKey not found in index", kEncodingNotFound);

    auto encodingBlte = impl.readRawBlte(encodingIdx->archiveIndex,
                                          encodingIdx->archiveOffset,
                                          encodingIdx->encodedSize);
    if (encodingBlte.empty())
        return setError("Failed to read encoding data from archive", kEncodingNotFound);

    auto encodingDecoded = blteDecode(encodingBlte, &impl.keyRing, opts.pool);
    if (!encodingDecoded.success)
        return setError("Encoding BLTE decode failed: " + encodingDecoded.error,
                        kEncodingDecodeFailed);

    impl.encodingTable = EncodingTable::parse(encodingDecoded.data);
    if (impl.encodingTable.entryCount() == 0)
        return setError("Encoding table is empty", kEncodingDecodeFailed);

    // Step 7: Resolve and parse root manifest.
    progress(ProgressStep::LoadingRootManifest);

    // For D3 roots, we need a resolver for sub-directory CKeys.
    // The resolver avoids inner pool parallelism to prevent contention
    // when D3Root::parse dispatches resolver calls on the same pool.
    CKeyResolver resolver = [&impl](std::span<const u8, 16> cKey) -> std::vector<u8> {
        auto encEntry = impl.encodingTable.findByCKey(cKey);
        if (!encEntry) return {};
        auto idxEntry = impl.indexTable.find(
            std::span<const u8>(encEntry->eKey.data(), 9));
        if (!idxEntry) return {};
        auto blteData = impl.readRawBlte(idxEntry->archiveIndex,
                                          idxEntry->archiveOffset,
                                          idxEntry->encodedSize);
        if (blteData.empty()) return {};
        auto decoded = blteDecode(blteData, &impl.keyRing, nullptr);
        if (!decoded.success) return {};
        return std::move(decoded.data);
    };

    // If a VFS root key is available (WC3 Reforged), parse the TVFS root with
    // sub-container resolution.  The resolver + known VFS EKeys allow the parser
    // to recursively descend into sub-manifests and prepend the container prefix
    // (e.g. "war3.w3mod:"), matching the paths CascLib reports.
    bool hasVfs = false;
    for (auto b : impl.buildConfig.vfsRootCKey) {
        if (b != 0) { hasVfs = true; break; }
    }
    if (hasVfs) {
        // Build the set of known VFS sub-manifest EKeys and a lookup map
        // for CKey-based fallback resolution.
        std::vector<std::array<u8, 16>> vfsEKeys;
        std::unordered_map<u64, std::array<u8, 16>> vfsEKeyToCKey;
        for (auto& sub : impl.buildConfig.vfsSubManifests) {
            // Skip the entry that duplicates vfs-root.
            if (sub.cKey == impl.buildConfig.vfsRootCKey) continue;
            vfsEKeys.push_back(sub.eKey);
            // Map first 8 bytes of EKey → CKey for fallback resolution.
            u64 h = 0;
            std::memcpy(&h, sub.eKey.data(), 8);
            vfsEKeyToCKey[h] = sub.cKey;
        }

        // Resolver: given a truncated EKey (eKeySize bytes), look it up in the
        // index, read the BLTE archive data, and decode it.
        // Falls back to CKey-based resolution via encoding table if direct
        // EKey lookup fails (handles cases where the data is in local archives
        // but indexed differently).
        VfsResolver vfsResolver = [&impl, &vfsEKeyToCKey](std::span<const u8> eKey) -> std::vector<u8> {
            std::array<u8, 16> eKey16{};
            std::memcpy(eKey16.data(), eKey.data(), std::min(eKey.size(), size_t(16)));

            // Try direct EKey resolution first.
            auto result = impl.resolveEKey(eKey16);
            if (!result.empty())
                return result;

            // Fallback: match EKey to a known VFS sub-manifest and resolve via CKey.
            u64 h = 0;
            std::memcpy(&h, eKey.data(), std::min(eKey.size(), size_t(8)));
            auto it = vfsEKeyToCKey.find(h);
            if (it != vfsEKeyToCKey.end()) {
                auto ckResult = impl.resolveCKey(it->second);
                if (!ckResult.empty())
                    return ckResult;
            }

            return {};
        };

        // Parse the vfs-root manifest with recursive sub-container resolution.
        auto vfsData = impl.resolveCKey(impl.buildConfig.vfsRootCKey);
        std::unique_ptr<TvfsRoot> tvfsRoot;
        if (!vfsData.empty())
            tvfsRoot = TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, opts.pool);

        if (tvfsRoot) {
            // Detect Diablo IV: enhance TVFS with CoreTOC SNO data.
            bool isFenris = impl.buildConfig.buildUid.starts_with("fenris") ||
                            impl.buildConfig.buildProduct.starts_with("fenris") ||
                            impl.buildConfig.buildProduct == "Fenris";

            if (isFenris) {
                auto tocEntries = tvfsRoot->findByPath("base:CoreTOC.dat");
                if (tocEntries.empty())
                    tocEntries = tvfsRoot->findByPath("base:coretoc.dat");

                std::vector<u8> tocData;
                for (auto& te : tocEntries) {
                    if (te.cKey != std::array<u8, 16>{})
                        tocData = impl.resolveCKey(te.cKey);
                    if (tocData.empty() && te.eKey != std::array<u8, 16>{})
                        tocData = impl.resolveEKey(te.eKey);
                    if (!tocData.empty()) break;
                }

                if (!tocData.empty()) {
                    sno::CoreToc coreToc;
                    if (coreToc.parse(tocData)) {
                        // Create a file resolver for combined meta loading.
                        FileResolver resolver = [&impl](const RootEntry& re) -> std::vector<u8> {
                            std::vector<u8> data;
                            if (re.cKey != std::array<u8, 16>{})
                                data = impl.resolveCKey(re.cKey);
                            if (data.empty() && re.eKey != std::array<u8, 16>{})
                                data = impl.resolveEKey(re.eKey);
                            return data;
                        };
                        auto d4Root = D4Root::create(
                            std::move(tvfsRoot), std::move(coreToc),
                            resolver, opts.pool);
                        if (d4Root)
                            impl.root = std::move(d4Root);
                    }
                }
            }

            // Fallback: use plain TVFS if D4 enhancement didn't apply.
            if (!impl.root && tvfsRoot)
                impl.root = std::move(tvfsRoot);
        }
    }

    // If VFS root didn't work, try rootCKey.
    if (!impl.root) {
        auto rootData = impl.resolveCKey(impl.buildConfig.rootCKey);
        if (rootData.empty())
            return setError("Failed to resolve root file", kRootNotFound);

        if (rootData.size() >= 4) {
            u32 magic = u32(rootData[0]) | (u32(rootData[1]) << 8) |
                        (u32(rootData[2]) << 16) | (u32(rootData[3]) << 24);

            if (magic == RootSignature::kTVFS) {
                impl.root = TvfsRoot::parse(rootData, opts.pool);
            } else if (magic == RootSignature::kMFST) {
                impl.root = WowRoot::parse(rootData, opts.pool);
            } else if (magic == RootSignature::kD3Root || magic == RootSignature::kD3Dir) {
                impl.root = D3Root::parse(rootData, resolver, opts.pool);
            }
        }

        // Try headerless WoW (legacy).
        if (!impl.root)
            impl.root = WowRoot::parse(rootData, opts.pool);
    }

    if (!impl.root)
        return setError("Failed to parse root manifest", kRootParseFailed);

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

    std::shared_lock lock(m_impl->mutex);
    auto normalized = normalizePath(cascPath);
    OverlayKey key{normalized, std::nullopt};
    auto entries = m_impl->root ? m_impl->root->findByPath(normalized) : std::vector<RootEntry>{};
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

    std::shared_lock lock(m_impl->mutex);
    OverlayKey key{"", static_cast<u32>(fileId)};
    auto entries = m_impl->root ? m_impl->root->findByFileDataId(static_cast<u32>(fileId))
                                : std::vector<RootEntry>{};
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

    std::shared_lock lock(m_impl->mutex);

    // Phase 0: Overlay check + root lookup for each request.
    // Determine which requests need BLTE resolution.
    struct ResolveWork {
        size_t requestIndex;         // Index into requests/results arrays.
        std::vector<RootEntry> rootEntries;
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
            oKey = {normalizePath(req.path), std::nullopt};
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
        std::vector<RootEntry> entries;
        if (m_impl->root) {
            if (byPath)
                entries = m_impl->root->findByPath(normalizePath(req.path));
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
        std::vector<u8> blteData;
        bool resolved = false;
        std::string error;
    };
    std::vector<ResolvedBlob> resolvedBlobs(toResolve.size());

    auto resolveOne = [&](size_t idx) {
        auto& work = toResolve[idx];
        auto& blob = resolvedBlobs[idx];

        // Select best locale match.
        const RootEntry* best = nullptr;
        for (auto& e : work.rootEntries) {
            if (e.contentFlags & ContentFlags::DoNotLoad) continue;
            if (work.localeFlags != 0 && e.localeFlags != 0 &&
                (e.localeFlags & work.localeFlags) == 0)
                continue;
            best = &e;
            break;
        }
        if (!best) best = &work.rootEntries[0];

        // Resolve CKey or EKey → index → raw BLTE.
        const IndexEntry* idxEntry = nullptr;
        if (best->cKey != kZeroKey) {
            auto encEntry = m_impl->encodingTable.findByCKey(best->cKey);
            if (encEntry)
                idxEntry = m_impl->indexTable.find(
                    std::span<const u8>(encEntry->eKey.data(), 9));
        } else if (best->eKey != kZeroKey) {
            idxEntry = m_impl->indexTable.find(
                std::span<const u8>(best->eKey.data(), 9));
        }

        if (!idxEntry) {
            blob.error = "file not found in index";
            return;
        }

        blob.blteData = idxEntry->directBLTE
            ? m_impl->readRawBlteDirect(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize)
            : m_impl->readRawBlte(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize);
        if (blob.blteData.empty()) {
            blob.error = "failed to read raw BLTE data from archive";
            return;
        }
        // Verify BLTE magic; retry with opposite method if wrong.
        static constexpr u8 kBlteMagic[4] = {'B','L','T','E'};
        if (blob.blteData.size() >= 4 &&
            std::memcmp(blob.blteData.data(), kBlteMagic, 4) != 0) {
            auto retry = idxEntry->directBLTE
                ? m_impl->readRawBlte(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize)
                : m_impl->readRawBlteDirect(idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize);
            if (!retry.empty() && retry.size() >= 4 &&
                std::memcmp(retry.data(), kBlteMagic, 4) == 0)
                blob.blteData = std::move(retry);
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
    std::shared_lock lock(m_impl->mutex);

    auto normalized = normalizePath(cascPath);
    OverlayKey key{normalized, std::nullopt};

    // Check overlay writes.
    if (m_impl->pendingWrites.count(key)) return true;
    // Check deletes.
    if (m_impl->pendingDeletes.count(key)) return false;

    return m_impl->root && !m_impl->root->findByPath(normalized).empty();
}

bool Storage::fileExists(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return false;
    std::shared_lock lock(m_impl->mutex);

    OverlayKey key{"", static_cast<u32>(fileId)};
    if (m_impl->pendingWrites.count(key)) return true;
    if (m_impl->pendingDeletes.count(key)) return false;

    return m_impl->root && !m_impl->root->findByFileDataId(static_cast<u32>(fileId)).empty();
}

std::optional<u64> Storage::fileSize(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByPath(normalizePath(cascPath));
    if (entries.empty()) return std::nullopt;

    auto encEntry = m_impl->resolveEncoding(entries[0]);
    if (!encEntry) return std::nullopt;
    return encEntry->fileSize;
}

std::optional<u64> Storage::fileSize(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByFileDataId(static_cast<u32>(fileId));
    if (entries.empty()) return std::nullopt;

    auto encEntry = m_impl->resolveEncoding(entries[0]);
    if (!encEntry) return std::nullopt;
    return encEntry->fileSize;
}

std::optional<FileFullInfo> Storage::fileInfo(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->fileInfoResolved(m_impl->root->findByPath(normalizePath(cascPath)));
}

std::optional<FileFullInfo> Storage::fileInfo(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->fileInfoResolved(
        m_impl->root->findByFileDataId(static_cast<u32>(fileId)));
}

void Storage::enumerate(std::function<bool(const FindEntry&)> callback) const {
    if (!m_impl || !m_impl->isValid || !callback) return;
    std::shared_lock lock(m_impl->mutex);

    m_impl->root->enumerate([&](const RootEntry& re) -> bool {
        FindEntry fe;
        fe.cKey = re.cKey;
        fe.localeFlags = re.localeFlags;
        fe.contentFlags = re.contentFlags;
        fe.fileDataId = static_cast<i32>(re.fileDataId);
        fe.path = re.path;

        auto encEntry = m_impl->resolveEncoding(re);
        if (encEntry) {
            fe.fileSize = encEntry->fileSize;
            fe.cKey = encEntry->cKey; // fill in CKey from encoding if root had only EKey
        }

        return callback(fe);
    });
}

std::vector<std::string> Storage::listFiles() const {
    std::vector<std::string> result;
    if (!m_impl || !m_impl->isValid) return result;
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
    enumerate([&](const FindEntry& fe) {
        result.push_back(fe);
        return true;
    });
    return result;
}

std::optional<u32> Storage::totalFileCount() const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
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

    auto normalized = normalizePath(path);
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
    std::unique_lock lock(m_impl->mutex);

    auto normalized = normalizePath(path);
    OverlayKey key{normalized, std::nullopt};

    // Remove from pending writes if present.
    m_impl->pendingWrites.erase(key);

    // Check if the file exists in source or overlay.
    bool existsInSource = m_impl->root && !m_impl->root->findByPath(normalized).empty();
    if (!existsInSource) {
        s_lastError = kFileNotFound;
        return false;
    }

    m_impl->pendingDeletes.insert(key);
    return true;
}

bool Storage::deleteFile(i32 fileId) {
    if (!m_impl) { s_lastError = kNotValid; return false; }
    std::unique_lock lock(m_impl->mutex);

    OverlayKey key{"", static_cast<u32>(fileId)};

    m_impl->pendingWrites.erase(key);

    bool existsInSource = m_impl->root &&
                          !m_impl->root->findByFileDataId(static_cast<u32>(fileId)).empty();
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
            OverlayKey pathKey{normalizePath(re.path), std::nullopt};
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
                auto idxEntry = m_impl->indexTable.find(
                    std::span<const u8>(encEntry->eKey.data(), 9));
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
                    entries[idx].encodedBlob = direct
                        ? m_impl->readRawBlteDirect(ai, ao, es)
                        : m_impl->readRawBlte(ai, ao, es);
                    entries[idx].hasPreEncoded = true;
                    jobGroup.done();
                };
                m_impl->pool->submit(task);
            }
            jobGroup.wait();
        } else {
            for (auto& pr : pendingReads) {
                entries[pr.entryIndex].encodedBlob = pr.directBLTE
                    ? m_impl->readRawBlteDirect(pr.archiveIndex, pr.archiveOffset, pr.encodedSize)
                    : m_impl->readRawBlte(pr.archiveIndex, pr.archiveOffset, pr.encodedSize);
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

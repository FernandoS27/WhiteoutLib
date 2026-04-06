// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file storage_core.cpp
/// @brief Shared Storage implementation: resolution, read, enumerate, encryption.

#include "storage_impl.h"
#include "storage_backend_impl.h"
#include "constants.h"
#include "../roots/wow_root.h"
#include "../roots/d3_root.h"
#include "../roots/tvfs_root.h"
#include "../roots/mndx_root.h"
#include "../roots/d4_root.h"
#include "../roots/wow_tvfs_root.h"
#include "../roots/ow_root.h"
#include "../roots/s1_root.h"
#include "../roots/generic_root.h"
#include "../roots/install_root.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace whiteout::storages::casc {

// ============================================================================
// Thread-local error (single definition)
// ============================================================================

thread_local u32 s_lastError = 0;

using storages::common::normalizeCascPath;

// ============================================================================
// Product-based TVFS decorator classification
// ============================================================================

/// Which decorator to apply on top of a plain TvfsRoot.
enum class TvfsDecorator : u8 {
    None,     ///< Plain TVFS (WC3 Reforged, etc.)
    Diablo4,  ///< D4Root — enrich with CoreTOC paths.
    WowTvfs,  ///< WowTvfsRoot — extract FileDataId/locale/content from encoded paths.
};

/// Classify the TVFS decorator from the build config product identifiers.
/// Uses buildUid (CDN product code) first, then buildProduct (display name).
/// Falls back to TvfsDecorator::None if neither matches a known product.
static TvfsDecorator classifyTvfsProduct(const BuildConfig& cfg) {
    // --- Check buildUid (CDN product code, e.g. "wow", "fenris") ---
    if (!cfg.buildUid.empty()) {
        std::string uid = cfg.buildUid;
        std::transform(uid.begin(), uid.end(), uid.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // D4 family: fenris, fenrist, fenrisdev, etc.
        if (uid.starts_with("fenris"))
            return TvfsDecorator::Diablo4;

        // WoW family: wow, wowt, wow_beta, wow_classic, wow_classic_era, wowlivetest, etc.
        if (uid.starts_with("wow"))
            return TvfsDecorator::WowTvfs;
    }

    // --- Fallback: check buildProduct (display name, e.g. "WoW", "Fenris") ---
    if (!cfg.buildProduct.empty()) {
        std::string prod = cfg.buildProduct;
        std::transform(prod.begin(), prod.end(), prod.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (prod.starts_with("fenris") || prod.find("diablo iv") != std::string::npos ||
            prod.find("diablo4") != std::string::npos)
            return TvfsDecorator::Diablo4;

        if (prod.starts_with("wow") || prod.find("world of warcraft") != std::string::npos)
            return TvfsDecorator::WowTvfs;
    }

    return TvfsDecorator::None;
}

/// Apply the appropriate TVFS decorator based on build config product info.
/// Takes ownership of @p tvfs and returns the decorated root, or the plain
/// TvfsRoot if no decorator applies or decoration fails.
static std::unique_ptr<RootManifest> decorateTvfsRoot(
    std::unique_ptr<TvfsRoot> tvfs,
    TvfsDecorator hint,
    const std::function<std::vector<u8>(std::span<const u8, 16>)>& eKeyReader,
    interfaces::WorkerPool* pool,
    std::span<const u8> listfile = {})
{
    if (!tvfs) return nullptr;

    switch (hint) {
    case TvfsDecorator::Diablo4: {
        auto d4 = D4Root::create(std::move(tvfs), eKeyReader, pool);
        if (d4) return d4;
        // D4Root::create consumes tvfs on success but returns nullptr on failure;
        // tvfs is moved-from, so we can't fall back — return nullptr.
        return nullptr;
    }
    case TvfsDecorator::WowTvfs: {
        if (WowTvfsRoot::looksLikeWowTvfs(*tvfs)) {
            auto wow = WowTvfsRoot::create(std::move(tvfs), pool, listfile);
            if (wow) return wow;
            return nullptr; // moved-from, can't recover
        }
        // Entries aren't hex-encoded WoW paths — return as plain TVFS.
        return tvfs;
    }
    case TvfsDecorator::None:
    default:
        return tvfs;
    }
}

// ============================================================================
// Resolution helpers — delegate to backend
// ============================================================================

std::vector<u8> Storage::Impl::resolveCKey(
    std::span<const u8, 16> cKey, interfaces::WorkerPool* poolToUse) const {
    if (backend) return backend->resolveCKey(cKey, poolToUse);

    // Fallback for Impl without backend (e.g. StorageWritable::create).
    std::array<u8, 16> cKey16{};
    std::memcpy(cKey16.data(), cKey.data(), 16);
    if (memCache) {
        if (auto cached = memCache->get(cKey16))
            return std::move(*cached);
    }

    auto encEntry = encodingTable.findByCKey(cKey, kEKeyTruncSize);
    if (!encEntry) return {};

    auto loc = dataSource->findInIndex(eKeyTrunc(encEntry->eKey));
    std::vector<u8> blteData;
    if (loc) {
        blteData = dataSource->fetchBlte(*loc);
    } else {
        blteData = dataSource->fetchBlte(encEntry->eKey);
    }
    if (blteData.empty()) return {};

    auto decoded = blteDecode(blteData, &keyRing, poolToUse);
    if (!decoded.success) return {};

    if (memCache)
        memCache->put(cKey16, decoded.data);
    return std::move(decoded.data);
}

std::vector<u8> Storage::Impl::resolveEKey(
    std::span<const u8, 16> eKey, interfaces::WorkerPool* poolToUse) const {
    if (backend) return backend->resolveEKey(eKey, poolToUse);

    // Fallback for Impl without backend.
    std::array<u8, 16> eKey16{};
    std::memcpy(eKey16.data(), eKey.data(), 16);
    if (memCache) {
        if (auto cached = memCache->get(eKey16))
            return std::move(*cached);
    }

    auto loc = dataSource->findInIndex(eKeyTrunc(eKey));
    std::vector<u8> blteData;
    if (loc) {
        blteData = dataSource->fetchBlte(*loc);
    } else {
        blteData = dataSource->fetchBlte(eKey16);
    }
    if (blteData.empty()) return {};

    auto decoded = blteDecode(blteData, &keyRing, poolToUse);
    if (!decoded.success) return {};

    if (memCache)
        memCache->put(eKey16, decoded.data);
    return std::move(decoded.data);
}

std::optional<std::vector<u8>> Storage::Impl::resolveRootEntry(
    const std::vector<const RootEntry*>& entries, u32 localeFlags) const {
    if (backend) return backend->resolveRootEntry(entries, localeFlags);
    if (entries.empty()) return std::nullopt;

    const RootEntry* best = selectBestEntry(entries, localeFlags);
    if (!best) return std::nullopt;

    // Container sub-entry: try the in-memory decoded-data cache first.
    if (best->containerOffset != 0 && memCache) {
        std::array<u8, 16> cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
        if (auto cached = memCache->view(cacheKey)) {
            auto off = static_cast<size_t>(best->containerOffset);
            auto sz  = static_cast<size_t>(best->containerSize);
            if (off + sz <= cached->size()) {
                size_t hdrSz = best->headerSize;
                std::vector<u8> result(hdrSz + sz);
                if (hdrSz > 0)
                    std::memcpy(result.data(), best->headerPrefix.data(), hdrSz);
                std::memcpy(result.data() + hdrSz, cached->data().data() + off, sz);
                return result;
            }
        }
    }

    // Resolve CKey or EKey → index → BLTE.
    const IndexEntry* idxEntry = nullptr;
    std::vector<u8> blteData;

    // For local storage, try single-pass CKey → encoding → index → mmap read.
    if (isLocal()) {
        auto& localDS = *localState->dataSource;
        if (!isZeroKey(best->cKey)) {
            auto encEntry = encodingTable.findByCKey(best->cKey, kEKeyTruncSize);
            if (encEntry) {
                idxEntry = localState->indexTable.find(eKeyTrunc(encEntry->eKey));
            }
        }
        if (!idxEntry && !isZeroKey(best->eKey)) {
            idxEntry = localState->indexTable.find(eKeyTrunc(best->eKey));
        }
        if (!idxEntry) return std::nullopt;
        auto span = localDS.readBlteFromIndex(*idxEntry);
        if (span.empty()) return std::nullopt;
        blteData.assign(span.begin(), span.end());
    } else {
        // Online path: use DataSource for fetch.
        if (!isZeroKey(best->cKey)) {
            auto data = resolveCKey(best->cKey);
            if (!data.empty()) {
                // resolveCKey already decoded — handle container slicing.
                if (best->containerOffset != 0) {
                    auto off = static_cast<size_t>(best->containerOffset);
                    auto sz  = static_cast<size_t>(best->containerSize);
                    if (off + sz > data.size()) return std::nullopt;
                    if (memCache) {
                        std::array<u8, 16> cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
                        memCache->put(cacheKey, data);
                    }
                    size_t hdrSz = best->headerSize;
                    std::vector<u8> result(hdrSz + sz);
                    if (hdrSz > 0)
                        std::memcpy(result.data(), best->headerPrefix.data(), hdrSz);
                    std::memcpy(result.data() + hdrSz, data.data() + off, sz);
                    return result;
                }
                return data;
            }
        }
        if (!isZeroKey(best->eKey)) {
            auto data = resolveEKey(best->eKey);
            if (!data.empty()) {
                if (best->containerOffset != 0) {
                    auto off = static_cast<size_t>(best->containerOffset);
                    auto sz  = static_cast<size_t>(best->containerSize);
                    if (off + sz > data.size()) return std::nullopt;
                    if (memCache) {
                        std::array<u8, 16> cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
                        memCache->put(cacheKey, data);
                    }
                    size_t hdrSz = best->headerSize;
                    std::vector<u8> result(hdrSz + sz);
                    if (hdrSz > 0)
                        std::memcpy(result.data(), best->headerPrefix.data(), hdrSz);
                    std::memcpy(result.data() + hdrSz, data.data() + off, sz);
                    return result;
                }
                return data;
            }
        }
        return std::nullopt;
    }

    // Local path: decode BLTE.
    auto decoded = blteDecode(blteData, &keyRing, pool);
    if (!decoded.success) return std::nullopt;

    if (best->containerOffset != 0) {
        auto off = static_cast<size_t>(best->containerOffset);
        auto sz  = static_cast<size_t>(best->containerSize);
        if (off + sz > decoded.data.size()) return std::nullopt;
        if (memCache) {
            std::array<u8, 16> cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
            memCache->put(cacheKey, decoded.data);
        }
        size_t hdrSz = best->headerSize;
        std::vector<u8> result(hdrSz + sz);
        if (hdrSz > 0)
            std::memcpy(result.data(), best->headerPrefix.data(), hdrSz);
        std::memcpy(result.data() + hdrSz, decoded.data.data() + off, sz);
        return result;
    }

    return std::move(decoded.data);
}

const EncodingEntry* Storage::Impl::resolveEncoding(const RootEntry& re) const {
    if (!isZeroKey(re.cKey)) {
        auto enc = encodingTable.findByCKey(re.cKey, kEKeyTruncSize);
        if (enc) return enc;
    }
    if (!isZeroKey(re.eKey))
        return encodingTable.findByEKey(re.eKey, kEKeyTruncSize);
    return nullptr;
}

std::optional<std::vector<u8>> Storage::Impl::readFileResolved(
    const OverlayKey& key,
    const std::vector<const RootEntry*>& entries,
    u32 localeFlags) const {

    // Check write overlay.
    if (writeOverlay) {
        if (auto it = writeOverlay->pendingWrites.find(key);
            it != writeOverlay->pendingWrites.end())
            return it->second.rawData;
        if (writeOverlay->pendingDeletes.count(key)) {
            s_lastError = kFileNotFound;
            return std::nullopt;
        }
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
// Deferred loading
// ============================================================================

bool Storage::Impl::ensureLoaded() const {
    if (!deferMode) return true;
    std::call_once(deferOnce, [this]() {
        deferLoadOk = loadEncodingAndRoot();
    });
    return deferLoadOk;
}

// ============================================================================
// loadEncodingAndRoot (shared, dispatches VFS prefetch)
// ============================================================================

bool Storage::Impl::loadEncodingAndRoot(std::span<const u8> prefetchedEncodingBlte) const {
    // Step 1: Fetch encoding table via DataSource (index first, loose fallback).
    std::vector<u8> encodingBlte;

    if (!prefetchedEncodingBlte.empty()) {
        encodingBlte.assign(prefetchedEncodingBlte.begin(), prefetchedEncodingBlte.end());
    }

    if (encodingBlte.empty()) {
        if (backend) {
            auto encLoc = backend->findInIndex(eKeyTrunc(buildConfig.encodingEKey));
            if (encLoc) {
                encodingBlte = backend->fetchBlte(*encLoc);
            }
        } else {
            auto encLoc = dataSource->findInIndex(eKeyTrunc(buildConfig.encodingEKey));
            if (encLoc) {
                encodingBlte = dataSource->fetchBlte(*encLoc);
            }
        }
    }

    if (encodingBlte.empty()) {
        // Loose file fallback (online: CDN fetch, local: index scan with full eKey).
        if (backend)
            encodingBlte = backend->fetchBlte(buildConfig.encodingEKey);
        else
            encodingBlte = dataSource->fetchBlte(buildConfig.encodingEKey);
    }

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

    // Step 2: Resolve root manifest.
    CKeyResolver ckeyResolver = [this](std::span<const u8, 16> cKey) -> std::vector<u8> {
        return resolveCKey(cKey, pool);
    };

    bool hasVfs = !isZeroKey(buildConfig.vfsRootCKey);
    if (hasVfs) {
        std::vector<std::array<u8, 16>> vfsEKeys;
        std::unordered_map<u64, std::array<u8, 16>> vfsEKeyToCKey;
        for (auto& sub : buildConfig.vfsSubManifests) {
            if (sub.cKey == buildConfig.vfsRootCKey) continue;
            vfsEKeys.push_back(sub.eKey);
            vfsEKeyToCKey[keyHash64(sub.eKey)] = sub.cKey;
        }

        // Dispatch VFS prefetch based on storage type.
        std::unordered_map<u64, std::vector<u8>> vfsCache;
        if (backend) {
            vfsCache = backend->prefetchVfs(*this, vfsEKeys, vfsEKeyToCKey);
        } else if (isLocal()) {
            vfsCache = prefetchVfsLocal(*this, vfsEKeys, vfsEKeyToCKey);
        } else if (isOnline()) {
            vfsCache = prefetchVfsOnline(*this, vfsEKeys, vfsEKeyToCKey);
        }

        VfsResolver vfsResolver = [&vfsCache, this](std::span<const u8> eKey) -> std::vector<u8> {
            u64 h = keyHash64(eKey.data());
            auto it = vfsCache.find(h);
            if (it != vfsCache.end())
                return it->second;  // copy — sub-manifest may be needed multiple times

            // Fallback: live resolve (shouldn't normally be needed).
            std::array<u8, 16> eKey16{};
            std::memcpy(eKey16.data(), eKey.data(), std::min(eKey.size(), size_t(16)));
            auto result = resolveEKey(eKey16);
            if (!result.empty()) return result;
            return {};
        };

        auto vfsData = resolveCKey(buildConfig.vfsRootCKey, pool);
        if (!vfsData.empty()) {
            auto tvfsRoot = TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, pool);
            if (tvfsRoot) {
                EKeyReader eKeyReader = [this](std::span<const u8, 16> eKey) -> std::vector<u8> {
                    return resolveEKey(eKey);
                };
                auto hint = classifyTvfsProduct(buildConfig);
                root = decorateTvfsRoot(std::move(tvfsRoot), hint, eKeyReader, pool, listfileData);
            }
        }
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
                auto tvfsPlain = TvfsRoot::parse(rootData, pool);
                if (tvfsPlain) {
                    EKeyReader eKeyReader = [this](std::span<const u8, 16> eKey) -> std::vector<u8> {
                        return resolveEKey(eKey);
                    };
                    auto hint = classifyTvfsProduct(buildConfig);
                    root = decorateTvfsRoot(std::move(tvfsPlain), hint, eKeyReader, pool, listfileData);
                }
            } else if (magic == RootSignature::kMFST) {
                root = WowRoot::parse(rootData, pool, listfileData);
            } else if (magic == RootSignature::kD3Root || magic == RootSignature::kD3Dir) {
                root = D3Root::parse(rootData, ckeyResolver, pool);
            } else if (magic == RootSignature::kMNDX) {
                root = MndxRoot::parse(rootData, pool);
            }

            // Overwatch text root (starts with '#').
            if (!root && !rootData.empty() && rootData[0] == '#') {
                root = OwRoot::parse(rootData, ckeyResolver, pool);
            }

            // S1 / Agent text root (pipe-delimited path|ckey lines).
            if (!root && S1Root::looksLikeS1Root(rootData)) {
                root = S1Root::parse(rootData, pool);
            }
        }

        if (!root)
            root = WowRoot::parse(rootData, pool, listfileData);

        // Fallback: try install manifest.
        if (!root && !isZeroKey(buildConfig.installCKey)) {
            auto installData = resolveCKey(buildConfig.installCKey);
            if (!installData.empty()) {
                auto installRoot = InstallRoot::parse(installData, pool);
                if (installRoot)
                    root = std::move(installRoot);
            }
        }

        // Final fallback: empty root.
        if (!root)
            root = GenericRoot::create();
    }

    if (!root) {
        s_lastError = kRootParseFailed;
        return false;
    }

    // Step 3: Pre-resolve file sizes and build encoding-referenced bitvector.
    m_encodingReferenced.assign(encodingTable.entryCount(), false);
    const auto* encBase = encodingTable.entries().data();
    root->resolveEntries([this, encBase](RootEntry& e) {
        const EncodingEntry* enc = nullptr;
        if (!isZeroKey(e.cKey))
            enc = encodingTable.findByCKey(e.cKey, kEKeyTruncSize);
        if (!enc && !isZeroKey(e.eKey))
            enc = encodingTable.findByEKey(e.eKey, kEKeyTruncSize);
        if (enc) {
            e.fileSize = enc->fileSize;
            if (isZeroKey(e.cKey))
                e.cKey = enc->cKey;
            m_encodingReferenced[static_cast<size_t>(enc - encBase)] = true;
        }
    });

    // Step 4: Container cache allocation (D4 combined-meta containers).
    constexpr size_t kD4MinCacheSize = 64 * 1024 * 1024;
    if (root->format() == RootFormat::Diablo4) {
        if (!memCache)
            memCache = std::make_unique<MemoryCache>(kD4MinCacheSize);

        // Upgrade backend to cache-enabled variant.
        if (backend && !backend->hasCache()) {
            MemCacheEnabled cacheTraits{std::make_unique<MemoryCache>(kD4MinCacheSize)};
            if (backend->isLocal()) {
                LocalDataTraits dataTraits{
                    &localState->indexTable,
                    localState->dataSource.get()};
                backend = std::make_unique<StorageBackendImpl<LocalDataTraits, MemCacheEnabled>>(
                    std::move(dataTraits), std::move(cacheTraits),
                    encodingTable, keyRing, pool);
            } else {
                OnlineDataTraits dataTraits{
                    onlineState->dataSource.get(),
                    &onlineState->onlineIndex};
                backend = std::make_unique<StorageBackendImpl<OnlineDataTraits, MemCacheEnabled>>(
                    std::move(dataTraits), std::move(cacheTraits),
                    encodingTable, keyRing, pool);
            }
        }
    }

    return true;
}

// ============================================================================
// Storage construction / destruction
// ============================================================================

Storage::Storage() = default;
Storage::~Storage() = default;
Storage::Storage(Storage&&) noexcept = default;
Storage& Storage::operator=(Storage&&) noexcept = default;

Storage::Storage(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}

void Storage::close() {
    if (m_impl) {
        std::unique_lock lock(m_impl->mutex);
        m_impl->isValid = false;
        if (m_impl->localState) {
            m_impl->localState->dataArchives.clear();
        }
        m_impl->root.reset();
    }
}

Storage::operator bool() const noexcept {
    return m_impl && m_impl->isValid;
}

// ============================================================================
// Capability queries
// ============================================================================

bool Storage::isLocal() const noexcept {
    return m_impl && m_impl->isLocal();
}

bool Storage::isOnline() const noexcept {
    return m_impl && m_impl->isOnline();
}

bool Storage::isWritable() const noexcept {
    return m_impl && m_impl->writeOverlay != nullptr;
}

RootFormat Storage::rootFormat() const noexcept {
    if (!m_impl || !m_impl->root) return RootFormat::Unknown;
    return m_impl->root->format();
}

// ============================================================================
// Read operations
// ============================================================================

std::optional<std::vector<u8>> Storage::readFile(const std::string& cascPath) const {
    return readFile(cascPath, 0, 0);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& cascPath,
                                                  u32 localeFlags, u32 /*openFlags*/) const {
    if (!m_impl || !m_impl->isValid) { s_lastError = kNotValid; return std::nullopt; }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return std::nullopt; }

    std::shared_lock lock(m_impl->mutex);
    auto normalized = normalizeCascPath(cascPath);
    OverlayKey key{normalized, std::nullopt};
    auto entries = m_impl->root ? m_impl->root->findByNormalizedPath(normalized)
                                : std::vector<const RootEntry*>{};
    return m_impl->readFileResolved(key, entries, localeFlags);
}

std::optional<std::vector<u8>> Storage::readFile(i32 fileId, FileIdHint hint) const {
    return readFile(fileId, 0, 0, hint);
}

std::optional<std::vector<u8>> Storage::readFile(i32 fileId,
                                                  u32 localeFlags, u32 /*openFlags*/,
                                                  FileIdHint hint) const {
    if (!m_impl || !m_impl->isValid) { s_lastError = kNotValid; return std::nullopt; }
    if (!m_impl->ensureLoaded()) { s_lastError = kNotValid; return std::nullopt; }

    std::shared_lock lock(m_impl->mutex);
    OverlayKey key{"", static_cast<u32>(fileId), hint};
    auto entries = m_impl->root ? m_impl->root->findByFileDataId(static_cast<u32>(fileId), hint)
                                : std::vector<const RootEntry*>{};
    return m_impl->readFileResolved(key, entries, localeFlags);
}

// ============================================================================
// readBatch (unified local + online)
// ============================================================================

std::vector<BatchReadResult> Storage::readBatch(
    std::span<const BatchReadRequest> requests) const {

    std::vector<BatchReadResult> results(requests.size());
    if (requests.empty()) return results;

    if (!m_impl || !m_impl->isValid) {
        for (auto& r : results) r.error = "storage not valid";
        return results;
    }
    if (!m_impl->ensureLoaded()) {
        for (auto& r : results) r.error = "deferred load failed";
        return results;
    }

    std::shared_lock lock(m_impl->mutex);

    // Phase 0: Overlay check + root lookup.
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

        // Check write overlay.
        if (m_impl->writeOverlay) {
            OverlayKey oKey;
            if (byPath)
                oKey = {normalizeCascPath(req.path), std::nullopt};
            else
                oKey = {"", static_cast<u32>(req.fileDataId), req.fileIdHint};

            if (auto it = m_impl->writeOverlay->pendingWrites.find(oKey);
                it != m_impl->writeOverlay->pendingWrites.end()) {
                results[i].data = it->second.rawData;
                results[i].success = true;
                continue;
            }
            if (m_impl->writeOverlay->pendingDeletes.count(oKey)) {
                results[i].error = "file is pending deletion";
                continue;
            }
        }

        // Root lookup.
        std::vector<const RootEntry*> entries;
        if (m_impl->root) {
            if (byPath)
                entries = m_impl->root->findByNormalizedPath(normalizeCascPath(req.path));
            else
                entries = m_impl->root->findByFileDataId(static_cast<u32>(req.fileDataId), req.fileIdHint);
        }

        if (entries.empty()) {
            results[i].error = "file not found in root manifest";
            continue;
        }

        u32 locale = req.localeFlags != 0 ? req.localeFlags : m_impl->localeMask;
        toResolve.push_back({i, std::move(entries), locale});
    }

    if (toResolve.empty()) return results;

    // Phase 0.5 (cache) + Phase 1 (fetch) — delegate to backend when available.
    std::vector<ResolvedBlob> resolvedBlobs(toResolve.size());

    if (m_impl->backend) {
        m_impl->backend->resolveBatch(toResolve, resolvedBlobs, m_impl->pool);

        // Transfer cached results (Phase 0.5 cache hits) to final output.
        for (size_t i = 0; i < toResolve.size(); ++i) {
            if (toResolve[i].cachedResult) {
                results[toResolve[i].requestIndex].data = std::move(resolvedBlobs[i].blteData);
                results[toResolve[i].requestIndex].success = true;
            }
        }
    } else {
        // Fallback: Phase 0.5 + Phase 1 inline (pre-backend Impl).

        // Phase 0.5: Container cache lookup.
        if (m_impl->memCache) {
            for (auto& work : toResolve) {
                const RootEntry* best = selectBestEntry(work.rootEntries, work.localeFlags);
                if (!best || best->containerOffset == 0) continue;

                std::array<u8, 16> cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
                auto cached = m_impl->memCache->view(cacheKey);
                if (!cached) continue;

                auto off = static_cast<size_t>(best->containerOffset);
                auto sz  = static_cast<size_t>(best->containerSize);
                if (off + sz > cached->size()) continue;

                size_t hdrSz = best->headerSize;
                std::vector<u8> sliced(hdrSz + sz);
                if (hdrSz > 0)
                    std::memcpy(sliced.data(), best->headerPrefix.data(), hdrSz);
                std::memcpy(sliced.data() + hdrSz, cached->data().data() + off, sz);
                results[work.requestIndex].data = std::move(sliced);
                results[work.requestIndex].success = true;
                work.cachedResult = true;
            }
        }

        // Phase 1: Resolution + fetch (dispatched by storage type).
        if (m_impl->isLocal()) {
            auto resolveOne = [&](size_t idx) {
                auto& work = toResolve[idx];
                auto& blob = resolvedBlobs[idx];
                if (work.cachedResult) return;

                const RootEntry* best = selectBestEntry(work.rootEntries, work.localeFlags);
                if (!best) { blob.error = "no matching root entry"; return; }

                const IndexEntry* idxEntry = nullptr;
                if (!isZeroKey(best->cKey)) {
                    auto encEntry = m_impl->encodingTable.findByCKey(best->cKey, kEKeyTruncSize);
                    if (encEntry)
                        idxEntry = m_impl->localState->indexTable.find(eKeyTrunc(encEntry->eKey));
                }
                if (!idxEntry && !isZeroKey(best->eKey)) {
                    idxEntry = m_impl->localState->indexTable.find(eKeyTrunc(best->eKey));
                }
                if (!idxEntry) { blob.error = "file not found in index"; return; }

                blob.blteSpan = m_impl->localState->dataSource->readBlteFromIndex(*idxEntry);
                if (blob.blteSpan.empty()) {
                    blob.error = "failed to read raw BLTE data from archive";
                    return;
                }
                blob.resolved = true;
                blob.useSpan = true;
            };

            constexpr size_t kMinBatchParallel = 4;
            if (m_impl->pool && toResolve.size() >= kMinBatchParallel &&
                m_impl->pool->threadCount() > 0) {
                utils::JobGroup resolveGroup;
                resolveGroup.add(toResolve.size());
                for (size_t idx = 0; idx < toResolve.size(); ++idx) {
                    interfaces::WorkerTask task;
                    task.fn = [&, idx]() { resolveOne(idx); resolveGroup.done(); };
                    m_impl->pool->submit(task);
                }
                resolveGroup.wait();
            } else {
                for (size_t idx = 0; idx < toResolve.size(); ++idx)
                    resolveOne(idx);
            }

        } else if (m_impl->isOnline()) {
            struct FetchParams {
                bool needsFetch = false;
                bool useArchive = false;
                u32 archiveIndex = 0;
                u64 archiveOffset = 0;
                u32 encodedSize = 0;
                std::array<u8, 16> eKey{};
            };
            std::vector<FetchParams> fetchParams(toResolve.size());

            for (size_t idx = 0; idx < toResolve.size(); ++idx) {
                auto& work = toResolve[idx];
                auto& blob = resolvedBlobs[idx];
                auto& fp = fetchParams[idx];
                if (work.cachedResult) continue;

                const RootEntry* best = selectBestEntry(work.rootEntries, work.localeFlags);
                if (!best) { blob.error = "no matching root entry"; continue; }

                const OnlineIndexTable::Entry* idxEntry = nullptr;
                std::array<u8, 16> eKey{};

                if (!isZeroKey(best->cKey)) {
                    auto encEntry = m_impl->encodingTable.findByCKey(best->cKey, kEKeyTruncSize);
                    if (encEntry) {
                        eKey = encEntry->eKey;
                        idxEntry = m_impl->onlineState->onlineIndex.find(eKeyTrunc(encEntry->eKey));
                    }
                }
                if (!idxEntry && !isZeroKey(best->eKey)) {
                    eKey = best->eKey;
                    idxEntry = m_impl->onlineState->onlineIndex.find(eKeyTrunc(best->eKey));
                }

                if (idxEntry) {
                    fp.needsFetch = true;
                    fp.useArchive = true;
                    fp.archiveIndex = idxEntry->archiveIndex;
                    fp.archiveOffset = idxEntry->archiveOffset;
                    fp.encodedSize = idxEntry->encodedSize;
                } else if (!isZeroKey(eKey)) {
                    fp.needsFetch = true;
                    fp.useArchive = false;
                    fp.eKey = eKey;
                } else {
                    blob.error = "failed to resolve to CDN fetch params";
                }
            }

            struct WaitState {
                std::atomic<size_t> completed{0};
                size_t total = 0;
                std::mutex mtx;
                std::condition_variable cv;
            };
            auto state = std::make_shared<WaitState>();
            for (auto& fp : fetchParams)
                if (fp.needsFetch) ++state->total;

            if (state->total > 0) {
                auto makeCallback = [&resolvedBlobs, state](size_t idx) {
                    return [&resolvedBlobs, state, idx](std::optional<std::vector<u8>> data) {
                        if (data && !data->empty()) {
                            resolvedBlobs[idx].blteData = std::move(*data);
                            resolvedBlobs[idx].resolved = true;
                        } else {
                            resolvedBlobs[idx].error = "failed to fetch BLTE data from CDN";
                        }
                        if (state->completed.fetch_add(1, std::memory_order_acq_rel) + 1 >= state->total) {
                            std::lock_guard<std::mutex> lk(state->mtx);
                            state->cv.notify_one();
                        }
                    };
                };

                for (size_t idx = 0; idx < toResolve.size(); ++idx) {
                    auto& fp = fetchParams[idx];
                    if (!fp.needsFetch) continue;
                    if (fp.useArchive) {
                        m_impl->onlineState->dataSource->fetchBlteAsync(
                            fp.archiveIndex, fp.archiveOffset, fp.encodedSize,
                            makeCallback(idx));
                    } else {
                        m_impl->onlineState->dataSource->fetchBlteAsync(
                            fp.eKey, makeCallback(idx));
                    }
                }

                std::unique_lock<std::mutex> lk(state->mtx);
                state->cv.wait_for(lk, std::chrono::seconds(120), [&] {
                    return state->completed.load(std::memory_order_acquire) >= state->total;
                });
            }
        }
    }

    // Phase 2: Batch BLTE decode.
    std::vector<BlteBatchEntry> batchEntries;
    std::vector<size_t> batchToResolveIdx;
    batchEntries.reserve(toResolve.size());
    batchToResolveIdx.reserve(toResolve.size());

    for (size_t idx = 0; idx < toResolve.size(); ++idx) {
        if (toResolve[idx].cachedResult) continue;
        if (!resolvedBlobs[idx].resolved) {
            results[toResolve[idx].requestIndex].error = std::move(resolvedBlobs[idx].error);
            continue;
        }
        BlteBatchEntry entry;
        entry.blteData = resolvedBlobs[idx].useSpan
            ? resolvedBlobs[idx].blteSpan
            : std::span<const u8>(resolvedBlobs[idx].blteData);
        batchEntries.push_back(entry);
        batchToResolveIdx.push_back(idx);
    }

    if (!batchEntries.empty()) {
        auto decoded = blteDecodeBatch(batchEntries, &m_impl->keyRing, m_impl->pool);

        // Phase 3: Map results + container slicing.
        for (size_t d = 0; d < decoded.size(); ++d) {
            size_t resolveIdx = batchToResolveIdx[d];
            size_t requestIdx = toResolve[resolveIdx].requestIndex;

            if (!decoded[d].success) {
                results[requestIdx].success = false;
                if (!decoded[d].error.empty())
                    results[requestIdx].error = std::move(decoded[d].error);
                continue;
            }

            const RootEntry* best = selectBestEntry(
                toResolve[resolveIdx].rootEntries,
                toResolve[resolveIdx].localeFlags);
            if (best && best->containerOffset != 0) {
                auto off = static_cast<size_t>(best->containerOffset);
                auto sz  = static_cast<size_t>(best->containerSize);
                if (off + sz <= decoded[d].data.size()) {
                    if (m_impl->memCache) {
                        std::array<u8, 16> cacheKey = !isZeroKey(best->eKey) ? best->eKey : best->cKey;
                        m_impl->memCache->put(cacheKey, decoded[d].data);
                    }
                    size_t hdrSz = best->headerSize;
                    std::vector<u8> sliced(hdrSz + sz);
                    if (hdrSz > 0)
                        std::memcpy(sliced.data(), best->headerPrefix.data(), hdrSz);
                    std::memcpy(sliced.data() + hdrSz, decoded[d].data.data() + off, sz);
                    results[requestIdx].data = std::move(sliced);
                } else {
                    results[requestIdx].error = "container sub-entry out of bounds";
                    results[requestIdx].success = false;
                    continue;
                }
            } else {
                results[requestIdx].data = std::move(decoded[d].data);
            }
            results[requestIdx].success = true;
        }
    }

    return results;
}

// ============================================================================
// fileExists
// ============================================================================

bool Storage::fileExists(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return false;
    if (!m_impl->ensureLoaded()) return false;
    std::shared_lock lock(m_impl->mutex);

    auto normalized = normalizeCascPath(cascPath);

    if (m_impl->writeOverlay) {
        OverlayKey key{normalized, std::nullopt};
        if (m_impl->writeOverlay->pendingWrites.count(key)) return true;
        if (m_impl->writeOverlay->pendingDeletes.count(key)) return false;
    }

    return m_impl->root && m_impl->root->hasPath(normalized);
}

bool Storage::fileExists(i32 fileId, FileIdHint hint) const {
    if (!m_impl || !m_impl->isValid) return false;
    if (!m_impl->ensureLoaded()) return false;
    std::shared_lock lock(m_impl->mutex);

    if (m_impl->writeOverlay) {
        OverlayKey key{"", static_cast<u32>(fileId), hint};
        if (m_impl->writeOverlay->pendingWrites.count(key)) return true;
        if (m_impl->writeOverlay->pendingDeletes.count(key)) return false;
    }

    return m_impl->root && m_impl->root->hasFileDataId(static_cast<u32>(fileId), hint);
}

// ============================================================================
// fileSize
// ============================================================================

std::optional<u64> Storage::fileSize(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByNormalizedPath(normalizeCascPath(cascPath));
    if (entries.empty()) return std::nullopt;
    if (entries[0]->fileSize > 0) return entries[0]->fileSize;

    auto encEntry = m_impl->resolveEncoding(*entries[0]);
    if (!encEntry) return std::nullopt;
    return encEntry->fileSize;
}

std::optional<u64> Storage::fileSize(i32 fileId, FileIdHint hint) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByFileDataId(static_cast<u32>(fileId), hint);
    if (entries.empty()) return std::nullopt;
    if (entries[0]->fileSize > 0) return entries[0]->fileSize;

    auto encEntry = m_impl->resolveEncoding(*entries[0]);
    if (!encEntry) return std::nullopt;
    return encEntry->fileSize;
}

// ============================================================================
// fileInfo
// ============================================================================

std::optional<FileFullInfo> Storage::fileInfo(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->fileInfoResolved(
        m_impl->root->findByNormalizedPath(normalizeCascPath(cascPath)));
}

std::optional<FileFullInfo> Storage::fileInfo(i32 fileId, FileIdHint hint) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->fileInfoResolved(
        m_impl->root->findByFileDataId(static_cast<u32>(fileId), hint));
}

// ============================================================================
// enumerate (unfiltered)
// ============================================================================

void Storage::enumerate(std::function<bool(const EnumerateEntry&)> callback) const {
    if (!m_impl || !m_impl->isValid || !callback) return;
    if (!m_impl->ensureLoaded()) return;
    std::shared_lock lock(m_impl->mutex);

    EnumerateEntry fe;
    m_impl->root->enumerate([&](const RootEntry& re) -> bool {
        fe.cKey = re.cKey;
        fe.fileSize = re.fileSize;
        fe.localeFlags = re.localeFlags;
        fe.contentFlags = re.contentFlags;
        fe.fileDataId = static_cast<i32>(re.fileDataId);
        fe.path = re.path;
        return callback(fe);
    });

    // Emit encoding-table orphans using pre-built bitvector.
    static constexpr char kHex[] = "0123456789abcdef";
    const auto& encEntries = m_impl->encodingTable.entries();
    char hexBuf[33];
    for (size_t i = 0; i < encEntries.size(); ++i) {
        auto& enc = encEntries[i];
        if (isZeroKey(enc.cKey)) continue;
        if (i < m_impl->m_encodingReferenced.size() && m_impl->m_encodingReferenced[i])
            continue;

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
        if (!callback(fe)) break;
    }
}

// ── Wildcard helpers ─────────────────────────────────────────────

namespace {

inline char normChar(char ch) noexcept {
    if (ch == '/') return '\\';
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch;
}

static std::string normalizeMask(const std::string& mask) {
    std::string r;
    r.reserve(mask.size());
    for (char c : mask) r.push_back(normChar(c));
    return r;
}

static bool wildcardMatch(std::string_view pat, std::string_view str) noexcept {
    size_t pi = 0, si = 0;
    size_t starPat = std::string_view::npos, starStr = 0;
    while (si < str.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == normChar(str[si]))) {
            ++pi; ++si;
        } else if (pi < pat.size() && pat[pi] == '*') {
            starPat = pi++; starStr = si;
        } else if (starPat != std::string_view::npos) {
            pi = starPat + 1; si = ++starStr;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

static bool isTrivialMask(std::string_view mask) noexcept {
    for (char ch : mask) if (ch != '*') return false;
    return true;
}

static std::string_view extractPrefix(std::string_view mask) noexcept {
    size_t wild = 0;
    while (wild < mask.size() && mask[wild] != '*' && mask[wild] != '?') ++wild;
    if (wild == 0) return {};
    size_t lastSep = mask.rfind('\\', wild - 1);
    if (lastSep == std::string_view::npos) return {};
    return mask.substr(0, lastSep + 1);
}

static std::string_view extractPureSuffix(std::string_view mask) noexcept {
    if (mask.size() < 2 || mask[0] != '*') return {};
    for (size_t i = 1; i < mask.size(); ++i)
        if (mask[i] == '*' || mask[i] == '?') return {};
    return mask.substr(1);
}

static bool endsWithNorm(std::string_view str, std::string_view suffix) noexcept {
    if (str.size() < suffix.size()) return false;
    size_t off = str.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
        if (normChar(str[off + i]) != suffix[i]) return false;
    return true;
}

} // anonymous namespace

void Storage::enumerate(const std::string& mask,
                        std::function<bool(const EnumerateEntry&)> callback) const {
    if (!callback) return;
    if (!m_impl || !m_impl->isValid) return;
    if (!m_impl->ensureLoaded()) return;

    if (mask.empty() || isTrivialMask(mask)) {
        enumerate(std::move(callback));
        return;
    }

    std::string normMask = normalizeMask(mask);
    std::shared_lock lock(m_impl->mutex);

    std::string_view pureSuffix = extractPureSuffix(normMask);
    std::string_view prefix = extractPrefix(normMask);

    EnumerateEntry fe;
    auto rootCallback = [&](const RootEntry& re) -> bool {
        if (!prefix.empty()) {
            if (re.path.size() < prefix.size()) return true;
            for (size_t i = 0; i < prefix.size(); ++i)
                if (normChar(re.path[i]) != prefix[i]) return true;
        }

        bool match = !pureSuffix.empty()
            ? endsWithNorm(re.path, pureSuffix)
            : wildcardMatch(normMask, re.path);
        if (!match) return true;

        fe.cKey = re.cKey;
        fe.fileSize = re.fileSize;
        fe.localeFlags = re.localeFlags;
        fe.contentFlags = re.contentFlags;
        fe.fileDataId = static_cast<i32>(re.fileDataId);
        fe.path = re.path;
        return callback(fe);
    };

    m_impl->root->enumerate(rootCallback);

    // Orphan phase — skip if mask can't match hex-string paths.
    bool canMatchOrphan = pureSuffix.empty() && prefix.empty();
    if (!canMatchOrphan) return;

    static constexpr char kHex[] = "0123456789abcdef";
    const auto& encEntries = m_impl->encodingTable.entries();
    char hexBuf[33];
    for (size_t i = 0; i < encEntries.size(); ++i) {
        auto& enc = encEntries[i];
        if (isZeroKey(enc.cKey)) continue;
        if (i < m_impl->m_encodingReferenced.size() && m_impl->m_encodingReferenced[i])
            continue;

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

// ============================================================================
// listFiles / listEntries / totalFileCount
// ============================================================================

std::vector<std::string> Storage::listFiles() const {
    std::vector<std::string> result;
    if (!m_impl || !m_impl->isValid) return result;
    if (!m_impl->ensureLoaded()) return result;
    std::shared_lock lock(m_impl->mutex);
    m_impl->root->enumerate([&](const RootEntry& re) -> bool {
        if (!re.path.empty()) result.push_back(re.path);
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

// ============================================================================
// product
// ============================================================================

std::optional<StorageProduct> Storage::product() const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;

    if (m_impl->isOnline()) {
        return m_impl->onlineState->productInfo;
    }

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
// Cache + misc
// ============================================================================

void Storage::flushCache() {
    if (m_impl) {
        if (m_impl->backend)
            m_impl->backend->flushCache();
        if (m_impl->memCache)
            m_impl->memCache->clear();
    }
}

bool Storage::prefetch() {
    if (!m_impl || !m_impl->isValid) return false;
    return m_impl->ensureLoaded();
}

u32 Storage::lastError() noexcept {
    return s_lastError;
}

} // namespace whiteout::storages::casc

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/storages/casc/online_storage.h>

// Internal headers.
#include "blte.h"
#include "cdn_cache.h"
#include "cdn_fetcher.h"
#include "config.h"
#include "constants.h"
#include "crypto.h"
#include "data_source.h"
#include "encoding.h"
#include "key_utils.h"
#include "memory_cache.h"
#include "online_data_source.h"
#include "online_index.h"
#include "roots/root.h"
#include "roots/wow_root.h"
#include "roots/d3_root.h"
#include "roots/tvfs_root.h"
#include "roots/mndx_root.h"
#include "../common/hex.h"
#include "../common/string_utils.h"

#include <whiteout/interfaces.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_set>

namespace whiteout::storages::casc {

using storages::common::normalizeCascPath;

// ============================================================================
// Thread-local error
// ============================================================================

static thread_local u32 s_onlineLastError = 0;

enum OnlineErrorCode : u32 {
    kOk = 0,
    kNoHttpHandler = CascError::NoHttpHandler,
    kVersionInfoNotFound = CascError::VersionInfoNotFound,
    kCdnInfoNotFound = CascError::CdnInfoNotFound,
    kHttpRequestFailed = CascError::HttpRequestFailed,
    kCdnServerUnavailable = CascError::CdnServerUnavailable,
    kRemoteFileNotFound = CascError::RemoteFileNotFound,
    kEncodingNotFound = 6,
    kEncodingDecodeFailed = 7,
    kRootNotFound = 8,
    kRootDecodeFailed = 9,
    kRootParseFailed = 10,
    kFileNotFound = 11,
    kFileDecodeFailed = 12,
    kNotValid = 13,
};

// ============================================================================
// Impl
// ============================================================================

struct OnlineStorage::Impl {
    interfaces::HttpHandler* http = nullptr;
    interfaces::WorkerPool* pool = nullptr;
    u32 localeMask = 0;

    std::vector<CdnServer> cdnServers;
    BuildConfig buildConfig;
    CdnConfig cdnConfig;
    StorageProduct productInfo;

    OnlineIndexTable onlineIndex;
    OnlineIndexTable looseIndex;
    mutable EncodingTable encodingTable;
    mutable std::unique_ptr<RootManifest> root;
    KeyRing keyRing;

    std::unique_ptr<CdnCache> cache;
    std::unique_ptr<CdnFetcher> fetcher;
    std::unique_ptr<MemoryCache> memCache;
    std::unique_ptr<OnlineDataSource> dataSource;

    mutable std::shared_mutex mutex;
    bool isValid = false;

    // LoadOnDemand: defer encoding + root loading until first access.
    bool deferMode = false;
    mutable std::once_flag deferOnce;
    mutable bool deferLoadOk = true;

    // ── Resolution helpers ─────────────────────────────────────────

    std::vector<u8> resolveCKey(std::span<const u8, 16> cKey) const;
    std::vector<u8> resolveEKey(std::span<const u8, 16> eKey) const;
    std::optional<std::vector<u8>> resolveRootEntry(
        const std::vector<RootEntry>& entries, u32 localeFlags) const;
    const EncodingEntry* resolveEncoding(const RootEntry& re) const;
    std::optional<FileFullInfo> fileInfoResolved(
        const std::vector<RootEntry>& entries) const;

    bool ensureLoaded() const;
    bool loadEncodingAndRoot(std::span<const u8> prefetchedEncodingBlte = {}) const;
};

// ── Resolution ─────────────────────────────────────────────────────

std::vector<u8> OnlineStorage::Impl::resolveCKey(std::span<const u8, 16> cKey) const {
    // Use 9-byte matching: TVFS roots truncate cKeys to eKeySize (9) bytes.
    // With 72 bits of MD5, false-positive risk is negligible (~10^-12).
    auto encEntry = encodingTable.findByCKey(cKey, kEKeyTruncSize);
    if (!encEntry) return {};

    // Try archive index first.
    auto trunc = eKeyTrunc(encEntry->eKey);
    auto idxEntry = onlineIndex.find(trunc);
    std::vector<u8> blteData;
    if (idxEntry) {
        blteData = dataSource->fetchBlte(idxEntry->archiveIndex,
                                          idxEntry->archiveOffset,
                                          idxEntry->encodedSize);
    } else {
        // Loose file fallback.
        blteData = dataSource->fetchBlte(encEntry->eKey);
    }
    if (blteData.empty()) return {};

    auto decoded = blteDecode(blteData, &keyRing, pool);
    if (!decoded.success) return {};
    return std::move(decoded.data);
}

std::vector<u8> OnlineStorage::Impl::resolveEKey(std::span<const u8, 16> eKey) const {
    auto idxEntry = onlineIndex.find(eKeyTrunc(eKey));
    std::vector<u8> blteData;
    if (idxEntry) {
        blteData = dataSource->fetchBlte(idxEntry->archiveIndex,
                                          idxEntry->archiveOffset,
                                          idxEntry->encodedSize);
    } else {
        std::array<u8, 16> eKey16{};
        std::memcpy(eKey16.data(), eKey.data(), 16);
        blteData = dataSource->fetchBlte(eKey16);
    }
    if (blteData.empty()) return {};

    auto decoded = blteDecode(blteData, &keyRing, pool);
    if (!decoded.success) return {};
    return std::move(decoded.data);
}

std::optional<std::vector<u8>> OnlineStorage::Impl::resolveRootEntry(
    const std::vector<RootEntry>& entries, u32 localeFlags) const {
    if (entries.empty()) return std::nullopt;

    const RootEntry* best = selectBestEntry(entries, localeFlags);
    if (!best) return std::nullopt;

    std::vector<u8> data;
    if (!isZeroKey(best->cKey)) {
        data = resolveCKey(best->cKey);
    }
    // Fallback to direct eKey resolution.
    // Fallback to direct eKey resolution.
    if (data.empty() && !isZeroKey(best->eKey)) {
        data = resolveEKey(best->eKey);
    }
    if (data.empty()) return std::nullopt;
    return data;
}

const EncodingEntry* OnlineStorage::Impl::resolveEncoding(const RootEntry& re) const {
    // TVFS roots truncate both cKey and eKey to eKeySize (9) bytes, zero-
    // padded to 16. Use truncated matching for both paths to avoid false
    // negatives from trailing zeros vs real key bytes.
    if (!isZeroKey(re.cKey)) {
        auto enc = encodingTable.findByCKey(re.cKey, kEKeyTruncSize);
        if (enc) return enc;
    }
    if (!isZeroKey(re.eKey))
        return encodingTable.findByEKey(re.eKey, kEKeyTruncSize);
    return nullptr;
}

std::optional<FileFullInfo> OnlineStorage::Impl::fileInfoResolved(
    const std::vector<RootEntry>& entries) const {
    if (entries.empty()) return std::nullopt;

    auto& re = entries[0];
    FileFullInfo info;
    info.cKey = re.cKey;
    info.eKey = re.eKey;
    info.localeFlags = re.localeFlags;
    info.contentFlags = re.contentFlags;
    info.fileDataId = static_cast<i32>(re.fileDataId);
    info.path = re.path;

    auto encEntry = resolveEncoding(re);
    if (encEntry) {
        info.fileSize = encEntry->fileSize;
        info.cKey = encEntry->cKey;
        info.eKey = encEntry->eKey;
    }

    return info;
}

// ── Lazy loading ─────────────────────────────────────────────────

bool OnlineStorage::Impl::ensureLoaded() const {
    if (!deferMode) return true;
    std::call_once(deferOnce, [this]() {
        deferLoadOk = loadEncodingAndRoot();
    });
    return deferLoadOk;
}

bool OnlineStorage::Impl::loadEncodingAndRoot(std::span<const u8> prefetchedEncodingBlte) const {
    // Fetch encoding table.
    auto encodingEKeyHex = storages::common::hexEncode16(buildConfig.encodingEKey);

    // Use pre-fetched encoding BLTE data if available.
    std::vector<u8> encodingBlte;
    if (!prefetchedEncodingBlte.empty()) {
        encodingBlte.assign(prefetchedEncodingBlte.begin(), prefetchedEncodingBlte.end());
    }

    // If no pre-fetch, try archive index first, then loose file.
    if (encodingBlte.empty()) {
        auto idxEntry = onlineIndex.find(eKeyTrunc(buildConfig.encodingEKey));
        if (idxEntry) {
            encodingBlte = dataSource->fetchBlte(idxEntry->archiveIndex,
                                                  idxEntry->archiveOffset,
                                                  idxEntry->encodedSize);
        }
    }
    if (encodingBlte.empty()) {
        // Loose file fetch.
        encodingBlte = dataSource->fetchBlte(buildConfig.encodingEKey);
    }
    if (encodingBlte.empty()) {
        s_onlineLastError = kEncodingNotFound;
        return false;
    }

    auto encodingDecoded = blteDecode(encodingBlte, &keyRing, pool);
    if (!encodingDecoded.success) {
        s_onlineLastError = kEncodingDecodeFailed;
        return false;
    }

    encodingTable = EncodingTable::parse(encodingDecoded.data);

    if (encodingTable.entryCount() == 0) {
        s_onlineLastError = kEncodingDecodeFailed;
        return false;
    }

    // Resolve root manifest.
    CKeyResolver ckeyResolver = [this](std::span<const u8, 16> cKey) -> std::vector<u8> {
        return resolveCKey(cKey);
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

        // Pre-fetch all VFS sub-manifest BLTE blobs in parallel so the
        // resolver doesn't need one HTTP round-trip per sub-manifest.
        struct PrefetchResult {
            u64 eKeyHash = 0;
            std::vector<u8> blteData;
        };
        const size_t totalVfs = vfsEKeys.size();
        std::vector<PrefetchResult> prefetchResults(totalVfs);

        struct WaitState {
            std::atomic<size_t> completed{0};
            std::mutex mtx;
            std::condition_variable cv;
        };
        auto wstate = std::make_shared<WaitState>();

        for (size_t i = 0; i < totalVfs; ++i) {
            prefetchResults[i].eKeyHash = keyHash64(vfsEKeys[i]);

            // Resolve eKey → cKey → encoding entry to find CDN path.
            auto ckIt = vfsEKeyToCKey.find(prefetchResults[i].eKeyHash);
            const EncodingEntry* enc = nullptr;
            if (ckIt != vfsEKeyToCKey.end())
                enc = encodingTable.findByCKey(ckIt->second);

            if (!enc) {
                // Can't resolve — signal completion immediately.
                if (wstate->completed.fetch_add(1, std::memory_order_acq_rel) + 1 == totalVfs) {
                    std::lock_guard<std::mutex> lk(wstate->mtx);
                    wstate->cv.notify_one();
                }
                continue;
            }

            // Check archive index first, then fall back to loose file.
            auto idxEntry = onlineIndex.find(eKeyTrunc(enc->eKey));
            if (idxEntry) {
                auto archiveKeyHex = storages::common::hexEncode16(
                    cdnConfig.archiveEKeys[idxEntry->archiveIndex]);
                fetcher->fetchRangeAsync(archiveKeyHex, idxEntry->archiveOffset,
                                          idxEntry->encodedSize,
                    [&prefetchResults, i, wstate, totalVfs](std::optional<std::vector<u8>> data) {
                        if (data) prefetchResults[i].blteData = std::move(*data);
                        if (wstate->completed.fetch_add(1, std::memory_order_acq_rel) + 1 == totalVfs) {
                            std::lock_guard<std::mutex> lk(wstate->mtx);
                            wstate->cv.notify_one();
                        }
                    });
            } else {
                auto eKeyHex = storages::common::hexEncode16(enc->eKey);
                fetcher->fetchAsync("data", eKeyHex,
                    [&prefetchResults, i, wstate, totalVfs](std::optional<std::vector<u8>> data) {
                        if (data) prefetchResults[i].blteData = std::move(*data);
                        if (wstate->completed.fetch_add(1, std::memory_order_acq_rel) + 1 == totalVfs) {
                            std::lock_guard<std::mutex> lk(wstate->mtx);
                            wstate->cv.notify_one();
                        }
                    });
            }
        }

        // Wait for all parallel fetches to complete.
        {
            std::unique_lock<std::mutex> lk(wstate->mtx);
            wstate->cv.wait_for(lk, std::chrono::seconds(120), [&] {
                return wstate->completed.load(std::memory_order_acquire) >= totalVfs;
            });
        }

        // Batch-decode all BLTE blobs in parallel.
        std::vector<BlteBatchEntry> batchEntries;
        std::vector<size_t> batchToResult;  // Maps batch index → prefetchResults index.
        batchEntries.reserve(totalVfs);
        batchToResult.reserve(totalVfs);
        for (size_t i = 0; i < totalVfs; ++i) {
            if (prefetchResults[i].blteData.empty()) continue;
            BlteBatchEntry entry;
            entry.blteData = prefetchResults[i].blteData;
            batchEntries.push_back(entry);
            batchToResult.push_back(i);
        }

        auto batchDecoded = blteDecodeBatch(batchEntries, &keyRing, pool);

        std::unordered_map<u64, std::vector<u8>> prefetchedVfs;
        prefetchedVfs.reserve(batchDecoded.size());
        for (size_t d = 0; d < batchDecoded.size(); ++d) {
            if (batchDecoded[d].success) {
                size_t ri = batchToResult[d];
                prefetchedVfs[prefetchResults[ri].eKeyHash] = std::move(batchDecoded[d].data);
            }
        }

        VfsResolver vfsResolver = [this, &vfsEKeyToCKey, &prefetchedVfs](std::span<const u8> eKey) -> std::vector<u8> {
            // Check pre-fetched cache first.
            auto hash = keyHash64(eKey.data());
            auto pfIt = prefetchedVfs.find(hash);
            if (pfIt != prefetchedVfs.end())
                return pfIt->second;

            // Fallback: live resolve (shouldn't normally be needed).
            std::array<u8, 16> eKey16{};
            std::memcpy(eKey16.data(), eKey.data(), std::min(eKey.size(), size_t(16)));
            auto result = resolveEKey(eKey16);
            if (!result.empty()) return result;

            auto it = vfsEKeyToCKey.find(hash);
            if (it != vfsEKeyToCKey.end())
                return resolveCKey(it->second);
            return {};
        };

        auto vfsData = resolveCKey(buildConfig.vfsRootCKey);
        if (!vfsData.empty()) {
            auto tvfsRoot = TvfsRoot::parse(vfsData, vfsResolver, vfsEKeys, pool);
            if (tvfsRoot)
                root = std::move(tvfsRoot);
        }
    }

    if (!root) {
        auto rootData = resolveCKey(buildConfig.rootCKey);
        if (rootData.empty()) {
            s_onlineLastError = kRootNotFound;
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
        s_onlineLastError = kRootParseFailed;
        return false;
    }

    return true;
}

// ============================================================================
// Construction / Destruction
// ============================================================================

OnlineStorage::OnlineStorage() = default;
OnlineStorage::~OnlineStorage() = default;
OnlineStorage::OnlineStorage(OnlineStorage&&) noexcept = default;
OnlineStorage& OnlineStorage::operator=(OnlineStorage&&) noexcept = default;

OnlineStorage::operator bool() const noexcept {
    return m_impl && m_impl->isValid;
}

void OnlineStorage::close() {
    m_impl.reset();
}

// ============================================================================
// open()
// ============================================================================

std::optional<OnlineStorage> OnlineStorage::open(const OnlineOpenOptions& opts) {
    s_onlineLastError = kOk;

    // Step 1: Validate.
    if (!opts.http) {
        s_onlineLastError = kNoHttpHandler;
        return std::nullopt;
    }

    OnlineStorage storage;
    storage.m_impl = std::make_unique<Impl>();
    auto& impl = *storage.m_impl;

    impl.http = opts.http;
    impl.pool = opts.pool;
    impl.localeMask = opts.localeMask;

    // Set up disk cache.
    if (!opts.cacheDir.empty())
        impl.cache = std::make_unique<CdnCache>(opts.cacheDir);

    // Set up memory cache.
    impl.memCache = std::make_unique<MemoryCache>(opts.memoryCacheSize);

    ProgressCallback progress = opts.progressCallback;
    auto reportProgress = [&](ProgressStep step, u32 current = 0, u32 total = 0) -> bool {
        if (progress) return progress(step, current, total);
        return true;
    };

    // Steps 2+3: Discover CDN servers and active version in parallel.
    // Both are independent HTTP requests to the version server.
    std::array<u8, 16> buildConfigKey{};
    std::array<u8, 16> cdnConfigKey{};

    if (!opts.cdnServers.empty() &&
        !opts.directBuildConfigKey.empty() && !opts.directCdnConfigKey.empty()) {
        // Everything provided directly — no network discovery needed.
        impl.cdnServers = opts.cdnServers;
        buildConfigKey = storages::common::hexDecode16(opts.directBuildConfigKey);
        cdnConfigKey = storages::common::hexDecode16(opts.directCdnConfigKey);
    } else {
        // Fire CDN + version requests in parallel.
        struct DiscoveryState {
            std::optional<std::vector<u8>> cdnsData;
            std::optional<std::vector<u8>> versionsData;
            std::atomic<u32> done{0};
            std::mutex mtx;
            std::condition_variable cv;
        };
        auto ds = std::make_shared<DiscoveryState>();

        bool needCdns = opts.cdnServers.empty();
        bool needVersions = opts.directBuildConfigKey.empty() ||
                            opts.directCdnConfigKey.empty();

        const u32 totalRequests = (needCdns ? 1u : 0u) + (needVersions ? 1u : 0u);
        if (needCdns) {
            std::string cdnsUrl = "https://" + opts.region +
                                  ".version.battle.net/v2/products/" + opts.product + "/cdns";
            impl.http->getAsync(cdnsUrl, [ds, totalRequests](interfaces::HttpResponse resp) {
                if (resp.statusCode == 200) ds->cdnsData = std::move(resp.body);
                if (ds->done.fetch_add(1, std::memory_order_acq_rel) + 1 == totalRequests) {
                    std::lock_guard<std::mutex> lk(ds->mtx);
                    ds->cv.notify_one();
                }
            });
        }
        if (needVersions) {
            std::string versionsUrl = "https://" + opts.region +
                                      ".version.battle.net/v2/products/" + opts.product + "/versions";
            impl.http->getAsync(versionsUrl, [ds, totalRequests](interfaces::HttpResponse resp) {
                if (resp.statusCode == 200) ds->versionsData = std::move(resp.body);
                if (ds->done.fetch_add(1, std::memory_order_acq_rel) + 1 == totalRequests) {
                    std::lock_guard<std::mutex> lk(ds->mtx);
                    ds->cv.notify_one();
                }
            });
        }

        if (totalRequests > 0) {
            std::unique_lock<std::mutex> lk(ds->mtx);
            ds->cv.wait_for(lk, std::chrono::seconds(60), [&] {
                return ds->done.load(std::memory_order_acquire) >= totalRequests;
            });
        }

        // Process CDN servers.
        if (needCdns) {
            if (!ds->cdnsData) {
                s_onlineLastError = kCdnInfoNotFound;
                return std::nullopt;
            }
            auto cdnInfos = parseCdnsResponse(*ds->cdnsData);
            for (auto& ci : cdnInfos) {
                if (ci.region == opts.region || opts.region.empty()) {
                    for (auto& host : ci.hosts) {
                        CdnServer srv;
                        srv.host = host;
                        srv.path = ci.path;
                        srv.configPath = ci.configPath;
                        impl.cdnServers.push_back(std::move(srv));
                    }
                }
            }
            if (impl.cdnServers.empty()) {
                s_onlineLastError = kCdnInfoNotFound;
                return std::nullopt;
            }
        } else {
            impl.cdnServers = opts.cdnServers;
        }

        // Process versions.
        if (needVersions) {
            if (!ds->versionsData) {
                s_onlineLastError = kVersionInfoNotFound;
                return std::nullopt;
            }
            auto versions = parseVersionsResponse(*ds->versionsData);
            const VersionInfo* selected = nullptr;
            for (auto& v : versions) {
                if (v.region == opts.region) { selected = &v; break; }
            }
            if (!selected && !versions.empty()) selected = &versions[0];
            if (!selected) {
                s_onlineLastError = kVersionInfoNotFound;
                return std::nullopt;
            }
            buildConfigKey = selected->buildConfigKey;
            cdnConfigKey = selected->cdnConfigKey;
            impl.productInfo.version = selected->versionName;
            impl.productInfo.buildId = std::to_string(selected->buildId);
        } else {
            buildConfigKey = storages::common::hexDecode16(opts.directBuildConfigKey);
            cdnConfigKey = storages::common::hexDecode16(opts.directCdnConfigKey);
        }
    }

    // Override build key if requested.
    if (!opts.buildKey.empty())
        buildConfigKey = storages::common::hexDecode16(opts.buildKey);

    // Create fetcher (needs servers).
    impl.fetcher = std::make_unique<CdnFetcher>(impl.http, impl.cdnServers,
                                                  impl.cache.get());

    // Steps 4+5: Fetch build config + CDN config in parallel.
    if (!reportProgress(ProgressStep::LoadingBuildConfig)) return std::nullopt;
    {
        struct ConfigState {
            std::optional<std::vector<u8>> buildData;
            std::optional<std::vector<u8>> cdnData;
            std::atomic<u32> done{0};
            std::mutex mtx;
            std::condition_variable cv;
        };
        auto cs = std::make_shared<ConfigState>();

        impl.fetcher->fetchAsync("config", storages::common::hexEncode16(buildConfigKey),
            [cs](std::optional<std::vector<u8>> data) {
                cs->buildData = std::move(data);
                if (cs->done.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
                    std::lock_guard<std::mutex> lk(cs->mtx);
                    cs->cv.notify_one();
                }
            });
        impl.fetcher->fetchAsync("config", storages::common::hexEncode16(cdnConfigKey),
            [cs](std::optional<std::vector<u8>> data) {
                cs->cdnData = std::move(data);
                if (cs->done.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
                    std::lock_guard<std::mutex> lk(cs->mtx);
                    cs->cv.notify_one();
                }
            });

        std::unique_lock<std::mutex> lk(cs->mtx);
        cs->cv.wait_for(lk, std::chrono::seconds(60), [&] {
            return cs->done.load(std::memory_order_acquire) >= 2;
        });

        if (!cs->buildData) {
            s_onlineLastError = kHttpRequestFailed;
            return std::nullopt;
        }
        impl.buildConfig = parseBuildConfig(*cs->buildData);
        impl.productInfo.name = impl.buildConfig.buildProduct;

        if (!cs->cdnData) {
            s_onlineLastError = kHttpRequestFailed;
            return std::nullopt;
        }
        impl.cdnConfig = parseCdnConfig(*cs->cdnData);
    }

    // Step 6: Fetch archive index files + speculatively fetch encoding table.
    // The encoding table is the largest download, so start fetching it as a
    // loose CDN file in parallel with the index files. If it's not available
    // as a loose file, loadEncodingAndRoot() will fall back to the index.
    if (!reportProgress(ProgressStep::LoadingIndexFiles)) return std::nullopt;

    // Fire speculative encoding fetch (loose file, no index needed).
    std::shared_ptr<std::vector<u8>> speculativeEncoding;
    bool skipSpeculative = (opts.flags & StorageFeatureFlags::LoadOnDemand) != 0;
    if (!skipSpeculative) {
        struct EncFetchState {
            std::vector<u8> data;
            std::atomic<bool> done{false};
            std::mutex mtx;
            std::condition_variable cv;
        };
        auto encState = std::make_shared<EncFetchState>();
        speculativeEncoding = std::make_shared<std::vector<u8>>();

        auto encodingEKeyHex = storages::common::hexEncode16(impl.buildConfig.encodingEKey);
        impl.fetcher->fetchAsync("data", encodingEKeyHex,
            [encState, speculativeEncoding](std::optional<std::vector<u8>> data) {
                if (data)
                    *speculativeEncoding = std::move(*data);
                encState->done.store(true, std::memory_order_release);
                std::lock_guard<std::mutex> lk(encState->mtx);
                encState->cv.notify_one();
            });

        // Index loading happens next — encoding fetch runs concurrently.
        impl.onlineIndex = OnlineIndexTable::loadAll(*impl.fetcher,
                                                      impl.cdnConfig.archiveEKeys,
                                                      impl.pool);

        // Wait for speculative encoding fetch if not done yet.
        if (!encState->done.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lk(encState->mtx);
            encState->cv.wait_for(lk, std::chrono::seconds(60), [&] {
                return encState->done.load(std::memory_order_acquire);
            });
        }
    } else {
        impl.onlineIndex = OnlineIndexTable::loadAll(*impl.fetcher,
                                                      impl.cdnConfig.archiveEKeys,
                                                      impl.pool);
    }

    // Load loose file index if available.
    if (!impl.cdnConfig.fileIndex.empty()) {
        impl.looseIndex = OnlineIndexTable::loadLoose(*impl.fetcher,
                                                       impl.cdnConfig.fileIndex);
    }

    // Create data source.
    impl.dataSource = std::make_unique<OnlineDataSource>(
        impl.fetcher.get(), &impl.onlineIndex, &impl.looseIndex,
        &impl.cdnConfig.archiveEKeys);

    // Step 7-8: Load encoding + root (or defer).
    if (opts.flags & StorageFeatureFlags::LoadOnDemand) {
        impl.deferMode = true;
    } else {
        if (!reportProgress(ProgressStep::LoadingEncodingTable)) return std::nullopt;
        // Pass speculatively-fetched encoding BLTE data if available.
        std::vector<u8> preEnc;
        if (speculativeEncoding && !speculativeEncoding->empty())
            preEnc = std::move(*speculativeEncoding);
        if (!impl.loadEncodingAndRoot(preEnc)) return std::nullopt;
    }

    if (!reportProgress(ProgressStep::Ready)) return std::nullopt;
    impl.isValid = true;
    return storage;
}

// ============================================================================
// readFile
// ============================================================================

std::optional<std::vector<u8>> OnlineStorage::readFile(const std::string& cascPath) const {
    return readFile(cascPath, 0, 0);
}

std::optional<std::vector<u8>> OnlineStorage::readFile(const std::string& cascPath,
                                                        u32 localeFlags, u32 /*openFlags*/) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto normalized = normalizeCascPath(cascPath);
    auto entries = m_impl->root->findByPath(normalized);

    u32 locale = localeFlags != 0 ? localeFlags : m_impl->localeMask;
    return m_impl->resolveRootEntry(entries, locale);
}

std::optional<std::vector<u8>> OnlineStorage::readFile(i32 fileId) const {
    return readFile(fileId, 0, 0);
}

std::optional<std::vector<u8>> OnlineStorage::readFile(i32 fileId,
                                                        u32 localeFlags, u32 /*openFlags*/) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByFileDataId(static_cast<u32>(fileId));

    u32 locale = localeFlags != 0 ? localeFlags : m_impl->localeMask;
    return m_impl->resolveRootEntry(entries, locale);
}

// ============================================================================
// fileExists
// ============================================================================

bool OnlineStorage::fileExists(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return false;
    if (!m_impl->ensureLoaded()) return false;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByPath(normalizeCascPath(cascPath));
    return !entries.empty();
}

bool OnlineStorage::fileExists(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return false;
    if (!m_impl->ensureLoaded()) return false;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByFileDataId(static_cast<u32>(fileId));
    return !entries.empty();
}

// ============================================================================
// fileSize / fileInfo
// ============================================================================

std::optional<u64> OnlineStorage::fileSize(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByPath(normalizeCascPath(cascPath));
    if (entries.empty()) return std::nullopt;
    auto encEntry = m_impl->resolveEncoding(entries[0]);
    if (!encEntry) return std::nullopt;
    return encEntry->fileSize;
}

std::optional<u64> OnlineStorage::fileSize(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);

    auto entries = m_impl->root->findByFileDataId(static_cast<u32>(fileId));
    if (entries.empty()) return std::nullopt;
    auto encEntry = m_impl->resolveEncoding(entries[0]);
    if (!encEntry) return std::nullopt;
    return encEntry->fileSize;
}

std::optional<FileFullInfo> OnlineStorage::fileInfo(const std::string& cascPath) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->fileInfoResolved(
        m_impl->root->findByPath(normalizeCascPath(cascPath)));
}

std::optional<FileFullInfo> OnlineStorage::fileInfo(i32 fileId) const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->fileInfoResolved(
        m_impl->root->findByFileDataId(static_cast<u32>(fileId)));
}

// ============================================================================
// enumerate / listFiles / listEntries / totalFileCount
// ============================================================================

void OnlineStorage::enumerate(std::function<bool(const FindEntry&)> callback) const {
    if (!m_impl || !m_impl->isValid || !callback) return;
    if (!m_impl->ensureLoaded()) return;
    std::shared_lock lock(m_impl->mutex);

    std::unordered_set<u64> rootKeyHashes;

    m_impl->root->enumerate([&](const RootEntry& re) -> bool {
        if (!isZeroKey(re.cKey))
            rootKeyHashes.insert(keyHash64(re.cKey));

        FindEntry fe;
        fe.cKey = re.cKey;
        fe.localeFlags = re.localeFlags;
        fe.contentFlags = re.contentFlags;
        fe.fileDataId = static_cast<i32>(re.fileDataId);
        fe.path = re.path;

        auto encEntry = m_impl->resolveEncoding(re);
        if (encEntry) {
            fe.fileSize = encEntry->fileSize;
            fe.cKey = encEntry->cKey;
            rootKeyHashes.insert(keyHash64(encEntry->cKey));
        }

        return callback(fe);
    });

    // Emit encoding-table orphans.
    static constexpr char kHex[] = "0123456789abcdef";
    for (auto& enc : m_impl->encodingTable.entries()) {
        if (isZeroKey(enc.cKey)) continue;
        if (rootKeyHashes.count(keyHash64(enc.cKey))) continue;

        char hexBuf[33];
        for (int i = 0; i < 16; ++i) {
            hexBuf[i * 2]     = kHex[enc.cKey[i] >> 4];
            hexBuf[i * 2 + 1] = kHex[enc.cKey[i] & 0xF];
        }
        hexBuf[32] = '\0';

        FindEntry fe;
        fe.cKey = enc.cKey;
        fe.fileSize = enc.fileSize;
        fe.path = hexBuf;

        if (!callback(fe)) break;
    }
}

std::vector<std::string> OnlineStorage::listFiles() const {
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

std::vector<FindEntry> OnlineStorage::listEntries() const {
    std::vector<FindEntry> result;
    enumerate([&](const FindEntry& fe) {
        result.push_back(fe);
        return true;
    });
    return result;
}

std::optional<u32> OnlineStorage::totalFileCount() const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    if (!m_impl->ensureLoaded()) return std::nullopt;
    return static_cast<u32>(m_impl->root->entryCount());
}

// ============================================================================
// readBatch
// ============================================================================

std::vector<BatchReadResult> OnlineStorage::readBatch(
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

    // Phase 0: Root lookup.
    struct ResolveWork {
        size_t requestIndex;
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
            results[i].error = "empty request";
            continue;
        }

        std::vector<RootEntry> entries;
        if (m_impl->root) {
            if (byPath)
                entries = m_impl->root->findByPath(normalizeCascPath(req.path));
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

    if (toResolve.empty()) return results;

    // Phase 1: Resolve root entry → fetch BLTE blob.
    struct ResolvedBlob {
        std::vector<u8> blteData;
        bool resolved = false;
        std::string error;
    };
    std::vector<ResolvedBlob> resolvedBlobs(toResolve.size());

    auto resolveOne = [&](size_t idx) {
        auto& work = toResolve[idx];
        auto& blob = resolvedBlobs[idx];

        const RootEntry* best = selectBestEntry(work.rootEntries, work.localeFlags);
        if (!best) { blob.error = "no matching root entry"; return; }

        // Resolve CKey/EKey → index → fetch BLTE data from CDN.
        const OnlineIndexTable::Entry* idxEntry = nullptr;
        std::array<u8, 16> eKey{};

        if (!isZeroKey(best->cKey)) {
            auto encEntry = m_impl->encodingTable.findByCKey(best->cKey, kEKeyTruncSize);
            if (encEntry) {
                eKey = encEntry->eKey;
                idxEntry = m_impl->onlineIndex.find(eKeyTrunc(encEntry->eKey));
            }
        }
        if (!idxEntry && !isZeroKey(best->eKey)) {
            eKey = best->eKey;
            idxEntry = m_impl->onlineIndex.find(eKeyTrunc(best->eKey));
        }

        if (idxEntry) {
            blob.blteData = m_impl->dataSource->fetchBlte(
                idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize);
        } else if (!isZeroKey(eKey)) {
            blob.blteData = m_impl->dataSource->fetchBlte(eKey);
        }

        if (blob.blteData.empty()) {
            blob.error = "failed to fetch BLTE data from CDN";
            return;
        }
        blob.resolved = true;
    };

    // Serial resolution (CDN fetches are inherently network-bound;
    // parallel resolution is done at the HTTP layer via async callbacks).
    for (size_t idx = 0; idx < toResolve.size(); ++idx)
        resolveOne(idx);

    // Phase 2: Batch BLTE decode.
    std::vector<BlteBatchEntry> batchEntries;
    std::vector<size_t> batchToResolveIdx;
    batchEntries.reserve(toResolve.size());
    batchToResolveIdx.reserve(toResolve.size());

    for (size_t idx = 0; idx < toResolve.size(); ++idx) {
        if (!resolvedBlobs[idx].resolved) {
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

// ============================================================================
// product
// ============================================================================

std::optional<StorageProduct> OnlineStorage::product() const {
    if (!m_impl || !m_impl->isValid) return std::nullopt;
    return m_impl->productInfo;
}

// ============================================================================
// Encryption key management
// ============================================================================

bool OnlineStorage::addEncryptionKey(u64 keyName, const std::array<u8, 16>& key) {
    if (!m_impl) return false;
    m_impl->keyRing.addKey(keyName, key);
    return true;
}

bool OnlineStorage::addEncryptionKey(u64 keyName, const std::string& keyHex) {
    if (!m_impl) return false;
    m_impl->keyRing.addKey(keyName, keyHex);
    return true;
}

bool OnlineStorage::importKeysFromString(const std::string& keyList) {
    if (!m_impl) return false;
    return m_impl->keyRing.importFromString(keyList);
}

bool OnlineStorage::importKeysFromFile(const std::string& keyFilePath) {
    if (!m_impl) return false;
    return m_impl->keyRing.importFromFile(keyFilePath);
}

std::optional<std::array<u8, 16>> OnlineStorage::findEncryptionKey(u64 keyName) const {
    if (!m_impl) return std::nullopt;
    auto key = m_impl->keyRing.findKey(keyName);
    if (key) return *key;
    return std::nullopt;
}

// ============================================================================
// Cache management
// ============================================================================

void OnlineStorage::flushCache() {
    // CdnCache writes are synchronous (write-through), so nothing to flush.
}

bool OnlineStorage::prefetch() {
    if (!m_impl || !m_impl->isValid) return false;
    return m_impl->ensureLoaded();
}

u32 OnlineStorage::lastError() noexcept {
    return s_onlineLastError;
}

} // namespace whiteout::storages::casc

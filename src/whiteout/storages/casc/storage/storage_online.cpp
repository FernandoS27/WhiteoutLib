// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file storage_online.cpp
/// @brief Online (CDN) factory, VFS prefetch via async HTTP.

#include "storage_impl.h"
#include "storage_backend_impl.h"
#include "constants.h"

#include <atomic>
#include <chrono>
#include <condition_variable>

namespace whiteout::storages::casc {

// ============================================================================
// prefetchVfsOnline — parallel async HTTP + batch BLTE decode
// ============================================================================

std::unordered_map<u64, std::vector<u8>> prefetchVfsOnline(
    const Storage::Impl& impl,
    const std::vector<std::array<u8, 16>>& vfsEKeys,
    const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey) {

    if (!impl.onlineState || !impl.onlineState->fetcher)
        return {};

    const size_t totalVfs = vfsEKeys.size();
    if (totalVfs == 0) return {};

    // Pre-fetch results.
    struct PrefetchResult {
        u64 eKeyHash = 0;
        std::vector<u8> blteData;
    };
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
            enc = impl.encodingTable.findByCKey(ckIt->second);

        if (!enc) {
            if (wstate->completed.fetch_add(1, std::memory_order_acq_rel) + 1 == totalVfs) {
                std::lock_guard<std::mutex> lk(wstate->mtx);
                wstate->cv.notify_one();
            }
            continue;
        }

        // Check archive index first, then fall back to loose file.
        auto idxEntry = impl.onlineState->onlineIndex.find(eKeyTrunc(enc->eKey));
        if (idxEntry) {
            impl.onlineState->dataSource->fetchBlteAsync(
                idxEntry->archiveIndex, idxEntry->archiveOffset, idxEntry->encodedSize,
                [&prefetchResults, i, wstate, totalVfs](std::optional<std::vector<u8>> data) {
                    if (data) prefetchResults[i].blteData = std::move(*data);
                    if (wstate->completed.fetch_add(1, std::memory_order_acq_rel) + 1 == totalVfs) {
                        std::lock_guard<std::mutex> lk(wstate->mtx);
                        wstate->cv.notify_one();
                    }
                });
        } else {
            impl.onlineState->dataSource->fetchBlteAsync(
                enc->eKey,
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
    std::vector<size_t> batchToResult;
    batchEntries.reserve(totalVfs);
    batchToResult.reserve(totalVfs);
    for (size_t i = 0; i < totalVfs; ++i) {
        if (prefetchResults[i].blteData.empty()) continue;
        BlteBatchEntry entry;
        entry.blteData = prefetchResults[i].blteData;
        batchEntries.push_back(entry);
        batchToResult.push_back(i);
    }

    auto batchDecoded = blteDecodeBatch(batchEntries, &impl.keyRing, impl.pool);

    std::unordered_map<u64, std::vector<u8>> vfsCache;
    vfsCache.reserve(batchDecoded.size());
    for (size_t d = 0; d < batchDecoded.size(); ++d) {
        if (batchDecoded[d].success) {
            size_t ri = batchToResult[d];
            vfsCache[prefetchResults[ri].eKeyHash] = std::move(batchDecoded[d].data);
        }
    }

    return vfsCache;
}

// ============================================================================
// Storage::openOnline
// ============================================================================

std::optional<Storage> Storage::openOnline(const OnlineOpenOptions& opts) {
    s_lastError = kOk;

    if (!opts.http) {
        s_lastError = kNoHttpHandler;
        return std::nullopt;
    }

    auto implPtr = std::make_unique<Impl>();
    auto& impl = *implPtr;
    impl.pool = opts.pool;
    impl.localeMask = opts.localeMask;
    impl.featureFlags = opts.flags;
    impl.listfileData = opts.listfile;

    impl.onlineState = std::make_unique<OnlineState>();
    impl.onlineState->http = opts.http;

    // Set up disk cache.
    if (!opts.cacheDir.empty())
        impl.onlineState->cache = std::make_unique<CdnCache>(opts.cacheDir);

    // Set up memory cache.
    impl.memCache = std::make_unique<MemoryCache>(opts.memoryCacheSize);

    auto reportProgress = [&](ProgressStep step, u32 current = 0, u32 total = 0) -> bool {
        if (opts.progressCallback) return opts.progressCallback(step, current, total);
        return true;
    };

    // Steps 2+3: Discover CDN servers and active version in parallel.
    std::array<u8, 16> buildConfigKey{};
    std::array<u8, 16> cdnConfigKey{};

    if (!opts.cdnServers.empty() &&
        !opts.directBuildConfigKey.empty() && !opts.directCdnConfigKey.empty()) {
        impl.onlineState->cdnServers = opts.cdnServers;
        buildConfigKey = storages::common::hexDecode16(opts.directBuildConfigKey);
        cdnConfigKey = storages::common::hexDecode16(opts.directCdnConfigKey);
    } else {
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
            opts.http->getAsync(cdnsUrl, [ds, totalRequests](interfaces::HttpResponse resp) {
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
            opts.http->getAsync(versionsUrl, [ds, totalRequests](interfaces::HttpResponse resp) {
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
                s_lastError = kCdnInfoNotFound;
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
                        impl.onlineState->cdnServers.push_back(std::move(srv));
                    }
                }
            }
            if (impl.onlineState->cdnServers.empty()) {
                s_lastError = kCdnInfoNotFound;
                return std::nullopt;
            }
        } else {
            impl.onlineState->cdnServers = opts.cdnServers;
        }

        // Process versions.
        if (needVersions) {
            if (!ds->versionsData) {
                s_lastError = kVersionInfoNotFound;
                return std::nullopt;
            }
            auto versions = parseVersionsResponse(*ds->versionsData);
            const VersionInfo* selected = nullptr;
            for (auto& v : versions) {
                if (v.region == opts.region) { selected = &v; break; }
            }
            if (!selected && !versions.empty()) selected = &versions[0];
            if (!selected) {
                s_lastError = kVersionInfoNotFound;
                return std::nullopt;
            }
            buildConfigKey = selected->buildConfigKey;
            cdnConfigKey = selected->cdnConfigKey;
            impl.onlineState->productInfo.version = selected->versionName;
            impl.onlineState->productInfo.buildId = std::to_string(selected->buildId);
        } else {
            buildConfigKey = storages::common::hexDecode16(opts.directBuildConfigKey);
            cdnConfigKey = storages::common::hexDecode16(opts.directCdnConfigKey);
        }
    }

    if (!opts.buildKey.empty())
        buildConfigKey = storages::common::hexDecode16(opts.buildKey);

    // Create fetcher.
    impl.onlineState->fetcher = std::make_unique<CdnFetcher>(
        impl.onlineState->http, impl.onlineState->cdnServers,
        impl.onlineState->cache.get());

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

        impl.onlineState->fetcher->fetchAsync(
            "config", storages::common::hexEncode16(buildConfigKey),
            [cs](std::optional<std::vector<u8>> data) {
                cs->buildData = std::move(data);
                if (cs->done.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
                    std::lock_guard<std::mutex> lk(cs->mtx);
                    cs->cv.notify_one();
                }
            });
        impl.onlineState->fetcher->fetchAsync(
            "config", storages::common::hexEncode16(cdnConfigKey),
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
            s_lastError = kHttpRequestFailed;
            return std::nullopt;
        }
        impl.buildConfig = parseBuildConfig(*cs->buildData);
        impl.onlineState->productInfo.name = impl.buildConfig.buildProduct;

        if (!cs->cdnData) {
            s_lastError = kHttpRequestFailed;
            return std::nullopt;
        }
        impl.cdnConfig = parseCdnConfig(*cs->cdnData);
    }

    // Step 6: Set up archive index (lazy or eager).
    if (!reportProgress(ProgressStep::LoadingIndexFiles)) return std::nullopt;

    if ((opts.flags & StorageFeatureFlags::LazyArchiveIndex) != 0) {
        impl.onlineState->onlineIndex = OnlineIndexTable::makeLazy(
            impl.onlineState->fetcher.get(), &impl.cdnConfig.archiveEKeys, impl.pool);
        // A listfile signals bulk path-based reads. The per-read lazy fault-in
        // does O(archives) serial .index fetches on a cold miss — pathological
        // at scale. Parallel-load every archive index up front instead, so the
        // cold-read path is a pure data fetch with no index round-trips.
        if (!opts.listfile.empty())
            impl.onlineState->onlineIndex.ensureAllLoaded();
    } else {
        impl.onlineState->onlineIndex = OnlineIndexTable::loadAll(
            *impl.onlineState->fetcher, impl.cdnConfig.archiveEKeys, impl.pool);
    }

    // Load loose file index if available.
    if (!impl.cdnConfig.fileIndex.empty()) {
        impl.onlineState->looseIndex = OnlineIndexTable::loadLoose(
            *impl.onlineState->fetcher, impl.cdnConfig.fileIndex);
    }

    // Create data source.
    impl.onlineState->dataSource = std::make_unique<OnlineDataSource>(
        impl.onlineState->fetcher.get(),
        &impl.onlineState->onlineIndex,
        &impl.onlineState->looseIndex,
        &impl.cdnConfig.archiveEKeys);
    impl.dataSource = impl.onlineState->dataSource.get();

    // Construct specialised backend (NoCachePolicy; D4 upgrades later).
    impl.backend = std::make_unique<StorageBackendImpl<OnlineDataTraits, NoCachePolicy>>(
        OnlineDataTraits{impl.onlineState->dataSource.get(), &impl.onlineState->onlineIndex},
        NoCachePolicy{},
        impl.encodingTable, impl.keyRing, impl.pool);

    // Step 7-8: Load encoding + root (or defer).
    if (opts.flags & StorageFeatureFlags::LoadOnDemand) {
        impl.deferMode = true;
    } else {
        if (!reportProgress(ProgressStep::LoadingEncodingTable)) return std::nullopt;
        if (!impl.loadEncodingAndRoot()) return std::nullopt;
    }

    if (!reportProgress(ProgressStep::Ready)) return std::nullopt;
    impl.isValid = true;
    Storage storage(std::move(implPtr));
    return storage;
}

} // namespace whiteout::storages::casc

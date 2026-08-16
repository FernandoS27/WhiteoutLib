// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file storage_local.cpp
/// @brief Local (disk) factory, archive mapping, VFS prefetch.

#include "../../common/md5.h"
#include "constants.h"
#include "storage_backend_impl.h"
#include "storage_impl.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace whiteout::storages::casc {

namespace {

std::vector<std::filesystem::path> scanLocalConfigs(const std::string& dataPath) {
    namespace fs = std::filesystem;
    std::vector<fs::path> out;

    fs::path const configRoot = fs::path(dataPath) / "config";
    std::error_code ec;
    if (!fs::exists(configRoot, ec) || !fs::is_directory(configRoot, ec))
        return out;

    auto isHex32 = [](const std::string& s) {
        if (s.size() != 32)
            return false;
        for (char const c : s) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
        return true;
    };

    for (auto& xx : fs::directory_iterator(configRoot, ec)) {
        if (!xx.is_directory())
            continue;
        for (auto& yy : fs::directory_iterator(xx.path(), ec)) {
            if (!yy.is_directory())
                continue;
            for (auto& f : fs::directory_iterator(yy.path(), ec)) {
                if (!f.is_regular_file())
                    continue;
                if (!isHex32(f.path().filename().string()))
                    continue;
                out.push_back(f.path());
            }
        }
    }

    // Newest first: a patched install keeps every superseded config around, and
    // the one written last is the build the storage is actually at.
    std::vector<std::pair<fs::file_time_type, fs::path>> stamped;
    stamped.reserve(out.size());
    for (auto& p : out) {
        std::error_code tec;
        auto when = fs::last_write_time(p, tec);
        stamped.emplace_back(tec ? fs::file_time_type::min() : when, p);
    }
    std::sort(stamped.begin(), stamped.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second < b.second;
    });

    out.clear();
    for (auto& [when, path] : stamped)
        out.push_back(path);
    return out;
}

bool keyNonZero(const std::array<u8, 16>& key) {
    for (u8 const b : key)
        if (b)
            return true;
    return false;
}

std::optional<BuildConfig> findConsistentBuildConfig(const std::string& dataPath,
                                                     const IndexTable& indexTable) {

    for (auto& path : scanLocalConfigs(dataPath)) {
        auto data = storages::common::readFileFully(path.string());
        if (!data)
            continue;

        auto cfg = parseBuildConfig(*data);
        if (!keyNonZero(cfg.encodingEKey))
            continue;

        if (indexTable.find(std::span(cfg.encodingEKey.data(), 9)))
            return cfg;
    }
    return std::nullopt;
}

std::optional<CdnConfig> findConsistentCdnConfig(const std::string& dataPath) {
    namespace fs = std::filesystem;

    fs::path const indicesRoot = fs::path(dataPath) / "indices";
    std::error_code ec;
    if (!fs::exists(indicesRoot, ec) || !fs::is_directory(indicesRoot, ec))
        return std::nullopt;

    std::unordered_set<std::string> localArchives;
    for (auto& e : fs::directory_iterator(indicesRoot, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".index")
            localArchives.insert(e.path().stem().string());
    }
    if (localArchives.empty())
        return std::nullopt;

    auto toHex = [](const std::array<u8, 16>& key) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string s;
        s.reserve(32);
        for (u8 const b : key) {
            s += hex[b >> 4];
            s += hex[b & 0xF];
        }
        return s;
    };

    std::optional<CdnConfig> best;
    size_t bestOverlap = 0;

    for (auto& path : scanLocalConfigs(dataPath)) {
        auto data = storages::common::readFileFully(path.string());
        if (!data)
            continue;

        auto cfg = parseCdnConfig(*data);
        if (cfg.archiveEKeys.empty())
            continue; // not a CDN config

        size_t overlap = 0;
        for (auto& k : cfg.archiveEKeys)
            if (localArchives.count(toHex(k)))
                ++overlap;

        if (overlap > bestOverlap) {
            bestOverlap = overlap;
            best = std::move(cfg);
        }
    }
    return best;
}

} // namespace

// ============================================================================
// LocalState helpers
// ============================================================================

bool LocalState::mapArchives(std::string* error, const ProgressSink* sink) {
    namespace fs = std::filesystem;

    std::string const dataSubdir = dataPath + "/data";
    if (!fs::exists(dataSubdir)) {
        if (error)
            *error = "Data subdirectory not found: " + dataSubdir;
        return false;
    }

    // Discover the max archive index.
    u32 maxIndex = 0;
    for (auto& entry : fs::directory_iterator(dataSubdir)) {
        auto name = entry.path().filename().string();
        if (name.size() >= 8 && name.substr(0, 5) == "data.") {
            u32 const idx = std::stoul(name.substr(5));
            if (idx > maxIndex)
                maxIndex = idx;
        }
    }

    dataArchives.resize(maxIndex + 1);
    bool sawSharingViolation = false;
    std::string firstFailure;
    for (u32 i = 0; i <= maxIndex; ++i) {
        char archiveName[32];
        std::snprintf(archiveName, sizeof(archiveName), "data/data.%03u", i);
        std::string const path = dataPath + "/" + archiveName;
        if (fs::exists(path)) {
            std::string mapErr;
            auto mapped = storages::common::MappedFile::open(
                path, storages::common::AccessHint::Random, &mapErr);
            if (mapped) {
                dataArchives[i] = std::move(*mapped);
            } else if (firstFailure.empty()) {
                firstFailure = "Failed to map '" + path + "': " + mapErr;
                if (storages::common::isSharingViolation(mapErr))
                    sawSharingViolation = true;
            }
        }
        if (sink && !(*sink)(i + 1, maxIndex + 1, archiveName))
            break;
    }

    if (!firstFailure.empty() && error)
        *error = firstFailure;
    if (sawSharingViolation)
        s_lastError = kSharingViolation;
    return true;
}

// ============================================================================
// prefetchVfsLocal — parallel resolve using JobGroup
// ============================================================================

/// Sub-manifests above this encoded size are decoded on the calling thread with
/// the pool, so their BLTE frames spread across all workers. WoW retail's
/// sub-manifest sizes are extremely skewed — one is ~140 MB, half of all VFS
/// bytes, and giving it a single worker sets the makespan for the whole phase.
static constexpr u32 kVfsFrameParallelBytes = 4u << 20;

std::unordered_map<u64, std::vector<u8>> prefetchVfsLocal(
    const Storage::Impl& impl, const std::vector<std::array<u8, 16>>& vfsEKeys,
    const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey, const ProgressSink* sink) {

    std::unordered_map<u64, std::vector<u8>> vfsCache;

    struct SubResult {
        u64 hash = 0;
        std::vector<u8> data;
    };
    std::vector<SubResult> results(vfsEKeys.size());
    std::atomic<u64> resolved{0};
    u64 const totalVfs = vfsEKeys.size();

    auto resolveOne = [&](size_t i, interfaces::WorkerPool* framePool) {
        std::array<u8, 16> eKey16{};
        std::memcpy(eKey16.data(), vfsEKeys[i].data(), std::min(size_t(16), vfsEKeys[i].size()));
        results[i].hash = keyHash64(eKey16);
        results[i].data = impl.resolveEKey(eKey16, framePool);
        if (results[i].data.empty()) {
            auto cIt = vfsEKeyToCKey.find(results[i].hash);
            if (cIt != vfsEKeyToCKey.end())
                results[i].data = impl.resolveCKey(cIt->second, framePool);
        }
        if (sink)
            (*sink)(resolved.fetch_add(1, std::memory_order_relaxed) + 1, totalVfs, {});
    };

    if (impl.pool && vfsEKeys.size() > 1) {
        // Split by encoded size so the few large manifests get frame-level
        // parallelism instead of competing for one worker each. Every index
        // lookup happens here, before the first task is submitted.
        std::vector<u32> encodedSize(vfsEKeys.size(), 0);
        std::vector<size_t> small;
        std::vector<size_t> large;
        for (size_t i = 0; i < vfsEKeys.size(); ++i) {
            auto loc = impl.backend ? impl.backend->findInIndex(eKeyTrunc(vfsEKeys[i]))
                                    : impl.dataSource->findInIndex(eKeyTrunc(vfsEKeys[i]));
            encodedSize[i] = loc ? loc->encodedSize : 0;
            (encodedSize[i] >= kVfsFrameParallelBytes ? large : small).push_back(i);
        }
        std::sort(large.begin(), large.end(),
                  [&](size_t a, size_t b) { return encodedSize[a] > encodedSize[b]; });

        utils::JobGroup jobGroup;
        jobGroup.add(small.size());
        for (size_t const i : small) {
            interfaces::WorkerTask task;
            task.fn = [&, i]() {
                resolveOne(i, nullptr);
                jobGroup.done();
            };
            impl.pool->submit(task);
        }

        // The submitted tasks hold references to jobGroup and results, so the
        // wait has to happen even if resolving a large manifest throws.
        struct WaitOnExit {
            utils::JobGroup& group;
            ~WaitOnExit() {
                group.wait();
            }
        } const waitOnExit{jobGroup};

        // Largest first, on this thread, while the small ones run on the pool.
        for (size_t const i : large)
            resolveOne(i, impl.pool);
    } else {
        for (size_t i = 0; i < vfsEKeys.size(); ++i)
            resolveOne(i, nullptr);
    }

    for (auto& r : results) {
        if (!r.data.empty())
            vfsCache.emplace(r.hash, std::move(r.data));
    }
    return vfsCache;
}

// ============================================================================
// Storage::open (local disk)
// ============================================================================

std::optional<Storage> Storage::open(const std::string& path, interfaces::WorkerPool* pool) {
    OpenOptions opts;
    opts.path = path;
    opts.pool = pool;
    return open(opts);
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
    opts.errorOut = error;
    return open(opts);
}

std::optional<Storage> Storage::open(const std::string& path, const std::string& product,
                                     interfaces::WorkerPool* pool) {
    OpenOptions opts;
    opts.path = path;
    opts.product = product;
    opts.pool = pool;
    return open(opts);
}

std::optional<Storage> Storage::open(const OpenOptions& opts) {
    namespace fs = std::filesystem;
    s_lastError = kOk;

    // Determine basePath and dataPath.
    std::string basePath = opts.path;
    std::string dataPath;

    // Flavor subdirectory (e.g. ".../Warcraft III/_ptr_"): its `.flavor.info`
    // names the product, but the shared `.build.info` + `Data` live in the
    // parent. Read the flavor code and resolve the real storage from the parent.
    std::string flavorProduct;
    if (fs::exists(basePath + "/.flavor.info")) {
        std::string flavErr;
        if (auto flav = storages::common::readFileFully(basePath + "/.flavor.info", &flavErr))
            flavorProduct = parseFlavorInfo(*flav);
        bool const hasOwnStorage = fs::exists(basePath + "/.build.info") ||
                                   fs::exists(basePath + "/Data") || fs::exists(basePath + "/data");
        if (!hasOwnStorage) {
            std::string const parent = fs::path(basePath).parent_path().string();
            if (!parent.empty() && (fs::exists(parent + "/.build.info") ||
                                    fs::exists(parent + "/Data") || fs::exists(parent + "/data")))
                basePath = parent;
        }
    }

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
        dataPath = basePath;
    }

    basePath = fs::path(basePath).lexically_normal().string();
    dataPath = fs::path(dataPath).lexically_normal().string();

    // Progress front-end. Owned here until the Impl exists, then handed over so
    // a deferred load reports through the same callback.
    // Ordered the way the work actually runs: the index table has to exist
    // before a missing build config can be recovered by consistency, so it is
    // loaded first. Reading `.build.info` itself is sub-millisecond and gets no
    // step of its own — reporting it before the index load and the config read
    // after it would make the bar go backwards.
    std::vector<ProgressStep> plan{
        ProgressStep::LoadingIndexFiles,
        ProgressStep::LoadingBuildConfig,
        ProgressStep::LoadingCdnConfig,
        ProgressStep::MappingArchives,
    };
    if (!(opts.flags & StorageFeatureFlags::LoadOnDemand)) {
        plan.push_back(ProgressStep::LoadingEncodingTable);
        plan.push_back(ProgressStep::LoadingVfsManifests);
        plan.push_back(ProgressStep::LoadingRootManifest);
    }
    auto reporterOwned = std::make_unique<ProgressReporter>(opts.progressCallback, std::move(plan));
    ProgressReporter& progress = *reporterOwned;

    /// Records the cancellation and returns true, so a failed progress call
    /// reads as `if (!progress.begin(...) && failCancelled()) return {};`.
    auto failCancelled = [&]() {
        s_lastError = CascError::Cancelled;
        if (opts.errorOut)
            *opts.errorOut = "Open cancelled by the progress callback";
        return true;
    };

    // Step 1: Parse .build.info. An install can be missing it entirely — hand-
    // copied game directories and ones the Battle.net agent never finished
    // registering both happen — so a failure here is not fatal on its own: the
    // build and CDN configs are recovered from `config/` by consistency below.
    // The diagnostic is held until that recovery also comes up empty.
    std::string buildInfoPath;
    if (fs::exists(basePath + "/.build.info"))
        buildInfoPath = basePath + "/.build.info";
    else if (fs::exists(dataPath + "/.build.info"))
        buildInfoPath = dataPath + "/.build.info";

    auto reportFileError = [&](const std::string& filePath, const std::string& sysErr,
                               u32 errorCode) {
        if (storages::common::isSharingViolation(sysErr))
            s_lastError = kSharingViolation;
        else
            s_lastError = errorCode;
        if (opts.errorOut)
            *opts.errorOut = "Failed to read '" + filePath + "': " + sysErr;
    };

    std::string buildInfoError;
    bool buildInfoLocked = false;
    std::vector<BuildInfo> builds;

    if (buildInfoPath.empty()) {
        buildInfoError = "'.build.info' not found under '" + basePath + "' or '" + dataPath + "'";
    } else {
        std::string sysErr;
        if (auto buildInfoFile = storages::common::readFileFully(buildInfoPath, &sysErr)) {
            builds = parseBuildInfo(*buildInfoFile);
            if (builds.empty())
                buildInfoError = "'.build.info' parsed empty: " + buildInfoPath;
        } else {
            buildInfoError = "Failed to read '" + buildInfoPath + "': " + sysErr;
            buildInfoLocked = storages::common::isSharingViolation(sysErr);
        }
    }

    // Select the build to open. Precedence: explicit build key → product code
    // → first active → first row.
    const BuildInfo* activeBuild = nullptr;

    // 1. Exact build key (most specific).
    if (!opts.buildKey.empty()) {
        for (auto& b : builds) {
            if (storages::common::hexEncode16(b.buildKey) == opts.buildKey) {
                activeBuild = &b;
                break;
            }
        }
    }

    // 2. Product code name, e.g. "w3" (retail) vs "w3t" (PTR). An explicit
    //    OpenOptions::product wins; otherwise a flavor subdirectory's
    //    `.flavor.info` supplies it. Matched case-insensitively among active
    //    builds — mirrors CascLib's code name. A specified-but-absent product is
    //    a hard error, so the caller never silently gets the wrong build (e.g.
    //    retail when asking for PTR).
    std::string const productCode = !opts.product.empty() ? opts.product : flavorProduct;
    if (!activeBuild && !productCode.empty() && !builds.empty()) {
        std::string const wanted = storages::common::toLower(productCode);
        for (auto& b : builds) {
            if (b.active && storages::common::toLower(b.product) == wanted) {
                activeBuild = &b;
                break;
            }
        }
        if (!activeBuild) {
            s_lastError = kBuildInfoNotFound;
            if (opts.errorOut)
                *opts.errorOut = "No active build with product '" + productCode +
                                 "' in '.build.info' (" + buildInfoPath + ")";
            return std::nullopt;
        }
    }

    // 3. First active build.
    if (!activeBuild) {
        for (auto& b : builds) {
            if (b.active) {
                activeBuild = &b;
                break;
            }
        }
    }

    // 4. Fallback: first row.
    if (!activeBuild && !builds.empty())
        activeBuild = &builds[0];

    // 5. No usable `.build.info`: an explicit build key still names a config
    //    file directly, and zeroed keys otherwise make both config lookups miss
    //    so the consistency fallbacks below do the selecting.
    BuildInfo recoveredBuild;
    if (!activeBuild) {
        if (!opts.buildKey.empty())
            recoveredBuild.buildKey = storages::common::hexDecode16(opts.buildKey);
        activeBuild = &recoveredBuild;
    }

    // Step 2: Create Impl with LocalState.
    auto implPtr = std::make_unique<Impl>();
    auto& impl = *implPtr;
    impl.pool = opts.pool;
    impl.localeMask = opts.localeMask;
    impl.featureFlags = opts.flags;
    impl.listfileData = opts.listfile;

    impl.localState = std::make_unique<LocalState>();
    impl.localState->basePath = basePath;
    impl.localState->dataPath = dataPath;
    impl.progress = std::move(reporterOwned);

    if (opts.memoryCacheSize > 0)
        impl.memCache = std::make_unique<MemoryCache>(opts.memoryCacheSize);

    // The index table is needed up-front so the build-config fallback can
    // verify candidates. LazyIdxBuckets defers per-bucket parsing only.
    if (!progress.begin(ProgressStep::LoadingIndexFiles) && failCancelled())
        return std::nullopt;
    if (opts.flags & StorageFeatureFlags::LazyIdxBuckets) {
        impl.localState->indexTable = IndexTable::loadLazyBuckets(dataPath, opts.pool,
                                                                  progress.sink());
    } else {
        impl.localState->indexTable = IndexTable::load(dataPath, opts.pool, progress.sink());
    }
    progress.end();
    if (progress.cancelled() && failCancelled())
        return std::nullopt;

    if (!impl.localState->indexTable.isValid()) {
        s_lastError = kIndexLoadFailed;
        if (opts.errorOut)
            *opts.errorOut = "No usable .idx files under '" + dataPath + "/data'";
        return std::nullopt;
    }

    // Step 4: Parse build config; if the active key's file is missing, fall
    // back to any local build config consistent with the index.
    auto buildConfigPath = impl.localState->configPath(activeBuild->buildKey);
    if (!progress.begin(ProgressStep::LoadingBuildConfig,
                        storages::common::hexEncode16(activeBuild->buildKey)) &&
        failCancelled())
        return std::nullopt;
    std::string sysErr;
    auto buildConfigFile = storages::common::readFileFully(buildConfigPath, &sysErr);
    if (buildConfigFile) {
        impl.buildConfig = parseBuildConfig(*buildConfigFile);
    } else {
        auto fallback = findConsistentBuildConfig(dataPath, impl.localState->indexTable);
        if (!fallback) {
            if (buildInfoError.empty()) {
                reportFileError(buildConfigPath, sysErr, kBuildConfigNotFound);
            } else {
                s_lastError = buildInfoLocked ? kSharingViolation : kBuildInfoNotFound;
                if (opts.errorOut)
                    *opts.errorOut = buildInfoError + "; no build config under '" + dataPath +
                                     "/config' matches the local index either";
            }
            return std::nullopt;
        }
        impl.buildConfig = std::move(*fallback);
    }
    progress.end(1);

    // Without a `.build.info` there was no Product column to match against, so
    // the recovered config has to answer for the requested product itself —
    // asking for a PTR flavor must never silently open retail.
    if (builds.empty() && !productCode.empty()) {
        std::string const wanted = storages::common::toLower(productCode);
        if (storages::common::toLower(impl.buildConfig.buildUid) != wanted &&
            storages::common::toLower(impl.buildConfig.buildProduct) != wanted) {
            s_lastError = kBuildInfoNotFound;
            if (opts.errorOut)
                *opts.errorOut = "Product '" + productCode +
                                 "' does not match the build config in '" + dataPath +
                                 "/config' (build-uid '" + impl.buildConfig.buildUid +
                                 "', build-product '" + impl.buildConfig.buildProduct + "')";
            return std::nullopt;
        }
    }

    // Step 5: Parse CDN config; on miss, pick the local CDN config whose
    // archive list overlaps best with indices/.
    auto cdnConfigPath = impl.localState->configPath(activeBuild->cdnKey);
    if (!progress.begin(ProgressStep::LoadingCdnConfig,
                        storages::common::hexEncode16(activeBuild->cdnKey)) &&
        failCancelled())
        return std::nullopt;
    auto cdnConfigFile = storages::common::readFileFully(cdnConfigPath);
    if (cdnConfigFile) {
        impl.cdnConfig = parseCdnConfig(*cdnConfigFile);
    } else {
        auto fallback = findConsistentCdnConfig(dataPath);
        if (fallback)
            impl.cdnConfig = std::move(*fallback);
    }
    progress.end(1);

    // The `indices/*.index` files describe CDN archives, whose ordinals in the
    // CDN config's archive list have nothing to do with the local data.NNN
    // numbering that LocalDataSource indexes with. Their entries are therefore
    // unreadable here, and merging them in only made files the install never
    // downloaded look available to listings. Local storages read exclusively
    // through the .idx buckets; the archive indices stay an online concern.

    // Step 6: Memory-map data archives.
    if (!progress.begin(ProgressStep::MappingArchives) && failCancelled())
        return std::nullopt;
    std::string mapError;
    if (!impl.localState->mapArchives(&mapError, progress.sink())) {
        s_lastError = kIndexLoadFailed;
        if (opts.errorOut)
            *opts.errorOut = mapError;
        return std::nullopt;
    }
    progress.end();
    if (progress.cancelled() && failCancelled())
        return std::nullopt;
    // mapArchives may report a per-file failure (and set s_lastError = kSharingViolation)
    // even when it returns true (it returns true as long as at least the directory exists).
    // Surface the first such failure to errorOut so the caller can see which archive
    // is locked, even if other archives mapped successfully.
    if (!mapError.empty() && opts.errorOut && opts.errorOut->empty())
        *opts.errorOut = mapError;

    // Step 7: Create LocalDataSource.
    impl.localState->dataSource = std::make_unique<LocalDataSource>(&impl.localState->indexTable,
                                                                    &impl.localState->dataArchives);
    impl.dataSource = impl.localState->dataSource.get();

    // Step 7b: Construct specialised backend (NoCachePolicy; D4 upgrades to MemCacheEnabled later).
    impl.backend = std::make_unique<StorageBackendImpl<LocalDataTraits, NoCachePolicy>>(
        LocalDataTraits{&impl.localState->indexTable, impl.localState->dataSource.get()},
        NoCachePolicy{}, impl.encodingTable, impl.keyRing, impl.pool);

    // Step 8: Load encoding + root (or defer).
    if (opts.flags & StorageFeatureFlags::LoadOnDemand) {
        impl.deferMode = true;
        impl.isValid = true;
        progress.ready();
        Storage storage(std::move(implPtr));
        return storage;
    }

    if (!impl.loadEncodingAndRoot()) {
        if (progress.cancelled())
            failCancelled();
        return std::nullopt;
    }

    progress.ready();
    impl.isValid = true;
    Storage storage(std::move(implPtr));
    return storage;
}

} // namespace whiteout::storages::casc

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file storage_local.cpp
/// @brief Local (disk) factory, archive mapping, VFS prefetch.

#include "storage_impl.h"
#include "storage_backend_impl.h"
#include "constants.h"
#include "../../common/md5.h"

#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace whiteout::storages::casc {

namespace {

std::vector<std::filesystem::path> scanLocalConfigs(const std::string& dataPath) {
    namespace fs = std::filesystem;
    std::vector<fs::path> out;

    fs::path configRoot = fs::path(dataPath) / "config";
    std::error_code ec;
    if (!fs::exists(configRoot, ec) || !fs::is_directory(configRoot, ec))
        return out;

    auto isHex32 = [](const std::string& s) {
        if (s.size() != 32) return false;
        for (char c : s) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F')))
                return false;
        }
        return true;
    };

    for (auto& xx : fs::directory_iterator(configRoot, ec)) {
        if (!xx.is_directory()) continue;
        for (auto& yy : fs::directory_iterator(xx.path(), ec)) {
            if (!yy.is_directory()) continue;
            for (auto& f : fs::directory_iterator(yy.path(), ec)) {
                if (!f.is_regular_file()) continue;
                if (!isHex32(f.path().filename().string())) continue;
                out.push_back(f.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool keyNonZero(const std::array<u8, 16>& key) {
    for (u8 b : key) if (b) return true;
    return false;
}

std::optional<BuildConfig> findConsistentBuildConfig(
    const std::string& dataPath, const IndexTable& indexTable) {

    for (auto& path : scanLocalConfigs(dataPath)) {
        auto data = storages::common::readFileFully(path.string());
        if (!data) continue;

        auto cfg = parseBuildConfig(*data);
        if (!keyNonZero(cfg.encodingEKey)) continue;

        if (indexTable.find(std::span(cfg.encodingEKey.data(), 9)))
            return cfg;
    }
    return std::nullopt;
}

std::optional<CdnConfig> findConsistentCdnConfig(const std::string& dataPath) {
    namespace fs = std::filesystem;

    fs::path indicesRoot = fs::path(dataPath) / "indices";
    std::error_code ec;
    if (!fs::exists(indicesRoot, ec) || !fs::is_directory(indicesRoot, ec))
        return std::nullopt;

    std::unordered_set<std::string> localArchives;
    for (auto& e : fs::directory_iterator(indicesRoot, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".index")
            localArchives.insert(e.path().stem().string());
    }
    if (localArchives.empty()) return std::nullopt;

    auto toHex = [](const std::array<u8, 16>& key) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string s;
        s.reserve(32);
        for (u8 b : key) { s += hex[b >> 4]; s += hex[b & 0xF]; }
        return s;
    };

    std::optional<CdnConfig> best;
    size_t bestOverlap = 0;

    for (auto& path : scanLocalConfigs(dataPath)) {
        auto data = storages::common::readFileFully(path.string());
        if (!data) continue;

        auto cfg = parseCdnConfig(*data);
        if (cfg.archiveEKeys.empty()) continue;  // not a CDN config

        size_t overlap = 0;
        for (auto& k : cfg.archiveEKeys)
            if (localArchives.count(toHex(k))) ++overlap;

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

bool LocalState::mapArchives(std::string* error) {
    namespace fs = std::filesystem;

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
    bool sawSharingViolation = false;
    std::string firstFailure;
    for (u32 i = 0; i <= maxIndex; ++i) {
        char archiveName[32];
        std::snprintf(archiveName, sizeof(archiveName), "data/data.%03u", i);
        std::string path = dataPath + "/" + archiveName;
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

std::unordered_map<u64, std::vector<u8>> prefetchVfsLocal(
    const Storage::Impl& impl,
    const std::vector<std::array<u8, 16>>& vfsEKeys,
    const std::unordered_map<u64, std::array<u8, 16>>& vfsEKeyToCKey) {

    std::unordered_map<u64, std::vector<u8>> vfsCache;

    if (impl.pool && vfsEKeys.size() > 1) {
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
                results[i].data = impl.resolveEKey(eKey16);
                if (results[i].data.empty()) {
                    auto cIt = vfsEKeyToCKey.find(results[i].hash);
                    if (cIt != vfsEKeyToCKey.end())
                        results[i].data = impl.resolveCKey(cIt->second);
                }
                jobGroup.done();
            };
            impl.pool->submit(task);
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
            auto data = impl.resolveEKey(eKey16);
            if (data.empty()) {
                auto cIt = vfsEKeyToCKey.find(h);
                if (cIt != vfsEKeyToCKey.end())
                    data = impl.resolveCKey(cIt->second);
            }
            if (!data.empty())
                vfsCache.emplace(h, std::move(data));
        }
    }

    return vfsCache;
}

// ============================================================================
// Storage::open (local disk)
// ============================================================================

std::optional<Storage> Storage::open(const std::string& path,
                                     interfaces::WorkerPool* pool) {
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

std::optional<Storage> Storage::open(const OpenOptions& opts) {
    namespace fs = std::filesystem;
    s_lastError = kOk;

    // Determine basePath and dataPath.
    std::string basePath = opts.path;
    std::string dataPath;

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

    // Progress callback.
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

    auto reportFileError = [&](const std::string& filePath, const std::string& sysErr,
                               u32 errorCode) {
        if (storages::common::isSharingViolation(sysErr))
            s_lastError = kSharingViolation;
        else
            s_lastError = errorCode;
        if (opts.errorOut)
            *opts.errorOut = "Failed to read '" + filePath + "': " + sysErr;
    };

    if (buildInfoPath.empty()) {
        s_lastError = kBuildInfoNotFound;
        if (opts.errorOut)
            *opts.errorOut = "'.build.info' not found under '" + basePath +
                             "' or '" + dataPath + "'";
        return std::nullopt;
    }

    std::string sysErr;
    auto buildInfoFile = storages::common::readFileFully(buildInfoPath, &sysErr);
    if (!buildInfoFile) {
        reportFileError(buildInfoPath, sysErr, kBuildInfoNotFound);
        return std::nullopt;
    }

    auto builds = parseBuildInfo(*buildInfoFile);
    if (builds.empty()) {
        s_lastError = kBuildInfoNotFound;
        if (opts.errorOut)
            *opts.errorOut = "'.build.info' parsed empty: " + buildInfoPath;
        return std::nullopt;
    }

    // Select active build.
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

    if (opts.memoryCacheSize > 0)
        impl.memCache = std::make_unique<MemoryCache>(opts.memoryCacheSize);

    // The index table is needed up-front so the build-config fallback can
    // verify candidates. LazyIdxBuckets defers per-bucket parsing only.
    progress(ProgressStep::LoadingIndexFiles);
    if (opts.flags & StorageFeatureFlags::LazyIdxBuckets) {
        impl.localState->indexTable = IndexTable::loadLazyBuckets(dataPath, opts.pool);
    } else {
        impl.localState->indexTable = IndexTable::load(dataPath, opts.pool);
    }

    if (!impl.localState->indexTable.isValid()) {
        s_lastError = kIndexLoadFailed;
        if (opts.errorOut)
            *opts.errorOut = "No usable .idx files under '" + dataPath + "/data'";
        return std::nullopt;
    }

    // Step 4: Parse build config; if the active key's file is missing, fall
    // back to any local build config consistent with the index.
    auto buildConfigPath = impl.localState->configPath(activeBuild->buildKey);
    sysErr.clear();
    auto buildConfigFile = storages::common::readFileFully(buildConfigPath, &sysErr);
    if (buildConfigFile) {
        impl.buildConfig = parseBuildConfig(*buildConfigFile);
    } else {
        auto fallback = findConsistentBuildConfig(dataPath,
                                                   impl.localState->indexTable);
        if (!fallback) {
            reportFileError(buildConfigPath, sysErr, kBuildConfigNotFound);
            return std::nullopt;
        }
        impl.buildConfig = std::move(*fallback);
    }

    // Step 5: Parse CDN config; on miss, pick the local CDN config whose
    // archive list overlaps best with indices/.
    auto cdnConfigPath = impl.localState->configPath(activeBuild->cdnKey);
    auto cdnConfigFile = storages::common::readFileFully(cdnConfigPath);
    if (cdnConfigFile) {
        impl.cdnConfig = parseCdnConfig(*cdnConfigFile);
    } else {
        auto fallback = findConsistentCdnConfig(dataPath);
        if (fallback)
            impl.cdnConfig = std::move(*fallback);
    }

    if (!impl.cdnConfig.archiveEKeys.empty()) {
        if (opts.flags & StorageFeatureFlags::LazyArchiveIndex) {
            impl.localState->indexTable.loadArchiveIndicesLazy(
                dataPath, impl.cdnConfig.archiveEKeys, opts.pool);
        } else {
            impl.localState->indexTable.loadArchiveIndices(
                dataPath, impl.cdnConfig.archiveEKeys, opts.pool);
        }
    }

    // Step 6: Memory-map data archives.
    progress(ProgressStep::MappingArchives);
    std::string mapError;
    if (!impl.localState->mapArchives(&mapError)) {
        s_lastError = kIndexLoadFailed;
        if (opts.errorOut)
            *opts.errorOut = mapError;
        return std::nullopt;
    }
    // mapArchives may report a per-file failure (and set s_lastError = kSharingViolation)
    // even when it returns true (it returns true as long as at least the directory exists).
    // Surface the first such failure to errorOut so the caller can see which archive
    // is locked, even if other archives mapped successfully.
    if (!mapError.empty() && opts.errorOut && opts.errorOut->empty())
        *opts.errorOut = mapError;

    // Step 7: Create LocalDataSource.
    impl.localState->dataSource = std::make_unique<LocalDataSource>(
        &impl.localState->indexTable,
        &impl.localState->dataArchives);
    impl.dataSource = impl.localState->dataSource.get();

    // Step 7b: Construct specialised backend (NoCachePolicy; D4 upgrades to MemCacheEnabled later).
    impl.backend = std::make_unique<StorageBackendImpl<LocalDataTraits, NoCachePolicy>>(
        LocalDataTraits{&impl.localState->indexTable, impl.localState->dataSource.get()},
        NoCachePolicy{},
        impl.encodingTable, impl.keyRing, impl.pool);

    // Step 8: Load encoding + root (or defer).
    if (opts.flags & StorageFeatureFlags::LoadOnDemand) {
        impl.deferMode = true;
        impl.isValid = true;
        progress(ProgressStep::Ready);
        Storage storage(std::move(implPtr));
        return storage;
    }

    progress(ProgressStep::LoadingEncodingTable);
    progress(ProgressStep::LoadingRootManifest);

    if (!impl.loadEncodingAndRoot()) {
        return std::nullopt;
    }

    progress(ProgressStep::Ready);
    impl.isValid = true;
    Storage storage(std::move(implPtr));
    return storage;
}

} // namespace whiteout::storages::casc

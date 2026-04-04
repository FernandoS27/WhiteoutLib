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

namespace whiteout::storages::casc {

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
    for (u32 i = 0; i <= maxIndex; ++i) {
        char archiveName[32];
        std::snprintf(archiveName, sizeof(archiveName), "data/data.%03u", i);
        std::string path = dataPath + "/" + archiveName;
        if (fs::exists(path)) {
            auto mapped = storages::common::MappedFile::open(
                path, storages::common::AccessHint::Random);
            if (mapped) {
                dataArchives[i] = std::move(*mapped);
            }
        }
    }

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

std::optional<Storage> Storage::open(const std::string& path, std::string* /*error*/,
                                     interfaces::WorkerPool* pool) {
    OpenOptions opts;
    opts.path = path;
    opts.pool = pool;
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

    if (buildInfoPath.empty()) {
        s_lastError = kBuildInfoNotFound;
        return std::nullopt;
    }

    auto buildInfoFile = storages::common::MappedFile::open(buildInfoPath);
    if (!buildInfoFile) {
        s_lastError = kBuildInfoNotFound;
        return std::nullopt;
    }

    auto builds = parseBuildInfo(buildInfoFile->data());
    if (builds.empty()) {
        s_lastError = kBuildInfoNotFound;
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
    impl.listfileData = opts.listfile;

    impl.localState = std::make_unique<LocalState>();
    impl.localState->basePath = basePath;
    impl.localState->dataPath = dataPath;

    if (opts.memoryCacheSize > 0)
        impl.memCache = std::make_unique<MemoryCache>(opts.memoryCacheSize);

    // Step 3: Parse build config.
    auto buildConfigPath = impl.localState->configPath(activeBuild->buildKey);
    auto buildConfigFile = storages::common::MappedFile::open(buildConfigPath);
    if (!buildConfigFile) {
        s_lastError = kBuildConfigNotFound;
        return std::nullopt;
    }
    impl.buildConfig = parseBuildConfig(buildConfigFile->data());

    // Step 4: Parse CDN config (optional).
    auto cdnConfigPath = impl.localState->configPath(activeBuild->cdnKey);
    auto cdnConfigFile = storages::common::MappedFile::open(cdnConfigPath);
    if (cdnConfigFile) {
        impl.cdnConfig = parseCdnConfig(cdnConfigFile->data());
    }

    // Step 5: Load index table.
    progress(ProgressStep::LoadingIndexFiles);
    impl.localState->indexTable = IndexTable::load(dataPath, opts.pool);

    if (!impl.cdnConfig.archiveEKeys.empty())
        impl.localState->indexTable.loadArchiveIndices(
            dataPath, impl.cdnConfig.archiveEKeys, opts.pool);

    if (impl.localState->indexTable.entryCount() == 0) {
        s_lastError = kIndexLoadFailed;
        return std::nullopt;
    }

    // Step 6: Memory-map data archives.
    progress(ProgressStep::MappingArchives);
    std::string mapError;
    if (!impl.localState->mapArchives(&mapError)) {
        s_lastError = kIndexLoadFailed;
        return std::nullopt;
    }

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

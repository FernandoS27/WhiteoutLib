// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file root_build_utils.h
/// @brief Shared helpers for building root manifest indices.
///
/// Internal header — not part of the public include path.
#pragma once

#include "../../../common/jenkins.h"
#include "../../../common/string_utils.h"
#include "../root.h"
#include "entry_index.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

/// Normalize all entry paths in parallel (when a pool is available and the
/// entry count exceeds a threshold). Returns a vector of lowercased,
/// normalized path strings aligned 1:1 with @p entries.
inline std::vector<std::string> normalizeEntryPaths(const std::vector<RootEntry>& entries,
                                                    interfaces::WorkerPool* pool) {

    using storages::common::normalizeCascPath;

    size_t n = entries.size();
    std::vector<std::string> result(n);

    if (pool && n > 1000) {
        size_t numThreads = std::max<size_t>(pool->threadCount(), 1);
        size_t chunkSize = (n + numThreads - 1) / numThreads;
        size_t chunks = (n + chunkSize - 1) / chunkSize;

        utils::JobGroup jobGroup;
        jobGroup.add(chunks);
        for (size_t c = 0; c < chunks; ++c) {
            interfaces::WorkerTask task;
            task.fn = [&, c]() {
                size_t start = c * chunkSize;
                size_t end = std::min(start + chunkSize, n);
                for (size_t i = start; i < end; ++i) {
                    if (!entries[i].path.empty())
                        result[i] = normalizeCascPath(entries[i].path);
                }
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    } else {
        for (size_t i = 0; i < n; ++i) {
            if (!entries[i].path.empty())
                result[i] = normalizeCascPath(entries[i].path);
        }
    }

    return result;
}

/// Look up entries by path, trying exact-path index first, then falling
/// back to Jenkins hash lookup.  Used by roots that maintain both a
/// normalised-path index and a Jenkins-hash index (S1, Install).
inline std::vector<const RootEntry*> findByPathOrHash(const std::string& path,
                                                      const std::vector<RootEntry>& entries,
                                                      const EntryIndex<std::string>& byPath,
                                                      const EntryIndex<u64>& byNameHash) {

    using storages::common::jenkinsHash;
    using storages::common::normalizePath;

    auto normalized = normalizePath(path);
    auto results = byPath.findAll(entries, normalized);
    if (!results.empty())
        return results;

    auto hash = jenkinsHash(path);
    u64 combined = u64(hash.pc) | (u64(hash.pb) << 32);
    return byNameHash.findAll(entries, combined);
}

/// Build both a normalised-path index and a Jenkins-hash index from entries.
/// Used by roots that maintain dual-index lookup (S1, Install).
inline void buildPathAndHashIndex(EntryIndex<std::string>& byPath, EntryIndex<u64>& byNameHash,
                                  const std::vector<RootEntry>& entries) {

    using storages::common::normalizePath;

    byNameHash.clear();
    byPath.clear();
    byNameHash.reserve(entries.size());
    byPath.reserve(entries.size());

    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        if (e.fileNameHash != 0)
            byNameHash.emplace(e.fileNameHash, i);
        if (!e.path.empty())
            byPath.emplace(normalizePath(e.path), i);
    }
}

} // namespace whiteout::storages::casc

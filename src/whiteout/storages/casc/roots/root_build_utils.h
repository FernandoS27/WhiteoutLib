// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file root_build_utils.h
/// @brief Shared helpers for building root manifest indices.
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"
#include "../../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

/// Normalize all entry paths in parallel (when a pool is available and the
/// entry count exceeds a threshold). Returns a vector of lowercased,
/// normalized path strings aligned 1:1 with @p entries.
inline std::vector<std::string> normalizeEntryPaths(
    const std::vector<RootEntry>& entries,
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

} // namespace whiteout::storages::casc

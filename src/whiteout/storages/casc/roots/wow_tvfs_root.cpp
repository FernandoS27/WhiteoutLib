// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "wow_tvfs_root.h"
#include "common/listfile_parser.h"
#include "common/wow_tvfs_path.h"
#include "../../common/string_utils.h"

#include <whiteout/interfaces.h>
#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>

namespace whiteout::storages::casc {

// ============================================================================
// WowTvfsRoot — detection
// ============================================================================

bool WowTvfsRoot::looksLikeWowTvfs(const TvfsRoot& tvfs) {
    // Walk entries until we either confirm enough encoded leaves or scan
    // through enough non-matches to give up. WoW retail vfs-roots reference
    // sub-manifests via container entries (paths ending in `:`) which always
    // appear before the recursively-parsed leaves — they never match the
    // encoded pattern, so a "first-N entries" sample at the start of the
    // enumeration may see only containers. Scanning until we find a few
    // matches (or hit a wide non-match cap) handles that.
    constexpr size_t kRequiredMatches = 4;
    constexpr size_t kMaxNonMatches = 4096;
    size_t matched = 0;
    size_t nonMatches = 0;
    tvfs.enumerate([&](const RootEntry& e) {
        if (wow_tvfs_path::matches(e.path)) {
            if (++matched >= kRequiredMatches) return false; // confirmed
        } else if (++nonMatches >= kMaxNonMatches) {
            return false; // give up
        }
        return true;
    });
    return matched >= kRequiredMatches;
}

// ============================================================================
// WowTvfsRoot — creation
// ============================================================================

std::unique_ptr<WowTvfsRoot> WowTvfsRoot::create(std::unique_ptr<TvfsRoot> tvfs,
                                                   interfaces::WorkerPool* pool,
                                                   std::span<const u8> listfile) {
    if (!tvfs) return nullptr;
    if (!looksLikeWowTvfs(*tvfs)) return nullptr;

    // Parse listfile if provided (parallel — community listfiles are ~140 MB).
    std::unordered_map<u32, std::string> listfilePaths;
    if (!listfile.empty())
        listfilePaths = casc::parseListfile(listfile, pool);

    auto result = std::unique_ptr<WowTvfsRoot>(new WowTvfsRoot());
    // Move the TVFS entry table in and transform it in place — avoids a second
    // multi-million-element allocation + copy.
    result->m_entries = tvfs->takeEntries();
    result->m_tvfs = std::move(tvfs);

    const size_t entryCount = result->m_entries.size();
    const bool haveListfile = !listfilePaths.empty();

    // Parse each encoded TVFS path to extract locale/content flags, FileDataId, and CKey.
    //
    // Path resolution policy:
    //   - With a listfile: only entries whose decoded FileDataId is found in
    //     the listfile keep a path; everything else (sub-manifest containers,
    //     paths that don't decode, FDIDs missing from the listfile) gets an
    //     empty path so listFiles()/enumerate() filter them out. This matches
    //     the user expectation that a listfile-loaded session only surfaces
    //     human-readable filenames, not the raw 53-char hex encoding.
    //   - Without a listfile: keep the encoded TVFS form so it stays queryable.
    auto enrichEntry = [&](size_t i) {
        auto& e = result->m_entries[i];

        wow_tvfs_path::Info info;
        const bool decoded = wow_tvfs_path::tryDecode(e.path, info);

        if (decoded) {
            e.cKey = info.cKey;
            e.localeFlags = info.localeFlags;
            e.contentFlags = info.contentFlags;
            e.fileDataId = info.fileDataId;
        }
        // else: keep the entry's existing cKey/locale/content/fileDataId.

        if (haveListfile) {
            // tryDecode already consumed e.path; safe to overwrite.
            std::string newPath;
            if (decoded) {
                auto it = listfilePaths.find(info.fileDataId);
                if (it != listfilePaths.end())
                    newPath = it->second;
            }
            e.path = std::move(newPath); // empty unless found in the listfile
        }
        // else: keep e.path (the encoded TVFS form).
    };

    // Parallel entry processing.
    if (pool && entryCount > 1000) {
        size_t numThreads = std::max<size_t>(pool->threadCount(), 1);
        size_t chunkSize = (entryCount + numThreads - 1) / numThreads;
        size_t chunks = (entryCount + chunkSize - 1) / chunkSize;

        utils::JobGroup jobGroup;
        jobGroup.add(chunks);
        for (size_t c = 0; c < chunks; ++c) {
            interfaces::WorkerTask task;
            task.fn = [&, c]() {
                size_t begin = c * chunkSize;
                size_t end = std::min(begin + chunkSize, entryCount);
                for (size_t i = begin; i < end; ++i)
                    enrichEntry(i);
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    } else {
        for (size_t i = 0; i < entryCount; ++i)
            enrichEntry(i);
    }

    result->buildIndex(pool);
    return result;
}

// ============================================================================
// WowTvfsRoot — RootManifest interface
// ============================================================================

std::vector<const RootEntry*> WowTvfsRoot::findByPath(const std::string& path) const {
    if (m_byPath.empty())
        return {};
    auto key = storages::common::normalizeCascPath(path);
    return m_byPath.findAll(m_entries, key);
}

std::vector<const RootEntry*> WowTvfsRoot::findByFileDataId(u32 fileDataId, FileIdHint /*hint*/) const {
    return m_byFileDataId.findAll(m_entries, fileDataId);
}

bool WowTvfsRoot::hasFileDataId(u32 fileDataId, FileIdHint /*hint*/) const {
    return m_byFileDataId.contains(fileDataId);
}

const std::vector<RootEntry>& WowTvfsRoot::entries() const {
    return m_entries;
}

std::vector<RootEntry>& WowTvfsRoot::mutableEntries() {
    return m_entries;
}

// ============================================================================
// WowTvfsRoot — index building
// ============================================================================

void WowTvfsRoot::buildIndex(interfaces::WorkerPool* pool) {
    // The two indices are independent members — build them concurrently.
    // The path index (with per-entry normalisation) is the long pole.
    auto buildFileDataIdIndex = [this]() {
        m_byFileDataId.reserve(m_entries.size());
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].fileDataId != kInvalidFileDataId)
                m_byFileDataId.emplace(m_entries[i].fileDataId, i);
        }
    };
    auto buildPathIndex = [this]() {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (!m_entries[i].path.empty()) {
                auto key = storages::common::normalizeCascPath(m_entries[i].path);
                m_byPath.emplace(std::move(key), i);
            }
        }
    };

    if (pool && m_entries.size() > 10000) {
        utils::JobGroup jobGroup;
        jobGroup.add(2);
        interfaces::WorkerTask t1;
        t1.fn = [&]() { buildFileDataIdIndex(); jobGroup.done(); };
        interfaces::WorkerTask t2;
        t2.fn = [&]() { buildPathIndex(); jobGroup.done(); };
        pool->submit(t1);
        pool->submit(t2);
        jobGroup.wait();
    } else {
        buildFileDataIdIndex();
        buildPathIndex();
    }
}

} // namespace whiteout::storages::casc

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../../common/string_utils.h"
#include "common/listfile_parser.h"
#include "common/wow_tvfs_path.h"
#include "wow_tvfs_root.h"

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
            if (++matched >= kRequiredMatches)
                return false; // confirmed
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
    if (!tvfs)
        return nullptr;
    if (!looksLikeWowTvfs(*tvfs))
        return nullptr;

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
        size_t const numThreads = std::max<size_t>(pool->threadCount(), 1);
        size_t chunkSize = (entryCount + numThreads - 1) / numThreads;
        size_t const chunks = (entryCount + chunkSize - 1) / chunkSize;

        utils::JobGroup jobGroup;
        jobGroup.add(chunks);
        for (size_t c = 0; c < chunks; ++c) {
            interfaces::WorkerTask task;
            task.fn = [&, c]() {
                size_t const begin = c * chunkSize;
                size_t const end = std::min(begin + chunkSize, entryCount);
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

    result->buildFileDataIdIndex();
    // A listfile is the only reason to carry human-readable paths, so treat it
    // as a declaration that path lookups are coming and build the index here,
    // on the open thread, where using the pool is safe. Without one the paths
    // are the 53-char hex form that nobody queries by name.
    if (haveListfile)
        result->buildPathIndex(pool);
    return result;
}

// ============================================================================
// WowTvfsRoot — RootManifest interface
// ============================================================================

std::vector<const RootEntry*> WowTvfsRoot::findByPath(const std::string& path) const {
    ensurePathIndex();

    auto key = storages::common::normalizeCascPath(path);
    auto* head = m_byPathHead.find(storages::common::cascPathHash64(key));
    if (!head)
        return {};

    std::vector<const RootEntry*> results;
    for (u32 i = *head; i != kNoPathChain; i = m_pathChain[i]) {
        if (storages::common::normalizedCascPathEquals(m_entries[i].path, key))
            results.push_back(&m_entries[i]);
    }
    return results;
}

namespace {
auto byFileDataId = [](const std::pair<u32, u32>& e, u32 id) { return e.first < id; };
} // namespace

std::vector<const RootEntry*> WowTvfsRoot::findByFileDataId(u32 fileDataId,
                                                            FileIdHint /*hint*/) const {
    auto it =
        std::lower_bound(m_byFileDataId.begin(), m_byFileDataId.end(), fileDataId, byFileDataId);

    std::vector<const RootEntry*> results;
    for (; it != m_byFileDataId.end() && it->first == fileDataId; ++it)
        results.push_back(&m_entries[it->second]);
    return results;
}

bool WowTvfsRoot::hasFileDataId(u32 fileDataId, FileIdHint /*hint*/) const {
    auto it =
        std::lower_bound(m_byFileDataId.begin(), m_byFileDataId.end(), fileDataId, byFileDataId);
    return it != m_byFileDataId.end() && it->first == fileDataId;
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

void WowTvfsRoot::buildFileDataIdIndex() {
    m_byFileDataId.clear();
    m_byFileDataId.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].fileDataId != kInvalidFileDataId)
            m_byFileDataId.emplace_back(m_entries[i].fileDataId, u32(i));
    }

    // Ties keep ascending entry order, so selectBestEntry resolves a FileDataId
    // with several locale/content variants to the one the manifest lists first.
    // The unordered_multimap this replaced enumerated equal keys in hash-bucket
    // order, which is neither portable nor meaningful.
    //
    // Two 16-bit LSD radix passes: the pairs are built in ascending index
    // order, and a stable sort on the id alone therefore preserves the tie
    // rule, at ~3x the speed of comparison-sorting 3.2M pairs. Small inputs
    // are not worth the scratch buffer.
    constexpr size_t kMinRadix = 1u << 16;
    if (m_byFileDataId.size() < kMinRadix) {
        std::sort(m_byFileDataId.begin(), m_byFileDataId.end());
        return;
    }

    size_t const n = m_byFileDataId.size();
    std::vector<std::pair<u32, u32>> scratch(n);
    auto* src = m_byFileDataId.data();
    auto* dst = scratch.data();
    std::vector<u32> counts(1u << 16);
    for (int shift = 0; shift <= 16; shift += 16) {
        std::fill(counts.begin(), counts.end(), 0u);
        for (size_t i = 0; i < n; ++i)
            ++counts[(src[i].first >> shift) & 0xFFFF];
        u32 running = 0;
        for (auto& c : counts) {
            u32 const here = c;
            c = running;
            running += here;
        }
        for (size_t i = 0; i < n; ++i)
            dst[counts[(src[i].first >> shift) & 0xFFFF]++] = src[i];
        std::swap(src, dst);
    }
    // Two passes land the result back in m_byFileDataId's own buffer.
}

void WowTvfsRoot::buildPathIndex(interfaces::WorkerPool* pool) {
    std::call_once(m_pathIndexOnce, [&]() { buildPathIndexImpl(pool); });
}

void WowTvfsRoot::ensurePathIndex() const {
    // No pool here: this runs on whichever thread first calls findByPath, which
    // may itself be one of the pool's workers — fanning out and waiting would
    // deadlock once the other workers block on this same call_once.
    std::call_once(m_pathIndexOnce, [this]() { buildPathIndexImpl(nullptr); });
}

void WowTvfsRoot::buildPathIndexImpl(interfaces::WorkerPool* pool) const {
    size_t const n = m_entries.size();

    // Hashing normalises on the fly, so no per-entry string is materialised.
    // A zero hash means "no path" — cascPathHash64 never returns 0.
    std::vector<u64> hashes(n, 0);
    auto hashRange = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (!m_entries[i].path.empty())
                hashes[i] = storages::common::normalizedCascPathHash64(m_entries[i].path);
        }
    };

    if (pool && n > 10000) {
        size_t const numThreads = std::max<size_t>(pool->threadCount(), 1);
        size_t const chunkSize = (n + numThreads - 1) / numThreads;
        size_t const chunks = (n + chunkSize - 1) / chunkSize;

        utils::JobGroup jobGroup;
        jobGroup.add(chunks);
        for (size_t c = 0; c < chunks; ++c) {
            interfaces::WorkerTask task;
            task.fn = [&, c]() {
                hashRange(c * chunkSize, std::min((c + 1) * chunkSize, n));
                jobGroup.done();
            };
            pool->submit(task);
        }
        jobGroup.wait();
    } else {
        hashRange(0, n);
    }

    // Chained back to front so each chain reads out in ascending entry order,
    // matching how findByFileDataId orders its ties.
    m_pathChain.assign(n, kNoPathChain);
    m_byPathHead.reserve(n);
    for (size_t i = n; i-- > 0;) {
        if (hashes[i] == 0)
            continue;
        if (auto* head = m_byPathHead.find(hashes[i])) {
            m_pathChain[i] = *head;
            m_byPathHead.insertOrAssign(hashes[i], u32(i));
        } else {
            m_byPathHead.emplace(hashes[i], u32(i));
        }
    }
}

} // namespace whiteout::storages::casc

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file wow_tvfs_root.h
/// @brief WoW TVFS root: FileDataId enricher over TVFS.
///
/// Modern WoW (11.x+) uses a TVFS root where each path encodes locale/content
/// flags, a FileDataId, and a CKey in a fixed 53-character hex string:
///   LLLLLLLLCCCC/FFFFFFFFkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk
///   ^locale(8)  ^content(4) ^fileDataId(8) ^cKey(32)
///
/// This class wraps a TvfsRoot, parses those encoded entries, and re-indexes
/// them by FileDataId — making the archive queryable like a classic WoW root.
///
/// Internal header — not part of the public include path.
#pragma once

#include "../tables/flat_hash_map.h"
#include "root.h"
#include "tvfs_root.h"

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::storages::casc {

class WowTvfsRoot final : public RootManifest {
public:
    /// Check whether a parsed TvfsRoot looks like a WoW TVFS root.
    /// Samples a few entries and checks for the 53-char hex-encoded pattern.
    static bool looksLikeWowTvfs(const TvfsRoot& tvfs);

    /// Enrich an existing TvfsRoot with WoW FileDataId/locale/content metadata.
    /// @param tvfs     Parsed TVFS root (ownership transferred).
    /// @param pool     Optional worker pool for parallel operations.
    /// @param listfile Optional external listfile (FileDataId;path per line).
    ///                 When provided, entries are enriched with human-readable paths.
    /// @return Enriched root, or nullptr if entries don't match expected format.
    static std::unique_ptr<WowTvfsRoot> create(std::unique_ptr<TvfsRoot> tvfs,
                                               interfaces::WorkerPool* pool = nullptr,
                                               std::span<const u8> listfile = {});

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByFileDataId(
        u32 fileDataId, FileIdHint hint = FileIdHint::None) const override;
    bool hasFileDataId(u32 fileDataId, FileIdHint hint = FileIdHint::None) const override;
    RootFormat format() const override {
        return RootFormat::WowTvfs;
    }

protected:
    const std::vector<RootEntry>& entries() const override;
    std::vector<RootEntry>& mutableEntries() override;

private:
    /// Sentinel terminating a path-collision chain.
    static constexpr u32 kNoPathChain = 0xFFFFFFFFu;

    /// The underlying TVFS root we own.
    std::unique_ptr<TvfsRoot> m_tvfs;

    /// Enriched entries with parsed locale/content flags and FileDataId.
    std::vector<RootEntry> m_entries;

    /// (FileDataId, index into m_entries), sorted — binary-searched by findByFileDataId.
    std::vector<std::pair<u32, u32>> m_byFileDataId;

    /// Path index: normalized-path hash → head entry, chained through m_pathChain.
    /// Built on first path lookup — WoW callers address files by FileDataId, and
    /// indexing 3M+ paths up front costs more than the whole rest of the open.
    /// The build is single-threaded; see ensurePathIndex.
    mutable FlatHashMap<u32> m_byPathHead;
    mutable std::vector<u32> m_pathChain;
    mutable std::once_flag m_pathIndexOnce;

    void buildFileDataIdIndex();

    /// Build the path index now, fanning the hashing out over @p pool. Only safe
    /// from a thread that is not itself one of that pool's workers; create()
    /// calls it on the open thread.
    void buildPathIndex(interfaces::WorkerPool* pool);

    /// Build the path index on the calling thread if it hasn't been built yet.
    void ensurePathIndex() const;

    void buildPathIndexImpl(interfaces::WorkerPool* pool) const;
};

} // namespace whiteout::storages::casc

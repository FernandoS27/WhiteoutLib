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

#include "root.h"
#include "common/entry_index.h"
#include "tvfs_root.h"

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

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
    std::vector<const RootEntry*> findByFileDataId(u32 fileDataId, FileIdHint hint = FileIdHint::None) const override;
    bool hasFileDataId(u32 fileDataId, FileIdHint hint = FileIdHint::None) const override;
    RootFormat format() const override { return RootFormat::WowTvfs; }

protected:
    const std::vector<RootEntry>& entries() const override;
    std::vector<RootEntry>& mutableEntries() override;

private:
    /// The underlying TVFS root we own.
    std::unique_ptr<TvfsRoot> m_tvfs;

    /// Enriched entries with parsed locale/content flags and FileDataId.
    std::vector<RootEntry> m_entries;

    /// FileDataId → index into m_entries.
    EntryIndex<u32> m_byFileDataId;

    /// Normalized path → index into m_entries (only populated when a listfile is provided).
    EntryIndex<std::string> m_byPath;

    void buildIndex(interfaces::WorkerPool* pool);

    /// Parse a listfile CSV (FileDataId;path per line) into a FileDataId → path map.
    static std::unordered_map<u32, std::string> parseListfile(std::span<const u8> data);
};

} // namespace whiteout::storages::casc

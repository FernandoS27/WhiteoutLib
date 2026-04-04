// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file d4_root.h
/// @brief Diablo IV root: path enricher over TVFS using CoreTOC.
///
/// D4 uses a TVFS root with numeric SNO IDs as file names.  This class
/// wraps a TvfsRoot, parses CoreTOC.dat (and optional side-tables like
/// SharedPayloads and EncryptedSNOs) from inside the archive, then
/// re-indexes every entry with a human-readable path:
///   Base\child\<GroupName>\<SnoName>.<ext>
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"
#include "common/entry_index.h"
#include "tvfs_root.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

/// Callback to read a file from the CASC archive by its EKey (16 bytes).
/// Used by D4Root during enrichment to fetch CoreTOC.dat and side-tables.
using EKeyReader = std::function<std::vector<u8>(std::span<const u8, 16> eKey)>;

class D4Root final : public RootManifest {
public:
    /// Enrich an existing TvfsRoot with human-readable D4 SNO paths.
    /// @param tvfs     Parsed TVFS root (ownership transferred).
    /// @param reader   Callback that resolves EKey → decoded file bytes.
    /// @param pool     Optional worker pool for parallel operations.
    /// @return Enriched root, or nullptr on failure (e.g. CoreTOC not found).
    static std::unique_ptr<D4Root> create(std::unique_ptr<TvfsRoot> tvfs,
                                          const EKeyReader& reader,
                                          interfaces::WorkerPool* pool = nullptr);

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByNormalizedPath(const std::string& normalizedPath) const override;
    bool hasPath(const std::string& normalizedPath) const override;
    std::vector<const RootEntry*> findByFileDataId(u32 fileDataId, FileIdHint hint = FileIdHint::None) const override;
    RootFormat format() const override { return RootFormat::Diablo4; }

    void enumerateUnder(const std::string& normalizedPrefix,
                        std::function<bool(const RootEntry&)> callback) const override;

protected:
    const std::vector<RootEntry>& entries() const override;
    std::vector<RootEntry>& mutableEntries() override;

private:
    /// The underlying TVFS root we own.
    std::unique_ptr<TvfsRoot> m_tvfs;

    /// Enriched entries with human-readable paths.
    /// Parallel to TVFS entries — same size, same order.
    std::vector<RootEntry> m_entries;

    /// Normalized path → index into m_entries.
    EntryIndex<std::string> m_byPath;

    /// snoId (stored as fileDataId) → index into m_entries.
    EntryIndex<u32> m_bySnoId;

    void buildIndex(interfaces::WorkerPool* pool);
};

} // namespace whiteout::storages::casc

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file d4_root.h
/// @brief Diablo IV root manifest (TVFS + CoreTOC SNO enrichment).
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"

#include <whiteout/sno/core_toc.h>

#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

class TvfsRoot;

/// Callback to resolve a CKey or EKey to raw file data.
using FileResolver = std::function<std::vector<u8>(const RootEntry& entry)>;

/// Diablo IV root manifest.
///
/// Wraps a TVFS root and enriches entries using CoreTOC data:
/// - Numeric TVFS paths (e.g. "base:meta/12345") are resolved to
///   human-readable names (e.g. "base:meta/Texture/SomeName.tex").
/// - SNO IDs are assigned as fileDataIds for findByFileDataId() lookups.
/// - Both original and enriched paths are indexed for backward compatibility.
/// - Combined meta files (Texture, StringList, etc.) are parsed to create
///   synthetic entries for SNOs that don't have individual files.
class D4Root final : public RootManifest {
public:
    /// Create a D4Root by enriching a TvfsRoot with CoreTOC data.
    /// @param tvfsRoot  Parsed TVFS root (consumed).
    /// @param coreToc   Parsed CoreTOC.dat.
    /// @param resolver  Callback to read file data from CASC (for combined metas).
    /// @param pool      Optional worker pool for parallel index building.
    /// @return Enriched D4 root, or nullptr on failure.
    static std::unique_ptr<D4Root> create(
        std::unique_ptr<TvfsRoot> tvfsRoot,
        sno::CoreToc coreToc,
        FileResolver resolver = nullptr,
        interfaces::WorkerPool* pool = nullptr);

    // --- RootManifest interface ---
    std::vector<RootEntry> findByPath(const std::string& path) const override;
    std::vector<RootEntry> findByFileDataId(u32 fileDataId) const override;
    std::vector<RootEntry> findByCKey(std::span<const u8, 16> cKey) const override;
    void enumerate(std::function<bool(const RootEntry&)> callback) const override;
    size_t entryCount() const override;
    RootFormat format() const override { return RootFormat::Diablo4; }

    /// Get synthetic SNO data for a combined meta entry by snoId.
    /// Returns nullptr if the snoId is not in the combined meta cache.
    const std::vector<u8>* findCombinedMeta(i32 snoId) const;

private:
    std::vector<RootEntry> m_entries;

    /// Path-based index (lowercase normalized). Contains both original and
    /// enriched paths so that callers can look up by either form.
    std::unordered_multimap<std::string, size_t> m_byPath;

    /// FileDataId-based index (snoId → entry indices).
    std::unordered_multimap<u32, size_t> m_byFileDataId;

    /// CoreTOC data (retained for potential future queries).
    sno::CoreToc m_coreToc;

    /// Synthetic SNO blobs for entries extracted from combined meta files.
    /// Key: snoId, Value: complete SNO data (16-byte header + entry data).
    std::unordered_map<i32, std::vector<u8>> m_combinedMetaCache;

    /// Load combined meta files and create synthetic entries.
    void loadCombinedMetas(const FileResolver& resolver);

    void buildIndices(interfaces::WorkerPool* pool = nullptr);
};

} // namespace whiteout::storages::casc

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file ow_root.h
/// @brief Overwatch (Tank) root manifest parser.
///
/// Overwatch uses a text-based root file listing manifest entries (CMF, APM,
/// TRG).  Each CMF (Content Manifest File) maps 64-bit GUIDs to content keys.
/// This parser reads the text root, resolves CMF files via a CKey callback,
/// and builds a flat GUID → CKey index for file access.
///
/// Internal header — not part of the public include path.
#pragma once

#include "common/entry_index.h"
#include "root.h"

#include <span>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::storages::casc {

/// A single entry from the Overwatch text root manifest.
struct OwRootFileEntry {
    std::string fileId;
    std::array<u8, 16> md5{};
    u8 chunkId = 0;
    u8 priority = 0;
    u8 mPriority = 0;
    std::string fileName;
    std::string installPath;
};

/// CMF header, normalized across the pre-1.22, 1.22 and 1.48 on-disk layouts.
struct CmfHeader {
    u32 buildVersion = 0;
    i32 dataCount = 0;
    i32 entryCount = 0;
    u32 magic = 0;
    u8 version = 0;
    bool encrypted = false;
};

/// CMF hash data entry, widened to the 1.35+ record.
struct CmfHashData {
    u64 guid = 0;
    u32 size = 0;
    u8 unknown = 0;
    std::array<u8, 16> contentKey{};
};

class OwRoot final : public RootManifest {
public:
    /// Parse an Overwatch text root file.
    /// @param data     Raw (BLTE-decoded) root file bytes (text format).
    /// @param resolver Callback that resolves CKey → file data (for CMF files).
    ///                 May be nullptr for root-only parsing (no CMF resolution).
    ///                 With a @p pool it is called from several threads at
    ///                 once, and must not itself put work on that pool — a
    ///                 resolver waiting on pool tasks from inside one deadlocks.
    /// @param pool     Optional worker pool for parallel CMF fetching and parsing.
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<OwRoot> parse(std::span<const u8> data, CKeyResolver resolver = nullptr,
                                         interfaces::WorkerPool* pool = nullptr);

    /// Parse an Overwatch text root file from the manifest entries directly.
    /// Useful for testing without raw byte parsing.
    static std::unique_ptr<OwRoot> fromManifestEntries(std::vector<OwRootFileEntry> manifestEntries,
                                                       CKeyResolver resolver = nullptr,
                                                       interfaces::WorkerPool* pool = nullptr);

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByNormalizedPath(
        const std::string& normalizedPath) const override;
    std::vector<const RootEntry*> findByFileDataId(
        u32 fileDataId, FileIdHint hint = FileIdHint::None) const override;
    bool hasFileDataId(u32, FileIdHint = FileIdHint::None) const override {
        return false;
    }

    /// Find entries by 64-bit Overwatch GUID.
    std::vector<const RootEntry*> findByGuid(u64 guid) const;

    /// Path for @p entry. Asset entries do not carry one: an asset path is its
    /// manifest's folder, its GUID in hex and its type extension, all of which
    /// the entry already implies, and a current install has twenty-four million
    /// of them — the strings alone cost more than a gigabyte and five seconds
    /// of the open. @ref enumerate fills them in as it goes; a caller holding
    /// an entry from @ref findByGuid asks for one here.
    ///
    /// Returns @p entry's own path for a manifest row, and an empty string for
    /// an entry this root does not own.
    std::string assetPath(const RootEntry& entry) const;

    void enumerateIndexed(std::function<bool(const RootEntry&, size_t)> callback) const override;
    void enumerateUnder(const std::string& normalizedPrefix,
                        std::function<bool(const RootEntry&)> callback) const override;

    RootFormat format() const override {
        return RootFormat::Overwatch;
    }

    /// Access the parsed manifest entries (CMF/APM/TRG listing).
    const std::vector<OwRootFileEntry>& manifestEntries() const {
        return m_manifestEntries;
    }

protected:
    const std::vector<RootEntry>& entries() const override {
        return m_entries;
    }
    std::vector<RootEntry>& mutableEntries() override {
        return m_entries;
    }

private:
    /// All entries from parsed CMF files.
    std::vector<RootEntry> m_entries;

    /// Text root manifest entries.
    std::vector<OwRootFileEntry> m_manifestEntries;

    /// GUID → entry index, sorted for binary search. A hash multimap costs
    /// about five times as much per entry, which at Overwatch's scale is a
    /// gigabyte; TACTLib binary-searches its hash list for the same reason.
    std::vector<std::pair<u64, u32>> m_byGuid;

    /// Path lookup for the manifest rows only. An asset's path ends in its own
    /// GUID, so those are found through @ref m_byGuid instead — a string index
    /// over a current Overwatch install's twelve million assets costs more than
    /// the entries it points at.
    EntryIndex<std::string> m_byManifestPath;

    /// Number of leading entries in @ref m_entries that are manifest rows.
    size_t m_manifestRowCount = 0;

    /// Folder prefix per CMF, and where each CMF's assets begin in
    /// @ref m_entries — @ref m_cmfEntryStart has one extra element holding the
    /// end. A hundred and eighty-six manifests, so @ref assetPath finds an
    /// entry's folder with a binary search over a table that fits in cache
    /// rather than by storing the folder on every entry.
    std::vector<std::string> m_cmfPrefix;
    std::vector<u32> m_cmfEntryStart;

    void buildIndices(interfaces::WorkerPool* pool);

    /// Write @p index's asset path into @p out, reusing its capacity.
    void buildAssetPath(size_t index, std::string& out) const;

    /// The CMF that owns entry @p index, or m_cmfPrefix.size() if none does.
    size_t cmfForEntry(size_t index) const;
};

} // namespace whiteout::storages::casc

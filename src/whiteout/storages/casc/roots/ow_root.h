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

#include "root.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

/// Callback to resolve a CKey to raw file data (for fetching CMF files).
using CKeyResolver = std::function<std::vector<u8>(std::span<const u8, 16> cKey)>;

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

/// CMF header (version 26+, Overwatch 1.48+).
struct CmfHeader {
    u32 buildVersion = 0;
    i32 dataCount = 0;
    i32 entryCount = 0;
    u32 magic = 0;
    u8  version = 0;
    bool encrypted = false;
};

/// CMF hash data entry (version 25+).
struct CmfHashData {
    u64 guid = 0;
    u32 size = 0;
    u8  unknown = 0;
    std::array<u8, 16> contentKey{};
};

class OwRoot final : public RootManifest {
public:
    /// Parse an Overwatch text root file.
    /// @param data     Raw (BLTE-decoded) root file bytes (text format).
    /// @param resolver Callback that resolves CKey → file data (for CMF files).
    ///                 May be nullptr for root-only parsing (no CMF resolution).
    /// @param pool     Optional worker pool for parallel CMF parsing.
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<OwRoot> parse(std::span<const u8> data,
                                         CKeyResolver resolver = nullptr,
                                         interfaces::WorkerPool* pool = nullptr);

    /// Parse an Overwatch text root file from the manifest entries directly.
    /// Useful for testing without raw byte parsing.
    static std::unique_ptr<OwRoot> fromManifestEntries(
        std::vector<OwRootFileEntry> manifestEntries,
        CKeyResolver resolver = nullptr,
        interfaces::WorkerPool* pool = nullptr);

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByFileDataId(u32 fileDataId) const override;

    /// Find entries by 64-bit Overwatch GUID.
    std::vector<const RootEntry*> findByGuid(u64 guid) const;

    RootFormat format() const override { return RootFormat::Overwatch; }

    /// Access the parsed manifest entries (CMF/APM/TRG listing).
    const std::vector<OwRootFileEntry>& manifestEntries() const { return m_manifestEntries; }

protected:
    const std::vector<RootEntry>& entries() const override { return m_entries; }
    std::vector<RootEntry>& mutableEntries() override { return m_entries; }

private:
    /// All entries from parsed CMF files.
    std::vector<RootEntry> m_entries;

    /// Text root manifest entries.
    std::vector<OwRootFileEntry> m_manifestEntries;

    /// GUID-based lookup (GUID stored in fileNameHash field).
    std::unordered_multimap<u64, size_t> m_byGuid;

    /// Path-based lookup (for manifest file entries).
    std::unordered_multimap<std::string, size_t> m_byPath;

    void buildIndices();
};

} // namespace whiteout::storages::casc

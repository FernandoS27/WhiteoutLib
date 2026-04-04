// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file install_root.h
/// @brief Install-manifest-based root handler.
///
/// Some Blizzard products (Hearthstone, Diablo Immortal, Battle.net Agent,
/// Warcraft I/II, etc.) ship an opaque root file (e.g. an executable) that
/// has no parseable manifest.  However, their build config includes an
/// `install` key referencing an install manifest — a binary blob that maps
/// file names to content keys.  This handler parses that manifest so those
/// products can enumerate and read files normally.
///
/// Install manifest format (big-endian):
///   Header:  'IN' (2 bytes) + version (1) + hashSize (1) + numTags (2 BE) + numFiles (4 BE)
///   Tags:    numTags × { name: CString, type: i16 BE, bits: ceil(numFiles/8) bytes }
///   Files:   numFiles × { name: CString, cKey: hashSize bytes, size: u32 BE }
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"
#include "common/entry_index.h"

#include <span>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

/// Tag metadata from the install manifest.
struct InstallTag {
    std::string name;
    i16 type = 0;
};

class InstallRoot final : public RootManifest {
public:
    /// Parse an install manifest.
    /// @param data  Raw (BLTE-decoded) install manifest bytes.
    /// @param pool  Optional worker pool (unused, for API symmetry).
    /// @return Parsed root, or nullptr on failure / unrecognised format.
    static std::unique_ptr<InstallRoot> parse(std::span<const u8> data,
                                               interfaces::WorkerPool* pool = nullptr);

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByFileDataId(u32 fileDataId, FileIdHint hint = FileIdHint::None) const override;
    bool hasFileDataId(u32, FileIdHint = FileIdHint::None) const override { return false; }

    RootFormat format() const override { return RootFormat::Agent; }

    /// Access the tag list.
    const std::vector<InstallTag>& tags() const { return m_tags; }

protected:
    const std::vector<RootEntry>& entries() const override { return m_entries; }
    std::vector<RootEntry>& mutableEntries() override { return m_entries; }

private:
    std::vector<RootEntry> m_entries;
    std::vector<InstallTag> m_tags;

    /// Jenkins hash → entry index.
    EntryIndex<u64> m_byNameHash;

    /// Normalised path → entry index.
    EntryIndex<std::string> m_byPath;

    void buildIndices();
};

} // namespace whiteout::storages::casc

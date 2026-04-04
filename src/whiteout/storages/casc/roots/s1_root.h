// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file s1_root.h
/// @brief StarCraft Remastered / Agent text-based root handler.
///
/// S1 (and similar agent-based products like Hearthstone) use a plain-text root
/// where each line is:   filepath|ckey_hex
///                   or:  filepath:locale|ckey_hex
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"
#include "common/entry_index.h"

#include <span>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

class S1Root final : public RootManifest {
public:
    /// Parse an S1-style text root file.
    /// @param data  Raw (BLTE-decoded) root file bytes (UTF-8 text).
    /// @param pool  Optional worker pool (unused, for API symmetry).
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<S1Root> parse(std::span<const u8> data,
                                         interfaces::WorkerPool* pool = nullptr);

    /// Heuristic check: does the buffer look like an S1 text root?
    /// Returns true if the data appears to be pipe-delimited text lines with
    /// hex content keys (not starting with '#' which is Overwatch).
    static bool looksLikeS1Root(std::span<const u8> data);

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByFileDataId(u32 fileDataId) const override;
    bool hasFileDataId(u32) const override { return false; }

    RootFormat format() const override { return RootFormat::Agent; }

protected:
    const std::vector<RootEntry>& entries() const override { return m_entries; }
    std::vector<RootEntry>& mutableEntries() override { return m_entries; }

private:
    std::vector<RootEntry> m_entries;

    /// Path-based lookup via Jenkins hash (fileNameHash).
    EntryIndex<u64> m_byNameHash;

    /// Path string-based lookup for exact path queries.
    EntryIndex<std::string> m_byPath;

    void buildIndices();
};

} // namespace whiteout::storages::casc

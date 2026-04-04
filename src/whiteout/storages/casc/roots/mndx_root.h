// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file mndx_root.h
/// @brief MNDX trie-based root manifest parser (StarCraft II, Heroes of the Storm).
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"
#include "common/entry_index.h"

#include <span>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

class MndxRoot final : public RootManifest {
public:
    /// Parse an MNDX root file.
    /// @param data  Raw (BLTE-decoded) MNDX root bytes.
    /// @param pool  Optional worker pool (unused currently).
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<MndxRoot> parse(std::span<const u8> data,
                                           interfaces::WorkerPool* pool = nullptr);

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByNormalizedPath(const std::string& normalizedPath) const override;
    bool hasPath(const std::string& normalizedPath) const override;
    std::vector<const RootEntry*> findByFileDataId(u32 fileDataId) const override;
    bool hasFileDataId(u32) const override { return false; }
    RootFormat format() const override { return RootFormat::Mndx; }

protected:
    const std::vector<RootEntry>& entries() const override { return m_entries; }
    std::vector<RootEntry>& mutableEntries() override { return m_entries; }

private:
    std::vector<RootEntry> m_entries;
    EntryIndex<std::string> m_byPath;

    void buildIndices();
};

} // namespace whiteout::storages::casc

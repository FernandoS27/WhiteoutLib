// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file wow_root.h
/// @brief WoW root manifest parser (all three header versions).
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

class WowRoot final : public RootManifest {
public:
    /// Parse a WoW root file (auto-detects header version).
    /// @param data  Raw (BLTE-decoded) root manifest bytes.
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<WowRoot> parse(std::span<const u8> data,
                                          interfaces::WorkerPool* pool = nullptr);

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByFileDataId(u32 fileDataId) const override;
    bool hasFileDataId(u32 fileDataId) const override;
    std::vector<const RootEntry*> findByCKey(std::span<const u8, 16> cKey) const override;
    RootFormat format() const override { return RootFormat::Wow; }

protected:
    const std::vector<RootEntry>& entries() const override { return m_entries; }
    std::vector<RootEntry>& mutableEntries() override { return m_entries; }

private:
    /// All entries in flat storage.
    std::vector<RootEntry> m_entries;

    /// Dual index: FileDataId → indices into m_entries.
    std::unordered_multimap<u32, size_t> m_byFileDataId;

    /// Dual index: JenkinsHash(name) → indices into m_entries.
    std::unordered_multimap<u64, size_t> m_byNameHash;

    void buildIndices();
};

} // namespace whiteout::storages::casc

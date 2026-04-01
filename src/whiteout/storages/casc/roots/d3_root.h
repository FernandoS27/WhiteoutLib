// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file d3_root.h
/// @brief Diablo III root manifest parser (hierarchical directory format).
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

/// Callback to resolve a CKey to raw file data (for sub-directory resolution).
using CKeyResolver = std::function<std::vector<u8>(std::span<const u8, 16> cKey)>;

class D3Root final : public RootManifest {
public:
    /// Parse a D3 root file.
    /// @param data     Raw (BLTE-decoded) root directory bytes.
    /// @param resolver Callback that resolves CKey → file data (for sub-directories).
    ///                 May be nullptr if sub-directory resolution is not needed.
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<D3Root> parse(std::span<const u8> data,
                                         CKeyResolver resolver = nullptr,
                                         interfaces::WorkerPool* pool = nullptr);

    // --- RootManifest interface ---
    std::vector<RootEntry> findByPath(const std::string& path) const override;
    std::vector<RootEntry> findByFileDataId(u32 fileDataId) const override;
    RootFormat format() const override { return RootFormat::Diablo3; }

protected:
    const std::vector<RootEntry>& entries() const override { return m_entries; }

private:
    /// All entries in flat storage.
    std::vector<RootEntry> m_entries;

    /// Path-based lookup (lowercase normalized).
    std::unordered_multimap<std::string, size_t> m_byPath;

    /// FileDataId-based lookup (fileIndex = SNO ID).
    std::unordered_multimap<u32, size_t> m_byFileDataId;

    void buildIndices(interfaces::WorkerPool* pool = nullptr);
};

} // namespace whiteout::storages::casc

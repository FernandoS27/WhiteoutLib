// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file tvfs_root.h
/// @brief TVFS root manifest parser (prefix-tree, WC3 Reforged).
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

class TvfsRoot final : public RootManifest {
public:
    /// Parse a single TVFS blob.
    /// @param data  Raw (BLTE-decoded) TVFS root bytes.
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<TvfsRoot> parse(std::span<const u8> data,
                                            interfaces::WorkerPool* pool = nullptr);

    /// Merge entries from another TvfsRoot into this one.
    void merge(const TvfsRoot& other);

    // --- RootManifest interface ---
    std::vector<RootEntry> findByPath(const std::string& path) const override;
    std::vector<RootEntry> findByFileDataId(u32 fileDataId) const override;
    std::vector<RootEntry> findByCKey(std::span<const u8, 16> cKey) const override;
    void enumerate(std::function<bool(const RootEntry&)> callback) const override;
    size_t entryCount() const override;
    RootFormat format() const override { return RootFormat::Tvfs; }

private:
    std::vector<RootEntry> m_entries;

    /// Path-based index (lowercase normalized).
    std::unordered_multimap<std::string, size_t> m_byPath;

    void buildIndices(interfaces::WorkerPool* pool = nullptr);
};

} // namespace whiteout::storages::casc

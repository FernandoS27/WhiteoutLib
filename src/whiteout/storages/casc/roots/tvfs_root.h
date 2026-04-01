// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file tvfs_root.h
/// @brief TVFS root manifest parser (prefix-tree, WC3 Reforged).
///
/// Internal header — not part of the public include path.
#pragma once

#include "root.h"

#include <array>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::interfaces { class WorkerPool; }

namespace whiteout::storages::casc {

/// Resolver function for VFS sub-manifest data.
/// Given an EKey (eKeySize bytes), returns the decoded TVFS blob, or empty on failure.
using VfsResolver = std::function<std::vector<u8>(std::span<const u8> eKey)>;

class TvfsRoot final : public RootManifest {
public:
    /// Parse a single TVFS blob (no sub-container resolution).
    /// @param data  Raw (BLTE-decoded) TVFS root bytes.
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<TvfsRoot> parse(std::span<const u8> data,
                                            interfaces::WorkerPool* pool = nullptr);

    /// Parse a TVFS blob with sub-container resolution (WC3 Reforged multi-VFS).
    /// When a leaf entry's EKey matches a known VFS sub-manifest, the entry is
    /// treated as a sub-container: a ':' separator is appended to the path and
    /// the sub-manifest is recursively parsed with that prefix. This matches
    /// CascLib's path reporting behavior.
    /// @param data      Raw (BLTE-decoded) TVFS root bytes.
    /// @param resolver  Resolves VFS sub-manifest EKeys to decoded data.
    /// @param vfsEKeys  EKeys of known VFS sub-manifests (matched by first eKeySize bytes).
    /// @param pool      Optional worker pool for parallel index building.
    static std::unique_ptr<TvfsRoot> parse(std::span<const u8> data,
                                            const VfsResolver& resolver,
                                            const std::vector<std::array<u8, 16>>& vfsEKeys,
                                            interfaces::WorkerPool* pool = nullptr);

    /// Merge entries from another TvfsRoot into this one.
    void merge(const TvfsRoot& other);

    // --- RootManifest interface ---
    std::vector<RootEntry> findByPath(const std::string& path) const override;
    std::vector<RootEntry> findByFileDataId(u32 fileDataId) const override;
    RootFormat format() const override { return RootFormat::Tvfs; }

protected:
    const std::vector<RootEntry>& entries() const override { return m_entries; }

private:
    std::vector<RootEntry> m_entries;

    /// Path-based index (lowercase normalized).
    std::unordered_multimap<std::string, size_t> m_byPath;

    void buildIndices(interfaces::WorkerPool* pool = nullptr);
};

} // namespace whiteout::storages::casc

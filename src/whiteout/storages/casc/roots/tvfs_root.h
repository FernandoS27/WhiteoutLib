// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file tvfs_root.h
/// @brief TVFS root manifest parser (prefix-tree, WC3 Reforged).
///
/// Internal header — not part of the public include path.
#pragma once

#include "../tables/flat_hash_map.h"
#include "common/path_trie.h"
#include "root.h"

#include <array>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::storages::casc {

/// Resolver function for VFS sub-manifest data.
/// Given an EKey (eKeySize bytes), returns the decoded TVFS blob, or empty on failure.
using VfsResolver = std::function<std::vector<u8>(std::span<const u8> eKey)>;

class TvfsRoot final : public RootManifest {
public:
    /// Parse a single TVFS blob (no sub-container resolution).
    /// @param data     Raw (BLTE-decoded) TVFS root bytes.
    /// @param pool     Optional worker pool for parallel index building.
    /// @param buildIdx Build the path index. Pass false when a decorator
    ///                 (WowTvfsRoot) builds its own indices — avoids a wasted
    ///                 O(n) pass. Call ensureIndexed() later if needed.
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<TvfsRoot> parse(std::span<const u8> data,
                                           interfaces::WorkerPool* pool = nullptr,
                                           bool buildIdx = true);

    /// Parse a TVFS blob with sub-container resolution (WC3 Reforged multi-VFS).
    /// When a leaf entry's EKey matches a known VFS sub-manifest, the entry is
    /// treated as a sub-container: a ':' separator is appended to the path and
    /// the sub-manifest is recursively parsed with that prefix. This matches
    /// CascLib's path reporting behavior.
    /// @param data      Raw (BLTE-decoded) TVFS root bytes.
    /// @param resolver  Resolves VFS sub-manifest EKeys to decoded data.
    /// @param vfsEKeys  EKeys of known VFS sub-manifests (matched by first eKeySize bytes).
    /// @param pool      Optional worker pool for parallel index building.
    /// @param buildIdx  Build the path index (see single-arg overload).
    static std::unique_ptr<TvfsRoot> parse(std::span<const u8> data, const VfsResolver& resolver,
                                           const std::vector<std::array<u8, 16>>& vfsEKeys,
                                           interfaces::WorkerPool* pool = nullptr,
                                           bool buildIdx = true);

    /// Build the path index if it hasn't been built yet. No-op otherwise.
    void ensureIndexed(interfaces::WorkerPool* pool = nullptr);

    /// Move the entry table out, leaving this root empty. Used by decorators
    /// (WowTvfsRoot) that transform the entries in place — avoids a second
    /// allocation + copy of a multi-million-element vector.
    std::vector<RootEntry> takeEntries();

    /// Merge entries from another TvfsRoot into this one.
    void merge(const TvfsRoot& other);

    // --- RootManifest interface ---
    std::vector<const RootEntry*> findByPath(const std::string& path) const override;
    std::vector<const RootEntry*> findByNormalizedPath(
        const std::string& normalizedPath) const override;
    bool hasPath(const std::string& normalizedPath) const override;
    std::vector<const RootEntry*> findByFileDataId(
        u32 fileDataId, FileIdHint hint = FileIdHint::None) const override;
    bool hasFileDataId(u32, FileIdHint = FileIdHint::None) const override {
        return false;
    }
    RootFormat format() const override {
        return RootFormat::Tvfs;
    }

    /// Enumerate all entries whose path starts with @p normalizedPrefix (directory-scoped).
    /// The prefix should be a normalized path (lowercase, backslash-separated).
    /// Use an empty prefix to enumerate everything.  Callback returns false to stop.
    void enumerateUnder(const std::string& normalizedPrefix,
                        std::function<bool(const RootEntry&)> callback) const override;

protected:
    const std::vector<RootEntry>& entries() const override {
        return m_entries;
    }
    std::vector<RootEntry>& mutableEntries() override {
        return m_entries;
    }

private:
    std::vector<RootEntry> m_entries;

    /// Hash-map index for O(1) exact path lookup.
    /// Keys are FNV-1a hashes of normalized paths; values are head indices
    /// into m_entries.  Entries sharing the same path hash are chained via
    /// m_chainNext (singly-linked list, UINT32_MAX = end).
    FlatHashMap<u32> m_byPathMap;
    std::vector<u32> m_chainNext; ///< Parallel to m_entries; links same-hash entries.

    /// Flat-array trie for prefix enumeration (lazy-built on first use).
    mutable std::vector<PathTrieNode> m_trie;

    void buildIndices(interfaces::WorkerPool* pool = nullptr, bool preNormalized = false);
    void ensureTrie() const;
};

} // namespace whiteout::storages::casc

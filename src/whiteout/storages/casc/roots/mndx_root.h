// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file mndx_root.h
/// @brief MNDX trie-based root manifest parser (StarCraft II, Heroes of the Storm).
///
/// Internal header — not part of the public include path.
#pragma once

#include "../tables/flat_hash_map.h"
#include "root.h"

#include <mutex>
#include <span>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::storages::casc {

class MndxRoot final : public RootManifest {
public:
    /// Parse an MNDX root file.
    /// @param data  Raw (BLTE-decoded) MNDX root bytes.
    /// @param pool  Optional worker pool; when given, the path index is built
    ///              eagerly with the hashing fanned out over it. Without one
    ///              the index is built lazily on first path lookup.
    /// @return Parsed root, or nullptr on failure.
    static std::unique_ptr<MndxRoot> parse(std::span<const u8> data,
                                           interfaces::WorkerPool* pool = nullptr);

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
        return RootFormat::Mndx;
    }

protected:
    const std::vector<RootEntry>& entries() const override {
        return m_entries;
    }
    std::vector<RootEntry>& mutableEntries() override {
        return m_entries;
    }

private:
    /// Sentinel terminating a path-collision chain.
    static constexpr u32 kNoPathChain = 0xFFFFFFFFu;

    std::vector<RootEntry> m_entries;

    /// Path index: normalized-path hash → head entry, chained through
    /// m_pathChain in ascending entry order. Same layout as WowTvfsRoot's.
    mutable FlatHashMap<u32> m_byPathHead;
    mutable std::vector<u32> m_pathChain;
    mutable std::once_flag m_pathIndexOnce;

    /// Build the path index now, fanning the hashing out over @p pool. Only
    /// safe from a thread that is not itself one of that pool's workers;
    /// parse() calls it on the open thread.
    void buildPathIndex(interfaces::WorkerPool* pool);

    /// Build the path index on the calling thread if it hasn't been built yet.
    void ensurePathIndex() const;

    void buildPathIndexImpl(interfaces::WorkerPool* pool) const;

    std::vector<const RootEntry*> findByPathKey(const std::string& normalizedKey) const;
};

} // namespace whiteout::storages::casc

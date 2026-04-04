// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file root.h
/// @brief Abstract CASC root manifest interface + auto-detection.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/storages/casc/types.h>

#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

// Locale and content flags are defined in the public <whiteout/storages/casc/types.h>.
// Use LocaleMasks:: and ContentFlags:: from there. No duplicate definitions needed.

/// Invalid file-data-ID sentinel.
constexpr u32 kInvalidFileDataId = 0xFFFFFFFF;

/// A single entry from a root manifest.
struct RootEntry {
    std::array<u8, 16> cKey{};
    std::array<u8, 16> eKey{};                    ///< EKey (if available, e.g. from TVFS).
    u32 fileDataId = kInvalidFileDataId;          ///< WoW-specific file-data ID.
    u64 fileNameHash = 0;                         ///< Jenkins hash (WoW path lookup).
    u32 localeFlags = 0;
    u32 contentFlags = 0;
    u64 fileSize = 0;                             ///< Cached uncompressed size (resolved from encoding).
    std::string path;                             ///< Resolved path (if available).

    /// Container sub-entry support.  When containerOffset != 0 this entry
    /// refers to a slice of a larger container file (e.g. D4 combined meta).
    /// The storage layer will decode the full container, then return
    /// headerPrefix[0..headerSize) + decoded[containerOffset..+containerSize).
    u64 containerOffset = 0;    ///< Byte offset within the decoded container (0 = whole file).
    u32 containerSize = 0;      ///< Size of the sub-entry within the container.
    u8  headerSize = 0;         ///< Number of valid bytes in headerPrefix (0–16).
    std::array<u8, 16> headerPrefix{}; ///< Prepended to the extracted data (e.g. synthetic SNO header).
};

/// Select the best matching root entry based on locale/content flags.
/// Returns nullptr only if @p entries is empty.
inline const RootEntry* selectBestEntry(const std::vector<const RootEntry*>& entries,
                                        u32 localeFlags) {
    for (auto* e : entries) {
        if (e->contentFlags & ContentFlags::DoNotLoad) continue;
        if (localeFlags != 0 && e->localeFlags != 0 && (e->localeFlags & localeFlags) == 0)
            continue;
        return e;
    }
    return entries.empty() ? nullptr : entries[0];
}

/// Abstract base class for CASC root manifest parsers.
class RootManifest {
public:
    virtual ~RootManifest() = default;

    /// Find entries matching a path (normalized + Jenkins-hashed internally).
    virtual std::vector<const RootEntry*> findByPath(const std::string& path) const = 0;

    /// Find entries matching an already-normalized path (lowercase, backslash-separated,
    /// no leading/trailing separators). Skips the redundant normalizeCascPath() call.
    /// Default implementation delegates to findByPath().
    virtual std::vector<const RootEntry*> findByNormalizedPath(const std::string& normalizedPath) const {
        return findByPath(normalizedPath);
    }

    /// Check whether any entry exists for an already-normalized path.
    /// Default scans findByNormalizedPath but subclasses can override for O(1).
    virtual bool hasPath(const std::string& normalizedPath) const {
        return !findByNormalizedPath(normalizedPath).empty();
    }

    /// Check whether any entry exists for a FileDataId.
    /// Default scans findByFileDataId but subclasses can override for O(1).
    virtual bool hasFileDataId(u32 fileDataId) const {
        return !findByFileDataId(fileDataId).empty();
    }

    /// Find entries by WoW-style FileDataId (returns empty for non-WoW roots).
    virtual std::vector<const RootEntry*> findByFileDataId(u32 fileDataId) const = 0;

    /// Find entries by content key (linear scan — override for faster lookup).
    virtual std::vector<const RootEntry*> findByCKey(std::span<const u8, 16> cKey) const {
        std::vector<const RootEntry*> results;
        for (auto& e : entries()) {
            if (std::memcmp(e.cKey.data(), cKey.data(), 16) == 0)
                results.push_back(&e);
        }
        return results;
    }

    /// Enumerate all entries. Callback returns false to stop.
    virtual void enumerate(std::function<bool(const RootEntry&)> callback) const {
        for (auto& e : entries()) {
            if (!callback(e)) break;
        }
    }

    /// Enumerate entries whose path starts with @p normalizedPrefix.
    /// The prefix must already be normalized (lowercase, backslash-separated,
    /// no leading/trailing separators). Default implementation does a linear
    /// scan; subclasses with hierarchical indices (e.g. TVFS trie) override
    /// for O(prefix-depth + matches) performance.
    virtual void enumerateUnder(const std::string& normalizedPrefix,
                                std::function<bool(const RootEntry&)> callback) const {
        if (!callback) return;
        for (auto& e : entries()) {
            if (e.path.size() >= normalizedPrefix.size() &&
                e.path.compare(0, normalizedPrefix.size(), normalizedPrefix) == 0)
            {
                if (!callback(e)) break;
            }
        }
    }

    /// Total number of root entries.
    virtual size_t entryCount() const { return entries().size(); }

    /// Which root format this manifest represents.
    virtual RootFormat format() const = 0;

    /// Pre-resolve additional data for all entries (e.g. file sizes from encoding).
    template<typename Fn>
    void resolveEntries(Fn&& fn) {
        for (auto& e : mutableEntries())
            fn(e);
    }

    /// Auto-detect root format from raw (BLTE-decoded) data and construct
    /// the correct parser.
    /// @param data  Raw root file data.
    /// @return Parsed manifest, or nullptr on failure.
    static std::unique_ptr<RootManifest> parse(std::span<const u8> data);

protected:
    /// Access the flat entry storage. Subclasses must override this.
    virtual const std::vector<RootEntry>& entries() const = 0;

    /// Mutable access to entries (for pre-resolution during load).
    virtual std::vector<RootEntry>& mutableEntries() = 0;
};

// Well-known root format magic signatures.
namespace RootSignature {
    constexpr u32 kMFST   = 0x5453464D; ///< 'MFST' (WoW root, build 30080+).
    constexpr u32 kTVFS   = 0x53465654; ///< 'TVFS' (WC3 Reforged / TVFS root).
    constexpr u32 kMNDX   = 0x58444E4D; ///< 'MNDX' (SC2, HotS trie-based root).
    constexpr u32 kD3Root     = 0x8007D0C4; ///< Diablo 3 root directory signature.
    constexpr u32 kD3Dir      = 0xEAF1FE87; ///< Diablo 3 subdirectory signature.
    constexpr u32 kD3Packages = 0xAABB0002; ///< Diablo 3 packages signature.
} // namespace RootSignature

} // namespace whiteout::storages::casc

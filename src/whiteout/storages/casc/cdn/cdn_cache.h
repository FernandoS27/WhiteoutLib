// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file cdn_cache.h
/// @brief Disk-backed cache for CDN data, mirroring CDN directory structure.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

/// Disk cache that mirrors CDN directory structure.
///
/// Layout:
///   <cacheDir>/config/XX/YY/<hash>            (build config, CDN config)
///   <cacheDir>/data/XX/YY/<hash>              (loose files, encoding, root)
///   <cacheDir>/indices/XX/YY/<hash>.index     (archive index files)
///   <cacheDir>/archives/XX/YY/<hash>/<offset>_<size>.blte  (range segments)
class CdnCache {
public:
    explicit CdnCache(const std::string& cacheDir);

    /// Check if a resource is cached.
    bool has(const std::string& pathType, const std::string& keyHex) const;

    /// Read a cached resource.
    std::optional<std::vector<u8>> read(const std::string& pathType,
                                        const std::string& keyHex) const;

    /// Write a resource to cache.
    void write(const std::string& pathType, const std::string& keyHex, std::span<const u8> data);

    /// Check if a partial archive range is cached.
    bool hasRange(const std::string& archiveKeyHex, u64 offset, u32 size) const;

    /// Read a cached archive range.
    std::optional<std::vector<u8>> readRange(const std::string& archiveKeyHex, u64 offset,
                                             u32 size) const;

    /// Write a partial archive range to cache.
    void writeRange(const std::string& archiveKeyHex, u64 offset, u32 size,
                    std::span<const u8> data);

private:
    std::string m_cacheDir;

    /// Build path: <cacheDir>/<pathType>/XX/YY/<keyHex>
    std::string resourcePath(const std::string& pathType, const std::string& keyHex) const;

    /// Build path: <cacheDir>/archives/XX/YY/<archiveKeyHex>/<offset>_<size>.blte
    std::string rangePath(const std::string& archiveKeyHex, u64 offset, u32 size) const;

    /// Ensure directory exists (creates parents).
    static void ensureDir(const std::string& path);

    /// Read entire file contents, or nullopt if not found.
    static std::optional<std::vector<u8>> readFileBytes(const std::string& path);

    /// Write data to file atomically (temp + rename).
    static void writeFileBytes(const std::string& path, std::span<const u8> data);
};

} // namespace whiteout::storages::casc

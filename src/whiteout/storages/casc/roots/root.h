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
    std::string path;                             ///< Resolved path (if available).
};

/// Abstract base class for CASC root manifest parsers.
class RootManifest {
public:
    virtual ~RootManifest() = default;

    /// Find entries matching a path (normalized + Jenkins-hashed internally).
    virtual std::vector<RootEntry> findByPath(const std::string& path) const = 0;

    /// Find entries by WoW-style FileDataId (returns empty for non-WoW roots).
    virtual std::vector<RootEntry> findByFileDataId(u32 fileDataId) const = 0;

    /// Find entries by content key.
    virtual std::vector<RootEntry> findByCKey(std::span<const u8, 16> cKey) const = 0;

    /// Enumerate all entries. Callback returns false to stop.
    virtual void enumerate(std::function<bool(const RootEntry&)> callback) const = 0;

    /// Total number of root entries.
    virtual size_t entryCount() const = 0;

    /// Which root format this manifest represents.
    virtual RootFormat format() const = 0;

    /// Auto-detect root format from raw (BLTE-decoded) data and construct
    /// the correct parser.
    /// @param data  Raw root file data.
    /// @return Parsed manifest, or nullptr on failure.
    static std::unique_ptr<RootManifest> parse(std::span<const u8> data);
};

// Well-known root format magic signatures.
namespace RootSignature {
    constexpr u32 kMFST   = 0x5453464D; ///< 'MFST' (WoW root, build 30080+).
    constexpr u32 kTVFS   = 0x53465654; ///< 'TVFS' (WC3 Reforged / TVFS root).
    constexpr u32 kD3Root     = 0x8007D0C4; ///< Diablo 3 root directory signature.
    constexpr u32 kD3Dir      = 0xEAF1FE87; ///< Diablo 3 subdirectory signature.
    constexpr u32 kD3Packages = 0xAABB0002; ///< Diablo 3 packages signature.
} // namespace RootSignature

} // namespace whiteout::storages::casc

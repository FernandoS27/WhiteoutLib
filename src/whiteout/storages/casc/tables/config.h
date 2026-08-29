// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file config.h
/// @brief CASC configuration file parsers (.build.info, build config, CDN config, shmem).
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

// ============================================================================
// .build.info
// ============================================================================

struct BuildInfo {
    std::string branch;
    bool active = false;
    std::array<u8, 16> buildKey{};
    std::array<u8, 16> cdnKey{};
    std::string cdnPath;
    std::string version;
    std::string product;
};

/// Parse `.build.info` TSV with typed column headers.
std::vector<BuildInfo> parseBuildInfo(std::span<const u8> data);

/// Parse a flavor subdirectory's `.flavor.info` and return its "Product Flavor"
/// code (e.g. "w3t" for a `_ptr_` directory), or an empty string if absent.
std::string parseFlavorInfo(std::span<const u8> data);

// ============================================================================
// Build Config
// ============================================================================

struct BuildConfig {
    std::array<u8, 16> rootCKey{};
    std::array<u8, 16> encodingCKey{};
    std::array<u8, 16> encodingEKey{};
    u64 encodingSize = 0;
    std::array<u8, 16> downloadCKey{};
    std::array<u8, 16> installCKey{};
    std::string buildName;
    std::string buildProduct;
    std::string buildUid;
    // VFS fields (WC3 Reforged+)
    std::array<u8, 16> vfsRootCKey{};
    std::array<u8, 16> vfsRootEKey{};
    /// CKey+EKey pairs of vfs-1 .. vfs-N sub-manifests.
    struct VfsManifest {
        std::array<u8, 16> cKey{};
        std::array<u8, 16> eKey{};
    };
    std::vector<VfsManifest> vfsSubManifests;
};

/// Parse a build configuration file (key = value format, `#` comments).
BuildConfig parseBuildConfig(std::span<const u8> data);

// ============================================================================
// Static Build Config
// ============================================================================

/// Where an EKey keeps its archive coordinates in a "static build config"
/// install — the shape Steam ships. Such an install has no `.idx` files at
/// all: the bytes trailing the hash in every EKey spell out which
/// `data.<chunk>.<uid>` holds the blob and where, laid out as the config's
/// `key-layout-N` line describes.
struct StaticKeyLayout {
    bool valid = false;
    u32 hashBytes = 0; ///< Leading EKey bytes that are the real hash.
    u32 chunkBytes = 0;
    u32 uidBytes = 0;
    u32 offsetBytes = 0;
};

/// Parse the `key-layout-*` lines of a static build config. Returns an invalid
/// layout when they are absent or describe a shape we cannot decode.
StaticKeyLayout parseStaticKeyLayout(std::span<const u8> data);

// ============================================================================
// CDN Config
// ============================================================================

struct CdnConfig {
    std::vector<std::array<u8, 16>> archiveEKeys;
    std::vector<u32> archiveIndexSizes;
    std::string fileIndex;
    u32 fileIndexSize = 0;
};

/// Parse a CDN configuration file.
CdnConfig parseCdnConfig(std::span<const u8> data);

// ============================================================================
// Shmem
// ============================================================================

struct ShmemInfo {
    u32 archiveCount = 0;
    u32 version = 0;
};

/// Parse the shmem binary file.
ShmemInfo parseShmem(std::span<const u8> data);

// ============================================================================
// TSV Table (shared parser)
// ============================================================================

/// Generic TSV table: column names + row data.
struct TsvTable {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

/// Parse a TSV table with typed column headers (e.g. "Col!TYPE:SIZE|...").
TsvTable parseTsv(std::span<const u8> data);

// ============================================================================
// Online: Versions / CDNs Endpoint Responses
// ============================================================================

struct VersionInfo {
    std::string region;
    std::array<u8, 16> buildConfigKey{};
    std::array<u8, 16> cdnConfigKey{};
    std::array<u8, 16> keyRing{};
    u32 buildId = 0;
    std::string versionName;
    std::string productConfig;
};

struct CdnInfo {
    std::string region;
    std::string path;
    std::vector<std::string> hosts;
    std::string configPath;
};

/// Parse `/v2/products/{product}/versions` TSV response.
std::vector<VersionInfo> parseVersionsResponse(std::span<const u8> data);

/// Parse `/v2/products/{product}/cdns` TSV response.
std::vector<CdnInfo> parseCdnsResponse(std::span<const u8> data);

} // namespace whiteout::storages::casc

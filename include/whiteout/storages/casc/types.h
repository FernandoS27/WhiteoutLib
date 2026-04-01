// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file types.h
 * @brief Type definitions and option structs for the CASC storage module
 *
 * This file defines:
 * - LocaleMasks, ContentFlags, FileOpenFlags, StorageFeatureFlags constants
 * - RootFormat enumeration and ProgressCallback
 * - FileSpanInfo, FileFullInfo, FindEntry query structs
 * - StorageTag and StorageProduct metadata structs
 * - OpenOptions, CreateOptions, WriteOptions option structs
 * - BatchReadRequest and BatchReadResult for bulk file reads
 */

#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
} // namespace whiteout::interfaces

namespace whiteout::storages::casc {

// ============================================================================
// Constants
// ============================================================================

/// Invalid file-data-ID sentinel.
static constexpr i32 kInvalidId = 0;

// ============================================================================
// Locale Masks
// ============================================================================

namespace LocaleMasks {
static constexpr u32 None    = 0x00000000;
static constexpr u32 All     = 0xFFFFFFFF;
static constexpr u32 enUS    = 0x00000002;
static constexpr u32 koKR    = 0x00000004;
static constexpr u32 frFR    = 0x00000010;
static constexpr u32 deDE    = 0x00000020;
static constexpr u32 zhCN    = 0x00000040;
static constexpr u32 esES    = 0x00000080;
static constexpr u32 zhTW    = 0x00000100;
static constexpr u32 enGB    = 0x00000200;
static constexpr u32 enCN    = 0x00000400;
static constexpr u32 enTW    = 0x00000800;
static constexpr u32 esMX    = 0x00001000;
static constexpr u32 ruRU    = 0x00002000;
static constexpr u32 ptBR    = 0x00004000;
static constexpr u32 itIT    = 0x00008000;
static constexpr u32 ptPT    = 0x00010000;
} // namespace LocaleMasks

// ============================================================================
// Content Flags
// ============================================================================

namespace ContentFlags {
static constexpr u32 None            = 0x00000000;
static constexpr u32 LoadOnWindows   = 0x00000008;
static constexpr u32 LoadOnMacOS     = 0x00000010;
static constexpr u32 LowViolence     = 0x00000080;
static constexpr u32 DoNotLoad       = 0x00000100;
static constexpr u32 Encrypted       = 0x08000000;
static constexpr u32 NoNameHash      = 0x10000000;
static constexpr u32 Bundle          = 0x40000000;
static constexpr u32 NoCompression   = 0x80000000;
} // namespace ContentFlags

// ============================================================================
// File Open Flags
// ============================================================================

namespace FileOpenFlags {
static constexpr u32 None           = 0x00000000;
static constexpr u32 IgnoreLocale   = 0x00000001; ///< Return first match regardless of locale.
} // namespace FileOpenFlags

// ============================================================================
// Storage Feature Flags
// ============================================================================

namespace StorageFeatureFlags {
static constexpr u32 None          = 0x00000000;
static constexpr u32 LoadOnDemand  = 0x00000001; ///< Defer encoding/root loading until first use.
} // namespace StorageFeatureFlags

// ============================================================================
// Root Format
// ============================================================================

/// Root manifest format.
enum class RootFormat : u8 {
    Unknown,  ///< Could not determine format.
    Wow,      ///< World of Warcraft root (FileDataId-based).
    Diablo3,  ///< Diablo III root (hierarchical directory).
    Tvfs,     ///< TVFS prefix-tree root (WC3 Reforged and general purpose).
    Mndx,     ///< MNDX trie-based root (StarCraft II, Heroes of the Storm).
};

// ============================================================================
// Progress Callback
// ============================================================================

/// Progress step identifiers.
enum class ProgressStep : u8 {
    LoadingBuildConfig,
    LoadingCdnConfig,
    LoadingIndexFiles,
    MappingArchives,
    LoadingEncodingTable,
    LoadingRootManifest,
    Ready,
};

/**
 * @brief Callback for open/save progress reporting.
 * @param step    Current progress step.
 * @param current Items processed so far.
 * @param total   Total items (0 if unknown).
 * @return false to cancel the operation.
 */
using ProgressCallback = std::function<bool(ProgressStep step, u32 current, u32 total)>;

// ============================================================================
// Query Structs
// ============================================================================

/// Information about a BLTE span within an archive.
struct FileSpanInfo {
    u32 archiveIndex = 0;
    u64 archiveOffset = 0;
    u32 encodedSize = 0;
    u64 decodedSize = 0;
};

/// Full metadata for a CASC file.
struct FileFullInfo {
    std::array<u8, 16> cKey{};   ///< Content key (MD5 of raw data).
    std::array<u8, 16> eKey{};   ///< First encoding key.
    u64 fileSize = 0;            ///< Uncompressed file size.
    u32 localeFlags = 0;
    u32 contentFlags = 0;
    i32 fileDataId = kInvalidId; ///< WoW-specific file data ID.
    std::string path;            ///< Path (if known).
};

/// Entry returned by enumerate/list operations.
struct FindEntry {
    std::array<u8, 16> cKey{};
    u64 fileSize = 0;
    u32 localeFlags = 0;
    u32 contentFlags = 0;
    i32 fileDataId = kInvalidId;
    std::string path;
};

// ============================================================================
// Storage Tags & Product
// ============================================================================

/// Metadata tag (from install/download manifests).
struct StorageTag {
    std::string name;
    u16 type = 0;
};

/// Product identification info.
struct StorageProduct {
    std::string name;
    std::string version;
    std::string buildId;
};

// ============================================================================
// Options Structs
// ============================================================================

/// Options for opening an existing CASC storage.
struct OpenOptions {
    std::string path;                            ///< Path to the CASC data directory.
    std::string buildKey;                        ///< Optional hex-MD5 to select a specific build.
    u32 localeMask = LocaleMasks::None;          ///< Locale filter (0 = accept all).
    u32 flags = StorageFeatureFlags::None;       ///< Feature flags.
    ProgressCallback progressCallback = nullptr; ///< Optional progress callback.
    interfaces::WorkerPool* pool = nullptr;      ///< Optional worker pool for parallel I/O.
};

/// Options for creating a new empty CASC storage.
struct CreateOptions {
    std::string product = "custom";
    std::string version = "1.0.0";
    u32 archiveMaxSize = 0x40000000;  ///< 1 GB.
    u32 blteFrameSize = 0x10000;      ///< 64 KB.
    RootFormat rootFormat = RootFormat::Tvfs;
};

/// Options for writing a file into a CASC storage.
struct WriteOptions {
    u32 localeFlags = LocaleMasks::All;
    u32 contentFlags = 0;
    bool compress = true;
};

// ============================================================================
// Batch Read Types
// ============================================================================

/// Request for a single file in a batch read.
struct BatchReadRequest {
    std::string path;                             ///< CASC path (mutually exclusive with fileDataId).
    i32 fileDataId = kInvalidId;                  ///< WoW-style FileDataId.
    u32 localeFlags = LocaleMasks::None;          ///< Locale filter (0 = accept all).
    u32 openFlags = 0;                            ///< FileOpenFlags.
};

/// Result of a single file in a batch read.
struct BatchReadResult {
    std::vector<u8> data;                         ///< File data (empty on failure).
    bool success = false;                         ///< True if the file was read successfully.
    std::string error;                            ///< Diagnostic message (empty on success).
};

} // namespace whiteout::storages::casc

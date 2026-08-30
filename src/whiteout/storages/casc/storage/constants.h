// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file constants.h
/// @brief Shared CASC module constants and small utilities.
///
/// Internal header — not part of the public include path.
/// Only constants used by more than one translation unit belong here.
/// Single-file constants live in their respective .cpp files.
#pragma once

#include <whiteout/common_types.h>
#include "../../common/hex.h"

#include <array>
#include <string>

namespace whiteout::storages::casc {

// ============================================================================
// Archive constants
// ============================================================================

/// Size of the per-entry header prepended to each BLTE blob in a data archive.
/// Layout: EKey(16 reversed) + encodedSize(4 LE) + flags(2) + checksum(8) = 30 bytes.
static constexpr u32 kArchiveEntryHeaderSize = 30;

/// Archive slots reserved per chunk in a static-build-config install, whose
/// archives are named `data.<chunk>.<uid>` rather than `data.<n>`.
static constexpr u32 kStaticArchiveUidSpan = 256;

/// Flatten a static-layout `data.<chunk>.<uid>` name into an archive slot.
inline constexpr u32 staticArchiveSlot(u32 chunk, u32 uid) noexcept {
    return chunk * kStaticArchiveUidSpan + uid;
}

/// Container cache budget for Diablo IV, whose root entries are slices of
/// shared combined-meta containers. 3.0.x ships 30 of them, 382 MB decoded in
/// total and 33 MB for the largest; a budget that cannot hold the working set
/// thrashes, and each miss costs a full container re-decode. The cache fills
/// lazily, so this is a ceiling and not an allocation.
static constexpr size_t kD4ContainerCacheSize = 512ull * 1024 * 1024;

// ============================================================================
// Key sizes
// ============================================================================

/// Standard content key size (MD5, 16 bytes).
static constexpr u8 kCKeySize = 16;

/// Standard encoding key size (MD5, 16 bytes).
static constexpr u8 kEKeySize = 16;

// ============================================================================
// TVFS constants (shared between parser and writer)
// ============================================================================

/// TVFS format version.
static constexpr u8 kTvfsFormatVersion = 1;

/// TVFS path table control bytes.
static constexpr u8 kTvfsPathSeparator = 0x00;
static constexpr u8 kTvfsNodeValueMarker = 0xFF;

/// TVFS folder node indicator bit (bit 31 of nodeValue).
static constexpr u32 kTvfsFolderNodeBit = 0x80000000;

// ============================================================================
// Utility
// ============================================================================

/// Build a config file path from a base directory and a 16-byte hash key.
/// Result: <baseDir>/config/XX/YY/<hex32>
inline std::string configFilePath(const std::string& baseDir, const std::array<u8, 16>& key) {
    auto hash = storages::common::hexEncode16(key);
    return baseDir + "/config/" + hash.substr(0, 2) + "/" + hash.substr(2, 2) + "/" + hash;
}

} // namespace whiteout::storages::casc

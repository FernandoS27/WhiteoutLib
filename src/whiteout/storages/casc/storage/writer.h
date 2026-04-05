// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file writer.h
/// @brief CASC archive assembly pipeline (write support).
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include <whiteout/storages/casc/types.h>

#include <array>
#include <string>
#include <vector>

namespace whiteout::storages::casc {

/// A single file to write into a CASC storage.
struct WriteEntry {
    std::string path;                       // File path in the root manifest.
    u32 fileDataId = 0xFFFFFFFF;            // WoW-style FileDataId (0xFFFFFFFF = unused).
    u64 fileNameHash = 0;                   // Jenkins hash from WoW root (preserved on re-serialize).
    u32 localeFlags = 0;                    // Locale mask.
    u32 contentFlags = 0;                   // Content flags.

    // New/modified file data (overlay).
    std::vector<u8> rawData;
    bool compress = true;

    // Pre-encoded data from source (raw-copy path).
    std::vector<u8> encodedBlob;
    bool hasPreEncoded = false;             // If true, skip encode — copy blob directly.

    // Keys (filled during source raw-copy, or computed during write).
    std::array<u8, 16> cKey{};
    std::array<u8, 16> eKey{};
    u64 fileSize = 0;
};

struct WriterOptions {
    u32 archiveMaxSize = 0x40000000;        // 1 GB (kDefaultArchiveMaxSize).
    u32 blteFrameSize = 0x10000;            // 64 KB (kDefaultBlteFrameSize).
    bool compressFiles = true;
    RootFormat rootFormat = RootFormat::Tvfs;
    std::string product = "custom";
    std::string version = "1.0.0";
};

/// Build and persist a complete CASC storage to disk.
/// @param outputDir  Directory to write the storage into.
/// @param entries    All files to include.
/// @param opts       Writer options.
/// @param pool       Optional worker pool for parallel encode.
/// @return true on success.
bool writeStorage(const std::string& outputDir,
                  std::vector<WriteEntry>& entries,
                  const WriterOptions& opts = {},
                  interfaces::WorkerPool* pool = nullptr);

} // namespace whiteout::storages::casc

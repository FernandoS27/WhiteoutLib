// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file writer.h
/// @brief MPQ archive writer — assembles a complete MPQ from tables + file data.

#pragma once

#include <whiteout/common_types.h>

#include "block_table.h"
#include "compression.h"
#include "file_data.h"
#include "hash_table.h"
#include "header.h"
#include "special_files.h"

#include <span>
#include <string>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::storages::mpq {

/// Describes a file to be written into the new archive.
struct WriteEntry {
    std::string filename;
    u16 locale = 0;

    /// If rawData is non-empty, this is a new/modified file from the overlay.
    std::vector<u8> rawData;
    u8 compression = CompressionFlag::kZlib;
    bool encrypt = false;
    bool singleUnit = false;

    /// If rawSectors is non-empty, this is a raw copy from the source archive
    /// (compressed sectors copied as-is, no re-encoding needed).
    std::span<const u8> rawSectors;
    BlockEntry sourceBlock{}; ///< Original block entry (for raw copies).
};

/// Write a complete MPQ archive to a byte buffer.
///
/// The writer:
/// 1. Writes the MPQ header.
/// 2. Writes file data sequentially (raw sector copies + freshly encoded files).
/// 3. Writes (listfile) and (attributes) as regular files.
/// 4. Writes encrypted hash table.
/// 5. Writes encrypted block table (+ hi-block table if V2+).
/// 6. Updates header with final offsets and sizes.
///
/// @param header    Template header (version, sector size, etc.).
/// @param entries   Files to write.
/// @param hashTableCapacity  Hash table size (power of 2).
/// @return The complete archive as a byte vector.
[[nodiscard]] std::vector<u8> writeArchive(const MpqHeader& header,
                                           const std::vector<WriteEntry>& entries,
                                           u32 hashTableCapacity,
                                           interfaces::WorkerPool* pool = nullptr);

} // namespace whiteout::storages::mpq

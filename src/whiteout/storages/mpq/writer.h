// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file writer.h
/// @brief MPQ archive writer — assembles a complete MPQ from tables + file data.

#pragma once

#include <whiteout/common_types.h>

#include "codecs/compression.h"
#include "file_data.h"
#include "special_files.h"
#include "tables/block_table.h"
#include "tables/hash_table.h"
#include "tables/header.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::storages::mpq {

/// Describes a file to be written into the new archive.
struct WriteEntry {
    /// Empty for a file of the source archive that no listfile names: it is
    /// carried by @ref hashA / @ref hashB at @ref sourceSlot instead.
    std::string filename;
    u16 locale = 0;
    u16 platform = 0;
    u32 hashA = 0;
    u32 hashB = 0;
    /// The source hash table slot it came from, kept while the table keeps
    /// its size: a nameless entry cannot be probed for again.
    std::optional<u32> sourceSlot;
    /// The HET name hash of a nameless entry, from the source's own HET/BET
    /// tables; 0 when unknown.
    u64 nameHash = 0;
    /// Where it stands in (listfile), which is also the order the HET table
    /// is filled in: the StarCraft II editor fills it in listfile order. Ties
    /// keep the entry order.
    u32 listRank = 0;

    /// If rawData is non-empty, this is a new/modified file from the overlay.
    std::vector<u8> rawData;
    CompressionFlag compression = CompressionFlag::kZlib;
    bool encrypt = false;
    bool singleUnit = false;

    /// A raw copy from the source archive (compressed sectors copied as-is,
    /// no re-encoding needed). Set with @ref rawSectors, which may be empty
    /// for an empty file.
    bool rawCopy = false;
    std::span<const u8> rawSectors;
    BlockEntry sourceBlock{}; ///< Original block entry (for raw copies).
    /// A FIX_KEY raw copy's key before its offset is added in, which is all
    /// it takes to encrypt it again where it lands.
    std::optional<u32> rekeyBase;

    /// (attributes) values carried from the source; unset ones are computed
    /// from @ref rawData, or left zero for a raw copy.
    std::optional<u32> crc32;
    std::optional<u64> filetime;
    std::optional<std::array<u8, 16>> md5;
};

/// How the archive around the entries is laid out.
struct WritePlan {
    /// Template header: version, sector size, raw chunk size.
    MpqHeader header;
    /// The hash table's size, grown to fit when the entries do not.
    u32 hashTableCapacity = 1024;
    /// The source archive's hash table. While the size holds, its slots are
    /// kept (an entry with a `sourceSlot` goes back where it was, the rest
    /// become deleted markers) so a nameless entry stays findable.
    const HashTable* sourceHashTable = nullptr;
    /// Write HET and BET tables (v3+).
    bool hetBet = false;
    /// Write (attributes) with these arrays.
    bool attributes = true;
    AttributeFlag attributeFlags = AttributeFlag::kCrc32 | AttributeFlag::kFiletime | AttributeFlag::kMd5;
    /// (listfile) and (attributes) as single units, the way the StarCraft II
    /// editor stores them. Classic Warcraft III predates single units.
    bool singleUnitSpecials = false;
};

/// A written archive, or why there is none.
struct WriteResult {
    std::vector<u8> archive;
    std::string error;
};

/// Write a complete MPQ archive to a byte buffer.
///
/// The writer:
/// 1. Writes the MPQ header.
/// 2. Writes file data sequentially (raw sector copies + freshly encoded
///    files), each followed by its raw chunk MD5s when the header asks.
/// 3. Writes (listfile) and (attributes) as regular files.
/// 4. Writes the HET and BET tables, when planned.
/// 5. Writes encrypted hash table.
/// 6. Writes encrypted block table (+ hi-block table if V2+).
/// 7. Updates header with final offsets, sizes and (V4) MD5s.
[[nodiscard]] WriteResult writeArchive(const WritePlan& plan, const std::vector<WriteEntry>& entries,
                                       interfaces::WorkerPool* pool = nullptr);

/// @ref writeArchive for a fresh archive with every attribute; empty on failure.
[[nodiscard]] std::vector<u8> writeArchive(const MpqHeader& header,
                                           const std::vector<WriteEntry>& entries,
                                           u32 hashTableCapacity,
                                           interfaces::WorkerPool* pool = nullptr);

} // namespace whiteout::storages::mpq

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file het_bet.h
/// @brief MPQ v3+ HET and BET tables — read and build.
///
/// A v4 archive (every StarCraft II map and mod) indexes its files twice: the
/// classic hash and block tables, and a HET table (64-bit name hashes, open
/// addressed) over a bit-packed BET table (one record per block). Both are
/// stored uncompressed, encrypted past their 12-byte extended header with the
/// classic tables' keys. The layout here is the one Blizzard's own writer
/// produces, field for field, measured against editor-saved `.SC2Map`s.

#pragma once

#include <whiteout/common_types.h>

#include "block_table.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::mpq {

/// "HET\x1A" and "BET\x1A".
static constexpr u32 kHetMagic = 0x1A544548;
static constexpr u32 kBetMagic = 0x1A544542;

/// The 64-bit name hash both tables file a name under: Jenkins' hashlittle2
/// over the lowercased, backslashed name, with the top bit set.
[[nodiscard]] u64 hetNameHash(const std::string& filename);

/// A parsed HET table.
struct HetTable {
    u32 entryCount = 0;
    u32 totalCount = 0;
    u32 nameHashBitSize = 64;
    u32 indexSizeTotal = 0;
    u32 indexSizeExtra = 0;
    u32 indexSize = 0;
    std::vector<u8> nameHashes1; ///< Top 8 bits of each slot's name hash; 0 = free.
    std::vector<u32> betIndexes; ///< Each slot's BET (= block) index.
};

/// A parsed BET table.
struct BetTable {
    u32 entryCount = 0;
    u32 unknown08 = 0;
    u32 tableEntrySize = 0;
    u32 bitIndexFilePos = 0, bitIndexFileSize = 0, bitIndexCmpSize = 0, bitIndexFlagIndex = 0,
        bitIndexUnknown = 0;
    u32 bitCountFilePos = 0, bitCountFileSize = 0, bitCountCmpSize = 0, bitCountFlagIndex = 0,
        bitCountUnknown = 0;
    u32 bitTotalNameHash2 = 0, bitExtraNameHash2 = 0, bitCountNameHash2 = 0;
    std::vector<u32> flags;
    struct Entry {
        u64 filePos = 0;
        u64 fileSize = 0;
        u64 cmpSize = 0;
        u32 flagIndex = 0;
        u64 nameHash2 = 0; ///< The name hash below its top 8 bits.
    };
    std::vector<Entry> entries;
};

/// Parse a table as stored in the archive (encrypted). Nothing when the magic,
/// the sizes or a bit width does not hold.
[[nodiscard]] std::optional<HetTable> parseHetTable(std::span<const u8> stored);
[[nodiscard]] std::optional<BetTable> parseBetTable(std::span<const u8> stored);

/// Each block's full name hash, as the two tables state it: the BET's low bits
/// under the HET's top byte for the slot that points at the block. 0 where no
/// slot does.
[[nodiscard]] std::vector<u64> blockNameHashes(const HetTable& het, const BetTable& bet);

/// The stored (encrypted) HET table for blocks whose name hashes are
/// @p nameHashes (by block index), filled in @p fillOrder: the block indices
/// in the order they take their slots. Empty fills in block order.
[[nodiscard]] std::vector<u8> buildHetTable(std::span<const u64> nameHashes,
                                            std::span<const u32> fillOrder = {});

/// The stored BET table for @p blocks, one name hash each.
[[nodiscard]] std::vector<u8> buildBetTable(std::span<const BlockEntry> blocks,
                                            std::span<const u64> nameHashes);

} // namespace whiteout::storages::mpq

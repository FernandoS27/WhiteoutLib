// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file block_table.h
/// @brief MPQ block table (classic V1/V2) — read, write/serialize.

#pragma once

#include <whiteout/common_types.h>

#include <optional>
#include <span>
#include <vector>

namespace whiteout::storages::mpq {

// ============================================================================
// File Flags (used in BlockEntry::flags)
// ============================================================================

enum class FileFlag : u32 {
    None = 0,
    kImplode = 0x00000100,      ///< File compressed with PKware DCL.
    kCompress = 0x00000200,     ///< File compressed with combination of codecs.
    kEncrypted = 0x00010000,    ///< File is encrypted.
    kFixKey = 0x00020000,       ///< Encryption key adjusted by file offset.
    kPatchFile = 0x00100000,    ///< Patch file (incremental update).
    kSingleUnit = 0x01000000,   ///< File stored as single unit (no sector splitting).
    kDeleteMarker = 0x02000000, ///< File is a delete marker (patch archives).
    kSectorCrc = 0x04000000,    ///< Per-sector CRC appended to sectors.
    kExists = 0x80000000,       ///< File exists.
};

inline FileFlag operator|(FileFlag a, FileFlag b) noexcept {
    return static_cast<FileFlag>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline FileFlag operator&(FileFlag a, FileFlag b) noexcept {
    return static_cast<FileFlag>(static_cast<u32>(a) & static_cast<u32>(b));
}
inline FileFlag operator~(FileFlag a) noexcept {
    return static_cast<FileFlag>(~static_cast<u32>(a));
}
inline FileFlag& operator|=(FileFlag& a, FileFlag b) noexcept {
    a = a | b;
    return a;
}
inline FileFlag& operator&=(FileFlag& a, FileFlag b) noexcept {
    a = a & b;
    return a;
}
inline bool hasFlag(FileFlag flags, FileFlag flag) noexcept {
    return (flags & flag) != FileFlag::None;
}

// ============================================================================
// Block Entry
// ============================================================================

/// A single 16-byte entry in the MPQ block table.
struct BlockEntry {
    u32 fileOffset = 0;              ///< Byte offset of the file data (relative to archive start).
    u32 compressedSize = 0;          ///< Compressed size of the file data.
    u32 uncompressedSize = 0;        ///< Uncompressed file size.
    FileFlag flags = FileFlag::None; ///< Combination of FileFlag values.

    [[nodiscard]] bool exists() const {
        return hasFlag(flags, FileFlag::kExists);
    }
    [[nodiscard]] bool isCompressed() const {
        return hasFlag(flags, FileFlag::kCompress) || hasFlag(flags, FileFlag::kImplode);
    }
    [[nodiscard]] bool isEncrypted() const {
        return hasFlag(flags, FileFlag::kEncrypted);
    }
    [[nodiscard]] bool hasFixKey() const {
        return hasFlag(flags, FileFlag::kFixKey);
    }
    [[nodiscard]] bool isSingleUnit() const {
        return hasFlag(flags, FileFlag::kSingleUnit);
    }
    [[nodiscard]] bool hasSectorCrc() const {
        return hasFlag(flags, FileFlag::kSectorCrc);
    }
};

static_assert(sizeof(BlockEntry) == 16, "BlockEntry must be exactly 16 bytes");

// ============================================================================
// Block Table
// ============================================================================

class BlockTable {
public:
    BlockTable() = default;

    /// Parse a block table from raw (encrypted) bytes.
    /// @param data     Raw block table data (must be blockTableEntries * 16 bytes).
    /// @param count    Number of entries (from header.blockTableEntries).
    /// @return true on success.
    bool parse(std::span<const u8> data, u32 count);

    /// Parse the hi-block table (V2+). Provides high 16 bits of file offsets.
    /// @param data   Raw hi-block table data (count * 2 bytes). NOT encrypted.
    /// @param count  Number of entries (same as block table).
    bool parseHiBlockTable(std::span<const u8> data, u32 count);

    /// Create an empty block table.
    void createEmpty();

    /// Append a new entry. Returns the block index.
    [[nodiscard]] u32 append(const BlockEntry& entry);

    /// Access an entry by index.
    [[nodiscard]] const BlockEntry& entry(u32 index) const {
        return m_entries[index];
    }
    [[nodiscard]] BlockEntry& entry(u32 index) {
        return m_entries[index];
    }

    /// Number of entries.
    [[nodiscard]] u32 count() const {
        return static_cast<u32>(m_entries.size());
    }

    /// Get the 48-bit file offset (block offset + hi-block table).
    [[nodiscard]] u64 fileOffset48(u32 index) const;

    /// Serialize block table to encrypted bytes for archive output.
    [[nodiscard]] std::vector<u8> serialize() const;

    /// Serialize hi-block table to raw bytes (not encrypted).
    [[nodiscard]] std::vector<u8> serializeHiBlockTable() const;

    /// Check if any entry has a high file offset (needs hi-block table).
    [[nodiscard]] bool needsHiBlockTable() const;

private:
    std::vector<BlockEntry> m_entries;
    std::vector<u16> m_hiBlockOffsets; ///< High 16 bits of file offsets (V2+).
};

} // namespace whiteout::storages::mpq

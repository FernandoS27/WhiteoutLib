// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file hash_table.h
/// @brief MPQ hash table (classic V1/V2) — read, lookup, write/serialize.

#pragma once

#include <whiteout/common_types.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace whiteout::storages::mpq {

// ============================================================================
// Constants
// ============================================================================

/// Sentinel: empty slot (never occupied).
static constexpr u32 kHashEntryEmpty = 0xFFFFFFFF;
/// Sentinel: deleted slot (was occupied, now free for probe continuation).
static constexpr u32 kHashEntryDeleted = 0xFFFFFFFE;

// ============================================================================
// Hash Entry
// ============================================================================

/// A single 16-byte entry in the MPQ hash table.
struct HashEntry {
    u32 hashA = kHashEntryEmpty;      ///< Hash of filename (type NameA).
    u32 hashB = kHashEntryEmpty;      ///< Hash of filename (type NameB).
    u16 locale = 0xFFFF;              ///< Locale ID.
    u16 platform = 0xFFFF;            ///< Platform (always 0 in practice).
    u32 blockIndex = kHashEntryEmpty; ///< Index into block table, or sentinel.

    [[nodiscard]] bool isEmpty() const {
        return blockIndex == kHashEntryEmpty;
    }
    [[nodiscard]] bool isDeleted() const {
        return blockIndex == kHashEntryDeleted;
    }
    [[nodiscard]] bool isOccupied() const {
        return !isEmpty() && !isDeleted();
    }
};

static_assert(sizeof(HashEntry) == 16, "HashEntry must be exactly 16 bytes");

// ============================================================================
// Hash Table
// ============================================================================

class HashTable {
public:
    HashTable() = default;

    /// Parse a hash table from raw (encrypted) bytes.
    /// @param data      Raw hash table data (must be hashTableEntries * 16 bytes).
    /// @param capacity  Number of entries (from header.hashTableEntries).
    /// @return true on success.
    bool parse(std::span<const u8> data, u32 capacity);

    /// Create an empty hash table with the given capacity.
    void createEmpty(u32 capacity);

    /// Look up a filename with any locale. Returns the hash table index,
    /// or nullopt if not found.
    [[nodiscard]] std::optional<u32> lookup(const std::string& filename) const;

    /// Look up a filename with a specific locale. Returns the hash table index,
    /// or nullopt if not found.
    [[nodiscard]] std::optional<u32> lookup(const std::string& filename, u16 locale) const;

    /// Insert a new entry. Used by the writer during save() when building fresh tables.
    /// Returns the hash table index, or nullopt if the table is full.
    [[nodiscard]] std::optional<u32> insert(const std::string& filename, u16 locale,
                                            u32 blockIndex);

    /// Mark an entry as deleted. Used by the writer during save().
    /// Returns true if the entry was found and removed.
    bool remove(const std::string& filename);

    /// Mark an entry with a specific locale as deleted.
    bool remove(const std::string& filename, u16 locale);

    /// Serialize the hash table to encrypted bytes for archive output.
    [[nodiscard]] std::vector<u8> serialize() const;

    /// Direct entry access.
    [[nodiscard]] const HashEntry& entry(u32 index) const {
        return m_entries[index];
    }
    [[nodiscard]] HashEntry& entry(u32 index) {
        return m_entries[index];
    }

    /// Number of entries (capacity, always power of 2).
    [[nodiscard]] u32 capacity() const {
        return static_cast<u32>(m_entries.size());
    }

    /// Number of occupied entries.
    [[nodiscard]] u32 occupiedCount() const;

private:
    std::vector<HashEntry> m_entries;
};

} // namespace whiteout::storages::mpq

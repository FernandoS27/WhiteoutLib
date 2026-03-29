// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "hash_table.h"
#include "crypto.h"

#include <algorithm>
#include <cstring>

namespace whiteout::storages::mpq {

// ============================================================================
// Parsing
// ============================================================================

bool HashTable::parse(std::span<const u8> data, u32 capacity) {
    const size_t expectedSize = static_cast<size_t>(capacity) * sizeof(HashEntry);
    if (data.size() < expectedSize) return false;

    // Copy the raw data and decrypt in place.
    std::vector<u32> raw(capacity * 4);
    std::memcpy(raw.data(), data.data(), expectedSize);
    decryptBlock(raw.data(), raw.size(), hashString("(hash table)", HashType::FileKey));

    // Convert u32 triples to HashEntry structs.
    m_entries.resize(capacity);
    for (u32 i = 0; i < capacity; ++i) {
        const u32* src = raw.data() + i * 4;
        m_entries[i].hashA = src[0];
        m_entries[i].hashB = src[1];
        m_entries[i].locale = static_cast<u16>(src[2] & 0xFFFF);
        m_entries[i].platform = static_cast<u16>((src[2] >> 16) & 0xFFFF);
        m_entries[i].blockIndex = src[3];
    }

    return true;
}

// ============================================================================
// Creation
// ============================================================================

void HashTable::createEmpty(u32 capacity) {
    m_entries.assign(capacity, HashEntry{}); // All filled with 0xFFFFFFFF sentinels.
}

// ============================================================================
// Lookup
// ============================================================================

std::optional<u32> HashTable::lookup(const std::string& filename) const {
    if (m_entries.empty()) return std::nullopt;

    u32 hashA = hashString(filename, HashType::NameA);
    u32 hashB = hashString(filename, HashType::NameB);
    u32 startIndex = hashString(filename, HashType::TableOffset) & (capacity() - 1);

    u32 index = startIndex;
    do {
        const auto& e = m_entries[index];
        if (e.isEmpty()) return std::nullopt; // End of probe chain.
        if (e.isOccupied() && e.hashA == hashA && e.hashB == hashB) {
            return index;
        }
        index = (index + 1) & (capacity() - 1);
    } while (index != startIndex);

    return std::nullopt;
}

std::optional<u32> HashTable::lookup(const std::string& filename, u16 locale) const {
    if (m_entries.empty()) return std::nullopt;

    u32 hashA = hashString(filename, HashType::NameA);
    u32 hashB = hashString(filename, HashType::NameB);
    u32 startIndex = hashString(filename, HashType::TableOffset) & (capacity() - 1);

    u32 index = startIndex;
    do {
        const auto& e = m_entries[index];
        if (e.isEmpty()) return std::nullopt;
        if (e.isOccupied() && e.hashA == hashA && e.hashB == hashB && e.locale == locale) {
            return index;
        }
        index = (index + 1) & (capacity() - 1);
    } while (index != startIndex);

    return std::nullopt;
}

// ============================================================================
// Insert
// ============================================================================

std::optional<u32> HashTable::insert(const std::string& filename, u16 locale, u32 blockIndex) {
    if (m_entries.empty()) return std::nullopt;

    u32 hashA = hashString(filename, HashType::NameA);
    u32 hashB = hashString(filename, HashType::NameB);
    u32 startIndex = hashString(filename, HashType::TableOffset) & (capacity() - 1);

    u32 index = startIndex;
    do {
        auto& e = m_entries[index];
        if (!e.isOccupied()) {
            // Found a free or deleted slot.
            e.hashA = hashA;
            e.hashB = hashB;
            e.locale = locale;
            e.platform = 0;
            e.blockIndex = blockIndex;
            return index;
        }
        index = (index + 1) & (capacity() - 1);
    } while (index != startIndex);

    return std::nullopt; // Table is full.
}

// ============================================================================
// Remove
// ============================================================================

bool HashTable::remove(const std::string& filename) {
    auto idx = lookup(filename);
    if (!idx) return false;
    m_entries[*idx].blockIndex = kHashEntryDeleted;
    return true;
}

bool HashTable::remove(const std::string& filename, u16 locale) {
    auto idx = lookup(filename, locale);
    if (!idx) return false;
    m_entries[*idx].blockIndex = kHashEntryDeleted;
    return true;
}

// ============================================================================
// Serialize
// ============================================================================

std::vector<u8> HashTable::serialize() const {
    const size_t byteSize = m_entries.size() * sizeof(HashEntry);
    std::vector<u32> raw(m_entries.size() * 4);

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& e = m_entries[i];
        raw[i * 4 + 0] = e.hashA;
        raw[i * 4 + 1] = e.hashB;
        raw[i * 4 + 2] = static_cast<u32>(e.locale) | (static_cast<u32>(e.platform) << 16);
        raw[i * 4 + 3] = e.blockIndex;
    }

    encryptBlock(raw.data(), raw.size(), hashString("(hash table)", HashType::FileKey));

    std::vector<u8> result(byteSize);
    std::memcpy(result.data(), raw.data(), byteSize);
    return result;
}

// ============================================================================
// Utilities
// ============================================================================

u32 HashTable::occupiedCount() const {
    u32 count = 0;
    for (const auto& e : m_entries) {
        if (e.isOccupied()) ++count;
    }
    return count;
}

} // namespace whiteout::storages::mpq

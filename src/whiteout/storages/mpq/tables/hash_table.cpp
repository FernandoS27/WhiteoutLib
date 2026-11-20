// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "../crypto.h"
#include "hash_table.h"

#include <algorithm>
#include <cstring>

namespace whiteout::storages::mpq {

// ============================================================================
// Parsing
// ============================================================================

bool HashTable::parse(std::span<const u8> data, u32 capacity) {
    const size_t expectedSize = static_cast<size_t>(capacity) * sizeof(HashEntry);
    if (data.size() < expectedSize)
        return false;

    // Copy the raw data and decrypt in place.
    std::vector<u32> raw(size_t{capacity} * 4);
    std::memcpy(raw.data(), data.data(), expectedSize);
    decryptBlock(raw.data(), raw.size(), hashString("(hash table)", HashType::FileKey));

    // Convert u32 triples to HashEntry structs.
    m_entries.resize(capacity);
    for (u32 i = 0; i < capacity; ++i) {
        const u32* src = raw.data() + size_t{i} * 4;
        m_entries[i].hashA = src[0];
        m_entries[i].hashB = src[1];
        m_entries[i].locale = static_cast<u16>(src[2] & 0xFFFF);
        m_entries[i].platform = static_cast<u16>((src[2] >> 16) & 0xFFFF);
        m_entries[i].blockIndex = src[3] >= kHashEntryDeleted ? src[3] : src[3] & kBlockIndexMask;
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

namespace {

/// The spellings a table may hold @p filename under, in the order they are
/// tried: its slashes hashed as slashes, as Storm and StormLib hash them,
/// then as backslashes. A name with no slash has the one, hashed once.
template <class Probe>
auto eachSpelling(std::string_view filename, Probe probe) -> decltype(probe(NameHash{}, false)) {
    NameHash const folded = hashName(filename);
    if (folded.hasSlash)
        if (auto hit = probe(hashName(filename, true), true))
            return hit;
    return probe(folded, false);
}

/// Shared probe logic for both locale-filtered and any-locale lookups.
std::optional<u32> probeFor(const std::vector<HashEntry>& entries, const NameHash& name, std::optional<u16> locale) {
    u32 const mask = static_cast<u32>(entries.size()) - 1;
    u32 const start = name.offset & mask;
    u32 index = start;
    do {
        const auto& e = entries[index];
        if (e.isEmpty())
            return std::nullopt;
        if (e.isOccupied() && e.hashA == name.a && e.hashB == name.b) {
            if (!locale || e.locale == *locale)
                return index;
        }
        index = (index + 1) & mask;
    } while (index != start);

    return std::nullopt;
}

} // anonymous namespace

std::optional<u32> HashTable::lookup(std::string_view filename) const {
    if (m_entries.empty())
        return std::nullopt;
    return eachSpelling(filename, [&](const NameHash& name, bool) { return probeFor(m_entries, name, std::nullopt); });
}

std::optional<u32> HashTable::lookup(std::string_view filename, u16 locale) const {
    if (m_entries.empty())
        return std::nullopt;
    return eachSpelling(filename, [&](const NameHash& name, bool) { return probeFor(m_entries, name, locale); });
}

std::optional<u32> HashTable::find(std::string_view filename, u16 locale, u32 blockCount) const {
    return eachSpelling(filename, [&](const NameHash& name, bool) { return findHashed(name, locale, blockCount); });
}

std::optional<u32> HashTable::findSpelled(std::string_view filename, bool keepSlashes, u16 locale,
                                          u32 blockCount) const {
    return findHashed(hashName(filename, keepSlashes), locale, blockCount);
}

std::optional<u32> HashTable::findHashed(const NameHash& name, u16 locale, u32 blockCount) const {
    if (m_entries.empty())
        return std::nullopt;

    u32 const mask = capacity() - 1;
    u32 const start = name.offset & mask;

    // Protected maps plant decoy neutral entries ahead of the real one:
    // the last neutral entry is the one the game reads.
    std::optional<u32> neutral;
    u32 index = start;
    do {
        const auto& e = m_entries[index];
        if (e.isEmpty())
            break;
        bool const ours = e.hashA == name.a && e.hashB == name.b && e.blockIndex < blockCount &&
                          (e.platform & 0xFF) == 0;
        if (ours && locale != 0 && e.locale == locale)
            return index;
        if (ours && e.locale == 0)
            neutral = index;
        index = (index + 1) & mask;
    } while (index != start);
    return neutral;
}

std::vector<u32> HashTable::slotsOf(std::string_view filename, bool* keepSlashes) const {
    std::vector<u32> slots;
    if (m_entries.empty())
        return slots;
    eachSpelling(filename, [&](const NameHash& name, bool spelled) {
        eachSlot(name, [&](u32 slot) { slots.push_back(slot); });
        if (keepSlashes)
            *keepSlashes = spelled;
        return !slots.empty();
    });
    return slots;
}

// ============================================================================
// Insert
// ============================================================================

std::optional<u32> HashTable::insert(std::string_view filename, u16 locale, u32 blockIndex, u16 platform,
                                     bool keepSlashes) {
    if (m_entries.empty())
        return std::nullopt;

    NameHash const name = hashName(filename, keepSlashes);
    u32 const start = name.offset & (capacity() - 1);
    u32 index = start;
    do {
        auto& e = m_entries[index];
        if (!e.isOccupied()) {
            // Found a free or deleted slot.
            e.hashA = name.a;
            e.hashB = name.b;
            e.locale = locale;
            e.platform = platform;
            e.blockIndex = blockIndex;
            return index;
        }
        index = (index + 1) & (capacity() - 1);
    } while (index != start);

    return std::nullopt; // Table is full.
}

// ============================================================================
// Remove
// ============================================================================

bool HashTable::remove(std::string_view filename) {
    auto idx = lookup(filename);
    if (!idx)
        return false;
    m_entries[*idx].blockIndex = kHashEntryDeleted;
    return true;
}

bool HashTable::remove(std::string_view filename, u16 locale) {
    auto idx = lookup(filename, locale);
    if (!idx)
        return false;
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
        if (e.isOccupied())
            ++count;
    }
    return count;
}

} // namespace whiteout::storages::mpq

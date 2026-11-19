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

/// What a name probes with: its two check hashes and its first slot.
struct NameHashes {
    u32 hashA;
    u32 hashB;
    u32 start;
};

NameHashes hashesOf(const std::string& filename, bool keepSlashes, u32 capacity) {
    return {hashString(filename, HashType::NameA, keepSlashes), hashString(filename, HashType::NameB, keepSlashes),
            hashString(filename, HashType::TableOffset, keepSlashes) & (capacity - 1)};
}

/// The spellings a table may hold @p filename under, in the order they are
/// tried: its slashes hashed as slashes, as Storm and StormLib hash them,
/// then as backslashes. A name with no slash has the one.
template <class Probe>
auto eachSpelling(const std::string& filename, Probe probe) -> decltype(probe(false)) {
    if (filename.find('/') != std::string::npos)
        if (auto hit = probe(true))
            return hit;
    return probe(false);
}

/// Shared probe logic for both locale-filtered and any-locale lookups.
std::optional<u32> probeFor(const std::vector<HashEntry>& entries, const NameHashes& name,
                            std::optional<u16> locale) {
    u32 const cap = static_cast<u32>(entries.size());
    u32 index = name.start;
    do {
        const auto& e = entries[index];
        if (e.isEmpty())
            return std::nullopt;
        if (e.isOccupied() && e.hashA == name.hashA && e.hashB == name.hashB) {
            if (!locale || e.locale == *locale)
                return index;
        }
        index = (index + 1) & (cap - 1);
    } while (index != name.start);

    return std::nullopt;
}

} // anonymous namespace

std::optional<u32> HashTable::lookup(const std::string& filename) const {
    if (m_entries.empty())
        return std::nullopt;
    return eachSpelling(filename, [&](bool keepSlashes) {
        return probeFor(m_entries, hashesOf(filename, keepSlashes, capacity()), std::nullopt);
    });
}

std::optional<u32> HashTable::lookup(const std::string& filename, u16 locale) const {
    if (m_entries.empty())
        return std::nullopt;
    return eachSpelling(filename, [&](bool keepSlashes) {
        return probeFor(m_entries, hashesOf(filename, keepSlashes, capacity()), locale);
    });
}

std::optional<u32> HashTable::find(const std::string& filename, u16 locale, u32 blockCount) const {
    return eachSpelling(filename,
                        [&](bool keepSlashes) { return findSpelled(filename, keepSlashes, locale, blockCount); });
}

std::optional<u32> HashTable::findSpelled(const std::string& filename, bool keepSlashes, u16 locale,
                                          u32 blockCount) const {
    if (m_entries.empty())
        return std::nullopt;

    u32 const mask = capacity() - 1;
    NameHashes const name = hashesOf(filename, keepSlashes, capacity());

    // Protected maps plant decoy neutral entries ahead of the real one:
    // the last neutral entry is the one the game reads.
    std::optional<u32> neutral;
    u32 index = name.start;
    do {
        const auto& e = m_entries[index];
        if (e.isEmpty())
            break;
        bool const ours = e.hashA == name.hashA && e.hashB == name.hashB && e.blockIndex < blockCount &&
                          (e.platform & 0xFF) == 0;
        if (ours && locale != 0 && e.locale == locale)
            return index;
        if (ours && e.locale == 0)
            neutral = index;
        index = (index + 1) & mask;
    } while (index != name.start);
    return neutral;
}

std::vector<u32> HashTable::slotsOf(const std::string& filename, bool* keepSlashes) const {
    std::vector<u32> slots;
    if (m_entries.empty())
        return slots;
    eachSpelling(filename, [&](bool spelled) {
        NameHashes const name = hashesOf(filename, spelled, capacity());
        u32 index = name.start;
        do {
            const auto& e = m_entries[index];
            if (e.isEmpty())
                break;
            if (e.isOccupied() && e.hashA == name.hashA && e.hashB == name.hashB)
                slots.push_back(index);
            index = (index + 1) & (capacity() - 1);
        } while (index != name.start);
        if (keepSlashes)
            *keepSlashes = spelled;
        return !slots.empty();
    });
    return slots;
}

// ============================================================================
// Insert
// ============================================================================

std::optional<u32> HashTable::insert(const std::string& filename, u16 locale, u32 blockIndex, u16 platform,
                                     bool keepSlashes) {
    if (m_entries.empty())
        return std::nullopt;

    NameHashes const name = hashesOf(filename, keepSlashes, capacity());
    u32 index = name.start;
    do {
        auto& e = m_entries[index];
        if (!e.isOccupied()) {
            // Found a free or deleted slot.
            e.hashA = name.hashA;
            e.hashB = name.hashB;
            e.locale = locale;
            e.platform = platform;
            e.blockIndex = blockIndex;
            return index;
        }
        index = (index + 1) & (capacity() - 1);
    } while (index != name.start);

    return std::nullopt; // Table is full.
}

// ============================================================================
// Remove
// ============================================================================

bool HashTable::remove(const std::string& filename) {
    auto idx = lookup(filename);
    if (!idx)
        return false;
    m_entries[*idx].blockIndex = kHashEntryDeleted;
    return true;
}

bool HashTable::remove(const std::string& filename, u16 locale) {
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

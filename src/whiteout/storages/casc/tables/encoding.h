// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file encoding.h
/// @brief CASC ENCODING manifest parser — maps CKey → EKey.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>

#include <array>
#include <span>
#include <string>
#include <vector>

#include "flat_hash_map.h"

namespace whiteout::storages::casc {

struct EncodingEntry {
    std::array<u8, 16> cKey{};
    std::array<u8, 16> eKey{};  ///< Primary EKey (first encoding key).
    u64 fileSize = 0;           ///< Uncompressed file size.
    std::string eSpec;          ///< BLTE encoding specification (e.g. "z", "n", "b:{65536*=z}").
};

class EncodingTable {
public:
    /// Parse an ENCODING manifest (already BLTE-decoded).
    /// @param pool Optional worker pool for parallel index building.
    static EncodingTable parse(std::span<const u8> data,
                               interfaces::WorkerPool* pool = nullptr);

    /// Find by CKey. matchBytes=0 means compare all 16 bytes.
    const EncodingEntry* findByCKey(std::span<const u8, 16> cKey, size_t matchBytes = 0) const;
    /// Find by EKey. matchBytes=0 means compare all 16 bytes.
    const EncodingEntry* findByEKey(std::span<const u8, 16> eKey, size_t matchBytes = 0) const;

    /// For write path.
    void insert(const EncodingEntry& entry);

    std::vector<u8> serialize() const;

    size_t entryCount() const { return m_entries.size(); }

    /// Read-only access to all entries (for orphan enumeration).
    const std::vector<EncodingEntry>& entries() const { return m_entries; }

private:
    std::vector<EncodingEntry> m_entries;

    /// CKey hash index: first 8 bytes of CKey → index into m_entries.
    FlatHashMap<size_t> m_cKeyIndex;

    /// EKey hash index: first 8 bytes of EKey → index into m_entries.
    FlatHashMap<size_t> m_eKeyIndex;
};

} // namespace whiteout::storages::casc

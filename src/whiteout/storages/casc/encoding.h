// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file encoding.h
/// @brief CASC ENCODING manifest parser — maps CKey → EKey.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::storages::casc {

struct EncodingEntry {
    std::array<u8, 16> cKey{};
    std::array<u8, 16> eKey{};  ///< Primary EKey (first encoding key).
    u64 fileSize = 0;           ///< Uncompressed file size.
};

class EncodingTable {
public:
    /// Parse an ENCODING manifest (already BLTE-decoded).
    static EncodingTable parse(std::span<const u8> data);

    const EncodingEntry* findByCKey(std::span<const u8, 16> cKey) const;
    /// Find by EKey. matchBytes=0 means compare all 16 bytes.
    const EncodingEntry* findByEKey(std::span<const u8, 16> eKey, size_t matchBytes = 0) const;

    /// For write path.
    void insert(const EncodingEntry& entry);

    std::vector<u8> serialize() const;

    size_t entryCount() const { return m_entries.size(); }

private:
    /// Sorted by CKey for binary search.
    mutable std::vector<EncodingEntry> m_entries;
    mutable bool m_sorted = false;

    /// Secondary index: first 8 bytes of EKey → index into m_entries.
    mutable std::unordered_map<u64, size_t> m_eKeyIndex;
    mutable bool m_eKeyIndexBuilt = false;

    void ensureSorted() const;
    void ensureEKeyIndex() const;
};

} // namespace whiteout::storages::casc

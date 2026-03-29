// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "block_table.h"
#include "crypto.h"

#include <cstring>

namespace whiteout::storages::mpq {

// ============================================================================
// Parsing
// ============================================================================

bool BlockTable::parse(std::span<const u8> data, u32 count) {
    const size_t expectedSize = static_cast<size_t>(count) * sizeof(BlockEntry);
    if (data.size() < expectedSize) return false;

    // Copy and decrypt in place.
    std::vector<u32> raw(count * 4);
    std::memcpy(raw.data(), data.data(), expectedSize);
    decryptBlock(raw.data(), raw.size(), hashString("(block table)", HashType::FileKey));

    m_entries.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const u32* src = raw.data() + i * 4;
        m_entries[i].fileOffset = src[0];
        m_entries[i].compressedSize = src[1];
        m_entries[i].uncompressedSize = src[2];
        m_entries[i].flags = src[3];
    }

    m_hiBlockOffsets.clear();
    return true;
}

bool BlockTable::parseHiBlockTable(std::span<const u8> data, u32 count) {
    const size_t expectedSize = static_cast<size_t>(count) * sizeof(u16);
    if (data.size() < expectedSize) return false;

    m_hiBlockOffsets.resize(count);
    std::memcpy(m_hiBlockOffsets.data(), data.data(), expectedSize);
    return true;
}

// ============================================================================
// Creation
// ============================================================================

void BlockTable::createEmpty() {
    m_entries.clear();
    m_hiBlockOffsets.clear();
}

// ============================================================================
// Append
// ============================================================================

u32 BlockTable::append(const BlockEntry& entry) {
    u32 index = static_cast<u32>(m_entries.size());
    m_entries.push_back(entry);
    return index;
}

// ============================================================================
// 48-bit offset
// ============================================================================

u64 BlockTable::fileOffset48(u32 index) const {
    u64 lo = m_entries[index].fileOffset;
    if (index < m_hiBlockOffsets.size()) {
        u64 hi = m_hiBlockOffsets[index];
        return lo | (hi << 32);
    }
    return lo;
}

// ============================================================================
// Serialize
// ============================================================================

std::vector<u8> BlockTable::serialize() const {
    const size_t byteSize = m_entries.size() * sizeof(BlockEntry);
    std::vector<u32> raw(m_entries.size() * 4);

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& e = m_entries[i];
        raw[i * 4 + 0] = e.fileOffset;
        raw[i * 4 + 1] = e.compressedSize;
        raw[i * 4 + 2] = e.uncompressedSize;
        raw[i * 4 + 3] = e.flags;
    }

    encryptBlock(raw.data(), raw.size(), hashString("(block table)", HashType::FileKey));

    std::vector<u8> result(byteSize);
    std::memcpy(result.data(), raw.data(), byteSize);
    return result;
}

std::vector<u8> BlockTable::serializeHiBlockTable() const {
    std::vector<u8> result(m_entries.size() * sizeof(u16));
    for (size_t i = 0; i < m_entries.size(); ++i) {
        u16 hi = 0;
        if (i < m_hiBlockOffsets.size()) {
            hi = m_hiBlockOffsets[i];
        }
        std::memcpy(result.data() + i * sizeof(u16), &hi, sizeof(u16));
    }
    return result;
}

bool BlockTable::needsHiBlockTable() const {
    for (const auto& hi : m_hiBlockOffsets) {
        if (hi != 0) return true;
    }
    return false;
}

} // namespace whiteout::storages::mpq

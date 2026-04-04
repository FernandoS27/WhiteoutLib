// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "encoding.h"
#include "../storage/constants.h"
#include "../storage/key_utils.h"
#include "../../common/byte_order.h"
#include "../../common/md5.h"

#include <whiteout/utils/job_group.h>

#include <algorithm>
#include <cstring>

namespace whiteout::storages::casc {

using storages::common::readBE16;
using storages::common::readBE32;
using storages::common::readBE40;
using storages::common::writeBE16;
using storages::common::writeBE32;
using storages::common::writeBE40;

// ---- Constants local to Encoding codec ----

/// Encoding file magic bytes: 'E', 'N'.
static constexpr u8 kEncodingMagic0 = 'E';
static constexpr u8 kEncodingMagic1 = 'N';

/// Expected encoding file version.
static constexpr u8 kEncodingVersion = 1;

/// Minimum encoding file size (header fields).
static constexpr size_t kEncodingMinHeaderSize = 22;

/// Default page size for encoding table serialization (4 KB).
static constexpr u16 kEncodingPageSizeKB = 4;
static constexpr size_t kEncodingPageSize = size_t(kEncodingPageSizeKB) * 1024;

// ============================================================================
// Helpers
// ============================================================================

static int cmpKey16(const std::array<u8, 16>& a, const std::array<u8, 16>& b) {
    return std::memcmp(a.data(), b.data(), 16);
}

// ============================================================================
// EncodingTable::parse
// ============================================================================

EncodingTable EncodingTable::parse(std::span<const u8> data,
                                   interfaces::WorkerPool* pool) {
    EncodingTable table;

    // Minimum header: magic(2) + version + sizes + counts.
    if (data.size() < kEncodingMinHeaderSize)
        return table;

    // Validate magic.
    if (data[0] != kEncodingMagic0 || data[1] != kEncodingMagic1)
        return table;

    u8 version = data[2];
    if (version != kEncodingVersion)
        return table;

    u8 cKeySize = data[3]; // Typically 16.
    u8 eKeySize = data[4]; // Typically 16.
    u16 cKeyPageSizeKB = readBE16(data.data() + 5);
    u16 eKeyPageSizeKB = readBE16(data.data() + 7);
    u32 cKeyPageCount = readBE32(data.data() + 9);
    u32 eKeyPageCount = readBE32(data.data() + 13);
    // u8 padding at offset 17.
    u32 eSpecBlockSize = readBE32(data.data() + 18);

    size_t cKeyPageSize = size_t(cKeyPageSizeKB) * 1024;

    size_t offset = kEncodingMinHeaderSize;

    // Skip ESpec string block.
    offset += eSpecBlockSize;

    if (offset > data.size())
        return table;

    // CKey page table: cKeyPageCount entries, each = firstCKey(cKeySize) + pageMD5(16).
    size_t cKeyPageTableEntrySize = cKeySize + 16;
    size_t cKeyPageTableSize = cKeyPageCount * cKeyPageTableEntrySize;
    size_t cKeyPageTableStart = offset;
    offset += cKeyPageTableSize;

    if (offset > data.size())
        return table;

    // Pre-reserve based on estimated entry count (each entry ~38 bytes).
    size_t estimatedEntries = cKeyPageCount * (cKeyPageSize / 38);
    table.m_entries.reserve(estimatedEntries);

    // --- Phase 1: Parse all entries from pages (no hash map inserts) ---
    for (u32 page = 0; page < cKeyPageCount; ++page) {
        size_t pageStart = offset + page * cKeyPageSize;
        if (pageStart + 2 > data.size())
            break;

        // Read entries from the page.
        size_t pos = pageStart;
        size_t pageEnd = std::min(pageStart + cKeyPageSize, data.size());

        while (pos + 1 + 5 + cKeySize + eKeySize <= pageEnd) {
            // keyCount: u8 — number of EKeys for this CKey.
            u8 keyCount = data[pos];
            if (keyCount == 0)
                break; // End of entries in this page (zero-padded).

            // File size: u40 BE (5 bytes).
            u64 fileSize = readBE40(data.data() + pos + 1);

            // CKey.
            size_t cKeyOff = pos + 6;
            if (cKeyOff + cKeySize > pageEnd) break;

            EncodingEntry entry;
            std::memcpy(entry.cKey.data(), data.data() + cKeyOff, cKeySize);
            entry.fileSize = fileSize;

            // EKey(s): keyCount keys, each eKeySize bytes.
            size_t eKeyOff = cKeyOff + cKeySize;
            if (eKeyOff + eKeySize > pageEnd) break;

            // Take only the first EKey.
            std::memcpy(entry.eKey.data(), data.data() + eKeyOff, eKeySize);

            table.m_entries.push_back(entry);

            // Advance past all EKeys.
            pos = eKeyOff + size_t(keyCount) * eKeySize;
        }
    }

    // --- Phase 2: Build hash indices ---
    size_t n = table.m_entries.size();
    table.m_cKeyIndex.reserve(n);
    table.m_eKeyIndex.reserve(n);

    if (pool) {
        // Build CKey and EKey indices in parallel.
        utils::JobGroup jobGroup;
        jobGroup.add(2);
        interfaces::WorkerTask ckeyTask;
        ckeyTask.fn = [&]() {
            for (size_t i = 0; i < n; ++i)
                table.m_cKeyIndex.emplace(keyHash64(table.m_entries[i].cKey), i);
            jobGroup.done();
        };
        interfaces::WorkerTask ekeyTask;
        ekeyTask.fn = [&]() {
            for (size_t i = 0; i < n; ++i)
                table.m_eKeyIndex.emplace(keyHash64(table.m_entries[i].eKey), i);
            jobGroup.done();
        };
        pool->submit(ckeyTask);
        pool->submit(ekeyTask);
        jobGroup.wait();
    } else {
        for (size_t i = 0; i < n; ++i) {
            table.m_cKeyIndex.emplace(keyHash64(table.m_entries[i].cKey), i);
            table.m_eKeyIndex.emplace(keyHash64(table.m_entries[i].eKey), i);
        }
    }

    return table;
}

// ============================================================================
// findByCKey
// ============================================================================

const EncodingEntry* EncodingTable::findByCKey(std::span<const u8, 16> cKey,
                                                size_t matchBytes) const {
    if (matchBytes == 0) matchBytes = 16;
    if (matchBytes > 16) matchBytes = 16;

    auto* idxPtr = m_cKeyIndex.find(keyHash64(cKey.data()));
    if (!idxPtr)
        return nullptr;

    auto& e = m_entries[*idxPtr];
    if (std::memcmp(e.cKey.data(), cKey.data(), matchBytes) == 0)
        return &e;

    return nullptr;
}

// ============================================================================
// findByEKey
// ============================================================================

const EncodingEntry* EncodingTable::findByEKey(std::span<const u8, 16> eKey,
                                                size_t matchBytes) const {
    if (matchBytes == 0) matchBytes = 16;
    if (matchBytes > 16) matchBytes = 16;

    // Look up by first-8-byte hash.
    auto* idxPtr = m_eKeyIndex.find(keyHash64(eKey.data()));
    if (!idxPtr)
        return nullptr;

    auto& e = m_entries[*idxPtr];
    // Verify full prefix match (handles collisions and matchBytes > 8).
    if (std::memcmp(e.eKey.data(), eKey.data(), matchBytes) == 0)
        return &e;

    return nullptr;
}

// ============================================================================
// insert
// ============================================================================

void EncodingTable::insert(const EncodingEntry& entry) {
    size_t idx = m_entries.size();
    m_entries.push_back(entry);

    m_cKeyIndex.emplace(keyHash64(entry.cKey), idx);
    m_eKeyIndex.emplace(keyHash64(entry.eKey), idx);
}

// ============================================================================
// serialize
// ============================================================================

std::vector<u8> EncodingTable::serialize() const {
    // Create a sorted copy for serialization (entries must be CKey-ordered).
    auto sorted = m_entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const EncodingEntry& a, const EncodingEntry& b) {
                  return cmpKey16(a.cKey, b.cKey) < 0;
              });

    constexpr u16 kPageSizeKB = kEncodingPageSizeKB;
    constexpr size_t kPageSize = kEncodingPageSize;

    // Build CKey pages.
    struct PageData {
        std::vector<u8> data;
        std::array<u8, 16> firstCKey{};
    };
    std::vector<PageData> pages;
    {
        PageData current;
        for (auto& entry : sorted) {
            // Entry: keyCount(1) + fileSize(5) + CKey(16) + EKey(16) = 38 bytes.
            constexpr size_t kEntrySize = 1 + 5 + kCKeySize + kEKeySize;

            if (current.data.size() + kEntrySize > kPageSize) {
                // Pad page to kPageSize.
                current.data.resize(kPageSize, 0);
                pages.push_back(std::move(current));
                current = PageData{};
            }

            if (current.data.empty())
                current.firstCKey = entry.cKey;

            // keyCount = 1.
            current.data.push_back(1);
            // fileSize (40-bit BE).
            size_t off = current.data.size();
            current.data.resize(off + 5);
            writeBE40(current.data.data() + off, entry.fileSize);
            // CKey.
            current.data.insert(current.data.end(), entry.cKey.begin(), entry.cKey.end());
            // EKey.
            current.data.insert(current.data.end(), entry.eKey.begin(), entry.eKey.end());
        }

        if (!current.data.empty()) {
            current.data.resize(kPageSize, 0); // Pad last page.
            pages.push_back(std::move(current));
        }
    }

    u32 cKeyPageCount = u32(pages.size());
    u32 eKeyPageCount = 0; // We don't write EKey pages for now.
    u32 eSpecBlockSize = 0;

    // Calculate total size.
    size_t headerSize = kEncodingMinHeaderSize;
    size_t cKeyPageTableSize = cKeyPageCount * (kCKeySize + 16);
    size_t eKeyPageTableSize = 0;
    size_t cKeyPagesSize = cKeyPageCount * kPageSize;

    size_t totalSize = headerSize + eSpecBlockSize + cKeyPageTableSize +
                       eKeyPageTableSize + cKeyPagesSize;

    std::vector<u8> output(totalSize, 0);
    size_t pos = 0;

    // Header.
    output[pos++] = kEncodingMagic0;
    output[pos++] = kEncodingMagic1;
    output[pos++] = kEncodingVersion;
    output[pos++] = kCKeySize;
    output[pos++] = kEKeySize;
    writeBE16(output.data() + pos, kPageSizeKB); pos += 2;
    writeBE16(output.data() + pos, kPageSizeKB); pos += 2;
    writeBE32(output.data() + pos, cKeyPageCount); pos += 4;
    writeBE32(output.data() + pos, eKeyPageCount); pos += 4;
    output[pos++] = 0; // padding
    writeBE32(output.data() + pos, eSpecBlockSize); pos += 4;

    // ESpec block (empty).

    // CKey page table: firstCKey + MD5 of page.
    for (auto& page : pages) {
        std::memcpy(output.data() + pos, page.firstCKey.data(), kCKeySize);
        pos += kCKeySize;
        auto hash = common::md5Hash(std::span(page.data.data(), page.data.size()));
        std::memcpy(output.data() + pos, hash.data(), 16);
        pos += 16;
    }

    // CKey pages.
    for (auto& page : pages) {
        std::memcpy(output.data() + pos, page.data.data(), page.data.size());
        pos += kPageSize;
    }

    return output;
}

} // namespace whiteout::storages::casc

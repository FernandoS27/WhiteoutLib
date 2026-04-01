// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "wow_root.h"

#include "../../common/byte_order.h"
#include "../../common/jenkins.h"

#include <algorithm>
#include <cstring>

namespace whiteout::storages::casc {

using storages::common::readLE32;
using storages::common::readLE64;
using storages::common::readLEi32;

// ---- Constants local to WoW root parser ----

/// WoW root block header sizes.
static constexpr size_t kWowBlockHeaderSizeStandard = 12;
static constexpr size_t kWowBlockHeaderSizeV3 = 17;

/// WoW old (interleaved) record size: CKey(16) + NameHash(8) = 24 bytes per entry.
static constexpr size_t kWowOldRecordSize = 24;

namespace {

// ============================================================================
// Header version detection
// ============================================================================

struct WowRootHeader {
    u32 totalFileCount = 0;
    u32 namedFileCount = 0;
    u32 headerVersion = 0;   ///< 0 = legacy headerless, 1 = v2 (30080), 2+ = v3 (50893)
    size_t dataOffset = 0;   ///< Where block data begins.
};

/// Try v3 header (build 50893+): MFST + headerSize + version + totalFiles + namedFiles + padding.
static bool tryParseHeaderV3(std::span<const u8> data, WowRootHeader& out) {
    if (data.size() < 24) return false;
    u32 magic = readLE32(data.data());
    if (magic != RootSignature::kMFST) return false;

    u32 headerSize = readLE32(data.data() + 4);
    u32 version    = readLE32(data.data() + 8);
    // V3 header: version must be 1 or 2, headerSize must be >= 16 (version+total+named+padding).
    if (version < 1 || version > 2) return false;
    if (headerSize < 16) return false;

    out.totalFileCount = readLE32(data.data() + 12);
    out.namedFileCount = readLE32(data.data() + 16);
    out.headerVersion = version + 1; // internal: 2 = v2-in-v3, 3 = v3 manifest version 2
    out.dataOffset = headerSize + 4; // headerSize is relative to after the magic
    if (out.dataOffset > data.size()) return false;

    return true;
}

/// Try v2 header (build 30080+): MFST + totalFiles + namedFiles.
static bool tryParseHeaderV2(std::span<const u8> data, WowRootHeader& out) {
    if (data.size() < 12) return false;
    u32 magic = readLE32(data.data());
    if (magic != RootSignature::kMFST) return false;

    out.totalFileCount = readLE32(data.data() + 4);
    out.namedFileCount = readLE32(data.data() + 8);
    out.headerVersion = 1;
    out.dataOffset = 12;
    return true;
}

/// Headerless format (build 18125+): data starts at offset 0.
static bool tryParseHeaderLegacy(std::span<const u8> data, WowRootHeader& out) {
    // Sanity: first 12 bytes should be a valid group header.
    // numRecords(u32) + contentFlags(u32) + localeFlags(u32).
    if (data.size() < 12) return false;

    u32 numRecords = readLE32(data.data());
    // Reasonable bounds check: numRecords should be > 0 and not absurdly large.
    if (numRecords == 0 || numRecords > 10000000) return false;

    out.totalFileCount = 0; // not available in legacy
    out.namedFileCount = 0;
    out.headerVersion = 0;
    out.dataOffset = 0;
    return true;
}

// ============================================================================
// Block parsing
// ============================================================================

struct BlockHeader {
    u32 numRecords = 0;
    u32 contentFlags = 0;
    u32 localeFlags = 0;
    size_t headerSize = 0; ///< Size of this block header in bytes.
};

/// Parse one block header at the given offset.
/// Returns false if there isn't enough data.
static bool parseBlockHeader(std::span<const u8> data, size_t offset,
                             u32 manifestVersion, BlockHeader& out) {
    // Manifest version 2 (in v3 container) has extended header:
    //   numRecords(4) + localeFlags(4) + unk1(4) + unk2(4) + unk3(1) = 17 bytes
    // All other versions:
    //   numRecords(4) + contentFlags(4) + localeFlags(4) = 12 bytes
    if (manifestVersion >= 3) {
        // v3 manifest version 2: numRecords, localeFlags, unk1, unk2, unk3
        if (offset + kWowBlockHeaderSizeV3 > data.size()) return false;
        const u8* p = data.data() + offset;
        out.numRecords = readLE32(p);
        out.localeFlags = readLE32(p + 4);
        u32 unk1 = readLE32(p + 8);
        u32 unk2 = readLE32(p + 12);
        u8 unk3 = p[16];
        // Reconstruct old-style contentFlags: unk1 | unk2 | (unk3 << 17)
        out.contentFlags = unk1 | unk2 | (u32(unk3) << 17);
        out.headerSize = kWowBlockHeaderSizeV3;
    } else {
        if (offset + kWowBlockHeaderSizeStandard > data.size()) return false;
        const u8* p = data.data() + offset;
        out.numRecords = readLE32(p);
        out.contentFlags = readLE32(p + 4);
        out.localeFlags = readLE32(p + 8);
        out.headerSize = kWowBlockHeaderSizeStandard;
    }
    return true;
}

/// Parse blocks from the root data.
static bool parseBlocks(std::span<const u8> data, const WowRootHeader& header,
                        std::vector<RootEntry>& outEntries) {
    size_t offset = header.dataOffset;
    bool useOldRecordFormat = (header.headerVersion == 0);
    bool allowNonNamedFiles =
        (header.totalFileCount != header.namedFileCount) ||
        (header.headerVersion == 0);

    while (offset < data.size()) {
        BlockHeader bh;
        if (!parseBlockHeader(data, offset, header.headerVersion, bh))
            break;

        offset += bh.headerSize;
        if (bh.numRecords == 0)
            break;

        // Delta-encoded FileDataIds (i32 deltas).
        size_t deltaSize = size_t(bh.numRecords) * 4;
        if (offset + deltaSize > data.size()) return false;

        // Decode FileDataIds from deltas.
        std::vector<u32> fileDataIds(bh.numRecords);
        {
            i32 fileDataId = 0;
            const u8* dp = data.data() + offset;
            for (u32 i = 0; i < bh.numRecords; ++i) {
                i32 delta = readLEi32(dp + size_t(i) * 4);
                fileDataId += delta;
                fileDataIds[i] = u32(fileDataId);
                fileDataId++; // implicit +1 per entry
            }
        }
        offset += deltaSize;

        // Record data: CKeys and optional NameHashes.
        if (useOldRecordFormat) {
            // Old interleaved format: [CKey(16) + NameHash(8)] per entry.
            size_t recordSize = size_t(bh.numRecords) * kWowOldRecordSize;
            if (offset + recordSize > data.size()) return false;

            for (u32 i = 0; i < bh.numRecords; ++i) {
                RootEntry entry;
                const u8* rp = data.data() + offset + size_t(i) * kWowOldRecordSize;
                std::memcpy(entry.cKey.data(), rp, 16);
                entry.fileNameHash = readLE64(rp + 16);
                entry.fileDataId = fileDataIds[i];
                entry.localeFlags = bh.localeFlags;
                entry.contentFlags = bh.contentFlags;
                outEntries.push_back(std::move(entry));
            }
            offset += recordSize;
        } else {
            // Split format: CKey[numRecords], then optional NameHash[numRecords].
            size_t cKeySize = size_t(bh.numRecords) * 16;
            if (offset + cKeySize > data.size()) return false;

            bool hasNameHash = !(allowNonNamedFiles &&
                                 (bh.contentFlags & ContentFlags::NoNameHash));

            size_t nameHashSize = hasNameHash ? (size_t(bh.numRecords) * 8) : 0;
            if (offset + cKeySize + nameHashSize > data.size()) return false;

            const u8* cKeyBase = data.data() + offset;
            const u8* nameBase = hasNameHash ? (data.data() + offset + cKeySize) : nullptr;

            for (u32 i = 0; i < bh.numRecords; ++i) {
                RootEntry entry;
                std::memcpy(entry.cKey.data(), cKeyBase + size_t(i) * 16, 16);
                if (nameBase)
                    entry.fileNameHash = readLE64(nameBase + size_t(i) * 8);
                entry.fileDataId = fileDataIds[i];
                entry.localeFlags = bh.localeFlags;
                entry.contentFlags = bh.contentFlags;
                outEntries.push_back(std::move(entry));
            }
            offset += cKeySize + nameHashSize;
        }
    }

    return !outEntries.empty();
}

} // anonymous namespace

// ============================================================================
// WowRoot public API
// ============================================================================

std::unique_ptr<WowRoot> WowRoot::parse(std::span<const u8> data,
                                        interfaces::WorkerPool* /*pool*/) {
    if (data.size() < 12) return nullptr;

    WowRootHeader header;
    // Try header versions from newest to oldest.
    if (!tryParseHeaderV3(data, header) &&
        !tryParseHeaderV2(data, header) &&
        !tryParseHeaderLegacy(data, header)) {
        return nullptr;
    }

    auto root = std::make_unique<WowRoot>();
    if (!parseBlocks(data, header, root->m_entries))
        return nullptr;

    root->buildIndices();
    return root;
}

std::vector<RootEntry> WowRoot::findByPath(const std::string& path) const {
    auto hash = common::jenkinsHash(path);
    u64 combined = u64(hash.pc) | (u64(hash.pb) << 32);

    std::vector<RootEntry> results;
    auto range = m_byNameHash.equal_range(combined);
    for (auto it = range.first; it != range.second; ++it)
        results.push_back(m_entries[it->second]);
    return results;
}

std::vector<RootEntry> WowRoot::findByFileDataId(u32 fileDataId) const {
    std::vector<RootEntry> results;
    auto range = m_byFileDataId.equal_range(fileDataId);
    for (auto it = range.first; it != range.second; ++it)
        results.push_back(m_entries[it->second]);
    return results;
}

std::vector<RootEntry> WowRoot::findByCKey(std::span<const u8, 16> cKey) const {
    // WoW root has a special zero-key check before comparison.
    std::vector<RootEntry> results;
    for (auto& e : m_entries) {
        if (e.cKey == std::array<u8, 16>{} ? false :
            std::memcmp(e.cKey.data(), cKey.data(), 16) == 0)
            results.push_back(e);
    }
    return results;
}

void WowRoot::buildIndices() {
    m_byFileDataId.reserve(m_entries.size());
    m_byNameHash.reserve(m_entries.size());

    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto& e = m_entries[i];
        if (e.fileDataId != kInvalidFileDataId)
            m_byFileDataId.emplace(e.fileDataId, i);
        if (e.fileNameHash != 0)
            m_byNameHash.emplace(e.fileNameHash, i);
    }
}

} // namespace whiteout::storages::casc

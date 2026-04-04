// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/sno/core_toc.h>

#include <algorithm>
#include <cstring>
#include <istream>

#include "../common/binary_reader.h"
#include "../common/streams.h"

namespace whiteout {
namespace sno {

bool CoreToc::parse(std::span<const u8> data) {
    m_all.clear();
    m_groupIndex.clear();
    m_idIndex.clear();
    m_formatHashes.clear();
    m_format = CoreTocFormat::Unknown;

    if (data.size() < 8) {
        return false;
    }

    common::span_streambuf sbuf(data);
    std::istream stream(&sbuf);
    common::BinaryReader reader(stream);

    const u32 firstWord = reader.read<u32>();

    if (firstWord == 0xBCDE6611u) {
        // D4 new format.
        m_format = CoreTocFormat::D4New;
        return parseD4(data, true);
    }

    // If the first word is 0 and the file is large enough for D3's 70-group
    // header (1120 bytes), try the D3 legacy format.  In D3, group-0 always
    // has 0 entries so the first u32 is 0.
    constexpr u32 kD3NumGroups = 70;
    constexpr size_t kD3HeaderSize = kD3NumGroups * 4 * 4; // 1120 bytes
    if (firstWord == 0 && data.size() > kD3HeaderSize + 64) {
        m_format = CoreTocFormat::D3Legacy;
        return parseD3Legacy(data);
    }

    // D4 old format: firstWord is snoGroupsCount.
    m_format = CoreTocFormat::D4Old;
    return parseD4(data, false);
}

// ---- D3 legacy format (no magic, 70 fixed groups, 4 header arrays) ---------

bool CoreToc::parseD3Legacy(std::span<const u8> data) {
    constexpr u32 kNumGroups = 70;
    constexpr size_t kHeaderSize = kNumGroups * 4 * 4; // 1120 bytes

    if (data.size() < kHeaderSize) {
        return false;
    }

    common::span_streambuf sbuf(data);
    std::istream stream(&sbuf);
    common::BinaryReader reader(stream);

    // Header layout:
    //   entryCounts[70]    @ 0
    //   sectionOffsets[70] @ 280  (byte offset per group into data section)
    //   hashCounts[70]     @ 560  (unused)
    //   hashData[70]       @ 840  (unused)

    // Read counts sequentially from offset 0.
    std::vector<u32> entryCounts(kNumGroups);
    for (u32 g = 0; g < kNumGroups; ++g) {
        entryCounts[g] = reader.read<u32>();
    }

    // Read offsets sequentially (reader is at offset 280).
    std::vector<u32> sectionOffsets(kNumGroups);
    for (u32 g = 0; g < kNumGroups; ++g) {
        sectionOffsets[g] = reader.read<u32>();
    }

    // Data section starts right after the header.
    const size_t dataStart = kHeaderSize;

    // Build a sorted list of group offsets so we can compute section bounds.
    struct GroupInfo {
        u32 groupId;
        u32 headerCount;
        size_t sectionStart;
    };
    std::vector<GroupInfo> groups;
    groups.reserve(kNumGroups);

    for (u32 g = 0; g < kNumGroups; ++g) {
        if (entryCounts[g] > 0) {
            groups.push_back({g, entryCounts[g], dataStart + sectionOffsets[g]});
        }
    }

    // Sort by section offset.
    std::sort(groups.begin(), groups.end(), [](const GroupInfo& a, const GroupInfo& b) {
        return a.sectionStart < b.sectionStart;
    });

    // Estimate total entries for reservation.
    size_t totalEstimate = 0;
    for (auto& gi : groups)
        totalEstimate += gi.headerCount;
    m_all.reserve(totalEstimate);

    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& info = groups[gi];
        const i32 expectedGroup = static_cast<i32>(info.groupId);

        // Section bounds: from sectionStart to the next group's start (or EOF).
        const size_t sectionEnd =
            (gi + 1 < groups.size()) ? groups[gi + 1].sectionStart : data.size();

        if (info.sectionStart >= data.size()) {
            continue;
        }

        // Scan entries: each is 12 bytes (snoGroup, snoId, nameOff).
        // Stop when the first field no longer matches the expected group ID.
        size_t entryCount = 0;
        {
            reader.setPosition(static_cast<u32>(info.sectionStart));
            size_t pos = info.sectionStart;
            while (pos + 12 <= sectionEnd) {
                const i32 g = reader.read<i32>();
                if (g != expectedGroup)
                    break;
                reader.skip(8);
                ++entryCount;
                pos += 12;
            }
        }

        // Name pool starts right after the entry records.
        const size_t nameBase = info.sectionStart + entryCount * 12;

        const size_t startIdx = m_all.size();

        for (size_t i = 0; i < entryCount; ++i) {
            reader.setPosition(static_cast<u32>(info.sectionStart + i * 12 + 4));

            const i32 snoId = reader.read<i32>();
            const i32 nameRelOffset = reader.read<i32>();

            const size_t namePos = nameBase + static_cast<size_t>(nameRelOffset);

            std::string name;
            if (namePos < data.size()) {
                reader.setPosition(static_cast<u32>(namePos));
                name = reader.readZString();
            }

            TocEntry entry;
            entry.group = static_cast<SnoGroup>(expectedGroup);
            entry.snoId = snoId;
            entry.name = std::move(name);

            m_idIndex[snoId] = m_all.size();
            m_all.push_back(std::move(entry));
        }

        m_groupIndex[expectedGroup] = {startIdx, m_all.size() - startIdx};
    }

    return true;
}

// ---- D4 format (old and new) ------------------------------------------------

bool CoreToc::parseD4(std::span<const u8> data, bool newFormat) {
    const size_t tocOffset = newFormat ? 8 : 4;

    common::span_streambuf sbuf(data);
    std::istream stream(&sbuf);
    common::BinaryReader reader(stream);

    // For newFormat: magic at 0-3, snoGroupsCount at 4-7.
    // For old format: snoGroupsCount at 0-3.
    reader.setPosition(newFormat ? 4 : 0);
    const u32 snoGroupsCount = reader.read<u32>();
    // Reader is now at tocOffset where the header arrays begin.

    if (snoGroupsCount > 1024) {
        return false; // sanity check
    }

    // Number of header arrays: old format has 3 (counts, offsets, unk),
    // new format has 4 (counts, offsets, unk, formatHashes).
    const u32 headerArrays = newFormat ? 4u : 3u;
    const size_t headerSize = tocOffset + static_cast<size_t>(headerArrays) * snoGroupsCount * 4;
    if (data.size() < headerSize + 4) {
        return false;
    }

    // Read per-group counts.
    std::vector<u32> counts(snoGroupsCount);
    for (u32 c = 0; c < snoGroupsCount; ++c) {
        counts[c] = reader.read<u32>();
    }

    // Read per-group offsets.
    std::vector<u32> offsets(snoGroupsCount);
    for (u32 c = 0; c < snoGroupsCount; ++c) {
        offsets[c] = reader.read<u32>();
    }

    // Skip unk array.
    reader.skip(snoGroupsCount * 4);

    // Read format hashes (new format only).
    if (newFormat) {
        for (u32 c = 0; c < snoGroupsCount; ++c) {
            u32 fh = reader.read<u32>();
            if (c > 0 && fh != 0) {
                m_formatHashes[static_cast<i32>(c)] = fh;
            }
        }
    }

    // Data entries start right after the header arrays + an extra 4-byte field.
    // (Matches parse.js: dataStart = (newFormat ? 12 : 8) + (newFormat ? 16 : 12) * snoGroupsCount)
    const size_t dataStart = headerSize + 4;

    // First pass: count total entries.
    size_t totalEntries = 0;
    for (u32 c = 0; c < snoGroupsCount; ++c) {
        totalEntries += counts[c];
    }
    m_all.reserve(totalEntries);

    // Second pass: read entries.
    for (u32 c = 0; c < snoGroupsCount; ++c) {
        const u32 entryCount = counts[c];
        const u32 entryOffset = offsets[c];

        if (entryCount == 0) {
            continue;
        }

        const size_t groupDataStart = dataStart + entryOffset;

        // Entry table: entryCount entries of 12 bytes each.
        const size_t entryTableSize = static_cast<size_t>(entryCount) * 12;

        // Names start after the entry table.
        const size_t nameTableStart = groupDataStart + entryTableSize;

        // Bounds check.
        if (groupDataStart + entryTableSize > data.size()) {
            return false;
        }

        const size_t startIdx = m_all.size();

        for (u32 i = 0; i < entryCount; ++i) {
            reader.setPosition(static_cast<u32>(groupDataStart + static_cast<size_t>(i) * 12));

            const i32 snoGroup = reader.read<i32>();
            const i32 snoId = reader.read<i32>();
            const i32 nameRelOffset = reader.read<i32>();

            // Name offset is relative: nameTableStart + nameRelOffset.
            const size_t namePos = nameTableStart + static_cast<size_t>(nameRelOffset);

            // Read null-terminated name.
            std::string name;
            if (namePos < data.size()) {
                reader.setPosition(static_cast<u32>(namePos));
                name = reader.readZString();
            }

            TocEntry entry;
            entry.group = static_cast<SnoGroup>(snoGroup);
            entry.snoId = snoId;
            entry.name = std::move(name);

            m_idIndex[snoId] = m_all.size();
            m_all.push_back(std::move(entry));
        }

        m_groupIndex[static_cast<i32>(c)] = {startIdx, m_all.size() - startIdx};
    }

    return true;
}

std::span<const TocEntry> CoreToc::entriesForGroup(SnoGroup group) const {
    const auto groupId = static_cast<i32>(group);
    auto it = m_groupIndex.find(groupId);
    if (it == m_groupIndex.end()) {
        return {};
    }
    return std::span<const TocEntry>(m_all.data() + it->second.first, it->second.second);
}

const TocEntry* CoreToc::findById(i32 snoId) const {
    auto it = m_idIndex.find(snoId);
    if (it == m_idIndex.end()) {
        return nullptr;
    }
    return &m_all[it->second];
}

// ============================================================================
// Mutation
// ============================================================================

bool CoreToc::addEntry(const TocEntry& entry) {
    if (m_idIndex.count(entry.snoId))
        return false;

    const i32 groupId = static_cast<i32>(entry.group);
    const size_t idx = m_all.size();
    m_all.push_back(entry);
    m_idIndex[entry.snoId] = idx;

    // Update group index — new entry appends to the group's range.
    auto git = m_groupIndex.find(groupId);
    if (git == m_groupIndex.end()) {
        m_groupIndex[groupId] = {idx, 1};
    } else {
        // If the group's entries are contiguous and this is at the end, extend.
        auto& [start, count] = git->second;
        if (start + count == idx) {
            ++count;
        } else {
            // Non-contiguous — relocate the group's entries to end for contiguity.
            // This happens when entries from other groups were added in between.
            const size_t oldCount = count;
            std::vector<TocEntry> groupEntries(
                m_all.begin() + static_cast<ptrdiff_t>(start),
                m_all.begin() + static_cast<ptrdiff_t>(start + oldCount));
            // The new entry is already at m_all.back() at idx.
            // We don't shuffle here — just track it as a separate range.
            // For serialization, we'll gather entries per group anyway.
            // Mark the group as needing re-gathering by setting start = 0, count = 0.
            // We'll use a gather approach in serialize instead.
            ++count; // Just increment; serialization gathers by group.
        }
    }

    return true;
}

const TocEntry* CoreToc::findByName(SnoGroup group, const std::string& name) const {
    const i32 groupId = static_cast<i32>(group);
    // Linear scan over entries — could be optimized with an index if needed.
    for (auto& e : m_all) {
        if (static_cast<i32>(e.group) == groupId && e.name == name)
            return &e;
    }
    return nullptr;
}

i32 CoreToc::maxSnoId() const {
    i32 maxId = 0;
    for (auto& e : m_all) {
        if (e.snoId > maxId)
            maxId = e.snoId;
    }
    return maxId;
}

// ============================================================================
// Serialization
// ============================================================================

/// Helper: write a little-endian u32 to a buffer.
static void writeU32(std::vector<u8>& buf, u32 val) {
    const auto off = buf.size();
    buf.resize(off + 4);
    std::memcpy(buf.data() + off, &val, 4);
}

/// Helper: write a little-endian i32 to a buffer.
static void writeI32(std::vector<u8>& buf, i32 val) {
    const auto off = buf.size();
    buf.resize(off + 4);
    std::memcpy(buf.data() + off, &val, 4);
}

std::vector<u8> CoreToc::serializeD3Legacy() const {
    constexpr u32 kNumGroups = 70;

    // Gather entries per group.
    std::vector<std::vector<const TocEntry*>> perGroup(kNumGroups);
    for (auto& e : m_all) {
        const auto gid = static_cast<u32>(e.group);
        if (gid < kNumGroups)
            perGroup[gid].push_back(&e);
    }

    // Build per-group section blobs (entry table + name pool).
    struct GroupSection {
        std::vector<u8> data;
        u32 entryCount = 0;
    };
    std::vector<GroupSection> sections(kNumGroups);

    for (u32 g = 0; g < kNumGroups; ++g) {
        auto& entries = perGroup[g];
        if (entries.empty()) continue;

        auto& sec = sections[g];
        sec.entryCount = static_cast<u32>(entries.size());

        // Build name pool and record relative offsets.
        std::vector<u8> namePool;
        std::vector<i32> nameRelOffsets;
        nameRelOffsets.reserve(entries.size());

        for (auto* e : entries) {
            nameRelOffsets.push_back(static_cast<i32>(namePool.size()));
            namePool.insert(namePool.end(), e->name.begin(), e->name.end());
            namePool.push_back(0); // null terminator
        }

        // Write entry records: 12 bytes each (snoGroup, snoId, nameRelOffset).
        sec.data.reserve(entries.size() * 12 + namePool.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            writeI32(sec.data, static_cast<i32>(g));
            writeI32(sec.data, entries[i]->snoId);
            writeI32(sec.data, nameRelOffsets[i]);
        }

        // Append name pool.
        sec.data.insert(sec.data.end(), namePool.begin(), namePool.end());
    }

    // Compute section offsets (relative to data section start).
    std::vector<u32> sectionOffsets(kNumGroups, 0);
    u32 offset = 0;
    for (u32 g = 0; g < kNumGroups; ++g) {
        sectionOffsets[g] = offset;
        offset += static_cast<u32>(sections[g].data.size());
    }

    // Build output.
    std::vector<u8> result;
    constexpr size_t kHeaderSize = kNumGroups * 4 * 4; // 1120
    result.reserve(kHeaderSize + offset);

    // Header: entryCounts[70]
    for (u32 g = 0; g < kNumGroups; ++g)
        writeU32(result, sections[g].entryCount);

    // Header: sectionOffsets[70]
    for (u32 g = 0; g < kNumGroups; ++g)
        writeU32(result, sectionOffsets[g]);

    // Header: hashCounts[70] (zeros)
    for (u32 g = 0; g < kNumGroups; ++g)
        writeU32(result, 0);

    // Header: hashData[70] (zeros)
    for (u32 g = 0; g < kNumGroups; ++g)
        writeU32(result, 0);

    // Data sections.
    for (u32 g = 0; g < kNumGroups; ++g) {
        result.insert(result.end(), sections[g].data.begin(), sections[g].data.end());
    }

    return result;
}

std::vector<u8> CoreToc::serializeD4New() const {
    // Collect all unique group IDs.
    std::unordered_map<i32, std::vector<const TocEntry*>> perGroup;
    i32 maxGroupId = 0;
    for (auto& e : m_all) {
        const auto gid = static_cast<i32>(e.group);
        if (gid > maxGroupId) maxGroupId = gid;
        perGroup[gid].push_back(&e);
    }

    const u32 snoGroupsCount = static_cast<u32>(maxGroupId + 1);

    // Build per-group section blobs.
    struct GroupBlob {
        std::vector<u8> data;
        u32 entryCount = 0;
    };
    std::vector<GroupBlob> groupBlobs(snoGroupsCount);

    for (auto& [gid, entries] : perGroup) {
        if (gid < 0 || static_cast<u32>(gid) >= snoGroupsCount) continue;

        auto& blob = groupBlobs[static_cast<u32>(gid)];
        blob.entryCount = static_cast<u32>(entries.size());

        // Build name pool.
        std::vector<u8> namePool;
        std::vector<i32> nameRelOffsets;
        nameRelOffsets.reserve(entries.size());

        for (auto* e : entries) {
            nameRelOffsets.push_back(static_cast<i32>(namePool.size()));
            namePool.insert(namePool.end(), e->name.begin(), e->name.end());
            namePool.push_back(0);
        }

        // Write entry records: 12 bytes each (snoGroup, snoId, nameRelOffset).
        blob.data.reserve(entries.size() * 12 + namePool.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            writeI32(blob.data, gid);
            writeI32(blob.data, entries[i]->snoId);
            writeI32(blob.data, nameRelOffsets[i]);
        }

        blob.data.insert(blob.data.end(), namePool.begin(), namePool.end());
    }

    // Compute offsets.
    std::vector<u32> offsets(snoGroupsCount, 0);
    u32 dataOffset = 0;
    for (u32 g = 0; g < snoGroupsCount; ++g) {
        offsets[g] = dataOffset;
        dataOffset += static_cast<u32>(groupBlobs[g].data.size());
    }

    // Build output.
    // Header: magic(4) + snoGroupsCount(4) + 4 arrays(counts, offsets, unk, formatHashes) + pad(4)
    const size_t headerSize = 8 + static_cast<size_t>(snoGroupsCount) * 4 * 4 + 4;
    std::vector<u8> result;
    result.reserve(headerSize + dataOffset);

    // Magic.
    writeU32(result, 0xBCDE6611u);
    // snoGroupsCount.
    writeU32(result, snoGroupsCount);

    // Counts array.
    for (u32 g = 0; g < snoGroupsCount; ++g)
        writeU32(result, groupBlobs[g].entryCount);

    // Offsets array.
    for (u32 g = 0; g < snoGroupsCount; ++g)
        writeU32(result, offsets[g]);

    // Unk array (zeros).
    for (u32 g = 0; g < snoGroupsCount; ++g)
        writeU32(result, 0);

    // Format hashes array.
    for (u32 g = 0; g < snoGroupsCount; ++g) {
        auto it = m_formatHashes.find(static_cast<i32>(g));
        writeU32(result, (it != m_formatHashes.end()) ? it->second : 0);
    }

    // Extra 4-byte field.
    writeU32(result, 0);

    // Data sections.
    for (u32 g = 0; g < snoGroupsCount; ++g) {
        result.insert(result.end(), groupBlobs[g].data.begin(), groupBlobs[g].data.end());
    }

    return result;
}

} // namespace sno
} // namespace whiteout

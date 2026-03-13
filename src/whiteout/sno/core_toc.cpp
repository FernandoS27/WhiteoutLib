// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/sno/core_toc.h>

#include <algorithm>
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

} // namespace sno
} // namespace whiteout

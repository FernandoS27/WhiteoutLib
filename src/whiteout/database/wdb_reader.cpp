// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WDB2 … WDB6 — the Cataclysm-through-Legion containers. They share a header
/// shape and grow one block at a time: an id index (WDB2), an offset map and
/// id list (WDB3), header flags (WDB4), a field structure block (WDB5) and a
/// common data table (WDB6).

#include "db_internal.h"

#include <algorithm>

namespace whiteout::database {

namespace {

/// Which optional blocks a file carries. Taken from the header flags for WDB4
/// and later; inferred for WDB3, whose flags lived in the client binary.
struct OptionalBlocks {
    bool offsetMap = false;
    bool relationshipIds = false;
    bool nonInlineIds = false;
};

u32 idSpan(const TableInfo& info) {
    if (info.maxId < info.minId) {
        return 0;
    }
    return info.maxId - info.minId + 1;
}

/// Value width of a WDB6 common-data column before the 7.3.0 padding change.
u32 commonDataTypeSize(u8 type) {
    switch (type) {
    case 1: // short
        return 2;
    case 2: // byte
        return 1;
    case 5: // int64
        return 8;
    default: // string, float, int
        return 4;
    }
}

/// Read the WDB6 common data table into per-field exception lists.
bool readCommonDataTable(TableData& table, ByteCursor& cursor, u32 blockSize, IssueSink& sink) {
    const size_t start = cursor.position();
    if (!cursor.has(blockSize)) {
        return sink.fail("WDB6 common data table runs past the end of the file");
    }

    const u32 columnCount = cursor.read<u32>();
    if (!cursor.ok() || columnCount > table.info.totalFieldCount + 1) {
        return sink.fail("WDB6 common data table declares an implausible column count");
    }

    // Values were tightly packed before 7.3.0 and four-byte padded after.
    // Nothing in the file marks the change, so measure the tight walk: if it
    // consumes the block exactly, the tight reading is the right one.
    struct ColumnHeader {
        u32 count;
        u8 type;
    };
    std::vector<ColumnHeader> headers;
    headers.reserve(columnCount);
    {
        ByteCursor probe = cursor;
        u64 tight = 4;
        for (u32 c = 0; c < columnCount; ++c) {
            const u32 count = probe.read<u32>();
            const u8 type = probe.read<u8>();
            if (!probe.ok()) {
                return sink.fail("truncated WDB6 common data table");
            }
            headers.push_back({count, type});
            const u64 entrySize = 4 + commonDataTypeSize(type);
            tight += 5 + static_cast<u64>(count) * entrySize;
            probe.skip(static_cast<size_t>(static_cast<u64>(count) * entrySize));
            if (!probe.ok()) {
                break;
            }
        }
        const bool padded = tight != blockSize;
        table.commonMaps.resize(std::max<size_t>(table.commonMaps.size(), columnCount));

        for (u32 c = 0; c < columnCount; ++c) {
            const u32 count = cursor.read<u32>();
            const u8 type = cursor.read<u8>();
            if (!cursor.ok()) {
                return sink.fail("truncated WDB6 common data table");
            }
            const u32 valueSize = padded ? 4u : commonDataTypeSize(type);
            auto& entries = table.commonMaps[c];
            entries.reserve(count);
            for (u32 e = 0; e < count; ++e) {
                const u32 id = cursor.read<u32>();
                u64 value = 0;
                for (u32 b = 0; b < valueSize; ++b) {
                    value |= static_cast<u64>(cursor.read<u8>()) << (8 * b);
                }
                if (!cursor.ok()) {
                    return sink.fail("truncated WDB6 common data entry");
                }
                entries.emplace_back(id, static_cast<u32>(value));
            }
            std::sort(entries.begin(), entries.end());
        }
    }

    cursor.seek(start + blockSize);
    return cursor.ok();
}

/// Place every block of the file for one candidate set of optional blocks.
/// Returns false when the layout does not add up, which is how the WDB3
/// inference below rejects wrong guesses.
bool layout(TableData& table, const std::vector<u8>& buffer, size_t afterHeader,
            const OptionalBlocks& blocks, u32 copyTableSize, u32 commonDataSize, IssueSink& sink,
            bool report) {
    TableInfo& info = table.info;
    const bool legacyOrder = info.version == Version::WDB3;
    const u32 span = idSpan(info);
    const size_t fileSize = buffer.size();

    ByteCursor cursor(buffer.data(), fileSize);
    cursor.seek(afterHeader);

    SectionData section;
    std::vector<OffsetMapEntry> offsetMap;

    auto readOffsetMap = [&](u32 count) {
        offsetMap.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            OffsetMapEntry entry;
            entry.offset = cursor.read<u32>();
            entry.size = cursor.read<u16>();
            offsetMap.push_back(entry);
        }
    };

    if (legacyOrder) {
        // WDB3 puts the offset map and relationship ids ahead of the records.
        if (blocks.offsetMap) {
            readOffsetMap(span);
        }
        if (blocks.relationshipIds) {
            section.relationByRecord = cursor.readVector<u32>(span);
        }
        if (!cursor.ok()) {
            return report ? sink.fail("truncated WDB3 index blocks") : false;
        }

        u32 recordsEnd = 0;
        if (blocks.offsetMap) {
            section.recordDataOffset = static_cast<u32>(cursor.position());
            for (const auto& entry : offsetMap) {
                if (entry.offset != 0) {
                    recordsEnd = std::max(recordsEnd, entry.offset + entry.size);
                }
            }
            if (recordsEnd == 0) {
                recordsEnd = section.recordDataOffset;
            }
        } else {
            section.recordDataOffset = static_cast<u32>(cursor.position());
            section.recordDataSize = info.recordCount * info.recordSize;
            recordsEnd = section.recordDataOffset + section.recordDataSize;
        }
        if (recordsEnd > fileSize) {
            return report ? sink.fail("WDB3 record data runs past the end of the file") : false;
        }
        cursor.seek(recordsEnd);
        section.stringDataOffset = recordsEnd;
        section.stringTableSize = info.stringTableSize;
        cursor.skip(info.stringTableSize);
    } else {
        if (blocks.offsetMap) {
            // string_table_size doubles as the absolute offset of the map.
            const u32 mapOffset = info.stringTableSize;
            if (mapOffset < cursor.position() || mapOffset > fileSize) {
                return report ? sink.fail("offset map offset is outside the file") : false;
            }
            section.recordDataOffset = static_cast<u32>(cursor.position());
            section.recordDataSize = mapOffset - section.recordDataOffset;
            section.stringTableSize = 0;
            cursor.seek(mapOffset);
            readOffsetMap(span);
        } else {
            section.recordDataOffset = static_cast<u32>(cursor.position());
            section.recordDataSize = info.recordCount * info.recordSize;
            cursor.skip(section.recordDataSize);
            section.stringDataOffset = static_cast<u32>(cursor.position());
            section.stringTableSize = info.stringTableSize;
            cursor.skip(info.stringTableSize);
        }
        if (!cursor.ok()) {
            return report ? sink.fail("record or string data runs past the end of the file")
                          : false;
        }
        if (blocks.relationshipIds) {
            section.relationByRecord = cursor.readVector<u32>(span);
        }
    }

    if (blocks.nonInlineIds) {
        section.idList = cursor.readVector<u32>(info.recordCount);
    }
    if (copyTableSize != 0) {
        const u32 entries = copyTableSize / 8;
        section.copyTable.reserve(entries);
        for (u32 i = 0; i < entries; ++i) {
            CopyEntry entry;
            entry.newId = cursor.read<u32>();
            entry.sourceId = cursor.read<u32>();
            section.copyTable.push_back(entry);
        }
    }
    if (!cursor.ok()) {
        return report ? sink.fail("id list or copy table runs past the end of the file") : false;
    }

    if (commonDataSize != 0) {
        if (!readCommonDataTable(table, cursor, commonDataSize, sink)) {
            return false;
        }
    }

    if (!report && cursor.position() != fileSize) {
        return false; // candidate layout rejected
    }

    // WDB3/WDB4 relationship ids are indexed by id, not by record.
    if (!section.relationByRecord.empty() && info.version <= Version::WDB4) {
        section.relationKeyedById = true;
        for (u32 i = 0; i < section.relationByRecord.size(); ++i) {
            if (section.relationByRecord[i] != 0) {
                section.relationById[info.minId + i] = section.relationByRecord[i];
            }
        }
        section.relationByRecord.clear();
    }

    section.fileOffset = section.recordDataOffset;
    section.recordCount = info.recordCount;
    section.offsetMap = std::move(offsetMap);

    info.hasOffsetMap = blocks.offsetMap;
    info.hasRelationshipData = blocks.relationshipIds;
    info.hasNonInlineIds = blocks.nonInlineIds;

    table.sections.clear();
    table.sections.push_back(std::move(section));
    return true;
}

} // namespace

bool readWdb(TableData& table, ByteCursor& cursor, IssueSink& sink) {
    TableInfo& info = table.info;
    const Version version = info.version;

    info.recordCount = cursor.read<u32>();
    info.fieldCount = cursor.read<u32>();
    info.recordSize = cursor.read<u32>();
    info.stringTableSize = cursor.read<u32>();
    info.tableHash = cursor.read<u32>();
    const u32 buildOrLayout = cursor.read<u32>();
    if (version <= Version::WDB4) {
        info.build = buildOrLayout;
        info.timestamp = cursor.read<u32>();
    } else {
        info.layoutHash = buildOrLayout;
    }
    info.minId = cursor.read<u32>();
    info.maxId = cursor.read<u32>();
    info.locale = cursor.read<u32>();
    const u32 copyTableSize = cursor.read<u32>();

    u32 commonDataSize = 0;
    if (version == Version::WDB4) {
        info.flags = cursor.read<u32>();
    } else if (version >= Version::WDB5) {
        info.flags = cursor.read<u16>();
        info.idIndex = cursor.read<u16>();
    }
    if (version == Version::WDB6) {
        info.totalFieldCount = cursor.read<u32>();
        commonDataSize = cursor.read<u32>();
    } else {
        info.totalFieldCount = info.fieldCount;
    }
    if (!cursor.ok()) {
        return sink.fail(std::string("truncated ") + versionName(version) + " header");
    }
    info.sectionCount = 1;

    // WDB2's id index maps ids to row numbers; the records themselves are read
    // sequentially, so it only has to be stepped over.
    if (version == Version::WDB2 && info.maxId != 0) {
        const u32 span = idSpan(info);
        cursor.skip(static_cast<size_t>(span) * 6);
        if (!cursor.ok()) {
            return sink.fail("WDB2 id index block runs past the end of the file");
        }
    }

    // WDB5 and later describe their fields; earlier versions leave the record
    // as an opaque row of four-byte slots.
    if (version >= Version::WDB5) {
        std::vector<u16> positions;
        std::vector<u32> byteSizes;
        positions.reserve(info.fieldCount);
        byteSizes.reserve(info.fieldCount);
        for (u32 i = 0; i < info.fieldCount; ++i) {
            const i16 size = cursor.read<i16>();
            const u16 position = cursor.read<u16>();
            positions.push_back(position);
            byteSizes.push_back(static_cast<u32>(std::max(0, (32 - size) / 8)));
        }
        if (!cursor.ok()) {
            return sink.fail("truncated field structure block");
        }
        deriveFieldLayout(table, positions, byteSizes);
    } else {
        table.fields.resize(info.fieldCount);
        for (u32 i = 0; i < info.fieldCount; ++i) {
            FieldInfo& field = table.fields[i];
            field.offsetBits = i * 32;
            field.sizeBits = 32;
            field.elementSize = 4;
        }
    }

    // Columns past field_count exist only in the common data table (WDB6).
    for (u32 i = info.fieldCount; i < info.totalFieldCount; ++i) {
        FieldInfo field;
        field.storage = FieldStorage::CommonData;
        field.sizeBits = 32;
        field.elementSize = 4;
        // Real defaults live in the client binary's DBMeta and are almost
        // always zero; the file itself carries none.
        field.defaultValue = 0;
        table.fields.push_back(field);
    }

    const size_t afterHeader = cursor.position();

    if (version == Version::WDB3) {
        // WDB3 kept its flags in the client binary. Recover them by trying
        // each combination of optional blocks and keeping the one whose
        // blocks tile the file exactly.
        std::vector<OptionalBlocks> candidates;
        for (int mask = 0; mask < 8; ++mask) {
            OptionalBlocks blocks;
            blocks.offsetMap = (mask & 1) != 0;
            blocks.relationshipIds = (mask & 2) != 0;
            blocks.nonInlineIds = (mask & 4) != 0;
            candidates.push_back(blocks);
        }
        for (const auto& blocks : candidates) {
            TableData probe;
            probe.info = table.info;
            if (layout(probe, table.buffer, afterHeader, blocks, copyTableSize, commonDataSize,
                       sink, false)) {
                return layout(table, table.buffer, afterHeader, blocks, copyTableSize,
                              commonDataSize, sink, true);
            }
        }
        sink.warn("WDB3 optional blocks could not be inferred; assuming none are present");
        return layout(table, table.buffer, afterHeader, OptionalBlocks{}, copyTableSize,
                      commonDataSize, sink, true);
    }

    OptionalBlocks blocks;
    blocks.offsetMap = (info.flags & TableFlags::OffsetMap) != 0;
    blocks.relationshipIds = (info.flags & TableFlags::RelationshipData) != 0;
    blocks.nonInlineIds = (info.flags & TableFlags::NonInlineIds) != 0;
    return layout(table, table.buffer, afterHeader, blocks, copyTableSize, commonDataSize, sink,
                  true);
}

} // namespace whiteout::database

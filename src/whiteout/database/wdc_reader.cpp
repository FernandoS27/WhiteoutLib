// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WDC1 … WDC5 — the compressed containers. WDC1 introduced field storage
/// info (bitpacking, pallet data, common data); WDC2 split the payload into
/// sections and made string references relative; WDC3 slimmed the offset map;
/// WDC4 added encrypted-record lists and reordered two blocks; WDC5 prefixed
/// the header with a version number and a build string.

#include "db_internal.h"

#include <algorithm>

namespace whiteout::database {

namespace {

constexpr u32 FIELD_STORAGE_INFO_SIZE = 24;

/// The three trailing words of field_storage_info, whose meaning depends on
/// the storage type.
struct StorageExtra {
    u32 first = 0;
    u32 second = 0;
    u32 third = 0;
};

bool readFieldStorageInfo(TableData& table, ByteCursor& cursor, u32 blockSize, IssueSink& sink) {
    const u32 count = blockSize / FIELD_STORAGE_INFO_SIZE;
    if (count == 0) {
        return true;
    }
    if (count != table.fields.size()) {
        // The field structure block and the storage info block should agree;
        // trust the storage info, which is the one the client reads.
        table.fields.resize(count);
    }

    u32 palletOffset = 0;
    u32 commonOffset = 0;
    for (u32 i = 0; i < count; ++i) {
        const u16 offsetBits = cursor.read<u16>();
        const u16 sizeBits = cursor.read<u16>();
        const u32 additionalSize = cursor.read<u32>();
        const u32 storage = cursor.read<u32>();
        StorageExtra extra;
        extra.first = cursor.read<u32>();
        extra.second = cursor.read<u32>();
        extra.third = cursor.read<u32>();
        if (!cursor.ok()) {
            return sink.fail("truncated field storage info block");
        }
        if (storage > static_cast<u32>(FieldStorage::BitpackedSigned)) {
            return sink.fail("unknown field storage type " + std::to_string(storage));
        }

        FieldInfo& field = table.fields[i];
        const u32 structureElementSize = field.elementSize;
        field.storage = static_cast<FieldStorage>(storage);
        field.offsetBits = offsetBits;
        field.sizeBits = sizeBits;
        field.additionalDataSize = additionalSize;

        switch (field.storage) {
        case FieldStorage::None:
            // The field structure block reports the width of one element and
            // the storage info the width of the whole field, so their ratio is
            // the array length. A zero element size means the structure block
            // disagreed; fall back to a scalar.
            field.elementSize = structureElementSize != 0 ? structureElementSize : sizeBits / 8;
            field.arrayCount =
                field.elementSize != 0 ? std::max(1u, sizeBits / (field.elementSize * 8)) : 1;
            break;
        case FieldStorage::Bitpacked:
        case FieldStorage::BitpackedSigned:
            field.arrayCount = 1;
            field.elementSize = 0;
            field.signExtend =
                field.storage == FieldStorage::BitpackedSigned || (extra.third & 0x1) != 0;
            break;
        case FieldStorage::CommonData:
            field.arrayCount = 1;
            field.elementSize = 4;
            field.defaultValue = extra.first;
            field.additionalDataOffset = commonOffset;
            commonOffset += additionalSize;
            break;
        case FieldStorage::BitpackedIndexed:
            field.arrayCount = 1;
            field.elementSize = 4;
            field.additionalDataOffset = palletOffset;
            palletOffset += additionalSize;
            break;
        case FieldStorage::BitpackedIndexedArray:
            field.arrayCount = std::max(1u, extra.third);
            field.elementSize = 4;
            field.additionalDataOffset = palletOffset;
            palletOffset += additionalSize;
            break;
        }
    }
    return true;
}

/// Slice the common-data block into per-field exception lists.
void sliceCommonData(TableData& table, u32 blockOffset, u32 blockSize) {
    table.commonMaps.assign(table.fields.size(), {});
    for (size_t i = 0; i < table.fields.size(); ++i) {
        const FieldInfo& field = table.fields[i];
        if (field.storage != FieldStorage::CommonData || field.additionalDataSize == 0) {
            continue;
        }
        const u64 start = static_cast<u64>(blockOffset) + field.additionalDataOffset;
        const u64 end = start + field.additionalDataSize;
        if (end > static_cast<u64>(blockOffset) + blockSize || end > table.buffer.size()) {
            continue;
        }
        auto& entries = table.commonMaps[i];
        const u32 pairs = field.additionalDataSize / 8;
        entries.reserve(pairs);
        for (u32 p = 0; p < pairs; ++p) {
            u32 id = 0;
            u32 value = 0;
            std::memcpy(&id, table.buffer.data() + start + p * 8, 4);
            std::memcpy(&value, table.buffer.data() + start + p * 8 + 4, 4);
            entries.emplace_back(id, value);
        }
        std::sort(entries.begin(), entries.end());
    }
}

} // namespace

bool readWdc(TableData& table, ByteCursor& cursor, IssueSink& sink) {
    TableInfo& info = table.info;
    const Version version = info.version;
    const bool sectioned = version >= Version::WDC2;

    if (version >= Version::WDC5) {
        info.versionNumber = cursor.read<u32>();
        info.schemaString = cursor.readFixedString(128);
    }

    info.recordCount = cursor.read<u32>();
    info.fieldCount = cursor.read<u32>();
    info.recordSize = cursor.read<u32>();
    info.stringTableSize = cursor.read<u32>();
    info.tableHash = cursor.read<u32>();
    info.layoutHash = cursor.read<u32>();
    info.minId = cursor.read<u32>();
    info.maxId = cursor.read<u32>();
    info.locale = cursor.read<u32>();

    u32 wdc1CopyTableSize = 0;
    if (!sectioned) {
        wdc1CopyTableSize = cursor.read<u32>();
    }
    info.flags = cursor.read<u16>();
    info.idIndex = cursor.read<u16>();
    info.totalFieldCount = cursor.read<u32>();
    info.bitpackedDataOffset = cursor.read<u32>();
    info.lookupColumnCount = cursor.read<u32>();

    u32 wdc1OffsetMapOffset = 0;
    u32 wdc1IdListSize = 0;
    u32 wdc1RelationshipSize = 0;
    if (!sectioned) {
        wdc1OffsetMapOffset = cursor.read<u32>();
        wdc1IdListSize = cursor.read<u32>();
    }
    const u32 fieldStorageInfoSize = cursor.read<u32>();
    const u32 commonDataSize = cursor.read<u32>();
    const u32 palletDataSize = cursor.read<u32>();
    if (sectioned) {
        info.sectionCount = cursor.read<u32>();
    } else {
        wdc1RelationshipSize = cursor.read<u32>();
        info.sectionCount = 1;
    }
    if (!cursor.ok()) {
        return sink.fail(std::string("truncated ") + versionName(version) + " header");
    }

    info.hasOffsetMap = (info.flags & TableFlags::OffsetMap) != 0;
    info.hasRelationshipData = (info.flags & TableFlags::RelationshipData) != 0;
    info.hasNonInlineIds = (info.flags & TableFlags::NonInlineIds) != 0;
    info.isBitpacked = (info.flags & TableFlags::Bitpacked) != 0;

    // ---- Section headers -----------------------------------------------

    struct SectionHeader {
        u64 tactKeyHash = 0;
        u32 fileOffset = 0;
        u32 recordCount = 0;
        u32 stringTableSize = 0;
        u32 offsetRecordsEnd = 0;
        u32 idListSize = 0;
        u32 relationshipDataSize = 0;
        u32 offsetMapIdCount = 0;
        u32 copyTableCount = 0;
    };

    std::vector<SectionHeader> headers;
    if (sectioned) {
        if (info.sectionCount > cursor.remaining() / 8) {
            return sink.fail("section count is larger than the file can hold");
        }
        headers.reserve(info.sectionCount);
        for (u32 i = 0; i < info.sectionCount; ++i) {
            SectionHeader header;
            header.tactKeyHash = cursor.read<u64>();
            header.fileOffset = cursor.read<u32>();
            header.recordCount = cursor.read<u32>();
            header.stringTableSize = cursor.read<u32>();
            if (version == Version::WDC2) {
                const u32 copyTableSize = cursor.read<u32>();
                header.copyTableCount = copyTableSize / 8;
                header.offsetRecordsEnd = cursor.read<u32>(); // 'offset_map_offset' in WDC2
                header.idListSize = cursor.read<u32>();
                header.relationshipDataSize = cursor.read<u32>();
                header.offsetMapIdCount = info.hasOffsetMap ? (info.maxId - info.minId + 1) : 0;
            } else {
                header.offsetRecordsEnd = cursor.read<u32>();
                header.idListSize = cursor.read<u32>();
                header.relationshipDataSize = cursor.read<u32>();
                header.offsetMapIdCount = cursor.read<u32>();
                header.copyTableCount = cursor.read<u32>();
            }
            headers.push_back(header);
        }
    } else {
        SectionHeader header;
        header.fileOffset = 0; // filled in below, once the fixed blocks are read
        header.recordCount = info.recordCount;
        header.stringTableSize = info.stringTableSize;
        header.offsetRecordsEnd = wdc1OffsetMapOffset;
        header.idListSize = wdc1IdListSize;
        header.relationshipDataSize = wdc1RelationshipSize;
        header.offsetMapIdCount = info.hasOffsetMap ? (info.maxId - info.minId + 1) : 0;
        header.copyTableCount = wdc1CopyTableSize / 8;
        headers.push_back(header);
    }
    if (!cursor.ok()) {
        return sink.fail("truncated section headers");
    }

    // ---- Field structure -------------------------------------------------

    {
        std::vector<u16> positions;
        std::vector<u32> byteSizes;
        positions.reserve(info.totalFieldCount);
        byteSizes.reserve(info.totalFieldCount);
        for (u32 i = 0; i < info.totalFieldCount; ++i) {
            const i16 size = cursor.read<i16>();
            const u16 position = cursor.read<u16>();
            positions.push_back(position);
            byteSizes.push_back(static_cast<u32>(std::max(0, (32 - size) / 8)));
        }
        if (!cursor.ok()) {
            return sink.fail("truncated field structure block");
        }
        deriveFieldLayout(table, positions, byteSizes);
    }

    // WDC1 keeps the record payload right after the field structure and the
    // fixed blocks at the end; WDC2 and later hoist the fixed blocks up front.
    if (!sectioned) {
        headers[0].fileOffset = static_cast<u32>(cursor.position());
        const u64 recordBytes =
            info.hasOffsetMap ? 0 : static_cast<u64>(info.recordCount) * info.recordSize;
        u64 skip = recordBytes + info.stringTableSize;
        if (info.hasOffsetMap) {
            if (wdc1OffsetMapOffset < cursor.position()) {
                return sink.fail("WDC1 offset map offset precedes the record data");
            }
            skip = static_cast<u64>(wdc1OffsetMapOffset) - cursor.position() +
                   static_cast<u64>(headers[0].offsetMapIdCount) * 6;
        }
        cursor.skip(static_cast<size_t>(skip));
        cursor.skip(headers[0].idListSize);
        cursor.skip(static_cast<size_t>(headers[0].copyTableCount) * 8);
        if (!cursor.ok()) {
            return sink.fail("WDC1 record, string or id data runs past the end of the file");
        }
    }

    if (!readFieldStorageInfo(table, cursor, fieldStorageInfoSize, sink)) {
        return false;
    }

    table.palletDataOffset = static_cast<u32>(cursor.position());
    table.palletDataSize = palletDataSize;
    cursor.skip(palletDataSize);
    const u32 commonDataOffset = static_cast<u32>(cursor.position());
    cursor.skip(commonDataSize);
    if (!cursor.ok()) {
        return sink.fail("pallet or common data runs past the end of the file");
    }
    sliceCommonData(table, commonDataOffset, commonDataSize);

    // WDC1 keeps its relationship map at the very end of the file, past the
    // pallet and common data blocks the cursor has just stepped over.
    const u32 wdc1RelationshipOffset = static_cast<u32>(cursor.position());

    // ---- Encrypted record lists (WDC4+) ---------------------------------

    std::vector<std::vector<u32>> encryptedIds(headers.size());
    if (version >= Version::WDC4) {
        for (size_t i = 0; i < headers.size(); ++i) {
            if (headers[i].tactKeyHash == 0) {
                continue;
            }
            const u32 count = cursor.read<u32>();
            if (!cursor.ok() || count > cursor.remaining() / 4) {
                return sink.fail("encrypted record list runs past the end of the file");
            }
            encryptedIds[i] = cursor.readVector<u32>(count);
        }
        if (!cursor.ok()) {
            return sink.fail("truncated encrypted record list");
        }
    }

    // ---- Sections --------------------------------------------------------

    table.sections.reserve(headers.size());
    for (size_t i = 0; i < headers.size(); ++i) {
        const SectionHeader& header = headers[i];
        cursor.seek(header.fileOffset);
        if (!cursor.ok()) {
            return sink.fail("section " + std::to_string(i) + " starts outside the file");
        }

        SectionData section;
        section.tactKeyHash = header.tactKeyHash;
        section.fileOffset = header.fileOffset;
        section.recordCount = header.recordCount;
        section.stringTableSize = header.stringTableSize;
        section.encryptedIds = std::move(encryptedIds[i]);

        auto readOffsetMap = [&](u32 count) {
            section.offsetMap.reserve(count);
            for (u32 e = 0; e < count; ++e) {
                OffsetMapEntry entry;
                entry.offset = cursor.read<u32>();
                entry.size = cursor.read<u16>();
                section.offsetMap.push_back(entry);
            }
        };

        if (info.hasOffsetMap) {
            section.recordDataOffset = header.fileOffset;
            section.recordDataSize = header.offsetRecordsEnd > header.fileOffset
                                         ? header.offsetRecordsEnd - header.fileOffset
                                         : 0;
            section.stringTableSize = 0;
            cursor.seek(static_cast<size_t>(header.fileOffset) + section.recordDataSize);
            // WDC1 and WDC2 park the offset map immediately after the records;
            // WDC3 moved it past the id list and copy table.
            if (version <= Version::WDC2) {
                readOffsetMap(header.offsetMapIdCount);
            }
        } else {
            section.recordDataOffset = header.fileOffset;
            section.recordDataSize = header.recordCount * info.recordSize;
            cursor.skip(section.recordDataSize);
            section.stringDataOffset = static_cast<u32>(cursor.position());
            cursor.skip(header.stringTableSize);
        }
        if (!cursor.ok()) {
            return sink.fail("section " + std::to_string(i) +
                             " record data runs past the end of the file");
        }

        section.idList = cursor.readVector<u32>(header.idListSize / 4);
        section.copyTable.reserve(header.copyTableCount);
        for (u32 c = 0; c < header.copyTableCount; ++c) {
            CopyEntry entry;
            entry.newId = cursor.read<u32>();
            entry.sourceId = cursor.read<u32>();
            section.copyTable.push_back(entry);
        }
        if (!cursor.ok()) {
            return sink.fail("section " + std::to_string(i) +
                             " id list or copy table runs past the end of the file");
        }

        auto readRelationships = [&](u32 blockSize) {
            if (blockSize == 0) {
                return;
            }
            const size_t start = cursor.position();
            const u32 count = cursor.read<u32>();
            cursor.skip(8); // min_id, max_id
            // Encrypted sections carry an unreadable count; the header's block
            // size is authoritative for stepping over it either way.
            const u32 storable = std::min<u32>(count, blockSize >= 12 ? (blockSize - 12) / 8 : 0);
            if (cursor.ok()) {
                const bool keyedById =
                    version >= Version::WDC4 && (info.flags & TableFlags::RelationshipData) != 0;
                section.relationKeyedById = keyedById;
                if (!keyedById) {
                    section.relationByRecord.assign(header.recordCount, 0);
                }
                for (u32 e = 0; e < storable; ++e) {
                    const u32 foreignId = cursor.read<u32>();
                    const u32 target = cursor.read<u32>();
                    if (!cursor.ok()) {
                        break;
                    }
                    if (keyedById) {
                        section.relationById[target] = foreignId;
                    } else if (target < section.relationByRecord.size()) {
                        section.relationByRecord[target] = foreignId;
                    }
                }
            }
            cursor.seek(start + blockSize);
        };

        if (version <= Version::WDC2) {
            if (!sectioned) {
                cursor.seek(wdc1RelationshipOffset);
            }
            readRelationships(header.relationshipDataSize);
        } else {
            readOffsetMap(header.offsetMapIdCount);
            if (version >= Version::WDC4 && info.hasRelationshipData) {
                section.offsetMapIds = cursor.readVector<u32>(header.offsetMapIdCount);
                readRelationships(header.relationshipDataSize);
            } else {
                readRelationships(header.relationshipDataSize);
                section.offsetMapIds = cursor.readVector<u32>(header.offsetMapIdCount);
            }
            if (!cursor.ok()) {
                return sink.fail("section " + std::to_string(i) +
                                 " offset map or relationship data is truncated");
            }
        }

        // A section with a TACT key whose records we cannot decrypt reads as
        // zeroes. Flag it so callers can tell "no data" from "zero data".
        if (section.tactKeyHash != 0 && section.recordDataOffset < table.buffer.size()) {
            const u8* start = table.buffer.data() + section.recordDataOffset;
            const size_t length = std::min<size_t>(section.recordDataSize,
                                                   table.buffer.size() - section.recordDataOffset);
            section.encrypted = std::all_of(start, start + length, [](u8 b) { return b == 0; });
            if (section.encrypted) {
                sink.warn("section " + std::to_string(i) + " is encrypted (TACT key " +
                          std::to_string(section.tactKeyHash) + ")");
            }
        }

        table.sections.push_back(std::move(section));
    }

    return true;
}

} // namespace whiteout::database

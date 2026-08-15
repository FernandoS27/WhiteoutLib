// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WDBC — the original .dbc container: header, records, string block.

#include "db_internal.h"

namespace whiteout::database {

bool readWdbc(TableData& table, ByteCursor& cursor, IssueSink& sink) {
    TableInfo& info = table.info;

    info.recordCount = cursor.read<u32>();
    info.fieldCount = cursor.read<u32>();
    info.recordSize = cursor.read<u32>();
    info.stringTableSize = cursor.read<u32>();
    if (!cursor.ok()) {
        return sink.fail("truncated WDBC header");
    }
    info.totalFieldCount = info.fieldCount;
    info.sectionCount = 1;

    // The container describes no field types, only how many four-byte slots a
    // record holds. Bind a Schema for anything richer.
    if (info.fieldCount != 0 && info.recordSize / 4 != info.fieldCount) {
        sink.warn("WDBC record_size does not match field_count * 4; fields are laid out as "
                  "four-byte slots");
    }

    const u64 recordBytes = static_cast<u64>(info.recordCount) * info.recordSize;
    if (!cursor.has(static_cast<size_t>(recordBytes)) ||
        cursor.remaining() < recordBytes + info.stringTableSize) {
        return sink.fail("WDBC record and string data run past the end of the file");
    }

    SectionData section;
    section.fileOffset = static_cast<u32>(cursor.position());
    section.recordCount = info.recordCount;
    section.recordDataOffset = section.fileOffset;
    section.recordDataSize = static_cast<u32>(recordBytes);
    section.stringDataOffset = section.recordDataOffset + section.recordDataSize;
    section.stringTableSize = info.stringTableSize;

    table.fields.resize(info.fieldCount);
    for (u32 i = 0; i < info.fieldCount; ++i) {
        FieldInfo& field = table.fields[i];
        field.storage = FieldStorage::None;
        field.offsetBits = i * 32;
        field.sizeBits = 32;
        field.arrayCount = 1;
        field.elementSize = 4;
    }

    table.sections.push_back(std::move(section));
    return true;
}

} // namespace whiteout::database

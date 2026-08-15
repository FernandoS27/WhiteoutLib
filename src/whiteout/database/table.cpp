// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "db_internal.h"

#include <algorithm>

namespace whiteout::database {

// ============================================================================
// Field layout
// ============================================================================

void deriveFieldLayout(TableData& table, const std::vector<u16>& positions,
                       const std::vector<u32>& byteSizes) {
    const size_t count = positions.size();
    table.fields.assign(count, FieldInfo{});

    for (size_t i = 0; i < count; ++i) {
        FieldInfo& field = table.fields[i];
        const u32 position = positions[i];
        const u32 elementSize = byteSizes[i];
        // The block records where each field starts but not how long it is;
        // the distance to the next field is the whole array.
        const u32 next =
            (i + 1 < count) ? positions[i + 1] : std::max<u32>(table.info.recordSize, position);
        const u32 span = next > position ? next - position : elementSize;

        field.storage = FieldStorage::None;
        field.offsetBits = position * 8;
        field.elementSize = elementSize;
        field.arrayCount = (elementSize != 0 && span >= elementSize) ? span / elementSize : 1;
        field.sizeBits = field.arrayCount * elementSize * 8;
    }
}

// ============================================================================
// TableData — assembly
// ============================================================================

namespace {

u32 idFieldIndex(const TableInfo& info) {
    return info.version >= Version::WDB5 ? info.idIndex : 0;
}

} // namespace

void TableData::finalize() {
    // Position every section inside the virtual blob the client assembles:
    // all record data first, then all string data. WDC2 and later express
    // string references relative to a field's place in that blob.
    u32 recordBase = 0;
    for (auto& section : sections) {
        section.virtualRecordBase = recordBase;
        recordBase += section.recordDataSize;
    }
    totalRecordDataSize = recordBase;

    u32 stringBase = totalRecordDataSize;
    for (auto& section : sections) {
        section.virtualStringBase = stringBase;
        stringBase += section.stringTableSize;
    }

    // -- Rows ------------------------------------------------------------

    for (u32 s = 0; s < sections.size(); ++s) {
        const SectionData& section = sections[s];

        if (info.hasOffsetMap) {
            u32 present = 0;
            for (u32 e = 0; e < section.offsetMap.size(); ++e) {
                const OffsetMapEntry& entry = section.offsetMap[e];
                if (entry.offset == 0 || entry.size == 0) {
                    continue; // dense pre-WDC3 maps leave holes for absent ids
                }
                if (static_cast<u64>(entry.offset) + entry.size > buffer.size()) {
                    continue;
                }
                RowEntry row;
                row.section = s;
                row.recordIndex = e;
                row.dataOffset = entry.offset;
                row.dataSize = entry.size;
                row.sparse = true;
                if (e < section.offsetMapIds.size()) {
                    row.id = section.offsetMapIds[e];
                } else if (present < section.idList.size()) {
                    row.id = section.idList[present];
                } else {
                    row.id = info.minId + e;
                }
                row.lookupId = row.id;
                rows.push_back(row);
                ++present;
            }
        } else {
            for (u32 r = 0; r < section.recordCount; ++r) {
                RowEntry row;
                row.section = s;
                row.recordIndex = r;
                row.dataOffset = section.recordDataOffset + r * info.recordSize;
                row.dataSize = info.recordSize;
                if (static_cast<u64>(row.dataOffset) + row.dataSize > buffer.size()) {
                    break;
                }
                if (r < section.idList.size()) {
                    row.id = section.idList[r];
                } else {
                    row.id = static_cast<u32>(fieldValue(row, idFieldIndex(info), 0, false));
                }
                row.lookupId = row.id;
                rows.push_back(row);
            }
        }
    }

    idToRow.reserve(rows.size());
    for (u32 i = 0; i < rows.size(); ++i) {
        idToRow.emplace(rows[i].id, i);
    }

    // Copy-table rows duplicate a record under a new id. Their values — and
    // therefore their common-data lookups — stay those of the source row.
    for (u32 s = 0; s < sections.size(); ++s) {
        for (const CopyEntry& copy : sections[s].copyTable) {
            const auto found = idToRow.find(copy.sourceId);
            if (found == idToRow.end()) {
                continue;
            }
            RowEntry row = rows[found->second];
            row.id = copy.newId;
            row.lookupId = copy.sourceId;
            row.copy = true;
            idToRow.emplace(row.id, static_cast<u32>(rows.size()));
            rows.push_back(row);
        }
    }

    rowIds.reserve(rows.size());
    for (const RowEntry& row : rows) {
        rowIds.push_back(row.id);
    }

    // -- Public section mirror -------------------------------------------

    sectionInfos.reserve(sections.size());
    for (const SectionData& section : sections) {
        SectionInfo out;
        out.tactKeyHash = section.tactKeyHash;
        out.fileOffset = section.fileOffset;
        out.recordCount = section.recordCount;
        out.stringTableSize = section.stringTableSize;
        out.copyCount = static_cast<u32>(section.copyTable.size());
        out.offsetMapCount = static_cast<u32>(section.offsetMap.size());
        out.encrypted = section.encrypted;
        out.encryptedIds = section.encryptedIds;
        sectionInfos.push_back(std::move(out));
    }
}

// ============================================================================
// TableData — decoding
// ============================================================================

std::span<const u8> TableData::recordBytes(const RowEntry& row) const {
    if (row.dataOffset >= buffer.size()) {
        return {};
    }
    const size_t available = buffer.size() - row.dataOffset;
    return std::span<const u8>(buffer.data() + row.dataOffset,
                               std::min<size_t>(row.dataSize, available));
}

u32 TableData::fieldByteOffset(u32 fieldIndex, u32 arrayIndex) const {
    if (fieldIndex >= fields.size()) {
        return 0;
    }
    const FieldInfo& field = fields[fieldIndex];
    const u32 elementSize = field.elementSize != 0 ? field.elementSize : 4;
    return field.offsetBits / 8 + arrayIndex * elementSize;
}

u64 TableData::commonDataValue(u32 fieldIndex, u32 id, u32 defaultValue) const {
    if (fieldIndex >= commonMaps.size()) {
        return defaultValue;
    }
    const auto& entries = commonMaps[fieldIndex];
    const auto found = std::lower_bound(
        entries.begin(), entries.end(), id,
        [](const std::pair<u32, u32>& entry, u32 key) { return entry.first < key; });
    if (found != entries.end() && found->first == id) {
        return found->second;
    }
    return defaultValue;
}

u64 TableData::palletValue(const FieldInfo& field, u32 index) const {
    const u64 offset = static_cast<u64>(palletDataOffset) + field.additionalDataOffset +
                       static_cast<u64>(index) * 4;
    if (static_cast<u64>(index) * 4 + 4 > field.additionalDataSize || offset + 4 > buffer.size()) {
        return 0;
    }
    u32 value = 0;
    std::memcpy(&value, buffer.data() + offset, 4);
    return value;
}

u64 TableData::fieldValue(const RowEntry& row, u32 fieldIndex, u32 arrayIndex,
                          bool signedValue) const {
    if (fieldIndex >= fields.size()) {
        return 0;
    }
    const FieldInfo& field = fields[fieldIndex];
    if (arrayIndex >= field.arrayCount && field.storage != FieldStorage::None) {
        return 0;
    }
    const std::span<const u8> data = recordBytes(row);

    switch (field.storage) {
    case FieldStorage::None: {
        const u32 elementSize = field.elementSize != 0 ? field.elementSize : 4;
        const u32 offset = field.offsetBits / 8 + arrayIndex * elementSize;
        u64 value = 0;
        for (u32 i = 0; i < elementSize && i < 8; ++i) {
            if (offset + i < data.size()) {
                value |= static_cast<u64>(data[offset + i]) << (8 * i);
            }
        }
        return signedValue ? static_cast<u64>(signExtend(value, elementSize * 8)) : value;
    }
    case FieldStorage::Bitpacked:
    case FieldStorage::BitpackedSigned: {
        const u64 value = readBits(data.data(), data.size(), field.offsetBits, field.sizeBits);
        return signedValue ? static_cast<u64>(signExtend(value, field.sizeBits)) : value;
    }
    case FieldStorage::CommonData: {
        const u64 value = commonDataValue(fieldIndex, row.lookupId, field.defaultValue);
        return signedValue ? static_cast<u64>(signExtend(value, 32)) : value;
    }
    case FieldStorage::BitpackedIndexed: {
        const u64 index = readBits(data.data(), data.size(), field.offsetBits, field.sizeBits);
        const u64 value = palletValue(field, static_cast<u32>(index));
        return signedValue ? static_cast<u64>(signExtend(value, 32)) : value;
    }
    case FieldStorage::BitpackedIndexedArray: {
        const u64 index = readBits(data.data(), data.size(), field.offsetBits, field.sizeBits);
        const u64 value =
            palletValue(field, static_cast<u32>(index) * field.arrayCount + arrayIndex);
        return signedValue ? static_cast<u64>(signExtend(value, 32)) : value;
    }
    }
    return 0;
}

namespace {

std::string_view cstringAt(const std::vector<u8>& buffer, u64 offset, u64 limit) {
    if (offset >= limit || offset >= buffer.size()) {
        return {};
    }
    const u64 end = std::min<u64>(limit, buffer.size());
    const char* start = reinterpret_cast<const char*>(buffer.data() + offset);
    u64 length = 0;
    while (offset + length < end && start[length] != '\0') {
        ++length;
    }
    return std::string_view(start, static_cast<size_t>(length));
}

} // namespace

std::string_view TableData::inlineString(const RowEntry& row, u32 byteOffsetInRecord) const {
    const u64 start = static_cast<u64>(row.dataOffset) + byteOffsetInRecord;
    const u64 limit = static_cast<u64>(row.dataOffset) + row.dataSize;
    return cstringAt(buffer, start, limit);
}

std::string_view TableData::resolveString(const RowEntry& row, u32 byteOffsetInRecord,
                                          u32 rawValue) const {
    if (row.sparse) {
        return inlineString(row, byteOffsetInRecord);
    }
    if (row.section >= sections.size()) {
        return {};
    }
    const SectionData& section = sections[row.section];

    if (info.version < Version::WDC2) {
        // The reference is an offset into the string block itself.
        if (rawValue == 0 || rawValue >= section.stringTableSize) {
            return {};
        }
        return cstringAt(buffer, static_cast<u64>(section.stringDataOffset) + rawValue,
                         static_cast<u64>(section.stringDataOffset) + section.stringTableSize);
    }

    // WDC2 and later: the reference is relative to the field that holds it,
    // measured inside the virtual blob.
    const u64 fieldPosition = static_cast<u64>(section.virtualRecordBase) +
                              static_cast<u64>(row.recordIndex) * info.recordSize +
                              byteOffsetInRecord;
    const u64 target = fieldPosition + rawValue;
    for (const SectionData& candidate : sections) {
        if (candidate.stringTableSize == 0) {
            continue;
        }
        if (target >= candidate.virtualStringBase &&
            target < static_cast<u64>(candidate.virtualStringBase) + candidate.stringTableSize) {
            const u64 offset = candidate.stringDataOffset + (target - candidate.virtualStringBase);
            return cstringAt(buffer, offset,
                             static_cast<u64>(candidate.stringDataOffset) +
                                 candidate.stringTableSize);
        }
    }
    return {};
}

// ============================================================================
// Table
// ============================================================================

Table::Table() = default;
Table::~Table() = default;
Table::Table(Table&&) noexcept = default;
Table& Table::operator=(Table&&) noexcept = default;

namespace {

const TableInfo& emptyInfo() {
    static const TableInfo info;
    return info;
}

} // namespace

const TableInfo& Table::info() const {
    return data_ ? data_->info : emptyInfo();
}

const std::vector<FieldInfo>& Table::fields() const {
    static const std::vector<FieldInfo> empty;
    return data_ ? data_->fields : empty;
}

const std::vector<SectionInfo>& Table::sections() const {
    static const std::vector<SectionInfo> empty;
    return data_ ? data_->sectionInfos : empty;
}

size_t Table::rowCount() const {
    return data_ ? data_->rows.size() : 0;
}

Row Table::row(size_t index) const {
    if (!data_ || index >= data_->rows.size()) {
        return Row();
    }
    return Row(data_.get(), static_cast<u32>(index));
}

std::optional<Row> Table::findById(u32 id) const {
    if (!data_) {
        return std::nullopt;
    }
    const auto found = data_->idToRow.find(id);
    if (found == data_->idToRow.end()) {
        return std::nullopt;
    }
    return Row(data_.get(), found->second);
}

const std::vector<u32>& Table::ids() const {
    static const std::vector<u32> empty;
    return data_ ? data_->rowIds : empty;
}

bool Table::bind(const Schema& schema, std::vector<std::string>* issues) {
    if (!data_) {
        if (issues != nullptr) {
            issues->push_back("table is empty");
        }
        return false;
    }
    return data_->bindSchema(schema, issues);
}

const Schema* Table::schema() const {
    if (!data_ || !data_->schema.has_value()) {
        return nullptr;
    }
    return &*data_->schema;
}

} // namespace whiteout::database

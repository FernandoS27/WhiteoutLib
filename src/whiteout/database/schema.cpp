// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "db_internal.h"

#include <algorithm>
#include <cstring>

namespace whiteout::database {

// ============================================================================
// Value
// ============================================================================

u32 columnTypeSize(ColumnType type) {
    switch (type) {
    case ColumnType::Int8:
    case ColumnType::UInt8:
        return 1;
    case ColumnType::Int16:
    case ColumnType::UInt16:
        return 2;
    case ColumnType::Int64:
    case ColumnType::UInt64:
        return 8;
    case ColumnType::Int32:
    case ColumnType::UInt32:
    case ColumnType::Float:
    case ColumnType::String:
    case ColumnType::LocString:
        break;
    }
    return 4;
}

bool columnTypeIsString(ColumnType type) {
    return type == ColumnType::String || type == ColumnType::LocString;
}

namespace {

bool columnTypeIsSigned(ColumnType type) {
    switch (type) {
    case ColumnType::Int8:
    case ColumnType::Int16:
    case ColumnType::Int32:
    case ColumnType::Int64:
        return true;
    default:
        return false;
    }
}

} // namespace

Value Value::makeInt(i64 v) {
    Value value;
    value.kind_ = Kind::Int;
    value.num_.i = v;
    return value;
}

Value Value::makeUInt(u64 v) {
    Value value;
    value.kind_ = Kind::UInt;
    value.num_.u = v;
    return value;
}

Value Value::makeFloat(f32 v) {
    Value value;
    value.kind_ = Kind::Float;
    value.num_.f = v;
    return value;
}

Value Value::makeString(std::string_view v) {
    Value value;
    value.kind_ = Kind::String;
    value.str_ = v;
    return value;
}

i64 Value::asInt() const {
    switch (kind_) {
    case Kind::Int:
        return num_.i;
    case Kind::UInt:
        return static_cast<i64>(num_.u);
    case Kind::Float:
        return static_cast<i64>(num_.f);
    default:
        return 0;
    }
}

u64 Value::asUInt() const {
    switch (kind_) {
    case Kind::Int:
        return static_cast<u64>(num_.i);
    case Kind::UInt:
        return num_.u;
    case Kind::Float:
        return static_cast<u64>(num_.f);
    default:
        return 0;
    }
}

f32 Value::asFloat() const {
    switch (kind_) {
    case Kind::Int:
        return static_cast<f32>(num_.i);
    case Kind::UInt:
        return static_cast<f32>(num_.u);
    case Kind::Float:
        return num_.f;
    default:
        return 0.0f;
    }
}

std::string_view Value::asString() const {
    return kind_ == Kind::String ? str_ : std::string_view{};
}

// ============================================================================
// Schema
// ============================================================================

Schema& Schema::add(std::string columnName, ColumnType type, u32 arraySize) {
    Column column;
    column.name = std::move(columnName);
    column.type = type;
    column.arraySize = std::max(1u, arraySize);
    columns.push_back(std::move(column));
    return *this;
}

Schema& Schema::addId(std::string columnName, ColumnType type) {
    add(std::move(columnName), type);
    columns.back().isId = true;
    return *this;
}

Schema& Schema::addRelation(std::string columnName, ColumnType type) {
    add(std::move(columnName), type);
    columns.back().isRelation = true;
    return *this;
}

const Column* Schema::find(std::string_view columnName) const {
    for (const Column& column : columns) {
        if (column.name == columnName) {
            return &column;
        }
    }
    return nullptr;
}

std::optional<u32> Schema::indexOf(std::string_view columnName) const {
    for (u32 i = 0; i < columns.size(); ++i) {
        if (columns[i].name == columnName) {
            return i;
        }
    }
    return std::nullopt;
}

u32 Schema::slotCount(const Column& column) const {
    return column.type == ColumnType::LocString ? std::max(1u, locStringSlots) : column.arraySize;
}

// ============================================================================
// Binding
// ============================================================================

bool TableData::bindSchema(const Schema& newSchema, std::vector<std::string>* issues) {
    auto report = [&](const std::string& message) {
        if (issues != nullptr) {
            issues->push_back(message);
        }
    };

    // WDB5 and later describe their own fields, so the schema is matched
    // against them column by column. Earlier versions describe nothing, and
    // the schema alone lays the record out.
    const bool fileDescribesFields = info.version >= Version::WDB5;

    std::vector<BoundColumn> candidate;
    candidate.reserve(newSchema.columns.size());
    bool ok = true;

    if (fileDescribesFields) {
        u32 fieldIndex = 0;
        for (const Column& column : newSchema.columns) {
            BoundColumn boundColumn;
            boundColumn.type = column.type;
            boundColumn.arraySize = column.arraySize;
            boundColumn.slots = newSchema.slotCount(column);
            boundColumn.isRelation = column.isRelation;
            boundColumn.isId = column.isId;

            if (column.isRelation) {
                // Supplied by the relationship map; consumes no field.
                boundColumn.viaField = false;
                candidate.push_back(boundColumn);
                continue;
            }
            if (column.isId && info.hasNonInlineIds) {
                // The id lives in the section's id list, not in the record.
                boundColumn.viaField = false;
                boundColumn.fromRowId = true;
                candidate.push_back(boundColumn);
                continue;
            }
            if (fieldIndex >= fields.size()) {
                report("column '" + column.name + "' has no matching field in the file");
                ok = false;
                break;
            }
            const FieldInfo& field = fields[fieldIndex];
            if (boundColumn.slots != field.arrayCount) {
                report("column '" + column.name + "' declares " +
                       std::to_string(boundColumn.slots) + " element(s) but field " +
                       std::to_string(fieldIndex) + " holds " + std::to_string(field.arrayCount));
                ok = false;
            }
            boundColumn.viaField = true;
            boundColumn.fieldIndex = fieldIndex;
            boundColumn.byteOffset = field.offsetBits / 8;
            boundColumn.elementSize =
                field.arrayCount != 0 ? (field.sizeBits / 8) / field.arrayCount : field.elementSize;
            candidate.push_back(boundColumn);
            ++fieldIndex;
        }
        if (ok && fieldIndex != fields.size()) {
            report("schema describes " + std::to_string(fieldIndex) +
                   " field(s) but the file has " + std::to_string(fields.size()));
            ok = false;
        }
    } else {
        u32 offset = 0;
        for (const Column& column : newSchema.columns) {
            BoundColumn boundColumn;
            boundColumn.type = column.type;
            boundColumn.arraySize = column.arraySize;
            boundColumn.slots = newSchema.slotCount(column);
            boundColumn.isRelation = column.isRelation;
            boundColumn.isId = column.isId;
            boundColumn.viaField = false;
            boundColumn.fromRowId = column.isId && info.hasNonInlineIds;
            boundColumn.elementSize = columnTypeSize(column.type);
            boundColumn.byteOffset = offset;
            if (!column.isRelation && !boundColumn.fromRowId) {
                offset += boundColumn.slots * boundColumn.elementSize;
            }
            candidate.push_back(boundColumn);
        }
        // A record may be padded up to the width of its widest field, but not
        // by more than that.
        if (offset > info.recordSize || info.recordSize - offset > 3) {
            report("schema spans " + std::to_string(offset) + " byte(s) but the record is " +
                   std::to_string(info.recordSize));
            ok = false;
        }
    }

    if (!ok) {
        return false;
    }

    schema = newSchema;
    bound = std::move(candidate);
    return true;
}

// ============================================================================
// Column access
// ============================================================================

bool TableData::sparseColumnOffset(const RowEntry& row, u32 column, u32 arrayIndex,
                                   u32& byteOffset) const {
    // Variable-length records inline their strings, so every column before the
    // one being read has to be stepped over.
    u32 offset = 0;
    const u32 limit = row.dataSize;

    auto skipString = [&]() {
        const u64 start = static_cast<u64>(row.dataOffset) + offset;
        u64 length = 0;
        while (offset + length < limit && start + length < buffer.size() &&
               buffer[static_cast<size_t>(start + length)] != '\0') {
            ++length;
        }
        offset += static_cast<u32>(length) + 1;
    };

    auto widthOf = [&](const BoundColumn& boundColumn) -> u32 {
        if (boundColumn.viaField && boundColumn.fieldIndex < fields.size()) {
            const u32 fromFile = fields[boundColumn.fieldIndex].sizeBits / 8;
            if (fromFile != 0) {
                return fromFile;
            }
        }
        return boundColumn.slots * columnTypeSize(boundColumn.type);
    };

    for (u32 c = 0; c < column && c < bound.size(); ++c) {
        const BoundColumn& boundColumn = bound[c];
        if (boundColumn.isRelation || boundColumn.fromRowId) {
            continue;
        }
        if (columnTypeIsString(boundColumn.type)) {
            for (u32 s = 0; s < boundColumn.slots; ++s) {
                skipString();
            }
        } else {
            offset += widthOf(boundColumn);
        }
        if (offset > limit) {
            return false;
        }
    }

    if (column < bound.size()) {
        const BoundColumn& boundColumn = bound[column];
        if (columnTypeIsString(boundColumn.type)) {
            for (u32 s = 0; s < arrayIndex; ++s) {
                skipString();
            }
        } else {
            const u32 elementSize = boundColumn.slots != 0
                                        ? widthOf(boundColumn) / boundColumn.slots
                                        : columnTypeSize(boundColumn.type);
            offset += arrayIndex * elementSize;
        }
    }

    if (offset > limit) {
        return false;
    }
    byteOffset = offset;
    return true;
}

namespace {

u64 readRecordBytes(std::span<const u8> data, u32 offset, u32 width) {
    u64 value = 0;
    for (u32 i = 0; i < width && i < 8; ++i) {
        if (offset + i < data.size()) {
            value |= static_cast<u64>(data[offset + i]) << (8 * i);
        }
    }
    return value;
}

Value makeTyped(ColumnType type, u64 raw, u32 width) {
    switch (type) {
    case ColumnType::Int8:
    case ColumnType::Int16:
    case ColumnType::Int32:
    case ColumnType::Int64:
        return Value::makeInt(signExtend(raw, width * 8));
    case ColumnType::Float: {
        const u32 bits = static_cast<u32>(raw);
        f32 value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return Value::makeFloat(value);
    }
    default:
        return Value::makeUInt(raw);
    }
}

} // namespace

Value TableData::columnValue(const RowEntry& row, u32 column, u32 arrayIndex) const {
    if (!schema.has_value() || column >= bound.size()) {
        return Value();
    }
    const BoundColumn& boundColumn = bound[column];
    if (arrayIndex >= boundColumn.slots) {
        return Value();
    }

    if (boundColumn.fromRowId) {
        return Value::makeUInt(row.id);
    }

    if (boundColumn.isRelation) {
        if (row.section >= sections.size()) {
            return Value();
        }
        const SectionData& section = sections[row.section];
        if (section.relationKeyedById) {
            const auto found = section.relationById.find(row.lookupId);
            return found != section.relationById.end() ? Value::makeUInt(found->second) : Value();
        }
        if (row.recordIndex < section.relationByRecord.size()) {
            return Value::makeUInt(section.relationByRecord[row.recordIndex]);
        }
        return Value();
    }

    if (row.sparse) {
        u32 offset = 0;
        if (!sparseColumnOffset(row, column, arrayIndex, offset)) {
            return Value();
        }
        if (columnTypeIsString(boundColumn.type)) {
            return Value::makeString(inlineString(row, offset));
        }
        const u32 width = columnTypeSize(boundColumn.type);
        return makeTyped(boundColumn.type, readRecordBytes(recordBytes(row), offset, width), width);
    }

    if (boundColumn.viaField) {
        const u32 fieldIndex = boundColumn.fieldIndex;
        if (columnTypeIsString(boundColumn.type)) {
            const u32 raw = static_cast<u32>(fieldValue(row, fieldIndex, arrayIndex, false));
            return Value::makeString(
                resolveString(row, fieldByteOffset(fieldIndex, arrayIndex), raw));
        }
        const bool wantSigned = columnTypeIsSigned(boundColumn.type);
        const u64 raw = fieldValue(row, fieldIndex, arrayIndex, wantSigned);
        if (wantSigned) {
            return Value::makeInt(static_cast<i64>(raw));
        }
        return makeTyped(boundColumn.type, raw, columnTypeSize(boundColumn.type));
    }

    // Legacy layout: the schema places the column itself.
    const u32 width = columnTypeSize(boundColumn.type);
    const u32 offset = boundColumn.byteOffset + arrayIndex * width;
    const u64 raw = readRecordBytes(recordBytes(row), offset, width);
    if (columnTypeIsString(boundColumn.type)) {
        return Value::makeString(resolveString(row, offset, static_cast<u32>(raw)));
    }
    return makeTyped(boundColumn.type, raw, width);
}

} // namespace whiteout::database

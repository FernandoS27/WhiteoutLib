// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file db_internal.h
/// @brief Storage behind database::Table plus the primitives the readers share.
///
/// Internal header — not part of the public include path.

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/database/schema.h>
#include <whiteout/database/table.h>
#include <whiteout/database/types.h>

#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout::database {

// ============================================================================
// Bit and byte primitives
// ============================================================================

/// Read `sizeBits` (≤ 64) starting at `offsetBits`, little-endian.
/// Bytes past the end of the record read as zero, matching how the client
/// tolerates records that stop short of the nominal record size.
inline u64 readBits(const u8* data, size_t dataSize, u32 offsetBits, u32 sizeBits) {
    if (sizeBits == 0 || sizeBits > 64) {
        return 0;
    }
    const size_t byteOffset = offsetBits >> 3;
    const u32 shift = offsetBits & 7;
    const u32 needed = (sizeBits + shift + 7) / 8;

    u64 low = 0;
    for (u32 i = 0; i < needed && i < 8; ++i) {
        if (byteOffset + i < dataSize) {
            low |= static_cast<u64>(data[byteOffset + i]) << (8 * i);
        }
    }
    u64 value = low >> shift;
    if (shift != 0 && needed > 8 && byteOffset + 8 < dataSize) {
        value |= static_cast<u64>(data[byteOffset + 8]) << (64 - shift);
    }
    if (sizeBits < 64) {
        value &= (static_cast<u64>(1) << sizeBits) - 1;
    }
    return value;
}

/// Reinterpret the low `bits` of `value` as a two's-complement signed integer.
inline i64 signExtend(u64 value, u32 bits) {
    if (bits == 0 || bits >= 64) {
        return static_cast<i64>(value);
    }
    const u64 sign = static_cast<u64>(1) << (bits - 1);
    return static_cast<i64>((value ^ sign) - sign);
}

/// Bounds-checked little-endian cursor over the whole file.
class ByteCursor {
public:
    ByteCursor(const u8* data, size_t size) : data_(data), size_(size) {}

    size_t position() const {
        return pos_;
    }
    size_t size() const {
        return size_;
    }
    bool ok() const {
        return ok_;
    }
    size_t remaining() const {
        return pos_ < size_ ? size_ - pos_ : 0;
    }

    void seek(size_t pos) {
        if (pos > size_) {
            ok_ = false;
            return;
        }
        pos_ = pos;
    }

    bool has(size_t bytes) const {
        return ok_ && bytes <= size_ - pos_;
    }

    template <typename T>
    T read() {
        static_assert(std::is_trivially_copyable_v<T>, "ByteCursor::read requires a POD");
        T value{};
        if (!has(sizeof(T))) {
            ok_ = false;
            return value;
        }
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    /// Read `count` values into a vector; empties the vector and fails the
    /// cursor when the range does not fit.
    template <typename T>
    std::vector<T> readVector(size_t count) {
        std::vector<T> out;
        if (count == 0) {
            return out;
        }
        if (!has(count * sizeof(T))) {
            ok_ = false;
            return out;
        }
        out.resize(count);
        std::memcpy(out.data(), data_ + pos_, count * sizeof(T));
        pos_ += count * sizeof(T);
        return out;
    }

    /// Read a fixed-width field holding a NUL-padded string.
    std::string readFixedString(size_t length) {
        std::string out;
        if (!has(length)) {
            ok_ = false;
            return out;
        }
        const char* start = reinterpret_cast<const char*>(data_ + pos_);
        size_t used = 0;
        while (used < length && start[used] != '\0') {
            ++used;
        }
        out.assign(start, used);
        pos_ += length;
        return out;
    }

    void skip(size_t bytes) {
        if (!has(bytes)) {
            ok_ = false;
            pos_ = size_;
            return;
        }
        pos_ += bytes;
    }

private:
    const u8* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    bool ok_ = true;
};

// ============================================================================
// Parsed structures
// ============================================================================

struct CopyEntry {
    u32 newId = 0;
    u32 sourceId = 0;
};

struct OffsetMapEntry {
    u32 offset = 0;
    u16 size = 0;
};

/// One section's worth of data, all offsets absolute within the file buffer.
struct SectionData {
    u64 tactKeyHash = 0;
    u32 fileOffset = 0;
    u32 recordCount = 0;
    u32 stringTableSize = 0;
    bool encrypted = false;

    u32 recordDataOffset = 0;
    u32 recordDataSize = 0;
    u32 stringDataOffset = 0;

    /// Position of this section's record data inside the virtual blob the
    /// client builds (all record data, then all string data). WDC2 and later
    /// express string references relative to that blob.
    u32 virtualRecordBase = 0;
    u32 virtualStringBase = 0;

    std::vector<u32> idList;
    std::vector<CopyEntry> copyTable;
    std::vector<OffsetMapEntry> offsetMap;
    std::vector<u32> offsetMapIds;
    std::vector<u32> encryptedIds;

    /// Foreign keys from the relationship map, keyed by record index.
    std::vector<u32> relationByRecord;
    /// Foreign keys keyed by record id (WDC4+ with TableFlags::RelationshipData).
    std::unordered_map<u32, u32> relationById;
    bool relationKeyedById = false;
};

/// One row: a record, plus the id it is exposed under.
struct RowEntry {
    u32 id = 0;
    /// Id used for common-data lookups. Differs from `id` on copy rows, which
    /// share every value with the record they duplicate.
    u32 lookupId = 0;
    u32 section = 0;
    u32 recordIndex = 0;
    u32 dataOffset = 0;
    u32 dataSize = 0;
    bool sparse = false;
    bool copy = false;
};

/// A schema column resolved against the file's layout.
struct BoundColumn {
    ColumnType type = ColumnType::Int32;
    u32 arraySize = 1;
    u32 slots = 1;
    u32 elementSize = 4;
    /// Index into TableData::fields when the file describes its fields.
    u32 fieldIndex = 0;
    /// Byte offset within the record, used when it does not (WDBC … WDB4)
    /// and when walking variable-length records.
    u32 byteOffset = 0;
    bool viaField = true;
    bool isRelation = false;
    bool isId = false;
    /// The column's value is the row id itself, because the file stores ids
    /// outside the record. Consumes no field and no record bytes.
    bool fromRowId = false;
};

// ============================================================================
// TableData
// ============================================================================

class TableData {
public:
    std::vector<u8> buffer;

    TableInfo info;
    std::vector<FieldInfo> fields;
    std::vector<SectionData> sections;
    std::vector<SectionInfo> sectionInfos;
    std::vector<RowEntry> rows;
    std::vector<u32> rowIds;
    std::unordered_map<u32, u32> idToRow;

    u32 palletDataOffset = 0;
    u32 palletDataSize = 0;
    u32 totalRecordDataSize = 0;

    /// Exception lists for FieldStorage::CommonData fields, one per field,
    /// sorted by record id.
    std::vector<std::vector<std::pair<u32, u32>>> commonMaps;

    std::optional<Schema> schema;
    std::vector<BoundColumn> bound;

    // -- Construction helpers (used by the readers) --

    /// Fill sectionInfos, the virtual blob bases and the row list.
    void finalize();

    // -- Decoding --

    const RowEntry* rowAt(u32 index) const {
        return index < rows.size() ? &rows[index] : nullptr;
    }

    std::span<const u8> recordBytes(const RowEntry& row) const;

    /// Raw field value, zero-extended. `signedValue` requests sign extension
    /// for bitpacked fields that carry it.
    u64 fieldValue(const RowEntry& row, u32 fieldIndex, u32 arrayIndex, bool signedValue) const;

    /// Byte offset of a field element within the record, for the fixed layout.
    u32 fieldByteOffset(u32 fieldIndex, u32 arrayIndex) const;

    /// Resolve a string reference read from `fieldByteOffset` of `row`.
    std::string_view resolveString(const RowEntry& row, u32 byteOffsetInRecord, u32 rawValue) const;

    /// Read a NUL-terminated string embedded in a record.
    std::string_view inlineString(const RowEntry& row, u32 byteOffsetInRecord) const;

    /// Byte offset of a bound column's element in a variable-length record,
    /// walking every preceding column. Returns false past the record's end.
    bool sparseColumnOffset(const RowEntry& row, u32 column, u32 arrayIndex, u32& byteOffset) const;

    Value columnValue(const RowEntry& row, u32 column, u32 arrayIndex) const;

    bool bindSchema(const Schema& newSchema, std::vector<std::string>* issues);

private:
    u64 commonDataValue(u32 fieldIndex, u32 id, u32 defaultValue) const;
    u64 palletValue(const FieldInfo& field, u32 index) const;
};

// ============================================================================
// Readers
// ============================================================================

/// Issue sink shared by the readers; mirrors textures::IssueSink.
struct IssueSink {
    std::vector<std::string>* issues = nullptr;

    bool fail(std::string message) {
        if (issues != nullptr) {
            issues->push_back(std::move(message));
        }
        return false;
    }

    void warn(std::string message) {
        if (issues != nullptr) {
            issues->push_back(std::move(message));
        }
    }
};

bool readWdbc(TableData& table, ByteCursor& cursor, IssueSink& sink);
bool readWdb(TableData& table, ByteCursor& cursor, IssueSink& sink);
bool readWdc(TableData& table, ByteCursor& cursor, IssueSink& sink);

/// Derive per-field array counts from the WDB5+ field structure block.
/// `positions` and `byteSizes` come straight from that block.
void deriveFieldLayout(TableData& table, const std::vector<u16>& positions,
                       const std::vector<u32>& byteSizes);

} // namespace whiteout::database

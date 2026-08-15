// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file table.h
 * @brief Parsed client database table and its row accessors.
 */

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/database/schema.h>
#include <whiteout/database/types.h>
#include <whiteout/database/value.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::database {

class Table;

/// Storage owned by a Table. Opaque to callers.
class TableData;

/// A lightweight view of one row. Valid for as long as its Table lives.
///
/// Field values are decoded on access, so iterating a table costs no
/// allocations. All accessors are bounds-checked and return zero (or an empty
/// string) rather than failing.
class Row {
public:
    Row() = default;

    bool valid() const noexcept {
        return data_ != nullptr;
    }
    explicit operator bool() const noexcept {
        return valid();
    }

    /// Record id.
    u32 id() const;

    /// Index of this row within its table.
    u32 index() const {
        return index_;
    }

    /// Index of the section this row's record lives in.
    u32 section() const;

    /// True when this row is a copy-table duplicate of another row: same
    /// values, different id.
    bool isCopy() const;

    /// True when the record is variable-length (offset-map table).
    bool isSparse() const;

    /// True when the row belongs to an encrypted section whose key is
    /// unavailable, so its field values are meaningless.
    bool isEncrypted() const;

    /// Foreign key from the file's relationship map, when present.
    std::optional<u32> relationId() const;

    /// The record's raw bytes.
    std::span<const u8> raw() const;

    // -- Raw field access. No schema required; the caller decides the type. --

    /// Zero-extended field value.
    u64 getUInt(u32 field, u32 arrayIndex = 0) const;

    /// Sign-extended field value.
    i64 getInt(u32 field, u32 arrayIndex = 0) const;

    /// Field value reinterpreted as a 32-bit float.
    f32 getFloat(u32 field, u32 arrayIndex = 0) const;

    /// Field value resolved as a reference into the string block. Empty when
    /// the reference is out of range.
    std::string_view getString(u32 field, u32 arrayIndex = 0) const;

    // -- Typed access. Requires a bound Schema; returns null otherwise. --

    Value value(u32 column, u32 arrayIndex = 0) const;
    Value value(std::string_view column, u32 arrayIndex = 0) const;

    /// One locale of a ColumnType::LocString column.
    std::string_view locString(std::string_view column, Locale locale = Locale::enUS) const;

private:
    friend class Table;
    Row(const TableData* data, u32 index) : data_(data), index_(index) {}

    const TableData* data_ = nullptr;
    u32 index_ = 0;
};

/// A parsed table: header, field layout, sections and rows.
///
/// Rows are numbered across all sections, with copy-table duplicates appended
/// after the physically stored records.
class Table {
public:
    Table();
    ~Table();

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    Table(Table&&) noexcept;
    Table& operator=(Table&&) noexcept;

    explicit operator bool() const noexcept {
        return data_ != nullptr;
    }

    const TableInfo& info() const;
    const std::vector<FieldInfo>& fields() const;
    const std::vector<SectionInfo>& sections() const;

    /// Rows in the table, including copy-table duplicates.
    size_t rowCount() const;

    /// Row by index; an invalid Row when out of range.
    Row row(size_t index) const;

    /// Row by record id.
    std::optional<Row> findById(u32 id) const;

    /// Every record id, in row order.
    const std::vector<u32>& ids() const;

    /// Attach a column layout for typed access.
    /// @param issues  If non-null, receives a description of every mismatch.
    /// @return True when the schema is consistent with the file's layout.
    ///         A schema that fails to bind is not attached.
    bool bind(const Schema& schema, std::vector<std::string>* issues = nullptr);

    /// Currently bound schema, or nullptr.
    const Schema* schema() const;

private:
    friend class Parser;

    std::unique_ptr<TableData> data_;
};

} // namespace whiteout::database

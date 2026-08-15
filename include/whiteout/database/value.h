// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file value.h
 * @brief Typed column value returned by schema-bound row access.
 */

#include <whiteout/common_types.h>
#include <whiteout/database/types.h>

#include <string_view>

namespace whiteout::database {

/// Declared type of a schema column.
enum class ColumnType {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    String,
    /// Pre-Cataclysm localized string: Schema::locStringSlots consecutive
    /// string references, one per locale, followed by a flags word.
    LocString,
};

/// Byte width of one element of @p type in a fixed-layout record.
/// LocString reports the width of a single slot; the column occupies
/// Schema::locStringSlots of them.
u32 columnTypeSize(ColumnType type);

/// Whether @p type is stored as a reference into the string block.
bool columnTypeIsString(ColumnType type);

/// A single column value, borrowed from the Table that produced it.
///
/// String values point into the table's string block, so a Value must not
/// outlive the Table it came from.
class Value {
public:
    enum class Kind { Null, Int, UInt, Float, String };

    Value() = default;

    static Value makeInt(i64 v);
    static Value makeUInt(u64 v);
    static Value makeFloat(f32 v);
    static Value makeString(std::string_view v);

    Kind kind() const noexcept {
        return kind_;
    }

    bool isNull() const noexcept {
        return kind_ == Kind::Null;
    }
    bool isInt() const noexcept {
        return kind_ == Kind::Int;
    }
    bool isUInt() const noexcept {
        return kind_ == Kind::UInt;
    }
    bool isFloat() const noexcept {
        return kind_ == Kind::Float;
    }
    bool isString() const noexcept {
        return kind_ == Kind::String;
    }

    /// Numeric conversions. Each converts across the numeric kinds and
    /// returns 0 for Null and String.
    i64 asInt() const;
    u64 asUInt() const;
    f32 asFloat() const;

    /// Empty unless this is a String value.
    std::string_view asString() const;

private:
    Kind kind_ = Kind::Null;
    union {
        i64 i;
        u64 u;
        f32 f;
    } num_ = {0};
    std::string_view str_;
};

} // namespace whiteout::database

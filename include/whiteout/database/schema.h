// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file schema.h
 * @brief Caller-supplied column layout used for typed row access.
 *
 * DBC and DB2 files record how wide each field is but never what it means:
 * a four-byte field may be an integer, a float or an offset into the string
 * block. Bind a Schema to a Table to get names and types.
 *
 * @example
 * @code
 * database::Schema schema("SpellName");
 * schema.addId("ID").add("Name_lang", database::ColumnType::String);
 *
 * std::vector<std::string> issues;
 * if (table.bind(schema, &issues)) {
 *     for (size_t i = 0; i < table.rowCount(); ++i) {
 *         auto row = table.row(i);
 *         printf("%u %s\n", row.id(), row.value("Name_lang").asString().data());
 *     }
 * }
 * @endcode
 */

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/database/types.h>
#include <whiteout/database/value.h>

#include <string>
#include <string_view>
#include <vector>

namespace whiteout::database {

/// One declared column.
struct Column {
    std::string name;
    ColumnType type = ColumnType::Int32;
    u32 arraySize = 1;       ///< Element count; 1 for scalars.
    bool isId = false;       ///< Holds the record id (may be a non-inline id).
    bool isRelation = false; ///< Supplied by the relationship map, not by the record.
};

/// An ordered list of columns describing one table.
///
/// For versions that carry a field structure block (WDB5 and later) the
/// columns are matched one-to-one against the file's fields. For WDBC through
/// WDB4 — where the file describes no fields at all — the schema alone
/// determines the record layout, and columns are laid out sequentially from
/// the start of the record.
struct Schema {
    std::string name;
    std::vector<Column> columns;

    /// String references per LocString column: 17 for Wrath and later,
    /// 9 for Vanilla and The Burning Crusade. Cataclysm dropped multi-locale
    /// columns entirely — use ColumnType::String there.
    u32 locStringSlots = 17;

    Schema() = default;
    explicit Schema(std::string tableName) : name(std::move(tableName)) {}

    /// Append a column.
    Schema& add(std::string columnName, ColumnType type, u32 arraySize = 1);

    /// Append the id column.
    Schema& addId(std::string columnName, ColumnType type = ColumnType::UInt32);

    /// Append a column whose values come from the file's relationship map.
    Schema& addRelation(std::string columnName, ColumnType type = ColumnType::UInt32);

    /// Look up a column by name, or nullptr when absent.
    const Column* find(std::string_view columnName) const;

    /// Index of a column by name.
    std::optional<u32> indexOf(std::string_view columnName) const;

    /// Number of string references a column occupies (locStringSlots for
    /// LocString columns, arraySize otherwise).
    u32 slotCount(const Column& column) const;
};

} // namespace whiteout::database

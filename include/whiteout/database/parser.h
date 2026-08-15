// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief Reader for every DBC and DB2 container version.
 *
 * Parsing is non-throwing: `parse()` returns std::nullopt on failure and
 * every problem encountered — including recoverable ones, such as a section
 * whose encryption key is unavailable — is collected in `getIssues()`.
 *
 * @example
 * @code
 * database::Parser parser;
 * auto table = parser.parse("DBFilesClient/SpellName.db2");
 * if (!table) {
 *     for (const auto& issue : parser.getIssues())
 *         std::cerr << issue << '\n';
 *     return;
 * }
 * std::cout << table->rowCount() << " rows\n";
 * @endcode
 */

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/database/table.h>

#include <span>
#include <string>
#include <vector>

namespace whiteout::database {

/// Identify a file's container version from its first four bytes.
Version detectVersion(std::span<const u8> buffer);

class Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse from memory. The buffer is copied into the returned Table.
    std::optional<Table> parse(std::span<const u8> buffer);

    /// Parse from memory, taking ownership of the buffer.
    std::optional<Table> parse(std::vector<u8>&& buffer);

    /// Parse a file from disk.
    std::optional<Table> parse(const std::string& filePath);

    bool hasIssues() const;
    const std::vector<std::string>& getIssues() const;

private:
    std::vector<std::string> issues_;
};

} // namespace whiteout::database

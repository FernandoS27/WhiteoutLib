// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file mdx_parser.h
 * @brief MDX file parser
 *
 * This file provides the Parser class for reading and parsing MDX model files.
 * The parser can handle multiple MDX versions from Classic (800) to Reforged (1200).
 *
 * @example Basic parsing
 * @code
 * mdx::Parser parser(mdx::Parser::ParseMode::Lenient);
 * mdx::Model model = parser.parse("model.mdx");
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cout << "Warning: " << issue << std::endl;
 *     }
 * }
 * @endcode
 */

#include <memory>
#include <string>
#include <vector>
#include "../compatibility.h"
#include "structures.h"
#include "types.h"

namespace whiteout {
namespace common {
class BinaryReader;
}

namespace mdx {

// Use BinaryReader from Common namespace
using common::BinaryReader;

// ============================================================================
// MDX Parser
// ============================================================================

/**
 * @brief Parser for MDX model files
 *
 * The Parser reads binary MDX files and converts them into the Model
 * structure. It supports multiple parsing modes and can handle version differences.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
class Parser {
public:
    /**
     * @brief Parsing strictness mode
     */
    enum class ParseMode {
        Strict, ///< Throw exceptions on unknown chunks or invalid data
        Lenient ///< Skip unknown chunks and try to recover from errors (recommended)
    };

    /**
     * @brief Version handling mode
     */
    enum class UpgradeMode {
        UpgradeOldVersions, ///< Automatically upgrade older versions to latest format
        PreserveOriginal    ///< Keep original version and structure as-is
    };

    /**
     * @brief Construct a new Parser
     * @param parseMode Strictness mode for parsing
     * @param upgradeMode How to handle version differences
     */
    Parser(ParseMode parseMode = ParseMode::Lenient,
           UpgradeMode upgradeMode = UpgradeMode::UpgradeOldVersions);

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Parser();

    /**
     * @brief Parse an MDX file from disk
     * @param filePath Path to the MDX file
     * @return Parsed MDX file data
     * @throws std::runtime_error If file cannot be opened or parsing fails in strict mode
     */
    Model parse(const std::string& filePath);

    /**
     * @brief Parse an MDX file from memory buffer
     * @param buffer Memory buffer containing MDX data
     * @return Parsed MDX file data
     * @throws std::runtime_error If parsing fails in strict mode
     */
    Model parse(std::span<const u8> buffer);

    /**
     * @brief Check if parsing encountered any issues
     * @return True if there were warnings or recoverable errors
     */
    bool hasIssues() const;

    /**
     * @brief Get list of issues encountered during parsing
     * @return Vector of issue description strings
     */
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace mdx
} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief M3 file parser
 *
 * This file provides the Parser class for reading and parsing M3 model files.
 * The parser handles binary M3 format used by StarCraft II and Heroes of the Storm.
 *
 * @example Basic parsing
 * @code
 * m3::Parser parser(m3::Parser::ParseMode::Lenient);
 * m3::Model model = parser.parse("model.m3");
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
#include "../../compatibility.h"
#include "structures.h"

namespace whiteout {
namespace common {
class BinaryReader;
}

namespace m3 {

// Use BinaryReader from Common namespace
using common::BinaryReader;

// ============================================================================
// M3 Parser
// ============================================================================

/**
 * @brief Parser for M3 model files
 *
 * The Parser reads binary M3 files and converts them into the Model
 * structure. It supports multiple parsing modes for error handling.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
/// @bind methods, js_name=M3Parser
class Parser {
public:
    /**
     * @brief Parsing strictness mode
     */
    /// @bind js_name=M3ParseMode
    enum class ParseMode {
        Strict, ///< Throw exceptions on invalid data or parsing errors
        Lenient ///< Skip problematic data and try to recover from errors (recommended)
    };

    /**
     * @brief Construct a new Parser
     * @param mode Strictness mode for parsing
     */
    explicit Parser(ParseMode mode = ParseMode::Lenient);

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Parser();

    /**
     * @brief Parse an M3 file from disk
     * @param filePath Path to the M3 file
     * @return Parsed M3 model data
     * @throws std::runtime_error If file cannot be opened or parsing fails in strict mode
     */
    Model parse(const std::string& filePath);

    /**
     * @brief Parse an M3 file from memory buffer
     * @param buffer Memory buffer containing M3 data
     * @return Parsed M3 model data
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

} // namespace m3
} // namespace whiteout

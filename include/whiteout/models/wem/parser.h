// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief WEM file parser
 *
 * This file provides the Parser class for reading and parsing WEM model files.
 * The parser handles the chunk-based WEM binary format.
 *
 * @example Basic parsing
 * @code
 * wem::Parser parser(wem::Parser::ParseMode::Lenient);
 * auto model = parser.parse("model.wem");
 *
 * if (model && parser.hasIssues()) {
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
namespace models {
namespace wem {

// ============================================================================
// WEM Parser
// ============================================================================

/**
 * @brief Parser for WEM model files
 *
 * The Parser reads binary WEM files and converts them into the Model structure.
 * It supports strict and lenient parsing modes.
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
        Lenient ///< Skip unknown chunks and try to recover from errors
    };

    /**
     * @brief Construct a new Parser
     * @param parseMode Strictness mode for parsing
     */
    explicit Parser(ParseMode parseMode = ParseMode::Lenient);

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Parser();

    /**
     * @brief Parse a WEM file from disk
     * @param filePath Path to the WEM file
     * @return Parsed model, or nullopt on failure in lenient mode
     * @throws std::runtime_error If file cannot be opened or parsing fails in strict mode
     */
    std::optional<Model> parse(const std::string& filePath);

    /**
     * @brief Parse a WEM file from memory buffer
     * @param buffer Memory buffer containing WEM data
     * @return Parsed model, or nullopt on failure in lenient mode
     * @throws std::runtime_error If parsing fails in strict mode
     */
    std::optional<Model> parse(std::span<const u8> buffer);

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

} // namespace wem
} // namespace models
} // namespace whiteout

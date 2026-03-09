// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief M2 file parser
 *
 * This file provides the Parser class for reading and parsing M2 model files.
 * The parser handles binary M2 format used by World of Warcraft, supporting both
 * classic (MD20) and modern (MD21) formats, including multi-file bundles.
 *
 * @example Basic parsing
 * @code
 * m2::Parser parser(m2::Parser::ParseMode::Lenient);
 * m2::FileSystem model = parser.parse("model.m2");
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cout << "Warning: " << issue << std::endl;
 *     }
 * }
 * @endcode
 */

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "../compatibility.h"
#include "structures.h"

namespace whiteout {
namespace common {
class BinaryReader;
}

namespace m2 {

// Use BinaryReader from Common namespace
using common::BinaryReader;

// ============================================================================
// M2 Parser
// ============================================================================

/**
 * @brief Parser for M2 model files
 *
 * The Parser reads binary M2 files and converts them into the FileSystem
 * structure. It supports both single-file and multi-file M2 bundles (with
 * separate skin, skeleton, bone, and animation files).
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
class Parser {
public:
    /**
     * @brief Parsing strictness mode
     */
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
     * @brief Parse an M2 file bundle from disk
     * @param filePath Path to the main M2 file (automatically finds associated files)
     * @return Parsed M2 FileSystem containing all model data
     * @throws std::runtime_error If file cannot be opened or parsing fails in strict mode
     */
    FileSystem parse(const std::string& filePath);

    /**
     * @brief Parse a specific M2 file type from memory buffer
     * @param buffer Memory buffer containing M2 data
     * @param fileSystem FileSystem to parse data into
     * @param fileType Type of M2 file being parsed (Base, Skin, Skeleton, etc.)
     * @throws std::runtime_error If parsing fails in strict mode
     */
    void parse(std::span<const uint8_t> buffer, FileSystem& fileSystem, FileType fileType);

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

} // namespace m2
} // namespace whiteout

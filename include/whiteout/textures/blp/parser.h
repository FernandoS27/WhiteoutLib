// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief BLP file parser
 *
 * This file provides the Parser class for reading and decoding BLP texture files.
 * The parser can handle both BLP1 (Warcraft III) and BLP2 (WoW) formats with
 * JPEG, palettized, DXT, and BGRA encodings.
 *
 * @example Basic parsing
 * @code
 * blp::Parser parser(blp::Parser::ParseMode::Lenient);
 * auto texture = parser.parse("texture.blp");
 *
 * if (parser.hasIssues()) {
 *     for (const auto& issue : parser.getIssues()) {
 *         std::cout << "Warning: " << issue << std::endl;
 *     }
 * }
 * @endcode
 */

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::blp {

// ============================================================================
// BLP Parser
// ============================================================================

/**
 * @brief Parser for BLP texture files
 *
 * The Parser reads binary BLP files and converts them into the Texture
 * structure. It supports multiple parsing modes and can handle both BLP1
 * (Warcraft III) and BLP2 (World of Warcraft) variants.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
class Parser {
public:
    /**
     * @brief Parsing strictness mode
     */
    enum class ParseMode {
        Strict, ///< Throw exceptions on invalid data
        Lenient ///< Try to recover from errors and log issues (recommended)
    };

    /**
     * @brief Construct a new Parser
     * @param parseMode Strictness mode for parsing
     */
    explicit Parser(ParseMode parseMode = ParseMode::Lenient);

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Parser();

    /**
     * @brief Parse a BLP file from disk
     * @param filePath Path to the BLP file
     * @return Parsed texture data, or std::nullopt on failure (in Lenient mode)
     * @throws std::runtime_error If file cannot be opened or parsing fails in strict mode
     */
    std::optional<Texture> parse(const std::string& filePath);

    /**
     * @brief Parse a BLP file from memory buffer
     * @param buffer Memory buffer containing BLP data
     * @return Parsed texture data, or std::nullopt on failure (in Lenient mode)
     * @throws std::runtime_error If parsing fails in strict mode
     */
    std::optional<Texture> parse(std::span<const u8> buffer);

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

} // namespace whiteout::textures::blp

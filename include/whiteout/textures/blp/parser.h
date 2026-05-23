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
 * blp::Parser parser;
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
#include <whiteout/textures/parser.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::blp {

// ============================================================================
// BLP Parser
// ============================================================================

/**
 * @brief Parser for BLP texture files
 *
 * The Parser reads binary BLP files and converts them into the Texture
 * structure. It can handle both BLP1 (Warcraft III) and BLP2 (World of
 * Warcraft) variants. Parsing is non-throwing — issues are collected via
 * `hasIssues()` / `getIssues()` and `parse()` returns `std::nullopt` on
 * failure.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
/// @bind methods=buffer_only, js_name=BlpParser
class Parser : public textures::Parser {
public:
    /// @brief Construct a new Parser.
    Parser();

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Parser();

    /**
     * @brief Parse a BLP file from disk
     * @param filePath Path to the BLP file
     * @return Parsed texture data, or std::nullopt on failure
     */
    std::optional<Texture> parse(const std::string& filePath) override;

    /**
     * @brief Parse a BLP file from memory buffer
     * @param buffer Memory buffer containing BLP data
     * @return Parsed texture data, or std::nullopt on failure
     * @throws std::runtime_error If parsing fails in strict mode
     */
    std::optional<Texture> parse(std::span<const u8> buffer) override;

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

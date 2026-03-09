// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief TEX file parser
 *
 * This file provides the Parser class for reading and decoding TEX texture files
 * (Diablo III SNO format).
 *
 * The parser supports two modes:
 * - **Strict** – any issue throws `std::runtime_error`.
 * - **Lenient** (default) – issues are collected and can be queried via
 *   `hasIssues()` / `getIssues()`.  On failure the parse methods return
 *   `std::nullopt`.
 */

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/tex/types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::tex {

// ============================================================================
// Parser
// ============================================================================

/// Reads a TEX file or byte buffer and decodes it into a Texture.
class Parser {
public:
    enum class ParseMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return nullopt on failure.
    };

    explicit Parser(ParseMode parseMode = ParseMode::Lenient);
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse a TEX file from disk.
    std::optional<Texture> parse(const std::string& filePath);

    /// Parse a TEX byte buffer.
    std::optional<Texture> parse(std::span<const u8> buffer);

    /// Parse a TEX byte buffer and extract metadata.
    std::optional<Texture> parse(std::span<const u8> buffer, TexInfo* outInfo);

    /// @return true if the last parse produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last parse call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::tex

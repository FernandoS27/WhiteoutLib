// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief PNG file parser
 *
 * This file provides the Parser class for reading and decoding PNG files.
 * It supports 8-bit and 16-bit truecolor, grayscale, indexed-color, and
 * alpha-channel images (non-interlaced).
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
#include <whiteout/textures/parser.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::png {

// ============================================================================
// Parser
// ============================================================================

/// Reads a PNG file or byte buffer and decodes it into a Texture.
/// @bind methods=buffer_only, js_name=PngParser
class Parser : public textures::Parser {
public:
    /// @bind js_name=PngParseMode
    enum class ParseMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return nullopt on failure.
    };

    explicit Parser(ParseMode parseMode = ParseMode::Lenient);
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse a PNG file from disk.
    std::optional<Texture> parse(const std::string& filePath) override;

    /// Parse a PNG byte buffer.
    std::optional<Texture> parse(std::span<const u8> buffer) override;

    /// @return true if the last parse produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last parse call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::png

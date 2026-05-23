// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief BMP file parser
 *
 * This file provides the Parser class for reading and decoding BMP bitmap files.
 * It supports 24-bit (RGB) and 32-bit (RGBA) uncompressed BMP images with
 * BITMAPINFOHEADER.
 *
 * Parsing is non-throwing. Issues are collected and can be queried via
 * `hasIssues()` / `getIssues()`; on failure the parse methods return
 * `std::nullopt`.
 */

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/parser.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::bmp {

// ============================================================================
// Parser
// ============================================================================

/// Reads a BMP file or byte buffer and decodes it into a Texture.
/// @bind methods=buffer_only, js_name=BmpParser
class Parser : public textures::Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse a BMP file from disk.
    std::optional<Texture> parse(const std::string& filePath) override;

    /// Parse a BMP byte buffer.
    std::optional<Texture> parse(std::span<const u8> buffer) override;

    /// @return true if the last parse produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last parse call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::bmp

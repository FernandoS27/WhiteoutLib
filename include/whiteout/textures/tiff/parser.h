// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief TIFF file parser.
 *
 * Reads classic TIFF 6.0 files (II / MM byte orders, magic 42). Supports a
 * pragmatic subset: 8/16-bit gray/RGB/RGBA, strips and tiles, uncompressed,
 * PackBits, Deflate, and LZW (with the TIFF 6.0 early-change quirk). See
 * docs/TIFF.md §1 for the full scope.
 *
 * Parsing is non-throwing. Issues are collected and queried via
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

namespace whiteout::textures::tiff {

/// Reads a TIFF file or byte buffer and decodes it into a Texture.
/// @bind methods=buffer_only, js_name=TiffParser
class Parser : public textures::Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse a TIFF file from disk.
    std::optional<Texture> parse(const std::string& filePath) override;

    /// Parse a TIFF byte buffer.
    std::optional<Texture> parse(std::span<const u8> buffer) override;

    /// @return true if the last parse produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last parse call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::tiff

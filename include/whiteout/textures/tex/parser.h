// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief TEX file parser
 *
 * This file provides the Parser class for reading and decoding TEX texture files
 * (Diablo III and Diablo IV SNO formats).
 *
 * D3 TEX files are monolithic — a single call to `parse()` returns the full
 * texture.  D4 TEX files separate metadata from pixel data, so `parse()`
 * takes two arguments: the .tex metadata and the pixel-data payload.
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
#include <whiteout/textures/tex/types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::tex {

// ============================================================================
// Parser
// ============================================================================

/// Reads a TEX file or byte buffer and decodes it into a Texture.
class Parser : public textures::Parser {
public:
    enum class ParseMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return nullopt on failure.
    };

    /// High-level TEX container kind inferred from the file header.
    enum class FileKind {
        Unknown,       ///< Not a recognized TEX container.
        Diablo3Tex,    ///< Diablo III monolithic TEX file.
        Diablo4MetaTex ///< Diablo IV metadata-only TEX SNO file.
    };

    explicit Parser(ParseMode parseMode = ParseMode::Lenient);
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse a TEX file from disk.
    std::optional<Texture> parse(const std::string& filePath) override;

    /// Parse a TEX byte buffer.
    std::optional<Texture> parse(std::span<const u8> buffer) override;

    /// Parse a TEX byte buffer and extract metadata.
    std::optional<Texture> parse(std::span<const u8> buffer, TexInfo* outInfo);

    /// Classify a TEX byte buffer as D3, D4-meta, or unknown.
    FileKind detectKind(std::span<const u8> buffer) const;

    /// Classify a TEX file on disk as D3, D4-meta, or unknown.
    FileKind detectKind(const std::string& filePath) const;

    // -- Diablo IV -----------------------------------------------------------

    /// Parse a D4 TEX file from two file paths (metadata .tex + pixel payload).
    std::optional<Texture> parse(const std::string& texFilePath,
                                 const std::string& payloadFilePath);

    /// Parse a D4 TEX file from two byte buffers (metadata + pixel payload).
    std::optional<Texture> parse(std::span<const u8> texData, std::span<const u8> payloadData);

    /// Parse a D4 TEX file from two byte buffers with metadata extraction.
    std::optional<Texture> parse(std::span<const u8> texData, std::span<const u8> payloadData,
                                 D4TexInfo* outInfo);

    /// Parse a D4 TEX with hi-res + low-res payloads from file paths.
    std::optional<Texture> parse(const std::string& texFilePath,
                                 const std::string& hiResPayloadFilePath,
                                 const std::string& lowResPayloadFilePath);

    /// Parse a D4 TEX with hi-res + low-res payload byte buffers.
    std::optional<Texture> parse(std::span<const u8> texData, std::span<const u8> hiResPayloadData,
                                 std::span<const u8> lowResPayloadData, D4TexInfo* outInfo);

    /// @return true if the last parse produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last parse call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::tex

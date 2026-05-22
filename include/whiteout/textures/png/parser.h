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
// APNG frame metadata
// ============================================================================

/// Per-frame metadata for an animated PNG (APNG).
/// @bind value_object, js_name=PngApngFrameInfo
struct ApngFrameInfo {
    u32 width = 0;     ///< Frame sub-rectangle width.
    u32 height = 0;    ///< Frame sub-rectangle height.
    u32 xOffset = 0;   ///< Frame sub-rectangle X offset on the canvas.
    u32 yOffset = 0;   ///< Frame sub-rectangle Y offset on the canvas.
    u32 delayMs = 0;   ///< Frame display duration in milliseconds.
    u32 disposeOp = 0; ///< 0 = NONE, 1 = BACKGROUND, 2 = PREVIOUS.
    u32 blendOp = 0;   ///< 0 = SOURCE, 1 = OVER.
};

// ============================================================================
// Parser
// ============================================================================

/// Reads a PNG file or byte buffer and decodes it into a Texture.
///
/// Animated PNG (APNG) is supported: `parse()` still returns the single
/// default image, while the animation frames are exposed via `isAnimated()`,
/// `frameCount()`, `frame()`, `frameDelayMs()` and `frameInfo()`.
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

    // ── APNG (Animated PNG) ─────────────────────────────────────────────

    /// @return true if the last parsed PNG carried APNG animation chunks.
    bool isAnimated() const;

    /// @return number of animation frames (0 when not animated).
    u32 frameCount() const;

    /// @return APNG loop count from the `acTL` chunk; 0 means loop forever.
    u32 loopCount() const;

    /// @return animation frame @p index, fully composited to the canvas size
    ///         as an RGBA8 texture. In lenient mode an out-of-range index
    ///         yields an empty texture; in strict mode it throws.
    /// @param index Zero-based frame index.
    const Texture& frame(u32 index) const;

    /// @return display duration of frame @p index in milliseconds.
    /// @param index Zero-based frame index.
    u32 frameDelayMs(u32 index) const;

    /// @return raw per-frame metadata for frame @p index.
    /// @param index Zero-based frame index.
    const ApngFrameInfo& frameInfo(u32 index) const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::png

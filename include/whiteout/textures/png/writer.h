// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief PNG file writer
 *
 * This file provides the Writer class for encoding Texture objects into PNG
 * format.  Outputs 32-bit RGBA or 24-bit RGB non-interlaced truecolor PNG.
 *
 * The writer supports two modes:
 * - **Strict** – any issue throws `std::runtime_error`.
 * - **Lenient** (default) – issues are collected and can be queried via
 *   `hasIssues()` / `getIssues()`.  On failure the write methods return
 *   an empty vector (or do nothing for the file overload).
 */

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/writer.h>

namespace whiteout::textures::png {

// ============================================================================
// APNG frame / options
// ============================================================================

/// One frame of an animated PNG (APNG), with its display duration.
/// @bind value_object, js_name=PngApngFrame
struct ApngFrame {
    Texture image;     ///< Full-canvas frame image (converted to RGBA8 on write).
    u32 delayMs = 100; ///< Display duration in milliseconds.
};

/// Options controlling animated PNG (APNG) encoding.
/// @bind value_object, js_name=PngApngSaveOptions
struct ApngSaveOptions {
    u32 loopCount = 0; ///< Number of times to loop; 0 means loop forever.
};

// ============================================================================
// Writer
// ============================================================================

/// Encodes a Texture into PNG format.
///
/// In addition to single-image PNG, the writer can emit an animated PNG
/// (APNG) from a sequence of frames via `writeAnimated()`. Each frame is
/// written full-canvas with no inter-frame optimisation.
/// @bind methods=buffer_only, js_name=PngWriter
class Writer : public textures::Writer {
public:
    /// @bind js_name=PngWriteMode
    enum class WriteMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return empty data on failure.
    };

    explicit Writer(WriteMode writeMode = WriteMode::Lenient);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Serialize the texture to a PNG file on disk.
    void write(const std::string& filePath, const Texture& texture) override;

    /// Serialize the texture to a PNG byte buffer.
    std::vector<u8> write(const Texture& texture) override;

    // ── APNG (Animated PNG) ─────────────────────────────────────────────

    /// Serialize a sequence of frames into an animated PNG (APNG) byte buffer.
    ///
    /// All frames must share the same dimensions (frame 0 defines the canvas).
    /// Each frame is emitted full-canvas with disposal NONE and blend SOURCE.
    /// Returns an empty buffer on failure (lenient mode).
    /// @param frames Ordered animation frames; must be non-empty.
    /// @param opts   Encoding options (loop count).
    std::vector<u8> writeAnimated(const std::vector<ApngFrame>& frames,
                                  const ApngSaveOptions& opts = {});

    /// Serialize a sequence of frames into an animated PNG (APNG) file on disk.
    /// @param filePath Destination path.
    /// @param frames   Ordered animation frames; must be non-empty.
    /// @param opts     Encoding options (loop count).
    void writeAnimated(const std::string& filePath, const std::vector<ApngFrame>& frames,
                       const ApngSaveOptions& opts = {});

    /// @return true if the last write produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last write call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::png

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief GIF89a file writer (animated and single-frame)
 *
 * Encodes one or more Texture frames into GIF89a format with a shared 256-color
 * palette produced by Wu's optimal quantizer.  All input frames must share the
 * same dimensions and pixel format, and the format must be uncompressed.
 *
 * Single-frame example:
 * @code
 *   std::vector<Texture> frames = { myTexture };
 *   gif::Writer writer;
 *   writer.write("output.gif", frames);
 * @endcode
 *
 * Animated example:
 * @code
 *   std::vector<Texture> frames = loadFrames();
 *   gif::Writer writer;
 *   auto bytes = writer.write(frames, gif::SaveOptions{.delayCs = 4});
 * @endcode
 */

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::textures::gif {

// ============================================================================
// Options
// ============================================================================

/// Per-write options for GIF encoding.
/// @bind value_object, js_name=GifSaveOptions
struct SaveOptions {
    /// Delay between frames in centiseconds (1/100 s).  0 = unspecified.
    u16 delayCs = 4;

    /// Number of times the animation should loop.  0 = loop forever.
    u16 loopCount = 0;

    /// Enable blue-noise ordered dithering when mapping pixels to the palette.
    bool dither = true;

    /// Dither strength in [0, 1].  0 = no visible dithering, 1 = full.
    f32 ditherStrength = 0.8f;

    /// Emit a transparent background. Pixels whose source alpha is below 50%
    /// become the GIF's transparent palette index; the rest are quantised
    /// normally. GIF transparency is 1-bit, so partially-covered (anti-
    /// aliased) edge pixels are forced fully opaque or fully transparent.
    bool transparent = false;
};

// ============================================================================
// Writer
// ============================================================================

/// Encodes a sequence of Texture frames into GIF89a format.
///
/// Unlike the single-image writers (BMP, TGA, …), this writer accepts a
/// vector of frames.  It does **not** inherit from `textures::Writer`.
/// @bind methods, js_name=GifWriter
class Writer {
public:
    /// @bind js_name=GifWriteMode
    enum class WriteMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return empty data on failure.
    };

    explicit Writer(WriteMode writeMode = WriteMode::Lenient,
                    interfaces::WorkerPool* pool = nullptr);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Write frames to a GIF file on disk using default options.
    void write(const std::string& filePath, const std::vector<Texture>& frames);

    /// Write frames to a GIF byte buffer using default options.
    std::vector<u8> write(const std::vector<Texture>& frames);

    /// Write frames to a GIF file on disk with explicit options.
    void write(const std::string& filePath, const std::vector<Texture>& frames,
               const SaveOptions& opts);

    /// Write frames to a GIF byte buffer with explicit options.
    std::vector<u8> write(const std::vector<Texture>& frames, const SaveOptions& opts);

    /// @return true if the last write produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last write call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::gif

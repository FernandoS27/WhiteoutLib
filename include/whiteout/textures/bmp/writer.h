// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief BMP file writer
 *
 * This file provides the Writer class for encoding Texture objects into BMP format.
 * Outputs 32-bit BGRA uncompressed BMP with BITMAPINFOHEADER.
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

namespace whiteout::textures::bmp {

// ============================================================================
// Writer
// ============================================================================

/// Encodes a Texture into BMP format.
/// @bind methods=buffer_only, js_name=BmpWriter
class Writer : public textures::Writer {
public:
    /// @bind js_name=BmpWriteMode
    enum class WriteMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return empty data on failure.
    };

    explicit Writer(WriteMode writeMode = WriteMode::Lenient);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Serialize the texture to a BMP file on disk.
    void write(const std::string& filePath, const Texture& texture) override;

    /// Serialize the texture to a BMP byte buffer.
    std::vector<u8> write(const Texture& texture) override;

    /// @return true if the last write produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last write call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::bmp

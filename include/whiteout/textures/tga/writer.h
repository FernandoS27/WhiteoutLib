// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief TGA file writer
 *
 * This file provides the Writer class for encoding Texture objects into TGA format.
 * Outputs uncompressed 32-bit BGRA TGA images.
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

namespace whiteout::textures::tga {

// ============================================================================
// Writer
// ============================================================================

/// Encodes a Texture into TGA format.
class Writer : public textures::Writer {
public:
    enum class WriteMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return empty data on failure.
    };

    explicit Writer(WriteMode writeMode = WriteMode::Lenient);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Serialize the texture to a TGA file on disk.
    void write(const std::string& filePath, const Texture& texture) override;

    /// Serialize the texture to a TGA byte buffer.
    std::vector<u8> write(const Texture& texture) override;

    /// @return true if the last write produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last write call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::tga

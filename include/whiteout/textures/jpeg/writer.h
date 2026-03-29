// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief JPEG file writer
 *
 * This file provides the Writer class for encoding Texture objects into
 * standard JPEG (JFIF) format with proper RGB-to-Y'CbCr colour conversion.
 * Outputs baseline sequential DCT (SOF0) images by default, with an
 * optional progressive (SOF2) mode.
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

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::textures::jpeg {

// ============================================================================
// Writer
// ============================================================================

/// Encodes a Texture into JPEG format.
class Writer : public textures::Writer {
public:
    enum class WriteMode {
        Strict, ///< Throw on any issue.
        Lenient ///< Collect issues, return empty data on failure.
    };

    explicit Writer(i32 quality = 75, WriteMode writeMode = WriteMode::Lenient,
                    interfaces::WorkerPool* pool = nullptr, bool progressive = false);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Serialize the texture to a JPEG file on disk.
    void write(const std::string& filePath, const Texture& texture) override;

    /// Serialize the texture to a JPEG byte buffer.
    std::vector<u8> write(const Texture& texture) override;

    /// @return true if the last write produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last write call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::jpeg

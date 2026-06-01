// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief TIFF file writer.
 *
 * Emits classic TIFF 6.0, little-endian, single-IFD. Configurable per-write
 * compression (None / Deflate / PackBits). Input textures are coerced to
 * RGBA8 via Texture::copyAsFormat before emission (same simplification as
 * the BMP writer).
 */

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/writer.h>

namespace whiteout::textures::tiff {

/// Encodes a Texture into TIFF format.
/// @bind methods=buffer_only, js_name=TiffWriter
class Writer : public textures::Writer {
public:
    Writer();
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Serialize the texture to a TIFF file on disk.
    void write(const std::string& filePath, const Texture& texture) override;

    /// Serialize the texture to a TIFF byte buffer.
    std::vector<u8> write(const Texture& texture) override;

    /// @return true if the last write produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last write call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::tiff

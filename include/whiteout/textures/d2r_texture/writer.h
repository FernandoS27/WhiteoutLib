// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief Writer for Diablo II: Resurrected `.texture` files.
 *
 * Writing is non-throwing. Issues are collected and can be queried via
 * `hasIssues()` / `getIssues()`; on failure the write methods return an empty
 * vector (or do nothing for the file overload).
 */

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/d2r_texture/types.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/writer.h>

namespace whiteout::textures::d2r_texture {

/// Encodes a Texture into the Diablo II: Resurrected `.texture` format.
class Writer : public textures::Writer {
public:
    Writer();
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Serialize the texture to a file on disk (default options).
    void write(const std::string& filePath, const Texture& texture) override;

    /// Serialize the texture to a byte buffer (default options).
    std::vector<u8> write(const Texture& texture) override;

    /// Serialize the texture to a file on disk.
    void write(const std::string& filePath, const Texture& texture, const SaveOptions& opts);

    /// Serialize the texture to a byte buffer.
    std::vector<u8> write(const Texture& texture, const SaveOptions& opts);

    /// @return true if the last write produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last write call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::d2r_texture

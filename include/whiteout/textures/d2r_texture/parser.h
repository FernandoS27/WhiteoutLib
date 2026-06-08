// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief Parser for Diablo II: Resurrected `.texture` files.
 *
 * Parsing is non-throwing. Issues are collected and can be queried via
 * `hasIssues()` / `getIssues()`; on failure the parse methods return
 * `std::nullopt`.
 */

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/d2r_texture/types.h>
#include <whiteout/textures/parser.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::d2r_texture {

/// Reads a Diablo II: Resurrected `.texture` file and decodes it into a Texture.
class Parser : public textures::Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse a `.texture` file from disk.
    std::optional<Texture> parse(const std::string& filePath) override;

    /// Parse a `.texture` byte buffer.
    std::optional<Texture> parse(std::span<const u8> buffer) override;

    /// Parse a `.texture` byte buffer and extract metadata.
    std::optional<Texture> parse(std::span<const u8> buffer, D2rTextureInfo* outInfo);

    /// @return true if @p buffer has a valid `.texture` header and known format.
    bool detect(std::span<const u8> buffer) const;

    /// @return true if the last parse produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last parse call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::d2r_texture

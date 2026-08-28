// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief Parser for Overwatch TXTR textures (`004` header + `04D` payloads).
 *
 * A texture's pixel data is spread across the header's own inline block and up
 * to three payload files.  The header names none of them: use payloadGuids()
 * to derive the payload GUIDs from the texture's GUID, fetch those files from
 * the storage, and hand the buffers back to parse().
 *
 * Payload buffers may be supplied in any order and the set may be incomplete.
 * Each payload declares the mip range it covers, so the parser places them by
 * that range rather than by position.  When the largest mips are missing the
 * result is the smaller texture that the available mips do describe, with
 * TxtrInfo::baseMip recording how many levels were dropped.
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
#include <whiteout/textures/parser.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/txtr/types.h>

namespace whiteout::textures::txtr {

/// Reads an Overwatch TXTR header, plus any payloads supplied with it, and
/// decodes the result into a Texture.
class Parser : public textures::Parser {
public:
    Parser();
    ~Parser();

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    /// Parse a `004` header file from disk, without payloads.
    std::optional<Texture> parse(const std::string& filePath) override;

    /// Parse a `004` header buffer, without payloads.
    std::optional<Texture> parse(std::span<const u8> header) override;

    /// Parse a `004` header buffer, without payloads, and extract metadata.
    std::optional<Texture> parse(std::span<const u8> header, TxtrInfo* outInfo);

    /// Parse a `004` header together with its `04D` payload buffers.
    /// @param header   The texture header file.
    /// @param payloads Payload files in any order; each is placed by the mip
    ///                 range it declares. Empty entries are ignored.
    /// @param outInfo  Optional destination for the parsed metadata.
    std::optional<Texture> parse(std::span<const u8> header,
                                 std::span<const std::span<const u8>> payloads,
                                 TxtrInfo* outInfo = nullptr);

    /// Parse a `004` header together with its `04D` payloads, all from disk.
    std::optional<Texture> parse(const std::string& headerPath,
                                 const std::vector<std::string>& payloadPaths,
                                 TxtrInfo* outInfo = nullptr);

    /// Read a header's declared payload chain length without decoding pixels.
    /// @return the value at header offset 0x06, or 0 when @p header is too
    ///         small to hold one.
    static u32 payloadCount(std::span<const u8> header);

    /// Derive the GUIDs of the external payloads @p textureGuid needs.
    ///
    /// Payload 0 lives inside the header and is skipped, so the result holds
    /// `payloadCount(header) - 1` entries and is empty for a self-contained
    /// texture.
    static std::vector<u64> payloadGuids(std::span<const u8> header, u64 textureGuid);

    /// @return true if @p buffer is structurally consistent with a TXTR header
    ///         in a format this parser can decode.
    bool detect(std::span<const u8> buffer) const;

    /// @return true if the last parse produced any issues.
    bool hasIssues() const;

    /// @return accumulated issues from the last parse call.
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::txtr

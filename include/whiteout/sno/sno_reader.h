// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <string>
#include <unordered_map>

namespace whiteout {
namespace sno {

class SnoTypeRegistry;

/// Result of parsing an SNO file.
struct SnoFile {
    u32 dwSignature;      ///< Always 0xDEADBEEF
    u32 formatHash;       ///< Format hash (identifies the root type)
    i32 snoId;            ///< SNO identifier
    std::string typeName; ///< Root type name (e.g. "TextureDefinition")
    SnoValue root;        ///< Deserialized data tree
};

/// Deserialiser for Diablo III / IV SNO files.
///
/// Given the raw bytes of a `.tex`, `.acr`, `.mat`, etc. file (read from CASC
/// or disk), this reader validates the header, resolves the root type from the
/// compiled type registry, and recursively deserialises into a generic
/// `SnoValue` tree.  D3 files are detected automatically when the D4 format-
/// hash lookup fails and a matching D3 group definition exists.
///
/// Usage:
/// @code
///   SnoReader reader;
///   auto file = reader.parse(rawBytes);
///   if (file) {
///       auto* width = file->root.field("dwWidth");
///   }
/// @endcode
class SnoReader {
public:
    SnoReader();

    /// Provide per-group format hashes from the CoreTOC (used when a file's
    /// own format hash is zero).
    void setFormatHashes(const std::unordered_map<i32, u32>& hashes);

    /// Return the per-group format hash table.
    const std::unordered_map<i32, u32>& groupFormatHashes() const {
        return m_groupFormatHashes;
    }

    /// Parse an SNO file from raw bytes.
    /// @returns The parsed file, or std::nullopt on error.
    std::optional<SnoFile> parse(std::span<const u8> data) const;

    /// Parse with an explicit SnoGroup hint (used to resolve format hash == 0).
    std::optional<SnoFile> parse(std::span<const u8> data, SnoGroup group) const;

    /// Parse with an explicit SnoGroup hint and external payload data.
    /// Some D4 SNO groups store variable-array data in a separate payload file
    /// (`Base\payloads\...`). When provided, external data fields are resolved
    /// from this buffer instead of being emitted as `__external__` markers.
    std::optional<SnoFile> parse(std::span<const u8> data, SnoGroup group,
                                 std::span<const u8> payloadData) const;

private:
    /// Internal: parse a Diablo IV SNO file with resolved root type.
    std::optional<SnoFile> parseD4(std::span<const u8> data, u32 magic, u32 formatHash,
                                   u32 rootTypeHash, std::span<const u8> payloadData) const;
    /// Internal: parse a Diablo III SNO file (called automatically by parse()
    /// when D4 format-hash lookup fails and a D3 group definition exists).
    std::optional<SnoFile> parseD3(std::span<const u8> data, SnoGroup group) const;
    const SnoTypeRegistry& m_registry;
    const SnoTypeRegistry& m_d3Registry;
    std::unordered_map<i32, u32> m_groupFormatHashes;
};

} // namespace sno
} // namespace whiteout

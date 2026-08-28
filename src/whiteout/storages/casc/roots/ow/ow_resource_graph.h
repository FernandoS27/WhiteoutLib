// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file ow_resource_graph.h
/// @brief Overwatch resource graph (TRG) parser.
///
/// A TRG sits beside the CMFs in the text root and says how assets relate,
/// where a CMF only says where they live. Two of its four blocks are read here:
///
///   packages  what the client loads as a unit
///   skins     substitution lists — "when this skin is on, use B wherever the
///             base asset is A", which is what ties a hero's textures, models
///             and effects to that hero rather than to the pile
///
/// The graph and type-bundle-index blocks are skipped. They are located and
/// their sizes checked, because that is what proves the rest was read
/// correctly, but nothing here interprets them.
///
/// Layouts come from the client's own reader; see docs/OW_CMF_ENCRYPTION.md.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace whiteout::storages::casc::ow {

/// Bytes of TRG header, ahead of the four blocks.
inline constexpr size_t kTrgHeaderSize = 88;

struct TrgHeader {
    u32 buildVersion = 0;
    u32 packageCount = 0;
    u32 packageBytes = 0;
    u32 skinCount = 0;
    u32 skinBytes = 0;
    u32 graphBytes = 0;
    u32 typeBundleIndexBytes = 0;
    u32 magic = 0;
    bool patch = false;
    bool encrypted = false;
};

/// One package. Only the GUID is interpreted; reaching the asset list behind
/// `graphRef` means reading the graph block, which this does not do.
struct TrgPackage {
    u64 guid = 0;
    u64 hash = 0;
    u32 graphRef = 0;
    u32 field14 = 0;
    u32 field18 = 0;
    u16 field1c = 0;
};

/// One asset a skin swaps out.
struct TrgSkinEntry {
    u64 source = 0;
    u64 target = 0;
};

struct TrgSkin {
    u64 guid = 0;
    u32 graphRef = 0;
    std::vector<TrgSkinEntry> entries;
};

struct ResourceGraph {
    TrgHeader header;
    std::vector<TrgPackage> packages;
    std::vector<TrgSkin> skins;
};

/// Read a TRG header. Cheap enough to call before deciding to decrypt.
/// @return false when @p data is too short or carries an unrecognised magic.
bool parseTrgHeader(std::span<const u8> data, TrgHeader& out);

/// Parse a whole resource graph, decrypting the body in place when needed.
///
/// @param data Complete TRG file. Modified in place if it is encrypted.
/// @param name Manifest name as the root lists it, with extension and without
///             any directory — its SHA-1 is the IV material.
/// @return nullopt when the header is unreadable, no provider covers the build,
///         or the blocks do not account for the file exactly. That last check
///         is what rejects a wrong decryption key: the skin block is a chain of
///         variable-length records, so it only lands on its own end if every
///         length along the way was right.
std::optional<ResourceGraph> parseResourceGraph(std::span<u8> data, std::string_view name);

} // namespace whiteout::storages::casc::ow

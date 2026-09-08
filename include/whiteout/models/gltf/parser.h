// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief `gltf::Parser` — `.gltf` and `.glb` to `gltf::Asset` (GLTF_DESIGN §10).
 *
 * The parser owns everything between bytes and the decoded struct: the GLB
 * container framing, the JSON body, data: URIs, and accessor decode (component
 * types, strides, normalization, sparse substitution). It touches no
 * filesystem — external `.bin`/image URIs go through the caller's resolver or
 * stay unresolved, with the buffer's `data` empty and a warning recorded.
 *
 * Failure is a message, not an exception; a file that parses with losses
 * (an unknown extension, a dropped morph target) succeeds and says so in
 * `warnings`, matching the library's diagnostics posture.
 */

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "gltf.h"

namespace whiteout {
namespace models {
namespace gltf {

struct ParseOptions {
    /// Resolves an external (non-data:) URI to bytes. Null or returning false
    /// leaves the buffer/image unresolved — a warning, not a failure.
    std::function<bool(const std::string& uri, std::vector<u8>& out)> resolveUri;
};

struct ParseOutcome {
    std::optional<Asset> asset;
    std::string error; ///< Set exactly when `asset` is empty.
    std::vector<std::string> warnings;

    bool ok() const {
        return asset.has_value();
    }
};

class Parser {
public:
    /// Sniffs the GLB magic; anything else is taken as JSON text.
    static ParseOutcome FromBytes(std::span<const u8> bytes, const ParseOptions& options = {});

    /// JSON text plus an optional out-of-band BIN chunk (the GLB path feeds
    /// the chunk it found; a `.gltf` caller passes none).
    static ParseOutcome FromJsonText(std::string_view text, std::span<const u8> binChunk = {},
                                     const ParseOptions& options = {});

    static bool LooksLikeGlb(std::span<const u8> bytes);
};

// ============================================================================
// Accessor decode
// ============================================================================

/**
 * @brief Accessor @p index as unpacked floats, `count * components` of them.
 *
 * Integer component types decode per the `normalized` flag (scaled to [0,1] /
 * [-1,1], the spec's rules) or as plain casts when unnormalized. Sparse
 * substitution is applied. False when the accessor or its views are out of
 * range or the backing buffer has no bytes.
 */
bool ReadAccessorF32(const Asset& asset, u32 index, std::vector<f32>& out);

/// The same accessor as zero-extended integers — indices, joints. False for
/// float-typed accessors.
bool ReadAccessorU32(const Asset& asset, u32 index, std::vector<u32>& out);

/// Decodes a base64 payload (the data: URI body). False on any non-alphabet
/// byte or truncated tail.
bool DecodeBase64(std::string_view text, std::vector<u8>& out);

} // namespace gltf
} // namespace models
} // namespace whiteout

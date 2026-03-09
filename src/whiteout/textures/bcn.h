// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bcn.h
/// @brief Unified BCn block-compression interface.
///
/// Provides format-agnostic encode/decode dispatchers that route to the
/// appropriate BC1–BC7 codec based on the PixelFormat.  This is the
/// recommended entry-point for callers that do not need block-level or
/// image-level control — use the per-format headers in
/// `src/whiteout/textures/bcn/` for fine-grained access.

#pragma once

#include <optional>
#include <string>

#include <whiteout/textures/texture.h>

namespace whiteout::textures::bcn {

// ============================================================================
// Format queries
// ============================================================================

/// Returns true if @p fmt is a BCn block-compressed format (BC1–BC7).
constexpr bool isCompressed(PixelFormat fmt) {
    return fmt == PixelFormat::BC1 || fmt == PixelFormat::BC2 || fmt == PixelFormat::BC3 ||
           fmt == PixelFormat::BC4 || fmt == PixelFormat::BC5 || fmt == PixelFormat::BC6H ||
           fmt == PixelFormat::BC7;
}

/// Returns the uncompressed pixel format that a BCn format decodes to.
/// BC6H → RGBA16F; all other BCn → RGBA8.
/// Returns @p fmt unchanged if it is not a BCn format.
constexpr PixelFormat decoded_format(PixelFormat fmt) {
    if (fmt == PixelFormat::BC6H)
        return PixelFormat::RGBA16F;
    if (isCompressed(fmt))
        return PixelFormat::RGBA8;
    return fmt;
}

/// Returns the BCn format(s) that can encode a given uncompressed format.
/// RGBA8  can encode to BC1, BC2, BC3, BC4, BC5, BC7.
/// RGBA16F / RGBA32F can encode to BC6H.
/// Returns true if @p source can be encoded to @p target.
constexpr bool can_encode(PixelFormat source, PixelFormat target) {
    if (!isCompressed(target))
        return false;
    if (target == PixelFormat::BC6H)
        return source == PixelFormat::RGBA16F || source == PixelFormat::RGBA32F;
    return source == PixelFormat::RGBA8;
}

// ============================================================================
// Texture-level encode / decode
// ============================================================================

/// Encode an uncompressed texture to a BCn format.
///
/// @param src      Source texture (RGBA8 for BC1–BC5/BC7, RGBA16F or RGBA32F
///                 for BC6H).
/// @param target   The target BCn pixel format.
/// @param out_error  Optional pointer to receive an error description on
///                   failure.
/// @return The compressed texture, or std::nullopt on error.
///
/// @note  Format-specific options (BC1 punch-through alpha, BC4 channel
///        selection, BC5 channel pair, BC7 quality) use their defaults
///        when going through this interface.  For full control, use the
///        per-format encode headers directly.
std::optional<Texture> encode(const Texture& src, PixelFormat target,
                              std::string* out_error = nullptr);

/// Decode a BCn-compressed texture to its uncompressed representation.
///
/// The source format is read from @p src.format() and must be one of BC1–BC7.
///
/// @param src       Source compressed texture.
/// @param out_error Optional pointer to receive an error description.
/// @return The decoded texture (RGBA8 or RGBA16F), or std::nullopt on error.
std::optional<Texture> decode(const Texture& src, std::string* out_error = nullptr);

} // namespace whiteout::textures::bcn

namespace whiteout {
namespace bcn = textures::bcn;
}

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
/// BC4 → R8; BC5 → RG8; BC6H → RGBA32F; all other BCn → RGBA8.
/// Returns @p fmt unchanged if it is not a BCn format.
constexpr PixelFormat decoded_format(PixelFormat fmt) {
    if (fmt == PixelFormat::BC4)
        return PixelFormat::R8;
    if (fmt == PixelFormat::BC5)
        return PixelFormat::RG8;
    if (fmt == PixelFormat::BC6H)
        return PixelFormat::RGBA32F;
    if (isCompressed(fmt))
        return PixelFormat::RGBA8;
    return fmt;
}

/// Returns true if @p source can be directly encoded to @p target.
/// R8 → BC4; RG8 → BC5; RGBA8 → BC1/BC2/BC3/BC7; RGBA32F → BC6H.
constexpr bool can_encode(PixelFormat source, PixelFormat target) {
    if (!isCompressed(target))
        return false;
    if (target == PixelFormat::BC4)
        return source == PixelFormat::R8;
    if (target == PixelFormat::BC5)
        return source == PixelFormat::RG8;
    if (target == PixelFormat::BC6H)
        return source == PixelFormat::RGBA32F;
    return source == PixelFormat::RGBA8;
}

// ============================================================================
// Texture-level encode / decode
// ============================================================================

/// Encode an uncompressed texture to a BCn format.
///
/// @param src      Source texture (R8 for BC4, RG8 for BC5, RGBA8 for
///                 BC1/BC2/BC3/BC7, RGBA32F for BC6H).
/// @param target   The target BCn pixel format.
/// @param out_error  Optional pointer to receive an error description on
///                   failure.
/// @param thread_count  Number of threads (1 = serial, 0 = auto, >1 = that
///                      many threads).  Work is split into 64×64-pixel tiles.
/// @return The compressed texture, or std::nullopt on error.
///
/// @note  For full control, use the per-format encode headers directly.
std::optional<Texture> encode(const Texture& src, PixelFormat target,
                              std::string* out_error = nullptr, u32 thread_count = 1);

/// Decode a BCn-compressed texture to its uncompressed representation.
///
/// The source format is read from @p src.format() and must be one of BC1–BC7.
///
/// @param src       Source compressed texture.
/// @param out_error Optional pointer to receive an error description.
/// @param thread_count  Number of threads (1 = serial, 0 = auto, >1 = that
///                      many threads).  Work is split into 64×64-pixel tiles.
/// @return The decoded texture (RGBA8 or RGBA32F), or std::nullopt on error.
std::optional<Texture> decode(const Texture& src, std::string* out_error = nullptr,
                              u32 thread_count = 1);

} // namespace whiteout::textures::bcn

namespace whiteout {
namespace bcn = textures::bcn;
}

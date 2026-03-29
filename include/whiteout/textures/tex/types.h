// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Type definitions and save options for the TEX (Diablo III SNO) texture format
 *
 * This file defines:
 * - TexFlags bitmask controlling texture behaviour (flip-book, tiling)
 * - TexFrame describing a single flip-book sub-rectangle
 * - TexInfo holding parsed metadata (SNO ID, flags, sampler hints, frames)
 * - SaveOptions controlling how a Texture is serialised into the TEX container
 */

#include <array>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::textures::tex {

// ============================================================================
// Enumerations
// ============================================================================

/// Bitmask flags stored in the TEX header.
enum class TexFlags : u32 {
    None = 0x00,     ///< No special flags.
    Flipbook = 0x01, ///< Texture contains flip-book animation frames.
    Tileable = 0x02, ///< Texture is seamlessly tileable (wrap addressing).
    Flag04 = 0x04,   ///< Reserved / unknown flag bit 2.
};

/// Bitwise OR for TexFlags.
inline TexFlags operator|(TexFlags a, TexFlags b) {
    return static_cast<TexFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

/// Bitwise AND for TexFlags.
inline TexFlags operator&(TexFlags a, TexFlags b) {
    return static_cast<TexFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}

// ============================================================================
// Flip-book Frame
// ============================================================================

/// Describes one frame of a flip-book animation within the texture atlas.
struct TexFrame {
    f32 uMin;         ///< Left UV coordinate of the frame rectangle.
    f32 vMin;         ///< Top UV coordinate of the frame rectangle.
    f32 uMax;         ///< Right UV coordinate of the frame rectangle.
    f32 vMax;         ///< Bottom UV coordinate of the frame rectangle.
    std::string name; ///< Optional frame name (may be empty).
};

// ============================================================================
// Parsed Metadata
// ============================================================================

/// Metadata extracted from a TEX file by the parser.
struct TexInfo {
    u32 snoId;                    ///< Diablo III SNO identifier for this texture asset.
    TexFlags flags;               ///< Bitmask flags (flip-book, tileable, etc.).
    u32 samplerHint1;             ///< First engine sampler / filtering hint.
    u32 samplerHint2;             ///< Second engine sampler / filtering hint.
    std::vector<TexFrame> frames; ///< Flip-book frames (empty when Flipbook flag is unset).
};

// ============================================================================
// Save Options
// ============================================================================

/// Controls how a Texture is serialised into the TEX container.
struct SaveOptions {
    u32 snoId;                    ///< SNO identifier to write into the header.
    TexFlags flags;               ///< Bitmask flags for the header.
    u32 samplerHint1;             ///< First sampler hint value.
    u32 samplerHint2;             ///< Second sampler hint value.
    std::vector<TexFrame> frames; ///< Flip-book frames to embed (may be empty).
};

// ============================================================================
// Diablo IV Types
// ============================================================================

/// A single frame/sprite within a D4 texture atlas.
struct D4TexFrame {
    u32 imageHandle; ///< GBID hash for this frame (0 = unnamed).
    f32 u0;          ///< Left UV.
    f32 v0;          ///< Top UV.
    f32 u1;          ///< Right UV.
    f32 v1;          ///< Bottom UV.
    f32 trimU0;      ///< Trimmed content left UV.
    f32 trimV0;      ///< Trimmed content top UV.
    f32 trimU1;      ///< Trimmed content right UV.
    f32 trimV1;      ///< Trimmed content bottom UV.
};

/// Metadata extracted from a Diablo IV TEX file by the parser.
struct D4TexInfo {
    i32 snoId;                      ///< SNO identifier.
    u32 texFormat;                  ///< Raw eTexFormat value.
    u32 width;                      ///< Full-resolution width.
    u32 height;                     ///< Full-resolution height.
    u32 depth;                      ///< Texture depth (1 for 2D).
    u32 faceCount;                  ///< 1 for 2D, 6 for cubemaps.
    u32 mipMapLevelMin;             ///< Smallest stored mip index.
    u32 mipMapLevelMax;             ///< Largest stored mip index.
    u32 importFlags;                ///< Import flag bitfield.
    u32 textureResourceType;        ///< 0 = standard, 1 = cubemap probe.
    std::array<f32, 4> avgColor;    ///< Average linear RGBA colour.
    std::vector<D4TexFrame> frames; ///< Atlas frame entries.
    bool isTwoTier;                 ///< True when pixel data uses two-tier streaming.
    u32 hiResMipCount;              ///< Number of mip levels in the hi-res payload.
    u32 lowResMipCount; ///< Number of mip levels in the low-res payload (0 if single-tier).
};

} // namespace whiteout::textures::tex

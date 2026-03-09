// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file texture.h
 * @brief Format-agnostic GPU texture container and pixel-format utilities
 *
 * This file defines:
 * - PixelFormat enumeration covering uncompressed (RGBA8/16F/32F) and
 *   block-compressed (BC1–BC7) pixel encodings
 * - Utility functions for computing block sizes, image byte sizes, and mip counts
 * - TextureType (2D, 3D, Cube)
 * - MipLevel descriptor (width, height, depth, byte offset, byte size)
 * - Texture class (PImpl) with factory constructors, mip-chain management,
 *   raw data access, and in-place / copying format conversion
 *
 * Texture is the central interchange type shared by every format-specific
 * parser and writer in the library (BLP, DDS, TEX).
 */

#include <memory>
#include <vector>

#include <whiteout/common_types.h>
#include "../compatibility.h"

namespace whiteout {
namespace textures {

// ============================================================================
// Pixel Formats
// ============================================================================

/// GPU pixel / block-compression format.
///
/// Uncompressed formats store one pixel per "block"; BCn formats store a
/// 4×4 pixel tile per block.
enum class PixelFormat : u32 {
    RGBA8,   ///< 8-bit RGBA (4 bytes per pixel).
    RGBA16F, ///< Half-precision RGBA (8 bytes per pixel).
    RGBA32F, ///< Single-precision RGBA (16 bytes per pixel).
    BC1,     ///< DXT1 – 8 bytes per 4×4 block (RGB + optional 1-bit alpha).
    BC2,     ///< DXT3 – 16 bytes per 4×4 block (explicit 4-bit alpha).
    BC3,     ///< DXT5 – 16 bytes per 4×4 block (interpolated alpha).
    BC4,     ///< Single-channel – 8 bytes per 4×4 block.
    BC5,     ///< Dual-channel – 16 bytes per 4×4 block.
    BC6H,    ///< HDR RGB – 16 bytes per 4×4 block (half-float output).
    BC7,     ///< High-quality RGBA – 16 bytes per 4×4 block.
};

// ============================================================================
// Pixel-Format Utilities
// ============================================================================

/// Return the byte size of one block (or one pixel for uncompressed formats).
/// @param fmt Pixel format to query.
u32 bytesPerBlock(PixelFormat fmt);

/// Return the edge length (in pixels) of one block.
/// Uncompressed formats return 1; all BCn formats return 4.
/// @param fmt Pixel format to query.
u32 blockEdge(PixelFormat fmt);

/// Compute the byte size of a single 2D image slice.
/// Formula: ceil(width / edge) × ceil(height / edge) × bytesPerBlock.
/// @param fmt    Pixel format.
/// @param width  Image width in pixels.
/// @param height Image height in pixels.
u64 computeImageSize(PixelFormat fmt, u32 width, u32 height);

/// Return the maximum number of mip levels for the given dimensions.
/// Takes the largest dimension and counts halvings down to 1 (inclusive).
/// @param w Base width.
/// @param h Base height.
/// @param d Base depth (default 1 for 2D / cube textures).
u32 computeMaxMipCount(u32 w, u32 h, u32 d = 1);

// ============================================================================
// Texture Type
// ============================================================================

/// Dimensionality / topology of a texture resource.
enum class TextureType : u32 {
    Texture2D,   ///< Standard 2D image (1 layer).
    Texture3D,   ///< Volume texture (depth > 1, depth halves each mip).
    TextureCube, ///< Cube map (6 square layers, one per face).
};

// ============================================================================
// Mip Level Descriptor
// ============================================================================

/// Describes a single mip level within a Texture's data buffer.
struct MipLevel {
    u32 width = 0;  ///< Width of this mip in pixels.
    u32 height = 0; ///< Height of this mip in pixels.
    u32 depth = 1;  ///< Depth of this mip (always 1 for 2D / cube textures).
    u64 offset = 0; ///< Byte offset into the Texture data buffer.
    u64 size = 0;   ///< Byte size of this mip's data.
};

// ============================================================================
// Texture
// ============================================================================

/**
 * @brief Format-agnostic GPU texture container
 *
 * Texture is the central interchange object used by every format-specific
 * parser and writer in the library. It owns a contiguous pixel-data buffer
 * and a mip chain describing the layout of every mip level and layer.
 *
 * Use the static factory methods (`create2D`, `create3D`, `createCube`)
 * to allocate a new texture, or obtain one from a parser.
 *
 * Supports in-place and copying format conversion between all PixelFormat
 * values (uncompressed ↔ BCn) via `format()` and `copyAsFormat()`.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide internals.
 */
struct Texture {

    /// @brief Default constructor – creates an empty texture.
    Texture();
    /// @brief Destructor (defined in .cpp for incomplete type).
    ~Texture();

    /// Deep-copy constructor.
    Texture(const Texture& other);
    /// Deep-copy assignment.
    Texture& operator=(const Texture& other);

    /// Move constructor.
    Texture(Texture&& other) noexcept;
    /// Move assignment.
    Texture& operator=(Texture&& other) noexcept;

    // ── Format conversion ──────────────────────────────────────────────

    /**
     * @brief Convert this texture to a new pixel format in-place.
     *
     * Replaces the internal data with the converted result. Equivalent to
     * `*this = copyAsFormat(new_fmt)`.
     *
     * @param new_fmt Target pixel format.
     */
    void format(PixelFormat new_fmt);

    /**
     * @brief Return a copy of this texture converted to a different pixel format.
     *
     * Conversion path:
     * - Same format → plain copy.
     * - BCn → decoded to RGBA8 (or RGBA16F for BC6H), then recurse.
     * - Uncompressed → uncompressed → per-pixel conversion.
     * - Uncompressed → BCn → encode via the appropriate codec.
     *
     * @param new_fmt Target pixel format.
     * @return A new Texture with the converted data.
     */
    Texture copyAsFormat(PixelFormat new_fmt) const;

    // ── Factory methods ────────────────────────────────────────────────

    /**
     * @brief Create a 2D texture.
     * @param fmt       Pixel format.
     * @param width     Width in pixels.
     * @param height    Height in pixels.
     * @param mipCount Number of mip levels (0 = auto-compute full chain).
     * @return A zero-filled Texture with the requested layout.
     */
    static Texture create2D(PixelFormat fmt, u32 width, u32 height, u32 mipCount = 0);

    /**
     * @brief Create a 3D (volume) texture.
     * @param fmt       Pixel format.
     * @param width     Width in pixels.
     * @param height    Height in pixels.
     * @param depth     Depth in slices.
     * @param mipCount Number of mip levels (0 = auto-compute full chain).
     * @return A zero-filled Texture with the requested layout.
     */
    static Texture create3D(PixelFormat fmt, u32 width, u32 height, u32 depth, u32 mipCount = 0);

    /**
     * @brief Create a cube-map texture.
     * @param fmt       Pixel format.
     * @param size      Face edge length in pixels (faces are square).
     * @param mipCount Number of mip levels (0 = auto-compute full chain).
     * @return A zero-filled Texture with 6 layers.
     */
    static Texture createCube(PixelFormat fmt, u32 size, u32 mipCount = 0);

    // ── Accessors ──────────────────────────────────────────────────────

    /// @return The texture dimensionality / topology.
    TextureType type() const;
    /// @return The pixel format of the stored data.
    PixelFormat format() const;

    /// @return Base mip width in pixels.
    u32 width() const;
    /// @return Base mip height in pixels.
    u32 height() const;
    /// @return Base mip depth (1 for 2D / cube textures).
    u32 depth() const;

    /// @return Number of array layers (6 for cube maps, 1 otherwise).
    u32 layerCount() const;

    /// @return Number of mip levels per layer.
    u32 mipCount() const;

    /**
     * @brief Get the mip-level descriptor for a given mip index and layer.
     * @param mip   Mip level index (0 = base).
     * @param layer Array layer index (0 for 2D / 3D textures).
     * @return Reference to the MipLevel struct.
     */
    const MipLevel& mipLevel(u32 mip, u32 layer = 0) const;

    // ── Data access ────────────────────────────────────────────────────

    /// @return Total byte size of the pixel-data buffer.
    u64 dataSize() const;

    /// @return Read-only span over the entire pixel-data buffer.
    std::span<const u8> data() const;

    /// @return Mutable span over the entire pixel-data buffer.
    std::span<u8> data();

    /// @return Raw read-only pointer to the pixel-data buffer.
    const u8* dataPtr() const;
    /// @return Raw mutable pointer to the pixel-data buffer.
    u8* dataPtr();

    /**
     * @brief Get a read-only span for a specific mip / layer.
     * @param mip   Mip level index.
     * @param layer Array layer (default 0).
     */
    std::span<const u8> mipData(u32 mip, u32 layer = 0) const;

    /**
     * @brief Get a mutable span for a specific mip / layer.
     * @param mip   Mip level index.
     * @param layer Array layer (default 0).
     */
    std::span<u8> mipData(u32 mip, u32 layer = 0);

    /**
     * @brief Move the data vector out of the texture (destructive).
     *
     * After this call the texture's dimensions and mip chain are cleared.
     * @return The owned pixel-data buffer.
     */
    std::vector<u8> takeData();

    /**
     * @brief Replace the pixel-data buffer.
     *
     * The new buffer must match the existing allocation size.
     * @param new_data Replacement data.
     */
    void setData(std::vector<u8> new_data);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace textures

} // namespace whiteout

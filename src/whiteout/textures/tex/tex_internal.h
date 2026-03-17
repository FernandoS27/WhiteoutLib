// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <array>
#include <span>
#include <string>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::tex {

// TEX file constants
static constexpr u32 TEX_MAGIC = 0xDEADBEEFu;
static constexpr u32 TEX_VERSION = 47;

static constexpr u32 SNO_PREAMBLE_SIZE = 32;
static constexpr u32 TEXTURE_DESC_SIZE = 24;
static constexpr u32 MIP_TABLE_SIZE = 480;
static constexpr u32 MIP_TABLE_ENTRIES = 60;
static constexpr u32 ATLAS_META_SIZE = 80;
static constexpr u32 HASH_ARRAY_HEADER_SIZE = 8;
static constexpr u32 FRAME_DESC_SIZE = 80;

static constexpr u32 OFF_PREAMBLE = 0x00;
static constexpr u32 OFF_TEXTURE_DESC = 0x20;
static constexpr u32 OFF_MIP_TABLE = 0x38;
static constexpr u32 OFF_ATLAS_META = 0x218;
static constexpr u32 OFF_HASH_HEADER = 0x268;
static constexpr u32 OFF_HASH_ARRAY = 0x270;
static constexpr u32 MIN_HEADER_SIZE = OFF_HASH_ARRAY;

// TEX pixel format IDs
static constexpr u32 TEX_FMT_A8R8G8B8 = 0;
static constexpr u32 TEX_FMT_A4R4G4B4 = 4;
static constexpr u32 TEX_FMT_L8 = 7;
static constexpr u32 TEX_FMT_DXT1 = 9;
static constexpr u32 TEX_FMT_DXT1_ALT = 10;
static constexpr u32 TEX_FMT_DXT3 = 11;
static constexpr u32 TEX_FMT_DXT5 = 12; ///< BC3/DXT5 compression
static constexpr u32 TEX_FMT_A8 = 23;
static constexpr u32 TEX_FMT_ATI2 = 43;

static constexpr u32 BC_MIP_PREFIX_SIZE = 16;

// ============================================================================
// D4 TEX pixel format IDs
// ============================================================================
static constexpr u32 D4_TEX_FMT_R8G8B8A8 = 0;
static constexpr u32 D4_TEX_FMT_BC1 = 9;
static constexpr u32 D4_TEX_FMT_BC1_ALT = 10;
static constexpr u32 D4_TEX_FMT_BC3 = 12;
static constexpr u32 D4_TEX_FMT_R8 = 23;
static constexpr u32 D4_TEX_FMT_RGBA16F = 25;
static constexpr u32 D4_TEX_FMT_BC4 = 41;
static constexpr u32 D4_TEX_FMT_BC5 = 42;
static constexpr u32 D4_TEX_FMT_BC5_ALT = 43; ///< D3-inherited BC5/ATI2 format ID
static constexpr u32 D4_TEX_FMT_BC5_SNORM = 44;
static constexpr u32 D4_TEX_FMT_R8G8B8A8_SRGB = 45;
static constexpr u32 D4_TEX_FMT_BC1_LINEAR = 46;
static constexpr u32 D4_TEX_FMT_BC1_SRGB = 47;
static constexpr u32 D4_TEX_FMT_BC2 = 48;
static constexpr u32 D4_TEX_FMT_BC3_ALT = 49;
static constexpr u32 D4_TEX_FMT_BC7 = 50;

static constexpr u32 D4_ROW_ALIGNMENT = 256;
static constexpr u32 D4_TEX_FORMAT_HASH = 0xF9CD83E6u;

#pragma pack(push, 1)

struct SnoPreamble {
    u32 magic;
    u32 version;
    std::array<u8, 8> _reserved08;
    u32 snoId;
    std::array<u8, 12> _reserved14;
};

struct TextureDescriptor {
    u32 pixelFormat;
    u32 width;
    u32 height;
    u32 depth;
    u32 flags;
    u32 extraMipCount;
};

struct MipEntry {
    u32 fileOffset;
    u32 dataSize;
};

struct AtlasMetadata {
    u32 frameCount;
    u32 frameTableOffset;
    u32 frameTableSize;
    std::array<u8, 20> _reserved224;
    u32 field238;
    u32 samplerHint1;
    u32 samplerHint2;
    std::array<u8, 36> _reserved244;
};

struct FrameHashArrayHeader {
    u32 hashArrayOffset;
    u32 hashArraySize;
};

struct FrameDescriptorDisk {
    f32 uMin;
    f32 vMin;
    f32 uMax;
    f32 vMax;
    std::array<char, 64> name;
};

#pragma pack(pop)

static_assert(sizeof(SnoPreamble) == 32, "SnoPreamble must be 32 bytes");
static_assert(sizeof(TextureDescriptor) == 24, "TextureDescriptor must be 24 bytes");
static_assert(sizeof(MipEntry) == 8, "MipEntry must be 8 bytes");
static_assert(sizeof(AtlasMetadata) == 80, "AtlasMetadata must be 80 bytes");
static_assert(sizeof(FrameHashArrayHeader) == 8, "FrameHashArrayHeader must be 8 bytes");
static_assert(sizeof(FrameDescriptorDisk) == 80, "FrameDescriptorDisk must be 80 bytes");

struct FormatMapping {
    PixelFormat format;
    bool needs_conversion;
};

std::optional<FormatMapping> tex_format_to_pixel_format(u32 tex_fmt);
std::optional<u32> pixel_format_to_tex_format(PixelFormat fmt);
u64 tex_compute_mip_size(u32 tex_fmt, u32 width, u32 height);

// Format conversion functions
void convert_a8r8g8b8_to_rgba8(const u8* src, u8* dst, u64 pixel_count);
void convert_rgba8_to_a8r8g8b8(const u8* src, u8* dst, u64 pixel_count);
void convert_a4r4g4b4_to_rgba8(const u8* src, u8* dst, u64 pixel_count);
void convert_l8_to_rgba8(const u8* src, u8* dst, u64 pixel_count);
void convert_a8_to_rgba8(const u8* src, u8* dst, u64 pixel_count);

// Block compression shuffle/unshuffle
bool is_shuffled_bc_format(u32 tex_fmt);

enum class BCShuffleDir { Shuffle, Unshuffle };
void bc_block_shuffle(u32 tex_fmt, u8* shuffled, u8* interleaved, u32 dataSize, BCShuffleDir dir);

// Mip level processing
void convert_mip_to_rgba8(u32 tex_fmt, const u8* src, u8* dst, u32 width, u32 height);
void decode_mip_face(u32 pixel_format, const u8* face_data, std::span<u8> destination,
                     u32 mip_width, u32 mip_height, u32 standard_data_size, bool needs_conversion,
                     bool is_shuffled);
void encode_mip_face(u32 tex_format, std::span<const u8> source, u8* destination, u32 mip_width,
                     u32 mip_height, u32 standard_data_size, bool needs_swizzle, bool is_shuffled);

u32 align_up(u32 value, u32 align);

// D4 format helpers
struct D4FormatMapping {
    PixelFormat format;
    bool is_srgb;
    u32 block_dim;     // 1 for uncompressed, 4 for BCn
    u32 bytes_per_unit; // per pixel or per 4×4 block
};

std::optional<D4FormatMapping> d4_tex_format_to_pixel_format(u32 d4_fmt);
u64 d4_compute_aligned_mip_size(u32 d4_fmt, u32 width, u32 height);
u64 d4_compute_raw_mip_size(u32 d4_fmt, u32 width, u32 height);

} // namespace whiteout::textures::tex

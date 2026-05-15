// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "tex_internal.h"

#include <algorithm>
#include <cstring>

#include "../utils/color_convert.h"

namespace whiteout::textures::tex {

std::optional<FormatMapping> tex_format_to_pixel_format(u32 tex_fmt) {
    switch (tex_fmt) {
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_L8:
    case TEX_FMT_A8:
        return FormatMapping{PixelFormat::RGBA8, true};
    case TEX_FMT_DXT1:
    case TEX_FMT_DXT1_ALT:
        return FormatMapping{PixelFormat::BC1, false};
    case TEX_FMT_DXT3:
        return FormatMapping{PixelFormat::BC2, false};
    case TEX_FMT_DXT5:
        return FormatMapping{PixelFormat::BC3, false};
    case TEX_FMT_ATI2:
        return FormatMapping{PixelFormat::BC5, false};
    default:
        return std::nullopt;
    }
}

std::optional<u32> pixel_format_to_tex_format(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::RGBA8:
        return TEX_FMT_A8R8G8B8;
    case PixelFormat::BC1:
        return TEX_FMT_DXT1;
    case PixelFormat::BC2:
        return TEX_FMT_DXT3;
    case PixelFormat::BC3:
        return TEX_FMT_DXT5;
    case PixelFormat::BC4:
        return std::nullopt; // No BC4/ATI1 in TEX format
    case PixelFormat::BC5:
        return TEX_FMT_ATI2;
    default:
        return std::nullopt;
    }
}

u64 tex_compute_mip_size(u32 tex_fmt, u32 width, u32 height) {
    switch (tex_fmt) {
    case TEX_FMT_A8R8G8B8:
        return static_cast<u64>(width) * height * 4;
    case TEX_FMT_A4R4G4B4:
        return static_cast<u64>(width) * height * 2;
    case TEX_FMT_L8:
    case TEX_FMT_A8:
        return static_cast<u64>(width) * height;
    case TEX_FMT_DXT1:
    case TEX_FMT_DXT1_ALT: {
        const u32 blocks_x = (width + 3) / 4;
        const u32 blocks_y = (height + 3) / 4;
        return static_cast<u64>(blocks_x) * blocks_y * 8;
    }
    case TEX_FMT_DXT3:
    case TEX_FMT_DXT5:
    case TEX_FMT_ATI2: {
        const u32 blocks_x = (width + 3) / 4;
        const u32 blocks_y = (height + 3) / 4;
        return static_cast<u64>(blocks_x) * blocks_y * 16;
    }
    default:
        return 0;
    }
}

void convert_a8r8g8b8_to_rgba8(const u8* src, u8* dst, u64 pixel_count) {
    swap_red_blue(src, dst, pixel_count);
}

void convert_rgba8_to_a8r8g8b8(const u8* src, u8* dst, u64 pixel_count) {
    swap_red_blue(src, dst, pixel_count);
}

void convert_a4r4g4b4_to_rgba8(const u8* src, u8* dst, u64 pixel_count) {
    for (u64 i = 0; i < pixel_count; ++i) {
        u16 pixel{};
        std::memcpy(&pixel, src + i * 2, 2);

        const u8 b = static_cast<u8>((pixel & 0x000Fu) * EXPAND_4BIT_TO_8BIT);
        const u8 g = static_cast<u8>(((pixel >> 4) & 0x0Fu) * EXPAND_4BIT_TO_8BIT);
        const u8 r = static_cast<u8>(((pixel >> 8) & 0x0Fu) * EXPAND_4BIT_TO_8BIT);
        const u8 a = static_cast<u8>(((pixel >> 12) & 0x0Fu) * EXPAND_4BIT_TO_8BIT);
        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = a;
    }
}

void convert_l8_to_rgba8(const u8* src, u8* dst, u64 pixel_count) {
    for (u64 i = 0; i < pixel_count; ++i) {
        const u8 luminance = src[i];
        dst[i * 4 + 0] = luminance;
        dst[i * 4 + 1] = luminance;
        dst[i * 4 + 2] = luminance;
        dst[i * 4 + 3] = 255;
    }
}

void convert_a8_to_rgba8(const u8* src, u8* dst, u64 pixel_count) {
    for (u64 i = 0; i < pixel_count; ++i) {
        dst[i * 4 + 0] = 0;
        dst[i * 4 + 1] = 0;
        dst[i * 4 + 2] = 0;
        dst[i * 4 + 3] = src[i];
    }
}

bool is_shuffled_bc_format(u32 tex_fmt) {
    return tex_fmt == TEX_FMT_DXT1 || tex_fmt == TEX_FMT_DXT1_ALT || tex_fmt == TEX_FMT_DXT3 ||
           tex_fmt == TEX_FMT_DXT5 || tex_fmt == TEX_FMT_ATI2;
}

void bc_block_shuffle(u32 tex_fmt, u8* shuffled, u8* interleaved, u32 dataSize, BCShuffleDir dir) {
    const u32 half_size = dataSize / 2;

    // Helper: copies bytes between shuffled/interleaved sides based on direction.
    auto directed_copy = [dir](u8* shuffled_ptr, u8* interleaved_ptr, size_t size) {
        if (dir == BCShuffleDir::Shuffle)
            std::memcpy(shuffled_ptr, interleaved_ptr, size);
        else
            std::memcpy(interleaved_ptr, shuffled_ptr, size);
    };

    switch (tex_fmt) {
    case TEX_FMT_DXT1:
    case TEX_FMT_DXT1_ALT: {
        const u32 block_count = half_size / 4;
        u8* color_endpoints = shuffled;
        u8* color_indices = shuffled + half_size;
        for (u32 i = 0; i < block_count; ++i) {
            directed_copy(color_endpoints + i * 4, interleaved + i * 8, 4);
            directed_copy(color_indices + i * 4, interleaved + i * 8 + 4, 4);
        }
        break;
    }
    case TEX_FMT_DXT3: {
        const u32 block_count = half_size / 8;
        u8* alpha_data = shuffled;
        u8* color_endpoints = shuffled + half_size;
        u8* color_indices = shuffled + half_size + half_size / 2;
        for (u32 i = 0; i < block_count; ++i) {
            directed_copy(alpha_data + i * 8, interleaved + i * 16, 8);
            directed_copy(color_endpoints + i * 4, interleaved + i * 16 + 8, 4);
            directed_copy(color_indices + i * 4, interleaved + i * 16 + 12, 4);
        }
        break;
    }
    case TEX_FMT_DXT5:
    case TEX_FMT_ATI2: {
        const u32 block_count = half_size / 8;
        u8* alpha_endpoints = shuffled;
        u8* alpha_indices = shuffled + half_size / 4;
        u8* color_endpoints = shuffled + half_size;
        u8* color_indices = shuffled + half_size + half_size / 2;
        for (u32 i = 0; i < block_count; ++i) {
            directed_copy(alpha_endpoints + i * 2, interleaved + i * 16, 2);
            directed_copy(alpha_indices + i * 6, interleaved + i * 16 + 2, 6);
            directed_copy(color_endpoints + i * 4, interleaved + i * 16 + 8, 4);
            directed_copy(color_indices + i * 4, interleaved + i * 16 + 12, 4);
        }
        break;
    }
    default:
        break;
    }
}

void convert_mip_to_rgba8(u32 tex_fmt, const u8* src, u8* dst, u32 width, u32 height) {
    const u64 pixels = static_cast<u64>(width) * height;
    switch (tex_fmt) {
    case TEX_FMT_A8R8G8B8:
        convert_a8r8g8b8_to_rgba8(src, dst, pixels);
        break;
    case TEX_FMT_A4R4G4B4:
        convert_a4r4g4b4_to_rgba8(src, dst, pixels);
        break;
    case TEX_FMT_L8:
        convert_l8_to_rgba8(src, dst, pixels);
        break;
    case TEX_FMT_A8:
        convert_a8_to_rgba8(src, dst, pixels);
        break;
    default:
        break;
    }
}

void decode_mip_face(u32 pixel_format, const u8* face_data, std::span<u8> destination,
                     u32 mip_width, u32 mip_height, u32 standard_data_size, bool needs_conversion,
                     bool is_shuffled) {
    if (needs_conversion) {
        convert_mip_to_rgba8(pixel_format, face_data, destination.data(), mip_width, mip_height);
        return;
    }
    if (is_shuffled) {
        bc_block_shuffle(pixel_format, const_cast<u8*>(face_data + BC_MIP_PREFIX_SIZE),
                         destination.data(), standard_data_size, BCShuffleDir::Unshuffle);
        return;
    }
    const u64 copy_size =
        std::min(static_cast<u64>(standard_data_size), static_cast<u64>(destination.size()));
    std::memcpy(destination.data(), face_data, static_cast<size_t>(copy_size));
}

void encode_mip_face(u32 tex_format, std::span<const u8> source, u8* destination, u32 mip_width,
                     u32 mip_height, u32 standard_data_size, bool needs_swizzle, bool is_shuffled) {
    if (needs_swizzle) {
        convert_rgba8_to_a8r8g8b8(source.data(), destination,
                                  static_cast<u64>(mip_width) * mip_height);
        return;
    }
    if (is_shuffled) {
        // 16-byte prefix is already zeroed; write shuffled data after it.
        bc_block_shuffle(tex_format, destination + BC_MIP_PREFIX_SIZE,
                         const_cast<u8*>(source.data()), standard_data_size, BCShuffleDir::Shuffle);
        return;
    }
    const u64 copy_size =
        std::min(static_cast<u64>(standard_data_size), static_cast<u64>(source.size()));
    std::memcpy(destination, source.data(), static_cast<size_t>(copy_size));
}

u32 align_up(u32 value, u32 align) {
    return (value + align - 1) / align * align;
}

// ============================================================================
// Diablo IV format helpers
// ============================================================================

std::optional<D4FormatMapping> d4_tex_format_to_pixel_format(u32 d4_fmt) {
    switch (d4_fmt) {
    case D4_TEX_FMT_R8G8B8A8:
        return D4FormatMapping{PixelFormat::RGBA8, false, 1, 4};
    case D4_TEX_FMT_R8:
        return D4FormatMapping{PixelFormat::R8, false, 1, 1};
    case D4_TEX_FMT_R8G8B8A8_SRGB:
        return D4FormatMapping{PixelFormat::RGBA8, true, 1, 4};
    case D4_TEX_FMT_BC1:
    case D4_TEX_FMT_BC1_ALT:
    case D4_TEX_FMT_BC1_LINEAR:
        return D4FormatMapping{PixelFormat::BC1, false, 4, 8};
    case D4_TEX_FMT_BC1_SRGB:
        return D4FormatMapping{PixelFormat::BC1, true, 4, 8};
    case D4_TEX_FMT_BC2:
        return D4FormatMapping{PixelFormat::BC2, false, 4, 16};
    case D4_TEX_FMT_BC3:
        return D4FormatMapping{PixelFormat::BC3, false, 4, 16};
    case D4_TEX_FMT_RGBA16F:
        return D4FormatMapping{PixelFormat::RGBA32F, false, 1, 8}; // will convert f16→f32
    case D4_TEX_FMT_RGBA32F:
        return D4FormatMapping{PixelFormat::RGBA32F, false, 1, 16}; // raw f32 RGBA
    case D4_TEX_FMT_BC4:
        return D4FormatMapping{PixelFormat::BC4, false, 4, 8};
    case D4_TEX_FMT_BC5:
    case D4_TEX_FMT_BC5_ALT:
    case D4_TEX_FMT_BC5_SNORM:
        return D4FormatMapping{PixelFormat::BC5, false, 4, 16};
    case D4_TEX_FMT_BC3_ALT:
        return D4FormatMapping{PixelFormat::BC3, false, 4, 16};
    case D4_TEX_FMT_BC7:
        return D4FormatMapping{PixelFormat::BC7, false, 4, 16};
    default:
        return std::nullopt;
    }
}

u64 d4_compute_aligned_mip_size(u32 d4_fmt, u32 width, u32 height) {
    auto mapping = d4_tex_format_to_pixel_format(d4_fmt);
    if (!mapping)
        return 0;

    u32 rows;
    u32 row_bytes;
    if (mapping->block_dim > 1) {
        const u32 bw = std::max(1u, (width + mapping->block_dim - 1) / mapping->block_dim);
        rows = std::max(1u, (height + mapping->block_dim - 1) / mapping->block_dim);
        row_bytes = bw * mapping->bytes_per_unit;
    } else {
        rows = height;
        row_bytes = width * mapping->bytes_per_unit;
    }

    const u32 aligned_pitch = align_up(row_bytes, D4_ROW_ALIGNMENT);
    return static_cast<u64>(aligned_pitch) * rows;
}

u64 d4_compute_raw_mip_size(u32 d4_fmt, u32 width, u32 height) {
    auto mapping = d4_tex_format_to_pixel_format(d4_fmt);
    if (!mapping)
        return 0;

    if (mapping->block_dim > 1) {
        const u32 bw = std::max(1u, (width + mapping->block_dim - 1) / mapping->block_dim);
        const u32 bh = std::max(1u, (height + mapping->block_dim - 1) / mapping->block_dim);
        return static_cast<u64>(bw) * bh * mapping->bytes_per_unit;
    }
    return static_cast<u64>(width) * height * mapping->bytes_per_unit;
}

} // namespace whiteout::textures::tex

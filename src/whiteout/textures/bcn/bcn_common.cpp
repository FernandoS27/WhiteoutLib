// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "bcn_common.h"

namespace whiteout::textures {

void gather_channel_block(std::span<const u8> rgba, u32 width, u32 height, u32 block_x, u32 block_y,
                          std::array<u8, 16>& out, u32 stride, u32 channel_offset) {
    for (u32 pixel_y = 0; pixel_y < 4; ++pixel_y) {
        for (u32 pixel_x = 0; pixel_x < 4; ++pixel_x) {
            u32 src_x = std::min(block_x * 4 + pixel_x, width - 1);
            u32 src_y = std::min(block_y * 4 + pixel_y, height - 1);
            out[pixel_y * 4 + pixel_x] = rgba[(src_y * width + src_x) * stride + channel_offset];
        }
    }
}

void scatter_channel_block(const std::array<u8, 16>& values, u32 width, u32 height, u32 block_x,
                           u32 block_y, u8* result, u32 stride, u32 channel_offset) {
    for (u32 pixel_y = 0; pixel_y < 4; ++pixel_y) {
        for (u32 pixel_x = 0; pixel_x < 4; ++pixel_x) {
            const u32 x = block_x * 4 + pixel_x;
            const u32 y = block_y * 4 + pixel_y;
            if (x < width && y < height)
                result[(y * width + x) * stride + channel_offset] = values[pixel_y * 4 + pixel_x];
        }
    }
}

Texture make_texture_like(const Texture& src, PixelFormat format) {
    switch (src.type()) {
    case TextureType::Texture2D:
        return Texture::create2D(format, src.width(), src.height(), src.mipCount());
    case TextureType::Texture3D:
        return Texture::create3D(format, src.width(), src.height(), src.depth(), src.mipCount());
    case TextureType::TextureCube:
        return Texture::createCube(format, src.width(), src.mipCount());
    case TextureType::Texture2DArray:
        return Texture::create2DArray(format, src.width(), src.height(), src.arraySize(),
                                      src.mipCount());
    case TextureType::TextureCubeArray:
        return Texture::createCubeArray(format, src.width(), src.arraySize(), src.mipCount());
    }
    return {}; // unreachable
}

} // namespace whiteout::textures

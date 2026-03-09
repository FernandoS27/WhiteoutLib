// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <array>
#include <cstring>
#include <optional>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures::dds {

static constexpr u32 DDS_MAGIC = 0x20534444u;

static constexpr u32 DDPF_ALPHAPIXELS = 0x00000001u;
static constexpr u32 DDPF_FOURCC = 0x00000004u;
static constexpr u32 DDPF_RGB = 0x00000040u;

static constexpr u32 DDSD_CAPS = 0x00000001u;
static constexpr u32 DDSD_HEIGHT = 0x00000002u;
static constexpr u32 DDSD_WIDTH = 0x00000004u;
static constexpr u32 DDSD_PITCH = 0x00000008u;
static constexpr u32 DDSD_PIXELFORMAT = 0x00001000u;
static constexpr u32 DDSD_MIPMAPCOUNT = 0x00020000u;
static constexpr u32 DDSD_LINEARSIZE = 0x00080000u;
static constexpr u32 DDSD_DEPTH = 0x00800000u;

static constexpr u32 DDSCAPS_COMPLEX = 0x00000008u;
static constexpr u32 DDSCAPS_MIPMAP = 0x00400000u;
static constexpr u32 DDSCAPS_TEXTURE = 0x00001000u;

static constexpr u32 DDSCAPS2_CUBEMAP = 0x00000200u;
static constexpr u32 DDSCAPS2_CUBEMAP_POSITIVEX = 0x00000400u;
static constexpr u32 DDSCAPS2_CUBEMAP_NEGATIVEX = 0x00000800u;
static constexpr u32 DDSCAPS2_CUBEMAP_POSITIVEY = 0x00001000u;
static constexpr u32 DDSCAPS2_CUBEMAP_NEGATIVEY = 0x00002000u;
static constexpr u32 DDSCAPS2_CUBEMAP_POSITIVEZ = 0x00004000u;
static constexpr u32 DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x00008000u;
static constexpr u32 DDSCAPS2_VOLUME = 0x00200000u;

static constexpr u32 DDSCAPS2_CUBEMAP_ALL_FACES =
    DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_POSITIVEX | DDSCAPS2_CUBEMAP_NEGATIVEX |
    DDSCAPS2_CUBEMAP_POSITIVEY | DDSCAPS2_CUBEMAP_NEGATIVEY | DDSCAPS2_CUBEMAP_POSITIVEZ |
    DDSCAPS2_CUBEMAP_NEGATIVEZ;

enum DxgiFormat : u32 {
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_BC1_UNORM = 71,
    DXGI_FORMAT_BC2_UNORM = 74,
    DXGI_FORMAT_BC3_UNORM = 77,
    DXGI_FORMAT_BC4_UNORM = 80,
    DXGI_FORMAT_BC5_UNORM = 83,
    DXGI_FORMAT_BC6H_UF16 = 95,
    DXGI_FORMAT_BC7_UNORM = 98,
};

enum D3D10ResourceDimension : u32 {
    D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3,
    D3D10_RESOURCE_DIMENSION_TEXTURE3D = 4,
};

static constexpr u32 DDS_RESOURCE_MISC_TEXTURECUBE = 0x4u;

static constexpr u32 make_fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(a) | (static_cast<u32>(b) << 8) | (static_cast<u32>(c) << 16) |
           (static_cast<u32>(d) << 24);
}

#pragma pack(push, 1)

struct DDS_PIXELFORMAT {
    u32 size;
    u32 flags;
    u32 fourCC;
    u32 rgbBitCount;
    u32 rBitMask;
    u32 gBitMask;
    u32 bBitMask;
    u32 aBitMask;
};

struct DDS_HEADER {
    u32 size;
    u32 flags;
    u32 height;
    u32 width;
    u32 pitchOrLinearSize;
    u32 depth;
    u32 mipMapCount;
    std::array<u32, 11> reserved1;
    DDS_PIXELFORMAT ddspf;
    u32 caps;
    u32 caps2;
    u32 caps3;
    u32 caps4;
    u32 reserved2;
};

struct DDS_HEADER_DXT10 {
    u32 dxgiFormat;
    u32 resourceDimension;
    u32 miscFlag;
    u32 arraySize;
    u32 miscFlags2;
};

#pragma pack(pop)

static_assert(sizeof(DDS_PIXELFORMAT) == 32, "DDS_PIXELFORMAT must be 32 bytes");
static_assert(sizeof(DDS_HEADER) == 124, "DDS_HEADER must be 124 bytes");
static_assert(sizeof(DDS_HEADER_DXT10) == 20, "DDS_HEADER_DXT10 must be 20 bytes");

inline std::optional<PixelFormat> dxgi_to_pixel_format(u32 dxgi_format) {
    switch (dxgi_format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return PixelFormat::RGBA8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return PixelFormat::RGBA16F;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return PixelFormat::RGBA32F;
    case DXGI_FORMAT_BC1_UNORM:
        return PixelFormat::BC1;
    case DXGI_FORMAT_BC2_UNORM:
        return PixelFormat::BC2;
    case DXGI_FORMAT_BC3_UNORM:
        return PixelFormat::BC3;
    case DXGI_FORMAT_BC4_UNORM:
        return PixelFormat::BC4;
    case DXGI_FORMAT_BC5_UNORM:
        return PixelFormat::BC5;
    case DXGI_FORMAT_BC6H_UF16:
        return PixelFormat::BC6H;
    case DXGI_FORMAT_BC7_UNORM:
        return PixelFormat::BC7;
    default:
        return std::nullopt;
    }
}

inline u32 pixel_format_to_dxgi(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::RGBA16F:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case PixelFormat::RGBA32F:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case PixelFormat::BC1:
        return DXGI_FORMAT_BC1_UNORM;
    case PixelFormat::BC2:
        return DXGI_FORMAT_BC2_UNORM;
    case PixelFormat::BC3:
        return DXGI_FORMAT_BC3_UNORM;
    case PixelFormat::BC4:
        return DXGI_FORMAT_BC4_UNORM;
    case PixelFormat::BC5:
        return DXGI_FORMAT_BC5_UNORM;
    case PixelFormat::BC6H:
        return DXGI_FORMAT_BC6H_UF16;
    case PixelFormat::BC7:
        return DXGI_FORMAT_BC7_UNORM;
    }
    return 0;
}

inline std::optional<PixelFormat> legacy_pixfmt_to_pixel_format(
    const DDS_PIXELFORMAT& pixel_format) {
    if (pixel_format.flags & DDPF_FOURCC) {
        switch (pixel_format.fourCC) {
        case make_fourcc('D', 'X', 'T', '1'):
            return PixelFormat::BC1;
        case make_fourcc('D', 'X', 'T', '3'):
            return PixelFormat::BC2;
        case make_fourcc('D', 'X', 'T', '5'):
            return PixelFormat::BC3;
        case make_fourcc('A', 'T', 'I', '1'):
        case make_fourcc('B', 'C', '4', 'U'):
            return PixelFormat::BC4;
        case make_fourcc('A', 'T', 'I', '2'):
        case make_fourcc('B', 'C', '5', 'U'):
            return PixelFormat::BC5;
        default:
            return std::nullopt;
        }
    }

    if (pixel_format.flags & DDPF_RGB) {
        const bool is_rgba8 =
            pixel_format.rgbBitCount == 32 && pixel_format.rBitMask == 0x000000FFu &&
            pixel_format.gBitMask == 0x0000FF00u && pixel_format.bBitMask == 0x00FF0000u &&
            (!(pixel_format.flags & DDPF_ALPHAPIXELS) || pixel_format.aBitMask == 0xFF000000u);

        if (is_rgba8) {
            return PixelFormat::RGBA8;
        }
    }

    return std::nullopt;
}

inline bool needs_dx10_header(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA16F:
    case PixelFormat::RGBA32F:
    case PixelFormat::BC6H:
    case PixelFormat::BC7:
        return true;
    default:
        return false;
    }
}

inline void fill_legacy_pixelformat(DDS_PIXELFORMAT& pixel_format, PixelFormat format) {
    std::memset(&pixel_format, 0, sizeof(pixel_format));
    pixel_format.size = 32;

    switch (format) {
    case PixelFormat::RGBA8:
        pixel_format.flags = DDPF_RGB | DDPF_ALPHAPIXELS;
        pixel_format.rgbBitCount = 32;
        pixel_format.rBitMask = 0x000000FFu;
        pixel_format.gBitMask = 0x0000FF00u;
        pixel_format.bBitMask = 0x00FF0000u;
        pixel_format.aBitMask = 0xFF000000u;
        break;
    case PixelFormat::BC1:
        pixel_format.flags = DDPF_FOURCC;
        pixel_format.fourCC = make_fourcc('D', 'X', 'T', '1');
        break;
    case PixelFormat::BC2:
        pixel_format.flags = DDPF_FOURCC;
        pixel_format.fourCC = make_fourcc('D', 'X', 'T', '3');
        break;
    case PixelFormat::BC3:
        pixel_format.flags = DDPF_FOURCC;
        pixel_format.fourCC = make_fourcc('D', 'X', 'T', '5');
        break;
    case PixelFormat::BC4:
        pixel_format.flags = DDPF_FOURCC;
        pixel_format.fourCC = make_fourcc('A', 'T', 'I', '1');
        break;
    case PixelFormat::BC5:
        pixel_format.flags = DDPF_FOURCC;
        pixel_format.fourCC = make_fourcc('A', 'T', 'I', '2');
        break;
    default:
        pixel_format.flags = DDPF_FOURCC;
        pixel_format.fourCC = make_fourcc('D', 'X', '1', '0');
        break;
    }
}

} // namespace whiteout::textures::dds

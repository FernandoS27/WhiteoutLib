// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/dds/parser.h>

#include "dds_internal.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include "../io_helpers.h"
#include "../issue_sink.h"

namespace whiteout::textures::dds {

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer);

private:
    std::span<const u8> buffer_;
};

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer) {
    issues.clear();
    buffer_ = buffer;

    const size_t min_size = 4 + sizeof(DDS_HEADER);
    if (buffer_.size() < min_size) {
        fail("Buffer too small for a DDS file");
        return std::nullopt;
    }

    u32 magic{};
    std::memcpy(&magic, buffer_.data(), 4);
    if (magic != DDS_MAGIC) {
        fail("Invalid DDS magic");
        return std::nullopt;
    }

    DDS_HEADER header{};
    std::memcpy(&header, buffer_.data() + 4, sizeof(DDS_HEADER));
    if (header.size != 124) {
        fail("DDS header size mismatch");
        return std::nullopt;
    }

    size_t data_offset = 4 + sizeof(DDS_HEADER);

    const bool has_dx10_header = (header.ddspf.flags & DDPF_FOURCC) &&
                                 header.ddspf.fourCC == make_fourcc('D', 'X', '1', '0');

    DDS_HEADER_DXT10 dx10_header{};
    if (has_dx10_header) {
        if (buffer_.size() < data_offset + sizeof(DDS_HEADER_DXT10)) {
            fail("Buffer too small for DXT10 header");
            return std::nullopt;
        }
        std::memcpy(&dx10_header, buffer_.data() + data_offset, sizeof(DDS_HEADER_DXT10));
        data_offset += sizeof(DDS_HEADER_DXT10);
    }

    std::optional<PixelFormat> pixel_format;
    bool srgb = false;
    LegacyChannelOrder channel_order = LegacyChannelOrder::Rgba;
    if (has_dx10_header) {
        pixel_format = dxgi_to_pixel_format(dx10_header.dxgiFormat);
        srgb = is_dxgi_srgb(dx10_header.dxgiFormat);
        if (!pixel_format) {
            fail("Unsupported DXGI format: " + std::to_string(dx10_header.dxgiFormat));
            return std::nullopt;
        }
    } else {
        pixel_format = legacy_pixfmt_to_pixel_format(header.ddspf, channel_order);
        if (!pixel_format) {
            fail("Unsupported legacy DDS pixel format");
            return std::nullopt;
        }
    }

    const u32 width = header.width;
    const u32 height = header.height;
    const u32 depth = (header.flags & DDSD_DEPTH) ? std::max(header.depth, 1u) : 1u;
    const u32 mipCount = (header.flags & DDSD_MIPMAPCOUNT) ? std::max(header.mipMapCount, 1u) : 1u;

    const bool is_cubemap = has_dx10_header
                                ? (dx10_header.miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE) != 0
                                : (header.caps2 & DDSCAPS2_CUBEMAP) != 0;

    const bool is_volume = has_dx10_header
                               ? dx10_header.resourceDimension == D3D10_RESOURCE_DIMENSION_TEXTURE3D
                               : (header.caps2 & DDSCAPS2_VOLUME) != 0;

    // arraySize counts cube-maps for cube arrays, 2D slices for 2D arrays.
    // Legacy headers have no array concept; DX10 stores 1 for non-array textures.
    const u32 array_size = has_dx10_header ? std::max(dx10_header.arraySize, 1u) : 1u;

    if (is_volume && array_size > 1) {
        fail("3D texture arrays are not supported");
        return std::nullopt;
    }

    Texture texture;
    if (is_cubemap) {
        if (width != height) {
            fail("Cubemap faces must be square, got " + std::to_string(width) + "x" +
                 std::to_string(height));
            return std::nullopt;
        }
        if (array_size > 1) {
            texture = Texture::createCubeArray(*pixel_format, width, array_size, mipCount);
        } else {
            texture = Texture::createCube(*pixel_format, width, mipCount);
        }
    } else if (is_volume) {
        texture = Texture::create3D(*pixel_format, width, height, depth, mipCount);
    } else if (array_size > 1) {
        texture = Texture::create2DArray(*pixel_format, width, height, array_size, mipCount);
    } else {
        texture = Texture::create2D(*pixel_format, width, height, mipCount);
    }

    texture.setSrgb(srgb);

    if (buffer_.size() < data_offset + texture.dataSize()) {
        fail("DDS file truncated: need " + std::to_string(data_offset + texture.dataSize()) +
             " bytes, got " + std::to_string(buffer_.size()));
        return std::nullopt;
    }

    std::memcpy(texture.dataPtr(), buffer_.data() + data_offset,
                static_cast<size_t>(texture.dataSize()));

    // BGRA -> RGBA. Every pixel is 4 bytes regardless of mip level, face or
    // array slice, so one pass over the whole payload covers them all.
    if (channel_order == LegacyChannelOrder::Bgra) {
        u8* texels = texture.dataPtr();
        const size_t byte_count = static_cast<size_t>(texture.dataSize());
        for (size_t i = 0; i + 3 < byte_count; i += 4) {
            std::swap(texels[i], texels[i + 2]);
        }
    }

    return texture;
}

Parser::Parser() : pImpl(std::make_unique<Impl>()) {}

Parser::~Parser() = default;

std::optional<Texture> Parser::parse(const std::string& filePath) {
    pImpl->issues.clear();
    auto buf = read_file_bytes(filePath, *pImpl);
    if (!buf) {
        return std::nullopt;
    }
    return pImpl->parse(std::span<const u8>{*buf});
}

std::optional<Texture> Parser::parse(std::span<const u8> buffer) {
    return pImpl->parse(buffer);
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::dds

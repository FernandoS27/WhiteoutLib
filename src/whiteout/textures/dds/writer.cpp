// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/dds/writer.h>

#include "dds_internal.h"

#include "../issue_sink.h"

#include "../io_helpers.h"

#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::dds {

class Writer::Impl : public IssueSink {
public:
    std::vector<u8> write(const Texture& texture);
};

std::vector<u8> Writer::Impl::write(const Texture& texture) {
    issues.clear();

    if (texture.width() == 0 || texture.height() == 0) {
        fail("Cannot save an empty texture");
        return {};
    }

    const bool is_array = texture.type() == TextureType::Texture2DArray ||
                          texture.type() == TextureType::TextureCubeArray;
    const bool use_dx10 = needs_dx10_header(texture.format()) || texture.isSrgb() || is_array;

    const size_t header_size = 4 + sizeof(DDS_HEADER) + (use_dx10 ? sizeof(DDS_HEADER_DXT10) : 0);
    const size_t total_size = header_size + static_cast<size_t>(texture.dataSize());

    std::vector<u8> output(total_size, 0);
    size_t offset = 0;

    const u32 magic = DDS_MAGIC;
    std::memcpy(output.data() + offset, &magic, 4);
    offset += 4;

    DDS_HEADER header{};
    header.size = 124;
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    header.height = texture.height();
    header.width = texture.width();
    header.depth = texture.depth();

    if (texture.mipCount() > 1) {
        header.flags |= DDSD_MIPMAPCOUNT;
        header.mipMapCount = texture.mipCount();
        header.caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
    }

    {
        const u64 top_mip_size =
            computeImageSize(texture.format(), texture.width(), texture.height());
        if (blockEdge(texture.format()) > 1) {
            header.flags |= DDSD_LINEARSIZE;
            header.pitchOrLinearSize = static_cast<u32>(top_mip_size);
        } else {
            header.flags |= DDSD_PITCH;
            header.pitchOrLinearSize = static_cast<u32>(top_mip_size / texture.height());
        }
    }

    header.caps |= DDSCAPS_TEXTURE;

    if (texture.type() == TextureType::TextureCube ||
        texture.type() == TextureType::TextureCubeArray) {
        header.caps |= DDSCAPS_COMPLEX;
        header.caps2 |= DDSCAPS2_CUBEMAP_ALL_FACES;
    } else if (texture.type() == TextureType::Texture3D) {
        header.flags |= DDSD_DEPTH;
        header.caps |= DDSCAPS_COMPLEX;
        header.caps2 |= DDSCAPS2_VOLUME;
    } else if (texture.type() == TextureType::Texture2DArray) {
        header.caps |= DDSCAPS_COMPLEX;
    }

    if (use_dx10) {
        // When we emit a DX10 extended header the pixel-format block must
        // advertise the "DX10" FourCC so the parser knows to read the
        // extra 20 bytes before pixel data. Otherwise it would treat those
        // 20 bytes as the start of the first mip.
        std::memset(&header.ddspf, 0, sizeof(header.ddspf));
        header.ddspf.size = 32;
        header.ddspf.flags = DDPF_FOURCC;
        header.ddspf.fourCC = make_fourcc('D', 'X', '1', '0');
    } else {
        fill_legacy_pixelformat(header.ddspf, texture.format());
    }

    std::memcpy(output.data() + offset, &header, sizeof(DDS_HEADER));
    offset += sizeof(DDS_HEADER);

    if (use_dx10) {
        DDS_HEADER_DXT10 dx10_header{};
        dx10_header.dxgiFormat = texture.isSrgb() ? pixel_format_to_dxgi_srgb(texture.format())
                                                  : pixel_format_to_dxgi(texture.format());
        dx10_header.arraySize = 1;
        dx10_header.miscFlags2 = 0;

        switch (texture.type()) {
        case TextureType::TextureCube:
            dx10_header.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
            dx10_header.miscFlag = DDS_RESOURCE_MISC_TEXTURECUBE;
            break;
        case TextureType::TextureCubeArray:
            dx10_header.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
            dx10_header.miscFlag = DDS_RESOURCE_MISC_TEXTURECUBE;
            dx10_header.arraySize = texture.arraySize();
            break;
        case TextureType::Texture3D:
            dx10_header.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE3D;
            break;
        case TextureType::Texture2DArray:
            dx10_header.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
            dx10_header.arraySize = texture.arraySize();
            break;
        case TextureType::Texture2D:
        default:
            dx10_header.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
            break;
        }

        std::memcpy(output.data() + offset, &dx10_header, sizeof(DDS_HEADER_DXT10));
        offset += sizeof(DDS_HEADER_DXT10);
    }

    std::memcpy(output.data() + offset, texture.dataPtr(), static_cast<size_t>(texture.dataSize()));

    return output;
}

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const Texture& texture) {
    auto data = pImpl->write(texture);
    if (data.empty()) {
        return; // issues already logged in Lenient mode, or threw in Strict mode
    }
    if (!write_file_bytes(filePath, data, *pImpl)) {
        return;
    }
}

std::vector<u8> Writer::write(const Texture& texture) {
    return pImpl->write(texture);
}

bool Writer::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Writer::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::dds

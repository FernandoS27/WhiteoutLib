// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/d2r_texture/writer.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../io_helpers.h"
#include "../issue_sink.h"

#include "d2r_texture_internal.h"

namespace whiteout::textures::d2r_texture {

class Writer::Impl : public IssueSink {
public:
    std::vector<u8> write(const Texture& texture, const SaveOptions& opts);
};

std::vector<u8> Writer::Impl::write(const Texture& texture, const SaveOptions& opts) {
    issues.clear();

    if (texture.width() == 0 || texture.height() == 0) {
        fail("Cannot save an empty texture");
        return {};
    }

    if (texture.type() != TextureType::Texture2D) {
        fail("D2R texture format only supports 2D textures");
        return {};
    }

    // Resolve the on-disk format code and whether a BGRA swizzle is needed.
    u16 format_code = 0;
    bool is_bgra = false;
    if (opts.formatCode) {
        auto mapping = d2r_format_to_pixel_format(*opts.formatCode);
        if (!mapping) {
            fail("Forced D2R format code " + std::to_string(*opts.formatCode) + " is not recognised");
            return {};
        }
        if (mapping->format != texture.format()) {
            fail("Forced D2R format code does not match the texture's pixel format");
            return {};
        }
        format_code = *opts.formatCode;
        is_bgra = mapping->is_bgra;
    } else {
        auto code = pixel_format_to_d2r_format(texture.format(), texture.isSrgb());
        if (!code) {
            fail("Pixel format not representable as a D2R texture (only RGBA8, BC1, "
                 "BC3, BC4, BC5 are supported)");
            return {};
        }
        format_code = *code;
        is_bgra = (format_code == D2R_FMT_RGBA8);
    }

    const u32 mipCount = texture.mipCount();
    const u64 data_start = static_cast<u64>(D2R_HEADER_SIZE) + static_cast<u64>(mipCount) * D2R_MIP_ENTRY_SIZE;

    u64 total = data_start;
    for (u32 mip = 0; mip < mipCount; ++mip)
        total += texture.mipLevel(mip).size;

    std::vector<u8> output(total, 0);

    Header header{};
    header.magic = D2R_MAGIC;
    header.format = format_code;
    header.type = D2R_TYPE_2D;
    header.mipCount = static_cast<u8>(mipCount);
    header.width = texture.width();
    header.height = texture.height();
    header.arraySize = 1;
    header.reserved0 = 0;
    header.reserved1 = 0;
    header.mipCountU32 = mipCount;
    header.reserved2 = D2R_RESERVED2;
    std::memcpy(output.data(), &header, sizeof(Header));

    u64 data_pos = data_start;
    for (u32 mip = 0; mip < mipCount; ++mip) {
        auto source = texture.mipData(mip);
        const u64 offset_field_pos = static_cast<u64>(D2R_HEADER_SIZE) + mip * D2R_MIP_ENTRY_SIZE + 4;

        MipEntry entry{};
        entry.size = static_cast<u32>(source.size());
        entry.offset = static_cast<u32>(data_pos - offset_field_pos);
        std::memcpy(output.data() + D2R_HEADER_SIZE + mip * D2R_MIP_ENTRY_SIZE, &entry,
                    sizeof(MipEntry));

        if (is_bgra) {
            const u32 mip_width = std::max(texture.width() >> mip, 1u);
            const u32 mip_height = std::max(texture.height() >> mip, 1u);
            swizzle_bgra_rgba(source.data(), output.data() + data_pos,
                              static_cast<u64>(mip_width) * mip_height);
        } else {
            std::memcpy(output.data() + data_pos, source.data(), source.size());
        }
        data_pos += source.size();
    }

    return output;
}

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const Texture& texture) {
    write(filePath, texture, SaveOptions{});
}

std::vector<u8> Writer::write(const Texture& texture) {
    return write(texture, SaveOptions{});
}

void Writer::write(const std::string& filePath, const Texture& texture, const SaveOptions& opts) {
    auto data = pImpl->write(texture, opts);
    if (data.empty())
        return;
    write_file_bytes(filePath, data, *pImpl);
}

std::vector<u8> Writer::write(const Texture& texture, const SaveOptions& opts) {
    return pImpl->write(texture, opts);
}

bool Writer::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Writer::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::d2r_texture

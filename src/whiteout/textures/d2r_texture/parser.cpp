// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/d2r_texture/parser.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "../io_helpers.h"
#include "../issue_sink.h"

#include "d2r_texture_internal.h"

namespace whiteout::textures::d2r_texture {

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer, D2rTextureInfo* outInfo);
};

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer, D2rTextureInfo* outInfo) {
    issues.clear();

    if (buffer.size() < D2R_HEADER_SIZE) {
        fail("D2R texture too small for header");
        return std::nullopt;
    }

    Header header{};
    std::memcpy(&header, buffer.data(), sizeof(Header));

    if (header.magic != D2R_MAGIC) {
        fail("Invalid D2R texture magic (expected \"<DE(\")");
        return std::nullopt;
    }

    if (header.type != D2R_TYPE_2D) {
        fail("Unsupported D2R texture type: " + std::to_string(header.type) + " (expected 2)");
        return std::nullopt;
    }

    if (header.width == 0 || header.height == 0) {
        fail("D2R texture has zero dimensions");
        return std::nullopt;
    }

    auto mapping = d2r_format_to_pixel_format(header.format);
    if (!mapping) {
        fail("Unsupported D2R pixel format code: " + std::to_string(header.format));
        return std::nullopt;
    }

    const u32 mipCount = header.mipCountU32;
    if (mipCount == 0) {
        fail("D2R texture has no mip levels");
        return std::nullopt;
    }
    if (mipCount > computeMaxMipCount(header.width, header.height)) {
        fail("D2R texture mip count " + std::to_string(mipCount) + " exceeds maximum for " +
             std::to_string(header.width) + "x" + std::to_string(header.height));
        return std::nullopt;
    }

    const u64 table_end = static_cast<u64>(D2R_HEADER_SIZE) + static_cast<u64>(mipCount) * D2R_MIP_ENTRY_SIZE;
    if (buffer.size() < table_end) {
        fail("D2R texture too small for mip table");
        return std::nullopt;
    }

    Texture result = Texture::create2D(mapping->format, header.width, header.height, mipCount);
    result.setSrgb(mapping->is_srgb);

    for (u32 mip = 0; mip < mipCount; ++mip) {
        MipEntry entry{};
        std::memcpy(&entry, buffer.data() + D2R_HEADER_SIZE + mip * D2R_MIP_ENTRY_SIZE, sizeof(MipEntry));

        // offset is self-relative to the address of the offset field itself,
        // which sits at (header) + 4 inside entry `mip`.
        const u64 offset_field_pos = static_cast<u64>(D2R_HEADER_SIZE) + mip * D2R_MIP_ENTRY_SIZE + 4;
        const u64 data_pos = offset_field_pos + entry.offset;

        auto destination = result.mipData(mip);
        if (entry.size != destination.size()) {
            fail("D2R mip " + std::to_string(mip) + " size mismatch (file " +
                 std::to_string(entry.size) + ", expected " + std::to_string(destination.size()) +
                 ")");
            return std::nullopt;
        }
        if (data_pos + entry.size > buffer.size()) {
            fail("D2R mip " + std::to_string(mip) + " data out of bounds");
            return std::nullopt;
        }

        const u8* src = buffer.data() + data_pos;
        if (mapping->is_bgra) {
            const u32 mip_width = std::max(header.width >> mip, 1u);
            const u32 mip_height = std::max(header.height >> mip, 1u);
            swizzle_bgra_rgba(src, destination.data(),
                              static_cast<u64>(mip_width) * mip_height);
        } else {
            std::memcpy(destination.data(), src, entry.size);
        }
    }

    if (outInfo) {
        outInfo->formatCode = header.format;
        outInfo->pixelFormat = mapping->format;
        outInfo->isSrgb = mapping->is_srgb;
        outInfo->width = header.width;
        outInfo->height = header.height;
        outInfo->mipCount = mipCount;
    }

    return result;
}

Parser::Parser() : pImpl(std::make_unique<Impl>()) {}

Parser::~Parser() = default;

std::optional<Texture> Parser::parse(const std::string& filePath) {
    pImpl->issues.clear();
    auto buf = read_file_bytes(filePath, *pImpl);
    if (!buf)
        return std::nullopt;
    return pImpl->parse(std::span<const u8>{*buf}, nullptr);
}

std::optional<Texture> Parser::parse(std::span<const u8> buffer) {
    return pImpl->parse(buffer, nullptr);
}

std::optional<Texture> Parser::parse(std::span<const u8> buffer, D2rTextureInfo* outInfo) {
    return pImpl->parse(buffer, outInfo);
}

bool Parser::detect(std::span<const u8> buffer) const {
    return looks_like_d2r_texture(buffer);
}

bool Parser::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Parser::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::d2r_texture

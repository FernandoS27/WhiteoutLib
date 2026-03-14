// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/bmp/parser.h>

#include "bmp_internal.h"

#include <cstring>
#include <memory>
#include <stdexcept>
#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/pixel_convert.h"

namespace whiteout::textures::bmp {

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer);
};

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer) {
    issues.clear();

    const size_t min_size = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    if (buffer.size() < min_size) {
        fail("Buffer too small for a BMP file");
        return std::nullopt;
    }

    BmpFileHeader file_header{};
    std::memcpy(&file_header, buffer.data(), sizeof(BmpFileHeader));
    if (file_header.signature != BMP_SIGNATURE) {
        fail("Invalid BMP signature");
        return std::nullopt;
    }

    BmpInfoHeader info_header{};
    std::memcpy(&info_header, buffer.data() + sizeof(BmpFileHeader), sizeof(BmpInfoHeader));
    if (info_header.headerSize < 40) {
        fail("Unsupported BMP header size: " + std::to_string(info_header.headerSize));
        return std::nullopt;
    }

    if (info_header.planes != 1) {
        fail("Invalid BMP planes value: " + std::to_string(info_header.planes));
        return std::nullopt;
    }

    if (info_header.compression != BI_RGB && info_header.compression != BI_BITFIELDS) {
        fail("Unsupported BMP compression: " + std::to_string(info_header.compression));
        return std::nullopt;
    }

    if (info_header.bitsPerPixel != 24 && info_header.bitsPerPixel != 32) {
        fail("Unsupported BMP bit depth: " + std::to_string(info_header.bitsPerPixel) +
             " (only 24 and 32 supported)");
        return std::nullopt;
    }

    const bool top_down = info_header.height < 0;
    const u32 width = static_cast<u32>(info_header.width);
    const u32 height = static_cast<u32>(top_down ? -info_header.height : info_header.height);

    if (width == 0 || height == 0) {
        fail("BMP has zero dimensions");
        return std::nullopt;
    }

    const u32 row_stride = bmp_row_stride(width, info_header.bitsPerPixel);
    const u64 required_pixel_data = static_cast<u64>(row_stride) * height;

    if (file_header.dataOffset + required_pixel_data > buffer.size()) {
        fail("BMP file truncated");
        return std::nullopt;
    }

    Texture texture = Texture::create2D(PixelFormat::RGBA8, width, height, 1);
    u8* dest_data = texture.dataPtr();
    const u8* pixel_data = buffer.data() + file_header.dataOffset;

    const u32 source_bytes_per_pixel = info_header.bitsPerPixel / 8;

    for (u32 y = 0; y < height; ++y) {
        // BMP default is bottom-up; top-down when height is negative.
        const u32 source_row = top_down ? y : (height - 1 - y);
        const u8* source_line = pixel_data + static_cast<u64>(source_row) * row_stride;
        u8* dest_line = dest_data + static_cast<u64>(y) * width * 4;

        // BMP stores BGR(A); convert to RGBA.
        convert_bgr_to_rgba(source_line, dest_line, width, source_bytes_per_pixel);
    }

    return texture;
}

Parser::Parser(ParseMode parseMode) : pImpl(std::make_unique<Impl>()) {
    pImpl->strict_mode = (parseMode == ParseMode::Strict);
}

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

} // namespace whiteout::textures::bmp

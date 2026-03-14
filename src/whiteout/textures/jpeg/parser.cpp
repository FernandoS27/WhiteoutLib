// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/jpeg/parser.h>

#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/pixel_convert.h"
#include "jpeg_decode.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::jpeg {

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer);
};

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer) {
    issues.clear();

    if (buffer.size() < 2) {
        fail("Buffer too small for a JPEG file");
        return std::nullopt;
    }

    // Verify SOI marker.
    if (buffer[0] != 0xFF || buffer[1] != 0xD8) {
        fail("Invalid JPEG signature (no SOI marker)");
        return std::nullopt;
    }

    // Decode using the raw (no colourspace conversion) baseline decoder.
    std::string decodeError;
    auto image = whiteout::textures::jpeg::decode_raw(buffer, &decodeError);
    if (!image) {
        fail("JPEG decode failed: " + decodeError);
        return std::nullopt;
    }

    const u32 width = image->width;
    const u32 height = image->height;
    const u32 components = image->components;

    if (width == 0 || height == 0) {
        fail("JPEG image has zero dimensions");
        return std::nullopt;
    }

    Texture texture = Texture::create2D(PixelFormat::RGBA8, width, height, 1);
    u8* dest = texture.dataPtr();
    const u8* src = image->pixels.data();

    if (components == 1) {
        // Grayscale: replicate Y to RGB, full alpha.
        for (u32 i = 0; i < width * height; ++i) {
            u8 y = src[i];
            dest[i * 4 + 0] = y;
            dest[i * 4 + 1] = y;
            dest[i * 4 + 2] = y;
            dest[i * 4 + 3] = 255;
        }
    } else if (components == 3) {
        // Standard JPEG Y'CbCr → RGB conversion (ITU-R BT.601).
        for (u32 i = 0; i < width * height; ++i) {
            ycbcr_to_rgb(src[i * 3 + 0], src[i * 3 + 1], src[i * 3 + 2],
                         dest[i * 4 + 0], dest[i * 4 + 1], dest[i * 4 + 2]);
            dest[i * 4 + 3] = 255;
        }
    } else if (components == 4) {
        // 4-component JPEG (Y'CbCrK or CMYK).  Apply Y'CbCr→RGB on the first
        // three channels and use the fourth as alpha.  This is a reasonable
        // fallback — true CMYK-to-RGB requires an ICC profile which we don't
        // parse.
        for (u32 i = 0; i < width * height; ++i) {
            ycbcr_to_rgb(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2],
                         dest[i * 4 + 0], dest[i * 4 + 1], dest[i * 4 + 2]);
            dest[i * 4 + 3] = src[i * 4 + 3];
        }
    } else {
        fail("Unsupported JPEG component count: " + std::to_string(components));
        return std::nullopt;
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

} // namespace whiteout::textures::jpeg

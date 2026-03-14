// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/jpeg/writer.h>

#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/pixel_convert.h"
#include "jpeg_encode.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::jpeg {

class Writer::Impl : public IssueSink {
public:
    i32 quality = 75;

    std::vector<u8> write(const Texture& texture);
};

/// Build a JFIF APP0 marker segment.
/// Returned bytes include the 0xFF 0xE0 marker prefix.
static std::vector<u8> build_jfif_app0() {
    std::vector<u8> seg;
    seg.reserve(20);

    // APP0 marker
    seg.push_back(0xFF);
    seg.push_back(0xE0);

    // Segment length (16 bytes, including this 2-byte length field)
    seg.push_back(0x00);
    seg.push_back(0x10);

    // "JFIF\0" identifier
    seg.push_back('J');
    seg.push_back('F');
    seg.push_back('I');
    seg.push_back('F');
    seg.push_back(0x00);

    // Version 1.01
    seg.push_back(0x01);
    seg.push_back(0x01);

    // Aspect ratio units: 0 = no units (pixel aspect ratio)
    seg.push_back(0x00);

    // X density = 1
    seg.push_back(0x00);
    seg.push_back(0x01);

    // Y density = 1
    seg.push_back(0x00);
    seg.push_back(0x01);

    // No thumbnail
    seg.push_back(0x00);
    seg.push_back(0x00);

    return seg;
}

std::vector<u8> Writer::Impl::write(const Texture& texture) {
    issues.clear();

    if (texture.width() == 0 || texture.height() == 0) {
        fail("Cannot save an empty texture");
        return {};
    }

    // Work with an RGBA8 copy.
    Texture rgba = texture.copyAsFormat(PixelFormat::RGBA8);
    const u32 width = rgba.width();
    const u32 height = rgba.height();
    const u8* src = rgba.dataPtr();

    // Determine if the image is fully opaque.  If it has meaningful alpha,
    // we still encode as 3-component Y'CbCr — JPEG has no standard alpha
    // channel support, so alpha is silently dropped.

    // Build a 3-component Y'CbCr image for the raw encoder.
    // RGB → Y'CbCr (ITU-R BT.601 / JFIF):
    //   Y  =  0.299   * R + 0.587   * G + 0.114   * B
    //   Cb = -0.168736 * R - 0.331264 * G + 0.5     * B + 128
    //   Cr =  0.5      * R - 0.418688 * G - 0.081312 * B + 128

    whiteout::textures::jpeg::Image image;
    image.width = width;
    image.height = height;
    image.components = 3;
    image.pixels.resize(static_cast<size_t>(width) * height * 3);

    for (u32 i = 0; i < width * height; ++i) {
        rgb_to_ycbcr(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2],
                     image.pixels[i * 3 + 0], image.pixels[i * 3 + 1], image.pixels[i * 3 + 2]);
    }

    // Encode using the raw baseline encoder.
    std::string encodeError;
    auto encoded = whiteout::textures::jpeg::encode_raw(image, quality, &encodeError);
    if (encoded.empty()) {
        fail("JPEG encode failed: " + encodeError);
        return {};
    }

    // Insert JFIF APP0 marker after the SOI marker (first 2 bytes).
    auto app0 = build_jfif_app0();
    std::vector<u8> output;
    output.reserve(encoded.size() + app0.size());

    // SOI (2 bytes)
    output.insert(output.end(), encoded.begin(), encoded.begin() + 2);
    // JFIF APP0
    output.insert(output.end(), app0.begin(), app0.end());
    // Rest of the encoded stream (DQT, SOF0, DHT, SOS, entropy data, EOI)
    output.insert(output.end(), encoded.begin() + 2, encoded.end());

    return output;
}

Writer::Writer(i32 quality, WriteMode writeMode) : pImpl(std::make_unique<Impl>()) {
    pImpl->strict_mode = (writeMode == WriteMode::Strict);
    pImpl->quality = std::clamp(quality, 1, 100);
}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const Texture& texture) {
    auto data = pImpl->write(texture);
    if (data.empty()) {
        return;
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

} // namespace whiteout::textures::jpeg

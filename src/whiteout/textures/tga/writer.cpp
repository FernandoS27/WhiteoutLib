// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/tga/writer.h>

#include "tga_internal.h"

#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/color_convert.h"

#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::tga {

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

    // Work with an RGBA8 copy of the texture data.
    Texture rgba_texture = texture.copyAsFormat(PixelFormat::RGBA8);

    const u32 width = rgba_texture.width();
    const u32 height = rgba_texture.height();
    const u32 bits_per_pixel = 32;
    const u64 pixel_data_size = static_cast<u64>(width) * height * 4;
    const u64 total_size = sizeof(TgaHeader) + pixel_data_size;

    std::vector<u8> output(static_cast<size_t>(total_size), 0);

    // TGA header — uncompressed true-color, 32-bit, top-down.
    TgaHeader header{};
    header.idLength = 0;
    header.colorMapType = 0;
    header.imageType = TGA_TYPE_UNCOMPRESSED_TRUE_COLOR;
    header.colorMapOrigin = 0;
    header.colorMapLength = 0;
    header.colorMapDepth = 0;
    header.xOrigin = 0;
    header.yOrigin = 0;
    header.width = static_cast<u16>(width);
    header.height = static_cast<u16>(height);
    header.bitsPerPixel = static_cast<u8>(bits_per_pixel);
    header.imageDescriptor = 0x28; // 8 alpha bits + top-down bit

    std::memcpy(output.data(), &header, sizeof(TgaHeader));

    // Pixel data: RGBA8 → BGRA, top-down order.
    const u8* source_data = rgba_texture.dataPtr();
    u8* dest_data = output.data() + sizeof(TgaHeader);

    for (u32 y = 0; y < height; ++y) {
        const u8* source_line = source_data + static_cast<u64>(y) * width * 4;
        u8* dest_line = dest_data + static_cast<u64>(y) * width * 4;

        swap_red_blue(source_line, dest_line, width);
    }

    return output;
}

Writer::Writer() : pImpl(std::make_unique<Impl>()) {}

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

} // namespace whiteout::textures::tga

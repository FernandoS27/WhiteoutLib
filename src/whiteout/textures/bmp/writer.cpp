// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/bmp/writer.h>

#include "bmp_internal.h"

#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/color_convert.h"

#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::bmp {

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
    const u32 row_stride = bmp_row_stride(width, 32);
    const u64 pixel_data_size = static_cast<u64>(row_stride) * height;

    const u32 data_offset = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    const u64 total_size = data_offset + pixel_data_size;

    std::vector<u8> output(static_cast<size_t>(total_size), 0);

    // File header.
    BmpFileHeader file_header{};
    file_header.signature = BMP_SIGNATURE;
    file_header.fileSize = static_cast<u32>(total_size);
    file_header.reserved1 = 0;
    file_header.reserved2 = 0;
    file_header.dataOffset = data_offset;
    std::memcpy(output.data(), &file_header, sizeof(BmpFileHeader));

    // Info header — write as 32-bit BGRA, bottom-up.
    BmpInfoHeader info_header{};
    info_header.headerSize = sizeof(BmpInfoHeader);
    info_header.width = static_cast<i32>(width);
    info_header.height = static_cast<i32>(height); // positive = bottom-up
    info_header.planes = 1;
    info_header.bitsPerPixel = 32;
    info_header.compression = BI_RGB;
    info_header.imageSize = static_cast<u32>(pixel_data_size);
    info_header.xPixelsPerMeter = 2835; // ~72 DPI
    info_header.yPixelsPerMeter = 2835;
    info_header.colorsUsed = 0;
    info_header.colorsImportant = 0;
    std::memcpy(output.data() + sizeof(BmpFileHeader), &info_header, sizeof(BmpInfoHeader));

    // Pixel data: RGBA8 → BGRA, bottom-up row order.
    const u8* source_data = rgba_texture.dataPtr();
    u8* dest_data = output.data() + data_offset;

    for (u32 y = 0; y < height; ++y) {
        const u32 source_row = y;
        const u32 dest_row = height - 1 - y; // bottom-up
        const u8* source_line = source_data + static_cast<u64>(source_row) * width * 4;
        u8* dest_line = dest_data + static_cast<u64>(dest_row) * row_stride;

        swap_red_blue(source_line, dest_line, width);
    }

    return output;
}

Writer::Writer(WriteMode writeMode) : pImpl(std::make_unique<Impl>()) {
    pImpl->strict_mode = (writeMode == WriteMode::Strict);
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

} // namespace whiteout::textures::bmp

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/tga/parser.h>

#include "tga_internal.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/pixel_convert.h"

namespace whiteout::textures::tga {

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer);

private:
    /// Decode RLE-compressed pixel data.
    /// @return true on success.
    bool decode_rle(const u8* compressed_data, size_t compressed_size, u8* output_pixels,
                    u64 pixel_count, u32 bytes_per_pixel);
};

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer) {
    issues.clear();

    if (buffer.size() < sizeof(TgaHeader)) {
        fail("Buffer too small for a TGA file");
        return std::nullopt;
    }

    TgaHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(TgaHeader));

    const bool is_rle =
        (header.imageType == TGA_TYPE_RLE_TRUE_COLOR || header.imageType == TGA_TYPE_RLE_GRAYSCALE);
    const bool is_uncompressed = (header.imageType == TGA_TYPE_UNCOMPRESSED_TRUE_COLOR ||
                                  header.imageType == TGA_TYPE_UNCOMPRESSED_GRAYSCALE);
    const bool is_grayscale = (header.imageType == TGA_TYPE_UNCOMPRESSED_GRAYSCALE ||
                               header.imageType == TGA_TYPE_RLE_GRAYSCALE);

    if (!is_rle && !is_uncompressed) {
        fail("Unsupported TGA image type: " + std::to_string(header.imageType));
        return std::nullopt;
    }

    if (header.colorMapType != 0) {
        fail("Color-mapped TGA images are not supported");
        return std::nullopt;
    }

    const u32 width = header.width;
    const u32 height = header.height;
    const u32 bits_per_pixel = header.bitsPerPixel;

    if (width == 0 || height == 0) {
        fail("TGA has zero dimensions");
        return std::nullopt;
    }

    if (is_grayscale && bits_per_pixel != 8) {
        fail("Unsupported grayscale bit depth: " + std::to_string(bits_per_pixel));
        return std::nullopt;
    }

    if (!is_grayscale && bits_per_pixel != 24 && bits_per_pixel != 32) {
        fail("Unsupported TGA bit depth: " + std::to_string(bits_per_pixel) +
             " (only 24 and 32 supported for true-color)");
        return std::nullopt;
    }

    const u32 source_bytes_per_pixel = bits_per_pixel / 8;
    const u64 pixel_count = static_cast<u64>(width) * height;
    const size_t data_offset =
        sizeof(TgaHeader) + header.idLength +
        (header.colorMapType ? header.colorMapLength * ((header.colorMapDepth + 7) / 8) : 0);

    if (data_offset > buffer.size()) {
        fail("TGA file truncated (header + ID overflows buffer)");
        return std::nullopt;
    }

    // Decode pixel data into a temporary raw buffer.
    std::vector<u8> raw_pixels(pixel_count * source_bytes_per_pixel);

    if (is_rle) {
        const u8* rle_data = buffer.data() + data_offset;
        const size_t rle_size = buffer.size() - data_offset;
        if (!decode_rle(rle_data, rle_size, raw_pixels.data(), pixel_count,
                        source_bytes_per_pixel)) {
            return std::nullopt;
        }
    } else {
        const u64 required_bytes = pixel_count * source_bytes_per_pixel;
        if (data_offset + required_bytes > buffer.size()) {
            fail("TGA file truncated");
            return std::nullopt;
        }
        std::memcpy(raw_pixels.data(), buffer.data() + data_offset,
                    static_cast<size_t>(required_bytes));
    }

    // Determine output format.
    const PixelFormat output_format = is_grayscale ? PixelFormat::R8 : PixelFormat::RGBA8;
    Texture texture = Texture::create2D(output_format, width, height, 1);
    u8* dest_data = texture.dataPtr();

    const bool top_down = is_top_down(header);

    if (is_grayscale) {
        // 8-bit grayscale → R8, just reorder rows.
        for (u32 y = 0; y < height; ++y) {
            const u32 source_row = top_down ? y : (height - 1 - y);
            std::memcpy(dest_data + static_cast<u64>(y) * width,
                        raw_pixels.data() + static_cast<u64>(source_row) * width, width);
        }
    } else {
        // True-color BGR(A) → RGBA8.
        for (u32 y = 0; y < height; ++y) {
            const u32 source_row = top_down ? y : (height - 1 - y);
            const u8* source_line =
                raw_pixels.data() + static_cast<u64>(source_row) * width * source_bytes_per_pixel;
            u8* dest_line = dest_data + static_cast<u64>(y) * width * 4;

            // True-color BGR(A) → RGBA8.
            convert_bgr_to_rgba(source_line, dest_line, width, source_bytes_per_pixel);
        }
    }

    return texture;
}

bool Parser::Impl::decode_rle(const u8* compressed_data, size_t compressed_size, u8* output_pixels,
                              u64 pixel_count, u32 bytes_per_pixel) {
    u64 pixels_decoded = 0;
    size_t read_pos = 0;

    while (pixels_decoded < pixel_count) {
        if (read_pos >= compressed_size) {
            fail("TGA RLE data truncated");
            return false;
        }

        const u8 packet_header = compressed_data[read_pos++];
        const u32 run_count = (packet_header & 0x7F) + 1;

        if (pixels_decoded + run_count > pixel_count) {
            fail("TGA RLE run overflows image");
            return false;
        }

        if (packet_header & 0x80) {
            // RLE packet: one pixel value repeated run_count times.
            if (read_pos + bytes_per_pixel > compressed_size) {
                fail("TGA RLE data truncated in RLE packet");
                return false;
            }
            const u8* repeated_pixel = compressed_data + read_pos;
            read_pos += bytes_per_pixel;
            for (u32 i = 0; i < run_count; ++i) {
                std::memcpy(output_pixels + (pixels_decoded + i) * bytes_per_pixel, repeated_pixel,
                            bytes_per_pixel);
            }
        } else {
            // Raw packet: run_count literal pixels.
            const u64 raw_bytes = static_cast<u64>(run_count) * bytes_per_pixel;
            if (read_pos + raw_bytes > compressed_size) {
                fail("TGA RLE data truncated in raw packet");
                return false;
            }
            std::memcpy(output_pixels + pixels_decoded * bytes_per_pixel,
                        compressed_data + read_pos, static_cast<size_t>(raw_bytes));
            read_pos += static_cast<size_t>(raw_bytes);
        }

        pixels_decoded += run_count;
    }

    return true;
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

} // namespace whiteout::textures::tga

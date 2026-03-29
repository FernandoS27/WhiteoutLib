// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/png/parser.h>

#include "deflate.h"
#include "png_internal.h"

#include "../io_helpers.h"
#include "../issue_sink.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::png {

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer);

private:
    // IHDR fields.
    u32 imgWidth = 0;
    u32 imgHeight = 0;
    u8 bitDepth = 0;
    u8 colorType = 0;
    u8 interlaceMethod = 0;

    // Palette and transparency.
    std::vector<u8> palette;  // PLTE: R,G,B triples.
    std::vector<u8> trnsData; // tRNS chunk raw data.

    /// Number of channels for the raw image (before expansion to RGBA).
    u32 rawChannels() const;

    /// Bytes per pixel in the raw scanline (before expansion).
    u32 rawBytesPerPixel() const;

    /// Reconstruct filtered scanlines in-place.
    bool unfilterScanlines(u8* data, u32 width, u32 height, u32 bpp);

    /// Convert defiltered raw data to RGBA8 pixels.
    bool convertToRGBA8(const u8* raw, u32 rawStride, u8* dest);
};

u32 Parser::Impl::rawChannels() const {
    switch (colorType) {
    case COLOR_GRAYSCALE:
        return 1;
    case COLOR_TRUECOLOR:
        return 3;
    case COLOR_INDEXED:
        return 1;
    case COLOR_GRAYSCALE_ALPHA:
        return 2;
    case COLOR_TRUECOLOR_ALPHA:
        return 4;
    default:
        return 0;
    }
}

u32 Parser::Impl::rawBytesPerPixel() const {
    u32 ch = rawChannels();
    u32 bitsPerPixel = ch * bitDepth;
    return std::max(1u, bitsPerPixel / 8);
}

bool Parser::Impl::unfilterScanlines(u8* data, u32 width, u32 height, u32 bpp) {
    // Each scanline: 1 filter byte + ceil(width * bitsPerPixel / 8) data bytes.
    u32 bitsPerPixel = rawChannels() * bitDepth;
    u32 stride = (width * bitsPerPixel + 7) / 8;

    u32 rowSize = 1 + stride; // filter byte + pixel data
    // bpp for filter purposes = bytes per complete pixel (min 1).

    for (u32 y = 0; y < height; ++y) {
        u8* row = data + static_cast<size_t>(y) * rowSize;
        u8 filterType = row[0];
        u8* cur = row + 1;
        const u8* prev = (y > 0) ? (data + static_cast<size_t>(y - 1) * rowSize + 1) : nullptr;

        switch (filterType) {
        case FILTER_NONE:
            break;

        case FILTER_SUB:
            for (u32 x = bpp; x < stride; ++x) {
                cur[x] = static_cast<u8>(cur[x] + cur[x - bpp]);
            }
            break;

        case FILTER_UP:
            if (prev) {
                for (u32 x = 0; x < stride; ++x) {
                    cur[x] = static_cast<u8>(cur[x] + prev[x]);
                }
            }
            break;

        case FILTER_AVERAGE:
            for (u32 x = 0; x < stride; ++x) {
                u8 a = (x >= bpp) ? cur[x - bpp] : 0;
                u8 b = prev ? prev[x] : 0;
                cur[x] = static_cast<u8>(cur[x] + ((static_cast<u32>(a) + b) >> 1));
            }
            break;

        case FILTER_PAETH:
            for (u32 x = 0; x < stride; ++x) {
                u8 a = (x >= bpp) ? cur[x - bpp] : 0;
                u8 b = prev ? prev[x] : 0;
                u8 c = (prev && x >= bpp) ? prev[x - bpp] : 0;
                cur[x] = static_cast<u8>(cur[x] + paethPredictor(a, b, c));
            }
            break;

        default:
            fail("Unknown PNG filter type: " + std::to_string(filterType));
            return false;
        }
    }
    return true;
}

bool Parser::Impl::convertToRGBA8(const u8* raw, u32 rawStride, u8* dest) {
    u32 rowBytes = rawStride; // Bytes of pixel data per row (after filter byte).

    for (u32 y = 0; y < imgHeight; ++y) {
        const u8* row = raw + static_cast<size_t>(y) * (1 + rowBytes) + 1; // Skip filter byte.
        u8* out = dest + static_cast<size_t>(y) * imgWidth * 4;

        if (colorType == COLOR_TRUECOLOR_ALPHA && bitDepth == 8) {
            std::memcpy(out, row, imgWidth * 4);
        } else if (colorType == COLOR_TRUECOLOR && bitDepth == 8) {
            for (u32 x = 0; x < imgWidth; ++x) {
                out[x * 4 + 0] = row[x * 3 + 0];
                out[x * 4 + 1] = row[x * 3 + 1];
                out[x * 4 + 2] = row[x * 3 + 2];
                out[x * 4 + 3] = 255;
            }
        } else if (colorType == COLOR_GRAYSCALE && bitDepth == 8) {
            // Check tRNS for transparent gray value.
            bool hasTrns = trnsData.size() >= 2;
            u8 trnsGray = hasTrns ? trnsData[1] : 0; // 16-bit BE, use low byte for 8-bit.
            for (u32 x = 0; x < imgWidth; ++x) {
                u8 g = row[x];
                out[x * 4 + 0] = g;
                out[x * 4 + 1] = g;
                out[x * 4 + 2] = g;
                out[x * 4 + 3] = (hasTrns && g == trnsGray) ? 0 : 255;
            }
        } else if (colorType == COLOR_GRAYSCALE_ALPHA && bitDepth == 8) {
            for (u32 x = 0; x < imgWidth; ++x) {
                u8 g = row[x * 2 + 0];
                out[x * 4 + 0] = g;
                out[x * 4 + 1] = g;
                out[x * 4 + 2] = g;
                out[x * 4 + 3] = row[x * 2 + 1];
            }
        } else if (colorType == COLOR_INDEXED && bitDepth == 8) {
            u32 paletteCount = static_cast<u32>(palette.size() / 3);
            for (u32 x = 0; x < imgWidth; ++x) {
                u8 idx = row[x];
                if (idx >= paletteCount) {
                    out[x * 4 + 0] = 0;
                    out[x * 4 + 1] = 0;
                    out[x * 4 + 2] = 0;
                    out[x * 4 + 3] = 255;
                } else {
                    out[x * 4 + 0] = palette[idx * 3 + 0];
                    out[x * 4 + 1] = palette[idx * 3 + 1];
                    out[x * 4 + 2] = palette[idx * 3 + 2];
                    out[x * 4 + 3] = (idx < trnsData.size()) ? trnsData[idx] : 255;
                }
            }
        } else if (colorType == COLOR_INDEXED && bitDepth < 8) {
            // Sub-byte indexed: 1, 2, or 4 bits per pixel.
            u32 pixelsPerByte = 8 / bitDepth;
            u32 mask = (1u << bitDepth) - 1;
            u32 paletteCount = static_cast<u32>(palette.size() / 3);
            for (u32 x = 0; x < imgWidth; ++x) {
                u32 byteIdx = x / pixelsPerByte;
                u32 bitIdx = (pixelsPerByte - 1 - (x % pixelsPerByte)) * bitDepth;
                u8 idx = (row[byteIdx] >> bitIdx) & mask;
                if (idx >= paletteCount) {
                    out[x * 4 + 0] = 0;
                    out[x * 4 + 1] = 0;
                    out[x * 4 + 2] = 0;
                    out[x * 4 + 3] = 255;
                } else {
                    out[x * 4 + 0] = palette[idx * 3 + 0];
                    out[x * 4 + 1] = palette[idx * 3 + 1];
                    out[x * 4 + 2] = palette[idx * 3 + 2];
                    out[x * 4 + 3] = (idx < trnsData.size()) ? trnsData[idx] : 255;
                }
            }
        } else if (colorType == COLOR_GRAYSCALE && bitDepth < 8) {
            // Sub-byte grayscale: 1, 2, or 4 bits per pixel.
            u32 pixelsPerByte = 8 / bitDepth;
            u32 mask = (1u << bitDepth) - 1;
            u32 maxVal = mask;
            for (u32 x = 0; x < imgWidth; ++x) {
                u32 byteIdx = x / pixelsPerByte;
                u32 bitIdx = (pixelsPerByte - 1 - (x % pixelsPerByte)) * bitDepth;
                u8 val = (row[byteIdx] >> bitIdx) & mask;
                u8 expanded = static_cast<u8>((static_cast<u32>(val) * 255 + maxVal / 2) / maxVal);
                out[x * 4 + 0] = expanded;
                out[x * 4 + 1] = expanded;
                out[x * 4 + 2] = expanded;
                out[x * 4 + 3] = 255;
            }
        } else if (bitDepth == 16) {
            // 16-bit channels — downsample to 8-bit.
            u32 ch = rawChannels();
            for (u32 x = 0; x < imgWidth; ++x) {
                const u8* px = row + static_cast<size_t>(x) * ch * 2;
                if (colorType == COLOR_TRUECOLOR_ALPHA) {
                    out[x * 4 + 0] = px[0]; // High byte of R
                    out[x * 4 + 1] = px[2]; // High byte of G
                    out[x * 4 + 2] = px[4]; // High byte of B
                    out[x * 4 + 3] = px[6]; // High byte of A
                } else if (colorType == COLOR_TRUECOLOR) {
                    out[x * 4 + 0] = px[0];
                    out[x * 4 + 1] = px[2];
                    out[x * 4 + 2] = px[4];
                    out[x * 4 + 3] = 255;
                } else if (colorType == COLOR_GRAYSCALE_ALPHA) {
                    out[x * 4 + 0] = px[0];
                    out[x * 4 + 1] = px[0];
                    out[x * 4 + 2] = px[0];
                    out[x * 4 + 3] = px[2];
                } else if (colorType == COLOR_GRAYSCALE) {
                    out[x * 4 + 0] = px[0];
                    out[x * 4 + 1] = px[0];
                    out[x * 4 + 2] = px[0];
                    out[x * 4 + 3] = 255;
                }
            }
        } else {
            fail("Unsupported PNG format: colorType=" + std::to_string(colorType) +
                 " bitDepth=" + std::to_string(bitDepth));
            return false;
        }
    }
    return true;
}

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer) {
    issues.clear();
    palette.clear();
    trnsData.clear();

    // Verify PNG signature.
    if (buffer.size() < 8 + 25) { // Signature + minimum IHDR chunk
        fail("Buffer too small for a PNG file");
        return std::nullopt;
    }
    if (std::memcmp(buffer.data(), PNG_SIGNATURE.data(), 8) != 0) {
        fail("Invalid PNG signature");
        return std::nullopt;
    }

    // Parse chunks.
    size_t pos = 8;
    bool foundIHDR = false;
    std::vector<u8> compressedData; // Concatenated IDAT data.

    while (pos + 12 <= buffer.size()) {
        u32 chunkLen = readU32BE(buffer.data() + pos);
        u32 chunkType = readU32BE(buffer.data() + pos + 4);

        if (pos + 12 + chunkLen > buffer.size()) {
            fail("PNG chunk extends beyond file");
            return std::nullopt;
        }

        const u8* chunkData = buffer.data() + pos + 8;

        // Verify CRC (covers type + data).
        u32 storedCrc = readU32BE(buffer.data() + pos + 8 + chunkLen);
        u32 computedCrc = crc32(buffer.data() + pos + 4, 4 + chunkLen);
        if (storedCrc != computedCrc) {
            fail("PNG chunk CRC mismatch");
            return std::nullopt;
        }

        switch (chunkType) {
        case CHUNK_IHDR: {
            if (chunkLen < 13) {
                fail("IHDR chunk too small");
                return std::nullopt;
            }
            imgWidth = readU32BE(chunkData);
            imgHeight = readU32BE(chunkData + 4);
            bitDepth = chunkData[8];
            colorType = chunkData[9];
            u8 compression = chunkData[10];
            u8 filter = chunkData[11];
            interlaceMethod = chunkData[12];

            if (imgWidth == 0 || imgHeight == 0) {
                fail("PNG has zero dimensions");
                return std::nullopt;
            }
            if (compression != 0) {
                fail("Unknown PNG compression method");
                return std::nullopt;
            }
            if (filter != 0) {
                fail("Unknown PNG filter method");
                return std::nullopt;
            }
            if (interlaceMethod != 0 && interlaceMethod != 1) {
                fail("Unknown PNG interlace method");
                return std::nullopt;
            }
            if (interlaceMethod == 1) {
                fail("Adam7 interlaced PNG is not supported");
                return std::nullopt;
            }

            // Validate bit depth / color type combinations.
            bool validCombo = false;
            if (colorType == COLOR_GRAYSCALE)
                validCombo = (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 ||
                              bitDepth == 16);
            else if (colorType == COLOR_TRUECOLOR)
                validCombo = (bitDepth == 8 || bitDepth == 16);
            else if (colorType == COLOR_INDEXED)
                validCombo = (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8);
            else if (colorType == COLOR_GRAYSCALE_ALPHA)
                validCombo = (bitDepth == 8 || bitDepth == 16);
            else if (colorType == COLOR_TRUECOLOR_ALPHA)
                validCombo = (bitDepth == 8 || bitDepth == 16);

            if (!validCombo) {
                fail("Invalid bit depth / color type combination");
                return std::nullopt;
            }
            foundIHDR = true;
            break;
        }

        case CHUNK_PLTE: {
            if (chunkLen % 3 != 0) {
                fail("PLTE chunk length not a multiple of 3");
                return std::nullopt;
            }
            palette.assign(chunkData, chunkData + chunkLen);
            break;
        }

        case CHUNK_tRNS: {
            trnsData.assign(chunkData, chunkData + chunkLen);
            break;
        }

        case CHUNK_IDAT: {
            compressedData.insert(compressedData.end(), chunkData, chunkData + chunkLen);
            break;
        }

        case CHUNK_IEND:
            goto done_parsing;

        default:
            // Skip unknown chunks.
            break;
        }

        pos += 12 + chunkLen;
    }

done_parsing:
    if (!foundIHDR) {
        fail("No IHDR chunk found");
        return std::nullopt;
    }

    if (colorType == COLOR_INDEXED && palette.empty()) {
        fail("Indexed PNG missing PLTE chunk");
        return std::nullopt;
    }

    if (compressedData.empty()) {
        fail("No IDAT data found");
        return std::nullopt;
    }

    // Decompress the IDAT data.
    std::string zlibError;
    auto rawData = zlib_decompress(
        std::span<const u8>(compressedData.data(), compressedData.size()), &zlibError);
    if (rawData.empty()) {
        fail("Failed to decompress PNG data: " + zlibError);
        return std::nullopt;
    }

    // Validate decompressed size.
    u32 bitsPerPixel = rawChannels() * bitDepth;
    u32 rawStride = (imgWidth * bitsPerPixel + 7) / 8;
    size_t expectedSize = static_cast<size_t>(imgHeight) * (1 + rawStride);
    if (rawData.size() < expectedSize) {
        fail("Decompressed PNG data too small (expected " + std::to_string(expectedSize) +
             ", got " + std::to_string(rawData.size()) + ")");
        return std::nullopt;
    }

    // Unfilter scanlines.
    u32 bpp = rawBytesPerPixel();
    if (!unfilterScanlines(rawData.data(), imgWidth, imgHeight, bpp)) {
        return std::nullopt;
    }

    // Convert to RGBA8.
    Texture texture = Texture::create2D(PixelFormat::RGBA8, imgWidth, imgHeight, 1);
    if (!convertToRGBA8(rawData.data(), rawStride, texture.dataPtr())) {
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

} // namespace whiteout::textures::png

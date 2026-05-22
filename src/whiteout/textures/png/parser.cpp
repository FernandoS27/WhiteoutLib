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

    // APNG results — populated by parse(), read by the public accessors.
    bool isApng = false;
    u32 actlFrames = 0;
    u32 actlPlays = 0;
    std::vector<Texture> compositedFrames;
    std::vector<ApngFrameInfo> frameInfos;

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

    // APNG decode state.
    struct FrameRecord {
        FcTL fctl;
        std::vector<u8> stream; // Concatenated zlib stream for this frame.
    };
    std::vector<FrameRecord> frameRecords;
    bool sawIDAT = false;
    bool sawFcTLBeforeIDAT = false;
    u32 expectedSeq = 0;

    /// Number of channels for the raw image (before expansion to RGBA).
    u32 rawChannels() const;

    /// Bytes per pixel in the raw scanline (before expansion).
    u32 rawBytesPerPixel() const;

    /// Reconstruct filtered scanlines in-place.
    bool unfilterScanlines(u8* data, u32 width, u32 height, u32 bpp);

    /// Convert defiltered raw data to a tight or strided RGBA8 buffer.
    bool convertToRGBA8(const u8* raw, u32 rawStride, u32 frameW, u32 frameH, u8* dest,
                        u32 destStride);

    /// Decompress + unfilter + convert one frame's zlib stream into a tight
    /// RGBA8 buffer (frameW * frameH * 4 bytes). Returns nullopt on failure.
    std::optional<std::vector<u8>> decodeFrameStream(std::span<const u8> zlibStream, u32 frameW,
                                                     u32 frameH);

    /// Decode every APNG frame record and composite it onto the canvas,
    /// populating compositedFrames / frameInfos. Returns false on failure.
    bool compositeFrames();
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
    u32 const ch = rawChannels();
    u32 const bitsPerPixel = ch * bitDepth;
    return std::max(1u, bitsPerPixel / 8);
}

bool Parser::Impl::unfilterScanlines(u8* data, u32 width, u32 height, u32 bpp) {
    // Each scanline: 1 filter byte + ceil(width * bitsPerPixel / 8) data bytes.
    u32 const bitsPerPixel = rawChannels() * bitDepth;
    u32 const stride = (width * bitsPerPixel + 7) / 8;

    u32 const rowSize = 1 + stride; // filter byte + pixel data
    // bpp for filter purposes = bytes per complete pixel (min 1).

    for (u32 y = 0; y < height; ++y) {
        u8* row = data + static_cast<size_t>(y) * rowSize;
        u8 const filterType = row[0];
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
                u8 const a = (x >= bpp) ? cur[x - bpp] : 0;
                u8 const b = prev ? prev[x] : 0;
                cur[x] = static_cast<u8>(cur[x] + ((static_cast<u32>(a) + b) >> 1));
            }
            break;

        case FILTER_PAETH:
            for (u32 x = 0; x < stride; ++x) {
                u8 const a = (x >= bpp) ? cur[x - bpp] : 0;
                u8 const b = prev ? prev[x] : 0;
                u8 const c = (prev && x >= bpp) ? prev[x - bpp] : 0;
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

bool Parser::Impl::convertToRGBA8(const u8* raw, u32 rawStride, u32 frameW, u32 frameH, u8* dest,
                                  u32 destStride) {
    u32 const rowBytes = rawStride; // Bytes of pixel data per row (after filter byte).

    for (u32 y = 0; y < frameH; ++y) {
        const u8* row = raw + static_cast<size_t>(y) * (1 + rowBytes) + 1; // Skip filter byte.
        u8* out = dest + static_cast<size_t>(y) * destStride;

        if (colorType == COLOR_TRUECOLOR_ALPHA && bitDepth == 8) {
            std::memcpy(out, row, static_cast<size_t>(frameW) * 4);
        } else if (colorType == COLOR_TRUECOLOR && bitDepth == 8) {
            for (u32 x = 0; x < frameW; ++x) {
                out[x * 4 + 0] = row[x * 3 + 0];
                out[x * 4 + 1] = row[x * 3 + 1];
                out[x * 4 + 2] = row[x * 3 + 2];
                out[x * 4 + 3] = 255;
            }
        } else if (colorType == COLOR_GRAYSCALE && bitDepth == 8) {
            // Check tRNS for transparent gray value.
            bool const hasTrns = trnsData.size() >= 2;
            u8 const trnsGray = hasTrns ? trnsData[1] : 0; // 16-bit BE, use low byte for 8-bit.
            for (u32 x = 0; x < frameW; ++x) {
                u8 const g = row[x];
                out[x * 4 + 0] = g;
                out[x * 4 + 1] = g;
                out[x * 4 + 2] = g;
                out[x * 4 + 3] = (hasTrns && g == trnsGray) ? 0 : 255;
            }
        } else if (colorType == COLOR_GRAYSCALE_ALPHA && bitDepth == 8) {
            for (u32 x = 0; x < frameW; ++x) {
                u8 const g = row[x * 2 + 0];
                out[x * 4 + 0] = g;
                out[x * 4 + 1] = g;
                out[x * 4 + 2] = g;
                out[x * 4 + 3] = row[x * 2 + 1];
            }
        } else if (colorType == COLOR_INDEXED && bitDepth == 8) {
            u32 const paletteCount = static_cast<u32>(palette.size() / 3);
            for (u32 x = 0; x < frameW; ++x) {
                u8 const idx = row[x];
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
            u32 const pixelsPerByte = 8 / bitDepth;
            u32 const mask = (1u << bitDepth) - 1;
            u32 const paletteCount = static_cast<u32>(palette.size() / 3);
            for (u32 x = 0; x < frameW; ++x) {
                u32 const byteIdx = x / pixelsPerByte;
                u32 const bitIdx = (pixelsPerByte - 1 - (x % pixelsPerByte)) * bitDepth;
                u8 const idx = (row[byteIdx] >> bitIdx) & mask;
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
            u32 const pixelsPerByte = 8 / bitDepth;
            u32 const mask = (1u << bitDepth) - 1;
            u32 const maxVal = mask;
            for (u32 x = 0; x < frameW; ++x) {
                u32 const byteIdx = x / pixelsPerByte;
                u32 const bitIdx = (pixelsPerByte - 1 - (x % pixelsPerByte)) * bitDepth;
                u8 const val = (row[byteIdx] >> bitIdx) & mask;
                u8 const expanded =
                    static_cast<u8>((static_cast<u32>(val) * 255 + maxVal / 2) / maxVal);
                out[x * 4 + 0] = expanded;
                out[x * 4 + 1] = expanded;
                out[x * 4 + 2] = expanded;
                out[x * 4 + 3] = 255;
            }
        } else if (bitDepth == 16) {
            // 16-bit channels — downsample to 8-bit.
            u32 const ch = rawChannels();
            for (u32 x = 0; x < frameW; ++x) {
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

    // Reset APNG state.
    isApng = false;
    actlFrames = 0;
    actlPlays = 0;
    compositedFrames.clear();
    frameInfos.clear();
    frameRecords.clear();
    sawIDAT = false;
    sawFcTLBeforeIDAT = false;
    expectedSeq = 0;

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
        bool exit = false;
        u32 const chunkLen = readU32BE(buffer.data() + pos);
        u32 const chunkType = readU32BE(buffer.data() + pos + 4);

        if (pos + 12 + chunkLen > buffer.size()) {
            fail("PNG chunk extends beyond file");
            return std::nullopt;
        }

        const u8* chunkData = buffer.data() + pos + 8;

        // Verify CRC (covers type + data).
        u32 const storedCrc = readU32BE(buffer.data() + pos + 8 + chunkLen);
        u32 const computedCrc = crc32(buffer.data() + pos + 4, 4 + chunkLen);
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
            u8 const compression = chunkData[10];
            u8 const filter = chunkData[11];
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
            switch (colorType) {
            case COLOR_GRAYSCALE:
                validCombo = (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
                              bitDepth == 8 || bitDepth == 16);
                break;
            case COLOR_INDEXED:
                validCombo = (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8);
                break;
            case COLOR_TRUECOLOR:
            case COLOR_GRAYSCALE_ALPHA:
            case COLOR_TRUECOLOR_ALPHA:
                validCombo = (bitDepth == 8 || bitDepth == 16);
                break;
            default:
                break;
            }

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
            sawIDAT = true;
            // When an fcTL precedes IDAT, IDAT also carries frame 0's data.
            if (sawFcTLBeforeIDAT && !frameRecords.empty()) {
                frameRecords.front().stream.insert(frameRecords.front().stream.end(), chunkData,
                                                   chunkData + chunkLen);
            }
            break;
        }

        case CHUNK_acTL: {
            if (chunkLen != 8) {
                fail("APNG acTL chunk has invalid size");
                return std::nullopt;
            }
            actlFrames = readU32BE(chunkData);
            actlPlays = readU32BE(chunkData + 4);
            isApng = true;
            break;
        }

        case CHUNK_fcTL: {
            if (chunkLen != 26) {
                fail("APNG fcTL chunk has invalid size");
                return std::nullopt;
            }
            FcTL fctl = readFcTL(chunkData);
            if (fctl.sequenceNumber != expectedSeq) {
                fail("APNG fcTL sequence number out of order (expected " +
                     std::to_string(expectedSeq) + ", got " +
                     std::to_string(fctl.sequenceNumber) + ")");
            }
            expectedSeq = fctl.sequenceNumber + 1;
            if (!sawIDAT) {
                sawFcTLBeforeIDAT = true;
            }
            frameRecords.push_back(FrameRecord{fctl, {}});
            break;
        }

        case CHUNK_fdAT: {
            if (chunkLen < 4) {
                fail("APNG fdAT chunk too small");
                return std::nullopt;
            }
            u32 const seq = readU32BE(chunkData);
            if (seq != expectedSeq) {
                fail("APNG fdAT sequence number out of order (expected " +
                     std::to_string(expectedSeq) + ", got " + std::to_string(seq) + ")");
            }
            expectedSeq = seq + 1;
            if (frameRecords.empty()) {
                fail("APNG fdAT chunk before any fcTL chunk");
            } else {
                frameRecords.back().stream.insert(frameRecords.back().stream.end(),
                                                  chunkData + 4, chunkData + chunkLen);
            }
            break;
        }

        case CHUNK_IEND:
            exit = true;
            break;

        default:
            // Skip unknown chunks.
            break;
        }

        if (exit) {
            break;
        }

        pos += 12 + chunkLen;
    }

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

    // Decode the default image (the concatenated IDAT data).
    auto defaultRgba = decodeFrameStream(std::span<const u8>(compressedData), imgWidth, imgHeight);
    if (!defaultRgba) {
        return std::nullopt;
    }
    Texture texture = Texture::create2D(PixelFormat::RGBA8, imgWidth, imgHeight, 1);
    std::memcpy(texture.dataPtr(), defaultRgba->data(), defaultRgba->size());

    // Decode and composite APNG animation frames, if present.
    if (isApng && !frameRecords.empty()) {
        if (!compositeFrames()) {
            compositedFrames.clear();
            frameInfos.clear();
        }
    }

    return texture;
}

std::optional<std::vector<u8>> Parser::Impl::decodeFrameStream(std::span<const u8> zlibStream,
                                                               u32 frameW, u32 frameH) {
    std::string zlibError;
    auto rawData = zlib_decompress(zlibStream, &zlibError);
    if (rawData.empty()) {
        fail("Failed to decompress PNG data: " + zlibError);
        return std::nullopt;
    }

    // Validate decompressed size against the frame's own dimensions.
    u32 const bitsPerPixel = rawChannels() * bitDepth;
    u32 const rawStride = (frameW * bitsPerPixel + 7) / 8;
    size_t const expectedSize = static_cast<size_t>(frameH) * (1 + rawStride);
    if (rawData.size() < expectedSize) {
        fail("Decompressed PNG data too small (expected " + std::to_string(expectedSize) +
             ", got " + std::to_string(rawData.size()) + ")");
        return std::nullopt;
    }

    u32 const bpp = rawBytesPerPixel();
    if (!unfilterScanlines(rawData.data(), frameW, frameH, bpp)) {
        return std::nullopt;
    }

    std::vector<u8> rgba(static_cast<size_t>(frameW) * frameH * 4);
    if (!convertToRGBA8(rawData.data(), rawStride, frameW, frameH, rgba.data(), frameW * 4)) {
        return std::nullopt;
    }
    return rgba;
}

bool Parser::Impl::compositeFrames() {
    u32 const cw = imgWidth;
    u32 const ch = imgHeight;
    std::vector<u8> canvas(static_cast<size_t>(cw) * ch * 4, 0); // transparent black
    std::vector<u8> prevSnapshot;

    compositedFrames.clear();
    frameInfos.clear();
    compositedFrames.reserve(frameRecords.size());
    frameInfos.reserve(frameRecords.size());

    for (size_t i = 0; i < frameRecords.size(); ++i) {
        const FcTL& f = frameRecords[i].fctl;

        // Validate the frame rectangle against the canvas.
        if (f.width == 0 || f.height == 0 ||
            static_cast<u64>(f.xOffset) + f.width > cw ||
            static_cast<u64>(f.yOffset) + f.height > ch) {
            fail("APNG frame rectangle is out of canvas bounds");
            return false;
        }

        auto sub = decodeFrameStream(std::span<const u8>(frameRecords[i].stream), f.width,
                                     f.height);
        if (!sub) {
            return false;
        }

        // PREVIOUS on the first frame is treated as BACKGROUND per the spec.
        u8 dispose = f.disposeOp;
        if (i == 0 && dispose == DISPOSE_PREVIOUS) {
            dispose = DISPOSE_BACKGROUND;
        }
        if (dispose == DISPOSE_PREVIOUS) {
            prevSnapshot = canvas; // snapshot before drawing
        }

        // Composite the frame's sub-rectangle onto the canvas.
        for (u32 y = 0; y < f.height; ++y) {
            for (u32 x = 0; x < f.width; ++x) {
                const u8* src = sub->data() + (static_cast<size_t>(y) * f.width + x) * 4;
                u8* dst = canvas.data() +
                          (static_cast<size_t>(f.yOffset + y) * cw + (f.xOffset + x)) * 4;
                if (f.blendOp == BLEND_SOURCE) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = src[3];
                } else {
                    // BLEND_OVER — 8-bit straight-alpha "over" composite.
                    u32 const sa = src[3];
                    u32 const da = dst[3];
                    u32 const oa = sa + da * (255 - sa) / 255;
                    if (oa == 0) {
                        dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    } else {
                        for (int c = 0; c < 3; ++c) {
                            u32 const v = static_cast<u32>(src[c]) * sa +
                                          static_cast<u32>(dst[c]) * da * (255 - sa) / 255;
                            dst[c] = static_cast<u8>((v + oa / 2) / oa);
                        }
                        dst[3] = static_cast<u8>(oa);
                    }
                }
            }
        }

        // The fully-composited canvas is this frame's output image.
        Texture frameTex = Texture::create2D(PixelFormat::RGBA8, cw, ch, 1);
        std::memcpy(frameTex.dataPtr(), canvas.data(), canvas.size());
        compositedFrames.push_back(std::move(frameTex));

        ApngFrameInfo info;
        info.width = f.width;
        info.height = f.height;
        info.xOffset = f.xOffset;
        info.yOffset = f.yOffset;
        info.delayMs = static_cast<u32>(static_cast<u64>(f.delayNum) * 1000 /
                                        (f.delayDen == 0 ? 100 : f.delayDen));
        info.disposeOp = f.disposeOp;
        info.blendOp = f.blendOp;
        frameInfos.push_back(info);

        // Apply disposal in preparation for the next frame.
        switch (dispose) {
        case DISPOSE_BACKGROUND:
            for (u32 y = 0; y < f.height; ++y) {
                std::memset(canvas.data() +
                                (static_cast<size_t>(f.yOffset + y) * cw + f.xOffset) * 4,
                            0, static_cast<size_t>(f.width) * 4);
            }
            break;
        case DISPOSE_PREVIOUS:
            canvas = prevSnapshot;
            break;
        case DISPOSE_NONE:
        default:
            break;
        }
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

bool Parser::isAnimated() const {
    return pImpl->isApng;
}

u32 Parser::frameCount() const {
    return pImpl->isApng ? pImpl->actlFrames : 0u;
}

u32 Parser::loopCount() const {
    return pImpl->actlPlays;
}

const Texture& Parser::frame(u32 index) const {
    if (index >= pImpl->compositedFrames.size()) {
        if (pImpl->strict_mode) {
            throw std::out_of_range("APNG frame index out of range");
        }
        static const Texture empty;
        return empty;
    }
    return pImpl->compositedFrames[index];
}

u32 Parser::frameDelayMs(u32 index) const {
    if (index >= pImpl->frameInfos.size()) {
        if (pImpl->strict_mode) {
            throw std::out_of_range("APNG frame index out of range");
        }
        return 0;
    }
    return pImpl->frameInfos[index].delayMs;
}

const ApngFrameInfo& Parser::frameInfo(u32 index) const {
    if (index >= pImpl->frameInfos.size()) {
        if (pImpl->strict_mode) {
            throw std::out_of_range("APNG frame index out of range");
        }
        static const ApngFrameInfo empty;
        return empty;
    }
    return pImpl->frameInfos[index];
}

} // namespace whiteout::textures::png

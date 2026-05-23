// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/png/writer.h>

#include "deflate.h"
#include "png_internal.h"

#include "../io_helpers.h"
#include "../issue_sink.h"

#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace whiteout::textures::png {

class Writer::Impl : public IssueSink {
public:
    std::vector<u8> write(const Texture& texture);
    std::vector<u8> writeAnimated(const std::vector<ApngFrame>& frames,
                                  const ApngSaveOptions& opts);

private:
    /// Append a PNG chunk (type + data + CRC32) to the output buffer.
    static void writeChunk(std::vector<u8>& out, u32 chunkType, const u8* data, u32 length);

    /// Apply sub-byte-level filter selection per scanline.
    /// Returns filtered image data with filter bytes prepended per row.
    static std::vector<u8> filterScanlines(const u8* rgba, u32 width, u32 height);
};

void Writer::Impl::writeChunk(std::vector<u8>& out, u32 chunkType, const u8* data, u32 length) {
    size_t const startPos = out.size();
    out.resize(startPos + 12 + length);

    u8* p = out.data() + startPos;
    writeU32BE(p, length);

    // Type (big-endian u32).
    writeU32BE(p + 4, chunkType);

    // Data.
    if (length > 0 && data) {
        std::memcpy(p + 8, data, length);
    }

    // CRC covers type + data.
    u32 const c = crc32(p + 4, 4 + length);
    writeU32BE(p + 8 + length, c);
}

std::vector<u8> Writer::Impl::filterScanlines(const u8* rgba, u32 width, u32 height) {
    // We write as RGBA8 (color type 6), so stride = width * 4.
    const u32 bpp = 4;
    const u32 stride = width * bpp;
    const size_t totalSize = static_cast<size_t>(height) * (1 + stride);
    std::vector<u8> filtered(totalSize);

    for (u32 y = 0; y < height; ++y) {
        const u8* cur = rgba + static_cast<size_t>(y) * stride;
        const u8* prev = (y > 0) ? (rgba + static_cast<size_t>(y - 1) * stride) : nullptr;
        u8* out = filtered.data() + static_cast<size_t>(y) * (1 + stride);

        // Use a simple heuristic: test None and Sub, pick the one with lower
        // absolute sum (results in better compression without much cost).
        u64 sumNone = 0;
        u64 sumSub = 0;
        u64 sumUp = 0;

        for (u32 x = 0; x < stride; ++x) {
            sumNone += cur[x] > 127 ? (256 - cur[x]) : cur[x];

            u8 const a = (x >= bpp) ? cur[x - bpp] : 0;
            u8 const sub = static_cast<u8>(cur[x] - a);
            sumSub += sub > 127 ? (256 - sub) : sub;

            u8 const b = prev ? prev[x] : 0;
            u8 const up = static_cast<u8>(cur[x] - b);
            sumUp += up > 127 ? (256 - up) : up;
        }

        u8 bestFilter = FILTER_NONE;
        u64 bestSum = sumNone;
        if (sumSub < bestSum) {
            bestFilter = FILTER_SUB;
            bestSum = sumSub;
        }
        if (sumUp < bestSum) {
            bestFilter = FILTER_UP;
        }

        out[0] = bestFilter;
        switch (bestFilter) {
        case FILTER_NONE:
            std::memcpy(out + 1, cur, stride);
            break;
        case FILTER_SUB:
            for (u32 x = 0; x < stride; ++x) {
                u8 const a = (x >= bpp) ? cur[x - bpp] : 0;
                out[1 + x] = static_cast<u8>(cur[x] - a);
            }
            break;
        case FILTER_UP:
            for (u32 x = 0; x < stride; ++x) {
                u8 const b = prev ? prev[x] : 0;
                out[1 + x] = static_cast<u8>(cur[x] - b);
            }
            break;
        }
    }
    return filtered;
}

std::vector<u8> Writer::Impl::write(const Texture& texture) {
    issues.clear();

    if (texture.width() == 0 || texture.height() == 0) {
        fail("Cannot save an empty texture");
        return {};
    }

    // Work with an RGBA8 copy.
    Texture rgba_texture = texture.copyAsFormat(PixelFormat::RGBA8);
    const u32 width = rgba_texture.width();
    const u32 height = rgba_texture.height();
    const u8* pixels = rgba_texture.dataPtr();

    std::vector<u8> output;
    output.reserve(64 + static_cast<size_t>(width) * height * 4);

    // PNG signature.
    output.insert(output.end(), PNG_SIGNATURE.begin(), PNG_SIGNATURE.end());

    // IHDR chunk: 13 bytes.
    u8 ihdr[13];
    writeU32BE(ihdr + 0, width);
    writeU32BE(ihdr + 4, height);
    ihdr[8] = 8;                     // bit depth
    ihdr[9] = COLOR_TRUECOLOR_ALPHA; // color type 6 (RGBA)
    ihdr[10] = 0;                    // compression method
    ihdr[11] = 0;                    // filter method
    ihdr[12] = 0;                    // interlace method (none)
    writeChunk(output, CHUNK_IHDR, ihdr, 13);

    // Filter the scanlines.
    std::vector<u8> filtered = filterScanlines(pixels, width, height);

    // Compress to zlib stream.
    std::string compressError;
    auto compressed =
        zlib_compress(std::span<const u8>(filtered.data(), filtered.size()), &compressError);
    if (compressed.empty()) {
        fail("Failed to compress PNG data: " + compressError);
        return {};
    }

    // IDAT chunk(s) — write a single IDAT for simplicity.
    writeChunk(output, CHUNK_IDAT, compressed.data(), static_cast<u32>(compressed.size()));

    // IEND chunk: 0 bytes of data.
    writeChunk(output, CHUNK_IEND, nullptr, 0);

    return output;
}

std::vector<u8> Writer::Impl::writeAnimated(const std::vector<ApngFrame>& frames,
                                            const ApngSaveOptions& opts) {
    issues.clear();

    if (frames.empty()) {
        fail("Cannot write an APNG with no frames");
        return {};
    }

    // Frame 0 defines the canvas dimensions.
    Texture canvas0 = frames[0].image.copyAsFormat(PixelFormat::RGBA8);
    const u32 canvasW = canvas0.width();
    const u32 canvasH = canvas0.height();
    if (canvasW == 0 || canvasH == 0) {
        fail("Cannot write an APNG frame with zero dimensions");
        return {};
    }

    std::vector<u8> output;
    output.reserve(64 + static_cast<size_t>(canvasW) * canvasH * 4 * frames.size());

    // PNG signature.
    output.insert(output.end(), PNG_SIGNATURE.begin(), PNG_SIGNATURE.end());

    // IHDR chunk: 13 bytes.
    u8 ihdr[13];
    writeU32BE(ihdr + 0, canvasW);
    writeU32BE(ihdr + 4, canvasH);
    ihdr[8] = 8;                     // bit depth
    ihdr[9] = COLOR_TRUECOLOR_ALPHA; // color type 6 (RGBA)
    ihdr[10] = 0;                    // compression method
    ihdr[11] = 0;                    // filter method
    ihdr[12] = 0;                    // interlace method (none)
    writeChunk(output, CHUNK_IHDR, ihdr, 13);

    // acTL chunk: 8 bytes — must precede IDAT.
    u8 actl[8];
    writeU32BE(actl + 0, static_cast<u32>(frames.size()));
    writeU32BE(actl + 4, opts.loopCount);
    writeChunk(output, CHUNK_acTL, actl, 8);

    u32 seq = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        Texture rgba =
            (i == 0) ? std::move(canvas0) : frames[i].image.copyAsFormat(PixelFormat::RGBA8);
        if (rgba.width() != canvasW || rgba.height() != canvasH) {
            fail("All APNG frames must share the same dimensions");
            return {};
        }

        // fcTL chunk: 26 bytes — one per frame.
        // delayMs is encoded exactly as delayMs/1000 s; clamp the numerator.
        u32 const dm = frames[i].delayMs;
        u16 const delayNum = (dm > 0xFFFF) ? 0xFFFF : static_cast<u16>(dm);
        u8 fctl[26];
        writeU32BE(fctl + 0, seq++);
        writeU32BE(fctl + 4, canvasW);
        writeU32BE(fctl + 8, canvasH);
        writeU32BE(fctl + 12, 0); // x_offset
        writeU32BE(fctl + 16, 0); // y_offset
        writeU16BE(fctl + 20, delayNum);
        writeU16BE(fctl + 22, 1000); // delay denominator
        fctl[24] = DISPOSE_NONE;
        fctl[25] = BLEND_SOURCE;
        writeChunk(output, CHUNK_fcTL, fctl, 26);

        // Filter + compress this frame as its own independent zlib stream.
        std::vector<u8> filtered = filterScanlines(rgba.dataPtr(), canvasW, canvasH);
        std::string compressError;
        auto compressed =
            zlib_compress(std::span<const u8>(filtered.data(), filtered.size()), &compressError);
        if (compressed.empty()) {
            fail("Failed to compress APNG frame data: " + compressError);
            return {};
        }

        if (i == 0) {
            // Frame 0's pixel data goes in IDAT.
            writeChunk(output, CHUNK_IDAT, compressed.data(), static_cast<u32>(compressed.size()));
        } else {
            // Subsequent frames go in fdAT: a 4-byte sequence number + frame data.
            std::vector<u8> fdat(4 + compressed.size());
            writeU32BE(fdat.data(), seq++);
            std::memcpy(fdat.data() + 4, compressed.data(), compressed.size());
            writeChunk(output, CHUNK_fdAT, fdat.data(), static_cast<u32>(fdat.size()));
        }
    }

    // IEND chunk: 0 bytes of data.
    writeChunk(output, CHUNK_IEND, nullptr, 0);

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

std::vector<u8> Writer::writeAnimated(const std::vector<ApngFrame>& frames,
                                      const ApngSaveOptions& opts) {
    return pImpl->writeAnimated(frames, opts);
}

void Writer::writeAnimated(const std::string& filePath, const std::vector<ApngFrame>& frames,
                           const ApngSaveOptions& opts) {
    auto data = pImpl->writeAnimated(frames, opts);
    if (data.empty()) {
        return;
    }
    write_file_bytes(filePath, data, *pImpl);
}

bool Writer::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Writer::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::png

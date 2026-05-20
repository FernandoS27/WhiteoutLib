// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/png/writer.h>

#include "deflate.h"
#include "png_internal.h"

#include "../io_helpers.h"
#include "../issue_sink.h"

#include <cstring>
#include <memory>

namespace whiteout::textures::png {

class Writer::Impl : public IssueSink {
public:
    std::vector<u8> write(const Texture& texture);

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

} // namespace whiteout::textures::png

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/jpeg/writer.h>

#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/color_convert.h"
#include "jpeg_common.h"
#include "jpeg_encode.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::jpeg {

class Writer::Impl : public IssueSink {
public:
    i32 quality = 75;
    interfaces::WorkerPool* pool = nullptr;
    bool progressive = false;

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
    const u32 pixelCount = width * height;

    // Build a 3-component Y'CbCr image for the raw encoder.
    whiteout::textures::jpeg::Image image;
    image.width = width;
    image.height = height;
    image.components = 3;
    image.pixels.resize(static_cast<size_t>(width) * height * 3);

    // --- Optionally set up parallel context ---

    std::unique_ptr<interfaces::TimelineSemaphore> sem;
    JpegContext jctx;
    JpegContext* ctxPtr = nullptr;

    if (pool && pool->threadCount() > 1) {
        sem = pool->createTimelineSemaphore();
        if (sem) {
            jctx.pool = pool;
            jctx.sem = sem.get();
            jctx.currentValue = sem->value();
            ctxPtr = &jctx;
        }
    }

    if (ctxPtr) {
        // --- DAG node 1: RGB → Y'CbCr colour conversion (parallel) ---
        parallel_for_blocks(pixelCount, ctxPtr,
            [src, &image](u32 begin, u32 end) {
                for (u32 i = begin; i < end; ++i) {
                    rgb_to_ycbcr(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2],
                                 image.pixels[i * 3 + 0], image.pixels[i * 3 + 1],
                                 image.pixels[i * 3 + 2]);
                }
            });

        // --- DAG nodes 2-4: encode (parallel data prep + serial entropy) ---
        std::vector<u8> encodedOutput;
        std::string encodeError;
        whiteout::textures::jpeg::encode_raw(image, quality, &encodeError, progressive,
                                             ctxPtr, &encodedOutput);

        // Wait for the entire DAG to complete.
        jctx.sem->wait(jctx.currentValue);

        if (encodedOutput.empty()) {
            fail("JPEG encode failed: " + encodeError);
            return {};
        }

        // Insert JFIF APP0 marker after the SOI marker (first 2 bytes).
        auto app0 = build_jfif_app0();
        std::vector<u8> output;
        output.reserve(encodedOutput.size() + app0.size());
        output.insert(output.end(), encodedOutput.begin(), encodedOutput.begin() + 2);
        output.insert(output.end(), app0.begin(), app0.end());
        output.insert(output.end(), encodedOutput.begin() + 2, encodedOutput.end());
        return output;
    }

    // --- Serial path ---

    for (u32 i = 0; i < pixelCount; ++i) {
        rgb_to_ycbcr(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2],
                     image.pixels[i * 3 + 0], image.pixels[i * 3 + 1], image.pixels[i * 3 + 2]);
    }

    std::string encodeError;
    auto encoded = whiteout::textures::jpeg::encode_raw(image, quality, &encodeError,
                                                        progressive);
    if (encoded.empty()) {
        fail("JPEG encode failed: " + encodeError);
        return {};
    }

    // Insert JFIF APP0 marker after the SOI marker (first 2 bytes).
    auto app0 = build_jfif_app0();
    std::vector<u8> output;
    output.reserve(encoded.size() + app0.size());
    output.insert(output.end(), encoded.begin(), encoded.begin() + 2);
    output.insert(output.end(), app0.begin(), app0.end());
    output.insert(output.end(), encoded.begin() + 2, encoded.end());
    return output;
}

Writer::Writer(i32 quality, WriteMode writeMode, interfaces::WorkerPool* pool,
               bool progressive)
    : pImpl(std::make_unique<Impl>()) {
    pImpl->strict_mode = (writeMode == WriteMode::Strict);
    pImpl->quality = std::clamp(quality, 1, 100);
    pImpl->pool = pool;
    pImpl->progressive = progressive;
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

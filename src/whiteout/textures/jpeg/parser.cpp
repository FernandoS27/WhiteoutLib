// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/jpeg/parser.h>

#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/color_convert.h"
#include "jpeg_common.h"
#include "jpeg_decode.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::jpeg {

class Parser::Impl : public IssueSink {
public:
    interfaces::WorkerPool* pool = nullptr;

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

    // --- Decode JPEG data (optionally with parallelised IDCT / assembly) ---

    std::unique_ptr<interfaces::TimelineSemaphore> sem;
    JpegContext jctx;
    JpegContext* ctxPtr = nullptr;
    Image asyncImage;

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
        // Async path: decode_raw blocks during parsing + entropy, then builds
        // parallel DAG for IDCT + assembly, then appends output-copy task.
        // Returns std::nullopt in both success and failure cases when ctx is set.
        std::string decodeError;
        whiteout::textures::jpeg::decode_raw(buffer, &decodeError, ctxPtr, &asyncImage);
        // Dimensions are set synchronously in decode_raw (parsing completes in
        // the call).  Zero dimensions means parsing itself failed.
        if (asyncImage.width == 0 && asyncImage.height == 0) {
            fail("JPEG decode failed: " + decodeError);
            return std::nullopt;
        }
    } else {
        // Serial path.
        std::string decodeError;
        auto image = whiteout::textures::jpeg::decode_raw(buffer, &decodeError);
        if (!image) {
            fail("JPEG decode failed: " + decodeError);
            return std::nullopt;
        }
        asyncImage = std::move(*image);
    }

    // --- Validate decoded image ---

    // For the async path the image dimensions are available immediately
    // (parsing completed synchronously inside decode_raw).
    const u32 width = asyncImage.width;
    const u32 height = asyncImage.height;
    const u32 components = asyncImage.components;

    if (width == 0 || height == 0) {
        fail("JPEG image has zero dimensions");
        return std::nullopt;
    }

    if (components != 1 && components != 3 && components != 4) {
        fail("Unsupported JPEG component count: " + std::to_string(components));
        return std::nullopt;
    }

    // --- Build output texture and perform colour conversion ---

    Texture texture = Texture::create2D(PixelFormat::RGBA8, width, height, 1);
    u8* dest = texture.dataPtr();
    const u32 pixelCount = width * height;

    if (ctxPtr) {
        // Append colour conversion as a parallel DAG node.
        // asyncImage.pixels (source) will be populated by the preceding DAG nodes.
        parallel_for_blocks(
            pixelCount, ctxPtr, [&asyncImage, dest, components, pixelCount](u32 begin, u32 end) {
                const u8* src = asyncImage.pixels.data();
                if (components == 1) {
                    for (u32 i = begin; i < end; ++i) {
                        u8 y = src[i];
                        dest[i * 4 + 0] = y;
                        dest[i * 4 + 1] = y;
                        dest[i * 4 + 2] = y;
                        dest[i * 4 + 3] = 255;
                    }
                } else if (components == 3) {
                    for (u32 i = begin; i < end; ++i) {
                        ycbcr_to_rgb(src[i * 3 + 0], src[i * 3 + 1], src[i * 3 + 2],
                                     dest[i * 4 + 0], dest[i * 4 + 1], dest[i * 4 + 2]);
                        dest[i * 4 + 3] = 255;
                    }
                } else if (components == 4) {
                    for (u32 i = begin; i < end; ++i) {
                        ycbcr_to_rgb(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2],
                                     dest[i * 4 + 0], dest[i * 4 + 1], dest[i * 4 + 2]);
                        dest[i * 4 + 3] = src[i * 4 + 3];
                    }
                }
            });

        // Wait for the entire DAG to complete.
        jctx.sem->wait(jctx.currentValue);
    } else {
        // Serial colour conversion.
        const u8* src = asyncImage.pixels.data();

        if (components == 1) {
            for (u32 i = 0; i < pixelCount; ++i) {
                u8 y = src[i];
                dest[i * 4 + 0] = y;
                dest[i * 4 + 1] = y;
                dest[i * 4 + 2] = y;
                dest[i * 4 + 3] = 255;
            }
        } else if (components == 3) {
            for (u32 i = 0; i < pixelCount; ++i) {
                ycbcr_to_rgb(src[i * 3 + 0], src[i * 3 + 1], src[i * 3 + 2], dest[i * 4 + 0],
                             dest[i * 4 + 1], dest[i * 4 + 2]);
                dest[i * 4 + 3] = 255;
            }
        } else if (components == 4) {
            for (u32 i = 0; i < pixelCount; ++i) {
                ycbcr_to_rgb(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2], dest[i * 4 + 0],
                             dest[i * 4 + 1], dest[i * 4 + 2]);
                dest[i * 4 + 3] = src[i * 4 + 3];
            }
        }
    }

    return texture;
}

Parser::Parser(ParseMode parseMode, interfaces::WorkerPool* pool)
    : pImpl(std::make_unique<Impl>()) {
    pImpl->strict_mode = (parseMode == ParseMode::Strict);
    pImpl->pool = pool;
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

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/gif/writer.h>

#include "gif_internal.h"

#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/quantize.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace whiteout::textures::gif {

namespace {

// ============================================================================
// LZW compressor for GIF
// ============================================================================

/// Minimum code size for 256-color images is always 8.
constexpr u32 LZW_MIN_CODE_SIZE = 8;

/// LZW table entry: parent code + suffix byte.
struct LzwEntry {
    u16 prefix;
    u8 suffix;
};

/// Write a stream of LZW-compressed sub-blocks into @p output.
///
/// The GIF LZW variant uses variable-width codes starting at
/// (minCodeSize + 1) bits, with CLEAR and EOI codes.
void lzw_compress(const u8* indices, u32 pixel_count, std::vector<u8>& output) {
    // Write the minimum code size byte.
    output.push_back(static_cast<u8>(LZW_MIN_CODE_SIZE));

    const u32 clear_code = 1u << LZW_MIN_CODE_SIZE; // 256
    const u32 eoi_code = clear_code + 1;            // 257
    constexpr u32 MAX_TABLE_SIZE = 4096;            // 12-bit codes max

    // --- LZW dictionary ---
    // Hash table: open-addressing with linear probing.
    // Key = (prefix << 8) | suffix, value = code.
    constexpr u32 HASH_SIZE = 8192; // power of 2, > MAX_TABLE_SIZE
    constexpr u32 HASH_MASK = HASH_SIZE - 1;
    constexpr u32 EMPTY = 0xFFFFFFFF;
    std::vector<u32> hash_keys(HASH_SIZE, EMPTY);
    std::vector<u16> hash_vals(HASH_SIZE);

    auto hash_reset = [&]() { std::fill(hash_keys.begin(), hash_keys.end(), EMPTY); };

    auto hash_find = [&](u32 key) -> u32 {
        u32 slot = (key * 2654435761u) & HASH_MASK;
        for (;;) {
            if (hash_keys[slot] == EMPTY)
                return EMPTY;
            if (hash_keys[slot] == key)
                return hash_vals[slot];
            slot = (slot + 1) & HASH_MASK;
        }
    };

    auto hash_insert = [&](u32 key, u16 val) {
        u32 slot = (key * 2654435761u) & HASH_MASK;
        while (hash_keys[slot] != EMPTY)
            slot = (slot + 1) & HASH_MASK;
        hash_keys[slot] = key;
        hash_vals[slot] = val;
    };

    // --- Bit-packing into sub-blocks ---
    // GIF sub-blocks are at most 255 bytes.  We buffer bits and flush when
    // the block fills up.
    std::vector<u8> block_buf;
    block_buf.reserve(260);
    u32 bit_buf = 0;
    u32 bits_in = 0;

    auto emit_bits = [&](u32 code, u32 nbits) {
        bit_buf |= code << bits_in;
        bits_in += nbits;
        while (bits_in >= 8) {
            block_buf.push_back(static_cast<u8>(bit_buf & 0xFF));
            bit_buf >>= 8;
            bits_in -= 8;
            if (block_buf.size() == 255) {
                output.push_back(static_cast<u8>(block_buf.size()));
                output.insert(output.end(), block_buf.begin(), block_buf.end());
                block_buf.clear();
            }
        }
    };

    auto flush_bits = [&]() {
        if (bits_in > 0) {
            block_buf.push_back(static_cast<u8>(bit_buf & 0xFF));
            bit_buf = 0;
            bits_in = 0;
        }
        if (!block_buf.empty()) {
            output.push_back(static_cast<u8>(block_buf.size()));
            output.insert(output.end(), block_buf.begin(), block_buf.end());
            block_buf.clear();
        }
    };

    // --- Encode ---
    auto init_table = [&](u32& next_code, u32& code_size) {
        hash_reset();
        next_code = eoi_code + 1;          // 258
        code_size = LZW_MIN_CODE_SIZE + 1; // 9
    };

    u32 next_code, code_size;
    init_table(next_code, code_size);

    emit_bits(clear_code, code_size);

    if (pixel_count == 0) {
        emit_bits(eoi_code, code_size);
        flush_bits();
        output.push_back(GIF_BLOCK_TERMINATOR);
        return;
    }

    u32 cur = indices[0]; // current string represented as its code
    for (u32 i = 1; i < pixel_count; ++i) {
        const u8 suffix = indices[i];
        const u32 key = (cur << 8) | suffix;
        const u32 found = hash_find(key);

        if (found != EMPTY) {
            cur = found;
        } else {
            // Output the code for the current string.
            emit_bits(cur, code_size);

            if (next_code < MAX_TABLE_SIZE) {
                hash_insert(key, static_cast<u16>(next_code));
                if (next_code >= (1u << code_size))
                    ++code_size;
                ++next_code;
            } else {
                // Table full — emit CLEAR and reinitialize.
                emit_bits(clear_code, code_size);
                init_table(next_code, code_size);
            }

            cur = suffix;
        }
    }

    emit_bits(cur, code_size);
    emit_bits(eoi_code, code_size);
    flush_bits();
    output.push_back(GIF_BLOCK_TERMINATOR);
}

// ============================================================================
// Netscape Application Extension (looping)
// ============================================================================

void write_netscape_ext(std::vector<u8>& output, u16 loop_count) {
    output.push_back(GIF_EXTENSION_INTRODUCER);        // 0x21
    output.push_back(GIF_APPLICATION_EXTENSION_LABEL); // 0xFF
    output.push_back(11);                              // block size
    // "NETSCAPE2.0"
    const char app[] = "NETSCAPE2.0";
    output.insert(output.end(), app, app + 11);
    output.push_back(3); // sub-block size
    output.push_back(1); // sub-block ID
    output.push_back(static_cast<u8>(loop_count & 0xFF));
    output.push_back(static_cast<u8>((loop_count >> 8) & 0xFF));
    output.push_back(GIF_BLOCK_TERMINATOR);
}

} // anonymous namespace

// ============================================================================
// Writer::Impl
// ============================================================================

class Writer::Impl : public IssueSink {
public:
    interfaces::WorkerPool* pool = nullptr;

    std::vector<u8> write(const std::vector<Texture>& frames, const SaveOptions& opts);
};

std::vector<u8> Writer::Impl::write(const std::vector<Texture>& frames, const SaveOptions& opts) {
    issues.clear();

    if (frames.empty()) {
        fail("No frames provided");
        return {};
    }

    const u32 width = frames[0].width();
    const u32 height = frames[0].height();
    const PixelFormat fmt = frames[0].format();

    if (width == 0 || height == 0) {
        fail("Frame dimensions are zero");
        return {};
    }
    if (width > 0xFFFF || height > 0xFFFF) {
        fail("Frame dimensions exceed GIF maximum (65535)");
        return {};
    }

    // Validate that the format is uncompressed (blockEdge == 1).
    if (blockEdge(fmt) != 1) {
        fail("Input textures must use an uncompressed pixel format");
        return {};
    }

    // Validate all frames share the same dimensions and format.
    for (size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].width() != width || frames[i].height() != height) {
            fail("All frames must have the same dimensions");
            return {};
        }
        if (frames[i].format() != fmt) {
            fail("All frames must have the same pixel format");
            return {};
        }
    }

    const u32 pixel_count = width * height;
    const bool animated = frames.size() > 1;

    // Convert all frames to RGBA8 and concatenate for global palette generation.
    std::vector<Texture> rgba_frames;
    rgba_frames.reserve(frames.size());
    std::vector<u8> all_rgba(static_cast<u64>(pixel_count) * 4 * frames.size());
    u64 offset = 0;
    for (const auto& frame : frames) {
        rgba_frames.push_back(frame.copyAsFormat(PixelFormat::RGBA8));
        const auto data = rgba_frames.back().data();
        std::memcpy(all_rgba.data() + offset, data.data(), data.size());
        offset += data.size();
    }

    // Build a single global palette from all frames. A transparent GIF caps
    // the palette at 255 colours so index 255 is free for the transparent
    // entry.
    auto quantizer = wu::Quantizer();
    if (pool)
        quantizer.workerPool(pool);
    if (opts.transparent)
        quantizer.maxColors(255);
    auto quantized =
        quantizer.quantize(all_rgba.data(), static_cast<u32>(pixel_count * frames.size()));
    const u32 color_count = quantized.color_count;

    // Build the GIF global color table (always 256 entries = 768 bytes).
    // GIF palette is packed RGB triplets.
    std::array<u8, 768> gct{};
    for (u32 c = 0; c < color_count; ++c) {
        gct[c * 3 + 0] = static_cast<u8>((quantized.palette[c] >> 16) & 0xFF); // R
        gct[c * 3 + 1] = static_cast<u8>((quantized.palette[c] >> 8) & 0xFF);  // G
        gct[c * 3 + 2] = static_cast<u8>(quantized.palette[c] & 0xFF);         // B
    }

    std::vector<u8> output;
    output.reserve(pixel_count * frames.size()); // rough estimate

    // --- GIF Header ---
    GifHeader header{};
    std::memcpy(header.signature, "GIF", 3);
    std::memcpy(header.version, "89a", 3);
    output.insert(output.end(), reinterpret_cast<const u8*>(&header),
                  reinterpret_cast<const u8*>(&header) + sizeof(header));

    // --- Logical Screen Descriptor ---
    LogicalScreenDescriptor lsd{};
    lsd.width = static_cast<u16>(width);
    lsd.height = static_cast<u16>(height);
    // packed: GCT flag=1, color resolution=7 (8 bits), sort=0, GCT size=7 (256 entries)
    lsd.packed = 0x80 | (7 << 4) | 7; // 0xF7
    // For a transparent GIF the canvas background is the transparent index,
    // so disposal-2 "restore to background" clears to transparent.
    lsd.bgColorIndex = opts.transparent ? 255 : 0;
    lsd.pixelAspectRatio = 0;
    output.insert(output.end(), reinterpret_cast<const u8*>(&lsd),
                  reinterpret_cast<const u8*>(&lsd) + sizeof(lsd));

    // --- Global Color Table (256 × RGB) ---
    output.insert(output.end(), gct.begin(), gct.end());

    // --- Netscape looping extension (for animated GIFs) ---
    if (animated) {
        write_netscape_ext(output, opts.loopCount);
    }

    // --- Encode each frame ---
    std::vector<u8> frame_indices(pixel_count);
    for (size_t f = 0; f < rgba_frames.size(); ++f) {
        // Graphic Control Extension (delay + disposal + transparency).
        if (animated || opts.delayCs > 0 || opts.transparent) {
            GraphicControlExtension gce{};
            gce.introducer = GIF_EXTENSION_INTRODUCER;
            gce.label = GIF_GRAPHIC_CONTROL_LABEL;
            gce.blockSize = 4;
            // packed: [reserved:3][disposal:3][userInput:1][transparency:1].
            // Transparent frames use disposal method 2 (restore to background)
            // so a moving subject doesn't ghost through transparent pixels.
            gce.packed = opts.transparent ? static_cast<u8>((2u << 2) | 0x01u) : 0x00;
            gce.delayTime = opts.delayCs;
            gce.transparentIdx = opts.transparent ? 255 : 0;
            gce.terminator = GIF_BLOCK_TERMINATOR;
            output.insert(output.end(), reinterpret_cast<const u8*>(&gce),
                          reinterpret_cast<const u8*>(&gce) + sizeof(gce));
        }

        // Image Descriptor.
        ImageDescriptor id{};
        id.separator = GIF_IMAGE_SEPARATOR;
        id.left = 0;
        id.top = 0;
        id.width = static_cast<u16>(width);
        id.height = static_cast<u16>(height);
        id.packed = 0; // no local color table, no interlace
        output.insert(output.end(), reinterpret_cast<const u8*>(&id),
                      reinterpret_cast<const u8*>(&id) + sizeof(id));

        // Map pixels to palette indices.
        const u8* frame_rgba = rgba_frames[f].dataPtr();
        if (opts.dither) {
            quantized.mapPixelsDithered(frame_rgba, width, height, opts.ditherStrength,
                                        frame_indices.data());
        } else {
            quantized.mapPixels(frame_rgba, pixel_count, frame_indices.data());
        }

        // 1-bit transparency: pixels below 50% alpha take the reserved index.
        if (opts.transparent) {
            for (u32 p = 0; p < pixel_count; ++p)
                if (frame_rgba[p * 4 + 3] < 128)
                    frame_indices[p] = 255;
        }

        // LZW-compress the index stream.
        lzw_compress(frame_indices.data(), pixel_count, output);
    }

    // --- GIF Trailer ---
    output.push_back(GIF_TRAILER);

    return output;
}

// ============================================================================
// Writer — public interface
// ============================================================================

Writer::Writer(interfaces::WorkerPool* pool) : pImpl(std::make_unique<Impl>()) {
    pImpl->pool = pool;
}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const std::vector<Texture>& frames) {
    write(filePath, frames, SaveOptions{});
}

std::vector<u8> Writer::write(const std::vector<Texture>& frames) {
    return write(frames, SaveOptions{});
}

void Writer::write(const std::string& filePath, const std::vector<Texture>& frames,
                   const SaveOptions& opts) {
    auto data = pImpl->write(frames, opts);
    if (data.empty())
        return;
    if (!write_file_bytes(filePath, data, *pImpl)) {
        return;
    }
}

std::vector<u8> Writer::write(const std::vector<Texture>& frames, const SaveOptions& opts) {
    return pImpl->write(frames, opts);
}

bool Writer::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Writer::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::gif

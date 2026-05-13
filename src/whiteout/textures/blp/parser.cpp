// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/blp/parser.h>

#include "../jpeg/jpeg_decode.h"
#include "blp_internal.h"

#include <whiteout/textures/blp/types.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include "../io_helpers.h"
#include "../issue_sink.h"
#include "../utils/color_convert.h"

namespace whiteout::textures::blp {

namespace {

/// Maximum size of the JPEG header embedded in a BLP2 file.
/// The palette field is 1024 bytes; the first 4 bytes store the header size,
/// leaving a maximum of 1020 bytes for the actual JPEG header data.
constexpr u32 MAX_BLP2_JPEG_HEADER_SIZE = 1020;

// ============================================================================
// Parser-side helpers
// ============================================================================

u32 count_valid_mips(const u32* offsets, const u32* sizes, u32 max_mips) {
    u32 count = 0;
    for (u32 i = 0; i < max_mips; ++i) {
        if (offsets[i] == 0 || sizes[i] == 0) {
            break;
        }
        ++count;
    }
    return std::max(count, 1u);
}

u32 resolve_mip_count(u32 has_mipmaps, const u32* offsets, const u32* sizes, u32 width,
                      u32 height) {
    if (has_mipmaps == 0) {
        return 1;
    }
    const u32 max_mips = std::min(textures::computeMaxMipCount(width, height), MAX_MIP_LEVELS);
    return std::min(count_valid_mips(offsets, sizes, MAX_MIP_LEVELS), max_mips);
}

u32 sanitize_alpha_bits(u32 alpha_bit_depth) {
    if (alpha_bit_depth == 1 || alpha_bit_depth == 4 || alpha_bit_depth == 8) {
        return alpha_bit_depth;
    }
    return 0;
}

u8 decode_palette_alpha(const u8* alpha_data, u32 alpha_bytes_avail, u32 pixel_index,
                        u32 alpha_bit_depth) {
    switch (alpha_bit_depth) {
    case 1: {
        const u32 byte_index = pixel_index / 8;
        if (byte_index >= alpha_bytes_avail)
            return 255;
        return ((alpha_data[byte_index] >> (pixel_index % 8)) & 1) ? 255 : 0;
    }
    case 4: {
        const u32 byte_index = pixel_index / 2;
        if (byte_index >= alpha_bytes_avail)
            return 255;
        const u8 nibble = (pixel_index % 2 == 0) ? (alpha_data[byte_index] & 0x0F)
                                                 : ((alpha_data[byte_index] >> 4) & 0x0F);
        return static_cast<u8>(nibble * EXPAND_4BIT_TO_8BIT);
    }
    case 8:
        return (pixel_index < alpha_bytes_avail) ? alpha_data[pixel_index] : static_cast<u8>(255);
    default:
        return 255;
    }
}

void decode_palettized_mip(const u32* palette, const u8* mipData, u32 mip_data_size,
                           u32 pixel_count, u32 alpha_bit_depth, u8* out_rgba) {
    const u32 index_bytes = std::min(pixel_count, mip_data_size);
    const u8* indices = mipData;
    const u8* alpha_data = mipData + pixel_count;
    const u32 alpha_bytes_avail = (mip_data_size > pixel_count) ? (mip_data_size - pixel_count) : 0;

    for (u32 i = 0; i < pixel_count; ++i) {
        const u8 palette_index = (i < index_bytes) ? indices[i] : 0;
        const u32 palette_entry = palette[palette_index];

        out_rgba[i * 4 + 0] = static_cast<u8>((palette_entry >> 16) & 0xFF);
        out_rgba[i * 4 + 1] = static_cast<u8>((palette_entry >> 8) & 0xFF);
        out_rgba[i * 4 + 2] = static_cast<u8>(palette_entry & 0xFF);
        out_rgba[i * 4 + 3] =
            decode_palette_alpha(alpha_data, alpha_bytes_avail, i, alpha_bit_depth);
    }
}

bool decode_jpeg_mip(const u8* jpeg_header, u32 jpeg_header_size, const u8* mip_chunk,
                     u32 mip_chunk_size, Texture& tex, u32 mip, bool alpha_opaque,
                     std::string* out_error) {
    std::vector<u8> jpeg_buf(jpeg_header_size + mip_chunk_size);
    if (jpeg_header_size > 0) {
        std::memcpy(jpeg_buf.data(), jpeg_header, jpeg_header_size);
    }
    if (mip_chunk_size > 0) {
        std::memcpy(jpeg_buf.data() + jpeg_header_size, mip_chunk, mip_chunk_size);
    }

    auto decode_result = jpeg::decode_raw(std::span<const u8>{jpeg_buf}, out_error);
    if (!decode_result) {
        return false;
    }

    const auto& decoded = *decode_result;
    if (decoded.components < 3) {
        if (out_error) {
            *out_error =
                "BLP JPEG: expected >= 3 components, got " + std::to_string(decoded.components);
        }
        return false;
    }

    const auto& mip_info = tex.mipLevel(mip);
    const u32 expected_width = mip_info.width;
    const u32 expected_height = mip_info.height;
    auto destination = tex.mipData(mip);

    const u32 copy_width = std::min(expected_width, decoded.width);
    const u32 copy_height = std::min(expected_height, decoded.height);
    const bool has_alpha = (decoded.components >= 4);

    std::memset(destination.data(), 0, destination.size());

    for (u32 y = 0; y < copy_height; y++) {
        for (u32 x = 0; x < copy_width; x++) {
            const size_t src_index =
                (static_cast<size_t>(y) * decoded.width + x) * decoded.components;
            const size_t dst_index = (static_cast<size_t>(y) * expected_width + x) * 4;

            destination[dst_index + 0] = decoded.pixels[src_index + 2]; // R
            destination[dst_index + 1] = decoded.pixels[src_index + 1]; // G
            destination[dst_index + 2] = decoded.pixels[src_index + 0]; // B
            destination[dst_index + 3] =
                (has_alpha && !alpha_opaque) ? decoded.pixels[src_index + 3] : 0xFF;
        }
    }
    return true;
}

std::optional<PixelFormat> dxt_alpha_type_to_format(u8 alpha_type) {
    switch (alpha_type) {
    case 0:
        return PixelFormat::BC1;
    case 1:
        return PixelFormat::BC2;
    case 7:
        return PixelFormat::BC3;
    default:
        return std::nullopt;
    }
}

} // anonymous namespace

/// Normalized header data from either BLP1 or BLP2, plus the raw input buffer.
/// After readBlp1Header / readBlp2Header fills this in, decodeMips()
/// can process all encoding types identically regardless of the source version.
struct ParseContext {
    std::span<const u8> buffer;
    u32 width{};
    u32 height{};
    u32 mipCount{};
    u32 alpha_bits{};
    BlpEncoding encoding{BlpEncoding::Infer};
    PixelFormat pixel_format{PixelFormat::RGBA8};
    std::array<u32, 16> mip_offsets{};
    std::array<u32, 16> mip_sizes{};

    // Palettized
    std::array<u32, 256> palette{};

    // JPEG
    const u8* jpeg_header{};
    u32 jpeg_header_size{};
    bool jpeg_alpha_opaque{true};

    // BLP0 external mip data — when non-empty, mip_span uses these instead of buffer offsets.
    std::vector<std::vector<u8>> external_mips;

    /// Safe sub-span of mip data. Uses external_mips when available (BLP0),
    /// otherwise indexes into the main buffer at the given offset/size.
    std::span<const u8> mip_span(u32 offset, u32 size, u32 mip_index = 0) const {
        if (!external_mips.empty()) {
            if (mip_index < external_mips.size() && !external_mips[mip_index].empty()) {
                return std::span<const u8>{external_mips[mip_index]};
            }
            return {};
        }
        if (offset == 0 || size == 0 || static_cast<size_t>(offset) > buffer.size()) {
            return {};
        }
        const size_t available = std::min(static_cast<size_t>(size), buffer.size() - offset);
        return buffer.subspan(offset, available);
    }
};

/// Copy raw mip data into @p destination, zero-filling any remainder.
static void copy_mip_raw(const ParseContext& ctx, u32 mip_index, std::span<u8> destination) {
    auto span = ctx.mip_span(ctx.mip_offsets[mip_index], ctx.mip_sizes[mip_index], mip_index);
    if (span.empty()) {
        std::memset(destination.data(), 0, destination.size());
        return;
    }
    const u32 to_copy =
        std::min(static_cast<u32>(span.size()), static_cast<u32>(destination.size()));
    std::memcpy(destination.data(), span.data(), to_copy);
    if (to_copy < destination.size()) {
        std::memset(destination.data() + to_copy, 0, destination.size() - to_copy);
    }
}

class Parser::Impl : public IssueSink {
public:
    std::optional<Texture> parse(std::span<const u8> buffer);
    std::optional<Texture> parse(std::span<const u8> buffer, const std::string& filePath);

private:
    bool readBlp0Header(ParseContext& ctx);
    bool readBlp1Header(ParseContext& ctx);
    bool readBlp2Header(ParseContext& ctx);
    bool loadBlp0ExternalMips(ParseContext& ctx, const std::string& filePath);
    std::optional<Texture> decodeMips(const ParseContext& ctx);

    bool loadJpegMips(const ParseContext& ctx, Texture& result);
    void loadPalettizedMips(const ParseContext& ctx, Texture& result);
};

// ============================================================================
// Parser
// ============================================================================

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer) {
    issues.clear();

    if (buffer.size() < 4) {
        fail("Buffer too small for BLP magic");
        return std::nullopt;
    }

    ParseContext ctx;
    ctx.buffer = buffer;

    u32 magic{};
    std::memcpy(&magic, buffer.data(), 4);

    bool header_ok = false;
    switch (magic) {
    case BLP0_MAGIC:
        // BLP0 from a raw buffer cannot load external mip files.
        // Parse header + content header; mip 0 data is not available.
        header_ok = readBlp0Header(ctx);
        if (header_ok) {
            fail("BLP0 parsed from buffer: external mip files (.b00-.b15) are not "
                 "available; use the file-path overload for full mipmap support");
        }
        break;
    case BLP1_MAGIC:
        header_ok = readBlp1Header(ctx);
        break;
    case BLP2_MAGIC:
        header_ok = readBlp2Header(ctx);
        break;
    default:
        fail("Not a BLP file (invalid magic)");
        return std::nullopt;
    }

    if (!header_ok) {
        return std::nullopt;
    }
    return decodeMips(ctx);
}

std::optional<Texture> Parser::Impl::parse(std::span<const u8> buffer,
                                           const std::string& filePath) {
    issues.clear();

    if (buffer.size() < 4) {
        fail("Buffer too small for BLP magic");
        return std::nullopt;
    }

    u32 magic{};
    std::memcpy(&magic, buffer.data(), 4);

    // Only BLP0 needs the file path (for external mip files).
    // Other versions delegate to the buffer-only path.
    if (magic != BLP0_MAGIC) {
        return parse(buffer);
    }

    // BLP0 path — load header, then read external mip files.
    ParseContext ctx;
    ctx.buffer = buffer;
    if (!readBlp0Header(ctx)) {
        return std::nullopt;
    }
    if (!loadBlp0ExternalMips(ctx, filePath)) {
        return std::nullopt;
    }
    return decodeMips(ctx);
}

bool Parser::Impl::loadJpegMips(const ParseContext& ctx, Texture& result) {
    for (u32 mip = 0; mip < ctx.mipCount; ++mip) {
        auto span = ctx.mip_span(ctx.mip_offsets[mip], ctx.mip_sizes[mip], mip);
        if (span.empty()) {
            auto dst = result.mipData(mip);
            std::memset(dst.data(), 0, dst.size());
            continue;
        }

        std::string jpeg_error;
        if (!decode_jpeg_mip(ctx.jpeg_header, ctx.jpeg_header_size, span.data(),
                             static_cast<u32>(span.size()), result, mip, ctx.jpeg_alpha_opaque,
                             &jpeg_error)) {
            fail(jpeg_error.empty() ? "JPEG mip decode failed" : jpeg_error);
            return false;
        }
    }
    return true;
}

void Parser::Impl::loadPalettizedMips(const ParseContext& ctx, Texture& result) {
    for (u32 mip = 0; mip < ctx.mipCount; ++mip) {
        auto span = ctx.mip_span(ctx.mip_offsets[mip], ctx.mip_sizes[mip], mip);
        if (span.empty()) {
            auto dst = result.mipData(mip);
            std::memset(dst.data(), 0, dst.size());
            continue;
        }

        const auto& mip_info = result.mipLevel(mip);
        const u32 pixel_count = mip_info.width * mip_info.height;

        decode_palettized_mip(ctx.palette.data(), span.data(), static_cast<u32>(span.size()),
                              pixel_count, ctx.alpha_bits, result.mipData(mip).data());
    }
}

// ============================================================================
// Header readers — normalize BLP0/BLP1/BLP2 into a common ParseContext
// ============================================================================

bool Parser::Impl::readBlp0Header(ParseContext& ctx) {
    // BLP0 header is identical to BLP1 (28 bytes), but has no mipmap locator.
    // Content header (JPEG header or palette) starts at offset 0x1C.
    if (ctx.buffer.size() < sizeof(BLP1Header)) {
        return fail("BLP0 file too small");
    }

    BLP1Header header{};
    std::memcpy(&header, ctx.buffer.data(), sizeof(BLP1Header));

    if (header.width == 0 || header.height == 0) {
        return fail("BLP0 has zero dimensions");
    }

    ctx.width = header.width;
    ctx.height = header.height;
    ctx.pixel_format = PixelFormat::RGBA8;

    // BLP0 has no mipmap locator — mip data comes from external .bXX files.
    // Mip count is determined later when external files are loaded.
    // For buffer-only parsing, only mip 0 is reported with empty data.
    if (header.hasMipmaps != 0) {
        ctx.mipCount =
            std::min(textures::computeMaxMipCount(header.width, header.height), MAX_MIP_LEVELS);
    } else {
        ctx.mipCount = 1;
    }

    // Content header starts right after the BLP1Header (offset 0x1C), no mipmap locator.
    static constexpr size_t CONTENT_HDR_OFFSET = sizeof(BLP1Header);

    if (header.content != CONTENT_DIRECT) {
        // JPEG path
        if (ctx.buffer.size() < CONTENT_HDR_OFFSET + 4) {
            return fail("BLP0 file too small for JPEG header size");
        }
        u32 jpeg_hdr_size = 0;
        std::memcpy(&jpeg_hdr_size, ctx.buffer.data() + CONTENT_HDR_OFFSET, 4);
        const size_t jpeg_hdr_data_off = CONTENT_HDR_OFFSET + 4;
        if (jpeg_hdr_size > 0 && ctx.buffer.size() < jpeg_hdr_data_off + jpeg_hdr_size) {
            jpeg_hdr_size = static_cast<u32>(ctx.buffer.size() - jpeg_hdr_data_off);
        }

        ctx.encoding = BlpEncoding::JPEG;
        ctx.jpeg_header = ctx.buffer.data() + jpeg_hdr_data_off;
        ctx.jpeg_header_size = jpeg_hdr_size;
        // Treat any non-zero alphaBitDepth as having alpha. WC3 model textures
        // sometimes use non-standard values (e.g. 9) where the JPEG still carries
        // a meaningful alpha component.
        ctx.jpeg_alpha_opaque = (header.alphaBitDepth == 0);
    } else {
        // Palettized (CONTENT_DIRECT) path
        static constexpr size_t PALETTE_SIZE = 256 * 4;
        if (ctx.buffer.size() < CONTENT_HDR_OFFSET + PALETTE_SIZE) {
            return fail("BLP0 file too small for palette");
        }

        std::memcpy(ctx.palette.data(), ctx.buffer.data() + CONTENT_HDR_OFFSET, PALETTE_SIZE);
        ctx.encoding = BlpEncoding::Palettized;
        ctx.alpha_bits = sanitize_alpha_bits(header.alphaBitDepth);
    }

    return true;
}

bool Parser::Impl::loadBlp0ExternalMips(ParseContext& ctx, const std::string& filePath) {
#ifdef __EMSCRIPTEN__
    // BLP0 sibling-file mips require disk access to .b00..b15 files next to
    // the .blp. The WASM build has no real filesystem; callers must use
    // buffer-based parse() with inline mips instead.
    (void)ctx;
    (void)filePath;
    return fail("BLP0 external mip files are not supported in the WASM build");
#else
    namespace fs = std::filesystem;

    const fs::path blpPath(filePath);
    const fs::path parentDir = blpPath.parent_path();
    const std::string stem = blpPath.stem().string();

    ctx.external_mips.resize(ctx.mipCount);

    for (u32 mip = 0; mip < ctx.mipCount; ++mip) {
        // Build extension: .b00, .b01, ..., .b15
        std::ostringstream ext;
        ext << ".b" << std::setfill('0') << std::setw(2) << mip;

        const fs::path mipFilePath = parentDir / (stem + ext.str());

        auto mipData = read_file_bytes(mipFilePath.string(), *this);
        if (!mipData || mipData->empty()) {
            // Truncate mip count at first missing file.
            ctx.mipCount = std::max(mip, 1u);
            ctx.external_mips.resize(ctx.mipCount);
            if (mip == 0) {
                return fail("BLP0: could not read base mip file: " + mipFilePath.string());
            }
            break;
        }

        ctx.external_mips[mip] = std::move(*mipData);
        // Set offset/size entries for compatibility (offset is unused when external_mips is set,
        // but size is used by some code paths).
        ctx.mip_offsets[mip] = 1; // Non-zero so mip_span doesn't treat it as empty.
        ctx.mip_sizes[mip] = static_cast<u32>(ctx.external_mips[mip].size());
    }

    return true;
#endif
}

bool Parser::Impl::readBlp1Header(ParseContext& ctx) {
    if (ctx.buffer.size() < sizeof(BLP1Header) + sizeof(MipmapLocator)) {
        return fail("BLP1 file too small");
    }

    BLP1Header header{};
    std::memcpy(&header, ctx.buffer.data(), sizeof(BLP1Header));

    MipmapLocator mip_locator{};
    std::memcpy(&mip_locator, ctx.buffer.data() + sizeof(BLP1Header), sizeof(MipmapLocator));

    if (header.width == 0 || header.height == 0) {
        return fail("BLP1 has zero dimensions");
    }

    ctx.width = header.width;
    ctx.height = header.height;
    ctx.pixel_format = PixelFormat::RGBA8;
    std::copy(mip_locator.mipOffsets.begin(), mip_locator.mipOffsets.end(),
              ctx.mip_offsets.begin());
    std::copy(mip_locator.mipSizes.begin(), mip_locator.mipSizes.end(), ctx.mip_sizes.begin());
    ctx.mipCount = resolve_mip_count(header.hasMipmaps, ctx.mip_offsets.data(),
                                     ctx.mip_sizes.data(), header.width, header.height);

    // Everything except CONTENT_DIRECT is treated as JPEG-compressed.
    if (header.content != CONTENT_DIRECT) {
        static constexpr size_t JPEG_HDR_OFFSET = sizeof(BLP1Header) + sizeof(MipmapLocator);

        if (ctx.buffer.size() < JPEG_HDR_OFFSET + 4) {
            return fail("BLP1 file too small for JPEG header size");
        }
        u32 jpeg_hdr_size = 0;
        std::memcpy(&jpeg_hdr_size, ctx.buffer.data() + JPEG_HDR_OFFSET, 4);
        const size_t jpeg_hdr_data_off = JPEG_HDR_OFFSET + 4;
        if (jpeg_hdr_size > 0 && ctx.buffer.size() < jpeg_hdr_data_off + jpeg_hdr_size) {
            jpeg_hdr_size = static_cast<u32>(ctx.buffer.size() - jpeg_hdr_data_off);
        }

        ctx.encoding = BlpEncoding::JPEG;
        ctx.jpeg_header = ctx.buffer.data() + jpeg_hdr_data_off;
        ctx.jpeg_header_size = jpeg_hdr_size;
        // Treat any non-zero alphaBitDepth as having alpha. WC3 model textures
        // sometimes use non-standard values (e.g. 9) where the JPEG still carries
        // a meaningful alpha component.
        ctx.jpeg_alpha_opaque = (header.alphaBitDepth == 0);
    } else {
        // Palettized (CONTENT_DIRECT) path
        static constexpr size_t PALETTE_OFFSET = sizeof(BLP1Header) + sizeof(MipmapLocator);
        static constexpr size_t PALETTE_SIZE = 256 * 4;

        if (ctx.buffer.size() < PALETTE_OFFSET + PALETTE_SIZE) {
            return fail("BLP1 file too small for palette");
        }

        std::memcpy(ctx.palette.data(), ctx.buffer.data() + PALETTE_OFFSET, PALETTE_SIZE);
        ctx.encoding = BlpEncoding::Palettized;
        ctx.alpha_bits = sanitize_alpha_bits(header.alphaBitDepth);
    }

    return true;
}

bool Parser::Impl::readBlp2Header(ParseContext& ctx) {
    if (ctx.buffer.size() < sizeof(BLP2Header)) {
        return fail("BLP2 file too small");
    }

    BLP2Header header{};
    std::memcpy(&header, ctx.buffer.data(), sizeof(BLP2Header));

    if (header.version != 1) {
        return fail("Unsupported BLP2 version: " + std::to_string(header.version));
    }

    if (header.width == 0 || header.height == 0) {
        return fail("BLP2 has zero dimensions");
    }

    ctx.width = header.width;
    ctx.height = header.height;
    ctx.pixel_format = PixelFormat::RGBA8;
    std::copy(header.mipOffsets.begin(), header.mipOffsets.end(), ctx.mip_offsets.begin());
    std::copy(header.mipSizes.begin(), header.mipSizes.end(), ctx.mip_sizes.begin());
    ctx.mipCount = resolve_mip_count(header.hasMipmaps, ctx.mip_offsets.data(),
                                     ctx.mip_sizes.data(), header.width, header.height);

    switch (header.colorEncoding) {
    case ENCODING_JPEG: {
        u32 jpeg_hdr_size = 0;
        std::memcpy(&jpeg_hdr_size, header.palette.data(), 4);
        if (jpeg_hdr_size > MAX_BLP2_JPEG_HEADER_SIZE) {
            jpeg_hdr_size = MAX_BLP2_JPEG_HEADER_SIZE;
        }
        // Point into the original buffer (not the local header copy) so the
        // pointer remains valid after this function returns.
        static constexpr size_t PALETTE_FIELD_OFFSET = offsetof(BLP2Header, palette);
        ctx.encoding = BlpEncoding::JPEG;
        ctx.jpeg_header = ctx.buffer.data() + PALETTE_FIELD_OFFSET + 4;
        ctx.jpeg_header_size = jpeg_hdr_size;
        ctx.jpeg_alpha_opaque = (header.alphaBitDepth == 0);
        break;
    }
    case ENCODING_PALETTIZED: {
        std::memcpy(ctx.palette.data(), header.palette.data(), sizeof(ctx.palette));
        ctx.encoding = BlpEncoding::Palettized;
        ctx.alpha_bits = sanitize_alpha_bits(header.alphaBitDepth);
        break;
    }
    case ENCODING_DXT: {
        auto fmt = dxt_alpha_type_to_format(header.alphaType);
        if (!fmt) {
            return fail("BLP2 unsupported DXT alphaType: " + std::to_string(header.alphaType));
        }
        ctx.encoding = BlpEncoding::DXT;
        ctx.pixel_format = *fmt;
        break;
    }
    case ENCODING_BGRA:
    case ENCODING_BGRA_DUP:
        ctx.encoding = BlpEncoding::BGRA;
        break;
    default:
        return fail("BLP2 unsupported colorEncoding: " + std::to_string(header.colorEncoding));
    }

    return true;
}

// ============================================================================
// Unified mip decoder — works identically for BLP1 and BLP2
// ============================================================================

std::optional<Texture> Parser::Impl::decodeMips(const ParseContext& ctx) {
    Texture result = Texture::create2D(ctx.pixel_format, ctx.width, ctx.height, ctx.mipCount);

    switch (ctx.encoding) {
    case BlpEncoding::JPEG:
        if (!loadJpegMips(ctx, result)) {
            return std::nullopt;
        }
        return result;

    case BlpEncoding::Palettized:
        loadPalettizedMips(ctx, result);
        return result;

    case BlpEncoding::DXT:
        for (u32 mip = 0; mip < ctx.mipCount; ++mip) {
            copy_mip_raw(ctx, mip, result.mipData(mip));
        }
        return result;

    case BlpEncoding::BGRA:
        for (u32 mip = 0; mip < ctx.mipCount; ++mip) {
            auto destination = result.mipData(mip);
            auto span = ctx.mip_span(ctx.mip_offsets[mip], ctx.mip_sizes[mip], mip);
            if (span.empty()) {
                std::memset(destination.data(), 0, destination.size());
                continue;
            }

            const auto& mip_info = result.mipLevel(mip);
            const u32 pixel_count = mip_info.width * mip_info.height;
            const u32 available_pixels = std::min(pixel_count, static_cast<u32>(span.size()) / 4);

            swap_red_blue(span.data(), destination.data(), available_pixels);

            if (available_pixels < pixel_count) {
                std::memset(destination.data() + available_pixels * 4, 0,
                            (pixel_count - available_pixels) * 4);
            }
        }
        return result;

    default:
        fail("Unresolved BLP encoding");
        return std::nullopt;
    }
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
    return pImpl->parse(std::span<const u8>{*buf}, filePath);
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

} // namespace whiteout::textures::blp
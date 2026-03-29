// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/blp/writer.h>

#include "../jpeg/jpeg_encode.h"
#include "../utils/quantize.h"
#include "blp_internal.h"

#include "../issue_sink.h"

#include "../io_helpers.h"
#include "../utils/srgb_linearize.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace whiteout::textures::blp {

namespace {

// ============================================================================
// Writer-side helpers
// ============================================================================

wu::QuantizeResult build_optimal_palette(const Texture& texture, u32 mipCount, bool dither,
                                         f32 dither_strength, interfaces::WorkerPool* pool) {
    u64 total_pixels = 0;
    for (u32 mip = 0; mip < mipCount; ++mip) {
        const auto& mip_info = texture.mipLevel(mip);
        total_pixels += static_cast<u64>(mip_info.width) * mip_info.height;
    }

    std::vector<u8> all_rgba(total_pixels * 4);
    u64 offset = 0;
    for (u32 mip = 0; mip < mipCount; ++mip) {
        const auto src = texture.mipData(mip);
        std::memcpy(all_rgba.data() + offset, src.data(), src.size());
        offset += src.size();
    }

    auto quantizer = wu::Quantizer();
    if (pool)
        quantizer.workerPool(pool);
    if (dither) {
        const auto& base = texture.mipLevel(0);
        quantizer.ditherAware(base.width, base.height, dither_strength);
    }
    return quantizer.quantize(all_rgba.data(), static_cast<u32>(total_pixels));
}

std::vector<u8> encode_alpha_data(const u8* rgba, u32 pixel_count, u32 alpha_bits) {
    switch (alpha_bits) {
    case 1: {
        std::vector<u8> packed((pixel_count + 7) / 8, 0u);
        for (u32 i = 0; i < pixel_count; ++i)
            if (rgba[i * 4 + 3] >= 128)
                packed[i / 8] |= static_cast<u8>(1u << (i % 8));
        return packed;
    }
    case 4: {
        std::vector<u8> packed((pixel_count + 1) / 2, 0u);
        for (u32 i = 0; i < pixel_count; ++i) {
            const u8 nibble = rgba[i * 4 + 3] >> 4;
            if (i % 2 == 0)
                packed[i / 2] = nibble;
            else
                packed[i / 2] |= static_cast<u8>(nibble << 4);
        }
        return packed;
    }
    case 8: {
        std::vector<u8> packed(pixel_count);
        for (u32 i = 0; i < pixel_count; ++i)
            packed[i] = rgba[i * 4 + 3];
        return packed;
    }
    default:
        return {};
    }
}

std::vector<u8> encode_palettized_mip(const u8* rgba, u32 width, u32 height, u32 alpha_bits,
                                      const wu::QuantizeResult& quantization, bool dither,
                                      f32 dither_strength) {
    const u32 pixel_count = width * height;
    std::vector<u8> indices(pixel_count);
    if (dither) {
        quantization.mapPixelsDithered(rgba, width, height, dither_strength, indices.data());
    } else {
        quantization.mapPixels(rgba, pixel_count, indices.data());
    }
    auto alpha = encode_alpha_data(rgba, pixel_count, alpha_bits);
    indices.insert(indices.end(), alpha.begin(), alpha.end());
    return indices;
}

std::vector<u8> encode_jpeg_mip(const u8* rgba, u32 width, u32 height, bool with_alpha, i32 quality,
                                bool progressive, std::string* out_error) {
    const u32 pixel_count = width * height;
    const u32 components = with_alpha ? 4u : 3u;
    jpeg::Image image;
    image.width = width;
    image.height = height;
    image.components = components;
    image.pixels.resize(pixel_count * components);
    for (u32 i = 0; i < pixel_count; ++i) {
        image.pixels[i * components + 0] = rgba[i * 4 + 2]; // B
        image.pixels[i * components + 1] = rgba[i * 4 + 1]; // G
        image.pixels[i * components + 2] = rgba[i * 4 + 0]; // R
        if (with_alpha)
            image.pixels[i * components + 3] = rgba[i * 4 + 3]; // A
    }
    return jpeg::encode_raw(image, quality, out_error, progressive);
}

} // anonymous namespace

/// Transient state for a single write operation.  Passed through the pipeline
/// so that Impl itself remains stateless between calls.
struct WriteContext {
    const Texture* texture{};
    SaveOptions opts;
    u32 mipCount{};
    u32 alpha_bits{};
    BlpEncoding encoding{BlpEncoding::Infer};
    std::array<u32, 256> palette{};
};

class Writer::Impl : public IssueSink {
public:
    interfaces::WorkerPool* pool = nullptr;

    std::vector<u8> write(const Texture& texture, const SaveOptions& opts);

private:
    void resolveEncoding(WriteContext& ctx);
    void validate(const WriteContext& ctx);
    std::vector<std::vector<u8>> encodeMipPayloads(WriteContext& ctx);
    std::vector<u8> buildBlp1(const WriteContext& ctx,
                              const std::vector<std::vector<u8>>& payloads);
    std::vector<u8> buildBlp2(const WriteContext& ctx,
                              const std::vector<std::vector<u8>>& payloads);
};

// ============================================================================
// Writer
// ============================================================================

std::vector<u8> Writer::Impl::write(const Texture& texture, const SaveOptions& opts) {
    issues.clear();

    // BLP has no sRGB flag — linearize if the source is sRGB.
    Texture linearized;
    const Texture* src = &texture;
    if (texture.isSrgb()) {
        linearized = linearizeSrgbCopy(texture);
        src = &linearized;
    }

    WriteContext ctx;
    ctx.texture = src;
    ctx.opts = opts;
    ctx.mipCount = std::min(src->mipCount(), MAX_MIP_LEVELS);
    ctx.alpha_bits = static_cast<u32>(opts.alpha);
    ctx.encoding = opts.encoding;
    ctx.palette.fill(0);

    if (src->width() == 0 || src->height() == 0) {
        fail("Cannot save an empty texture as BLP");
        return {};
    }
    if (src->type() != TextureType::Texture2D) {
        fail("BLP only supports 2D textures");
        return {};
    }

    resolveEncoding(ctx);
    if (!issues.empty())
        return {};
    validate(ctx);
    if (!issues.empty())
        return {};

    auto payloads = encodeMipPayloads(ctx);
    if (!issues.empty())
        return {};

    if (ctx.opts.version == BlpVersion::BLP1) {
        return buildBlp1(ctx, payloads);
    }
    return buildBlp2(ctx, payloads);
}

void Writer::Impl::resolveEncoding(WriteContext& ctx) {
    if (ctx.encoding != BlpEncoding::Infer) {
        return;
    }

    if (ctx.opts.version == BlpVersion::BLP1) {
        if (ctx.texture->format() != PixelFormat::RGBA8) {
            fail("BLP1 Infer: only RGBA8 textures are supported");
            return;
        }
        ctx.encoding = BlpEncoding::Palettized;
    } else {
        switch (ctx.texture->format()) {
        case PixelFormat::RGBA8:
            ctx.encoding = BlpEncoding::BGRA;
            break;
        case PixelFormat::BC1:
        case PixelFormat::BC2:
        case PixelFormat::BC3:
            ctx.encoding = BlpEncoding::DXT;
            break;
        default:
            fail("BLP2 Infer: pixel format not representable in BLP "
                 "(only RGBA8, BC1, BC2, BC3 supported)");
            return;
        }
    }
}

void Writer::Impl::validate(const WriteContext& ctx) {
    if (ctx.opts.version == BlpVersion::BLP1 &&
        (ctx.encoding == BlpEncoding::BGRA || ctx.encoding == BlpEncoding::DXT)) {
        fail("BLP1 does not support BGRA or DXT encoding");
        return;
    }
    if ((ctx.encoding == BlpEncoding::JPEG || ctx.encoding == BlpEncoding::Palettized ||
         ctx.encoding == BlpEncoding::BGRA) &&
        ctx.texture->format() != PixelFormat::RGBA8) {
        fail("JPEG, Palettized, and BGRA encodings require an RGBA8 texture");
        return;
    }
    if (ctx.encoding == BlpEncoding::DXT && ctx.texture->format() != PixelFormat::BC1 &&
        ctx.texture->format() != PixelFormat::BC2 && ctx.texture->format() != PixelFormat::BC3) {
        fail("DXT encoding requires a BC1, BC2, or BC3 texture");
    }
}

std::vector<std::vector<u8>> Writer::Impl::encodeMipPayloads(WriteContext& ctx) {
    wu::QuantizeResult quantized;
    if (ctx.encoding == BlpEncoding::Palettized) {
        quantized = build_optimal_palette(*ctx.texture, ctx.mipCount, ctx.opts.dither,
                                          ctx.opts.ditherStrength, pool);
        std::memcpy(ctx.palette.data(), quantized.palette.data(), sizeof(ctx.palette));
    }

    std::vector<std::vector<u8>> payloads(ctx.mipCount);
    for (u32 mip = 0; mip < ctx.mipCount; ++mip) {
        const auto& mip_info = ctx.texture->mipLevel(mip);
        const auto source = ctx.texture->mipData(mip);

        switch (ctx.encoding) {
        case BlpEncoding::JPEG: {
            const bool with_alpha = (ctx.alpha_bits == 8);
            std::string jpeg_error;
            payloads[mip] =
                encode_jpeg_mip(source.data(), mip_info.width, mip_info.height, with_alpha,
                                ctx.opts.jpegQuality, ctx.opts.jpegProgressive, &jpeg_error);
            if (payloads[mip].empty()) {
                fail(jpeg_error.empty() ? "JPEG mip encode failed" : jpeg_error);
                return {};
            }
            break;
        }
        case BlpEncoding::Palettized: {
            payloads[mip] = encode_palettized_mip(source.data(), mip_info.width, mip_info.height,
                                                  ctx.alpha_bits, quantized, ctx.opts.dither,
                                                  ctx.opts.ditherStrength);
            break;
        }
        case BlpEncoding::BGRA: {
            payloads[mip].resize(mip_info.width * mip_info.height * 4);
            swap_red_blue(source.data(), payloads[mip].data(), mip_info.width * mip_info.height);
            break;
        }
        case BlpEncoding::DXT: {
            payloads[mip].assign(source.begin(), source.end());
            break;
        }
        default:
            break;
        }
    }
    return payloads;
}

std::vector<u8> Writer::Impl::buildBlp1(const WriteContext& ctx,
                                        const std::vector<std::vector<u8>>& payloads) {
    const bool is_jpeg = (ctx.encoding == BlpEncoding::JPEG);
    const size_t fixed_size =
        sizeof(BLP1Header) + sizeof(MipmapLocator) + (is_jpeg ? sizeof(u32) : 256u * sizeof(u32));

    u64 total_mip_bytes = 0;
    for (u32 mip = 0; mip < ctx.mipCount; ++mip)
        total_mip_bytes += payloads[mip].size();

    std::vector<u8> output(fixed_size + static_cast<size_t>(total_mip_bytes), 0u);

    BLP1Header header{};
    header.magic = BLP1_MAGIC;
    header.content = is_jpeg ? CONTENT_JPEG : CONTENT_DIRECT;
    header.alphaBitDepth = ctx.alpha_bits;
    header.width = ctx.texture->width();
    header.height = ctx.texture->height();
    header.extra = 5;
    header.hasMipmaps = (ctx.mipCount > 1) ? 1u : 0u;
    std::memcpy(output.data(), &header, sizeof(BLP1Header));

    MipmapLocator mip_locator{};
    fill_mip_table(mip_locator.mipOffsets, mip_locator.mipSizes, payloads, ctx.mipCount,
                   static_cast<u32>(fixed_size));
    std::memcpy(output.data() + sizeof(BLP1Header), &mip_locator, sizeof(MipmapLocator));

    if (!is_jpeg) {
        std::memcpy(output.data() + sizeof(BLP1Header) + sizeof(MipmapLocator), ctx.palette.data(),
                    256 * sizeof(u32));
    }
    // For JPEG: shared jpeg_hdr_size = 0 is already zero-initialised above.

    write_mip_payloads(output.data(), mip_locator.mipOffsets, payloads, ctx.mipCount);
    return output;
}

std::vector<u8> Writer::Impl::buildBlp2(const WriteContext& ctx,
                                        const std::vector<std::vector<u8>>& payloads) {
    u8 color_encoding;
    u8 alpha_bit_depth_val;
    u8 alpha_type = 0;

    switch (ctx.encoding) {
    case BlpEncoding::JPEG:
        color_encoding = ENCODING_JPEG;
        alpha_bit_depth_val = (ctx.alpha_bits == 8) ? 8u : 0u;
        break;
    case BlpEncoding::Palettized:
        color_encoding = ENCODING_PALETTIZED;
        alpha_bit_depth_val = static_cast<u8>(ctx.alpha_bits);
        break;
    case BlpEncoding::BGRA:
        color_encoding = ENCODING_BGRA;
        alpha_bit_depth_val = 8;
        break;
    case BlpEncoding::DXT:
        color_encoding = ENCODING_DXT;
        switch (ctx.texture->format()) {
        case PixelFormat::BC1:
            alpha_bit_depth_val = 1;
            alpha_type = 0;
            break;
        case PixelFormat::BC2:
            alpha_bit_depth_val = 4;
            alpha_type = 1;
            break;
        case PixelFormat::BC3:
            alpha_bit_depth_val = 8;
            alpha_type = 7;
            break;
        default:
            alpha_bit_depth_val = 0;
            break;
        }
        break;
    default:
        fail("Unresolved BLP encoding");
        return {};
    }

    u64 total_mip_bytes = 0;
    for (u32 mip = 0; mip < ctx.mipCount; ++mip)
        total_mip_bytes += payloads[mip].size();

    std::vector<u8> output(sizeof(BLP2Header) + static_cast<size_t>(total_mip_bytes), 0u);

    BLP2Header header{};
    header.magic = BLP2_MAGIC;
    header.version = 1;
    header.colorEncoding = color_encoding;
    header.alphaBitDepth = alpha_bit_depth_val;
    header.alphaType = alpha_type;
    header.hasMipmaps = (ctx.mipCount > 1) ? 1 : 0;
    header.width = ctx.texture->width();
    header.height = ctx.texture->height();

    fill_mip_table(header.mipOffsets, header.mipSizes, payloads, ctx.mipCount,
                   static_cast<u32>(sizeof(BLP2Header)));

    if (ctx.encoding == BlpEncoding::Palettized)
        std::memcpy(header.palette.data(), ctx.palette.data(), 256 * sizeof(u32));
    // For JPEG: header.palette[0] = 0 (shared jpeg_hdr_size = 0) -- already zero.

    std::memcpy(output.data(), &header, sizeof(BLP2Header));
    write_mip_payloads(output.data(), header.mipOffsets, payloads, ctx.mipCount);

    return output;
}

Writer::Writer(WriteMode writeMode, interfaces::WorkerPool* pool)
    : pImpl(std::make_unique<Impl>()) {
    pImpl->strict_mode = (writeMode == WriteMode::Strict);
    pImpl->pool = pool;
}

Writer::~Writer() = default;

void Writer::write(const std::string& filePath, const Texture& texture) {
    write(filePath, texture, SaveOptions{});
}

std::vector<u8> Writer::write(const Texture& texture) {
    return write(texture, SaveOptions{});
}

void Writer::write(const std::string& filePath, const Texture& texture, const SaveOptions& opts) {
    auto data = pImpl->write(texture, opts);
    if (data.empty()) {
        return; // issues already logged in Lenient mode, or threw in Strict mode
    }
    if (!write_file_bytes(filePath, data, *pImpl)) {
        return;
    }
}

std::vector<u8> Writer::write(const Texture& texture, const SaveOptions& opts) {
    return pImpl->write(texture, opts);
}

bool Writer::hasIssues() const {
    return !pImpl->issues.empty();
}

const std::vector<std::string>& Writer::getIssues() const {
    return pImpl->issues;
}

} // namespace whiteout::textures::blp
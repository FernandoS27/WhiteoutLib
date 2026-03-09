// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/texture.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <stdexcept>

#include "bcn.h"
#include "pixel_convert.h"

namespace whiteout::textures {

u32 bytesPerBlock(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::RGBA8:
        return 4;
    case PixelFormat::RGBA16F:
        return 8;
    case PixelFormat::RGBA32F:
        return 16;
    case PixelFormat::BC1:
        return 8;
    case PixelFormat::BC2:
        return 16;
    case PixelFormat::BC3:
        return 16;
    case PixelFormat::BC4:
        return 8;
    case PixelFormat::BC5:
        return 16;
    case PixelFormat::BC6H:
        return 16;
    case PixelFormat::BC7:
        return 16;
    }
    return 0;
}

u32 blockEdge(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::RGBA8:
    case PixelFormat::RGBA16F:
    case PixelFormat::RGBA32F:
        return 1;
    default:
        return 4;
    }
}

u64 computeImageSize(PixelFormat fmt, u32 width, u32 height) {
    const u32 edge = blockEdge(fmt);
    const u32 bpb = bytesPerBlock(fmt);

    const u32 blocks_x = (width + edge - 1) / edge;
    const u32 blocks_y = (height + edge - 1) / edge;

    return static_cast<u64>(blocks_x) * blocks_y * bpb;
}

struct Texture::Impl {
    TextureType type = TextureType::Texture2D;
    PixelFormat format = PixelFormat::RGBA8;

    u32 width = 0;
    u32 height = 0;
    u32 depth = 1;

    std::vector<MipLevel> mips;

    std::vector<u8> data;

    u32 layerCount() const {
        return type == TextureType::TextureCube ? 6u : 1u;
    }

    u32 mipCount() const {
        return static_cast<u32>(mips.size()) / layerCount();
    }
};

u32 computeMaxMipCount(u32 w, u32 h, u32 d) {
    u32 dim = std::max({w, h, d});
    u32 count = 1;
    while (dim > 1) {
        dim >>= 1;
        ++count;
    }
    return count;
}

namespace {

u64 build_mip_chain(PixelFormat fmt, u32 w, u32 h, u32 d, u32 mipCount, u32 layers,
                    std::vector<MipLevel>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(layers) * mipCount);

    u64 offset = 0;
    for (u32 layer = 0; layer < layers; ++layer) {
        u32 mw = w, mh = h, md = d;
        for (u32 mip = 0; mip < mipCount; ++mip) {
            const u64 slice_size = computeImageSize(fmt, mw, mh);
            const u64 total = slice_size * md;

            out.push_back(MipLevel{
                .width = mw,
                .height = mh,
                .depth = md,
                .offset = offset,
                .size = total,
            });

            offset += total;

            mw = std::max(mw >> 1, 1u);
            mh = std::max(mh >> 1, 1u);
            md = std::max(md >> 1, 1u);
        }
    }
    return offset;
}

} // anonymous namespace

Texture::Texture() : impl_(std::make_unique<Impl>()) {}
Texture::~Texture() = default;

Texture::Texture(const Texture& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}

Texture& Texture::operator=(const Texture& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}

Texture::Texture(Texture&& other) noexcept = default;
Texture& Texture::operator=(Texture&& other) noexcept = default;

Texture Texture::create2D(PixelFormat fmt, u32 width, u32 height, u32 mipCount) {
    assert(width > 0 && height > 0);

    if (mipCount == 0) {
        mipCount = computeMaxMipCount(width, height, 1);
    }

    Texture tex;
    tex.impl_->type = TextureType::Texture2D;
    tex.impl_->format = fmt;
    tex.impl_->width = width;
    tex.impl_->height = height;
    tex.impl_->depth = 1;

    const u64 total = build_mip_chain(fmt, width, height, 1, mipCount, 1, tex.impl_->mips);
    tex.impl_->data.resize(static_cast<size_t>(total), 0);
    return tex;
}

Texture Texture::create3D(PixelFormat fmt, u32 width, u32 height, u32 depth, u32 mipCount) {
    assert(width > 0 && height > 0 && depth > 0);

    if (mipCount == 0) {
        mipCount = computeMaxMipCount(width, height, depth);
    }

    Texture tex;
    tex.impl_->type = TextureType::Texture3D;
    tex.impl_->format = fmt;
    tex.impl_->width = width;
    tex.impl_->height = height;
    tex.impl_->depth = depth;

    const u64 total = build_mip_chain(fmt, width, height, depth, mipCount, 1, tex.impl_->mips);
    tex.impl_->data.resize(static_cast<size_t>(total), 0);
    return tex;
}

Texture Texture::createCube(PixelFormat fmt, u32 size, u32 mipCount) {
    assert(size > 0);

    if (mipCount == 0) {
        mipCount = computeMaxMipCount(size, size, 1);
    }

    Texture tex;
    tex.impl_->type = TextureType::TextureCube;
    tex.impl_->format = fmt;
    tex.impl_->width = size;
    tex.impl_->height = size;
    tex.impl_->depth = 1;

    const u64 total = build_mip_chain(fmt, size, size, 1, mipCount, 6, tex.impl_->mips);
    tex.impl_->data.resize(static_cast<size_t>(total), 0);
    return tex;
}

TextureType Texture::type() const {
    return impl_->type;
}
PixelFormat Texture::format() const {
    return impl_->format;
}

u32 Texture::width() const {
    return impl_->width;
}
u32 Texture::height() const {
    return impl_->height;
}
u32 Texture::depth() const {
    return impl_->depth;
}

u32 Texture::layerCount() const {
    return impl_->layerCount();
}

u32 Texture::mipCount() const {
    return impl_->mipCount();
}

const MipLevel& Texture::mipLevel(u32 mip, u32 layer) const {
    return impl_->mips[layer * mipCount() + mip];
}

u64 Texture::dataSize() const {
    return impl_->data.size();
}

std::span<const u8> Texture::data() const {
    return {impl_->data.data(), impl_->data.size()};
}

std::span<u8> Texture::data() {
    return {impl_->data.data(), impl_->data.size()};
}

const u8* Texture::dataPtr() const {
    return impl_->data.data();
}

u8* Texture::dataPtr() {
    return impl_->data.data();
}

std::span<const u8> Texture::mipData(u32 mip, u32 layer) const {
    const auto& m = mipLevel(mip, layer);
    return {impl_->data.data() + m.offset, m.size};
}

std::span<u8> Texture::mipData(u32 mip, u32 layer) {
    const auto& m = mipLevel(mip, layer);
    return {impl_->data.data() + m.offset, m.size};
}

std::vector<u8> Texture::takeData() {
    impl_->mips.clear();
    impl_->width = impl_->height = impl_->depth = 0;
    return std::move(impl_->data);
}

void Texture::setData(std::vector<u8> new_data) {
    assert(new_data.size() == impl_->data.size());
    impl_->data = std::move(new_data);
}

namespace {

// ============================================================================
// Uncompressed format conversion
// ============================================================================

/// Convert an image between two uncompressed formats (RGBA8, RGBA16F, RGBA32F).
/// Both textures must already have the same dimensions and mip structure.
Texture convert_uncompressed(const Texture& src, PixelFormat new_fmt) {
    assert(!bcn::isCompressed(src.format()));
    assert(!bcn::isCompressed(new_fmt));

    const PixelFormat src_fmt = src.format();
    if (src_fmt == new_fmt)
        return src;

    const u32 layers = src.layerCount();
    const u32 mips = src.mipCount();

    // Build destination texture with the same shape but the new format.
    Texture dst;
    switch (src.type()) {
    case TextureType::Texture2D:
        dst = Texture::create2D(new_fmt, src.width(), src.height(), mips);
        break;
    case TextureType::Texture3D:
        dst = Texture::create3D(new_fmt, src.width(), src.height(), src.depth(), mips);
        break;
    case TextureType::TextureCube:
        dst = Texture::createCube(new_fmt, src.width(), mips);
        break;
    }

    // Select the per-pixel converter once, outside all loops.
    using PixelConverter = void (*)(const u8* src, u8* dst);

    PixelConverter convert_pixel = nullptr;

    if (src_fmt == PixelFormat::RGBA8 && new_fmt == PixelFormat::RGBA32F) {
        convert_pixel = [](const u8* s, u8* d) { rgba8_to_rgba32f(s, reinterpret_cast<f32*>(d)); };
    } else if (src_fmt == PixelFormat::RGBA32F && new_fmt == PixelFormat::RGBA8) {
        convert_pixel = [](const u8* s, u8* d) {
            rgba32f_to_rgba8(reinterpret_cast<const f32*>(s), d);
        };
    } else if (src_fmt == PixelFormat::RGBA8 && new_fmt == PixelFormat::RGBA16F) {
        convert_pixel = [](const u8* s, u8* d) {
            std::array<f32, 4> tmp;
            rgba8_to_rgba32f(s, tmp.data());
            rgba32f_to_rgba16f(tmp.data(), reinterpret_cast<u16*>(d));
        };
    } else if (src_fmt == PixelFormat::RGBA16F && new_fmt == PixelFormat::RGBA8) {
        convert_pixel = [](const u8* s, u8* d) {
            std::array<f32, 4> tmp;
            rgba16f_to_rgba32f(reinterpret_cast<const u16*>(s), tmp.data());
            rgba32f_to_rgba8(tmp.data(), d);
        };
    } else if (src_fmt == PixelFormat::RGBA16F && new_fmt == PixelFormat::RGBA32F) {
        convert_pixel = [](const u8* s, u8* d) {
            rgba16f_to_rgba32f(reinterpret_cast<const u16*>(s), reinterpret_cast<f32*>(d));
        };
    } else if (src_fmt == PixelFormat::RGBA32F && new_fmt == PixelFormat::RGBA16F) {
        convert_pixel = [](const u8* s, u8* d) {
            rgba32f_to_rgba16f(reinterpret_cast<const f32*>(s), reinterpret_cast<u16*>(d));
        };
    }

    assert(convert_pixel && "unsupported format pair");

    const u32 src_bpp = bytesPerBlock(src_fmt); // bytes per pixel for uncompressed
    const u32 dst_bpp = bytesPerBlock(new_fmt);

    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mips; ++mip) {
            const auto src_span = src.mipData(mip, layer);
            auto dst_span = dst.mipData(mip, layer);

            const u32 w = src.mipLevel(mip, layer).width;
            const u32 h = src.mipLevel(mip, layer).height;
            const u32 d = src.mipLevel(mip, layer).depth;
            const u32 n = w * h * d;

            const u8* s = src_span.data();
            u8* d8 = dst_span.data();

            for (u32 px = 0; px < n; ++px) {
                convert_pixel(s + px * src_bpp, d8 + px * dst_bpp);
            }
        }
    }
    return dst;
}

} // anonymous namespace

// ============================================================================
// format() and copyAsFormat()
// ============================================================================

/// Convert this texture in-place to @p new_fmt.
///
/// Conversion path:
///   1. Same format → no-op.
///   2. BCn source  → decode to RGBA8 (or RGBA16F for BC6H), then proceed.
///   3. Uncompressed → uncompressed → pixel-level conversion.
///   4. Uncompressed → BCn → bcn::encode (with an intermediate pixel-level
///      conversion if the decoded format doesn't match what the encoder needs).
void Texture::format(PixelFormat new_fmt) {
    *this = copyAsFormat(new_fmt);
}

Texture Texture::copyAsFormat(PixelFormat new_fmt) const {
    // 1. Same format — just copy.
    if (impl_->format == new_fmt)
        return *this;

    // 2. If source is compressed, decode it first.
    if (bcn::isCompressed(impl_->format)) {
        std::string err;
        auto decoded = bcn::decode(*this, &err);
        if (!decoded)
            throw std::runtime_error("Texture::copyAsFormat: decode failed: " + err);
        return decoded->copyAsFormat(new_fmt);
    }

    // Source is now guaranteed to be uncompressed.

    // 3. Target is also uncompressed.
    if (!bcn::isCompressed(new_fmt))
        return convert_uncompressed(*this, new_fmt);

    // 4. Target is BCn — encode.
    //    bcn::encode requires RGBA8 for BC1–BC5/BC7 and RGBA16F/RGBA32F for BC6H.
    const PixelFormat needed =
        (new_fmt == PixelFormat::BC6H) ? PixelFormat::RGBA16F : PixelFormat::RGBA8;

    // Convert to the needed intermediate format if necessary.
    const Texture* src_ptr = this;
    Texture intermediate;
    if (impl_->format != needed) {
        intermediate = convert_uncompressed(*this, needed);
        src_ptr = &intermediate;
    }

    std::string err;
    auto encoded = bcn::encode(*src_ptr, new_fmt, &err);
    if (!encoded)
        throw std::runtime_error("Texture::copyAsFormat: encode failed: " + err);
    return std::move(*encoded);
}

} // namespace whiteout::textures

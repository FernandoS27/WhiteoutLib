// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/texture.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "bcn.h"
#include "mipmap/generator.h"
#include "mipmap/mip_convert.h"
#include "mipmap/stages.h"
#include "utils/pixel_convert.h"
#include "utils/srgb_linearize.h"

namespace whiteout::textures {

u32 bytesPerBlock(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::R8:
        return 1;
    case PixelFormat::R16:
        return 2;
    case PixelFormat::R32F:
        return 4;
    case PixelFormat::RG8:
        return 2;
    case PixelFormat::RG16:
        return 4;
    case PixelFormat::RG32F:
        return 8;
    case PixelFormat::RGBA8:
        return 4;
    case PixelFormat::RGBA16:
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
    case PixelFormat::R8:
    case PixelFormat::R16:
    case PixelFormat::R32F:
    case PixelFormat::RG8:
    case PixelFormat::RG16:
    case PixelFormat::RG32F:
    case PixelFormat::RGBA8:
    case PixelFormat::RGBA16:
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
    TextureKind kind = TextureKind::Other;
    // Per-channel kinds — only meaningful when kind == Multikind.
    std::array<TextureKind, 4> channelKinds = {TextureKind::Other, TextureKind::Other,
                                               TextureKind::Other, TextureKind::Other};
    // Per-channel default fill values; 1.0f by convention for all channels.
    std::array<f32, 4> channelDefaults = {1.0f, 1.0f, 1.0f, 1.0f};
    bool srgb = false;

    u32 width = 0;
    u32 height = 0;
    u32 depth = 1;
    // Number of array slices (cube-maps for TextureCubeArray,
    // 2D images for Texture2DArray). 1 for non-array types.
    u32 arraySize = 1;

    std::vector<MipLevel> mips;

    std::vector<u8> data;

    u32 layerCount() const {
        switch (type) {
        case TextureType::TextureCube:
            return 6u;
        case TextureType::Texture2DArray:
            return arraySize;
        case TextureType::TextureCubeArray:
            return 6u * arraySize;
        default:
            return 1u;
        }
    }

    u32 mipCount() const {
        const u32 layers = layerCount();
        return layers == 0 ? 0u : static_cast<u32>(mips.size()) / layers;
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

Texture Texture::create2DArray(PixelFormat fmt, u32 width, u32 height, u32 arraySize,
                               u32 mipCount) {
    assert(width > 0 && height > 0 && arraySize > 0);

    if (mipCount == 0) {
        mipCount = computeMaxMipCount(width, height, 1);
    }

    Texture tex;
    tex.impl_->type = TextureType::Texture2DArray;
    tex.impl_->format = fmt;
    tex.impl_->width = width;
    tex.impl_->height = height;
    tex.impl_->depth = 1;
    tex.impl_->arraySize = arraySize;

    const u64 total =
        build_mip_chain(fmt, width, height, 1, mipCount, arraySize, tex.impl_->mips);
    tex.impl_->data.resize(static_cast<size_t>(total), 0);
    return tex;
}

Texture Texture::createCubeArray(PixelFormat fmt, u32 size, u32 arraySize, u32 mipCount) {
    assert(size > 0 && arraySize > 0);

    if (mipCount == 0) {
        mipCount = computeMaxMipCount(size, size, 1);
    }

    Texture tex;
    tex.impl_->type = TextureType::TextureCubeArray;
    tex.impl_->format = fmt;
    tex.impl_->width = size;
    tex.impl_->height = size;
    tex.impl_->depth = 1;
    tex.impl_->arraySize = arraySize;

    const u32 totalLayers = 6u * arraySize;
    const u64 total =
        build_mip_chain(fmt, size, size, 1, mipCount, totalLayers, tex.impl_->mips);
    tex.impl_->data.resize(static_cast<size_t>(total), 0);
    return tex;
}

TextureType Texture::type() const {
    return impl_->type;
}
PixelFormat Texture::format() const {
    return impl_->format;
}
TextureKind Texture::kind() const {
    return impl_->kind;
}
void Texture::setKind(TextureKind k) {
    if (k == TextureKind::ORM) {
        impl_->kind = TextureKind::Multikind;
        impl_->channelKinds[0] = TextureKind::AmbientOcclusion;
        impl_->channelKinds[1] = TextureKind::Roughness;
        impl_->channelKinds[2] = TextureKind::Metalness;
        impl_->channelKinds[3] = TextureKind::Unused;
        return;
    }
    impl_->kind = k;
}
TextureKind Texture::channelKind(Channel ch) const {
    const u32 idx = static_cast<u32>(ch);
    return (idx < 4) ? impl_->channelKinds[idx] : TextureKind::Other;
}
void Texture::setChannelKind(Channel ch, TextureKind kind) {
    const u32 idx = static_cast<u32>(ch);
    if (idx < 4)
        impl_->channelKinds[idx] = kind;
}
f32 Texture::channelDefault(Channel ch) const {
    const u32 idx = static_cast<u32>(ch);
    return (idx < 4) ? impl_->channelDefaults[idx] : 1.0f;
}
void Texture::setChannelDefault(Channel ch, f32 value) {
    const u32 idx = static_cast<u32>(ch);
    if (idx < 4)
        impl_->channelDefaults[idx] = value;
}
bool Texture::isSrgb() const {
    return impl_->srgb;
}
void Texture::setSrgb(bool srgb) {
    impl_->srgb = srgb;
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

u32 Texture::arraySize() const {
    return impl_->arraySize;
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

// Runtime format-info tables — auto-derived from FormatTraits.
template <PixelFormat... Fmts>
constexpr std::array<u32, sizeof...(Fmts)> make_channels_table(FormatList<Fmts...>) {
    return {{FormatTraits<Fmts>::channels...}};
}

template <PixelFormat... Fmts>
constexpr std::array<u32, sizeof...(Fmts)> make_bpc_table(FormatList<Fmts...>) {
    return {{(FormatTraits<Fmts>::bytes_per_pixel / FormatTraits<Fmts>::channels)...}};
}

constexpr auto kChannelCounts = make_channels_table(UncompressedFormats{});
constexpr auto kBytesPerChannel = make_bpc_table(UncompressedFormats{});

u32 format_channel_count(PixelFormat fmt) {
    const u32 idx = static_cast<u32>(fmt);
    return (idx < kUncompressedCount) ? kChannelCounts[idx] : 0u;
}

u32 format_bytes_per_channel(PixelFormat fmt) {
    const u32 idx = static_cast<u32>(fmt);
    return (idx < kUncompressedCount) ? kBytesPerChannel[idx] : 0u;
}

PixelFormat single_channel_format(PixelFormat fmt) {
    switch (format_bytes_per_channel(fmt)) {
    case 1:
        return PixelFormat::R8;
    case 2:
        return PixelFormat::R16;
    case 4:
        return PixelFormat::R32F;
    default:
        return fmt;
    }
}

PixelFormat rgba_format_for(PixelFormat fmt) {
    switch (format_bytes_per_channel(fmt)) {
    case 1:
        return PixelFormat::RGBA8;
    case 2:
        return PixelFormat::RGBA16;
    case 4:
        return PixelFormat::RGBA32F;
    default:
        return fmt;
    }
}

bool is_rg_normal_format(PixelFormat format) {
    return format == PixelFormat::RG8 || format == PixelFormat::RG16 ||
           format == PixelFormat::RG32F;
}

bool is_rgba_normal_format(PixelFormat format) {
    return format == PixelFormat::RGBA8 || format == PixelFormat::RGBA16 ||
           format == PixelFormat::RGBA32F;
}

Texture make_texture_like(const Texture& src, PixelFormat new_fmt) {
    switch (src.type()) {
    case TextureType::Texture2D:
        return Texture::create2D(new_fmt, src.width(), src.height(), src.mipCount());
    case TextureType::Texture3D:
        return Texture::create3D(new_fmt, src.width(), src.height(), src.depth(), src.mipCount());
    case TextureType::TextureCube:
        return Texture::createCube(new_fmt, src.width(), src.mipCount());
    case TextureType::Texture2DArray:
        return Texture::create2DArray(new_fmt, src.width(), src.height(), src.arraySize(),
                                      src.mipCount());
    case TextureType::TextureCubeArray:
        return Texture::createCubeArray(new_fmt, src.width(), src.arraySize(), src.mipCount());
    }
    return {};
}

void copy_texture_metadata(const Texture& src, Texture& dst) {
    dst.setKind(src.kind());
    dst.setSrgb(src.isSrgb());
    if (src.kind() == TextureKind::Multikind) {
        for (u32 i = 0; i < 4; ++i) {
            const auto ch = static_cast<Channel>(i);
            dst.setChannelKind(ch, src.channelKind(ch));
        }
    }
}

// ============================================================================
// Uncompressed format conversion
// ============================================================================

/// Convert an image between any two uncompressed formats.
/// Uses direct channel conversion (no float intermediate) when possible.
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
    Texture dst = make_texture_like(src, new_fmt);
    copy_texture_metadata(src, dst);

    // Direct bulk conversion — avoids per-pixel float intermediate.
    auto fn = get_converter(src_fmt, new_fmt);
    assert(fn && "unsupported format pair");

    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mips; ++mip) {
            const auto src_span = src.mipData(mip, layer);
            auto dst_span = dst.mipData(mip, layer);

            const u32 w = src.mipLevel(mip, layer).width;
            const u32 h = src.mipLevel(mip, layer).height;
            const u32 d = src.mipLevel(mip, layer).depth;

            fn(src_span.data(), dst_span.data(), w * h * d);
        }
    }
    return dst;
}

std::optional<Texture> copy_normal_to_rgba8(const Texture& src, PixelFormat orig_fmt) {
    const bool is_rg = is_rg_normal_format(src.format());
    const bool is_rgba = is_rgba_normal_format(src.format());
    if (!is_rg && !is_rgba)
        return std::nullopt;

    size_t x = 0, y = 1;
    bool flip_y = false;
    // BC3N
    if (is_rgba && orig_fmt == PixelFormat::BC3) {
        y = 1;
        x = 3;
        flip_y = true;
    }

    Texture dst = make_texture_like(src, PixelFormat::RGBA8);
    copy_texture_metadata(src, dst);

    auto to_f32 = get_to_rgba32f(src.format());
    auto from_f32 = get_from_rgba32f(PixelFormat::RGBA8);
    if (!to_f32 || !from_f32)
        return std::nullopt;

    const u32 src_bpp = bytesPerBlock(src.format());
    const u32 layers = src.layerCount();
    const u32 mips = src.mipCount();

    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mips; ++mip) {
            const auto src_span = src.mipData(mip, layer);
            auto dst_span = dst.mipData(mip, layer);

            const u32 width = src.mipLevel(mip, layer).width;
            const u32 height = src.mipLevel(mip, layer).height;
            const u32 depth = src.mipLevel(mip, layer).depth;
            const u32 pixel_count = width * height * depth;

            const u8* src_bytes = src_span.data();
            u8* dst_bytes = dst_span.data();

            for (u32 pixel = 0; pixel < pixel_count; ++pixel) {
                f32 rgba[4];
                to_f32(src_bytes + pixel * src_bpp, rgba);

                const f32 x_value = rgba[x];
                const f32 y_value = rgba[y];
                const f32 normal_x = x_value * 2.0f - 1.0f;
                const f32 normal_y = y_value * 2.0f - 1.0f;
                const f32 normal_z =
                    std::sqrt(std::max(0.0f, 1.0f - normal_x * normal_x - normal_y * normal_y));
                rgba[0] = x_value;
                rgba[1] = (flip_y ? 1.0f - y_value : y_value);
                rgba[2] = (normal_z + 1.0f) * 0.5f;
                rgba[3] = 1.0f;
                from_f32(rgba, dst_bytes + pixel * bytesPerBlock(PixelFormat::RGBA8));
            }
        }
    }

    return dst;
}

// ============================================================================
// Channel operation dispatch structs
// ============================================================================

struct SwapChannelsOp {
    template <PixelFormat Fmt>
    static void apply(Texture::Impl& impl, u32 ai, u32 bi, bool& ok) {
        using T = typename FormatTraits<Fmt>::channel_type;
        constexpr u32 ch = FormatTraits<Fmt>::channels;
        constexpr u32 bpp = FormatTraits<Fmt>::bytes_per_pixel;

        if (ai >= ch || bi >= ch) {
            ok = false;
            return;
        }
        if (ai == bi) {
            ok = true;
            return;
        }

        const u32 layers = impl.layerCount();
        const u32 mips = impl.mipCount();

        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 mip = 0; mip < mips; ++mip) {
                const auto& lvl = impl.mips[layer * mips + mip];
                const u32 pixel_count = lvl.width * lvl.height * lvl.depth;
                u8* pixels = impl.data.data() + lvl.offset;

                for (u32 px = 0; px < pixel_count; ++px) {
                    u8* pixel = pixels + px * bpp;
                    T va, vb;
                    std::memcpy(&va, pixel + ai * sizeof(T), sizeof(T));
                    std::memcpy(&vb, pixel + bi * sizeof(T), sizeof(T));
                    std::memcpy(pixel + ai * sizeof(T), &vb, sizeof(T));
                    std::memcpy(pixel + bi * sizeof(T), &va, sizeof(T));
                }
            }
        }
        ok = true;
    }
};

struct InvertChannelOp {
    template <PixelFormat Fmt>
    static void apply(Texture::Impl& impl, u32 ci, bool& ok) {
        using T = typename FormatTraits<Fmt>::channel_type;
        constexpr u32 ch = FormatTraits<Fmt>::channels;
        constexpr u32 bpp = FormatTraits<Fmt>::bytes_per_pixel;

        if (ci >= ch) {
            ok = false;
            return;
        }

        const u32 layers = impl.layerCount();
        const u32 mips = impl.mipCount();

        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 mip = 0; mip < mips; ++mip) {
                const auto& lvl = impl.mips[layer * mips + mip];
                const u32 pixel_count = lvl.width * lvl.height * lvl.depth;
                u8* pixels = impl.data.data() + lvl.offset;

                for (u32 px = 0; px < pixel_count; ++px) {
                    u8* ch_ptr = pixels + px * bpp + ci * sizeof(T);
                    T val;
                    std::memcpy(&val, ch_ptr, sizeof(T));
                    if constexpr (std::is_same_v<T, u8>)
                        val = static_cast<u8>(255u - val);
                    else if constexpr (std::is_same_v<T, u16>)
                        val = static_cast<u16>(65535u - val);
                    else
                        val = 1.0f - val;
                    std::memcpy(ch_ptr, &val, sizeof(T));
                }
            }
        }
        ok = true;
    }
};

struct ExpandNormalOp {
    template <PixelFormat Fmt>
    static void apply(Texture::Impl& impl, u32 ai, u32 bi, u32 ci, bool& ok) {
        using T = typename FormatTraits<Fmt>::channel_type;
        constexpr u32 ch = FormatTraits<Fmt>::channels;
        constexpr u32 bpp = FormatTraits<Fmt>::bytes_per_pixel;

        if (ai >= ch || bi >= ch || ci >= ch) {
            ok = false;
            return;
        }

        // UNORM decode/encode constants folded at compile time.
        // For integer types the stored range is [0, numeric_limits::max];
        // for f32 it is the normalised [0, 1] interval, so max is 1.
        constexpr f32 kTypeMax =
            std::is_same_v<T, f32> ? 1.0f : static_cast<f32>(std::numeric_limits<T>::max());
        constexpr f32 kDecodeScale = 2.0f / kTypeMax; // maps [0, max] → [-1, 1]
        constexpr f32 kHalfMax = kTypeMax * 0.5f;     // maps [-1, 1] → [0, max]

        const u32 x_off = ai * sizeof(T);
        const u32 y_off = bi * sizeof(T);
        const u32 z_off = ci * sizeof(T);

        const u32 layers = impl.layerCount();
        const u32 mips = impl.mipCount();

        // Three float arrays at this chunk size fit comfortably in L1 cache.
        constexpr u32 kChunk = 1024;
        f32 xs[kChunk], ys[kChunk], zs[kChunk];

        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 mip = 0; mip < mips; ++mip) {
                const auto& lvl = impl.mips[layer * mips + mip];
                const u32 pixel_count = lvl.width * lvl.height * lvl.depth;
                u8* pixels = impl.data.data() + lvl.offset;

                for (u32 base = 0; base < pixel_count; base += kChunk) {
                    const u32 n = std::min(pixel_count - base, kChunk);
                    u8* const row = pixels + static_cast<size_t>(base) * bpp;

                    // Phase 1 – gather: deinterleave X and Y into contiguous
                    // float arrays with UNORM → [-1, 1] decoding applied.
                    for (u32 i = 0; i < n; ++i) {
                        T va, vb;
                        std::memcpy(&va, row + i * bpp + x_off, sizeof(T));
                        std::memcpy(&vb, row + i * bpp + y_off, sizeof(T));
                        xs[i] = static_cast<f32>(va) * kDecodeScale - 1.0f;
                        ys[i] = static_cast<f32>(vb) * kDecodeScale - 1.0f;
                    }

                    // Phase 2 – compute: reconstruct Z for every pixel.
                    // Tight loop over contiguous float arrays → auto-vectorizable
                    // (SIMD sqrt + FMA).
                    for (u32 i = 0; i < n; ++i)
                        zs[i] = std::sqrt(std::max(0.0f, 1.0f - xs[i] * xs[i] - ys[i] * ys[i]));

                    // Phase 3 – scatter: encode Z and write back to channel ci
                    // in the interleaved pixel buffer.
                    for (u32 i = 0; i < n; ++i) {
                        // (z + 1) * kHalfMax maps [-1, 1] → [0, max].
                        // Integer types require rounding to the nearest
                        // representable value; f32 stores the exact result.
                        const f32 encoded = (zs[i] + 1.0f) * kHalfMax;
                        T vc;
                        if constexpr (std::is_same_v<T, f32>)
                            vc = encoded;
                        else
                            vc = static_cast<T>(std::round(encoded));
                        std::memcpy(row + i * bpp + z_off, &vc, sizeof(T));
                    }
                }
            }
        }
        ok = true;
    }
};

struct FillChannelOp {
    template <PixelFormat Fmt>
    static void apply(Texture::Impl& impl, u32 ci, f32 value, bool& ok) {
        using T = typename FormatTraits<Fmt>::channel_type;
        constexpr u32 ch = FormatTraits<Fmt>::channels;
        constexpr u32 bpp = FormatTraits<Fmt>::bytes_per_pixel;

        if (ci >= ch) {
            ok = false;
            return;
        }

        const T encoded = convert_channel<T, f32>(value);

        const u32 layers = impl.layerCount();
        const u32 mips = impl.mipCount();

        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 mip = 0; mip < mips; ++mip) {
                const auto& lvl = impl.mips[layer * mips + mip];
                const u32 pixel_count = lvl.width * lvl.height * lvl.depth;
                u8* pixels = impl.data.data() + lvl.offset;

                for (u32 px = 0; px < pixel_count; ++px)
                    std::memcpy(pixels + px * bpp + ci * sizeof(T), &encoded, sizeof(T));
            }
        }
        ok = true;
    }
};

} // anonymous namespace

// ============================================================================
// format() and copyAsFormat()
// ============================================================================

/// Convert this texture in-place to @p new_fmt.
///
/// Conversion path:
///   1. Same format → no-op.
///   2. BCn source  → decode to native format (R8/RG8/RGBA8/RGBA32F),
///      then proceed.
///   3. Uncompressed → uncompressed → pixel-level conversion.
///   4. Uncompressed → BCn → bcn::encode (with an intermediate pixel-level
///      conversion if the decoded format doesn't match what the encoder needs).
void Texture::format(PixelFormat new_fmt) {
    *this = copyAsFormat(new_fmt);
}

Texture Texture::copyAsFormat(PixelFormat new_fmt, interfaces::WorkerPool* pool) const {
    // 1. Same format — just copy.
    if (impl_->format == new_fmt)
        return *this;

    // 2. If source is compressed, decode it first.
    if (bcn::isCompressed(impl_->format)) {
        std::string err;
        auto decoded = bcn::decode(*this, &err, pool);
        if (!decoded)
            throw std::runtime_error("Texture::copyAsFormat: decode failed: " + err);
        return decoded->copyAsFormat(new_fmt, pool);
    }

    // Source is now guaranteed to be uncompressed.

    // 3. Target is also uncompressed.
    if (!bcn::isCompressed(new_fmt))
        return convert_uncompressed(*this, new_fmt);

    // 4. Target is BCn — encode.
    //    bcn::encode requires R8 for BC4, RG8 for BC5, RGBA32F for BC6H,
    //    RGBA8 for all others.
    PixelFormat needed;
    if (new_fmt == PixelFormat::BC4)
        needed = PixelFormat::R8;
    else if (new_fmt == PixelFormat::BC5)
        needed = PixelFormat::RG8;
    else if (new_fmt == PixelFormat::BC6H)
        needed = PixelFormat::RGBA32F;
    else
        needed = PixelFormat::RGBA8;

    // Convert to the needed intermediate format if necessary.
    const Texture* src_ptr = this;
    Texture intermediate;
    if (impl_->format != needed) {
        intermediate = convert_uncompressed(*this, needed);
        src_ptr = &intermediate;
    }

    std::string err;
    auto encoded = bcn::encode(*src_ptr, new_fmt, &err, pool);
    if (!encoded)
        throw std::runtime_error("Texture::copyAsFormat: encode failed: " + err);
    return std::move(*encoded);
}

std::optional<Texture> Texture::copyFromNormalToRGBA(interfaces::WorkerPool* pool) const {
    if (kind() != TextureKind::Normal)
        return std::nullopt;

    if (bcn::isCompressed(impl_->format)) {
        std::string err;
        auto decoded = bcn::decode(*this, &err, pool);
        if (!decoded)
            return std::nullopt;

        decoded->setKind(kind());
        // BC5 decodes to RG8, BC4 to R8 — ensure RGBA8 for channel operations.
        if (decoded->format() != PixelFormat::RGBA8)
            *decoded = decoded->copyAsFormat(PixelFormat::RGBA8, pool);
        ensureColorSpace(*decoded, false);
        if (impl_->format == PixelFormat::BC3) {
            decoded->invertChannel(Channel::G);
            decoded->swapChannels(Channel::R, Channel::A);
        }
        decoded->expandNormal(Channel::R, Channel::G, Channel::B);
        decoded->fillChannel(Channel::A, 1.0f);
        return decoded;
    }

    auto dst = copyAsFormat(PixelFormat::RGBA8, pool);
    dst.setKind(TextureKind::Normal);
    ensureColorSpace(dst, false);
    dst.expandNormal(Channel::R, Channel::G, Channel::B);
    dst.fillChannel(Channel::A, 1.0f);
    return dst;
}

std::optional<std::vector<Texture>> Texture::splitChannels(
    const std::vector<Channel>& channels) const {
    const u32 srcChCount = format_channel_count(impl_->format);
    if (srcChCount == 0) // BCn
        return std::nullopt;

    for (auto ch : channels) {
        if (static_cast<u32>(ch) >= srcChCount)
            return std::nullopt;
    }

    const PixelFormat singleFmt = single_channel_format(impl_->format);
    const u32 bytesPerCh = format_bytes_per_channel(impl_->format);
    const u32 srcBpp = bytesPerBlock(impl_->format);
    const u32 layers = layerCount();
    const u32 mips = mipCount();

    std::vector<Texture> result;
    result.reserve(channels.size());

    for (auto ch : channels) {
        const u32 ci = static_cast<u32>(ch);
        Texture dst = make_texture_like(*this, singleFmt);
        dst.setSrgb(isSrgb());

        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 mip = 0; mip < mips; ++mip) {
                const auto srcSpan = mipData(mip, layer);
                auto dstSpan = dst.mipData(mip, layer);
                const auto& lvl = mipLevel(mip, layer);
                const u32 n = lvl.width * lvl.height * lvl.depth;

                const u8* s = srcSpan.data();
                u8* d = dstSpan.data();

                for (u32 px = 0; px < n; ++px)
                    std::memcpy(d + static_cast<size_t>(px) * bytesPerCh,
                                s + static_cast<size_t>(px) * srcBpp + ci * bytesPerCh, bytesPerCh);
            }
        }

        result.push_back(std::move(dst));
    }

    return result;
}

std::optional<Texture> Texture::mergeChannels(const std::vector<Texture>& sources,
                                              const std::vector<Channel>& targetChannels) {
    if (sources.empty() || sources.size() != targetChannels.size())
        return std::nullopt;

    const Texture& ref = sources[0];
    const u32 srcChCount = format_channel_count(ref.format());
    if (srcChCount != 1)
        return std::nullopt;

    const u32 bytesPerCh = format_bytes_per_channel(ref.format());
    if (bytesPerCh == 0)
        return std::nullopt;

    // Validate all sources match.
    for (size_t i = 1; i < sources.size(); ++i) {
        const Texture& s = sources[i];
        if (s.format() != ref.format() || s.type() != ref.type() || s.width() != ref.width() ||
            s.height() != ref.height() || s.depth() != ref.depth() ||
            s.mipCount() != ref.mipCount())
            return std::nullopt;
    }

    const PixelFormat dstFmt = rgba_format_for(ref.format());
    const u32 dstBpp = bytesPerBlock(dstFmt);
    Texture dst = make_texture_like(ref, dstFmt);

    const u32 layers = ref.layerCount();
    const u32 mips = ref.mipCount();

    for (size_t i = 0; i < sources.size(); ++i) {
        const u32 ci = static_cast<u32>(targetChannels[i]);
        if (ci >= 4)
            return std::nullopt;

        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 mip = 0; mip < mips; ++mip) {
                const auto srcSpan = sources[i].mipData(mip, layer);
                auto dstSpan = dst.mipData(mip, layer);
                const auto& lvl = ref.mipLevel(mip, layer);
                const u32 n = lvl.width * lvl.height * lvl.depth;

                const u8* s = srcSpan.data();
                u8* d = dstSpan.data();

                for (u32 px = 0; px < n; ++px)
                    std::memcpy(d + static_cast<size_t>(px) * dstBpp + ci * bytesPerCh,
                                s + static_cast<size_t>(px) * bytesPerCh, bytesPerCh);
            }
        }
    }

    return dst;
}

std::optional<std::string> Texture::generateMipmaps(interfaces::WorkerPool* pool) {
    return generateMipmaps(kKeepMipCount, pool);
}

std::optional<std::string> Texture::generateMipmaps(u32 newMipCount, interfaces::WorkerPool* pool) {
    // Validate and apply the requested mip count.
    const u32 maxMips = computeMaxMipCount(impl_->width, impl_->height, impl_->depth);

    if (newMipCount != kKeepMipCount) {
        if (newMipCount > maxMips)
            return std::string("newMipCount exceeds maximum (" + std::to_string(maxMips) + ")");

        if (newMipCount != mipCount()) {
            // Rebuild the mip chain with the new count, preserving mip 0 data.
            const u32 layers = impl_->layerCount();
            std::vector<MipLevel> newMips;
            const u64 total = build_mip_chain(impl_->format, impl_->width, impl_->height,
                                              impl_->depth, newMipCount, layers, newMips);

            std::vector<u8> newData(static_cast<size_t>(total), 0);

            // Copy mip 0 from each layer.
            for (u32 layer = 0; layer < layers; ++layer) {
                const auto& oldMip0 = impl_->mips[layer * mipCount()];
                const auto& newMip0 = newMips[layer * newMipCount];
                const size_t copySize = std::min<size_t>(oldMip0.size, newMip0.size);
                std::memcpy(newData.data() + newMip0.offset, impl_->data.data() + oldMip0.offset,
                            copySize);
            }

            impl_->mips = std::move(newMips);
            impl_->data = std::move(newData);
        }

        // Single mip level means no generation work needed.
        if (newMipCount == 1)
            return std::nullopt;
    }

    const bool isMultikind = (impl_->kind == TextureKind::Multikind);

    if (!isMultikind)
        return mipmap::generateMipmaps(*this, pool);

    // Multikind: split into per-channel textures, generate
    // mipmaps independently with kind-appropriate pipelines, then recombine.
    const u32 srcChCount = format_channel_count(impl_->format);
    if (srcChCount == 0)
        return std::string("Multikind mipmap generation requires an uncompressed format");

    // Build the per-channel kind array.
    std::array<TextureKind, 4> chKinds = impl_->channelKinds;

    // Determine which channels to process.
    const u32 splitCount = srcChCount;
    std::vector<Channel> channels;
    std::vector<u32> indices_map;
    channels.reserve(splitCount);
    for (u32 i = 0; i < splitCount; ++i) {
        if (chKinds[i] == TextureKind::Unused)
            continue;
        channels.push_back(static_cast<Channel>(i));
        indices_map.push_back(i);
    }

    auto split = splitChannels(channels);
    if (!split)
        return std::string("Multikind splitChannels failed");

    auto& channelTextures = *split;
    for (size_t i = 0; i < channelTextures.size(); ++i) {
        // Map Unused → Other so pipelineForKind uses a plain box filter.
        const TextureKind ck = chKinds[indices_map[i]];
        channelTextures[i].setKind(ck);
    }

    for (auto& chTex : channelTextures) {
        auto err = mipmap::generateMipmaps(chTex, pool);
        if (err)
            return err;
    }

    // Merge filtered channels back into this texture (mips 1+).
    auto merged = Texture::mergeChannels(channelTextures, channels);
    if (!merged)
        return std::string("Multikind mergeChannels failed");

    // Multikind: fill with default
    for (size_t i = 0; i < splitCount; ++i) {
        if (chKinds[i] == TextureKind::Unused) {
            Channel ch = static_cast<Channel>(i);
            merged->fillChannel(ch, channelDefault(ch));
        }
    }

    // mergeChannels produces rgba_format_for(singleFmt) which may differ from
    // the original format (e.g. RG8 splits to R8, merges back to RGBA8).
    // Convert back to the original format so data layout matches impl_->mips.
    if (merged->format() != impl_->format) {
        *merged = convert_uncompressed(*merged, impl_->format);
    }

    std::swap(impl_->data, merged->impl_->data);

    return std::nullopt;
}

std::optional<std::string> Texture::downscale(u32 levels, interfaces::WorkerPool* pool) {
    if (levels == 0)
        return std::nullopt;

    const u32 maxMips = computeMaxMipCount(impl_->width, impl_->height, impl_->depth);
    if (levels >= maxMips)
        return std::string("cannot downscale by " + std::to_string(levels) + " levels (max " +
                           std::to_string(maxMips - 1) + ")");

    const u32 currentMips = mipCount();
    const u32 targetMips = std::min(currentMips + levels, maxMips);

    // Regenerate mipmaps with extra levels.
    auto err = generateMipmaps(targetMips, pool);
    if (err)
        return err;

    // Drop the first `levels` mips: promote mip[levels] to the new base.
    const u32 layers = impl_->layerCount();
    const u32 oldMipCount = mipCount();
    const u32 newMipCount = oldMipCount - levels;

    const auto& newBase = impl_->mips[levels];
    const u32 newWidth = newBase.width;
    const u32 newHeight = newBase.height;
    const u32 newDepth = newBase.depth;

    std::vector<MipLevel> newMips;
    const u64 newTotal =
        build_mip_chain(impl_->format, newWidth, newHeight, newDepth, newMipCount, layers, newMips);

    std::vector<u8> newData(static_cast<size_t>(newTotal), 0);

    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < newMipCount; ++mip) {
            const auto& src = impl_->mips[layer * oldMipCount + mip + levels];
            const auto& dst = newMips[layer * newMipCount + mip];
            const size_t copySize = std::min<size_t>(src.size, dst.size);
            std::memcpy(newData.data() + dst.offset, impl_->data.data() + src.offset, copySize);
        }
    }

    impl_->width = newWidth;
    impl_->height = newHeight;
    impl_->depth = newDepth;
    impl_->mips = std::move(newMips);
    impl_->data = std::move(newData);

    return std::nullopt;
}

bool Texture::swapChannels(Channel a, Channel b) {
    if (bcn::isCompressed(impl_->format))
        return false;

    u32 ai = static_cast<u32>(a);
    u32 bi = static_cast<u32>(b);
    bool ok = false;
    dispatch_uncompressed<SwapChannelsOp>(impl_->format, *impl_, ai, bi, ok);
    return ok;
}

bool Texture::invertChannel(Channel ch) {
    if (bcn::isCompressed(impl_->format))
        return false;

    u32 ci = static_cast<u32>(ch);
    bool ok = false;
    dispatch_uncompressed<InvertChannelOp>(impl_->format, *impl_, ci, ok);
    return ok;
}

bool Texture::fillChannel(Channel target, f32 value) {
    if (bcn::isCompressed(impl_->format))
        return false;

    u32 ci = static_cast<u32>(target);
    bool ok = false;
    dispatch_uncompressed<FillChannelOp>(impl_->format, *impl_, ci, value, ok);
    return ok;
}

bool Texture::expandNormal(Channel xChannel, Channel yChannel, Channel zChannel) {
    if (bcn::isCompressed(impl_->format))
        return false;

    u32 ai = static_cast<u32>(xChannel);
    u32 bi = static_cast<u32>(yChannel);
    u32 ci = static_cast<u32>(zChannel);
    bool ok = false;
    dispatch_uncompressed<ExpandNormalOp>(impl_->format, *impl_, ai, bi, ci, ok);
    return ok;
}

} // namespace whiteout::textures

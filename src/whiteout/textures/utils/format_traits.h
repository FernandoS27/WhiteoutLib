// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file format_traits.h
/// @brief Compile-time pixel-format traits, channel conversion, and dispatch.
///
/// Internal header — not part of the public include path.
///
/// This is the single source of truth for uncompressed format metadata.
/// To add a new uncompressed format:
///   1. Add a FormatTraits<PixelFormat::NewFmt> specialization below
///   2. Append PixelFormat::NewFmt to the UncompressedFormats list
/// Everything else (tables, dispatch, bounds) auto-derives from the list.

#pragma once

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>

namespace whiteout::textures {

// ============================================================================
// Format list infrastructure
// ============================================================================

template <PixelFormat... Fmts>
struct FormatList {
    static constexpr u32 size = sizeof...(Fmts);
};

// ============================================================================
// FormatTraits — compile-time properties per uncompressed format
// ============================================================================

template <PixelFormat>
struct FormatTraits; // primary template intentionally undefined

#define WHITEOUT_FORMAT_TRAITS(FMT, TYPE, CH, BPP)                             \
    template <>                                                                \
    struct FormatTraits<PixelFormat::FMT> {                                     \
        using channel_type = TYPE;                                             \
        static constexpr u32 channels = CH;                                    \
        static constexpr u32 bytes_per_pixel = BPP;                            \
    }

WHITEOUT_FORMAT_TRAITS(R8,      u8,  1,  1);
WHITEOUT_FORMAT_TRAITS(R16,     u16, 1,  2);
WHITEOUT_FORMAT_TRAITS(R32F,    f32, 1,  4);
WHITEOUT_FORMAT_TRAITS(RG8,     u8,  2,  2);
WHITEOUT_FORMAT_TRAITS(RG16,    u16, 2,  4);
WHITEOUT_FORMAT_TRAITS(RG32F,   f32, 2,  8);
WHITEOUT_FORMAT_TRAITS(RGBA8,   u8,  4,  4);
WHITEOUT_FORMAT_TRAITS(RGBA16,  u16, 4,  8);
WHITEOUT_FORMAT_TRAITS(RGBA32F, f32, 4, 16);

#undef WHITEOUT_FORMAT_TRAITS

// ============================================================================
// Canonical format list — the single source of truth
// ============================================================================

using UncompressedFormats = FormatList<
    PixelFormat::R8,
    PixelFormat::R16,
    PixelFormat::R32F,
    PixelFormat::RG8,
    PixelFormat::RG16,
    PixelFormat::RG32F,
    PixelFormat::RGBA8,
    PixelFormat::RGBA16,
    PixelFormat::RGBA32F
>;

static constexpr u32 kUncompressedCount = UncompressedFormats::size;

// Verify that enum indices match list positions (guards against reordering).
namespace detail {

template <PixelFormat... Fmts, std::size_t... Is>
constexpr bool check_contiguity(FormatList<Fmts...>, std::index_sequence<Is...>) {
    return ((static_cast<u32>(Fmts) == static_cast<u32>(Is)) && ...);
}

static_assert(
    check_contiguity(UncompressedFormats{},
                     std::make_index_sequence<UncompressedFormats::size>{}),
    "PixelFormat enum values must be contiguous starting at 0 and match UncompressedFormats order");

} // namespace detail

// ============================================================================
// Channel conversion — exact integer math where possible
// ============================================================================

/// Default alpha value: max representable value for the channel type.
template <typename T>
constexpr T alpha_default() {
    if constexpr (std::is_same_v<T, f32>)
        return 1.0f;
    else if constexpr (std::is_same_v<T, u16>)
        return u16{65535};
    else
        return u8{255};
}

/// Convert a single channel value from SrcType to DstType.
/// Uses exact integer math for u8↔u16; float intermediate only when necessary.
template <typename Dst, typename Src>
constexpr Dst convert_channel(Src val) {
    if constexpr (std::is_same_v<Dst, Src>) {
        return val;
    }
    // u8 → u16: exact (v * 257 maps 0→0, 255→65535)
    else if constexpr (std::is_same_v<Src, u8> && std::is_same_v<Dst, u16>) {
        return static_cast<u16>(static_cast<u32>(val) * 257u);
    }
    // u16 → u8: exact inverse ((v + 128) / 257 maps 0→0, 65535→255)
    else if constexpr (std::is_same_v<Src, u16> && std::is_same_v<Dst, u8>) {
        return static_cast<u8>((static_cast<u32>(val) + 128u) / 257u);
    }
    // u8 → f32
    else if constexpr (std::is_same_v<Src, u8> && std::is_same_v<Dst, f32>) {
        return static_cast<f32>(val) / 255.0f;
    }
    // f32 → u8
    else if constexpr (std::is_same_v<Src, f32> && std::is_same_v<Dst, u8>) {
        return static_cast<u8>(std::clamp(val, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    // u16 → f32
    else if constexpr (std::is_same_v<Src, u16> && std::is_same_v<Dst, f32>) {
        return static_cast<f32>(val) / 65535.0f;
    }
    // f32 → u16
    else if constexpr (std::is_same_v<Src, f32> && std::is_same_v<Dst, u16>) {
        return static_cast<u16>(std::clamp(val, 0.0f, 1.0f) * 65535.0f + 0.5f);
    }
    else {
        static_assert(!std::is_same_v<Src, Src>, "unsupported channel type pair in convert_channel");
    }
}

// ============================================================================
// Per-channel I/O helpers (memcpy-safe, no strict-aliasing UB)
// ============================================================================

namespace detail {

template <typename T>
T read_channel(const u8* ptr) {
    T val;
    std::memcpy(&val, ptr, sizeof(T));
    return val;
}

template <typename T>
void write_channel(u8* ptr, T val) {
    std::memcpy(ptr, &val, sizeof(T));
}

} // namespace detail

// ============================================================================
// read_rgba32f / write_rgba32f — template per-pixel float converters
// ============================================================================

/// Read one pixel in format @p Fmt and write 4 floats (RGBA) to @p dst.
/// Missing channels default to 0 for RGB and 1 for alpha.
template <PixelFormat Fmt>
void read_rgba32f(const u8* src, f32* dst) {
    using Traits = FormatTraits<Fmt>;
    using T = typename Traits::channel_type;
    constexpr u32 ch = Traits::channels;

    // Read existing channels and convert to float
    if constexpr (ch >= 1) dst[0] = convert_channel<f32, T>(detail::read_channel<T>(src + 0 * sizeof(T)));
    if constexpr (ch >= 2) dst[1] = convert_channel<f32, T>(detail::read_channel<T>(src + 1 * sizeof(T)));
    if constexpr (ch >= 4) {
        dst[2] = convert_channel<f32, T>(detail::read_channel<T>(src + 2 * sizeof(T)));
        dst[3] = convert_channel<f32, T>(detail::read_channel<T>(src + 3 * sizeof(T)));
    }

    // Fill defaults for missing channels
    if constexpr (ch < 2) dst[1] = 0.0f;  // green
    if constexpr (ch < 4) dst[2] = 0.0f;  // blue
    if constexpr (ch < 4) dst[3] = 1.0f;  // alpha
}

/// Read 4 floats (RGBA) from @p src and write one pixel in format @p Fmt to @p dst.
template <PixelFormat Fmt>
void write_rgba32f(const f32* src, u8* dst) {
    using Traits = FormatTraits<Fmt>;
    using T = typename Traits::channel_type;
    constexpr u32 ch = Traits::channels;

    if constexpr (ch >= 1) detail::write_channel<T>(dst + 0 * sizeof(T), convert_channel<T, f32>(src[0]));
    if constexpr (ch >= 2) detail::write_channel<T>(dst + 1 * sizeof(T), convert_channel<T, f32>(src[1]));
    if constexpr (ch >= 4) {
        detail::write_channel<T>(dst + 2 * sizeof(T), convert_channel<T, f32>(src[2]));
        detail::write_channel<T>(dst + 3 * sizeof(T), convert_channel<T, f32>(src[3]));
    }
}

// ============================================================================
// convert_pixel — direct conversion, avoids float intermediate when possible
// ============================================================================

/// Convert one pixel from SrcFmt to DstFmt.
/// Uses direct channel conversion when possible (same channel type or integer pairs).
template <PixelFormat SrcFmt, PixelFormat DstFmt>
void convert_pixel(const u8* src, u8* dst) {
    if constexpr (SrcFmt == DstFmt) {
        std::memcpy(dst, src, FormatTraits<SrcFmt>::bytes_per_pixel);
        return;
    }

    using SrcTraits = FormatTraits<SrcFmt>;
    using DstTraits = FormatTraits<DstFmt>;
    using SrcT = typename SrcTraits::channel_type;
    using DstT = typename DstTraits::channel_type;
    constexpr u32 src_ch = SrcTraits::channels;
    constexpr u32 dst_ch = DstTraits::channels;

    // Direct path: convert channels directly without float intermediate
    constexpr u32 common_ch = (src_ch < dst_ch) ? src_ch : dst_ch;

    // Copy common channels with conversion
    if constexpr (common_ch >= 1) detail::write_channel<DstT>(dst + 0 * sizeof(DstT), convert_channel<DstT, SrcT>(detail::read_channel<SrcT>(src + 0 * sizeof(SrcT))));
    if constexpr (common_ch >= 2) detail::write_channel<DstT>(dst + 1 * sizeof(DstT), convert_channel<DstT, SrcT>(detail::read_channel<SrcT>(src + 1 * sizeof(SrcT))));
    if constexpr (common_ch >= 4) {
        detail::write_channel<DstT>(dst + 2 * sizeof(DstT), convert_channel<DstT, SrcT>(detail::read_channel<SrcT>(src + 2 * sizeof(SrcT))));
        detail::write_channel<DstT>(dst + 3 * sizeof(DstT), convert_channel<DstT, SrcT>(detail::read_channel<SrcT>(src + 3 * sizeof(SrcT))));
    }

    // Fill missing destination channels with defaults
    if constexpr (dst_ch >= 2 && src_ch < 2) {
        detail::write_channel<DstT>(dst + 1 * sizeof(DstT), DstT{0}); // green
    }
    if constexpr (dst_ch >= 4 && src_ch < 4) {
        if constexpr (src_ch < 3) {
            detail::write_channel<DstT>(dst + 2 * sizeof(DstT), DstT{0}); // blue
        }
        detail::write_channel<DstT>(dst + 3 * sizeof(DstT), alpha_default<DstT>()); // alpha
    }
}

/// Bulk-convert @p count pixels from SrcFmt to DstFmt.
template <PixelFormat SrcFmt, PixelFormat DstFmt>
void convert_pixels(const u8* src, u8* dst, u32 count) {
    constexpr u32 src_bpp = FormatTraits<SrcFmt>::bytes_per_pixel;
    constexpr u32 dst_bpp = FormatTraits<DstFmt>::bytes_per_pixel;

    for (u32 i = 0; i < count; ++i) {
        convert_pixel<SrcFmt, DstFmt>(src + i * src_bpp, dst + i * dst_bpp);
    }
}

// ============================================================================
// dispatch_uncompressed — runtime PixelFormat → compile-time template call
// ============================================================================

namespace detail {

template <typename Fn, PixelFormat... Fmts, std::size_t... Is, typename... Args>
bool dispatch_impl(FormatList<Fmts...>, std::index_sequence<Is...>,
                   PixelFormat fmt, Args&... args) {
    bool matched = false;
    // Fold expression: try each format (args passed by lref — safe across fold iterations)
    ((static_cast<u32>(Fmts) == static_cast<u32>(fmt)
          ? (Fn::template apply<Fmts>(args...), matched = true, 0)
          : 0),
     ...);
    return matched;
}

} // namespace detail

/// Dispatch a runtime PixelFormat to a compile-time template call.
/// Fn must have: template <PixelFormat Fmt> static void apply(Args&...);
/// Returns true if the format was matched.
template <typename Fn, typename... Args>
bool dispatch_uncompressed(PixelFormat fmt, Args&... args) {
    return detail::dispatch_impl<Fn>(
        UncompressedFormats{},
        std::make_index_sequence<UncompressedFormats::size>{},
        fmt, args...);
}

// ============================================================================
// for_each_format — iterate over format list at compile time
// ============================================================================

namespace detail {

template <typename Fn, PixelFormat... Fmts>
void for_each_impl(FormatList<Fmts...>, Fn&& fn) {
    (fn.template operator()<Fmts>(), ...);
}

} // namespace detail

/// Call fn.template operator()<Fmt>() for each format in List.
template <typename List, typename Fn>
void for_each_format(Fn&& fn) {
    detail::for_each_impl(List{}, std::forward<Fn>(fn));
}

} // namespace whiteout::textures

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Tests for legacy (non-DX10) DDS pixel formats, whose channel layout comes
// from bit masks rather than a format enum.

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include <whiteout/textures/dds/dds.h>
#include <whiteout/textures/texture.h>

using namespace whiteout;
using namespace whiteout::textures;

namespace {

constexpr u32 kDdpfAlphaPixels = 0x00000001u;
constexpr u32 kDdpfRgb = 0x00000040u;

// Build a single-mip 1x2 uncompressed DDS with caller-supplied channel masks,
// so a test can ask for either byte order without a fixture file.
std::vector<u8> make_legacy_dds(u32 rMask, u32 gMask, u32 bMask, u32 aMask,
                                std::span<const u8> texels) {
    std::vector<u8> out(128, 0);
    std::memcpy(out.data(), "DDS ", 4);
    auto put = [&out](std::size_t byteOffset, u32 value) {
        std::memcpy(out.data() + byteOffset, &value, sizeof(value));
    };
    // DDS_HEADER starts at 4. Fields below are at their documented offsets.
    put(4, 124);                                // size
    put(8, 0x1u | 0x2u | 0x4u | 0x1000u);       // CAPS|HEIGHT|WIDTH|PIXELFORMAT
    put(12, 2);                                 // height
    put(16, 1);                                 // width
    put(76, 32);                                // ddspf.size
    put(80, kDdpfRgb | kDdpfAlphaPixels);       // ddspf.flags
    put(84, 0);                                 // ddspf.fourCC
    put(88, 32);                                // ddspf.rgbBitCount
    put(92, rMask);
    put(96, gMask);
    put(100, bMask);
    put(104, aMask);
    out.insert(out.end(), texels.begin(), texels.end());
    return out;
}

} // namespace

// Two pixels with every channel distinct, so a mis-ordered read cannot pass by
// coincidence the way a grey or symmetric pixel would.
static constexpr u8 kRgbaTexels[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
static constexpr u8 kBgraTexels[] = {0x30, 0x20, 0x10, 0x40, 0x70, 0x60, 0x50, 0x80};

TEST_CASE("legacy DDS: R8G8B8A8 masks decode as-is", "[texture][dds]") {
    const auto file = make_legacy_dds(0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u,
                                      kRgbaTexels);
    dds::Parser parser;
    auto tex = parser.parse(std::span<const u8>{file});
    REQUIRE(tex.has_value());
    REQUIRE(tex->format() == PixelFormat::RGBA8);
    const auto mip0 = tex->mipData(0);
    REQUIRE(mip0.size() == sizeof(kRgbaTexels));
    CHECK(std::memcmp(mip0.data(), kRgbaTexels, sizeof(kRgbaTexels)) == 0);
}

// D3DFMT_A8R8G8B8 — what D3D9-era bakers (PopcornFX among them) emit.
TEST_CASE("legacy DDS: A8R8G8B8 masks decode to RGBA8", "[texture][dds]") {
    const auto file = make_legacy_dds(0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u,
                                      kBgraTexels);
    dds::Parser parser;
    auto tex = parser.parse(std::span<const u8>{file});
    REQUIRE(tex.has_value());
    REQUIRE(tex->format() == PixelFormat::RGBA8);
    const auto mip0 = tex->mipData(0);
    REQUIRE(mip0.size() == sizeof(kBgraTexels));
    // Same image as the RGBA case above, so it must decode to the same texels.
    CHECK(std::memcmp(mip0.data(), kRgbaTexels, sizeof(kRgbaTexels)) == 0);
}

TEST_CASE("legacy DDS: unknown masks are rejected", "[texture][dds]") {
    // 16-bit R5G6B5 — a real legacy format the parser has no target for.
    auto file = make_legacy_dds(0xF800u, 0x07E0u, 0x001Fu, 0u, kRgbaTexels);
    u32 bits = 16;
    std::memcpy(file.data() + 88, &bits, sizeof(bits));
    dds::Parser parser;
    auto tex = parser.parse(std::span<const u8>{file});
    CHECK_FALSE(tex.has_value());
    CHECK(parser.hasIssues());
}

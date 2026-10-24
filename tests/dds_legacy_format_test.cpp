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
#include <whiteout/textures/dds/writer.h>
#include <whiteout/textures/texture.h>

using namespace whiteout;
using namespace whiteout::textures;

namespace {

constexpr u32 kDdpfAlphaPixels = 0x00000001u;
constexpr u32 kDdpfRgb = 0x00000040u;
constexpr u32 kDdpfLuminance = 0x00020000u;

// Build a single-mip 1x2 uncompressed DDS with caller-supplied channel masks,
// so a test can ask for any layout without a fixture file.
std::vector<u8> make_legacy_dds(u32 rMask, u32 gMask, u32 bMask, u32 aMask,
                                std::span<const u8> texels, u32 bits = 32,
                                u32 flags = kDdpfRgb | kDdpfAlphaPixels) {
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
    put(80, flags);                             // ddspf.flags
    put(84, 0);                                 // ddspf.fourCC
    put(88, bits);                              // ddspf.rgbBitCount
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

// D3DFMT_R8G8B8 — 24-bit, no alpha, what older SC2 mod tools write.
TEST_CASE("legacy DDS: R8G8B8 masks expand to opaque RGBA8", "[texture][dds]") {
    static constexpr u8 kBgr[] = {0x30, 0x20, 0x10, 0x70, 0x60, 0x50};
    const auto file =
        make_legacy_dds(0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0u, kBgr, 24, kDdpfRgb);
    dds::Parser parser;
    auto tex = parser.parse(std::span<const u8>{file});
    REQUIRE(tex.has_value());
    REQUIRE(tex->format() == PixelFormat::RGBA8);
    static constexpr u8 kExpected[] = {0x10, 0x20, 0x30, 0xFF, 0x50, 0x60, 0x70, 0xFF};
    const auto mip0 = tex->mipData(0);
    REQUIRE(mip0.size() == sizeof(kExpected));
    CHECK(std::memcmp(mip0.data(), kExpected, sizeof(kExpected)) == 0);
}

TEST_CASE("legacy DDS: narrow channels widen to 8 bits", "[texture][dds]") {
    dds::Parser parser;
    SECTION("R5G6B5") {
        static constexpr u8 kTexels[] = {0x00, 0xF8, 0x1F, 0x00}; // pure red, pure blue
        const auto file =
            make_legacy_dds(0xF800u, 0x07E0u, 0x001Fu, 0u, kTexels, 16, kDdpfRgb);
        auto tex = parser.parse(std::span<const u8>{file});
        REQUIRE(tex.has_value());
        static constexpr u8 kExpected[] = {0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0xFF};
        CHECK(std::memcmp(tex->mipData(0).data(), kExpected, sizeof(kExpected)) == 0);
    }
    SECTION("A8L8") {
        static constexpr u8 kTexels[] = {0x40, 0x80, 0xC0, 0x10};
        const auto file = make_legacy_dds(0x00FFu, 0u, 0u, 0xFF00u, kTexels, 16,
                                          kDdpfLuminance | kDdpfAlphaPixels);
        auto tex = parser.parse(std::span<const u8>{file});
        REQUIRE(tex.has_value());
        static constexpr u8 kExpected[] = {0x40, 0x40, 0x40, 0x80, 0xC0, 0xC0, 0xC0, 0x10};
        CHECK(std::memcmp(tex->mipData(0).data(), kExpected, sizeof(kExpected)) == 0);
    }
}

TEST_CASE("legacy DDS: a layout with no colour masks is rejected", "[texture][dds]") {
    const auto file = make_legacy_dds(0u, 0u, 0u, 0u, kRgbaTexels, 32, kDdpfRgb);
    dds::Parser parser;
    auto tex = parser.parse(std::span<const u8>{file});
    CHECK_FALSE(tex.has_value());
    CHECK(parser.hasIssues());
}

TEST_CASE("DDS writer: depth is a volume's field", "[texture][dds]") {
    // Every shipped StarCraft II .dds writes 0 here for a 2D texture; the
    // writer put the texture's depth of 1 there, under no DDSD_DEPTH flag.
    constexpr u32 kDdsdDepth = 0x00800000u;
    const auto depthOf = [](const std::vector<u8>& file, u32& flags) {
        u32 depth = 0;
        std::memcpy(&flags, file.data() + 8, sizeof(flags));
        std::memcpy(&depth, file.data() + 24, sizeof(depth));
        return depth;
    };
    u32 flags = 0;
    const std::vector<u8> flat = dds::Writer().write(Texture::create2D(PixelFormat::RGBA8, 2, 2));
    REQUIRE(flat.size() > 128);
    CHECK(depthOf(flat, flags) == 0u);
    CHECK((flags & kDdsdDepth) == 0u);

    const std::vector<u8> volume =
        dds::Writer().write(Texture::create3D(PixelFormat::RGBA8, 2, 2, 3, 1));
    REQUIRE(volume.size() > 128);
    CHECK(depthOf(volume, flags) == 3u);
    CHECK((flags & kDdsdDepth) == kDdsdDepth);
}

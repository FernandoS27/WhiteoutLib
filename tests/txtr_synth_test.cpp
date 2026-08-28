// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Synthetic Overwatch TXTR files, built byte by byte, so the `004` / `04D`
// texture path is exercised without an Overwatch install.  The header layout,
// the payload prefix, the surface-format table and the payload-GUID arithmetic
// all come from the client itself -- see docs/OW_TXTR_FORMAT.md.

#include <catch2/catch_all.hpp>

#include <whiteout/textures/texture.h>
#include <whiteout/textures/txtr/txtr.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <span>
#include <vector>

using namespace whiteout;
using namespace whiteout::textures;

namespace {

constexpr u32 kHeaderSize = 0x20;

// Surface format codes, in the client's numbering.
constexpr u8 kFmtBc1Typeless = 71;
constexpr u8 kFmtBc1Unorm = 72;
constexpr u8 kFmtBc1Srgb = 73;
constexpr u8 kFmtBc3Unorm = 78;
constexpr u8 kFmtBc3Srgb = 79;
constexpr u8 kFmtBc7Unorm = 99;
constexpr u8 kFmtBgra8Unorm = 88; // DXGI B8G8R8A8_UNORM
constexpr u8 kFmtBgrx8Unorm = 89; // DXGI B8G8R8X8_UNORM
constexpr u8 kFmtRgba8Unorm = 29; // DXGI R8G8B8A8_UNORM
constexpr u8 kFmtRgba8Srgb = 30;  // DXGI R8G8B8A8_UNORM_SRGB
constexpr u8 kFmtRgba8Uint = 31;  // DXGI R8G8B8A8_UINT
constexpr u8 kFmtRgba16f = 10;    // DXGI R16G16B16A16_FLOAT
constexpr u8 kFmtRg16f = 35;      // DXGI R16G16_FLOAT
constexpr u8 kFmtR16f = 55;       // DXGI R16_FLOAT
constexpr u8 kFmtRg8Uint = 51;    // DXGI R8G8_UINT
constexpr u8 kFmtBc6hUf16 = 96;   // DXGI BC6H_UF16
constexpr u8 kFmtBc7Srgb = 100;   // DXGI BC7_UNORM_SRGB

void put16(std::vector<u8>& b, size_t off, u16 v) {
    b[off] = static_cast<u8>(v & 0xFF);
    b[off + 1] = static_cast<u8>((v >> 8) & 0xFF);
}

void put32(std::vector<u8>& b, size_t off, u32 v) {
    for (int i = 0; i < 4; ++i)
        b[off + static_cast<size_t>(i)] = static_cast<u8>((v >> (8 * i)) & 0xFF);
}

/// Bytes one mip level of a BCn surface occupies, the way the client counts:
/// dimensions clamp up to a whole block before the block count is taken.
u64 blockMipBytes(u32 blockBytes, u32 width, u32 height, u32 mip, u32 depth = 1) {
    const u32 w = std::max(width >> mip, 4u);
    const u32 h = std::max(height >> mip, 4u);
    const u32 d = std::max(depth >> mip, 1u);
    return static_cast<u64>(w / 4) * (h / 4) * blockBytes * d;
}

u64 pixelMipBytes(u32 bytesPerPixel, u32 width, u32 height, u32 mip) {
    const u32 w = std::max(width >> mip, 1u);
    const u32 h = std::max(height >> mip, 1u);
    return static_cast<u64>(w) * h * bytesPerPixel;
}

/// Deterministic filler so a mip's bytes identify where they came from.
std::vector<u8> pattern(u64 size, u8 seed) {
    std::vector<u8> out(static_cast<size_t>(size));
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<u8>(seed + (i * 7));
    return out;
}

/// Builds a `004` header. Pixel bytes are appended at 0x20 exactly as given.
struct HeaderBuilder {
    u16 flags = 0x0002; // 2D
    u8 mipCount = 1;
    u8 format = kFmtBc1Unorm;
    u8 surfaces = 1;
    u8 unknown05 = 0;
    u8 payloadCount = 0;
    u8 unknown07 = 0;
    u16 width = 64;
    u16 height = 64;
    u8 headerMipCount = 1;
    u8 streamMask = 0;
    u8 unknown12 = 0;
    std::vector<u8> inlineBlock;

    std::vector<u8> build() const {
        std::vector<u8> b(kHeaderSize + inlineBlock.size(), 0);
        put16(b, 0x00, flags);
        b[0x02] = mipCount;
        b[0x03] = format;
        b[0x04] = surfaces;
        b[0x05] = unknown05;
        b[0x06] = payloadCount;
        b[0x07] = unknown07;
        put16(b, 0x08, width);
        put16(b, 0x0A, height);
        put32(b, 0x0C, static_cast<u32>(inlineBlock.size()));
        b[0x10] = headerMipCount;
        b[0x11] = streamMask;
        b[0x12] = unknown12;
        if (!inlineBlock.empty())
            std::memcpy(b.data() + kHeaderSize, inlineBlock.data(), inlineBlock.size());
        return b;
    }
};

/// Wraps pixel bytes in the 16-byte prefix every payload carries.
std::vector<u8> makePayload(u32 texMipCount, u32 firstMip, u32 mipCount,
                            const std::vector<u8>& pixels) {
    std::vector<u8> b(16 + pixels.size(), 0);
    put32(b, 0, texMipCount - firstMip - 1); // mipsFromSmallest
    put32(b, 4, mipCount);
    put32(b, 8, static_cast<u32>(pixels.size()));
    put32(b, 12, 16); // dataOffset
    if (!pixels.empty())
        std::memcpy(b.data() + 16, pixels.data(), pixels.size());
    return b;
}

/// Concatenate the per-mip patterns for a contiguous run of a BC1 surface.
std::vector<u8> bc1Run(u32 width, u32 height, u32 firstMip, u32 mipCount, u8 seed) {
    std::vector<u8> out;
    for (u32 mip = firstMip; mip < firstMip + mipCount; ++mip) {
        auto part = pattern(blockMipBytes(8, width, height, mip), static_cast<u8>(seed + mip));
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

} // namespace

TEST_CASE("TXTR self-contained header decodes every mip", "[txtr]") {
    constexpr u32 kWidth = 64, kHeight = 64, kMips = 7;

    HeaderBuilder hb;
    hb.mipCount = kMips;
    hb.headerMipCount = kMips;
    hb.width = kWidth;
    hb.height = kHeight;
    hb.payloadCount = 0;
    hb.inlineBlock = bc1Run(kWidth, kHeight, 0, kMips, 0x10);

    const auto file = hb.build();

    txtr::Parser parser;
    txtr::TxtrInfo info;
    auto texture = parser.parse(std::span<const u8>{file}, &info);

    REQUIRE(texture.has_value());
    CAPTURE(parser.getIssues());
    REQUIRE_FALSE(parser.hasIssues());

    CHECK(info.dimension == txtr::TxtrDimension::Texture2D);
    CHECK(info.pixelFormat == PixelFormat::BC1);
    CHECK_FALSE(info.isSrgb);
    CHECK(info.dxgiFormat == 71); // DXGI_FORMAT_BC1_UNORM
    CHECK(info.baseMip == 0);
    CHECK(info.missingPayloads == 0);

    CHECK(texture->width() == kWidth);
    CHECK(texture->height() == kHeight);
    CHECK(texture->mipCount() == kMips);
    CHECK(texture->type() == TextureType::Texture2D);

    for (u32 mip = 0; mip < kMips; ++mip) {
        const auto expected =
            pattern(blockMipBytes(8, kWidth, kHeight, mip), static_cast<u8>(0x10 + mip));
        auto actual = texture->mipData(mip);
        REQUIRE(actual.size() == expected.size());
        CHECK(std::equal(expected.begin(), expected.end(), actual.begin()));
    }
}

TEST_CASE("TXTR reassembles a split mip chain from its payloads", "[txtr]") {
    constexpr u32 kWidth = 64, kHeight = 64, kMips = 7;

    // Payload 0 lives inline and holds mips 4..6; payload 1 holds 2..3;
    // payload 2 holds 0..1. Detail rises with the payload index.
    HeaderBuilder hb;
    hb.mipCount = kMips;
    hb.headerMipCount = 3;
    hb.width = kWidth;
    hb.height = kHeight;
    hb.payloadCount = 3;
    hb.inlineBlock = makePayload(kMips, 4, 3, bc1Run(kWidth, kHeight, 4, 3, 0x10));

    const auto header = hb.build();
    const auto payload1 = makePayload(kMips, 2, 2, bc1Run(kWidth, kHeight, 2, 2, 0x10));
    const auto payload2 = makePayload(kMips, 0, 2, bc1Run(kWidth, kHeight, 0, 2, 0x10));

    SECTION("all payloads present") {
        // Order must not matter: each payload declares the range it covers.
        std::vector<std::span<const u8>> payloads{std::span<const u8>{payload2},
                                                  std::span<const u8>{payload1}};

        txtr::Parser parser;
        txtr::TxtrInfo info;
        auto texture = parser.parse(std::span<const u8>{header}, payloads, &info);

        REQUIRE(texture.has_value());
        CAPTURE(parser.getIssues());
        REQUIRE_FALSE(parser.hasIssues());

        CHECK(info.baseMip == 0);
        CHECK(info.missingPayloads == 0);
        CHECK(info.payloadCount == 3);
        CHECK(info.headerMipCount == 3);
        CHECK(texture->width() == kWidth);
        CHECK(texture->mipCount() == kMips);

        for (u32 mip = 0; mip < kMips; ++mip) {
            const auto expected =
                pattern(blockMipBytes(8, kWidth, kHeight, mip), static_cast<u8>(0x10 + mip));
            auto actual = texture->mipData(mip);
            REQUIRE(actual.size() == expected.size());
            CHECK(std::equal(expected.begin(), expected.end(), actual.begin()));
        }
    }

    SECTION("header alone yields the resident tail") {
        txtr::Parser parser;
        txtr::TxtrInfo info;
        auto texture = parser.parse(std::span<const u8>{header}, &info);

        REQUIRE(texture.has_value());
        CHECK(info.baseMip == 4);
        CHECK(info.missingPayloads == 2);
        CHECK(parser.hasIssues()); // The missing payloads are reported, not fatal.

        CHECK(texture->width() == kWidth >> 4);
        CHECK(texture->height() == kHeight >> 4);
        CHECK(texture->mipCount() == 3);

        for (u32 mip = 4; mip < kMips; ++mip) {
            const auto expected =
                pattern(blockMipBytes(8, kWidth, kHeight, mip), static_cast<u8>(0x10 + mip));
            auto actual = texture->mipData(mip - 4);
            REQUIRE(actual.size() == expected.size());
            CHECK(std::equal(expected.begin(), expected.end(), actual.begin()));
        }
    }

    SECTION("a gap in the chain is rejected") {
        // Supplying only the top payload leaves mips 2..3 uncovered.
        std::vector<std::span<const u8>> payloads{std::span<const u8>{payload2}};

        txtr::Parser parser;
        auto texture = parser.parse(std::span<const u8>{header}, payloads, nullptr);

        CHECK_FALSE(texture.has_value());
        CHECK(parser.hasIssues());
    }
}

TEST_CASE("TXTR cube maps are laid out face-major", "[txtr]") {
    constexpr u32 kSize = 16, kMips = 3;

    std::vector<u8> pixels;
    for (u32 face = 0; face < 6; ++face) {
        auto run = bc1Run(kSize, kSize, 0, kMips, static_cast<u8>(0x40 + face * 16));
        pixels.insert(pixels.end(), run.begin(), run.end());
    }

    HeaderBuilder hb;
    hb.flags = 0x0008; // Cube
    hb.mipCount = kMips;
    hb.headerMipCount = kMips;
    hb.width = kSize;
    hb.height = kSize;
    hb.payloadCount = 0;
    hb.inlineBlock = pixels;

    const auto file = hb.build();

    txtr::Parser parser;
    txtr::TxtrInfo info;
    auto texture = parser.parse(std::span<const u8>{file}, &info);

    REQUIRE(texture.has_value());
    CAPTURE(parser.getIssues());
    REQUIRE_FALSE(parser.hasIssues());

    CHECK(info.dimension == txtr::TxtrDimension::TextureCube);
    CHECK(info.faceCount == 6);
    CHECK(texture->type() == TextureType::TextureCube);
    CHECK(texture->layerCount() == 6);

    for (u32 face = 0; face < 6; ++face) {
        for (u32 mip = 0; mip < kMips; ++mip) {
            const auto expected = pattern(blockMipBytes(8, kSize, kSize, mip),
                                          static_cast<u8>(0x40 + face * 16 + mip));
            auto actual = texture->mipData(mip, face);
            REQUIRE(actual.size() == expected.size());
            CHECK(std::equal(expected.begin(), expected.end(), actual.begin()));
        }
    }
}

TEST_CASE("TXTR 2D arrays are laid out slice-major", "[txtr]") {
    constexpr u32 kWidth = 32, kHeight = 32, kMips = 2, kSlices = 3;

    std::vector<u8> pixels;
    for (u32 slice = 0; slice < kSlices; ++slice) {
        auto run = bc1Run(kWidth, kHeight, 0, kMips, static_cast<u8>(0x80 + slice * 16));
        pixels.insert(pixels.end(), run.begin(), run.end());
    }

    HeaderBuilder hb;
    hb.flags = 0x0002 | 0x0040; // 2D | Array
    hb.mipCount = kMips;
    hb.headerMipCount = kMips;
    hb.width = kWidth;
    hb.height = kHeight;
    hb.surfaces = kSlices;
    hb.payloadCount = 0;
    hb.inlineBlock = pixels;

    const auto file = hb.build();

    txtr::Parser parser;
    txtr::TxtrInfo info;
    auto texture = parser.parse(std::span<const u8>{file}, &info);

    REQUIRE(texture.has_value());
    CAPTURE(parser.getIssues());
    REQUIRE_FALSE(parser.hasIssues());

    CHECK(info.dimension == txtr::TxtrDimension::Texture2DArray);
    CHECK(info.arraySize == kSlices);
    CHECK(texture->type() == TextureType::Texture2DArray);
    CHECK(texture->arraySize() == kSlices);

    for (u32 slice = 0; slice < kSlices; ++slice) {
        for (u32 mip = 0; mip < kMips; ++mip) {
            const auto expected = pattern(blockMipBytes(8, kWidth, kHeight, mip),
                                          static_cast<u8>(0x80 + slice * 16 + mip));
            auto actual = texture->mipData(mip, slice);
            REQUIRE(actual.size() == expected.size());
            CHECK(std::equal(expected.begin(), expected.end(), actual.begin()));
        }
    }
}

TEST_CASE("TXTR surface format codes map onto library formats", "[txtr]") {
    struct Case {
        u8 code;
        PixelFormat format;
        bool srgb;
        u32 blockBytes; // 0 marks an uncompressed format
        u32 bytesPerPixel;
    };

    const Case cases[] = {
        {kFmtBc1Typeless, PixelFormat::BC1, false, 8, 0},
        {kFmtBc1Unorm, PixelFormat::BC1, false, 8, 0},
        {kFmtBc1Srgb, PixelFormat::BC1, true, 8, 0},
        {kFmtBc3Unorm, PixelFormat::BC3, false, 16, 0},
        {kFmtBc3Srgb, PixelFormat::BC3, true, 16, 0},
        {kFmtBc7Unorm, PixelFormat::BC7, false, 16, 0},
        {kFmtRgba8Unorm, PixelFormat::RGBA8, false, 0, 4},
        {kFmtRgba8Srgb, PixelFormat::RGBA8, true, 0, 4},
        {kFmtBgra8Unorm, PixelFormat::RGBA8, false, 0, 4},
        {kFmtRgba8Uint, PixelFormat::RGBA8, false, 0, 4},
        {kFmtBc6hUf16, PixelFormat::BC6H, false, 16, 0},
        {kFmtBc7Srgb, PixelFormat::BC7, true, 16, 0},
        {kFmtRg8Uint, PixelFormat::RG8, false, 0, 2},
        {kFmtR16f, PixelFormat::R16F, false, 0, 2},
        {kFmtRg16f, PixelFormat::RG16F, false, 0, 4},
        {kFmtRgba16f, PixelFormat::RGBA16F, false, 0, 8},
    };

    for (const auto& c : cases) {
        CAPTURE(c.code);

        HeaderBuilder hb;
        hb.format = c.code;
        hb.mipCount = 1;
        hb.headerMipCount = 1;
        hb.width = 8;
        hb.height = 8;
        hb.inlineBlock = pattern(c.blockBytes ? blockMipBytes(c.blockBytes, 8, 8, 0)
                                              : pixelMipBytes(c.bytesPerPixel, 8, 8, 0),
                                 0x33);

        const auto file = hb.build();

        txtr::Parser parser;
        txtr::TxtrInfo info;
        auto texture = parser.parse(std::span<const u8>{file}, &info);

        REQUIRE(texture.has_value());
        CAPTURE(parser.getIssues());
        REQUIRE_FALSE(parser.hasIssues());
        CHECK(info.pixelFormat == c.format);
        CHECK(info.isSrgb == c.srgb);
        CHECK(texture->format() == c.format);
        CHECK(texture->isSrgb() == c.srgb);
    }
}

// The client's own g_teFormat_ToDXGI table runs 0-24 as DXGI, has one
// client-only format at 25, then runs one ahead of DXGI to 100. Getting the
// step wrong shifts the whole R10G10B10A2 group, so the boundary is pinned
// here rather than left to the shipped codes, none of which sit on it.
TEST_CASE("TXTR surface codes resolve to the client's DXGI values", "[txtr]") {
    struct Case {
        u8 code;
        u32 dxgi;
    };

    const Case cases[] = {
        {0, 0},   {10, 10}, {22, 22}, {23, 23},  {24, 24}, {25, 0},
        {26, 25}, {29, 28}, {72, 71}, {100, 99}, {101, 0}, {113, 103},
    };

    for (const auto& c : cases) {
        CAPTURE(c.code);
        CHECK(txtr::dxgiFormatFor(c.code) == c.dxgi);
    }
}

TEST_CASE("TXTR swizzles BGRA surfaces into RGBA", "[txtr]") {
    const auto decode = [](u8 formatCode) {
        HeaderBuilder hb;
        hb.format = formatCode;
        hb.mipCount = 1;
        hb.headerMipCount = 1;
        hb.width = 2;
        hb.height = 1;
        hb.inlineBlock = {0x11, 0x22, 0x33, 0x77, 0x44, 0x55, 0x66, 0x88};
        return hb.build();
    };

    SECTION("B8G8R8A8 keeps its alpha") {
        const auto file = decode(kFmtBgra8Unorm);
        txtr::Parser parser;
        auto texture = parser.parse(std::span<const u8>{file});

        REQUIRE(texture.has_value());
        CAPTURE(parser.getIssues());
        REQUIRE_FALSE(parser.hasIssues());
        REQUIRE(texture->format() == PixelFormat::RGBA8);

        auto data = texture->mipData(0);
        REQUIRE(data.size() == 8);
        CHECK(data[0] == 0x33);
        CHECK(data[1] == 0x22);
        CHECK(data[2] == 0x11);
        CHECK(data[3] == 0x77);
        CHECK(data[4] == 0x66);
        CHECK(data[5] == 0x55);
        CHECK(data[6] == 0x44);
        CHECK(data[7] == 0x88);
    }

    SECTION("B8G8R8X8 has no alpha, so it is forced opaque") {
        const auto file = decode(kFmtBgrx8Unorm);
        txtr::Parser parser;
        auto texture = parser.parse(std::span<const u8>{file});

        REQUIRE(texture.has_value());
        CAPTURE(parser.getIssues());
        REQUIRE_FALSE(parser.hasIssues());

        auto data = texture->mipData(0);
        REQUIRE(data.size() == 8);
        CHECK(data[0] == 0x33);
        CHECK(data[3] == 0xFF);
        CHECK(data[4] == 0x66);
        CHECK(data[7] == 0xFF);
    }
}

TEST_CASE("TXTR rejects headers the client would reject", "[txtr]") {
    const auto valid = [] {
        HeaderBuilder hb;
        hb.mipCount = 1;
        hb.headerMipCount = 1;
        hb.width = 8;
        hb.height = 8;
        hb.inlineBlock = pattern(blockMipBytes(8, 8, 8, 0), 0x55);
        return hb;
    };

    SECTION("format code out of range") {
        auto hb = valid();
        hb.format = 114;
        const auto file = hb.build();
        txtr::Parser parser;
        CHECK_FALSE(parser.parse(std::span<const u8>{file}).has_value());
        CHECK(parser.hasIssues());
    }

    SECTION("mip count over the cap") {
        auto hb = valid();
        hb.mipCount = 17;
        const auto file = hb.build();
        txtr::Parser parser;
        CHECK_FALSE(parser.parse(std::span<const u8>{file}).has_value());
    }

    SECTION("payload count over the cap") {
        auto hb = valid();
        hb.payloadCount = 5;
        const auto file = hb.build();
        txtr::Parser parser;
        CHECK_FALSE(parser.parse(std::span<const u8>{file}).has_value());
    }

    SECTION("header mip count exceeding the chain") {
        auto hb = valid();
        hb.headerMipCount = 2;
        const auto file = hb.build();
        txtr::Parser parser;
        CHECK_FALSE(parser.parse(std::span<const u8>{file}).has_value());
    }

    SECTION("zero dimensions") {
        auto hb = valid();
        hb.width = 0;
        const auto file = hb.build();
        txtr::Parser parser;
        CHECK_FALSE(parser.parse(std::span<const u8>{file}).has_value());
    }

    SECTION("no dimension flag set") {
        auto hb = valid();
        hb.flags = 0;
        const auto file = hb.build();
        txtr::Parser parser;
        CHECK_FALSE(parser.parse(std::span<const u8>{file}).has_value());
    }

    SECTION("platform-tiled data") {
        auto hb = valid();
        hb.flags = 0x0002 | 0x0800;
        const auto file = hb.build();
        txtr::Parser parser;
        CHECK_FALSE(parser.parse(std::span<const u8>{file}).has_value());
    }

    SECTION("truncated inline block") {
        auto hb = valid();
        auto file = hb.build();
        file.resize(file.size() - 8);
        txtr::Parser parser;
        CHECK_FALSE(parser.parse(std::span<const u8>{file}).has_value());
    }
}

TEST_CASE("TXTR detection accepts a valid header and rejects noise", "[txtr]") {
    HeaderBuilder hb;
    hb.mipCount = 1;
    hb.headerMipCount = 1;
    hb.width = 8;
    hb.height = 8;
    hb.inlineBlock = pattern(blockMipBytes(8, 8, 8, 0), 0x55);
    const auto file = hb.build();

    txtr::Parser parser;
    CHECK(parser.detect(std::span<const u8>{file}));

    const std::vector<u8> tooSmall(8, 0);
    CHECK_FALSE(parser.detect(std::span<const u8>{tooSmall}));

    std::vector<u8> zeros(64, 0);
    CHECK_FALSE(parser.detect(std::span<const u8>{zeros}));
}

TEST_CASE("TXTR payload GUIDs are derived from the texture GUID", "[txtr]") {
    // A texture of asset type 004: the top 12 bits reverse to type id 4.
    constexpr u64 kTextureGuid = 0x0C0000000000ABCDull;
    REQUIRE(txtr::assetTypeId(kTextureGuid) == txtr::kAssetTypeTexture);
    REQUIRE(txtr::isTextureGuid(kTextureGuid));

    const u64 payload2 = txtr::makePayloadGuid(kTextureGuid, 2);
    CHECK(payload2 == 0x0320000200000000ull + 0xABCDull);
    CHECK(txtr::assetTypeId(payload2) == txtr::kAssetTypeTexturePayload);
    CHECK(txtr::payloadIndexOf(payload2) == 2);
    CHECK(txtr::payloadGuidToTextureGuid(payload2) == kTextureGuid);

    // The alternate texture type 0F1 selects the other variant nibble.
    constexpr u64 kAltGuid = 0x00F0000000001234ull;
    REQUIRE(txtr::assetTypeId(kAltGuid) == txtr::kAssetTypeTextureAlt);
    const u64 altPayload = txtr::makePayloadGuid(kAltGuid, 1);
    CHECK(((altPayload >> 40) & 3) == 1);
    CHECK(txtr::payloadGuidToTextureGuid(altPayload) == kAltGuid);
}

TEST_CASE("TXTR names the payload files a header depends on", "[txtr]") {
    constexpr u64 kTextureGuid = 0x0C0000000000ABCDull;

    HeaderBuilder hb;
    hb.mipCount = 4;
    hb.headerMipCount = 2;
    hb.width = 32;
    hb.height = 32;
    hb.payloadCount = 3;
    hb.inlineBlock = makePayload(4, 2, 2, bc1Run(32, 32, 2, 2, 0x10));
    const auto file = hb.build();

    CHECK(txtr::Parser::payloadCount(std::span<const u8>{file}) == 3);

    const auto guids = txtr::Parser::payloadGuids(std::span<const u8>{file}, kTextureGuid);
    REQUIRE(guids.size() == 2); // Payload 0 is the inline block.
    CHECK(guids[0] == txtr::makePayloadGuid(kTextureGuid, 1));
    CHECK(guids[1] == txtr::makePayloadGuid(kTextureGuid, 2));

    HeaderBuilder selfContained;
    selfContained.mipCount = 1;
    selfContained.headerMipCount = 1;
    selfContained.width = 8;
    selfContained.height = 8;
    selfContained.inlineBlock = pattern(blockMipBytes(8, 8, 8, 0), 0x55);
    const auto lone = selfContained.build();
    CHECK(txtr::Parser::payloadGuids(std::span<const u8>{lone}, kTextureGuid).empty());
}

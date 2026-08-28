// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Synthetic Diablo IV TEX files, built byte by byte, so the D4 texture path is
// exercised without a CASC corpus.  The record layout comes from the client's
// own type descriptor for `TextureDefinition` (group 44, record size 120) --
// see docs/D4 Specs/TEX_ENGINE_NOTES.md.

#include <catch2/catch_all.hpp>

#include <whiteout/textures/tex/tex.h>
#include <whiteout/textures/texture.h>

#include <array>
#include <cstring>
#include <vector>

using namespace whiteout;
using namespace whiteout::textures;

namespace {

constexpr u32 kSnoMagic = 0xDEADBEEFu;
constexpr u32 kTexFormatHash = 0xF9CD83E7u;
constexpr u32 kHeaderSize = 16;
constexpr u32 kRecordSize = 120;

void put8(std::vector<u8>& b, size_t off, u8 v) {
    b[off] = v;
}

void put16(std::vector<u8>& b, size_t off, u16 v) {
    b[off] = static_cast<u8>(v & 0xFF);
    b[off + 1] = static_cast<u8>((v >> 8) & 0xFF);
}

void put32(std::vector<u8>& b, size_t off, u32 v) {
    for (int i = 0; i < 4; ++i)
        b[off + static_cast<size_t>(i)] = static_cast<u8>((v >> (8 * i)) & 0xFF);
}

void putf(std::vector<u8>& b, size_t off, f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    put32(b, off, bits);
}

/// Builds a minimal but structurally faithful D4 `.tex` metadata file.
struct TexBuilder {
    u32 snoId = 4242;
    u32 recordFlags = 0;
    u32 uiStylePreset = 0;
    u32 texFormat = 9; // BC1
    u16 volumeX = 1;
    u16 volumeY = 1;
    u16 width = 64;
    u16 height = 64;
    u32 depth = 1;
    u8 faceCount = 1;
    u8 mipMin = 0;
    u8 mipMax = 1;
    u32 importFlags = 0;
    i32 resourceType = 0;
    std::array<f32, 4> avgColor{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<i32, 2> hotspot{0, 0};
    std::vector<std::pair<u32, u32>> serTex; ///< {dwOffset, dwSizeAndFlags}
    u32 frameCount = 1;

    std::vector<u8> build() const {
        const u32 serBytes = static_cast<u32>(serTex.size()) * 8;
        const u32 frameBytes = frameCount * 36;

        const u32 serOff = kRecordSize;         // relative to the record base
        const u32 frameOff = serOff + serBytes; // ditto
        const u32 total = kHeaderSize + frameOff + frameBytes;

        std::vector<u8> b(total, 0);

        // -- 16-byte SNO header --
        put32(b, 0, kSnoMagic);
        put32(b, 4, kTexFormatHash);

        const size_t r = kHeaderSize; // record base

        put32(b, r + 0, snoId);
        put32(b, r + 4, recordFlags);
        put32(b, r + 8, uiStylePreset);
        put32(b, r + 12, texFormat);
        put16(b, r + 16, volumeX);
        put16(b, r + 18, volumeY);
        put16(b, r + 20, width);
        put16(b, r + 22, height);
        put32(b, r + 24, depth);
        put8(b, r + 28, faceCount);
        put8(b, r + 29, mipMin);
        put8(b, r + 30, mipMax);
        put32(b, r + 32, importFlags);
        put32(b, r + 36, static_cast<u32>(resourceType));
        for (size_t i = 0; i < 4; ++i)
            putf(b, r + 40 + 4 * i, avgColor[i]);
        put32(b, r + 56, static_cast<u32>(hotspot[0]));
        put32(b, r + 60, static_cast<u32>(hotspot[1]));

        // -- variable-array descriptors: {8 bytes runtime ptr, i32 off, i32 size} --
        put32(b, r + 64 + 8, serOff);
        put32(b, r + 64 + 12, serBytes);
        put32(b, r + 80 + 8, frameOff);
        put32(b, r + 80 + 12, frameBytes);
        // ptGCoeffs at +96 stays empty; ptPostprocessed at +112 stays zero.

        // -- serTex payload --
        for (size_t i = 0; i < serTex.size(); ++i) {
            put32(b, r + serOff + 8 * i, serTex[i].first);
            put32(b, r + serOff + 8 * i + 4, serTex[i].second);
        }

        // -- ptFrame payload: identity UVs --
        for (u32 f = 0; f < frameCount; ++f) {
            const size_t base = r + frameOff + 36ull * f;
            put32(b, base + 0, 0); // hImageHandle
            putf(b, base + 4, 0.0f);
            putf(b, base + 8, 0.0f);
            putf(b, base + 12, 1.0f);
            putf(b, base + 16, 1.0f);
            putf(b, base + 20, 0.0f);
            putf(b, base + 24, 0.0f);
            putf(b, base + 28, 1.0f);
            putf(b, base + 32, 1.0f);
        }

        return b;
    }
};

/// Row geometry of one BCn mip level as the engine lays it out in the payload.
struct MipGeom {
    u32 rows;
    u32 rowBytes;
    u32 pitch;
    u32 payloadBytes;
    u32 tightBytes;
};

MipGeom bcGeom(u32 w, u32 h, u32 bytesPerBlock) {
    MipGeom g{};
    g.rows = (h + 3) / 4;
    g.rowBytes = ((w + 3) / 4) * bytesPerBlock;
    g.pitch = (g.rowBytes + 255) & ~255u;
    g.payloadBytes = g.pitch * g.rows;
    g.tightBytes = g.rowBytes * g.rows;
    return g;
}

MipGeom pixelGeom(u32 w, u32 h, u32 bytesPerPixel) {
    MipGeom g{};
    g.rows = h;
    g.rowBytes = w * bytesPerPixel;
    g.pitch = (g.rowBytes + 255) & ~255u;
    g.payloadBytes = g.pitch * g.rows;
    g.tightBytes = g.rowBytes * g.rows;
    return g;
}

/// Fill one mip's payload region: byte i of row r is (r + 1), padding is 0xEE.
void fillMip(std::vector<u8>& payload, u32 base, const MipGeom& g) {
    for (u32 r = 0; r < g.rows; ++r) {
        const u32 off = base + r * g.pitch;
        for (u32 i = 0; i < g.pitch; ++i)
            payload[off + i] = (i < g.rowBytes) ? static_cast<u8>(r + 1) : 0xEE;
    }
}

bool checkMip(std::span<const u8> data, const MipGeom& g) {
    if (data.size() != g.tightBytes)
        return false;
    for (u32 r = 0; r < g.rows; ++r)
        for (u32 i = 0; i < g.rowBytes; ++i)
            if (data[r * g.rowBytes + i] != static_cast<u8>(r + 1))
                return false;
    return true;
}

std::vector<u8> withSnoHeader(const std::vector<u8>& body) {
    std::vector<u8> out(body.size() + kHeaderSize, 0);
    put32(out, 0, kSnoMagic);
    put32(out, 4, kTexFormatHash);
    std::memcpy(out.data() + kHeaderSize, body.data(), body.size());
    return out;
}

} // namespace

TEST_CASE("D4 TEX: dwSizeAndFlags bit 31 is not part of the size", "[d4][tex]") {
    const MipGeom g = bcGeom(64, 64, 8);

    TexBuilder tb;
    tb.mipMin = 0;
    tb.mipMax = 0;
    tb.serTex = {{0u, g.payloadBytes | 0x80000000u}};
    const auto meta = tb.build();

    std::vector<u8> payload(g.payloadBytes, 0);
    fillMip(payload, 0, g);

    tex::Parser parser;
    tex::D4TexInfo info{};
    auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

    REQUIRE(texture.has_value());
    CHECK(info.snoId == 4242);
    CHECK(texture->mipCount() == 1);
    CHECK(checkMip(texture->mipData(0, 0), g));
}

TEST_CASE("D4 TEX: 256-byte row padding is stripped", "[d4][tex]") {
    // 16x16 BC1: 4 block rows of 32 bytes, padded out to 256.
    const MipGeom g = bcGeom(16, 16, 8);
    REQUIRE(g.pitch == 256);
    REQUIRE(g.rowBytes == 32);

    TexBuilder tb;
    tb.width = 16;
    tb.height = 16;
    tb.mipMin = 0;
    tb.mipMax = 0;
    tb.serTex = {{0u, g.payloadBytes}};
    const auto meta = tb.build();

    std::vector<u8> payload(g.payloadBytes, 0);
    fillMip(payload, 0, g);

    tex::Parser parser;
    tex::D4TexInfo info{};
    auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

    REQUIRE(texture.has_value());
    CHECK(checkMip(texture->mipData(0, 0), g));
}

TEST_CASE("D4 TEX: the record flags carry the payload tiers", "[d4][tex]") {
    const MipGeom g0 = bcGeom(64, 64, 8);
    const MipGeom g1 = bcGeom(32, 32, 8);

    SECTION("single tier, contiguous offsets") {
        TexBuilder tb;
        tb.recordFlags = 0;
        tb.serTex = {{0u, g0.payloadBytes}, {g0.payloadBytes, g1.payloadBytes}};
        const auto meta = tb.build();

        std::vector<u8> payload(g0.payloadBytes + g1.payloadBytes, 0);
        fillMip(payload, 0, g0);
        fillMip(payload, g0.payloadBytes, g1);

        tex::Parser parser;
        tex::D4TexInfo info{};
        auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

        REQUIRE(texture.has_value());
        CHECK_FALSE(info.isTwoTier);
        CHECK_FALSE(info.hasLowPayload);
        CHECK(info.hiResMipCount == 2);
        CHECK(info.lowResMipCount == 0);
        CHECK(texture->mipCount() == 2);
        CHECK(checkMip(texture->mipData(0, 0), g0));
        CHECK(checkMip(texture->mipData(1, 0), g1));
    }

    SECTION("two tier: bit 27 says a paylow payload exists") {
        TexBuilder tb;
        tb.recordFlags = 0x08000000u;
        tb.serTex = {{0u, g0.payloadBytes}, {0u, g1.payloadBytes}};
        const auto meta = tb.build();

        std::vector<u8> hi(g0.payloadBytes, 0);
        fillMip(hi, 0, g0);
        std::vector<u8> lo(g1.payloadBytes, 0);
        fillMip(lo, 0, g1);

        tex::Parser parser;
        tex::D4TexInfo info{};
        auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{hi},
                                    std::span<const u8>{lo}, &info);

        REQUIRE(texture.has_value());
        CHECK(info.isTwoTier);
        CHECK(info.hasLowPayload);
        CHECK_FALSE(info.hasMedPayload);
        CHECK(info.hiResMipCount == 1);
        CHECK(info.lowResMipCount == 1);
        CHECK(checkMip(texture->mipData(0, 0), g0));
        CHECK(checkMip(texture->mipData(1, 0), g1));
    }

    SECTION("bit 26 reports a paymed payload, bit 28 a stub record") {
        TexBuilder tb;
        tb.recordFlags = 0x04000000u | 0x10000000u;
        tb.mipMax = 0;
        tb.serTex = {{0u, g0.payloadBytes}};
        const auto meta = tb.build();

        std::vector<u8> payload(g0.payloadBytes, 0);
        fillMip(payload, 0, g0);

        tex::Parser parser;
        tex::D4TexInfo info{};
        auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

        REQUIRE(texture.has_value());
        CHECK(info.hasMedPayload);
        CHECK(info.isStubRecord);
        CHECK_FALSE(info.hasLowPayload);
        CHECK_FALSE(info.isTwoTier);
        CHECK(info.recordFlags == (0x04000000u | 0x10000000u));
    }
}

TEST_CASE("D4 TEX: a payload that kept its SNO header is detected", "[d4][tex]") {
    const MipGeom g = bcGeom(64, 64, 8);

    TexBuilder tb;
    tb.mipMax = 0;
    tb.serTex = {{0u, g.payloadBytes}};
    const auto meta = tb.build();

    std::vector<u8> body(g.payloadBytes, 0);
    fillMip(body, 0, g);

    SECTION("header present") {
        const auto payload = withSnoHeader(body);
        tex::Parser parser;
        tex::D4TexInfo info{};
        auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);
        REQUIRE(texture.has_value());
        CHECK(info.payloadHeaderStripped);
        CHECK(checkMip(texture->mipData(0, 0), g));
    }

    SECTION("header absent") {
        tex::Parser parser;
        tex::D4TexInfo info{};
        auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{body}, &info);
        REQUIRE(texture.has_value());
        CHECK_FALSE(info.payloadHeaderStripped);
        CHECK(checkMip(texture->mipData(0, 0), g));
    }
}

TEST_CASE("D4 TEX: the 16-byte-per-block formats are accepted", "[d4][tex]") {
    // 16 bytes per 4x4 block per PixelFormat_BytesPerBlockOrPixel.
    const auto fmt = GENERATE(11u, 12u, 42u, 44u, 48u, 49u, 50u, 51u);
    const MipGeom g = bcGeom(64, 64, 16);

    TexBuilder tb;
    tb.texFormat = fmt;
    tb.mipMax = 0;
    tb.serTex = {{0u, g.payloadBytes}};
    const auto meta = tb.build();

    std::vector<u8> payload(g.payloadBytes, 0);
    fillMip(payload, 0, g);

    tex::Parser parser;
    tex::D4TexInfo info{};
    auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

    INFO("eTexFormat = " << fmt);
    REQUIRE(texture.has_value());
    CHECK(info.texFormat == fmt);
    CHECK(checkMip(texture->mipData(0, 0), g));
}

TEST_CASE("D4 TEX: BC7, BC6H and their gamma partners decode to the right format",
          "[d4][tex]") {
    // eTexFormat_ToPrismFormat: 44 -> BC7_RGBA, 50 -> BC7_RGBA_sRGB,
    // 51 -> BC6H_RGBUfloat, 49 -> BC3_RGBA_sRGB.  All are 16 B/block, so only
    // the format identity distinguishes them.
    struct Expect {
        u32 texFormat;
        PixelFormat pixelFormat;
        bool srgb;
    };
    const auto e = GENERATE(Expect{44u, PixelFormat::BC7, false},
                            Expect{50u, PixelFormat::BC7, true},
                            Expect{51u, PixelFormat::BC6H, false},
                            Expect{49u, PixelFormat::BC3, true},
                            Expect{12u, PixelFormat::BC3, false});
    const MipGeom g = bcGeom(64, 64, 16);

    TexBuilder tb;
    tb.texFormat = e.texFormat;
    tb.mipMax = 0;
    tb.serTex = {{0u, g.payloadBytes}};
    const auto meta = tb.build();
    std::vector<u8> payload(g.payloadBytes, 0);
    fillMip(payload, 0, g);

    tex::Parser parser;
    tex::D4TexInfo info{};
    auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

    INFO("eTexFormat = " << e.texFormat);
    REQUIRE(texture.has_value());
    CHECK(texture->format() == e.pixelFormat);
    CHECK(texture->isSrgb() == e.srgb);
}

TEST_CASE("D4 TEX: BC6H_SF16 is decoded at load time", "[d4][tex]") {
    // PixelFormat::BC6H means the unsigned variant, so format 43 cannot be
    // handed back compressed -- the parser decodes it to RGBA32F instead.
    const MipGeom g = bcGeom(64, 64, 16);

    TexBuilder tb;
    tb.texFormat = 43; // BC6H_RGBFloat -- signed endpoints
    tb.mipMax = 0;
    tb.serTex = {{0u, g.payloadBytes}};
    const auto meta = tb.build();
    std::vector<u8> payload(g.payloadBytes, 0);
    fillMip(payload, 0, g);

    tex::Parser parser;
    tex::D4TexInfo info{};
    auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

    REQUIRE(texture.has_value());
    CHECK(info.texFormat == 43);
    CHECK(texture->format() == PixelFormat::RGBA32F);
    CHECK(texture->width() == 64);
    CHECK(texture->height() == 64);
    CHECK(texture->mipData(0, 0).size() == 64ull * 64ull * 16ull);
}

TEST_CASE("D4 TEX: B8G8R8A8 is swizzled and A8 lands in the alpha channel", "[d4][tex]") {
    SECTION("format 0 swaps red and blue") {
        const MipGeom g = pixelGeom(4, 4, 4);

        TexBuilder tb;
        tb.texFormat = 0; // B8G8R8A8
        tb.width = 4;
        tb.height = 4;
        tb.mipMax = 0;
        tb.serTex = {{0u, g.payloadBytes}};
        const auto meta = tb.build();

        std::vector<u8> payload(g.payloadBytes, 0xEE);
        for (u32 r = 0; r < g.rows; ++r) {
            for (u32 x = 0; x < 4; ++x) {
                u8* px = payload.data() + r * g.pitch + x * 4;
                px[0] = 0x11; // B
                px[1] = 0x22; // G
                px[2] = 0x33; // R
                px[3] = 0x44; // A
            }
        }

        tex::Parser parser;
        auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload});
        REQUIRE(texture.has_value());
        REQUIRE(texture->format() == PixelFormat::RGBA8);
        const auto mip = texture->mipData(0, 0);
        REQUIRE(mip.size() == g.tightBytes);
        CHECK(mip[0] == 0x33);
        CHECK(mip[1] == 0x22);
        CHECK(mip[2] == 0x11);
        CHECK(mip[3] == 0x44);
    }

    SECTION("format 23 expands to RGBA8 with RGB at zero") {
        const MipGeom g = pixelGeom(4, 4, 1);

        TexBuilder tb;
        tb.texFormat = 23; // A8
        tb.width = 4;
        tb.height = 4;
        tb.mipMax = 0;
        tb.serTex = {{0u, g.payloadBytes}};
        const auto meta = tb.build();

        std::vector<u8> payload(g.payloadBytes, 0xEE);
        for (u32 r = 0; r < g.rows; ++r)
            for (u32 x = 0; x < 4; ++x)
                payload[r * g.pitch + x] = 0x7Fu;

        tex::Parser parser;
        auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload});
        REQUIRE(texture.has_value());
        REQUIRE(texture->format() == PixelFormat::RGBA8);
        const auto mip = texture->mipData(0, 0);
        REQUIRE(mip.size() == 4ull * 4ull * 4ull);
        CHECK(mip[0] == 0);
        CHECK(mip[1] == 0);
        CHECK(mip[2] == 0);
        CHECK(mip[3] == 0x7Fu);
    }
}

TEST_CASE("D4 TEX: every stored 2D level is decoded", "[d4][tex]") {
    const MipGeom g0 = bcGeom(64, 64, 8);
    const MipGeom g1 = bcGeom(32, 32, 8);
    const MipGeom g2 = bcGeom(16, 16, 8);

    TexBuilder tb;
    tb.mipMin = 0;
    tb.mipMax = 2;
    tb.serTex = {{0u, g0.payloadBytes},
                 {g0.payloadBytes, g1.payloadBytes},
                 {g0.payloadBytes + g1.payloadBytes, g2.payloadBytes}};
    const auto meta = tb.build();

    std::vector<u8> payload(g0.payloadBytes + g1.payloadBytes + g2.payloadBytes, 0);
    fillMip(payload, 0, g0);
    fillMip(payload, g0.payloadBytes, g1);
    fillMip(payload, g0.payloadBytes + g1.payloadBytes, g2);

    tex::Parser parser;
    tex::D4TexInfo info{};
    auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

    REQUIRE(texture.has_value());
    CHECK(info.mipMapLevelMin == 0);
    CHECK(info.mipMapLevelMax == 2);
    REQUIRE(texture->mipCount() == 3);
    CHECK(checkMip(texture->mipData(0, 0), g0));
    CHECK(checkMip(texture->mipData(1, 0), g1));
    CHECK(checkMip(texture->mipData(2, 0), g2));
}

TEST_CASE("D4 TEX: cubemap serTex uses a fixed stride of 11", "[d4][tex]") {
    // 6 faces x 11 slots = 66 entries, face-major, with only the first two
    // slots of each face populated -- the shape
    // `TextureCube_UploadFromPayload` walks with `base += 11`.
    const MipGeom g0 = bcGeom(64, 64, 8);
    const MipGeom g1 = bcGeom(32, 32, 8);
    const u32 faceBytes = g0.payloadBytes + g1.payloadBytes;

    TexBuilder tb;
    tb.faceCount = 6;
    tb.resourceType = 1;
    tb.mipMin = 0;
    tb.mipMax = 1;
    tb.serTex.assign(66, {0u, 0u});
    for (u32 f = 0; f < 6; ++f) {
        tb.serTex[f * 11 + 0] = {f * faceBytes, g0.payloadBytes};
        tb.serTex[f * 11 + 1] = {f * faceBytes + g0.payloadBytes, g1.payloadBytes};
    }
    const auto meta = tb.build();

    std::vector<u8> payload(6ull * faceBytes, 0);
    for (u32 f = 0; f < 6; ++f) {
        fillMip(payload, f * faceBytes, g0);
        fillMip(payload, f * faceBytes + g0.payloadBytes, g1);
    }

    tex::Parser parser;
    tex::D4TexInfo info{};
    auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

    REQUIRE(texture.has_value());
    CHECK(info.faceCount == 6);
    CHECK(info.textureResourceType == 1);
    REQUIRE(texture->mipCount() == 2);
    for (u32 f = 0; f < 6; ++f) {
        INFO("face " << f);
        CHECK(checkMip(texture->mipData(0, f), g0));
        CHECK(checkMip(texture->mipData(1, f), g1));
    }
}

TEST_CASE("D4 TEX: previously unread metadata fields", "[d4][tex]") {
    const MipGeom g = bcGeom(64, 64, 8);

    TexBuilder tb;
    tb.mipMax = 0;
    tb.uiStylePreset = 777;
    tb.volumeX = 32;
    tb.volumeY = 2;
    tb.hotspot = {17, -9};
    tb.avgColor = {0.25f, 0.5f, 0.75f, 1.0f};
    tb.resourceType = 0;
    tb.importFlags = 0x00000010u;
    tb.serTex = {{0u, g.payloadBytes}};
    const auto meta = tb.build();

    std::vector<u8> payload(g.payloadBytes, 0);
    fillMip(payload, 0, g);

    tex::Parser parser;
    tex::D4TexInfo info{};
    auto texture = parser.parse(std::span<const u8>{meta}, std::span<const u8>{payload}, &info);

    REQUIRE(texture.has_value());
    CHECK(info.uiStylePreset == 777); // Preset SNO id, not the eTexFormat value
    CHECK(info.volumeXSlices == 32);
    CHECK(info.volumeYSlices == 2);
    CHECK(info.hotspot[0] == 17);
    CHECK(info.hotspot[1] == -9);
    CHECK(info.avgColor[0] == Catch::Approx(0.25f));
    CHECK(info.avgColor[3] == Catch::Approx(1.0f));
    CHECK(info.importFlags == 0x00000010u);
    CHECK(info.frames.size() == 1);
}

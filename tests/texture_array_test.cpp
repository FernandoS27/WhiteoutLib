// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Tests for Texture2DArray / TextureCubeArray support, including DDS
// parse/write round-trip.

#include <catch2/catch_all.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <whiteout/textures/dds/dds.h>
#include <whiteout/textures/texture.h>

using namespace whiteout;
using namespace whiteout::textures;

namespace {

std::optional<Texture> load_dds_file(const std::string& path, std::string* error) {
    dds::Parser parser;
    auto result = parser.parse(path);
    if (parser.hasIssues()) {
        if (error) *error = parser.getIssues().front();
        return std::nullopt;
    }
    return result;
}

bool save_dds_file(const Texture& tex, const std::string& path, std::string* error) {
    dds::Writer writer;
    writer.write(path, tex);
    if (writer.hasIssues()) {
        if (error) *error = writer.getIssues().front();
        return false;
    }
    return true;
}

// Deterministic byte-level fill so each (layer, mip) pair contains
// distinguishable, reproducible content. Operates on the raw byte span
// so it works for both uncompressed and block-compressed formats.
void fill_texture(Texture& tex) {
    const u32 layers = tex.layerCount();
    const u32 mips = tex.mipCount();
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 mip = 0; mip < mips; ++mip) {
            auto span = tex.mipData(mip, layer);
            for (size_t i = 0; i < span.size(); ++i) {
                u32 v = (layer * 37u + static_cast<u32>(i) * 11u + mip * 7u) & 0xFFu;
                span[i] = static_cast<u8>(v);
            }
        }
    }
}

bool textures_data_equal(const Texture& a, const Texture& b) {
    if (a.dataSize() != b.dataSize()) return false;
    return std::memcmp(a.dataPtr(), b.dataPtr(), static_cast<size_t>(a.dataSize())) == 0;
}

} // namespace

// ============================================================================
// Basic construction
// ============================================================================

TEST_CASE("Texture2DArray basic shape", "[texture][array]") {
    Texture tex = Texture::create2DArray(PixelFormat::RGBA8, 32, 16, 4);

    CHECK(tex.type() == TextureType::Texture2DArray);
    CHECK(tex.format() == PixelFormat::RGBA8);
    CHECK(tex.width() == 32);
    CHECK(tex.height() == 16);
    CHECK(tex.depth() == 1);
    CHECK(tex.arraySize() == 4);
    CHECK(tex.layerCount() == 4);

    // 32 → 16 → 8 → 4 → 2 → 1 ⇒ 6 mips.
    CHECK(tex.mipCount() == computeMaxMipCount(32, 16));

    // Every (layer, mip) pair must have a valid descriptor and span.
    for (u32 layer = 0; layer < tex.layerCount(); ++layer) {
        for (u32 mip = 0; mip < tex.mipCount(); ++mip) {
            const auto& lvl = tex.mipLevel(mip, layer);
            CHECK(lvl.size > 0);
            CHECK(tex.mipData(mip, layer).size() == lvl.size);
        }
    }
}

TEST_CASE("TextureCubeArray basic shape", "[texture][array]") {
    Texture tex = Texture::createCubeArray(PixelFormat::RGBA8, 16, 3);

    CHECK(tex.type() == TextureType::TextureCubeArray);
    CHECK(tex.width() == 16);
    CHECK(tex.height() == 16);
    CHECK(tex.arraySize() == 3);
    CHECK(tex.layerCount() == 18); // 6 faces × 3 cube-maps
    CHECK(tex.mipCount() == computeMaxMipCount(16, 16));
}

TEST_CASE("Non-array textures report arraySize == 1", "[texture][array]") {
    CHECK(Texture::create2D(PixelFormat::RGBA8, 8, 8).arraySize() == 1);
    CHECK(Texture::create3D(PixelFormat::RGBA8, 8, 8, 4).arraySize() == 1);
    CHECK(Texture::createCube(PixelFormat::RGBA8, 8).arraySize() == 1);
}

TEST_CASE("Array layer byte offsets are distinct and well-ordered",
          "[texture][array]") {
    Texture tex = Texture::create2DArray(PixelFormat::R8, 8, 8, 3, 1);
    const auto& l0 = tex.mipLevel(0, 0);
    const auto& l1 = tex.mipLevel(0, 1);
    const auto& l2 = tex.mipLevel(0, 2);
    CHECK(l0.offset == 0);
    CHECK(l1.offset == l0.offset + l0.size);
    CHECK(l2.offset == l1.offset + l1.size);
    CHECK(l2.offset + l2.size == tex.dataSize());
}

// ============================================================================
// DDS round-trip
// ============================================================================

TEST_CASE("DDS round-trip: Texture2DArray uncompressed", "[texture][array][dds]") {
    const std::string path = "test_tex2darray.dds";

    Texture src = Texture::create2DArray(PixelFormat::RGBA8, 16, 8, 5);
    fill_texture(src);

    std::string err;
    REQUIRE(save_dds_file(src, path, &err));
    auto loaded = load_dds_file(path, &err);
    REQUIRE(loaded.has_value());

    CHECK(loaded->type() == TextureType::Texture2DArray);
    CHECK(loaded->format() == PixelFormat::RGBA8);
    CHECK(loaded->width() == 16);
    CHECK(loaded->height() == 8);
    CHECK(loaded->arraySize() == 5);
    CHECK(loaded->layerCount() == 5);
    CHECK(loaded->mipCount() == src.mipCount());
    CHECK(textures_data_equal(*loaded, src));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("DDS round-trip: Texture2DArray BC3", "[texture][array][dds]") {
    const std::string path = "test_tex2darray_bc3.dds";

    // Construct BC3 array manually (no encode needed — opaque blob).
    Texture src = Texture::create2DArray(PixelFormat::BC3, 16, 16, 3, 2);
    fill_texture(src);

    std::string err;
    REQUIRE(save_dds_file(src, path, &err));
    auto loaded = load_dds_file(path, &err);
    REQUIRE(loaded.has_value());

    CHECK(loaded->type() == TextureType::Texture2DArray);
    CHECK(loaded->format() == PixelFormat::BC3);
    CHECK(loaded->arraySize() == 3);
    CHECK(loaded->mipCount() == 2);
    CHECK(textures_data_equal(*loaded, src));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("DDS round-trip: TextureCubeArray uncompressed",
          "[texture][array][dds]") {
    const std::string path = "test_texcubearray.dds";

    Texture src = Texture::createCubeArray(PixelFormat::RGBA8, 8, 2);
    fill_texture(src);

    std::string err;
    REQUIRE(save_dds_file(src, path, &err));
    auto loaded = load_dds_file(path, &err);
    REQUIRE(loaded.has_value());

    CHECK(loaded->type() == TextureType::TextureCubeArray);
    CHECK(loaded->format() == PixelFormat::RGBA8);
    CHECK(loaded->arraySize() == 2);
    CHECK(loaded->layerCount() == 12);
    CHECK(loaded->mipCount() == src.mipCount());
    CHECK(textures_data_equal(*loaded, src));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("DDS round-trip: TextureCubeArray BC7 sRGB",
          "[texture][array][dds]") {
    const std::string path = "test_texcubearray_bc7_srgb.dds";

    Texture src = Texture::createCubeArray(PixelFormat::BC7, 8, 2, 1);
    src.setSrgb(true);
    fill_texture(src);

    std::string err;
    REQUIRE(save_dds_file(src, path, &err));
    auto loaded = load_dds_file(path, &err);
    REQUIRE(loaded.has_value());

    CHECK(loaded->type() == TextureType::TextureCubeArray);
    CHECK(loaded->format() == PixelFormat::BC7);
    CHECK(loaded->arraySize() == 2);
    CHECK(loaded->layerCount() == 12);
    CHECK(loaded->isSrgb());
    CHECK(textures_data_equal(*loaded, src));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("DDS round-trip: 2D (non-array) preserves arraySize == 1",
          "[texture][array][dds]") {
    const std::string path = "test_tex2d_single.dds";

    Texture src = Texture::create2D(PixelFormat::RGBA8, 16, 16, 1);
    fill_texture(src);

    std::string err;
    REQUIRE(save_dds_file(src, path, &err));
    auto loaded = load_dds_file(path, &err);
    REQUIRE(loaded.has_value());

    CHECK(loaded->type() == TextureType::Texture2D);
    CHECK(loaded->arraySize() == 1);
    CHECK(loaded->layerCount() == 1);
    CHECK(textures_data_equal(*loaded, src));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ============================================================================
// Per-layer data integrity
// ============================================================================

TEST_CASE("Array layers hold independent data after DDS round-trip",
          "[texture][array][dds]") {
    const std::string path = "test_tex2darray_perlayer.dds";

    Texture src = Texture::create2DArray(PixelFormat::RGBA8, 4, 4, 3, 1);

    // Paint each layer a solid distinct color.
    const std::array<std::array<u8, 4>, 3> colors = {{
        {{10, 20, 30, 40}},
        {{90, 80, 70, 60}},
        {{200, 150, 100, 50}},
    }};
    for (u32 layer = 0; layer < 3; ++layer) {
        auto span = src.mipData(0, layer);
        for (size_t px = 0; px < span.size() / 4; ++px) {
            std::memcpy(span.data() + px * 4, colors[layer].data(), 4);
        }
    }

    std::string err;
    REQUIRE(save_dds_file(src, path, &err));
    auto loaded = load_dds_file(path, &err);
    REQUIRE(loaded.has_value());

    for (u32 layer = 0; layer < 3; ++layer) {
        auto span = loaded->mipData(0, layer);
        for (size_t px = 0; px < span.size() / 4; ++px) {
            CHECK(span[px * 4 + 0] == colors[layer][0]);
            CHECK(span[px * 4 + 1] == colors[layer][1]);
            CHECK(span[px * 4 + 2] == colors[layer][2]);
            CHECK(span[px * 4 + 3] == colors[layer][3]);
        }
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ============================================================================
// Copy / move semantics for array textures
// ============================================================================

TEST_CASE("Texture2DArray deep copy preserves layers", "[texture][array]") {
    Texture src = Texture::create2DArray(PixelFormat::RGBA8, 4, 4, 3, 1);
    fill_texture(src);

    Texture copy = src;
    CHECK(copy.type() == TextureType::Texture2DArray);
    CHECK(copy.arraySize() == 3);
    CHECK(copy.layerCount() == 3);
    CHECK(textures_data_equal(copy, src));

    // Mutating the copy must not affect the original.
    copy.mipData(0, 1)[0] = 0xAB;
    CHECK(src.mipData(0, 1)[0] != 0xAB);
}

TEST_CASE("TextureCubeArray copyAsFormat preserves array layout",
          "[texture][array]") {
    Texture src = Texture::createCubeArray(PixelFormat::RGBA8, 8, 2, 1);
    fill_texture(src);

    Texture dst = src.copyAsFormat(PixelFormat::RGBA16);

    CHECK(dst.type() == TextureType::TextureCubeArray);
    CHECK(dst.arraySize() == 2);
    CHECK(dst.layerCount() == 12);
    CHECK(dst.format() == PixelFormat::RGBA16);
    CHECK(dst.mipCount() == src.mipCount());
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The panorama-to-cube projection: which face reads which part of the sky.

#include <catch2/catch_all.hpp>

#include <whiteout/textures/environment_map.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <array>

using namespace whiteout;
using namespace whiteout::textures;

namespace {

/// A black 64x32 panorama with @p rgb painted over columns [x, x + w) of
/// rows [y, y + 4).
Texture panoramaWithBlock(u32 x, u32 y, u32 w, const u8 rgb[3]) {
    Texture panorama = Texture::create2D(PixelFormat::RGBA8, 64, 32, 1);
    const std::span<u8> px = panorama.mipData(0);
    std::fill(px.begin(), px.end(), u8{0});
    for (u32 row = y; row < y + 4 && row < 32; ++row) {
        for (u32 col = x; col < x + w && col < 64; ++col) {
            u8* at = &px[(static_cast<std::size_t>(row) * 64 + col) * 4];
            at[0] = rgb[0];
            at[1] = rgb[1];
            at[2] = rgb[2];
            at[3] = 255;
        }
    }
    return panorama;
}

/// A 4x4 cube of three levels whose face f at level l reads (40f, 60l, 0), with
/// a blue marker at (0, 0) of face 5's first level.
Texture labelledCube() {
    Texture cube = Texture::createCube(PixelFormat::RGBA8, 4, 3);
    for (u32 level = 0; level < 3; ++level) {
        for (u32 face = 0; face < 6; ++face) {
            const std::span<u8> px = cube.mipData(level, face);
            for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
                px[i] = static_cast<u8>(40 * face);
                px[i + 1] = static_cast<u8>(60 * level);
                px[i + 2] = 0;
                px[i + 3] = 255;
            }
        }
    }
    cube.mipData(0, 5)[2] = 255;
    return cube;
}

const u8* texelAt(const Texture& cube, u32 face, u32 level, u32 x, u32 y) {
    const u32 n = std::max<u32>(cube.width() >> level, 1);
    return &cube.mipData(level, face)[(static_cast<std::size_t>(y) * n + x) * 4];
}

const u8* levelCentre(const Texture& cube, u32 face, u32 level) {
    const u32 n = std::max<u32>(cube.width() >> level, 1);
    return texelAt(cube, face, level, n / 2, n / 2);
}

constexpr std::array<f32, 9> kIdentity = {1, 0, 0, 0, 1, 0, 0, 0, 1};

/// The texel at the centre of @p face.
const u8* faceCentre(const Texture& cube, u32 face) {
    const u32 size = cube.width();
    return &cube.mipData(0, face)[(static_cast<std::size_t>(size / 2) * size + size / 2) * 4];
}

} // namespace

TEST_CASE("the panorama's top rows are the +Z face", "[environment_map]") {
    // The pole is every column of the top row at once, so the band spans them.
    const u8 white[3] = {255, 255, 255};
    const std::optional<Texture> cube =
        env::CubeFromPanorama(panoramaWithBlock(0, 0, 64, white), 16);
    REQUIRE(cube.has_value());
    CHECK(cube->type() == TextureType::TextureCube);
    CHECK(cube->layerCount() == 6);
    CHECK(cube->width() == 16);
    CHECK(faceCentre(*cube, 4)[0] > 100); // +Z looks at the zenith
    CHECK(faceCentre(*cube, 5)[0] < 20);  // -Z looks at the ground
    CHECK(faceCentre(*cube, 2)[0] < 20);  // +Y looks at the horizon
}

TEST_CASE("the panorama's centre column is +Y and a quarter turn east is +X",
          "[environment_map]") {
    const u8 green[3] = {0, 255, 0};
    const std::optional<Texture> forward =
        env::CubeFromPanorama(panoramaWithBlock(30, 14, 4, green), 16);
    REQUIRE(forward.has_value());
    CHECK(faceCentre(*forward, 2)[1] > 100); // +Y
    CHECK(faceCentre(*forward, 0)[1] < 20);
    CHECK(faceCentre(*forward, 1)[1] < 20);

    const u8 red[3] = {255, 0, 0};
    const std::optional<Texture> east =
        env::CubeFromPanorama(panoramaWithBlock(46, 14, 4, red), 16);
    REQUIRE(east.has_value());
    CHECK(faceCentre(*east, 0)[0] > 100); // +X
    CHECK(faceCentre(*east, 1)[0] < 20);  // -X
    CHECK(faceCentre(*east, 2)[0] < 20);
}

TEST_CASE("an empty panorama or a zero face projects to nothing", "[environment_map]") {
    const u8 white[3] = {255, 255, 255};
    CHECK_FALSE(env::CubeFromPanorama(panoramaWithBlock(0, 0, 4, white), 0).has_value());
    CHECK_FALSE(env::CubeFromPanorama(Texture{}, 16).has_value());
}

TEST_CASE("a cube moved to a new up axis keeps every level and turns its faces",
          "[environment_map]") {
    // A z-up direction read from a y-up cube, stored at (x, z, -y).
    constexpr std::array<f32, 9> yUpFromZUp = {1, 0, 0, 0, 0, 1, 0, -1, 0};
    const std::optional<Texture> cube = env::CubeFromCube(labelledCube(), 0, yUpFromZUp);
    REQUIRE(cube.has_value());
    CHECK(cube->type() == TextureType::TextureCube);
    CHECK(cube->mipCount() == 3);
    for (u32 level = 0; level < 3; ++level) {
        CHECK(levelCentre(*cube, 4, level)[0] == 40 * 2);     // +Z reads the source +Y
        CHECK(levelCentre(*cube, 5, level)[0] == 40 * 3);     // -Z reads -Y
        CHECK(levelCentre(*cube, 2, level)[0] == 40 * 5);     // +Y reads -Z
        CHECK(levelCentre(*cube, 3, level)[0] == 40 * 4);     // -Y reads +Z
        CHECK(levelCentre(*cube, 0, level)[0] == 0);          // +X stays
        CHECK(levelCentre(*cube, 4, level)[1] == 60 * level); // each from its own level
    }
    // The -Z marker at (0, 0) turns half a revolution on its way to +Y.
    CHECK(texelAt(*cube, 2, 0, 3, 3)[2] == 255);
    CHECK(texelAt(*cube, 2, 0, 0, 0)[2] == 0);
}

TEST_CASE("a cube face order names the stored layer each face reads", "[environment_map]") {
    const std::optional<Texture> cube =
        env::CubeFromCube(labelledCube(), 0, kIdentity, {0, 5, 2, 3, 4, 1});
    REQUIRE(cube.has_value());
    CHECK(levelCentre(*cube, 1, 0)[0] == 40 * 5);
    CHECK(levelCentre(*cube, 5, 0)[0] == 40 * 1);
    CHECK(levelCentre(*cube, 2, 0)[0] == 40 * 2);
}

TEST_CASE("the second cube of a cube array is read from its own layers", "[environment_map]") {
    Texture array = Texture::createCubeArray(PixelFormat::RGBA8, 2, 2, 1);
    for (u32 layer = 0; layer < 12; ++layer) {
        const std::span<u8> px = array.mipData(0, layer);
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
            px[i] = static_cast<u8>(20 * layer);
            px[i + 3] = 255;
        }
    }
    const std::optional<Texture> cube = env::CubeFromCube(array, 1, kIdentity);
    REQUIRE(cube.has_value());
    CHECK(texelAt(*cube, 0, 0, 0, 0)[0] == 120);
    CHECK(texelAt(*cube, 5, 0, 1, 1)[0] == 220);
}

TEST_CASE("a flat texture, a missing cube or a bad face order re-expresses to nothing",
          "[environment_map]") {
    CHECK_FALSE(env::CubeFromCube(Texture::create2D(PixelFormat::RGBA8, 4, 4, 1), 0, kIdentity)
                    .has_value());
    CHECK_FALSE(env::CubeFromCube(labelledCube(), 1, kIdentity).has_value());
    CHECK_FALSE(env::CubeFromCube(labelledCube(), 0, kIdentity, {0, 1, 2, 3, 4, 6}).has_value());
}

TEST_CASE("a cube read through the identity basis comes back texel for texel",
          "[environment_map]") {
    // Every texel distinct, so each face's lookup branch has to invert its
    // direction exactly.
    Texture cube = Texture::createCube(PixelFormat::RGBA8, 4, 1);
    for (u32 face = 0; face < 6; ++face) {
        const std::span<u8> px = cube.mipData(0, face);
        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                u8* at = &px[(static_cast<std::size_t>(y) * 4 + x) * 4];
                at[0] = static_cast<u8>(40 * face);
                at[1] = static_cast<u8>(60 * x);
                at[2] = static_cast<u8>(60 * y);
                at[3] = 255;
            }
        }
    }
    const std::optional<Texture> same = env::CubeFromCube(cube, 0, kIdentity);
    REQUIRE(same.has_value());
    for (u32 face = 0; face < 6; ++face) {
        const std::span<const u8> a = cube.mipData(0, face);
        const std::span<const u8> b = same->mipData(0, face);
        CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));
    }
}

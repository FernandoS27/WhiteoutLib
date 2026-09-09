// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The panorama-to-cube projection: which face reads which part of the sky.

#include <catch2/catch_all.hpp>

#include <whiteout/textures/environment_map.h>
#include <whiteout/textures/texture.h>

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

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The Diablo III `.tex` reader on what only the shipped files settle: how a
/// cube is laid out, and what pixel format 43 holds.
///
/// A cube's mip table is face-major, ten slots a face, and its data is one
/// 16-byte prefix followed by each face's whole mip chain. The flat cubes pin
/// the faces, `CharacterSelect_IrradianceMap` the chain order within a face.
///
/// The `[install]` arms read the game through CASC and skip with a warning when
/// no Diablo III install is found.

#include <catch2/catch_all.hpp>

#include <whiteout/textures/tex/tex.h>
#include <whiteout/textures/texture.h>

// White-box: the synthetic arm checks the table the writer lays out.
#include "whiteout/textures/tex/tex_internal.h"

#if defined(WHITEOUT_HAS_CASC)
#include "d3_install.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::textures;
namespace tx = whiteout::textures::tex;

namespace {

tx::MipEntry entryAt(const std::vector<u8>& file, u32 slot) {
    tx::MipEntry entry{};
    std::memcpy(&entry, file.data() + tx::OFF_MIP_TABLE + slot * sizeof(tx::MipEntry),
                sizeof(entry));
    return entry;
}

} // namespace

TEST_CASE("D3 tex: a cube is written and read face-major", "[d3][tex]") {
    // Two mips of BC1, every block a different byte pattern, so a swapped face,
    // a swapped level or a misplaced prefix all change what comes back.
    Texture cube = Texture::createCube(PixelFormat::BC1, 8, 2);
    for (u32 face = 0; face < 6; ++face) {
        for (u32 mip = 0; mip < 2; ++mip) {
            auto data = cube.mipData(mip, face);
            for (size_t i = 0; i < data.size(); ++i) {
                data[i] = static_cast<u8>(face * 40 + mip * 17 + i * 3);
            }
        }
    }

    tx::Writer writer;
    const std::vector<u8> file = writer.write(cube);
    REQUIRE_FALSE(file.empty());

    // One prefix, then face 0's chain, face 1's, ...: every entry starts where
    // the one before it ended, and the data of each sits past the prefix.
    const u32 sizes[2] = {32, 8};
    u32 expected = entryAt(file, 0).fileOffset;
    for (u32 face = 0; face < 6; ++face) {
        for (u32 mip = 0; mip < 2; ++mip) {
            const tx::MipEntry entry = entryAt(file, face * tx::CUBE_MIP_SLOTS_PER_FACE + mip);
            CHECK(entry.fileOffset == expected);
            CHECK(entry.dataSize == sizes[mip]);
            expected += entry.dataSize;
        }
        // The slots past a face's chain stay empty.
        CHECK(entryAt(file, face * tx::CUBE_MIP_SLOTS_PER_FACE + 2).dataSize == 0);
    }
    CHECK(file.size() == expected + tx::BC_MIP_PREFIX_SIZE);

    tx::Parser parser;
    auto back = parser.parse(file);
    REQUIRE(back.has_value());
    CHECK(back->type() == TextureType::TextureCube);
    REQUIRE(back->mipCount() == 2);
    for (u32 face = 0; face < 6; ++face) {
        for (u32 mip = 0; mip < 2; ++mip) {
            const auto want = cube.mipData(mip, face);
            const auto got = back->mipData(mip, face);
            CHECK(std::equal(want.begin(), want.end(), got.begin(), got.end()));
        }
    }
}

TEST_CASE("D3 tex: format 43 is read as DXT5, not BC5", "[d3][tex]") {
    // The id says ATI2, but the shipped 43s are DXT5 normal maps; see the
    // [install] arm. Nothing writes a BC5 .tex, which the game would misread.
    tx::Writer writer;
    const std::vector<u8> bc5 = writer.write(Texture::create2D(PixelFormat::BC5, 4, 4, 1));
    CHECK(bc5.empty());
    CHECK(writer.hasIssues());

    std::vector<u8> file = writer.write(Texture::create2D(PixelFormat::BC3, 4, 4, 1));
    REQUIRE_FALSE(file.empty());
    const u32 ati2 = 43;
    std::memcpy(file.data() + tx::OFF_TEXTURE_DESC, &ati2, sizeof(ati2));
    tx::Parser parser;
    auto back = parser.parse(file);
    REQUIRE(back.has_value());
    CHECK(back->format() == PixelFormat::BC3);
}

#if defined(WHITEOUT_HAS_CASC)

namespace {

/// Decode a shipped texture to RGBA8, or nullopt with the reason.
std::optional<Texture> decodeShipped(const storages::casc::Storage& storage,
                                     const std::string& name) {
    const std::vector<u8> bytes =
        test::d3::readInstallFile(storage, "Base\\Textures\\" + name + ".tex");
    INFO(name);
    REQUIRE_FALSE(bytes.empty());
    tx::Parser parser;
    auto parsed = parser.parse(bytes);
    if (!parsed) {
        for (const std::string& issue : parser.getIssues()) {
            UNSCOPED_INFO(issue);
        }
        return std::nullopt;
    }
    return parsed->copyAsFormat(PixelFormat::RGBA8);
}

/// Mean of R, G and B over one level of one face, in [0, 1].
f64 meanOf(const Texture& rgba, u32 mip, u32 face) {
    const auto data = rgba.mipData(mip, face);
    f64 sum = 0.0;
    for (size_t i = 0; i < data.size(); i += 4) {
        sum += data[i] + data[i + 1] + data[i + 2];
    }
    return sum / (3.0 * 255.0 * static_cast<f64>(data.size() / 4));
}

} // namespace

TEST_CASE("D3 tex: the shipped irradiance cubes decode", "[d3][tex][install]") {
    auto storage = test::d3::openInstall();
    if (!storage) {
        WARN("no Diablo III install found (set WHITEOUT_CASC_D3); skipping");
        return;
    }

    SECTION("the flat cubes are one value on every texel of every face and level") {
        struct Flat {
            const char* name;
            f64 value;
        };
        // grey_irradiance is 565-quantised: R and B 0.441, G 0.429.
        for (const Flat& flat : {Flat{"neutral_unlit_irradiance", 0.5},
                                 Flat{"white_irradiance", 1.0}, Flat{"grey_irradiance", 0.44}}) {
            INFO(flat.name);
            auto rgba = decodeShipped(*storage, flat.name);
            REQUIRE(rgba.has_value());
            REQUIRE(rgba->type() == TextureType::TextureCube);
            u32 off = 0, texels = 0;
            for (u32 face = 0; face < 6; ++face) {
                for (u32 mip = 0; mip < rgba->mipCount(); ++mip) {
                    const auto data = rgba->mipData(mip, face);
                    for (size_t i = 0; i < data.size(); i += 4, ++texels) {
                        for (size_t c = 0; c < 3; ++c) {
                            if (std::fabs(data[i + c] / 255.0 - flat.value) > 0.015) {
                                ++off;
                            }
                        }
                    }
                }
            }
            CHECK(texels > 0);
            CHECK(off == 0);
        }
    }

    SECTION("the core irradiance cube is 32x32x6 with its own face means") {
        // Base\Textures\irradiance.tex, SNO 47336: a single-mip cube, the case
        // the old per-face-prefix reading ran off the end of the file on.
        auto rgba = decodeShipped(*storage, "irradiance");
        REQUIRE(rgba.has_value());
        REQUIRE(rgba->type() == TextureType::TextureCube);
        CHECK(rgba->width() == 32);
        CHECK(rgba->height() == 32);
        CHECK(rgba->mipCount() == 1);
        const f64 means[6] = {0.297, 1.0, 0.567, 0.933, 0.623, 0.826};
        for (u32 face = 0; face < 6; ++face) {
            INFO("face " << face);
            CHECK(meanOf(*rgba, 0, face) == Catch::Approx(means[face]).margin(0.01));
        }
    }

    SECTION("each face's chain holds that face's levels, in order") {
        // 128x128, six mips a face: a level read from the wrong place would not
        // be its parent averaged down.
        auto rgba = decodeShipped(*storage, "CharacterSelect_IrradianceMap");
        REQUIRE(rgba.has_value());
        REQUIRE(rgba->mipCount() == 6);
        for (u32 face = 0; face < 6; ++face) {
            INFO("face " << face);
            const auto top = rgba->mipData(0, face);
            const auto next = rgba->mipData(1, face);
            const u32 w = rgba->width() / 2;
            f64 err = 0.0;
            for (u32 y = 0; y < w; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    for (u32 c = 0; c < 3; ++c) {
                        f64 box = 0.0;
                        for (u32 j = 0; j < 2; ++j) {
                            for (u32 i = 0; i < 2; ++i) {
                                box += top[((2 * y + j) * 2 * w + 2 * x + i) * 4 + c];
                            }
                        }
                        err += std::fabs(box / 4.0 - next[(y * w + x) * 4 + c]);
                    }
                }
            }
            CHECK(err / (3.0 * w * w * 255.0) < 0.01);
            CHECK(meanOf(*rgba, 1, face) == Catch::Approx(meanOf(*rgba, 0, face)).margin(0.01));
        }
    }
}

TEST_CASE("D3 tex: a shipped format 43 is a DXT5 normal map", "[d3][tex][install]") {
    auto storage = test::d3::openInstall();
    if (!storage) {
        WARN("no Diablo III install found (set WHITEOUT_CASC_D3); skipping");
        return;
    }
    // X in alpha, Y in green, R = 1 and B = 0: every texel of both. Read as BC5,
    // the colour half's 565 endpoints become a second channel of noise.
    for (const char* name : {"flat_NM", "a2dun_zolt_Statue_A_autoNM"}) {
        INFO(name);
        auto rgba = decodeShipped(*storage, name);
        REQUIRE(rgba.has_value());
        const auto data = rgba->mipData(0, 0);
        size_t swizzled = 0;
        for (size_t i = 0; i < data.size(); i += 4) {
            if (data[i] == 255 && data[i + 2] == 0) {
                ++swizzled;
            }
        }
        CHECK(swizzled == data.size() / 4);
        // Texel (0, 0) sits in a flat area of both: a normal of (0, 0, 1).
        CHECK(data[1] == 128);
        CHECK(data[3] == 128);
    }
}

TEST_CASE("D3 tex: the projected layer's default mask", "[d3][tex][install]") {
    auto storage = test::d3::openInstall();
    if (!storage) {
        WARN("no Diablo III install found (set WHITEOUT_CASC_D3); skipping");
        return;
    }
    // CoreAssets 153, what a projected layer's receiver without a type-58
    // entry gates the layer by (its red channel): white, so it lets all of it
    // through wherever it is sampled.
    auto rgba = decodeShipped(*storage, "default_pma_mask");
    REQUIRE(rgba.has_value());
    CHECK(rgba->width() == 4);
    CHECK(rgba->height() == 4);
    const auto data = rgba->mipData(0, 0);
    for (size_t i = 0; i < data.size(); i += 4) {
        CHECK(data[i] == 255);
    }
}

#endif // WHITEOUT_HAS_CASC

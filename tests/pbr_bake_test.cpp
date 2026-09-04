// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// The specular/gloss -> metal/rough crossing (textures/pbr_bake.h). Every case
// here is a *value* check, because the failure mode this file exists to catch
// is a bake that runs, writes a plausible-looking map and puts the wrong number
// in it -- a green gate over a channel nobody read.

#include <catch2/catch_all.hpp>

#include <cmath>
#include <vector>

#include <whiteout/textures/pbr_bake.h>
#include <whiteout/textures/texture.h>

using namespace whiteout;
using namespace whiteout::textures;

namespace {

/// A 2x2 RGBA8 texture, every texel the same.
Texture solid(u8 r, u8 g, u8 b, u8 a, u32 size = 2) {
    Texture texture = Texture::create2D(PixelFormat::RGBA8, size, size, 1);
    const std::span<u8> pixels = texture.mipData(0);
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = a;
    }
    return texture;
}

u8 channelOf(const Texture& texture, Channel channel) {
    return texture.mipData(0)[static_cast<u32>(channel)];
}

f32 srgbToLinear(f32 value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

f32 smoothstep(f32 edge0, f32 edge1, f32 x) {
    const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

TEST_CASE("an absent recipe bakes nothing", "[pbr_bake]") {
    pbr::OrmRecipe recipe;
    REQUIRE_FALSE(pbr::BakeOrm(recipe).has_value());
}

TEST_CASE("the neutral: no specular, no gloss, no masks", "[pbr_bake]") {
    const Texture specular = solid(0, 0, 0, 255);
    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    // Occlusion defaults to unoccluded, metalness to none, and a black
    // specular is the roughest a surface gets: 0.1 + 0.8 * 1 = 0.9.
    CHECK(channelOf(*orm, Channel::R) == 255);
    CHECK(channelOf(*orm, Channel::G) == 230); // round(0.9 * 255)
    CHECK(channelOf(*orm, Channel::B) == 0);
    CHECK(channelOf(*orm, Channel::A) == 0);
}

TEST_CASE("gloss is roughness backwards", "[pbr_bake]") {
    const Texture specular = solid(0, 0, 0, 255);
    // A gloss map authored in green, which is how `Adept_Golden_Gloss.dds`
    // ships -- the channel select is not decoration.
    const Texture gloss = solid(0, 200, 0, 0);

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;
    recipe.gloss.texture = &gloss;
    recipe.gloss.channel = Channel::G;
    recipe.gloss.invert = true;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(channelOf(*orm, Channel::G) == 55); // 255 - 200
    // Reading the wrong channel would give 255 (red is 0, so 1 - 0).
    CHECK(channelOf(*orm, Channel::G) != 255);
}

TEST_CASE("a bright coloured specular is metal, a dim grey one is not", "[pbr_bake]") {
    const Texture gold = solid(220, 170, 80, 255);
    const Texture dimGrey = solid(30, 30, 30, 255);

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &gold;
    const std::optional<Texture> metal = pbr::BakeOrm(recipe);
    recipe.specular.texture = &dimGrey;
    const std::optional<Texture> dielectric = pbr::BakeOrm(recipe);

    REQUIRE(metal.has_value());
    REQUIRE(dielectric.has_value());
    CHECK(channelOf(*metal, Channel::B) > 240);
    CHECK(channelOf(*dielectric, Channel::B) == 0);
}

TEST_CASE("the specular is measured linear, not encoded", "[pbr_bake]") {
    // 0.30 encoded is 0.073 linear -- just under the 0.08 evidence floor. A
    // bake that measured the byte would call this a conductor.
    const u8 encoded = static_cast<u8>(std::lround(0.30f * 255.0f));
    const Texture greyish = solid(encoded, encoded, encoded, 255);

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &greyish;
    recipe.specular.srgb = true;
    const std::optional<Texture> linear = pbr::BakeOrm(recipe);

    recipe.specular.srgb = false;
    const std::optional<Texture> asEncoded = pbr::BakeOrm(recipe);

    REQUIRE(linear.has_value());
    REQUIRE(asEncoded.has_value());
    CHECK(channelOf(*linear, Channel::B) == 0);
    CHECK(channelOf(*asEncoded, Channel::B) > 0);
}

TEST_CASE("the environment mask halves the metalness", "[pbr_bake]") {
    const Texture gold = solid(220, 170, 80, 255);
    const Texture masked = solid(255, 0, 0, 0);

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &gold;
    const std::optional<Texture> unmasked = pbr::BakeOrm(recipe);
    recipe.environmentMask.texture = &masked;
    recipe.environmentMask.channel = Channel::R;
    const std::optional<Texture> reflective = pbr::BakeOrm(recipe);

    REQUIRE(unmasked.has_value());
    REQUIRE(reflective.has_value());
    const f32 ratio = static_cast<f32>(channelOf(*reflective, Channel::B)) /
                      static_cast<f32>(channelOf(*unmasked, Channel::B));
    CHECK(ratio == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("the team mask lands in alpha, from the channel the layer names", "[pbr_bake]") {
    const Texture specular = solid(0, 0, 0, 255);
    const Texture team = solid(10, 20, 200, 40);

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;
    recipe.teamMask.texture = &team;
    recipe.teamMask.channel = Channel::B; // `Adept_Golden`'s spelling

    // Blue is 200 and red is 10, so reading the wrong channel is the difference
    // between a team-coloured texel and a plain one. The value written is
    // coverage rather than the sample -- see the binary cases below.
    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(channelOf(*orm, Channel::A) == 255);

    recipe.teamMask.channel = Channel::R;
    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 0);
}

TEST_CASE("occlusion rides red", "[pbr_bake]") {
    const Texture specular = solid(0, 0, 0, 255);
    const Texture occlusion = solid(90, 0, 0, 0);

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;
    recipe.occlusion.texture = &occlusion;
    recipe.occlusion.channel = Channel::R;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(channelOf(*orm, Channel::R) == 90);
}

TEST_CASE("the output takes the largest source's size", "[pbr_bake]") {
    const Texture big = solid(0, 0, 0, 255, 64);
    const Texture small = solid(0, 128, 0, 0, 8);

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &big;
    recipe.gloss.texture = &small;
    recipe.gloss.channel = Channel::G;
    recipe.gloss.invert = true;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(orm->width() == 64);
    CHECK(orm->height() == 64);
    // The 8x8 gloss was resampled up rather than cropping the 64x64 specular.
    CHECK(channelOf(*orm, Channel::G) == 127);
}

TEST_CASE("the roughness fallback matches the stated curve", "[pbr_bake]") {
    for (u8 level : {u8(0), u8(40), u8(120), u8(200), u8(255)}) {
        const Texture specular = solid(level, level, level, 255);
        pbr::OrmRecipe recipe;
        recipe.specular.texture = &specular;
        const std::optional<Texture> orm = pbr::BakeOrm(recipe);
        REQUIRE(orm.has_value());

        const f32 lin = srgbToLinear(static_cast<f32>(level) / 255.0f);
        const f32 expected = 0.1f + 0.8f * std::pow(std::max(0.0f, 1.0f - lin), 0.7f);
        CHECK(static_cast<i32>(channelOf(*orm, Channel::G)) ==
              static_cast<i32>(std::lround(expected * 255.0f)));

        const f32 evidence = smoothstep(0.08f, 0.30f, lin) * 0.25f; // grey: no colour evidence
        CHECK(static_cast<i32>(channelOf(*orm, Channel::B)) ==
              static_cast<i32>(std::lround(evidence * 255.0f)));
    }
}

TEST_CASE("a DXT5nm normal moves x out of alpha", "[pbr_bake]") {
    // x = 0.8 in alpha, y = 0.3 in green, and red/blue carrying the padding
    // DXT5nm leaves behind.
    const Texture source = solid(255, 77, 255, 204);

    const std::optional<Texture> restated =
        pbr::ConvertNormalXInAlpha(source, pbr::NormalRestatement{});
    REQUIRE(restated.has_value());
    CHECK(channelOf(*restated, Channel::R) == 204); // x, from alpha
    CHECK(channelOf(*restated, Channel::G) == 77);  // y, unchanged
    CHECK(restated->kind() == TextureKind::Normal);
    CHECK_FALSE(restated->isSrgb());

    // z is reconstructed rather than left at the source's blue, so a mip
    // filter has a whole vector to renormalise.
    const f32 x = 2.0f * (204.0f / 255.0f) - 1.0f;
    const f32 y = 2.0f * (77.0f / 255.0f) - 1.0f;
    const f32 z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
    CHECK(static_cast<i32>(channelOf(*restated, Channel::B)) ==
          Catch::Approx(std::lround((z + 1.0f) * 0.5f * 255.0f)).margin(1));
}

TEST_CASE("swapping x and y is the other", "[pbr_bake]") {
    const Texture source = solid(255, 77, 255, 204);
    const std::optional<Texture> plain =
        pbr::ConvertNormalXInAlpha(source, pbr::NormalRestatement{});
    const std::optional<Texture> swapped =
        pbr::ConvertNormalXInAlpha(source, pbr::NormalRestatement{true, false});
    REQUIRE(plain.has_value());
    REQUIRE(swapped.has_value());
    CHECK(channelOf(*swapped, Channel::R) == channelOf(*plain, Channel::G));
    CHECK(channelOf(*swapped, Channel::G) == channelOf(*plain, Channel::R));
    // z is reconstructed from the components as written, so swapping two
    // magnitudes leaves it alone.
    CHECK(channelOf(*swapped, Channel::B) == channelOf(*plain, Channel::B));
}

TEST_CASE("the two team masks are combined by max", "[pbr_bake]") {
    // Neither mechanism alone owns the texel; their union does. Taking the
    // diffuse's weight alone would leave this one plain.
    const Texture specular = solid(0, 0, 0, 255);
    const Texture fromDiffuse = solid(0, 0, 0, 200); // 0.216 once inverted
    const Texture fromLayer = solid(0, 0, 160, 0);   // 0.627

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;
    recipe.teamMask.texture = &fromDiffuse;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 0);

    recipe.teamMaskAlt.texture = &fromLayer;
    recipe.teamMaskAlt.channel = Channel::B;
    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 255);
}

TEST_CASE("inverting y is the one thing the restatement chooses", "[pbr_bake]") {
    const Texture source = solid(255, 77, 255, 204);
    const std::optional<Texture> plain =
        pbr::ConvertNormalXInAlpha(source, pbr::NormalRestatement{});
    const std::optional<Texture> flipped =
        pbr::ConvertNormalXInAlpha(source, pbr::NormalRestatement{false, true});
    REQUIRE(plain.has_value());
    REQUIRE(flipped.has_value());
    CHECK(channelOf(*flipped, Channel::G) == 255 - channelOf(*plain, Channel::G));
    CHECK(channelOf(*flipped, Channel::R) == channelOf(*plain, Channel::R));
}

// ---------------------------------------------------------------------------
// The team-colour crossing: a replace restated as a modulate
// ---------------------------------------------------------------------------

TEST_CASE("the base colour is lightened where the team mask selects", "[pbr_bake]") {
    // StarCraft II art under a team mask is dark on purpose -- the engine was
    // going to replace it -- and Warcraft III multiplies instead, so the albedo
    // has to become the identity of a modulate where the mask is 1.
    const Texture diffuse = solid(26, 26, 26, 64); // a = 0.25, so the mask is 0.75
    pbr::TeamBaseColorRecipe recipe;
    recipe.baseColor.texture = &diffuse;
    recipe.teamMask.texture = &diffuse;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    const std::optional<Texture> base = pbr::BakeTeamBaseColor(recipe);
    REQUIRE(base.has_value());

    // lerp(white, diffuse, a) in LINEAR, then re-encoded.
    const f32 keep = 64.0f / 255.0f;
    const f32 expected = 1.0f + (srgbToLinear(26.0f / 255.0f) - 1.0f) * keep;
    const f32 encoded = expected <= 0.0031308f ? expected * 12.92f
                                               : 1.055f * std::pow(expected, 1.0f / 2.4f) - 0.055f;
    CHECK(static_cast<i32>(channelOf(*base, Channel::R)) ==
          Catch::Approx(std::lround(encoded * 255.0f)).margin(1));
    // The alpha was the mask, and Warcraft III reads that channel as coverage.
    CHECK(channelOf(*base, Channel::A) == 255);
}

TEST_CASE("a fully masked texel goes white, an unmasked one is untouched", "[pbr_bake]") {
    const Texture diffuse = solid(40, 80, 120, 255);
    pbr::TeamBaseColorRecipe recipe;
    recipe.baseColor.texture = &diffuse;

    SECTION("mask 0 leaves the art alone") {
        recipe.teamMask.constant = 0.0f;
        const std::optional<Texture> base = pbr::BakeTeamBaseColor(recipe);
        REQUIRE(base.has_value());
        CHECK(static_cast<i32>(channelOf(*base, Channel::R)) == Catch::Approx(40).margin(1));
        CHECK(static_cast<i32>(channelOf(*base, Channel::G)) == Catch::Approx(80).margin(1));
        CHECK(static_cast<i32>(channelOf(*base, Channel::B)) == Catch::Approx(120).margin(1));
    }
    SECTION("mask 1 is white, so the runtime swatch supplies the colour outright") {
        const Texture mask = solid(255, 255, 255, 255);
        recipe.teamMask.texture = &mask;
        const std::optional<Texture> base = pbr::BakeTeamBaseColor(recipe);
        REQUIRE(base.has_value());
        CHECK(channelOf(*base, Channel::R) == 255);
        CHECK(channelOf(*base, Channel::G) == 255);
        CHECK(channelOf(*base, Channel::B) == 255);
    }
}

TEST_CASE("no base colour is nothing to rewrite", "[pbr_bake]") {
    pbr::TeamBaseColorRecipe recipe;
    REQUIRE_FALSE(pbr::BakeTeamBaseColor(recipe).has_value());
}

TEST_CASE("the team mask comes out binary, not as the weight it went in as", "[pbr_bake]") {
    // Warcraft III's team mask is coverage: over a shipped footman's five ORMs
    // the alpha is 88-100% at 0..15 and 7-11% at 240..255, with about 1% in
    // between. Every source states a blend weight instead, so the bake decides.
    const Texture specular = solid(0, 0, 0, 255);
    const Texture diffuse = solid(26, 26, 26, 128); // dark art, a = 0.50

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;
    recipe.teamMask.texture = &diffuse;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;
    recipe.baseColor.texture = &diffuse;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(channelOf(*orm, Channel::A) == 255);
}

TEST_CASE("what decides the mask is how dark the paint under it is", "[pbr_bake]") {
    // The same weight lands on either side of the line depending on the albedo:
    // `lerp(team, diffuse, a)` shows mostly team colour when `1-a` outweighs
    // `a*luminance(diffuse)`. A fixed threshold on the raw weight cannot do
    // this, and it is the whole reason the albedo is an input to the ORM.
    const Texture specular = solid(0, 0, 0, 255);
    const Texture dark = solid(26, 26, 26, 200);     // weight 0.216 over luminance 0.010
    const Texture light = solid(230, 230, 230, 200); // the same weight over 0.777

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    SECTION("a fifth of a dark texel is the team's") {
        recipe.teamMask.texture = &dark;
        recipe.baseColor.texture = &dark;
        const std::optional<Texture> orm = pbr::BakeOrm(recipe);
        REQUIRE(orm.has_value());
        CHECK(channelOf(*orm, Channel::A) == 255);
    }
    SECTION("a fifth of a light one is not") {
        recipe.teamMask.texture = &light;
        recipe.baseColor.texture = &light;
        const std::optional<Texture> orm = pbr::BakeOrm(recipe);
        REQUIRE(orm.has_value());
        CHECK(channelOf(*orm, Channel::A) == 0);
    }
}

TEST_CASE("with no albedo to judge against, half the weight is the line", "[pbr_bake]") {
    const Texture specular = solid(0, 0, 0, 255);
    const Texture belowHalf = solid(0, 0, 0, 200); // weight 0.216
    const Texture aboveHalf = solid(0, 0, 0, 64);  // weight 0.749

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    recipe.teamMask.texture = &belowHalf;
    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 0);
    recipe.teamMask.texture = &aboveHalf;
    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 255);
}

TEST_CASE("the shading a binary mask cannot hold stays in the base colour", "[pbr_bake]") {
    // Two texels the mask calls equally team-coloured, whose source weights
    // differ: the ORM cannot tell them apart and must not try, and the albedo
    // has to carry the difference or the tint comes out flat.
    Texture diffuse = Texture::create2D(PixelFormat::RGBA8, 2, 1, 1);
    const std::span<u8> pixels = diffuse.mipData(0);
    for (std::size_t i = 0; i < 2; ++i) {
        pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 26;
    }
    pixels[3] = 0;   // a = 0, all team
    pixels[7] = 128; // a = 0.5, half

    pbr::TeamBaseColorRecipe recipe;
    recipe.baseColor.texture = &diffuse;
    recipe.teamMask.texture = &diffuse;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    const std::optional<Texture> base = pbr::BakeTeamBaseColor(recipe);
    REQUIRE(base.has_value());
    const std::span<const u8> out = base->mipData(0);
    CHECK(out[0] == 255);
    CHECK(out[4] < 255);
    CHECK(out[4] > 128);
}

TEST_CASE("the base colour never decides the map's size", "[pbr_bake]") {
    // A 512 colour map carrying the mask in its alpha must not drag a 64
    // specular map up with it -- the ORM follows the material maps.
    const Texture specular = solid(0, 0, 0, 255, 4);
    const Texture diffuse = solid(26, 26, 26, 128, 16);

    pbr::OrmRecipe recipe;
    recipe.specular.texture = &specular;
    recipe.baseColor.texture = &diffuse;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(orm->width() == 4);
    CHECK(orm->height() == 4);
}

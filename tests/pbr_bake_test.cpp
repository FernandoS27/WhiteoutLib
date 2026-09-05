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

/// A channel of a baked map as the number it stands for.
f32 unitOf(const Texture& texture, Channel channel) {
    return static_cast<f32>(channelOf(texture, channel)) / 255.0f;
}

/// A channel of a baked *colour* map, back in linear light.
f32 linearOf(const Texture& texture, Channel channel) {
    return srgbToLinear(unitOf(texture, channel));
}

} // namespace

TEST_CASE("an absent recipe bakes nothing", "[pbr_bake]") {
    pbr::OrmRecipe recipe;
    REQUIRE_FALSE(pbr::BakeOrm(recipe).has_value());
}

TEST_CASE("the neutral: no specular, no gloss, no masks", "[pbr_bake]") {
    const Texture specular = solid(0, 0, 0, 255);
    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &specular;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    // Occlusion defaults to unoccluded and a black specular reflects nothing,
    // so there is no metalness to pay for. The roughness is the material's own
    // highlight width and has nothing to do with the map being black: exponent
    // 20 is what the engine substitutes for an unauthored zero.
    CHECK(channelOf(*orm, Channel::R) == 255);
    CHECK(static_cast<i32>(channelOf(*orm, Channel::G)) ==
          static_cast<i32>(std::lround(pbr::RoughnessFromExponent(20.0f) * 255.0f)));
    CHECK(channelOf(*orm, Channel::B) == 0);
    CHECK(channelOf(*orm, Channel::A) == 0);
}

TEST_CASE("the roughness is the exponent's, not the map's", "[pbr_bake]") {
    // The bug this replaces read the roughness off how bright the specular map
    // was, which pinned every StarCraft II surface near 0.88 -- a surface with
    // no highlight for a normal map to move. Two maps, one exponent: the
    // roughness must not move.
    const Texture dim = solid(20, 20, 20, 255);
    const Texture bright = solid(220, 220, 220, 255);

    pbr::OrmRecipe recipe;
    recipe.reflectance.exponent = 80.0f;
    recipe.reflectance.specular.texture = &dim;
    const std::optional<Texture> a = pbr::BakeOrm(recipe);
    recipe.reflectance.specular.texture = &bright;
    const std::optional<Texture> b = pbr::BakeOrm(recipe);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(channelOf(*a, Channel::G) == channelOf(*b, Channel::G));

    // And two exponents, one map: it must.
    recipe.reflectance.exponent = 20.0f;
    const std::optional<Texture> rough = pbr::BakeOrm(recipe);
    REQUIRE(rough.has_value());
    CHECK(channelOf(*rough, Channel::G) > channelOf(*b, Channel::G));
    CHECK(static_cast<i32>(channelOf(*b, Channel::G)) ==
          static_cast<i32>(std::lround(pbr::RoughnessFromExponent(80.0f) * 255.0f)));
}

TEST_CASE("a gloss layer scales the exponent, squared", "[pbr_bake]") {
    // `psmaterial.fx:579` does `specPower *= g*g`. A gloss map is a
    // specularity multiplier, not a smoothness -- reading it as `1 - g` makes
    // the glossiest texels the roughest, which is backwards twice over.
    const Texture specular = solid(0, 0, 0, 255);
    const Texture gloss = solid(0, 0, 0, 128); // the shader reads the alpha

    pbr::OrmRecipe recipe;
    recipe.reflectance.exponent = 80.0f;
    recipe.reflectance.specular.texture = &specular;
    recipe.reflectance.exponentScale.texture = &gloss;
    recipe.reflectance.exponentScale.channel = Channel::A;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    const f32 g = 128.0f / 255.0f;
    CHECK(static_cast<i32>(channelOf(*orm, Channel::G)) ==
          Catch::Approx(std::lround(pbr::RoughnessFromExponent(80.0f * g * g) * 255.0f)).margin(1));
    // A quarter of the exponent is rougher than the material's own, and much
    // rougher than `1 - g` would have made it.
    CHECK(channelOf(*orm, Channel::G) >
          static_cast<u8>(std::lround(pbr::RoughnessFromExponent(80.0f) * 255.0f)));
}

TEST_CASE("the metal split gives back exactly what it takes", "[pbr_bake]") {
    // Reforged spends the albedo on the two lobes and has no dielectric F0:
    // `diffuseAlbedo = (1-m)*albedo`, `F0 = m*albedo`. The pair of bakes has to
    // solve both at once or the model comes back darker than it went in.
    const Texture diffuse = solid(96, 96, 96, 255);
    const Texture specular = solid(150, 150, 150, 255);

    pbr::SpecularReflectance reflectance;
    reflectance.exponent = 40.0f;
    reflectance.specular.texture = &specular;

    pbr::OrmRecipe orm;
    orm.reflectance = reflectance;
    orm.baseColor.texture = &diffuse;
    pbr::BaseColorRecipe base;
    base.reflectance = reflectance;
    base.baseColor.texture = &diffuse;

    const std::optional<Texture> map = pbr::BakeOrm(orm);
    const std::optional<Texture> albedo = pbr::BakeBaseColor(base);
    REQUIRE(map.has_value());
    REQUIRE(albedo.has_value());

    const f32 sourceDiffuse = srgbToLinear(96.0f / 255.0f);
    const f32 f0 = srgbToLinear(150.0f / 255.0f) * pbr::ReflectanceScale(40.0f, true);
    const f32 metal = unitOf(*map, Channel::B);
    const f32 written = linearOf(*albedo, Channel::R);

    CHECK(written == Catch::Approx(sourceDiffuse + f0).margin(0.004f));
    CHECK((1.0f - metal) * written == Catch::Approx(sourceDiffuse).margin(0.006f));
    CHECK(metal * written == Catch::Approx(f0).margin(0.006f));
    // And the albedo went UP. A converter that only writes the metalness takes
    // this out of the diffuse and gives nothing back.
    CHECK(channelOf(*albedo, Channel::R) > 96);
}

TEST_CASE("the specular is measured linear, not encoded", "[pbr_bake]") {
    const Texture greyish = solid(128, 128, 128, 255);
    const Texture diffuse = solid(96, 96, 96, 255);

    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &greyish;
    recipe.baseColor.texture = &diffuse;
    recipe.reflectance.specular.srgb = true;
    const std::optional<Texture> linear = pbr::BakeOrm(recipe);
    recipe.reflectance.specular.srgb = false;
    const std::optional<Texture> asEncoded = pbr::BakeOrm(recipe);

    REQUIRE(linear.has_value());
    REQUIRE(asEncoded.has_value());
    // 0.502 encoded is 0.216 linear: reading the byte more than doubles the
    // reflectance the source states.
    CHECK(channelOf(*asEncoded, Channel::B) > channelOf(*linear, Channel::B));
    const f32 f0 = srgbToLinear(128.0f / 255.0f) * pbr::ReflectanceScale(20.0f, true);
    const f32 albedo = srgbToLinear(96.0f / 255.0f);
    CHECK(unitOf(*linear, Channel::B) == Catch::Approx(f0 / (f0 + albedo)).margin(0.004f));
}

TEST_CASE("a splat of alpha is read encoded, because an sRGB view leaves it so",
          "[pbr_bake]") {
    // `SpecularMode::AlphaOnly` reads the layer's alpha, and an sRGB view
    // gamma-decodes RGB and never alpha. Decoding it here would understate the
    // reflectance by the same factor that once tilted every DXT5nm normal.
    const Texture specular = solid(0, 0, 0, 128);
    const Texture diffuse = solid(96, 96, 96, 255);

    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &specular;
    recipe.reflectance.specular.splat = Channel::A;
    recipe.baseColor.texture = &diffuse;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    const f32 f0 = (128.0f / 255.0f) * pbr::ReflectanceScale(20.0f, true);
    const f32 albedo = srgbToLinear(96.0f / 255.0f);
    CHECK(unitOf(*orm, Channel::B) == Catch::Approx(f0 / (f0 + albedo)).margin(0.004f));
}

TEST_CASE("the layer's own tint is part of the reflectance", "[pbr_bake]") {
    // A specular layer authored at x2 reflects twice as much, and the
    // environment layer that ships with the golden Adept carries its whole look
    // in an `add`. Dropping either is dropping the number the shader multiplies.
    const Texture specular = solid(128, 128, 128, 255);
    const Texture diffuse = solid(96, 96, 96, 255);

    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &specular;
    recipe.baseColor.texture = &diffuse;
    const std::optional<Texture> plain = pbr::BakeOrm(recipe);

    recipe.reflectance.specular.scale[0] = 2.0f;
    recipe.reflectance.specular.scale[1] = 2.0f;
    recipe.reflectance.specular.scale[2] = 2.0f;
    const std::optional<Texture> tinted = pbr::BakeOrm(recipe);

    recipe.reflectance.specular.scale[0] = 1.0f;
    recipe.reflectance.specular.scale[1] = 1.0f;
    recipe.reflectance.specular.scale[2] = 1.0f;
    recipe.reflectance.factor = 2.0f; // hdrSpecularMultiplier, the same lever
    const std::optional<Texture> boosted = pbr::BakeOrm(recipe);

    REQUIRE(plain.has_value());
    REQUIRE(tinted.has_value());
    REQUIRE(boosted.has_value());
    CHECK(channelOf(*tinted, Channel::B) > channelOf(*plain, Channel::B));
    CHECK(channelOf(*tinted, Channel::B) == channelOf(*boosted, Channel::B));
}

TEST_CASE("a StarCraft II specular white is a tenth of a mirror", "[pbr_bake]") {
    // The calibration the whole crossing rests on, and the reason it needs no
    // tuned constant: the engine's own energy dim is very nearly proportional
    // to the exponent, so it cancels the (n+8)/8pi normalisation and the scale
    // comes out flat across every exponent shipped content uses.
    CHECK(pbr::ReflectanceScale(20.0f, true) == Catch::Approx(0.078f).margin(0.01f));
    CHECK(pbr::ReflectanceScale(40.0f, true) == Catch::Approx(0.095f).margin(0.01f));
    CHECK(pbr::ReflectanceScale(80.0f, true) == Catch::Approx(0.092f).margin(0.01f));
    // Without the dim the same sample is a near-mirror, which is what the 76
    // materials in 13,091 that set `SimulateRoughness` are asking for.
    CHECK(pbr::ReflectanceScale(20.0f, false) > 0.8f);
}

TEST_CASE("the roughness curve is the lobe-width match", "[pbr_bake]") {
    for (f32 n : {1.0f, 20.0f, 40.0f, 80.0f, 512.0f}) {
        CHECK(pbr::RoughnessFromExponent(n) ==
              Catch::Approx(std::pow(2.0f / (n + 2.0f), 0.25f)).margin(1e-5f));
    }
    // 20 is 72.7% of the corpus and 0.55 is where it lands -- against the 0.88
    // the old brightness fallback produced for the same material.
    CHECK(pbr::RoughnessFromExponent(20.0f) == Catch::Approx(0.549f).margin(0.002f));
    CHECK(pbr::RoughnessFromExponent(80.0f) < pbr::RoughnessFromExponent(20.0f));
}

TEST_CASE("the team mask lands in alpha, from the channel the layer names", "[pbr_bake]") {
    const Texture specular = solid(0, 0, 0, 255);
    const Texture team = solid(10, 20, 200, 40);

    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &specular;
    recipe.teamMask.texture = &team;
    recipe.teamMask.channel = Channel::B; // `Adept_Golden`'s spelling

    // Blue is 200 and red is 10, so reading the wrong channel is the difference
    // between a team-coloured texel and a plain one.
    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(channelOf(*orm, Channel::A) == 200);

    recipe.teamMask.channel = Channel::R;
    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 10);
}

TEST_CASE("occlusion rides red", "[pbr_bake]") {
    const Texture specular = solid(0, 0, 0, 255);
    const Texture occlusion = solid(90, 0, 0, 0);

    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &specular;
    recipe.occlusion.texture = &occlusion;
    recipe.occlusion.channel = Channel::R;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(channelOf(*orm, Channel::R) == 90);
}

TEST_CASE("the output takes the largest source's size", "[pbr_bake]") {
    const Texture big = solid(0, 0, 0, 255, 64);
    const Texture small = solid(0, 0, 0, 128, 8);

    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &big;
    recipe.reflectance.exponentScale.texture = &small;
    recipe.reflectance.exponentScale.channel = Channel::A;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(orm->width() == 64);
    CHECK(orm->height() == 64);
    // The 8x8 gloss was resampled up rather than cropping the 64x64 specular.
    const f32 g = 128.0f / 255.0f;
    CHECK(static_cast<i32>(channelOf(*orm, Channel::G)) ==
          Catch::Approx(std::lround(pbr::RoughnessFromExponent(20.0f * g * g) * 255.0f)).margin(1));
}

TEST_CASE("a DXT5nm normal moves x out of alpha", "[pbr_bake]") {
    // x = 0.8 in alpha, y = 0.3 in green, and red/blue carrying the padding
    // DXT5nm leaves there. A converter that copied rgb would take the padding.
    const Texture source = solid(255, 77, 255, 204);
    const std::optional<Texture> moved =
        pbr::ConvertNormalXInAlpha(source, pbr::NormalRestatement{});
    REQUIRE(moved.has_value());
    CHECK(channelOf(*moved, Channel::R) == 204);
    CHECK(channelOf(*moved, Channel::G) == 77);
    CHECK(channelOf(*moved, Channel::A) == 255);
}

TEST_CASE("swapping x and y is the other", "[pbr_bake]") {
    const Texture source = solid(255, 77, 255, 204);
    const std::optional<Texture> moved =
        pbr::ConvertNormalXInAlpha(source, pbr::NormalRestatement{true, false});
    REQUIRE(moved.has_value());
    CHECK(channelOf(*moved, Channel::R) == 77);
    CHECK(channelOf(*moved, Channel::G) == 204);
}

TEST_CASE("the two team masks are combined by max", "[pbr_bake]") {
    // Neither mechanism alone owns the texel; their union does. Taking the
    // diffuse's weight alone would leave this one plain.
    const Texture specular = solid(0, 0, 0, 255);
    const Texture fromDiffuse = solid(0, 0, 0, 200); // 0.216 once inverted
    const Texture fromLayer = solid(0, 0, 160, 0);   // 0.627

    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &specular;
    recipe.teamMask.texture = &fromDiffuse;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 55);

    recipe.teamMaskAlt.texture = &fromLayer;
    recipe.teamMaskAlt.channel = Channel::B;
    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 160);
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
    pbr::BaseColorRecipe recipe;
    recipe.baseColor.texture = &diffuse;
    recipe.teamMask.texture = &diffuse;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    const std::optional<Texture> base = pbr::BakeBaseColor(recipe);
    REQUIRE(base.has_value());

    // lerp(white, diffuse, a) in LINEAR, then re-encoded.
    const f32 keep = 64.0f / 255.0f;
    const f32 expected = 1.0f + (srgbToLinear(26.0f / 255.0f) - 1.0f) * keep;
    CHECK(linearOf(*base, Channel::R) == Catch::Approx(expected).margin(0.004f));
    // The alpha was the mask, and Warcraft III reads that channel as coverage.
    CHECK(channelOf(*base, Channel::A) == 255);
}

TEST_CASE("a fully masked texel goes white, an unmasked one is untouched", "[pbr_bake]") {
    const Texture diffuse = solid(40, 80, 120, 255);
    pbr::BaseColorRecipe recipe;
    recipe.baseColor.texture = &diffuse;

    SECTION("mask 0 and no specular leaves the art alone") {
        recipe.teamMask.constant = 0.0f;
        const std::optional<Texture> base = pbr::BakeBaseColor(recipe);
        REQUIRE(base.has_value());
        CHECK(static_cast<i32>(channelOf(*base, Channel::R)) == Catch::Approx(40).margin(1));
        CHECK(static_cast<i32>(channelOf(*base, Channel::G)) == Catch::Approx(80).margin(1));
        CHECK(static_cast<i32>(channelOf(*base, Channel::B)) == Catch::Approx(120).margin(1));
    }
    SECTION("mask 1 is white, so the runtime swatch supplies the colour outright") {
        const Texture mask = solid(255, 255, 255, 255);
        recipe.teamMask.texture = &mask;
        const std::optional<Texture> base = pbr::BakeBaseColor(recipe);
        REQUIRE(base.has_value());
        CHECK(channelOf(*base, Channel::R) == 255);
        CHECK(channelOf(*base, Channel::G) == 255);
        CHECK(channelOf(*base, Channel::B) == 255);
    }
}

TEST_CASE("no base colour is nothing to rewrite", "[pbr_bake]") {
    pbr::BaseColorRecipe recipe;
    REQUIRE_FALSE(pbr::BakeBaseColor(recipe).has_value());
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
    recipe.reflectance.specular.texture = &specular;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    SECTION("a fifth of a dark texel is nearly all the team's") {
        recipe.teamMask.texture = &dark;
        recipe.baseColor.texture = &dark;
        const std::optional<Texture> orm = pbr::BakeOrm(recipe);
        REQUIRE(orm.has_value());
        CHECK(channelOf(*orm, Channel::A) > 230);
    }
    SECTION("a fifth of a light one is barely any of it") {
        recipe.teamMask.texture = &light;
        recipe.baseColor.texture = &light;
        const std::optional<Texture> orm = pbr::BakeOrm(recipe);
        REQUIRE(orm.has_value());
        CHECK(channelOf(*orm, Channel::A) < 100);
    }
}

TEST_CASE("with no albedo to judge against, the weight goes through as it came",
          "[pbr_bake]") {
    // The sharpening is a judgement about the paint under the mask, so without
    // the paint there is nothing to judge and the source's own weight stands.
    const Texture specular = solid(0, 0, 0, 255);
    const Texture weighted = solid(0, 0, 0, 200); // weight 0.216

    pbr::OrmRecipe recipe;
    recipe.reflectance.specular.texture = &specular;
    recipe.teamMask.texture = &weighted;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    CHECK(channelOf(*pbr::BakeOrm(recipe), Channel::A) == 55);
}

TEST_CASE("the shading a mask cannot hold stays in the base colour", "[pbr_bake]") {
    // Two texels the mask calls team-coloured, whose source weights differ: the
    // albedo has to carry the difference or the tint comes out flat.
    Texture diffuse = Texture::create2D(PixelFormat::RGBA8, 2, 1, 1);
    const std::span<u8> pixels = diffuse.mipData(0);
    for (std::size_t i = 0; i < 2; ++i) {
        pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 26;
    }
    pixels[3] = 0;   // a = 0, all team
    pixels[7] = 128; // a = 0.5, half

    pbr::BaseColorRecipe recipe;
    recipe.baseColor.texture = &diffuse;
    recipe.teamMask.texture = &diffuse;
    recipe.teamMask.channel = Channel::A;
    recipe.teamMask.invert = true;

    const std::optional<Texture> base = pbr::BakeBaseColor(recipe);
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
    recipe.reflectance.specular.texture = &specular;
    recipe.baseColor.texture = &diffuse;

    const std::optional<Texture> orm = pbr::BakeOrm(recipe);
    REQUIRE(orm.has_value());
    CHECK(orm->width() == 4);
    CHECK(orm->height() == 4);
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/pbr_bake.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace whiteout::textures::pbr {

namespace {

/// One source, decoded to RGBA8 mip 0 and nothing else. The bake reads every
/// texel of every source once per output texel, so decoding on the fly would
/// pay for a BCn block decode four times over.
struct Plane {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;

    bool empty() const {
        return width == 0 || height == 0 || rgba.empty();
    }
};

Plane decode(const Texture* texture) {
    Plane plane;
    if (texture == nullptr || texture->width() == 0 || texture->height() == 0) {
        return plane;
    }
    const Texture rgba8 = texture->copyAsFormat(PixelFormat::RGBA8);
    const std::span<const u8> pixels = rgba8.mipData(0);
    if (pixels.empty()) {
        return plane;
    }
    plane.width = rgba8.width();
    plane.height = rgba8.height();
    plane.rgba.assign(pixels.begin(), pixels.end());
    return plane;
}

/// Bilinear, clamped at the edges, in normalised coordinates so a source of any
/// size answers for any output size. Returns [0,1] per channel.
void sample(const Plane& plane, f32 u, f32 v, f32 out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (plane.empty()) {
        return;
    }
    const f32 x = u * static_cast<f32>(plane.width) - 0.5f;
    const f32 y = v * static_cast<f32>(plane.height) - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(x));
    const i32 y0 = static_cast<i32>(std::floor(y));
    const f32 fx = x - static_cast<f32>(x0);
    const f32 fy = y - static_cast<f32>(y0);

    const auto clampX = [&](i32 value) {
        return std::clamp(value, 0, static_cast<i32>(plane.width) - 1);
    };
    const auto clampY = [&](i32 value) {
        return std::clamp(value, 0, static_cast<i32>(plane.height) - 1);
    };
    const i32 xs[2] = {clampX(x0), clampX(x0 + 1)};
    const i32 ys[2] = {clampY(y0), clampY(y0 + 1)};
    const f32 wx[2] = {1.0f - fx, fx};
    const f32 wy[2] = {1.0f - fy, fy};

    for (i32 j = 0; j < 2; ++j) {
        for (i32 i = 0; i < 2; ++i) {
            const std::size_t offset =
                (static_cast<std::size_t>(ys[j]) * plane.width + static_cast<std::size_t>(xs[i])) *
                4;
            const f32 weight = wx[i] * wy[j];
            for (i32 c = 0; c < 4; ++c) {
                out[c] += weight *
                          static_cast<f32>(plane.rgba[offset + static_cast<std::size_t>(c)]) /
                          255.0f;
            }
        }
    }
}

f32 srgbToLinear(f32 value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

f32 linearToSrgb(f32 value) {
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

f32 luminance(const f32 rgb[3]) {
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

/// A colour term, resolved against its plane or its constant, and decoded.
void colorAt(const ColorInput& input, const Plane& plane, f32 u, f32 v, f32 out[3]) {
    f32 texel[4] = {input.constant[0], input.constant[1], input.constant[2], 0.0f};
    if (input.present() && !plane.empty()) {
        sample(plane, u, v, texel);
    }
    if (input.srgb) {
        for (i32 c = 0; c < 3; ++c) {
            texel[c] = srgbToLinear(texel[c]);
        }
    }
    // After the decode and never before: an sRGB *view* gamma-decodes RGB and
    // leaves alpha alone, so a splat of alpha carries the encoded byte. Taking
    // the splat first would de-gamma a channel the sampler never touched.
    const f32 splat =
        input.splat.has_value() ? texel[static_cast<u32>(*input.splat)] : 0.0f;
    for (i32 c = 0; c < 3; ++c) {
        const f32 value = input.splat.has_value() ? splat : texel[c];
        out[c] = value * input.scale[c] + input.bias;
    }
}

/// Like `colorAt`, but hands back the sample's ALPHA too (raw — an sRGB view
/// never decodes alpha), for the ops that weight by it.
f32 colorWithAlphaAt(const ColorInput& input, const Plane& plane, f32 u, f32 v, f32 out[3]) {
    colorAt(input, plane, u, v, out);
    if (!input.present() || plane.empty()) {
        return 1.0f;
    }
    f32 texel[4];
    sample(plane, u, v, texel);
    return texel[3];
}

/// `CombineLayerColor`, the albedo half: fold @p decal into @p albedo in
/// linear light. The op vocabulary is M3's `layerBlendMode`.
void foldDecal(const ColorInput& decal, const Plane& plane, DecalOp op, f32 u, f32 v,
               f32 albedo[3]) {
    if (!decal.present() || plane.empty()) {
        return;
    }
    f32 d[3];
    const f32 a = colorWithAlphaAt(decal, plane, u, v, d);
    switch (op) {
    case DecalOp::Mod:
        for (i32 c = 0; c < 3; ++c) {
            albedo[c] *= d[c];
        }
        break;
    case DecalOp::Mod2x:
        for (i32 c = 0; c < 3; ++c) {
            albedo[c] *= d[c] * 2.0f;
        }
        break;
    case DecalOp::AddScaled:
        for (i32 c = 0; c < 3; ++c) {
            albedo[c] += d[c] * a;
        }
        break;
    case DecalOp::Add:
        for (i32 c = 0; c < 3; ++c) {
            albedo[c] += d[c];
        }
        break;
    case DecalOp::Lerp:
        for (i32 c = 0; c < 3; ++c) {
            albedo[c] += (d[c] - albedo[c]) * a;
        }
        break;
    }
}

u8 quantise(f32 value) {
    return static_cast<u8>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

/// A scalar term, resolved against its plane or its constant, then run through
/// the layer pipeline in the source shader's order: `* scale`, invert,
/// `* postScale + bias`.
f32 scalarAt(const ScalarInput& input, const Plane& plane, f32 u, f32 v) {
    f32 value = input.constant;
    if (input.present() && !plane.empty()) {
        f32 texel[4];
        sample(plane, u, v, texel);
        if (input.srgb) {
            for (i32 c = 0; c < 3; ++c) {
                texel[c] = srgbToLinear(texel[c]);
            }
        }
        value = input.luminance ? luminance(texel) : texel[static_cast<u32>(input.channel)];
        if (input.alphaWeighted) {
            value *= texel[3];
        }
    }
    value *= input.scale;
    if (input.invert) {
        value = 1.0f - value;
    }
    return value * input.postScale + input.bias;
}

/// The union of the two team mechanisms. One material can carry both, and a
/// single packed channel has to hold the union.
f32 teamAt(const ScalarInput& primary, const Plane& primaryPlane, const ScalarInput& secondary,
           const Plane& secondaryPlane, f32 u, f32 v) {
    return std::max(scalarAt(primary, primaryPlane, u, v),
                    scalarAt(secondary, secondaryPlane, u, v));
}

/// `lerp(teamColor, albedo, 1 - team)`, written so the identity at `team == 0`
/// is exact. This is the albedo both bakes reason about: the ORM's metalness
/// splits it and the base colour writes it. The colour is white unless the
/// target has no team slot to multiply one in.
void teamAlbedoAt(const f32 albedo[3], f32 team, const f32 teamColor[3], f32 out[3]) {
    const f32 keep = 1.0f - team;
    for (i32 c = 0; c < 3; ++c) {
        out[c] = teamColor[c] + (albedo[c] - teamColor[c]) * keep;
    }
}

/// Everything the two bakes have to agree on about the specular, resolved once
/// per texel: the reflectance the source states and the exponent behind it.
struct Reflectance {
    f32 f0[3] = {0.0f, 0.0f, 0.0f};
    f32 exponent = 20.0f;
    f32 roughness = 0.5f;
    /// The envio term's per-texel value — already in @ref f0 unless
    /// @ref envModulates, when it still wants the albedo's colour.
    f32 env = 0.0f;
    bool envModulates = false;
};

/// How much brighter the albedo has to be for `(1 - m) * albedo` to come back
/// out as the source's diffuse, and the `m` that goes with it.
///
/// Scaling the albedo rather than adding the reflectance to it is what makes
/// the identity hold **per channel** and not only in luminance: with
/// `gain = (D + S) / D` and `m = S / (D + S)` on luminances, `(1-m) * gain == 1`
/// exactly, so every channel of the diffuse survives untouched. Adding `F0`
/// component-wise instead reproduces the same luminance and drifts the hue
/// toward the specular's — a Reaper whose specular map is blue came back blue
/// in the shadows, where StarCraft II shows none of it.
///
/// The specular's own hue is lost either way: Reforged reads `F0 = m * albedo`,
/// so its highlight is the albedo's colour whatever we write. Given the choice,
/// the diffuse is the half worth keeping.
struct MetalSplit {
    f32 gain = 1.0f;
    f32 metallic = 0.0f;
};

MetalSplit metalSplit(f32 albedoLum, f32 specLum) {
    MetalSplit split;
    const f32 total = albedoLum + specLum;
    if (total <= 1e-6f) {
        return split;
    }
    split.metallic = specLum / total;
    // Below this the art is black and Reforged cannot reflect anything off it
    // — `F0 = m * albedo` is zero however large `m` grows — so the caller adds
    // the reflectance outright instead of scaling by a number that runs away.
    split.gain = albedoLum > 1e-4f ? total / albedoLum : 0.0f;
    return split;
}

Reflectance reflectanceAt(const SpecularReflectance& source, const Plane& specular,
                          const Plane& exponentScale, const Plane& envMask, f32 u, f32 v) {
    Reflectance out;
    const f32 g = scalarAt(source.exponentScale, exponentScale, u, v);
    // Under SimulateRoughness the gloss is perceptual — the engine blurs the
    // reflection by `1 - g` and Reforged picks its mip by the roughness — so
    // the written width is `1 - g`. The F0 keeps the exponent's width: the
    // source's highlight is narrower than its gloss says, and matching its
    // peak at the perceptual width over-drives F0 past 1 on rough texels.
    const bool perceptualGloss = source.simulateRoughness && source.exponentScale.present();
    out.exponent = std::max(1.0f, source.exponent * g * g);
    out.roughness = perceptualGloss ? std::clamp(1.0f - g, 0.0f, 1.0f)
                                    : RoughnessFromExponent(out.exponent);
    const bool anySpecular = source.specular.present() ||
                             source.specular.constant[0] != 0.0f ||
                             source.specular.constant[1] != 0.0f ||
                             source.specular.constant[2] != 0.0f || source.specular.bias != 0.0f;
    if (anySpecular) {
        f32 spec[3];
        colorAt(source.specular, specular, u, v, spec);
        const f32 scale = source.factor * ReflectanceScale(out.exponent, source.energyConserving);
        for (i32 c = 0; c < 3; ++c) {
            out.f0[c] = std::max(0.0f, spec[c]) * scale;
        }
    }
    // The envio layer, as an F0 bump. Grey on purpose: Reforged reads
    // F0 = m * albedo, so a hue here could not survive anyway. A modulated
    // one waits for the albedo (`foldModulatedEnv`). The mask is not clamped
    // above 1 — the league skins bias theirs past it, and a reflection
    // brighter than its cube is what the source drew.
    if (source.envReflectance > 0.0f) {
        out.env = source.envReflectance * std::max(0.0f, scalarAt(source.envMask, envMask, u, v));
        out.envModulates = source.envModulates;
        if (!out.envModulates) {
            for (i32 c = 0; c < 3; ++c) {
                out.f0[c] += out.env;
            }
        }
        // A live reflection reads sharper than the material's flat width,
        // exactly where its mask says so — unless the gloss already graded it.
        if (!perceptualGloss && out.env > 1e-3f) {
            out.roughness = std::min(out.roughness, source.envRoughnessCap);
        }
    }
    return out;
}

/// `lit * cube * mask` is nothing but a reflection in the surface's own
/// colour — a metal, whose albedo IS its reflectance. The lit colour the
/// engine multiplies holds its direct specular too, but that term stays out:
/// the league skins pair an exponent of 3 with an HDR multiplier of 5, a
/// Blinn lobe no F0 below 1 can say, and folding it in only saturated the
/// metal to white — the hue is the one thing a metal keeps.
void modulatedAlbedo(const Reflectance& reflectance, const f32 albedo[3], f32 out[3]) {
    for (i32 c = 0; c < 3; ++c) {
        out[c] = std::max(0.0f, albedo[c]) * reflectance.env;
    }
}

} // namespace

f32 RoughnessFromExponent(f32 exponent) {
    const f32 n = std::max(1.0f, exponent);
    // alpha = sqrt(2 / (n + 2)); Reforged's `ggxNDF` squares what it is handed
    // before using it as alpha, so the map holds sqrt(alpha).
    return std::clamp(std::pow(2.0f / (n + 2.0f), 0.25f), 0.0f, 1.0f);
}

f32 ExponentFromRoughness(f32 roughness) {
    // Clamped at the rough end so a painted-white roughness cannot demand an
    // exponent below Blinn-Phong's floor, and at the glossy end so a black
    // texel does not explode the exponent past anything a `.m3` states.
    const f32 r = std::clamp(roughness, 0.05f, 1.0f);
    const f32 alpha2 = r * r * r * r;
    return std::clamp(2.0f / alpha2 - 2.0f, 1.0f, 4096.0f);
}

f32 GlossFromRoughness(f32 roughness) {
    return 1.0f - std::clamp(roughness, 0.0f, 1.0f);
}

f32 GlossCeilingExponent(f32 roughness) {
    const f32 gloss = std::max(GlossFromRoughness(roughness), 0.05f);
    return std::clamp(ExponentFromRoughness(roughness) / (gloss * gloss), 20.0f, 2048.0f);
}

f32 ReflectanceScale(f32 exponent, bool energyConserving) {
    const f32 n = std::max(1.0f, exponent);
    f32 dim = 1.0f;
    if (energyConserving) {
        // psmaterial.fx:203 FakeEnergyConservingSpec, verbatim, on the clamp
        // the engine applies (CMaterial_ApplyForDraw).
        const f32 p = std::clamp(n, 1.0f, 512.0f);
        dim = std::clamp(-0.000004444f * p * p + 0.004333f * p + 0.0020834f, 0.0f, 1.0f);
    }
    // Peak-referenced, not energy-referenced, and that is a MEASUREMENT.
    // Reforged's `finalColor = PI * accum` multiplies the specular too, so the
    // rendered direct lobe is pi*D*F*V and at its peak pi*D*V = (n+2)/8 under
    // the matched width alpha^2 = 2/(n+2). The gray-sphere harness (a white-
    // specular sphere at n = 20/40/80 through both pipelines, 2026-09-05) put
    // the old energy calibration (8*pi/(n+8)) at 1.8/1.8/1.5x native at the
    // highlight's peak and 3.5/4.2/4.5x integrated; the geometric mean of the
    // two errors — 2.53/2.76/2.60 — is the ratio between the two references,
    // pi*(n+2)/(n+8), to within 11%. Dividing by it lands the peak at ~0.75x
    // and the energy at ~1.5x, balanced about 1 with one constant.
    const f32 normalisation = 8.0f / (n + 2.0f);
    return dim * normalisation;
}

std::optional<Texture> BakeOrm(const OrmRecipe& recipe) {
    const Plane specular = decode(recipe.reflectance.specular.texture);
    const Plane exponentScale = decode(recipe.reflectance.exponentScale.texture);
    const Plane envMask = decode(recipe.reflectance.envMask.texture);
    const Plane occlusion = decode(recipe.occlusion.texture);
    const Plane teamMask = decode(recipe.teamMask.texture);
    const Plane teamMaskAlt = decode(recipe.teamMaskAlt.texture);
    const Plane baseColor = decode(recipe.baseColor.texture);
    const Plane decal = decode(recipe.decal.texture);

    u32 width = recipe.width;
    u32 height = recipe.height;
    if (width == 0 || height == 0) {
        for (const Plane* plane :
             {&specular, &exponentScale, &envMask, &occlusion, &teamMask, &teamMaskAlt}) {
            width = std::max(width, plane->width);
            height = std::max(height, plane->height);
        }
    }
    if (width == 0 || height == 0) {
        // No material map names a size — but a CONSTANT term can still need a
        // map. The live case is a whole-surface `TeamColor*Add` (an RGB-select
        // team layer is a constant 1): with no ORM at all the runtime reads
        // the stock map's alpha of 0 and the unit is tinted nowhere. Four
        // texels say a constant. The base colour still never decides a size —
        // that rule is about not upscaling to the 2048 colour map.
        const bool constantSignal =
            recipe.teamMask.constant > 0.0f || recipe.teamMaskAlt.constant > 0.0f ||
            recipe.reflectance.specular.constant[0] > 0.0f ||
            recipe.reflectance.specular.constant[1] > 0.0f ||
            recipe.reflectance.specular.constant[2] > 0.0f ||
            recipe.reflectance.specular.bias > 0.0f || recipe.reflectance.envReflectance > 0.0f;
        if (!constantSignal) {
            return std::nullopt;
        }
        width = height = 4;
    }

    Texture out = Texture::create2D(PixelFormat::RGBA8, width, height, 1);
    out.setSrgb(false);
    // Per-channel kinds, because one filter is wrong for all four: roughness
    // wants the variance-preserving kernel (filter r^2 and take the root), a
    // coverage mask wants a box, and occlusion and metalness want a mean.
    out.setKind(TextureKind::Multikind);
    out.setChannelKind(Channel::R, TextureKind::AmbientOcclusion);
    out.setChannelKind(Channel::G, TextureKind::Roughness);
    out.setChannelKind(Channel::B, TextureKind::Metalness);
    out.setChannelKind(Channel::A, TextureKind::AlphaMask);

    const std::span<u8> pixels = out.mipData(0);
    for (u32 y = 0; y < height; ++y) {
        const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
        for (u32 x = 0; x < width; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);

            const Reflectance reflectance =
                reflectanceAt(recipe.reflectance, specular, exponentScale, envMask, u, v);

            // The roughness is the material's own highlight width, not a
            // measurement of the map: StarCraft II varies the highlight's
            // strength per texel and its width only where a gloss layer says
            // so. Reading the map instead pinned every StarCraft II surface at
            // 0.88 and left nothing for a normal map to move.
            const f32 roughness = reflectance.roughness;

            f32 share = teamAt(recipe.teamMask, teamMask, recipe.teamMaskAlt, teamMaskAlt, u, v);

            // The metalness IS the specular knob — Reforged's shader has no
            // dielectric F0 and reads `F0 = metalness * albedo` — so the
            // question is what fraction of the albedo the highlight is owed.
            // `BakeBaseColor` puts that fraction back in, which is why the two
            // have to read the same team-lightened albedo.
            f32 metallic = 0.0f;
            if (recipe.baseColor.present() && !baseColor.empty()) {
                f32 albedo[3];
                colorAt(recipe.baseColor, baseColor, u, v, albedo);
                foldDecal(recipe.decal, decal, recipe.decalOp, u, v, albedo);
                f32 teamAlbedo[3];
                teamAlbedoAt(albedo, share, recipe.teamColor, teamAlbedo);
                if (reflectance.envModulates) {
                    f32 metal[3];
                    modulatedAlbedo(reflectance, teamAlbedo, metal);
                    metallic = luminance(metal) > 1e-6f ? 1.0f : 0.0f;
                } else {
                    metallic = metalSplit(std::max(0.0f, luminance(teamAlbedo)),
                                          luminance(reflectance.f0))
                                   .metallic;
                }

                // Warcraft III's team mask is coverage where StarCraft II's is
                // a weight, so the crossing is a decision, and the term that
                // decides it is how dark the art under the mask is.
                // `lerp(teamColour, diffuse, a)` shows mostly team colour when
                // `1-a` outweighs `a*luminance(diffuse)`. Over StarCraft II art
                // (luminance about 0.05) that fires at a raw weight near 0.05;
                // over a light surface it holds out for much more, which a
                // fixed threshold on the raw weight could not do.
                const f32 paint = (1.0f - share) * luminance(albedo) * 0.8f;
                share = share + paint > 1e-5f ? share / (share + paint) : 0.0f;
            }

            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
            pixels[offset + 0] = quantise(scalarAt(recipe.occlusion, occlusion, u, v));
            pixels[offset + 1] = quantise(roughness);
            pixels[offset + 2] = quantise(metallic);
            pixels[offset + 3] = quantise(share);
        }
    }
    return out;
}

std::optional<Texture> BakeBaseColor(const BaseColorRecipe& recipe) {
    const Plane baseColor = decode(recipe.baseColor.texture);
    if (baseColor.empty()) {
        return std::nullopt;
    }
    const Plane teamMask = decode(recipe.teamMask.texture);
    const Plane teamMaskAlt = decode(recipe.teamMaskAlt.texture);
    const Plane decal = decode(recipe.decal.texture);
    const Plane coverage1 = decode(recipe.coverage1.texture);
    const Plane coverage2 = decode(recipe.coverage2.texture);
    const Plane specular = decode(recipe.reflectance.specular.texture);
    const Plane exponentScale = decode(recipe.reflectance.exponentScale.texture);
    const Plane envMask = decode(recipe.reflectance.envMask.texture);

    // The cutout can out-resolve the paint: a coverage mask bigger than the
    // base colour would come back quantised to the albedo's grid, and a fixed
    // alpha test then cuts in blocks. The output follows the largest source.
    u32 width = baseColor.width;
    u32 height = baseColor.height;
    for (const Plane* plane : {&coverage1, &coverage2}) {
        width = std::max(width, plane->width);
        height = std::max(height, plane->height);
    }
    Texture out = Texture::create2D(PixelFormat::RGBA8, width, height, 1);
    out.setSrgb(recipe.baseColor.srgb);
    // Per-channel kinds, because the alpha is not colour: a cutout's mips must
    // preserve COVERAGE under the runtime's alpha test — a box-filtered binary
    // alpha converges on 127, which fails Warcraft III's fixed 0.75 ref at
    // every distant mip and erases the model from afar.
    out.setKind(TextureKind::Multikind);
    out.setChannelKind(Channel::R, TextureKind::Diffuse);
    out.setChannelKind(Channel::G, TextureKind::Diffuse);
    out.setChannelKind(Channel::B, TextureKind::Diffuse);
    out.setChannelKind(Channel::A, recipe.coverageCutoff > 0.0f ? TextureKind::BinaryMask
                                                                : TextureKind::AlphaMask);

    const std::span<u8> pixels = out.mipData(0);
    for (u32 y = 0; y < height; ++y) {
        const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
        for (u32 x = 0; x < width; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32 team =
                teamAt(recipe.teamMask, teamMask, recipe.teamMaskAlt, teamMaskAlt, u, v);

            f32 albedo[3];
            colorAt(recipe.baseColor, baseColor, u, v, albedo);
            foldDecal(recipe.decal, decal, recipe.decalOp, u, v, albedo);
            f32 mixed[3];
            teamAlbedoAt(albedo, team, recipe.teamColor, mixed);

            // The metalness beside this takes exactly the reflectance back out
            // again, so the diffuse response is unchanged and the highlight is
            // paid for out of the raise rather than out of the surface.
            const Reflectance reflectance =
                reflectanceAt(recipe.reflectance, specular, exponentScale, envMask, u, v);
            f32 written[3];
            if (reflectance.envModulates) {
                modulatedAlbedo(reflectance, mixed, written);
            } else {
                const MetalSplit split =
                    metalSplit(std::max(0.0f, luminance(mixed)), luminance(reflectance.f0));
                for (i32 c = 0; c < 3; ++c) {
                    written[c] = split.gain > 0.0f ? mixed[c] * split.gain
                                                   : mixed[c] + reflectance.f0[c];
                }
            }

            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
            for (i32 c = 0; c < 3; ++c) {
                const f32 value = std::clamp(written[c], 0.0f, 1.0f);
                pixels[offset + static_cast<std::size_t>(c)] =
                    quantise(recipe.baseColor.srgb ? linearToSrgb(value) : value);
            }
            // The composed coverage — the source's own `mask1.a * mask2.a`,
            // never the diffuse alpha, which is the team mask on every source
            // this reads. Both defaults are 1, so a maskless recipe is opaque.
            f32 coverage = scalarAt(recipe.coverage1, coverage1, u, v) *
                           scalarAt(recipe.coverage2, coverage2, u, v);
            if (recipe.coverageCutoff > 0.0f) {
                // An alpha-KEYED surface wants the test's result, not its
                // input: the target's own ref is fixed and need not agree.
                coverage = coverage >= recipe.coverageCutoff ? 1.0f : 0.0f;
            }
            pixels[offset + 3] = quantise(coverage);
        }
    }
    return out;
}

f32 TeamReplaceFromBlend(const f32 albedo[3], f32 weight, bool srgb, f32 out[3]) {
    f32 base[3];
    f32 peak = 0.0f;
    for (i32 c = 0; c < 3; ++c) {
        base[c] = std::clamp(albedo[c], 0.0f, 1.0f);
        if (srgb) {
            base[c] = srgbToLinear(base[c]);
        }
        peak = std::max(peak, base[c]);
    }
    // The hue's ramp is sqrt(w); the brightness under it is the art's own.
    const f32 onset = std::sqrt(std::clamp(weight, 0.0f, 1.0f));
    const f32 alpha = 1.0f - onset * peak;
    for (i32 c = 0; c < 3; ++c) {
        // Under a whole share the art is never seen; it stays as it came.
        f32 value = alpha > 1e-4f ? (1.0f - onset) * base[c] / alpha : base[c];
        value = std::clamp(value, 0.0f, 1.0f);
        out[c] = srgb ? linearToSrgb(value) : value;
    }
    return alpha;
}

void DiffuseFromMetalness(const f32 albedo[3], f32 metalness, bool srgb, f32 out[3]) {
    const f32 keep = 1.0f - std::clamp(metalness, 0.0f, 1.0f);
    for (i32 c = 0; c < 3; ++c) {
        const f32 value = std::clamp(albedo[c], 0.0f, 1.0f);
        out[c] = srgb ? linearToSrgb(srgbToLinear(value) * keep) : value * keep;
    }
}

std::optional<Texture> BakeEmissiveSum(const ColorInput& first, bool firstWeightByAlpha,
                                       const ColorInput& second, bool secondWeightByAlpha) {
    const Plane a = decode(first.texture);
    const Plane b = decode(second.texture);
    const u32 width = std::max(a.width, b.width);
    const u32 height = std::max(a.height, b.height);
    if (width == 0 || height == 0) {
        return std::nullopt;
    }
    Texture out = Texture::create2D(PixelFormat::RGBA8, width, height, 1);
    out.setSrgb(first.srgb || second.srgb);
    out.setKind(TextureKind::Emissive);
    const bool srgb = first.srgb || second.srgb;
    const std::span<u8> pixels = out.mipData(0);
    for (u32 y = 0; y < height; ++y) {
        const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
        for (u32 x = 0; x < width; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            f32 ca[3];
            const f32 aa = colorWithAlphaAt(first, a, u, v, ca);
            f32 cb[3];
            const f32 ab = colorWithAlphaAt(second, b, u, v, cb);
            const f32 wa = firstWeightByAlpha ? aa : 1.0f;
            const f32 wb = secondWeightByAlpha ? ab : 1.0f;
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
            for (i32 c = 0; c < 3; ++c) {
                const f32 value = std::clamp(ca[c] * wa + cb[c] * wb, 0.0f, 1.0f);
                pixels[offset + static_cast<std::size_t>(c)] =
                    quantise(srgb ? linearToSrgb(value) : value);
            }
            pixels[offset + 3] = 255;
        }
    }
    return out;
}

std::optional<Texture> ConvertNormalXInAlpha(const Texture& source,
                                             const NormalRestatement& options) {
    if (source.width() == 0 || source.height() == 0) {
        return std::nullopt;
    }
    Texture out = source.copyAsFormat(PixelFormat::RGBA8);
    const std::span<u8> pixels = out.mipData(0);
    if (pixels.empty()) {
        return std::nullopt;
    }
    // Only mip 0 survives: the source's own chain was filtered for the source's
    // packing, and this one is regenerated by the caller anyway.
    Texture moved = Texture::create2D(PixelFormat::RGBA8, out.width(), out.height(), 1);
    const std::span<u8> dst = moved.mipData(0);
    for (std::size_t i = 0; i + 3 < pixels.size() && i + 3 < dst.size(); i += 4) {
        const u8 sourceX = pixels[i + 3]; // alpha, where DXT5nm puts x
        const u8 sourceY = pixels[i + 1]; // green
        const u8 outX = options.swapXY ? sourceY : sourceX;
        const u8 outY = options.swapXY ? sourceX : sourceY;
        dst[i + 0] = outX;
        dst[i + 1] = options.invertY ? static_cast<u8>(255 - outY) : outY;
        dst[i + 2] = 0;
        dst[i + 3] = 255;
    }
    moved.setSrgb(false);
    moved.setKind(TextureKind::Normal);
    // Blue is z, so a mip filter has a whole vector to renormalise. An encode to
    // BC5 drops it again and the shader reconstructs it, which is where it came
    // from.
    moved.expandNormal(Channel::R, Channel::G, Channel::B);
    return moved;
}

} // namespace whiteout::textures::pbr

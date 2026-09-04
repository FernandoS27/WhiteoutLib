// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/pbr_bake.h>

#include <algorithm>
#include <cmath>
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
    out[0] = input.constant[0];
    out[1] = input.constant[1];
    out[2] = input.constant[2];
    if (input.present() && !plane.empty()) {
        f32 texel[4];
        sample(plane, u, v, texel);
        out[0] = texel[0];
        out[1] = texel[1];
        out[2] = texel[2];
    }
    if (input.srgb) {
        for (i32 c = 0; c < 3; ++c) {
            out[c] = srgbToLinear(out[c]);
        }
    }
}

f32 smoothstep(f32 edge0, f32 edge1, f32 x) {
    const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

u8 quantise(f32 value) {
    return static_cast<u8>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

/// A scalar term, resolved against its plane or its constant.
f32 scalarAt(const ScalarInput& input, const Plane& plane, f32 u, f32 v) {
    if (!input.present() || plane.empty()) {
        return input.invert ? 1.0f - input.constant : input.constant;
    }
    f32 texel[4];
    sample(plane, u, v, texel);
    const f32 value = texel[static_cast<u32>(input.channel)];
    return input.invert ? 1.0f - value : value;
}

/// The union of the two team mechanisms. One material can carry both, and a
/// single packed channel has to hold the union.
f32 teamAt(const ScalarInput& primary, const Plane& primaryPlane, const ScalarInput& secondary,
           const Plane& secondaryPlane, f32 u, f32 v) {
    return std::max(scalarAt(primary, primaryPlane, u, v),
                    scalarAt(secondary, secondaryPlane, u, v));
}

/// Above what share of a texel the team colour owns it outright.
///
/// Half, because the question a coverage mask answers is "is this texel team
/// colour or is it paint", and the honest place to draw that line is where one
/// stops being the larger part of what the source would have shown.
constexpr f32 kTeamCoverageThreshold = 0.5f;

} // namespace

std::optional<Texture> BakeOrm(const OrmRecipe& recipe) {
    const Plane specular = decode(recipe.specular.texture);
    const Plane gloss = decode(recipe.gloss.texture);
    const Plane occlusion = decode(recipe.occlusion.texture);
    const Plane environmentMask = decode(recipe.environmentMask.texture);
    const Plane teamMask = decode(recipe.teamMask.texture);
    const Plane teamMaskAlt = decode(recipe.teamMaskAlt.texture);
    const Plane baseColor = decode(recipe.baseColor.texture);

    u32 width = recipe.width;
    u32 height = recipe.height;
    if (width == 0 || height == 0) {
        for (const Plane* plane :
             {&specular, &gloss, &occlusion, &environmentMask, &teamMask, &teamMaskAlt}) {
            width = std::max(width, plane->width);
            height = std::max(height, plane->height);
        }
    }
    if (width == 0 || height == 0) {
        return std::nullopt;
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

            // `colorAt` de-gammas when the input says the samples are
            // display-referred, and they are on every source this reads. The
            // thresholds below are linear ones — 0.08 is a shade above a
            // dielectric's 0.04 F0 and 0.30 is well into conductor territory —
            // so measuring the encoded byte instead calls half the dielectrics
            // metal.
            f32 spec[3];
            colorAt(recipe.specular, specular, u, v, spec);

            const f32 specLum = luminance(spec);
            const f32 maxS = std::max(spec[0], std::max(spec[1], spec[2]));
            const f32 minS = std::min(spec[0], std::min(spec[1], spec[2]));
            const f32 saturation = maxS > 1e-5f ? (maxS - minS) / maxS : 0.0f;

            const f32 intensityEvidence = smoothstep(0.08f, 0.30f, specLum);
            const f32 colorEvidence = smoothstep(0.05f, 0.30f, saturation);
            const f32 metalEvidence = intensityEvidence * (0.25f + 0.75f * colorEvidence);

            const f32 environment = scalarAt(recipe.environmentMask, environmentMask, u, v);
            const f32 metallic = metalEvidence * (0.5f + 0.5f * (1.0f - environment));

            // A gloss layer the caller named but that would not decode falls
            // through to the specular fallback rather than to `1 - 0`, which is
            // a fully rough surface and reads as a bug in the source art.
            const bool hasGloss = recipe.gloss.present() && !gloss.empty();
            const f32 roughness =
                hasGloss ? scalarAt(recipe.gloss, gloss, u, v)
                         : 0.1f + 0.8f * std::pow(std::max(0.0f, 1.0f - specLum), 0.7f);

            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
            pixels[offset + 0] = quantise(scalarAt(recipe.occlusion, occlusion, u, v));
            pixels[offset + 1] = quantise(roughness);
            pixels[offset + 2] = quantise(metallic);
            // Warcraft III's team mask is COVERAGE, not a blend weight, and
            // shipped content says so: over a footman's five maps the alpha is
            // 88-100% at 0..15 and 7-11% at 240..255, with 0.4-1.3% in between
            // — an antialiased edge and nothing else. The shading of a
            // team-coloured surface lives in its base colour; this channel only
            // says where.
            //
            // StarCraft II's is a genuine weight, so the crossing is a
            // decision, and the term that decides it is how dark the art under
            // the mask is. `lerp(teamColour, diffuse, a)` shows mostly team
            // colour when `1-a` outweighs `a*luminance(diffuse)`, so that
            // comparison — the team's *relative* share, thresholded — is the
            // question "is this texel team colour or is it paint". Over
            // StarCraft II art (luminance about 0.05) it fires at a raw weight
            // near 0.05; over a light surface it holds out for much more, which
            // a fixed threshold on the raw weight could not do.
            f32 share = teamAt(recipe.teamMask, teamMask, recipe.teamMaskAlt, teamMaskAlt, u, v);
            if (recipe.baseColor.present() && !baseColor.empty()) {
                f32 albedo[3];
                colorAt(recipe.baseColor, baseColor, u, v, albedo);
                const f32 paint = (1.0f - share) * luminance(albedo);
                share = share + paint > 1e-5f ? share / (share + paint) : 0.0f;
            }
            pixels[offset + 3] = share > kTeamCoverageThreshold ? 255 : 0;
        }
    }
    return out;
}

std::optional<Texture> BakeTeamBaseColor(const TeamBaseColorRecipe& recipe) {
    const Plane baseColor = decode(recipe.baseColor.texture);
    if (baseColor.empty()) {
        return std::nullopt;
    }
    const Plane teamMask = decode(recipe.teamMask.texture);
    const Plane teamMaskAlt = decode(recipe.teamMaskAlt.texture);

    const u32 width = baseColor.width;
    const u32 height = baseColor.height;
    Texture out = Texture::create2D(PixelFormat::RGBA8, width, height, 1);
    out.setSrgb(recipe.baseColor.srgb);
    out.setKind(TextureKind::Diffuse);

    const std::span<u8> pixels = out.mipData(0);
    for (u32 y = 0; y < height; ++y) {
        const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
        for (u32 x = 0; x < width; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32 team =
                teamAt(recipe.teamMask, teamMask, recipe.teamMaskAlt, teamMaskAlt, u, v);
            const f32 keep = 1.0f - team;

            f32 albedo[3];
            colorAt(recipe.baseColor, baseColor, u, v, albedo);

            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
            for (i32 c = 0; c < 3; ++c) {
                // `lerp(white, albedo, keep)`, which is `a*diffuse + (1-a)`
                // written so the identity at `team == 0` is exact.
                const f32 mixed = 1.0f + (albedo[c] - 1.0f) * keep;
                pixels[offset + static_cast<std::size_t>(c)] =
                    quantise(recipe.baseColor.srgb ? linearToSrgb(mixed) : mixed);
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

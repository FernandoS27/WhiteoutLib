// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/environment_map.h>

#include <algorithm>
#include <cmath>
#include <span>

namespace whiteout::textures::env {

namespace {

constexpr f32 kPi = 3.14159265358979f;

/// Direction for texel (@p s, @p t) in [-1, 1] on cube @p face, DDS / D3D
/// face order and orientation. Not normalised.
void FaceDirection(u32 face, f32 s, f32 t, f32 out[3]) {
    switch (face) {
    case 0: out[0] = 1.0f;  out[1] = -t;    out[2] = -s;    break; // +X
    case 1: out[0] = -1.0f; out[1] = -t;    out[2] = s;     break; // -X
    case 2: out[0] = s;     out[1] = 1.0f;  out[2] = t;     break; // +Y
    case 3: out[0] = s;     out[1] = -1.0f; out[2] = -t;    break; // -Y
    case 4: out[0] = s;     out[1] = -t;    out[2] = 1.0f;  break; // +Z
    default: out[0] = -s;   out[1] = -t;    out[2] = -1.0f; break; // -Z
    }
}

/// Bilinear RGBA8 fetch; the longitude wraps, the latitude clamps at the poles.
void SampleBilinear(std::span<const u8> src, u32 w, u32 h, f32 u, f32 v, u8 out[4]) {
    const f32 x = u * static_cast<f32>(w) - 0.5f;
    const f32 y = std::clamp(v, 0.0f, 1.0f) * static_cast<f32>(h) - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(x));
    const i32 y0 = static_cast<i32>(std::floor(y));
    const f32 fx = x - static_cast<f32>(x0);
    const f32 fy = y - static_cast<f32>(y0);
    const i32 wi = static_cast<i32>(w);
    const i32 hi = static_cast<i32>(h);
    const i32 xa = ((x0 % wi) + wi) % wi;
    const i32 xb = (xa + 1) % wi;
    const i32 ya = std::clamp(y0, 0, hi - 1);
    const i32 yb = std::clamp(y0 + 1, 0, hi - 1);
    for (i32 c = 0; c < 4; ++c) {
        const f32 t00 = src[(static_cast<std::size_t>(ya) * w + static_cast<std::size_t>(xa)) * 4 + c];
        const f32 t10 = src[(static_cast<std::size_t>(ya) * w + static_cast<std::size_t>(xb)) * 4 + c];
        const f32 t01 = src[(static_cast<std::size_t>(yb) * w + static_cast<std::size_t>(xa)) * 4 + c];
        const f32 t11 = src[(static_cast<std::size_t>(yb) * w + static_cast<std::size_t>(xb)) * 4 + c];
        const f32 top = t00 + (t10 - t00) * fx;
        const f32 bottom = t01 + (t11 - t01) * fx;
        out[c] = static_cast<u8>(std::lround(std::clamp(top + (bottom - top) * fy, 0.0f, 255.0f)));
    }
}

} // namespace

std::optional<Texture> CubeFromPanorama(const Texture& panorama, u32 faceSize) {
    if (faceSize == 0 || panorama.width() == 0 || panorama.height() == 0) {
        return std::nullopt;
    }
    const Texture source = panorama.copyAsFormat(PixelFormat::RGBA8);
    if (source.width() == 0) {
        return std::nullopt;
    }
    const u32 w = source.width();
    const u32 h = source.height();
    const std::span<const u8> px = source.mipData(0);
    if (px.size() < static_cast<std::size_t>(w) * h * 4) {
        return std::nullopt;
    }

    Texture cube = Texture::createCube(PixelFormat::RGBA8, faceSize, 1);
    cube.setSrgb(source.isSrgb());
    for (u32 face = 0; face < 6; ++face) {
        const std::span<u8> out = cube.mipData(0, face);
        for (u32 y = 0; y < faceSize; ++y) {
            const f32 t = 2.0f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(faceSize) - 1.0f;
            for (u32 x = 0; x < faceSize; ++x) {
                const f32 s =
                    2.0f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(faceSize) - 1.0f;
                f32 d[3];
                FaceDirection(face, s, t, d);
                const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                d[0] /= len;
                d[1] /= len;
                d[2] /= len;
                // Longitude from +Y, latitude from the zenith: the centre
                // column faces +Y and the top row is straight up.
                const f32 u = 0.5f + std::atan2(d[0], d[1]) / (2.0f * kPi);
                const f32 v = 0.5f - std::asin(std::clamp(d[2], -1.0f, 1.0f)) / kPi;
                SampleBilinear(px, w, h, u, v,
                               &out[(static_cast<std::size_t>(y) * faceSize + x) * 4]);
            }
        }
    }
    return cube;
}

} // namespace whiteout::textures::env

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

/// The face direction @p d lands on and its (@p sc, @p tc) there, still over
/// @p major -- the inverse of @ref FaceDirection.
void FaceCoordinates(const f32 d[3], u32& face, f32& major, f32& sc, f32& tc) {
    const f32 ax = std::abs(d[0]);
    const f32 ay = std::abs(d[1]);
    const f32 az = std::abs(d[2]);
    if (ax >= ay && ax >= az) {
        face = d[0] > 0.0f ? 0 : 1;
        major = ax;
        sc = d[0] > 0.0f ? -d[2] : d[2];
        tc = -d[1];
    } else if (ay >= az) {
        face = d[1] > 0.0f ? 2 : 3;
        major = ay;
        sc = d[0];
        tc = d[1] > 0.0f ? d[2] : -d[2];
    } else {
        face = d[2] > 0.0f ? 4 : 5;
        major = az;
        sc = d[2] > 0.0f ? d[0] : -d[0];
        tc = -d[1];
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

std::optional<Texture> CubeFromCube(const Texture& source, u32 cubeIndex,
                                    const std::array<f32, 9>& sourceFromTarget,
                                    const std::array<u32, 6>& faceOrder) {
    const TextureType type = source.type();
    if ((type != TextureType::TextureCube && type != TextureType::TextureCubeArray) ||
        source.width() == 0 || cubeIndex >= std::max<u32>(source.arraySize(), 1)) {
        return std::nullopt;
    }
    for (const u32 layer : faceOrder) {
        if (layer > 5) {
            return std::nullopt;
        }
    }
    const Texture rgba = source.copyAsFormat(PixelFormat::RGBA8);
    const u32 size = rgba.width();
    if (size == 0) {
        return std::nullopt;
    }
    const u32 levels = std::max<u32>(rgba.mipCount(), 1);
    const std::array<f32, 9>& m = sourceFromTarget;

    Texture cube = Texture::createCube(PixelFormat::RGBA8, size, levels);
    cube.setSrgb(source.isSrgb());
    for (u32 level = 0; level < levels; ++level) {
        const u32 n = std::max<u32>(size >> level, 1);
        const f32 extent = static_cast<f32>(n);
        for (u32 face = 0; face < 6; ++face) {
            const std::span<u8> out = cube.mipData(level, face);
            for (u32 y = 0; y < n; ++y) {
                const f32 t = 2.0f * (static_cast<f32>(y) + 0.5f) / extent - 1.0f;
                for (u32 x = 0; x < n; ++x) {
                    const f32 s = 2.0f * (static_cast<f32>(x) + 0.5f) / extent - 1.0f;
                    f32 d[3];
                    FaceDirection(face, s, t, d);
                    const f32 p[3] = {m[0] * d[0] + m[1] * d[1] + m[2] * d[2],
                                      m[3] * d[0] + m[4] * d[1] + m[5] * d[2],
                                      m[6] * d[0] + m[7] * d[1] + m[8] * d[2]};
                    u32 from = 0;
                    f32 major = 0.0f;
                    f32 sc = 0.0f;
                    f32 tc = 0.0f;
                    FaceCoordinates(p, from, major, sc, tc);
                    if (major <= 0.0f) {
                        continue;
                    }
                    const f32 u =
                        std::clamp((sc / major + 1.0f) * 0.5f * extent, 0.0f, extent - 1.0f);
                    const f32 v =
                        std::clamp((tc / major + 1.0f) * 0.5f * extent, 0.0f, extent - 1.0f);
                    const std::span<const u8> src =
                        rgba.mipData(level, cubeIndex * 6 + faceOrder[from]);
                    const std::size_t at =
                        (static_cast<std::size_t>(v) * n + static_cast<std::size_t>(u)) * 4;
                    std::copy_n(&src[at], 4, &out[(static_cast<std::size_t>(y) * n + x) * 4]);
                }
            }
        }
    }
    return cube;
}

} // namespace whiteout::textures::env

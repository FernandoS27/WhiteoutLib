// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Decoders for the packed fields of a D3 Appearance vertex.
//
// FatVertex stores 44 bytes per vertex and only the position is plain float
// data.  The packing was recovered from three agreeing sources: the vertex
// declaration the engine builds at start-up (VertexDecl_BuildAll,
// 0x7100093DD0 in the Switch 2.6.2 build), the shipped ARB vertex programs,
// which carry the decode arithmetic literally, and the corpus itself.
//
//   +0   float3 position
//   +12  normal    3 unsigned bytes, n = b * 2/255 - 1   (4th byte always 0)
//   +16  color     R,G,B,A in memory order
//   +20  auxcolor  second colour channel
//   +24  texcoord0 two u16, coord = u16 / 512 - 64
//   +28  texcoord1
//   +32  tangent   packed like the normal, aligned with +dP/du
//   +36  binormal  packed like the normal, aligned with -dP/dv
//   +40  cloth vertex index, non-zero only when the SubObject carries cloth
//
// The engine's own shaders spell the two decodes out:
//   MUL R.xyz, vertex.attrib[2], 0.0078431377 ; ADD R.xyz, R, -1
//   MAD R.y, a8.x, 0.00390625, a8.y ; MUL R,0.5 ; ADD R,-64

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/sno/d3/native/types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace sno {
namespace d3 {
namespace native {

/// A packed unit vector: three unsigned bytes biased into [-1, 1].
inline Vector3f unpackVertexVector(u32 packed) {
    const auto ch = [packed](int i) {
        return static_cast<f32>((packed >> (8 * i)) & 0xFFu) * (2.0f / 255.0f) - 1.0f;
    };
    return {ch(0), ch(1), ch(2)};
}

/// A packed texture coordinate: two u16 in 8.8 fixed point, biased by 64.
inline Vector2f unpackTexCoord(u32 packed) {
    return {static_cast<f32>(packed & 0xFFFFu) / 512.0f - 64.0f,
            static_cast<f32>(packed >> 16) / 512.0f - 64.0f};
}

/// A vertex colour. The bytes are R, G, B, A in memory order.
struct VertexColor {
    u8 r{}, g{}, b{}, a{};
};

inline VertexColor unpackVertexColor(u32 packed) {
    return {static_cast<u8>(packed & 0xFFu), static_cast<u8>((packed >> 8) & 0xFFu),
            static_cast<u8>((packed >> 16) & 0xFFu), static_cast<u8>(packed >> 24)};
}

inline Vector3f vertexNormal(const FatVertex& v) {
    return unpackVertexVector(v.dwNormal);
}
inline Vector3f vertexTangent(const FatVertex& v) {
    return unpackVertexVector(v.dwTangent);
}
inline Vector3f vertexBinormal(const FatVertex& v) {
    return unpackVertexVector(v.dwBinormal);
}
inline Vector2f vertexTexCoord0(const FatVertex& v) {
    return unpackTexCoord(v.dwTexCoord0);
}
inline Vector2f vertexTexCoord1(const FatVertex& v) {
    return unpackTexCoord(v.dwTexCoord1);
}
inline VertexColor vertexColor(const FatVertex& v) {
    return unpackVertexColor(v.dwColor);
}
inline VertexColor vertexAuxColor(const FatVertex& v) {
    return unpackVertexColor(v.dwAuxColor);
}

/// SubObject.dwVertexFormat is a flag word. Only one bit is provable against
/// this build; see scripts/seed_d3_type_overrides.py for the evidence.
enum : u32 {
    kSubObjectHasClothIndex = 0x100u,   ///< exact over 191543 corpus SubObjects
    kSubObjectCompressedMask = 0x18000u,   ///< post-v260; == 0x8000 selects it
};

} // namespace native
} // namespace d3
} // namespace sno
} // namespace whiteout

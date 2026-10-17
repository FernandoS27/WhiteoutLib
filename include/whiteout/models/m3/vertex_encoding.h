// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file vertex_encoding.h
 * @brief How one value of an `.m3` vertex record becomes its bytes.
 *
 * The converter's vertex encoder writes every record with these, and so does a
 * renderer that patches a record in place, so the two cannot disagree: a
 * record patched with the values the converter read is the record it wrote.
 */

#include <algorithm>
#include <cstring>

#include <whiteout/vector_types.h>

namespace whiteout::m3 {

/// What the stock region pair (`uvScale` 16, `uvOffset` 0) reads one raw i16
/// UV unit as: `16 / 32767`, which is the `1 / 2048` every older region implies.
inline constexpr f32 kStockUvUnit = 2048.0f;

/// One axis of a basis vector as the UNORM byte the engine decodes `2 * v - 1`.
inline u8 EncodeUnitByte(f32 value) {
    const f32 mapped = (std::clamp(value, -1.0f, 1.0f) * 0.5f + 0.5f) * 255.0f;
    return static_cast<u8>(mapped + 0.5f);
}

/// The bitangent's handedness, as the normal's fourth byte carries it.
inline u8 EncodeHandedness(f32 tangentW) {
    return tangentW < 0.0f ? 0 : 255;
}

/// A normal's four bytes: the axes, then the tangent's handedness.
inline void EncodeNormal(u8* out, const Vector3f& normal, f32 tangentW) {
    out[0] = EncodeUnitByte(normal.x);
    out[1] = EncodeUnitByte(normal.y);
    out[2] = EncodeUnitByte(normal.z);
    out[3] = EncodeHandedness(tangentW);
}

/// A tangent's four bytes: the axes, then 255 -- its own fourth is unused.
inline void EncodeTangent(u8* out, const Vector3f& tangent) {
    out[0] = EncodeUnitByte(tangent.x);
    out[1] = EncodeUnitByte(tangent.y);
    out[2] = EncodeUnitByte(tangent.z);
    out[3] = 255;
}

/// A UV pair at the stock region pair: `uv * 2048`, truncated and clamped.
inline void EncodeStockUv(u8* out, const Vector2f& uv) {
    const i16 u = static_cast<i16>(std::clamp(uv.x * kStockUvUnit, -32768.0f, 32767.0f));
    const i16 v = static_cast<i16>(std::clamp(uv.y * kStockUvUnit, -32768.0f, 32767.0f));
    std::memcpy(out, &u, 2);
    std::memcpy(out + 2, &v, 2);
}

} // namespace whiteout::m3

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// What the Diablo III client makes of an appearance's collision shapes: the
// density table, the offline polytope cook and the two bone predicates its rig
// builders share (D3_PHYSICS_PLAN.md §3.5, §6). The viewer's rigs and the WEM
// import both read these, so the rules live in one place.
//
// A polytope shape carries a 96-byte header whose four array references are
// offsets into the WHOLE `.app` payload (past its 16-byte SNO preamble):
//
//     +32  Vector3f  the centroid
//     +44  u32       vertex count
//     +48  u32       face count
//     +52  u32       half-edge count
//     +56  f32       volume  <- the client rejects a cook below 4.4143e-6
//     +64  (i32 offset, i32 size)  vertices,    12 bytes each
//     +72  (i32 offset, i32 size)  face planes, 16 bytes each
//     +80  (i32 offset, i32 size)  half-edges,   4 bytes each
//     +88  (i32 offset, i32 size)  face -> first half-edge, 1 byte each
//
// A half-edge is {unrecovered, origin vertex, face, next around the face}.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/sno/d3/native/types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace sno {
namespace d3 {
namespace native {

/// `g_density(nBodyClass)`, the table at `0x7100E5DA60`: a shape's density is
/// this times its `flScaleX`. Anything outside 1..14 takes the switch's
/// default, which is also what a model with no `.phy` gets.
inline f32 densityForClass(i32 bodyClass) {
    static constexpr f32 kTable[14] = {3.75f,   1.5625f, 1.9375f, 1.9375f, 1.9375f, 2.5f,   2.5f,
                                       1.5625f, 3.75f,   3.75f,   1.5625f, 1.5625f, 2.5f,   2.5f};
    return bodyClass < 1 || bodyClass > 14 ? 1.9375f : kTable[bodyClass - 1];
}

/// A bone carrying a shape with `dwFlags` bit 0, at any lod: the anchored
/// builder makes its body kinematic.
inline bool boneIsAnchor(const BoneStructure& bone) {
    return std::any_of(bone.arCollisionShapes.begin(), bone.arCollisionShapes.end(),
                       [](const CollisionShape& s) { return (s.dwFlags & 1) != 0; });
}

/// A bone the bone-body builder makes dynamic at @p lod: a shape there with
/// mass. Every other bodied bone is static.
inline bool boneIsDynamic(const BoneStructure& bone, i32 lod) {
    return std::any_of(bone.arCollisionShapes.begin(), bone.arCollisionShapes.end(),
                       [lod](const CollisionShape& s) { return s.nLodIndex == lod && s.flScaleX > 0.0f; });
}

inline constexpr std::size_t kPolytopeHeaderBytes = 96;
/// The client builds no fixture from a cook below this volume, or a non-finite one.
inline constexpr f32 kPolytopeMinVolume = 4.4143e-6f;
/// The SNO preamble ahead of every payload, which the array offsets skip.
inline constexpr std::size_t kSnoPreambleBytes = 16;

struct Polytope {
    Vector3f centroid{0.0f, 0.0f, 0.0f};
    f32 volume = 0.0f;
    std::vector<Vector3f> points;
    /// Vertex index pairs, one pair per undirected edge.
    std::vector<u16> edges;
};

/// The cook at @p header (a shape's `arPolytopeData`) out of @p file, the whole
/// `.app` it came from; nothing where the client would build nothing.
inline std::optional<Polytope> readPolytope(std::span<const u8> header, std::span<const u8> file) {
    if (header.size() != kPolytopeHeaderBytes || file.size() <= kSnoPreambleBytes) {
        return std::nullopt;
    }
    const auto word = [&](std::size_t i) {
        i32 v = 0;
        std::memcpy(&v, header.data() + i * 4, 4);
        return v;
    };
    const auto real = [&](std::size_t i) {
        f32 v = 0.0f;
        std::memcpy(&v, header.data() + i * 4, 4);
        return v;
    };
    const auto nv = static_cast<u32>(word(11));
    const auto ne = static_cast<u32>(word(13));
    const f32 volume = real(14);
    if (!(volume >= kPolytopeMinVolume) || !std::isfinite(volume)) {
        return std::nullopt;
    }
    // A convex solid is at least a tetrahedron.
    if (nv < 4 || ne < 6) {
        return std::nullopt;
    }
    const std::span<const u8> payload = file.subspan(kSnoPreambleBytes);
    const auto arrayAt = [&](std::size_t refWord, u32 stride, u32 count) -> const u8* {
        const i32 off = word(refWord);
        const i32 size = word(refWord + 1);
        if (off <= 0 || size <= 0 || static_cast<u32>(size) != count * stride) {
            return nullptr;
        }
        const auto o = static_cast<std::size_t>(off);
        const auto n = static_cast<std::size_t>(size);
        if (o > payload.size() || n > payload.size() - o) {
            return nullptr;
        }
        return payload.data() + o;
    };
    const u8* pts = arrayAt(16, 12, nv);
    const u8* hes = arrayAt(20, 4, ne);
    if (pts == nullptr || hes == nullptr) {
        return std::nullopt;
    }
    Polytope out;
    out.centroid = {real(8), real(9), real(10)};
    out.volume = volume;
    out.points.resize(nv);
    for (u32 i = 0; i < nv; ++i) {
        f32 c[3];
        std::memcpy(c, pts + i * 12, 12);
        out.points[i] = {c[0], c[1], c[2]};
    }
    // `(origin[i], origin[next[i]])`, deduplicated: each edge is walked once per half.
    std::vector<u32> keys;
    keys.reserve(ne);
    for (u32 i = 0; i < ne; ++i) {
        const u32 a = hes[i * 4 + 1];
        const u32 next = hes[i * 4 + 3];
        if (next >= ne) {
            continue;
        }
        const u32 b = hes[next * 4 + 1];
        if (a >= nv || b >= nv || a == b) {
            continue;
        }
        keys.push_back((std::min(a, b) << 16) | std::max(a, b));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    out.edges.reserve(keys.size() * 2);
    for (const u32 k : keys) {
        out.edges.push_back(static_cast<u16>(k >> 16));
        out.edges.push_back(static_cast<u16>(k & 0xFFFFu));
    }
    return out;
}

} // namespace native
} // namespace d3
} // namespace sno
} // namespace whiteout

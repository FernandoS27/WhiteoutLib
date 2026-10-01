// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <array>
#include <cstdint>

#include "../../../common_types.h"
#include "../../../compatibility.h"
#include "../../../vector_types.h"

namespace whiteout {
namespace models {
namespace wow {
namespace wmo {

/// @brief A chunk id as the file spells it. WMO writes its four characters
///        reversed ("MVER" is stored `R E V M`), so the little-endian dword of
///        the bytes on disk is the name read most-significant byte first.
template <std::size_t N>
constexpr u32 makeTag(const char (&name)[N]) {
    static_assert(N == 5, "a chunk id is four characters");
    return (static_cast<u32>(static_cast<u8>(name[0])) << 24) |
           (static_cast<u32>(static_cast<u8>(name[1])) << 16) |
           (static_cast<u32>(static_cast<u8>(name[2])) << 8) |
           static_cast<u32>(static_cast<u8>(name[3]));
}

/// The only version 12.1 accepts; a root that says otherwise fails to load.
constexpr u32 kVersion = 17;

/// A `CImVector`: one byte per channel, stored B, G, R, A.
struct Color {
    u8 b = 0;
    u8 g = 0;
    u8 r = 0;
    u8 a = 0;

    constexpr bool operator==(const Color&) const = default;
};
static_assert(sizeof(Color) == 4);

/// A `CAaBox`: the two corners.
struct Box {
    Vector3f minimum{0.0f, 0.0f, 0.0f};
    Vector3f maximum{0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(Box) == 24);

/// A `C4Plane`: points `p` with `dot(normal, p) + distance == 0`.
struct Plane {
    Vector3f normal{0.0f, 0.0f, 1.0f};
    f32 distance = 0.0f;
};
static_assert(sizeof(Plane) == 16);

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout

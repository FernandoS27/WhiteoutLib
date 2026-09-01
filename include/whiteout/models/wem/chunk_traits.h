// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file chunk_traits.h
 * @brief Chunk identity for WEM v3 (design §11.1, §11.3).
 *
 * `ChunkTagTraits<T>` answers *what is this chunk* — its FourCC, the highest
 * version this build writes, and whether a run of them can be memcpy'd. It is
 * deliberately separate from `reflect(V&)` (§11.2), which answers *what is
 * inside one record*: a trivial type has no `reflect` and is still written as
 * one bulk run under its tag.
 *
 * The shape is v2's, unchanged — same primary template, same three members —
 * because §11.1's whole claim is that v3 keeps the container. What is new is
 * only where the specialisations come from: the native material blocks are
 * generated (§15.2) and their `max_version` is the schema lock's per-block
 * version, so a mirror whose shape moved cannot keep claiming the old version.
 *
 * P4 fills in the rest of the v3 registry alongside these.
 */

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace wem {

/// FourCC from a 4-character literal, little-endian — the byte order every
/// tag in the file is written in.
constexpr u32 kTag(const char (&s)[5]) {
    return static_cast<u32>(static_cast<u8>(s[0])) |
           (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(s[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(s[3])) << 24);
}

/// Primary template — specialised per chunk type. Left undefined on purpose:
/// asking for the tag of a type that has none should not compile.
template <typename T>
struct ChunkTagTraits;

} // namespace wem
} // namespace models
} // namespace whiteout

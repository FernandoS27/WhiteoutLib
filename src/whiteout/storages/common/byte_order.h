// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file byte_order.h
/// @brief Big-endian and little-endian read/write helpers.
///
/// Internal header — not part of the public include path.

#pragma once

#include <whiteout/common_types.h>

#include <cstring>
#include <vector>

namespace whiteout::storages::common {

// ============================================================================
// Big-endian readers
// ============================================================================

inline u16 readBE16(const u8* p) {
    return static_cast<u16>((u16(p[0]) << 8) | u16(p[1]));
}

inline u32 readBE32(const u8* p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

inline u64 readBE40(const u8* p) {
    return (u64(p[0]) << 32) | (u64(p[1]) << 24) | (u64(p[2]) << 16) |
           (u64(p[3]) << 8) | u64(p[4]);
}

/// Read a variable-length big-endian integer (1–4 bytes).
inline u32 readBEVar(const u8* p, u32 size) {
    u32 val = 0;
    for (u32 i = 0; i < size; ++i)
        val = (val << 8) | u32(p[i]);
    return val;
}

// ============================================================================
// Little-endian readers
// ============================================================================

inline u32 readLE32(const u8* p) {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

inline u64 readLE64(const u8* p) {
    return u64(readLE32(p)) | (u64(readLE32(p + 4)) << 32);
}

inline i32 readLEi32(const u8* p) {
    u32 v = readLE32(p);
    i32 r;
    std::memcpy(&r, &v, 4);
    return r;
}

// ============================================================================
// Big-endian writers
// ============================================================================

inline void writeBE16(u8* p, u16 v) {
    p[0] = u8(v >> 8);
    p[1] = u8(v);
}

inline void writeBE32(u8* p, u32 v) {
    p[0] = u8(v >> 24);
    p[1] = u8(v >> 16);
    p[2] = u8(v >> 8);
    p[3] = u8(v);
}

inline void writeBE40(u8* p, u64 v) {
    p[0] = u8(v >> 32);
    p[1] = u8(v >> 24);
    p[2] = u8(v >> 16);
    p[3] = u8(v >> 8);
    p[4] = u8(v);
}

// ============================================================================
// Vector push helpers (big-endian / little-endian)
// ============================================================================

inline void pushBE16(std::vector<u8>& v, u16 val) {
    v.push_back(static_cast<u8>((val >> 8) & 0xFF));
    v.push_back(static_cast<u8>(val & 0xFF));
}

inline void pushBE32(std::vector<u8>& v, u32 val) {
    v.push_back(static_cast<u8>((val >> 24) & 0xFF));
    v.push_back(static_cast<u8>((val >> 16) & 0xFF));
    v.push_back(static_cast<u8>((val >> 8) & 0xFF));
    v.push_back(static_cast<u8>(val & 0xFF));
}

inline void pushLE32(std::vector<u8>& v, u32 val) {
    v.push_back(static_cast<u8>(val & 0xFF));
    v.push_back(static_cast<u8>((val >> 8) & 0xFF));
    v.push_back(static_cast<u8>((val >> 16) & 0xFF));
    v.push_back(static_cast<u8>((val >> 24) & 0xFF));
}

} // namespace whiteout::storages::common

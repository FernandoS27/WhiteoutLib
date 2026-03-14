// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file checksum.h
/// @brief CRC32 and Adler-32 checksum utilities.

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include <whiteout/common_types.h>

namespace whiteout {

// ============================================================================
// CRC32 (polynomial 0xEDB88320, used by PNG / zlib)
// ============================================================================

constexpr std::array<u32, 256> buildCrc32Table() {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (i32 j = 0; j < 8; ++j) {
            if (c & 1)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c >>= 1;
        }
        table[i] = c;
    }
    return table;
}

inline constexpr auto CRC32_TABLE = buildCrc32Table();

inline u32 crc32(const u8* data, size_t length) {
    u32 crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc = CRC32_TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ============================================================================
// Adler-32 (RFC 1950, used by zlib wrapper)
// ============================================================================

inline u32 adler32(std::span<const u8> data) {
    static constexpr u32 MOD = 65521;
    u32 a = 1, b = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        a = (a + data[i]) % MOD;
        b = (b + a) % MOD;
    }
    return (b << 16) | a;
}

} // namespace whiteout

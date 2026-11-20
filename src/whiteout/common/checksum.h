// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file checksum.h
/// @brief CRC32 and Adler-32 checksum utilities.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <span>

#include <whiteout/common_types.h>

namespace whiteout {

// ============================================================================
// CRC32 (polynomial 0xEDB88320, used by PNG / zlib)
// ============================================================================

/// The tables for a CRC taken eight bytes a step: table k is the CRC of a
/// byte followed by k zero bytes, so eight lookups fold eight bytes at once.
/// Table 0 is the classic byte-at-a-time table.
constexpr std::array<std::array<u32, 256>, 8> buildCrc32Tables() {
    std::array<std::array<u32, 256>, 8> tables{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (i32 j = 0; j < 8; ++j) {
            if (c & 1)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c >>= 1;
        }
        tables[0][i] = c;
    }
    for (u32 i = 0; i < 256; ++i) {
        for (size_t k = 1; k < 8; ++k)
            tables[k][i] = tables[0][tables[k - 1][i] & 0xFF] ^ (tables[k - 1][i] >> 8);
    }
    return tables;
}

inline constexpr auto CRC32_TABLES = buildCrc32Tables();
inline constexpr const std::array<u32, 256>& CRC32_TABLE = CRC32_TABLES[0];

inline u32 crc32(const u8* data, size_t length) {
    u32 crc = 0xFFFFFFFF;
    if constexpr (std::endian::native == std::endian::little) {
        const auto& t = CRC32_TABLES;
        while (length >= 8) {
            u32 low = 0;
            u32 high = 0;
            std::memcpy(&low, data, 4);
            std::memcpy(&high, data + 4, 4);
            low ^= crc;
            crc = t[7][low & 0xFF] ^ t[6][(low >> 8) & 0xFF] ^ t[5][(low >> 16) & 0xFF] ^ t[4][low >> 24] ^
                  t[3][high & 0xFF] ^ t[2][(high >> 8) & 0xFF] ^ t[1][(high >> 16) & 0xFF] ^ t[0][high >> 24];
            data += 8;
            length -= 8;
        }
    }
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
    // Largest n such that 255*n*(n+1)/2 + (n+1)*(MOD-1) fits in u32 ≈ 5552.
    static constexpr size_t NMAX = 5552;
    u32 a = 1, b = 0;
    size_t remaining = data.size();
    const u8* p = data.data();
    while (remaining > 0) {
        size_t block = remaining > NMAX ? NMAX : remaining;
        remaining -= block;
        for (size_t i = 0; i < block; ++i) {
            a += p[i];
            b += a;
        }
        a %= MOD;
        b %= MOD;
        p += block;
    }
    return (b << 16) | a;
}

} // namespace whiteout

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file sha1.cpp
/// @brief Standalone SHA-1 implementation (FIPS 180-1).
///
/// Clean-room implementation based on the published specification.

#include "sha1.h"

#include <cstring>

namespace whiteout::storages::common {

namespace {

inline u32 rotl(u32 x, int n) {
    return (x << n) | (x >> (32 - n));
}

} // namespace

SHA1::SHA1() : m_state{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u} {}

void SHA1::processBlock(const u8* block) {
    u32 w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (u32(block[i * 4]) << 24) | (u32(block[i * 4 + 1]) << 16) |
               (u32(block[i * 4 + 2]) << 8) | u32(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    u32 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4];

    for (int i = 0; i < 80; ++i) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        u32 const t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
}

void SHA1::update(const void* data, size_t length) {
    const u8* p = static_cast<const u8*>(data);
    m_totalLen += length;

    if (m_bufferLen > 0) {
        size_t const want = 64 - m_bufferLen;
        size_t const take = (length < want) ? length : want;
        std::memcpy(m_buffer + m_bufferLen, p, take);
        m_bufferLen += take;
        p += take;
        length -= take;
        if (m_bufferLen < 64)
            return;
        processBlock(m_buffer);
        m_bufferLen = 0;
    }

    while (length >= 64) {
        processBlock(p);
        p += 64;
        length -= 64;
    }

    if (length > 0) {
        std::memcpy(m_buffer, p, length);
        m_bufferLen = length;
    }
}

std::array<u8, 20> SHA1::finalize() {
    u64 const bitLen = m_totalLen * 8;

    u8 pad[72]{};
    pad[0] = 0x80;
    size_t const padLen = (m_bufferLen < 56) ? (56 - m_bufferLen) : (120 - m_bufferLen);
    for (int i = 0; i < 8; ++i)
        pad[padLen + i] = u8(bitLen >> (56 - i * 8));
    update(pad, padLen + 8);

    std::array<u8, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = u8(m_state[i] >> 24);
        digest[i * 4 + 1] = u8(m_state[i] >> 16);
        digest[i * 4 + 2] = u8(m_state[i] >> 8);
        digest[i * 4 + 3] = u8(m_state[i]);
    }
    return digest;
}

} // namespace whiteout::storages::common

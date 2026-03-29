// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file md5.cpp
/// @brief Standalone MD5 implementation (RFC 1321).
///
/// Clean-room implementation based on the public RFC specification.

#include "md5.h"

#include <cstring>

namespace whiteout::storages::common {

namespace {

inline u32 F(u32 x, u32 y, u32 z) {
    return (x & y) | (~x & z);
}
inline u32 G(u32 x, u32 y, u32 z) {
    return (x & z) | (y & ~z);
}
inline u32 H(u32 x, u32 y, u32 z) {
    return x ^ y ^ z;
}
inline u32 I(u32 x, u32 y, u32 z) {
    return y ^ (x | ~z);
}

inline u32 rotl(u32 x, int n) {
    return (x << n) | (x >> (32 - n));
}

inline void FF(u32& a, u32 b, u32 c, u32 d, u32 x, int s, u32 ac) {
    a += F(b, c, d) + x + ac;
    a = rotl(a, s) + b;
}
inline void GG(u32& a, u32 b, u32 c, u32 d, u32 x, int s, u32 ac) {
    a += G(b, c, d) + x + ac;
    a = rotl(a, s) + b;
}
inline void HH(u32& a, u32 b, u32 c, u32 d, u32 x, int s, u32 ac) {
    a += H(b, c, d) + x + ac;
    a = rotl(a, s) + b;
}
inline void II(u32& a, u32 b, u32 c, u32 d, u32 x, int s, u32 ac) {
    a += I(b, c, d) + x + ac;
    a = rotl(a, s) + b;
}

inline u32 readLE32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

inline void writeLE32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

} // anonymous namespace

MD5::MD5() {
    m_state[0] = 0x67452301;
    m_state[1] = 0xEFCDAB89;
    m_state[2] = 0x98BADCFE;
    m_state[3] = 0x10325476;
}

void MD5::processBlock(const u8* block) {
    u32 M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = readLE32(block + i * 4);
    }

    u32 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];

    // Round 1
    FF(a, b, c, d, M[0], 7, 0xD76AA478);
    FF(d, a, b, c, M[1], 12, 0xE8C7B756);
    FF(c, d, a, b, M[2], 17, 0x242070DB);
    FF(b, c, d, a, M[3], 22, 0xC1BDCEEE);
    FF(a, b, c, d, M[4], 7, 0xF57C0FAF);
    FF(d, a, b, c, M[5], 12, 0x4787C62A);
    FF(c, d, a, b, M[6], 17, 0xA8304613);
    FF(b, c, d, a, M[7], 22, 0xFD469501);
    FF(a, b, c, d, M[8], 7, 0x698098D8);
    FF(d, a, b, c, M[9], 12, 0x8B44F7AF);
    FF(c, d, a, b, M[10], 17, 0xFFFF5BB1);
    FF(b, c, d, a, M[11], 22, 0x895CD7BE);
    FF(a, b, c, d, M[12], 7, 0x6B901122);
    FF(d, a, b, c, M[13], 12, 0xFD987193);
    FF(c, d, a, b, M[14], 17, 0xA679438E);
    FF(b, c, d, a, M[15], 22, 0x49B40821);

    // Round 2
    GG(a, b, c, d, M[1], 5, 0xF61E2562);
    GG(d, a, b, c, M[6], 9, 0xC040B340);
    GG(c, d, a, b, M[11], 14, 0x265E5A51);
    GG(b, c, d, a, M[0], 20, 0xE9B6C7AA);
    GG(a, b, c, d, M[5], 5, 0xD62F105D);
    GG(d, a, b, c, M[10], 9, 0x02441453);
    GG(c, d, a, b, M[15], 14, 0xD8A1E681);
    GG(b, c, d, a, M[4], 20, 0xE7D3FBC8);
    GG(a, b, c, d, M[9], 5, 0x21E1CDE6);
    GG(d, a, b, c, M[14], 9, 0xC33707D6);
    GG(c, d, a, b, M[3], 14, 0xF4D50D87);
    GG(b, c, d, a, M[8], 20, 0x455A14ED);
    GG(a, b, c, d, M[13], 5, 0xA9E3E905);
    GG(d, a, b, c, M[2], 9, 0xFCEFA3F8);
    GG(c, d, a, b, M[7], 14, 0x676F02D9);
    GG(b, c, d, a, M[12], 20, 0x8D2A4C8A);

    // Round 3
    HH(a, b, c, d, M[5], 4, 0xFFFA3942);
    HH(d, a, b, c, M[8], 11, 0x8771F681);
    HH(c, d, a, b, M[11], 16, 0x6D9D6122);
    HH(b, c, d, a, M[14], 23, 0xFDE5380C);
    HH(a, b, c, d, M[1], 4, 0xA4BEEA44);
    HH(d, a, b, c, M[4], 11, 0x4BDECFA9);
    HH(c, d, a, b, M[7], 16, 0xF6BB4B60);
    HH(b, c, d, a, M[10], 23, 0xBEBFBC70);
    HH(a, b, c, d, M[13], 4, 0x289B7EC6);
    HH(d, a, b, c, M[0], 11, 0xEAA127FA);
    HH(c, d, a, b, M[3], 16, 0xD4EF3085);
    HH(b, c, d, a, M[6], 23, 0x04881D05);
    HH(a, b, c, d, M[9], 4, 0xD9D4D039);
    HH(d, a, b, c, M[12], 11, 0xE6DB99E5);
    HH(c, d, a, b, M[15], 16, 0x1FA27CF8);
    HH(b, c, d, a, M[2], 23, 0xC4AC5665);

    // Round 4
    II(a, b, c, d, M[0], 6, 0xF4292244);
    II(d, a, b, c, M[7], 10, 0x432AFF97);
    II(c, d, a, b, M[14], 15, 0xAB9423A7);
    II(b, c, d, a, M[5], 21, 0xFC93A039);
    II(a, b, c, d, M[12], 6, 0x655B59C3);
    II(d, a, b, c, M[3], 10, 0x8F0CCC92);
    II(c, d, a, b, M[10], 15, 0xFFEFF47D);
    II(b, c, d, a, M[1], 21, 0x85845DD1);
    II(a, b, c, d, M[8], 6, 0x6FA87E4F);
    II(d, a, b, c, M[15], 10, 0xFE2CE6E0);
    II(c, d, a, b, M[6], 15, 0xA3014314);
    II(b, c, d, a, M[13], 21, 0x4E0811A1);
    II(a, b, c, d, M[4], 6, 0xF7537E82);
    II(d, a, b, c, M[11], 10, 0xBD3AF235);
    II(c, d, a, b, M[2], 15, 0x2AD7D2BB);
    II(b, c, d, a, M[9], 21, 0xEB86D391);

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
}

void MD5::update(const void* data, size_t length) {
    const auto* input = static_cast<const u8*>(data);
    m_totalLen += length;

    // If we have buffered data, try to complete a block.
    if (m_bufferLen > 0) {
        size_t needed = 64 - m_bufferLen;
        if (length < needed) {
            std::memcpy(m_buffer + m_bufferLen, input, length);
            m_bufferLen += length;
            return;
        }
        std::memcpy(m_buffer + m_bufferLen, input, needed);
        processBlock(m_buffer);
        input += needed;
        length -= needed;
        m_bufferLen = 0;
    }

    // Process complete 64-byte blocks.
    while (length >= 64) {
        processBlock(input);
        input += 64;
        length -= 64;
    }

    // Buffer remaining bytes.
    if (length > 0) {
        std::memcpy(m_buffer, input, length);
        m_bufferLen = length;
    }
}

std::array<u8, 16> MD5::finalize() {
    // Padding: append 0x80, then zeros, then 64-bit length in bits (LE).
    u64 totalBits = m_totalLen * 8;

    u8 padding[72]; // At most 64 + 8 bytes of padding needed.
    padding[0] = 0x80;
    size_t padLen = (m_bufferLen < 56) ? (56 - m_bufferLen) : (120 - m_bufferLen);
    std::memset(padding + 1, 0, padLen - 1);

    // Append length in bits as 64-bit LE.
    writeLE32(padding + padLen, static_cast<u32>(totalBits));
    writeLE32(padding + padLen + 4, static_cast<u32>(totalBits >> 32));

    update(padding, padLen + 8);

    // Produce the digest.
    std::array<u8, 16> digest{};
    writeLE32(digest.data() + 0, m_state[0]);
    writeLE32(digest.data() + 4, m_state[1]);
    writeLE32(digest.data() + 8, m_state[2]);
    writeLE32(digest.data() + 12, m_state[3]);
    return digest;
}

} // namespace whiteout::storages::common

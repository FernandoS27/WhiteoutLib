// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file jenkins.cpp
/// @brief Jenkins hashlittle2 implementation (Bob Jenkins' lookup3).
///
/// Reference: http://burtleburtle.net/bob/c/lookup3.c
/// This is a clean-room implementation based on the public-domain algorithm description.

#include "jenkins.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace whiteout::storages::common {

namespace {

inline u32 rot(u32 x, int k) {
    return (x << k) | (x >> (32 - k));
}

inline void mix(u32& a, u32& b, u32& c) {
    a -= c;
    a ^= rot(c, 4);
    c += b;
    b -= a;
    b ^= rot(a, 6);
    a += c;
    c -= b;
    c ^= rot(b, 8);
    b += a;
    a -= c;
    a ^= rot(c, 16);
    c += b;
    b -= a;
    b ^= rot(a, 19);
    a += c;
    c -= b;
    c ^= rot(b, 4);
    b += a;
}

inline void final_(u32& a, u32& b, u32& c) {
    c ^= b;
    c -= rot(b, 14);
    a ^= c;
    a -= rot(c, 11);
    b ^= a;
    b -= rot(a, 25);
    c ^= b;
    c -= rot(b, 16);
    a ^= c;
    a -= rot(c, 4);
    b ^= a;
    b -= rot(a, 14);
    c ^= b;
    c -= rot(b, 24);
}

} // anonymous namespace

void jenkinsHashlittle2(const void* key, size_t length, u32& pc, u32& pb) {
    u32 a, b, c;
    a = b = c = 0xDEADBEEF + static_cast<u32>(length) + pc;
    c += pb;

    const auto* k = static_cast<const u8*>(key);

    // Process 12-byte chunks.
    while (length > 12) {
        u32 k0, k1, k2;
        std::memcpy(&k0, k + 0, 4);
        std::memcpy(&k1, k + 4, 4);
        std::memcpy(&k2, k + 8, 4);
        a += k0;
        b += k1;
        c += k2;
        mix(a, b, c);
        length -= 12;
        k += 12;
    }

    // Handle the last few bytes (little-endian).
    // All the case statements fall through.
    switch (length) {
    case 12:
        c += static_cast<u32>(k[11]) << 24;
        [[fallthrough]];
    case 11:
        c += static_cast<u32>(k[10]) << 16;
        [[fallthrough]];
    case 10:
        c += static_cast<u32>(k[9]) << 8;
        [[fallthrough]];
    case 9:
        c += static_cast<u32>(k[8]);
        [[fallthrough]];
    case 8:
        b += static_cast<u32>(k[7]) << 24;
        [[fallthrough]];
    case 7:
        b += static_cast<u32>(k[6]) << 16;
        [[fallthrough]];
    case 6:
        b += static_cast<u32>(k[5]) << 8;
        [[fallthrough]];
    case 5:
        b += static_cast<u32>(k[4]);
        [[fallthrough]];
    case 4: {
        u32 k0;
        std::memcpy(&k0, k, 4);
        a += k0;
        break;
    }
    case 3:
        a += static_cast<u32>(k[2]) << 16;
        [[fallthrough]];
    case 2:
        a += static_cast<u32>(k[1]) << 8;
        [[fallthrough]];
    case 1:
        a += static_cast<u32>(k[0]);
        break;
    case 0:
        pc = c;
        pb = b;
        return; // Nothing to add — return early.
    }

    final_(a, b, c);
    pc = c;
    pb = b;
}

std::string normalizePath(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (char ch : name) {
        if (ch == '/')
            result.push_back('\\');
        else
            result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return result;
}

JenkinsHash jenkinsHash(const std::string& filename) {
    std::string normalized = normalizePath(filename);
    u32 pc = 0, pb = 0;
    jenkinsHashlittle2(normalized.data(), normalized.size(), pc, pb);
    return {pc, pb};
}

} // namespace whiteout::storages::common

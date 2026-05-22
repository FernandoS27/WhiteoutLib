// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file hex.h
/// @brief Hex encoding/decoding utilities.
///
/// Internal header — not part of the public include path.

#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <string>
#include <string_view>

namespace whiteout::storages::common {

/// Convert a single hex character to its 4-bit value (0–15).
/// Returns -1 for invalid characters.
inline int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

/// Decode a hex string into a byte array. Returns false if the string
/// has incorrect length or contains invalid characters.
inline bool hexDecode(std::string_view hex, u8* out, size_t count) {
    if (hex.size() != count * 2)
        return false;
    for (size_t i = 0; i < count; ++i) {
        int hi = hexDigit(hex[i * 2]);
        int lo = hexDigit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<u8>((hi << 4) | lo);
    }
    return true;
}

/// Decode a hex string into a 16-byte key. Tolerant of shorter input.
inline std::array<u8, 16> hexDecode16(std::string_view hex) {
    std::array<u8, 16> result{};
    size_t len = std::min(hex.size() / 2, size_t(16));
    for (size_t i = 0; i < len; ++i) {
        int hi = hexDigit(hex[i * 2]);
        int lo = hexDigit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            break;
        result[i] = static_cast<u8>((hi << 4) | lo);
    }
    return result;
}

/// Parse a hex string as a u64 value.
inline u64 hexToU64(std::string_view hex) {
    u64 result = 0;
    for (char c : hex) {
        int d = hexDigit(c);
        if (d < 0)
            return 0;
        result = (result << 4) | static_cast<u64>(d);
    }
    return result;
}

/// Hex-encode a 16-byte key to a 32-character lowercase string.
inline std::string hexEncode16(const std::array<u8, 16>& key) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (auto b : key) {
        result.push_back(hex[b >> 4]);
        result.push_back(hex[b & 0xF]);
    }
    return result;
}

/// Hex-encode an arbitrary byte span to a lowercase string.
inline std::string hexEncode(const u8* data, size_t size) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result.push_back(hex[data[i] >> 4]);
        result.push_back(hex[data[i] & 0xF]);
    }
    return result;
}

} // namespace whiteout::storages::common

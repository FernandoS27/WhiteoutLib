// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file key_utils.h
/// @brief Inline helpers for CASC key manipulation (EKey/CKey).
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <cstring>
#include <span>

namespace whiteout::storages::casc {

/// Truncated EKey size used in index lookups.
static constexpr size_t kEKeyTruncSize = 9;

/// 16-byte zero sentinel — used to detect EKey-only TVFS entries.
static constexpr std::array<u8, 16> kZeroKey{};

/// Create a 9-byte truncated EKey span suitable for index lookups.
inline std::span<const u8> eKeyTrunc(const std::array<u8, 16>& key) {
    return std::span<const u8>(key.data(), kEKeyTruncSize);
}

/// Create a 9-byte truncated EKey span from a fixed-size span.
inline std::span<const u8> eKeyTrunc(std::span<const u8, 16> key) {
    return std::span<const u8>(key.data(), kEKeyTruncSize);
}

/// Extract the first 8 bytes of a 16-byte key as a u64 hash for hash-map lookups.
inline u64 keyHash64(const u8* key) {
    u64 h = 0;
    std::memcpy(&h, key, 8);
    return h;
}

/// Extract the first 8 bytes of a 16-byte key as a u64 hash for hash-map lookups.
inline u64 keyHash64(const std::array<u8, 16>& key) {
    return keyHash64(key.data());
}

/// Check whether a 16-byte key is all zeros (sentinel value).
inline bool isZeroKey(const std::array<u8, 16>& key) {
    return key == kZeroKey;
}

} // namespace whiteout::storages::casc

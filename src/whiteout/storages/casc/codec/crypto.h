// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file crypto.h
/// @brief CASC encryption: Salsa20, ARC4, and key management.
///
/// Internal header — not part of the public include path.
#pragma once

#include <whiteout/common_types.h>

#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace whiteout::storages::casc {

// ============================================================================
// Key Management
// ============================================================================

class KeyRing {
public:
    /// Add a key by binary value.
    void addKey(u64 keyName, std::array<u8, 16> key);

    /// Add a key from a hex string (32 hex characters).
    void addKey(u64 keyName, const std::string& hexKey);

    /// Import keys from a multi-line string: "keyNameHex keyValueHex\n" per line.
    /// @return true if at least one key was imported.
    bool importFromString(const std::string& keyList);

    /// Import keys from a file (same format as importFromString).
    /// @return true if at least one key was imported.
    bool importFromFile(const std::string& path);

    /// Lookup a key. Returns nullptr if not found (and records the miss).
    const std::array<u8, 16>* findKey(u64 keyName) const;

    /// Return the first key name that was looked up but not found.
    std::optional<u64> firstMissingKey() const;

private:
    std::unordered_map<u64, std::array<u8, 16>> m_keys;
    mutable std::atomic<u64> m_firstMissing{0}; ///< 0 = no miss (0 is not a valid key name).
};

// ============================================================================
// Salsa20
// ============================================================================

/// Salsa20 decrypt (20-round, 256-bit key variant).
/// For 128-bit CASC keys, the key is expanded to 256 bits by repeating.
/// Operates in-place on `data`.
void salsa20Decrypt(std::span<u8> data, std::span<const u8, 16> key, std::span<const u8, 8> iv);

// ============================================================================
// ARC4 (RC4)
// ============================================================================

/// ARC4 (RC4) decrypt/encrypt (symmetric). Operates in-place.
void arc4Transform(std::span<u8> data, std::span<const u8> key);

} // namespace whiteout::storages::casc

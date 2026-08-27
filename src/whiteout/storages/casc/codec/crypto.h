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

    /// What a decoder should do with a frame whose key is unavailable: fail the
    /// whole file (false, the default) or substitute zeros for that frame
    /// (true). CascLib spells the latter CASC_OVERCOME_ENCRYPTED.
    ///
    /// Unreleased content ships encrypted with keys nobody outside Blizzard
    /// has, and one such frame otherwise takes an entire client database with
    /// it — a table that is 99% readable is worth more than none of it. Lives
    /// here because the ring is what knows a key is missing, and it already
    /// reaches every decoder.
    void setZeroFillUnknown(bool on) {
        m_zeroFillUnknown = on;
    }
    bool zeroFillUnknown() const noexcept {
        return m_zeroFillUnknown;
    }

private:
    bool m_zeroFillUnknown = false;
    std::unordered_map<u64, std::array<u8, 16>> m_keys;
    mutable std::atomic<u64> m_firstMissing{0}; ///< 0 = no miss (0 is not a valid key name).
};

// ============================================================================
// Salsa20
// ============================================================================

/// Salsa20 decrypt, 20 rounds with a 128-bit key (the "tau" key schedule, with
/// the key repeated across both halves of the state). Operates in-place.
void salsa20Decrypt(std::span<u8> data, std::span<const u8, 16> key, std::span<const u8, 8> iv);

// ============================================================================
// ARC4 (RC4)
// ============================================================================

/// ARC4 (RC4) decrypt/encrypt (symmetric). Operates in-place.
void arc4Transform(std::span<u8> data, std::span<const u8> key);

// ============================================================================
// AES-256-CBC
// ============================================================================

/// AES-256-CBC decrypt, in place. Whole blocks only — a trailing partial block
/// is left untouched, because Overwatch's manifest decrypter hands over the raw
/// body and keeps only the records the header promises rather than trusting a
/// padding byte.
void aes256CbcDecrypt(std::span<u8> data, std::span<const u8, 32> key, std::span<const u8, 16> iv);

} // namespace whiteout::storages::casc

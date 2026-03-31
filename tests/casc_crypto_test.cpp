// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_crypto_test: Validates Salsa20, ARC4, and KeyRing implementations.

#include "../src/whiteout/storages/casc/crypto.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

static int g_passed = 0;
static int g_failed = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        std::cout << "  PASS: " << name << "\n";
        ++g_passed;
    } else {
        std::cout << "  FAIL: " << name << "\n";
        ++g_failed;
    }
}

static bool spanEqual(std::span<const u8> a, std::span<const u8> b) {
    if (a.size() != b.size())
        return false;
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// ============================================================================
// Salsa20 Tests
// ============================================================================

static void testSalsa20() {
    std::cout << "[Test: Salsa20]\n";

    // DJB test vector: 128-bit key (repeated to 256), zero nonce, zero plaintext
    // Key: 80 00 00 00 ... (128-bit key = 1 bit set)
    // For CASC: key is repeated → 256-bit key = key||key
    // We test encrypt-then-decrypt round-trip since the standard test vectors
    // are for Salsa20/20 with 256-bit key which requires key expansion.

    // Round-trip test: encrypt then decrypt should return original.
    std::array<u8, 16> key{};
    key[0] = 0x80;

    std::array<u8, 8> iv{};
    iv[0] = 0x01;

    std::vector<u8> plaintext(64, 0);
    for (size_t i = 0; i < plaintext.size(); ++i)
        plaintext[i] = static_cast<u8>(i);

    std::vector<u8> original = plaintext;

    // Encrypt.
    salsa20Decrypt(plaintext, key, iv);
    check(plaintext != original, "Salsa20 encryption changes data");

    // Decrypt (Salsa20 is symmetric — same operation reverses).
    salsa20Decrypt(plaintext, key, iv);
    check(spanEqual(plaintext, original), "Salsa20 decrypt restores original data");

    // Empty data — should not crash.
    std::vector<u8> empty;
    salsa20Decrypt(empty, key, iv);
    check(true, "Salsa20 with empty data does not crash");

    // 1 byte test.
    std::vector<u8> single = {0x42};
    std::vector<u8> singleOrig = single;
    salsa20Decrypt(single, key, iv);
    salsa20Decrypt(single, key, iv);
    check(single == singleOrig, "Salsa20 round-trip on 1 byte");

    // Large data (non-aligned to 64-byte block).
    std::vector<u8> large(1000, 0xAB);
    std::vector<u8> largeOrig = large;
    salsa20Decrypt(large, key, iv);
    check(large != largeOrig, "Salsa20 encryption changes large data");
    salsa20Decrypt(large, key, iv);
    check(large == largeOrig, "Salsa20 round-trip on large data");
}

// ============================================================================
// ARC4 Tests
// ============================================================================

static void testARC4() {
    std::cout << "[Test: ARC4]\n";

    // RFC 6229 test vector with key = 0102030405 (5-byte key)
    // Key: 01 02 03 04 05
    // Offset 0 output stream bytes (first 16): b2 39 63 05 f0 3d c0 27 cc c3 52 4a 0a 11 18 a8
    // XOR with zero plaintext gives the keystream.
    u8 keyBytes[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::span<const u8> key(keyBytes, 5);

    // Expected keystream (first 16 bytes) from RFC 6229.
    u8 expected[] = {0xb2, 0x39, 0x63, 0x05, 0xf0, 0x3d, 0xc0, 0x27,
                     0xcc, 0xc3, 0x52, 0x4a, 0x0a, 0x11, 0x18, 0xa8};

    // Zero plaintext → output is keystream.
    std::vector<u8> data(16, 0);
    arc4Transform(data, key);
    check(spanEqual(data, std::span<const u8>(expected, 16)), "ARC4 matches RFC 6229 test vector");

    // Round-trip: encrypt then decrypt with same key.
    std::vector<u8> plaintext = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    std::vector<u8> original = plaintext;
    arc4Transform(plaintext, key);
    check(plaintext != original, "ARC4 encryption changes data");
    arc4Transform(plaintext, key);
    check(plaintext == original, "ARC4 decrypt restores original data");

    // Empty data — should not crash.
    std::vector<u8> empty;
    arc4Transform(empty, key);
    check(true, "ARC4 with empty data does not crash");
}

// ============================================================================
// KeyRing Tests
// ============================================================================

static void testKeyRing() {
    std::cout << "[Test: KeyRing]\n";

    KeyRing ring;

    // Test addKey + findKey round-trip.
    std::array<u8, 16> testKey = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                  0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    ring.addKey(0x12345678ABCDEF00ULL, testKey);
    const auto* found = ring.findKey(0x12345678ABCDEF00ULL);
    check(found != nullptr, "findKey returns non-null for added key");
    check(found && *found == testKey, "findKey returns correct key value");

    // Missing key tracking.
    const auto* missing = ring.findKey(0xDEADBEEFDEADBEEFULL);
    check(missing == nullptr, "findKey returns null for unknown key");
    auto firstMiss = ring.firstMissingKey();
    check(firstMiss.has_value() && *firstMiss == 0xDEADBEEFDEADBEEFULL,
          "firstMissingKey returns the first miss");
}

static void testKeyRingHex() {
    std::cout << "[Test: KeyRing hex import]\n";

    KeyRing ring;
    ring.addKey(0xAAAABBBBCCCCDDDDULL, "0102030405060708090A0B0C0D0E0F10");
    const auto* found = ring.findKey(0xAAAABBBBCCCCDDDDULL);
    check(found != nullptr, "Hex addKey returns non-null");
    std::array<u8, 16> expected = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                   0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    check(found && *found == expected, "Hex key value is correct");
}

static void testKeyRingImportFromString() {
    std::cout << "[Test: KeyRing importFromString]\n";

    KeyRing ring;
    std::string keyList =
        "12345678ABCDEF00 0102030405060708090A0B0C0D0E0F10\n"
        "AABBCCDD11223344 A0B0C0D0E0F0A1B1C1D1E1F1A2B2C2D2\n";
    bool ok = ring.importFromString(keyList);
    check(ok, "importFromString returns true");

    const auto* k1 = ring.findKey(0x12345678ABCDEF00ULL);
    check(k1 != nullptr, "First imported key found");

    const auto* k2 = ring.findKey(0xAABBCCDD11223344ULL);
    check(k2 != nullptr, "Second imported key found");

    std::array<u8, 16> expected2 = {0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0xA1, 0xB1,
                                    0xC1, 0xD1, 0xE1, 0xF1, 0xA2, 0xB2, 0xC2, 0xD2};
    check(k2 && *k2 == expected2, "Second imported key value is correct");
}

int main() {
    std::cout << "=== CASC Crypto Tests ===\n\n";

    testSalsa20();
    testARC4();
    testKeyRing();
    testKeyRingHex();
    testKeyRingImportFromString();

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}

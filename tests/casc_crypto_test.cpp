// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_crypto_test: Validates Salsa20, ARC4, and KeyRing implementations.

#include "../src/whiteout/storages/casc/codec/crypto.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

static bool spanEqual(std::span<const u8> a, std::span<const u8> b) {
    if (a.size() != b.size())
        return false;
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// ============================================================================
// Salsa20 Tests
// ============================================================================

// The round-trip sections below pass for *any* self-consistent keystream, which
// is how a wrong key schedule ("sigma", the 256-bit constants, instead of
// "tau") survived here while every encrypted CASC frame decoded to noise. This
// is ECRYPT Salsa20 Set 1 vector #0 for a 128-bit key: XORing the cipher over
// 64 zero bytes yields the keystream itself, so the expected block is the
// published one.
TEST_CASE("Salsa20 matches the published 128-bit key vector", "[casc][crypto]") {
    std::array<u8, 16> key{};
    key[0] = 0x80;
    std::array<u8, 8> const iv{};

    std::vector<u8> stream(64, 0);
    salsa20Decrypt(stream, key, iv);

    static constexpr u8 kExpected[64] = {
        0x4D, 0xFA, 0x5E, 0x48, 0x1D, 0xA2, 0x3E, 0xA0, 0x9A, 0x31, 0x02, 0x20, 0x50,
        0x85, 0x99, 0x36, 0xDA, 0x52, 0xFC, 0xEE, 0x21, 0x80, 0x05, 0x16, 0x4F, 0x26,
        0x7C, 0xB6, 0x5F, 0x5C, 0xFD, 0x7F, 0x2B, 0x4F, 0x97, 0xE0, 0xFF, 0x16, 0x92,
        0x4A, 0x52, 0xDF, 0x26, 0x95, 0x15, 0x11, 0x0A, 0x07, 0xF9, 0xE4, 0x60, 0xBC,
        0x65, 0xEF, 0x95, 0xDA, 0x58, 0xF7, 0x40, 0xB7, 0xD1, 0xDB, 0xB0, 0xAA,
    };
    CHECK(std::memcmp(stream.data(), kExpected, sizeof(kExpected)) == 0);
}

TEST_CASE("Salsa20", "[casc][crypto]") {
    // Round-trip test: encrypt then decrypt should return original.
    std::array<u8, 16> key{};
    key[0] = 0x80;

    std::array<u8, 8> iv{};
    iv[0] = 0x01;

    std::vector<u8> plaintext(64, 0);
    for (size_t i = 0; i < plaintext.size(); ++i)
        plaintext[i] = static_cast<u8>(i);

    std::vector<u8> original = plaintext;

    SECTION("Encryption changes data") {
        salsa20Decrypt(plaintext, key, iv);
        CHECK(plaintext != original);
    }

    SECTION("Decrypt restores original data") {
        salsa20Decrypt(plaintext, key, iv);
        salsa20Decrypt(plaintext, key, iv);
        CHECK(spanEqual(plaintext, original));
    }

    SECTION("Empty data does not crash") {
        std::vector<u8> empty;
        salsa20Decrypt(empty, key, iv);
        SUCCEED();
    }

    SECTION("Round-trip on 1 byte") {
        std::vector<u8> single = {0x42};
        std::vector<u8> singleOrig = single;
        salsa20Decrypt(single, key, iv);
        salsa20Decrypt(single, key, iv);
        CHECK(single == singleOrig);
    }

    SECTION("Round-trip on large data") {
        std::vector<u8> large(1000, 0xAB);
        std::vector<u8> largeOrig = large;
        salsa20Decrypt(large, key, iv);
        CHECK(large != largeOrig);
        salsa20Decrypt(large, key, iv);
        CHECK(large == largeOrig);
    }
}

// ============================================================================
// ARC4 Tests
// ============================================================================

TEST_CASE("ARC4", "[casc][crypto]") {
    u8 keyBytes[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::span<const u8> key(keyBytes, 5);

    SECTION("Matches RFC 6229 test vector") {
        u8 expected[] = {0xb2, 0x39, 0x63, 0x05, 0xf0, 0x3d, 0xc0, 0x27,
                         0xcc, 0xc3, 0x52, 0x4a, 0x0a, 0x11, 0x18, 0xa8};
        std::vector<u8> data(16, 0);
        arc4Transform(data, key);
        CHECK(spanEqual(data, std::span<const u8>(expected, 16)));
    }

    SECTION("Round-trip restores original data") {
        std::vector<u8> plaintext = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
        std::vector<u8> original = plaintext;
        arc4Transform(plaintext, key);
        CHECK(plaintext != original);
        arc4Transform(plaintext, key);
        CHECK(plaintext == original);
    }

    SECTION("Empty data does not crash") {
        std::vector<u8> empty;
        arc4Transform(empty, key);
        SUCCEED();
    }
}

// ============================================================================
// KeyRing Tests
// ============================================================================

TEST_CASE("KeyRing", "[casc][crypto]") {
    KeyRing ring;

    SECTION("addKey + findKey round-trip") {
        std::array<u8, 16> testKey = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                      0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
        ring.addKey(0x12345678ABCDEF00ULL, testKey);
        const auto* found = ring.findKey(0x12345678ABCDEF00ULL);
        REQUIRE(found != nullptr);
        CHECK(*found == testKey);
    }

    SECTION("Missing key tracking") {
        const auto* missing = ring.findKey(0xDEADBEEFDEADBEEFULL);
        CHECK(missing == nullptr);
        auto firstMiss = ring.firstMissingKey();
        REQUIRE(firstMiss.has_value());
        CHECK(*firstMiss == 0xDEADBEEFDEADBEEFULL);
    }
}

TEST_CASE("KeyRing hex import", "[casc][crypto]") {
    KeyRing ring;
    ring.addKey(0xAAAABBBBCCCCDDDDULL, "0102030405060708090A0B0C0D0E0F10");
    const auto* found = ring.findKey(0xAAAABBBBCCCCDDDDULL);
    REQUIRE(found != nullptr);
    std::array<u8, 16> expected = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                   0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    CHECK(*found == expected);
}

TEST_CASE("KeyRing importFromString", "[casc][crypto]") {
    KeyRing ring;
    std::string keyList =
        "12345678ABCDEF00 0102030405060708090A0B0C0D0E0F10\n"
        "AABBCCDD11223344 A0B0C0D0E0F0A1B1C1D1E1F1A2B2C2D2\n";
    REQUIRE(ring.importFromString(keyList));

    const auto* k1 = ring.findKey(0x12345678ABCDEF00ULL);
    REQUIRE(k1 != nullptr);

    const auto* k2 = ring.findKey(0xAABBCCDD11223344ULL);
    REQUIRE(k2 != nullptr);

    std::array<u8, 16> expected2 = {0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0xA1, 0xB1,
                                    0xC1, 0xD1, 0xE1, 0xF1, 0xA2, 0xB2, 0xC2, 0xD2};
    CHECK(*k2 == expected2);
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_crypto_test: Validates Salsa20, ARC4, and KeyRing implementations.

#include "../src/whiteout/storages/casc/codec/crypto.h"
#include "../src/whiteout/storages/common/sha1.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
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
    std::string keyList = "12345678ABCDEF00 0102030405060708090A0B0C0D0E0F10\n"
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

// ============================================================================
// SHA-1 Tests
// ============================================================================

static std::string hex(std::span<const u8> data) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (u8 b : data) {
        s += d[b >> 4];
        s += d[b & 0xF];
    }
    return s;
}

static std::span<const u8> asBytes(const std::string& s) {
    return {reinterpret_cast<const u8*>(s.data()), s.size()};
}

// FIPS 180-1 appendix A/B/C.
TEST_CASE("SHA-1 matches the published vectors", "[casc][crypto]") {
    using namespace whiteout::storages::common;

    CHECK(hex(sha1Hash(asBytes(std::string("")))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(sha1Hash(asBytes(std::string("abc")))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(sha1Hash(
              asBytes(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hex(sha1Hash(asBytes(std::string(1000000, 'a')))) ==
          "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

// A digest fed in uneven pieces must match the one-shot digest, which is what
// the 64-byte block buffering is there to guarantee.
TEST_CASE("SHA-1 incremental update matches one-shot", "[casc][crypto]") {
    using namespace whiteout::storages::common;

    std::vector<u8> data(1000);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = u8(i * 7 + 3);

    SHA1 h;
    for (size_t off = 0, step = 1; off < data.size(); off += step, step = step % 97 + 1)
        h.update(data.data() + off, std::min(step, data.size() - off));

    CHECK(hex(h.finalize()) == hex(sha1Hash(data)));
}

// ============================================================================
// AES-256-CBC Tests
// ============================================================================

static std::vector<u8> unhex(std::string_view s) {
    auto nib = [](char c) { return u8(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
    std::vector<u8> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(u8((nib(s[i]) << 4) | nib(s[i + 1])));
    return out;
}

// NIST SP 800-38A, F.2.6 CBC-AES256.Decrypt.
TEST_CASE("AES-256-CBC matches the NIST vector", "[casc][crypto]") {
    auto const key = unhex("603deb1015ca71be2b73aef0857d7781"
                           "1f352c073b6108d72d9810a30914dff4");
    auto const iv = unhex("000102030405060708090a0b0c0d0e0f");
    auto data = unhex("f58c4c04d6e5f1ba779eabfb5f7bfbd6"
                      "9cfc4e967edb808d679f777bc6702c7d"
                      "39f23369a9d9bacfa530e26304231461"
                      "b2eb05e2c39be9fcda6c19078c6a9d1b");

    aes256CbcDecrypt(data, std::span<const u8, 32>(key), std::span<const u8, 16>(iv));

    CHECK(hex(data) == "6bc1bee22e409f96e93d7e117393172a"
                       "ae2d8a571e03ac9c9eb76fac45af8e51"
                       "30c81c46a35ce411e5fbc1191a0a52ef"
                       "f69f2445df4f9b17ad2b417be66c3710");
}

// The manifest decrypter hands over whatever whole blocks the body contains and
// keeps only the records it expects, so a trailing partial block must be left
// alone rather than treated as padding.
TEST_CASE("AES-256-CBC ignores a trailing partial block", "[casc][crypto]") {
    auto const key = unhex("603deb1015ca71be2b73aef0857d7781"
                           "1f352c073b6108d72d9810a30914dff4");
    auto const iv = unhex("000102030405060708090a0b0c0d0e0f");
    auto data = unhex("f58c4c04d6e5f1ba779eabfb5f7bfbd6"
                      "9cfc4e967edb808d679f777bc6702c7d"
                      "deadbeef");

    aes256CbcDecrypt(data, std::span<const u8, 32>(key), std::span<const u8, 16>(iv));

    CHECK(hex(data) == "6bc1bee22e409f96e93d7e117393172a"
                       "ae2d8a571e03ac9c9eb76fac45af8e51"
                       "deadbeef");
}

// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_blte_test: Validates BLTE encode/decode round-trip and edge cases.

#include "../src/whiteout/storages/casc/blte.h"
#include "../src/whiteout/storages/casc/crypto.h"

#include <cstring>
#include <iostream>
#include <numeric>
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

// ============================================================================
// Round-Trip Tests
// ============================================================================

static void testRoundTripEmpty() {
    std::cout << "[Test: Round-trip empty data]\n";
    std::vector<u8> empty;
    auto encoded = blteEncode(empty);
    check(!encoded.empty(), "Encoding empty data produces non-empty BLTE blob");

    auto result = blteDecode(encoded);
    check(result.success, "Decode succeeds");
    check(result.data.empty(), "Decoded data is empty");
}

static void testRoundTripOneByte() {
    std::cout << "[Test: Round-trip 1 byte]\n";
    std::vector<u8> data = {0x42};
    auto encoded = blteEncode(data);
    auto result = blteDecode(encoded);
    check(result.success, "Decode succeeds");
    check(result.data == data, "Decoded data matches original");
}

static void testRoundTrip64K() {
    std::cout << "[Test: Round-trip 64KB]\n";
    std::vector<u8> data(65536);
    std::iota(data.begin(), data.end(), u8(0));

    auto encoded = blteEncode(data);
    auto result = blteDecode(encoded);
    check(result.success, "Decode succeeds");
    check(result.data == data, "Decoded data matches original (64KB)");
}

static void testRoundTrip1MB() {
    std::cout << "[Test: Round-trip 1MB (multi-frame)]\n";
    std::vector<u8> data(1024 * 1024);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<u8>(i * 37 + 13);

    auto encoded = blteEncode(data);
    auto result = blteDecode(encoded);
    check(result.success, "Decode succeeds");
    check(result.data == data, "Decoded data matches original (1MB)");
}

static void testRoundTripRaw() {
    std::cout << "[Test: Round-trip with compression disabled]\n";
    std::vector<u8> data(200);
    std::iota(data.begin(), data.end(), u8(0));

    BlteEncodeOptions opts;
    opts.compress = false;
    auto encoded = blteEncode(data, opts);
    auto result = blteDecode(encoded);
    check(result.success, "Decode succeeds (raw mode)");
    check(result.data == data, "Decoded data matches original (raw mode)");
}

static void testRoundTripSmallFrames() {
    std::cout << "[Test: Round-trip with small frame size]\n";
    std::vector<u8> data(500);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<u8>(i ^ 0xAA);

    BlteEncodeOptions opts;
    opts.frameSize = 100; // Small frames → many frames.
    auto encoded = blteEncode(data, opts);
    auto result = blteDecode(encoded);
    check(result.success, "Decode succeeds (small frames)");
    check(result.data == data, "Decoded data matches original (small frames)");
}

// ============================================================================
// Error/Edge Cases
// ============================================================================

static void testDecodeEmpty() {
    std::cout << "[Test: Decode empty input]\n";
    auto result = blteDecode({});
    check(!result.success, "Decode fails on empty input");
    check(!result.error.empty(), "Error message is non-empty");
}

static void testDecodeGarbage() {
    std::cout << "[Test: Decode garbage data]\n";
    std::vector<u8> garbage = {0x01, 0x02, 0x03, 0x04};
    auto result = blteDecode(garbage);
    check(!result.success, "Decode fails on garbage data");
}

static void testDecodeTruncated() {
    std::cout << "[Test: Decode truncated BLTE]\n";
    // BLTE magic + a few bytes — not enough for a full container.
    std::vector<u8> truncated = {'B', 'L', 'T', 'E', 0x00, 0x00};
    auto result = blteDecode(truncated);
    check(!result.success, "Decode fails on truncated data");
}

int main() {
    std::cout << "=== CASC BLTE Tests ===\n\n";

    testRoundTripEmpty();
    testRoundTripOneByte();
    testRoundTrip64K();
    testRoundTrip1MB();
    testRoundTripRaw();
    testRoundTripSmallFrames();
    testDecodeEmpty();
    testDecodeGarbage();
    testDecodeTruncated();

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed > 0 ? 1 : 0;
}

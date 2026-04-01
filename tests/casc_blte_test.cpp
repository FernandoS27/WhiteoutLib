// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// casc_blte_test: Validates BLTE encode/decode round-trip and edge cases.

#include "../src/whiteout/storages/casc/blte.h"
#include "../src/whiteout/storages/casc/crypto.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <numeric>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

// ============================================================================
// Round-Trip Tests
// ============================================================================

TEST_CASE("BLTE round-trip empty data", "[casc][blte]") {
    std::vector<u8> empty;
    auto encoded = blteEncode(empty);
    CHECK_FALSE(encoded.empty());

    auto result = blteDecode(encoded);
    CHECK(result.success);
    CHECK(result.data.empty());
}

TEST_CASE("BLTE round-trip 1 byte", "[casc][blte]") {
    std::vector<u8> data = {0x42};
    auto encoded = blteEncode(data);
    auto result = blteDecode(encoded);
    CHECK(result.success);
    CHECK(result.data == data);
}

TEST_CASE("BLTE round-trip 64KB", "[casc][blte]") {
    std::vector<u8> data(65536);
    std::iota(data.begin(), data.end(), u8(0));

    auto encoded = blteEncode(data);
    auto result = blteDecode(encoded);
    CHECK(result.success);
    CHECK(result.data == data);
}

TEST_CASE("BLTE round-trip 1MB multi-frame", "[casc][blte]") {
    std::vector<u8> data(1024 * 1024);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<u8>(i * 37 + 13);

    auto encoded = blteEncode(data);
    auto result = blteDecode(encoded);
    CHECK(result.success);
    CHECK(result.data == data);
}

TEST_CASE("BLTE round-trip raw mode", "[casc][blte]") {
    std::vector<u8> data(200);
    std::iota(data.begin(), data.end(), u8(0));

    BlteEncodeOptions opts;
    opts.compress = false;
    auto encoded = blteEncode(data, opts);
    auto result = blteDecode(encoded);
    CHECK(result.success);
    CHECK(result.data == data);
}

TEST_CASE("BLTE round-trip small frames", "[casc][blte]") {
    std::vector<u8> data(500);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<u8>(i ^ 0xAA);

    BlteEncodeOptions opts;
    opts.frameSize = 100;
    auto encoded = blteEncode(data, opts);
    auto result = blteDecode(encoded);
    CHECK(result.success);
    CHECK(result.data == data);
}

// ============================================================================
// Error/Edge Cases
// ============================================================================

TEST_CASE("BLTE decode empty input", "[casc][blte]") {
    auto result = blteDecode({});
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("BLTE decode garbage data", "[casc][blte]") {
    std::vector<u8> garbage = {0x01, 0x02, 0x03, 0x04};
    auto result = blteDecode(garbage);
    CHECK_FALSE(result.success);
}

TEST_CASE("BLTE decode truncated", "[casc][blte]") {
    std::vector<u8> truncated = {'B', 'L', 'T', 'E', 0x00, 0x00};
    auto result = blteDecode(truncated);
    CHECK_FALSE(result.success);
}

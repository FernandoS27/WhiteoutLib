// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// deflate_test: Unit tests for zlib_compress/zlib_decompress (deflate.h)
/// and the optimized zlibInflateFast (inflate_fast.h).
///
/// Tests cover:
///   - Round-trip: compress → decompress for various data patterns/sizes.
///   - Cross-validation: inflate_fast produces identical output to zlib_decompress.
///   - Edge cases: empty, 1-byte, all-zero, incompressible, very large.
///   - DEFLATE block types: stored, fixed Huffman, dynamic Huffman.
///   - Error handling: truncated, corrupted, invalid zlib headers.
///   - Back-reference patterns: distance=1 (memset), short overlap, long overlap.

#include "../src/whiteout/common/deflate.h"
#include "../src/whiteout/storages/common/inflate_fast.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

using namespace whiteout;

// Shorthand for the fast inflate.
static std::vector<u8> inflateFast(std::span<const u8> src, size_t expectedSize = 0) {
    return storages::common::zlibInflateFast(src, expectedSize);
}

// ============================================================================
// Helpers
// ============================================================================

/// Generate pseudo-random data with a fixed seed for reproducibility.
static std::vector<u8> randomData(size_t size, u32 seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<u32> dist(0, 255);
    std::vector<u8> data(size);
    for (auto& b : data)
        b = static_cast<u8>(dist(rng));
    return data;
}

/// Generate highly compressible data (repeating pattern).
static std::vector<u8> repeatPattern(const std::vector<u8>& pattern, size_t totalSize) {
    std::vector<u8> data(totalSize);
    for (size_t i = 0; i < totalSize; ++i)
        data[i] = pattern[i % pattern.size()];
    return data;
}

/// Generate data that exercises distance=1 back-references (run of same byte).
static std::vector<u8> runOfByte(u8 value, size_t count) {
    return std::vector<u8>(count, value);
}

// ============================================================================
// deflate.h: zlib_compress / zlib_decompress round-trip
// ============================================================================

TEST_CASE("deflate round-trip empty", "[deflate]") {
    std::vector<u8> empty;
    auto compressed = zlib_compress(empty);
    CHECK_FALSE(compressed.empty());

    auto decompressed = zlib_decompress(compressed);
    CHECK(decompressed.empty());
}

TEST_CASE("deflate round-trip 1 byte", "[deflate]") {
    std::vector<u8> data = {0xAB};
    auto compressed = zlib_compress(data);
    auto decompressed = zlib_decompress(compressed);
    CHECK(decompressed == data);
}

TEST_CASE("deflate round-trip sequential 256 bytes", "[deflate]") {
    std::vector<u8> data(256);
    std::iota(data.begin(), data.end(), u8(0));

    auto compressed = zlib_compress(data);
    auto decompressed = zlib_decompress(compressed);
    CHECK(decompressed == data);
}

TEST_CASE("deflate round-trip 64KB", "[deflate]") {
    std::vector<u8> data(65536);
    std::iota(data.begin(), data.end(), u8(0));

    auto compressed = zlib_compress(data);
    auto decompressed = zlib_decompress(compressed);
    CHECK(decompressed == data);
}

TEST_CASE("deflate round-trip 1MB random", "[deflate]") {
    auto data = randomData(1024 * 1024);
    auto compressed = zlib_compress(data);
    auto decompressed = zlib_decompress(compressed);
    CHECK(decompressed == data);
}

TEST_CASE("deflate round-trip all zeros", "[deflate]") {
    auto data = runOfByte(0x00, 100000);
    auto compressed = zlib_compress(data);
    CHECK(compressed.size() < data.size()); // Should compress well.
    auto decompressed = zlib_decompress(compressed);
    CHECK(decompressed == data);
}

TEST_CASE("deflate round-trip repeating pattern", "[deflate]") {
    auto data = repeatPattern({0xDE, 0xAD, 0xBE, 0xEF}, 200000);
    auto compressed = zlib_compress(data);
    CHECK(compressed.size() < data.size());
    auto decompressed = zlib_decompress(compressed);
    CHECK(decompressed == data);
}

TEST_CASE("deflate round-trip single repeated byte", "[deflate]") {
    // Exercises distance=1 back-references heavily.
    auto data = runOfByte(0xFF, 300000);
    auto compressed = zlib_compress(data);
    auto decompressed = zlib_decompress(compressed);
    CHECK(decompressed == data);
}

TEST_CASE("deflate round-trip short repeating patterns", "[deflate]") {
    // Distance 2-7 back-references (short overlap copy path).
    for (size_t patLen = 2; patLen <= 7; ++patLen) {
        std::vector<u8> pattern(patLen);
        std::iota(pattern.begin(), pattern.end(), u8(0x10));
        auto data = repeatPattern(pattern, 50000);
        auto compressed = zlib_compress(data);
        auto decompressed = zlib_decompress(compressed);
        CHECK(decompressed == data);
    }
}

TEST_CASE("deflate round-trip with expectedSize hint", "[deflate]") {
    auto data = randomData(8192);
    auto compressed = zlib_compress(data);
    auto decompressed = zlib_decompress(compressed, nullptr, data.size());
    CHECK(decompressed == data);
}

TEST_CASE("deflate error reporting", "[deflate]") {
    std::string error;

    // Invalid zlib header.
    std::vector<u8> garbage = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    auto result = zlib_decompress(garbage, &error);
    CHECK(result.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("deflate rejects too-short input", "[deflate]") {
    // Less than 6 bytes (zlib header + adler-32 minimum).
    std::vector<u8> tiny = {0x78, 0x9C, 0x03};
    auto result = zlib_decompress(tiny);
    CHECK(result.empty());
}

// ============================================================================
// inflate_fast: zlibInflateFast round-trip and cross-validation
// ============================================================================

TEST_CASE("inflate_fast round-trip empty", "[inflate_fast]") {
    std::vector<u8> empty;
    auto compressed = zlib_compress(empty);
    auto result = inflateFast(compressed, 0);
    CHECK(result.empty());
}

TEST_CASE("inflate_fast round-trip 1 byte", "[inflate_fast]") {
    std::vector<u8> data = {0x42};
    auto compressed = zlib_compress(data);
    auto result = inflateFast(compressed, data.size());
    CHECK(result == data);
}

TEST_CASE("inflate_fast round-trip 64KB with known size", "[inflate_fast]") {
    std::vector<u8> data(65536);
    std::iota(data.begin(), data.end(), u8(0));

    auto compressed = zlib_compress(data);
    auto result = inflateFast(compressed, data.size());
    CHECK(result == data);
}

TEST_CASE("inflate_fast round-trip 64KB without known size", "[inflate_fast]") {
    // Use random data — the heuristic (src.size() * 4) only works when
    // compression ratio is modest.  Highly compressible data needs expectedSize.
    auto data = randomData(65536);

    auto compressed = zlib_compress(data);
    auto result = inflateFast(compressed, 0); // Heuristic allocation.
    CHECK(result == data);
}

TEST_CASE("inflate_fast round-trip 1MB random", "[inflate_fast]") {
    auto data = randomData(1024 * 1024);
    auto compressed = zlib_compress(data);
    auto result = inflateFast(compressed, data.size());
    CHECK(result == data);
}

TEST_CASE("inflate_fast round-trip all zeros", "[inflate_fast]") {
    auto data = runOfByte(0x00, 100000);
    auto compressed = zlib_compress(data);
    auto result = inflateFast(compressed, data.size());
    CHECK(result == data);
}

TEST_CASE("inflate_fast round-trip repeating pattern", "[inflate_fast]") {
    auto data = repeatPattern({0xDE, 0xAD, 0xBE, 0xEF}, 200000);
    auto compressed = zlib_compress(data);
    auto result = inflateFast(compressed, data.size());
    CHECK(result == data);
}

TEST_CASE("inflate_fast round-trip single repeated byte", "[inflate_fast]") {
    auto data = runOfByte(0xAA, 300000);
    auto compressed = zlib_compress(data);
    auto result = inflateFast(compressed, data.size());
    CHECK(result == data);
}

TEST_CASE("inflate_fast round-trip short overlap distances", "[inflate_fast]") {
    // Exercises distance 2-7 (doubling copy path in inflate_fast).
    for (size_t patLen = 2; patLen <= 7; ++patLen) {
        std::vector<u8> pattern(patLen);
        std::iota(pattern.begin(), pattern.end(), u8(0x20));
        auto data = repeatPattern(pattern, 50000);
        auto compressed = zlib_compress(data);

        auto result = inflateFast(compressed, data.size());
        CAPTURE(patLen);
        CHECK(result == data);
    }
}

TEST_CASE("inflate_fast round-trip distance >= 8 (wide copy)", "[inflate_fast]") {
    // Exercises the 8-byte wide copy path.
    std::vector<u8> pattern(16);
    std::iota(pattern.begin(), pattern.end(), u8(0x30));
    auto data = repeatPattern(pattern, 100000);
    auto compressed = zlib_compress(data);
    auto result = inflateFast(compressed, data.size());
    CHECK(result == data);
}

// ============================================================================
// Cross-validation: inflate_fast vs zlib_decompress byte-identical
// ============================================================================

TEST_CASE("cross-validate: inflate_fast == zlib_decompress (random data)", "[inflate_fast][cross]") {
    // Multiple sizes and seeds to exercise different code paths.
    for (u32 seed = 0; seed < 5; ++seed) {
        for (size_t sz : {128, 4096, 32768, 131072, 524288}) {
            auto data = randomData(sz, seed * 1000 + 1);
            auto compressed = zlib_compress(data);

            auto slow = zlib_decompress(compressed);
            auto fast = inflateFast(compressed, data.size());

            CAPTURE(seed, sz);
            REQUIRE(slow == data);
            REQUIRE(fast == data);
            CHECK(fast == slow);
        }
    }
}

TEST_CASE("cross-validate: inflate_fast == zlib_decompress (compressible data)", "[inflate_fast][cross]") {
    struct TestCase {
        std::string label;
        std::vector<u8> data;
    };
    std::vector<TestCase> cases;

    // All zeros.
    cases.push_back({"all_zeros_64K", runOfByte(0, 65536)});

    // Single repeated byte.
    cases.push_back({"single_byte_256K", runOfByte(0xCC, 262144)});

    // 4-byte repeating pattern.
    cases.push_back({"pattern4_128K", repeatPattern({1, 2, 3, 4}, 131072)});

    // 13-byte pattern (prime — not power-of-two aligned).
    cases.push_back({"pattern13_100K", repeatPattern({10,20,30,40,50,60,70,80,90,100,110,120,130}, 100000)});

    // Sequential bytes.
    {
        std::vector<u8> seq(65536);
        std::iota(seq.begin(), seq.end(), u8(0));
        cases.push_back({"sequential_64K", std::move(seq)});
    }

    // Two-value alternating.
    cases.push_back({"alternating_64K", repeatPattern({0x00, 0xFF}, 65536)});

    for (auto& [label, data] : cases) {
        auto compressed = zlib_compress(data);
        auto slow = zlib_decompress(compressed);
        auto fast = inflateFast(compressed, data.size());

        CAPTURE(label);
        REQUIRE(slow == data);
        REQUIRE(fast == data);
    }
}

// ============================================================================
// inflate_fast: edge cases and error handling
// ============================================================================

TEST_CASE("inflate_fast rejects empty input", "[inflate_fast][error]") {
    auto result = inflateFast({}, 0);
    CHECK(result.empty());
}

TEST_CASE("inflate_fast rejects too-short input", "[inflate_fast][error]") {
    std::vector<u8> tiny = {0x78, 0x9C, 0x03};
    auto result = inflateFast(tiny, 0);
    CHECK(result.empty());
}

TEST_CASE("inflate_fast rejects invalid zlib header", "[inflate_fast][error]") {
    std::vector<u8> bad = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    auto result = inflateFast(bad, 0);
    CHECK(result.empty());
}

TEST_CASE("inflate_fast rejects preset dictionary flag", "[inflate_fast][error]") {
    // CMF=0x78 (deflate, window=7), FLG=0xBB (FDICT set, FCHECK adjusted).
    // 0x78BB: (0x78 << 8 | 0xBB) = 0x78BB, 0x78BB % 31 = 0.
    std::vector<u8> fdict = {0x78, 0xBB, 0x00, 0x00, 0x00, 0x00};
    auto result = inflateFast(fdict, 0);
    CHECK(result.empty());
}

TEST_CASE("inflate_fast rejects truncated compressed data", "[inflate_fast][error]") {
    auto data = randomData(10000);
    auto compressed = zlib_compress(data);

    // Truncate at various points.
    for (size_t cutAt : {6ULL, compressed.size() / 4, compressed.size() / 2}) {
        std::vector<u8> truncated(compressed.begin(), compressed.begin() + cutAt);
        auto result = inflateFast(truncated, data.size());
        // Must not crash.  May return empty (failure) or partial data that
        // doesn't match the original.  The key invariant is correctness:
        // it should never return the full correct data from truncated input.
        CHECK(result != data);
    }
}

TEST_CASE("inflate_fast rejects corrupted data", "[inflate_fast][error]") {
    auto data = randomData(50000);
    auto compressed = zlib_compress(data);

    // Flip bits in the middle of compressed data.
    auto corrupted = compressed;
    corrupted[compressed.size() / 2] ^= 0xFF;
    corrupted[compressed.size() / 2 + 1] ^= 0x55;

    auto result = inflateFast(corrupted, data.size());
    // Either fails (empty) or produces wrong data — must not crash.
    // We can't assert empty because corruption might still produce valid
    // DEFLATE tokens; the key invariant is no crash/UB.
    CHECK((result.empty() || result != data));
}

// ============================================================================
// Size boundary tests: exercise the fast/careful loop transition
// ============================================================================

TEST_CASE("inflate_fast near output safety margin (258 bytes)", "[inflate_fast][boundary]") {
    // Data sizes near the wpSafeEnd = outCap - 258 boundary.
    for (size_t sz : {200, 258, 259, 300, 512, 1024}) {
        auto data = randomData(sz, static_cast<u32>(sz));
        auto compressed = zlib_compress(data);
        auto result = inflateFast(compressed, data.size());
        CAPTURE(sz);
        CHECK(result == data);
    }
}

TEST_CASE("inflate_fast very small data (careful loop only)", "[inflate_fast][boundary]") {
    // Data so small the fast loop may never run (output < 258 bytes).
    for (size_t sz = 1; sz <= 260; sz += 13) {
        auto data = randomData(sz, static_cast<u32>(sz + 100));
        auto compressed = zlib_compress(data);
        auto result = inflateFast(compressed, data.size());
        CAPTURE(sz);
        CHECK(result == data);
    }
}

TEST_CASE("inflate_fast 4MB stress", "[inflate_fast][stress]") {
    auto data = randomData(4 * 1024 * 1024, 999);
    auto compressed = zlib_compress(data);

    auto fast = inflateFast(compressed, data.size());
    REQUIRE(fast.size() == data.size());
    CHECK(fast == data);
}

TEST_CASE("inflate_fast 4MB compressible stress", "[inflate_fast][stress]") {
    auto data = repeatPattern({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}, 4 * 1024 * 1024);
    auto compressed = zlib_compress(data);

    auto fast = inflateFast(compressed, data.size());
    REQUIRE(fast.size() == data.size());
    CHECK(fast == data);
}

// ============================================================================
// Benchmarks: zlib_decompress (general) vs zlibInflateFast (optimized)
// ============================================================================
// Run with: deflate_test.exe "[bench]" --benchmark-samples 20

TEST_CASE("bench: 1MB random data inflate", "[bench]") {
    auto data = randomData(1024 * 1024, 7777);
    auto compressed = zlib_compress(data);

    // Verify both produce correct output first.
    REQUIRE(zlib_decompress(compressed) == data);
    REQUIRE(inflateFast(compressed, data.size()) == data);

    BENCHMARK("zlib_decompress 1MB random") {
        return zlib_decompress(compressed);
    };
    BENCHMARK("zlibInflateFast 1MB random (known size)") {
        return inflateFast(compressed, data.size());
    };
    BENCHMARK("zlibInflateFast 1MB random (unknown size)") {
        return inflateFast(compressed, 0);
    };
}

TEST_CASE("bench: 1MB compressible inflate", "[bench]") {
    auto data = repeatPattern({0xDE, 0xAD, 0xBE, 0xEF}, 1024 * 1024);
    auto compressed = zlib_compress(data);

    REQUIRE(zlib_decompress(compressed) == data);
    REQUIRE(inflateFast(compressed, data.size()) == data);

    BENCHMARK("zlib_decompress 1MB pattern") {
        return zlib_decompress(compressed);
    };
    BENCHMARK("zlibInflateFast 1MB pattern (known size)") {
        return inflateFast(compressed, data.size());
    };
}

TEST_CASE("bench: 1MB all-zeros inflate", "[bench]") {
    auto data = runOfByte(0x00, 1024 * 1024);
    auto compressed = zlib_compress(data);

    REQUIRE(zlib_decompress(compressed) == data);
    REQUIRE(inflateFast(compressed, data.size()) == data);

    BENCHMARK("zlib_decompress 1MB zeros") {
        return zlib_decompress(compressed);
    };
    BENCHMARK("zlibInflateFast 1MB zeros (known size)") {
        return inflateFast(compressed, data.size());
    };
}

TEST_CASE("bench: 4MB random data inflate", "[bench]") {
    auto data = randomData(4 * 1024 * 1024, 12345);
    auto compressed = zlib_compress(data);

    REQUIRE(zlib_decompress(compressed) == data);
    REQUIRE(inflateFast(compressed, data.size()) == data);

    BENCHMARK("zlib_decompress 4MB random") {
        return zlib_decompress(compressed);
    };
    BENCHMARK("zlibInflateFast 4MB random (known size)") {
        return inflateFast(compressed, data.size());
    };
}

TEST_CASE("bench: 4MB compressible inflate", "[bench]") {
    auto data = repeatPattern({1, 2, 3, 4, 5, 6, 7, 8}, 4 * 1024 * 1024);
    auto compressed = zlib_compress(data);

    REQUIRE(zlib_decompress(compressed) == data);
    REQUIRE(inflateFast(compressed, data.size()) == data);

    BENCHMARK("zlib_decompress 4MB pattern") {
        return zlib_decompress(compressed);
    };
    BENCHMARK("zlibInflateFast 4MB pattern (known size)") {
        return inflateFast(compressed, data.size());
    };
}

TEST_CASE("bench: 64KB small frame inflate", "[bench]") {
    // Typical BLTE frame size.
    auto data = randomData(65536, 9999);
    auto compressed = zlib_compress(data);

    REQUIRE(zlib_decompress(compressed) == data);
    REQUIRE(inflateFast(compressed, data.size()) == data);

    BENCHMARK("zlib_decompress 64KB random") {
        return zlib_decompress(compressed);
    };
    BENCHMARK("zlibInflateFast 64KB random (known size)") {
        return inflateFast(compressed, data.size());
    };
}

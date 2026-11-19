// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MPQ codec round-trip test suite.
// Tests individual compress/decompress functions for all codecs,
// plus the mpqCompress/mpqDecompress integration layer.

// Internal codec headers (not public API — test-only usage).
#include <catch2/catch_all.hpp>

#include "../src/whiteout/storages/mpq/codecs/compression.h"
#include "../src/whiteout/storages/mpq/crypto.h"
#include "../src/whiteout/storages/mpq/file_data.h"
#include "../src/whiteout/storages/mpq/codecs/sparse.h"
#include "../src/whiteout/storages/mpq/codecs/huffman.h"
#include "../src/whiteout/storages/common/codecs/bzip2.h"
#include "../src/whiteout/storages/mpq/codecs/adpcm.h"
#include "../src/whiteout/storages/mpq/codecs/pkware.h"
#include "../src/whiteout/storages/common/codecs/lzma.h"
#include "../src/whiteout/storages/common/zlib.h"

#include <whiteout/common_types.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using whiteout::u8;
using whiteout::u16;
using whiteout::u32;
namespace mpq = whiteout::storages::mpq;
namespace common = whiteout::storages::common;

// ============================================================================
// Test harness
// ============================================================================
static void pass(const char* fmt, ...) {
    
    std::printf("  PASS: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

static void fail(const char* fmt, ...) {
    
    std::printf("  FAIL: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

static void expect(bool condition, const char* fmt, ...) {
    if (condition)
        return;
    char message[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    std::printf("  FAIL: %s\n", message);
    FAIL_CHECK(message);
}

// ============================================================================
// Test data generators
// ============================================================================

static std::vector<u8> makeCompressibleData(size_t size) {
    static constexpr char kPhrase[] = "whiteout-mpq-codec-test-data-";
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>(kPhrase[i % (sizeof(kPhrase) - 1)]);
    }
    return data;
}

static std::vector<u8> makePatternData(size_t size, u8 seed) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>((i * 13 + (i / 17) * 7 + seed) & 0xFF);
    }
    return data;
}

static std::vector<u8> makeZeroData(size_t size) {
    return std::vector<u8>(size, 0);
}

/// Make fake 16-bit PCM audio data (little-endian).
static std::vector<u8> makePcmData(size_t sampleCount, int channelCount) {
    std::vector<u8> data(sampleCount * channelCount * 2);
    for (size_t i = 0; i < sampleCount * static_cast<size_t>(channelCount); i++) {
        // Generate a simple waveform with some variation.
        short sample = static_cast<short>((i * 137 + (i / 3) * 53) % 65536 - 32768);
        // Limit range to make ADPCM happy (not too wild).
        sample = static_cast<short>(sample / 4);
        data[i * 2]     = static_cast<u8>(sample & 0xFF);
        data[i * 2 + 1] = static_cast<u8>((sample >> 8) & 0xFF);
    }
    return data;
}

// ============================================================================
// Sparse codec tests
// ============================================================================

TEST_CASE("testSparseRoundTrip", "[mpq][codec]") {
    std::printf("\n=== Sparse Codec Tests ===\n");

    // Test with zeros-heavy data (ideal for sparse).
    {
        auto data = makeZeroData(512);
        auto compressed = mpq::sparseCompress(data);
        expect(!compressed.empty(), "sparse compress zeros: not empty");
        expect(compressed.size() < data.size(), "sparse compress zeros: actually compressed");
        auto decompressed = mpq::sparseDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "sparse decompress zeros: correct size");
        expect(decompressed == data, "sparse round-trip zeros: data matches");
        if (decompressed == data) pass("sparse round-trip: 512 zeros");
    }

    // Test with compressible data (has some zero runs).
    {
        auto data = makeCompressibleData(1024);
        auto compressed = mpq::sparseCompress(data);
        expect(!compressed.empty(), "sparse compress text: not empty");
        auto decompressed = mpq::sparseDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "sparse decompress text: correct size");
        expect(decompressed == data, "sparse round-trip text: data matches");
        if (decompressed == data) pass("sparse round-trip: 1024 text bytes");
    }

    // Test with random-like pattern data.
    {
        auto data = makePatternData(256, 42);
        auto compressed = mpq::sparseCompress(data);
        expect(!compressed.empty(), "sparse compress pattern: not empty");
        auto decompressed = mpq::sparseDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "sparse decompress pattern: correct size");
        expect(decompressed == data, "sparse round-trip pattern: data matches");
        if (decompressed == data) pass("sparse round-trip: 256 pattern bytes");
    }

    // Test with small data (1 byte).
    {
        std::vector<u8> data = {0x42};
        auto compressed = mpq::sparseCompress(data);
        expect(!compressed.empty(), "sparse compress 1 byte: not empty");
        auto decompressed = mpq::sparseDecompress(compressed, data.size());
        expect(decompressed == data, "sparse round-trip 1 byte: data matches");
        if (decompressed == data) pass("sparse round-trip: 1 byte");
    }

    // Test mixed zeros and non-zeros.
    {
        std::vector<u8> data(128, 0);
        // Insert some non-zero spans.
        for (int i = 0; i < 10; i++) data[i] = static_cast<u8>(i + 1);
        for (int i = 60; i < 70; i++) data[i] = static_cast<u8>(i);
        for (int i = 120; i < 128; i++) data[i] = 0xFF;

        auto compressed = mpq::sparseCompress(data);
        expect(!compressed.empty(), "sparse compress mixed: not empty");
        auto decompressed = mpq::sparseDecompress(compressed, data.size());
        expect(decompressed == data, "sparse round-trip mixed: data matches");
        if (decompressed == data) pass("sparse round-trip: mixed zeros/nonzeros");
    }
}

// ============================================================================
// Huffman codec tests
// ============================================================================

TEST_CASE("testHuffmanRoundTrip", "[mpq][codec]") {
    std::printf("\n=== Huffman Codec Tests ===\n");

    // Test with compressible data.
    {
        auto data = makeCompressibleData(512);
        auto compressed = mpq::huffmanCompress(data, 0);
        expect(!compressed.empty(), "huffman compress text: not empty");
        auto decompressed = mpq::huffmanDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "huffman decompress text: correct size");
        expect(decompressed == data, "huffman round-trip text: data matches");
        if (decompressed == data) pass("huffman round-trip: 512 text (type 0)");
    }

    // Test with pattern data.
    {
        auto data = makePatternData(300, 77);
        auto compressed = mpq::huffmanCompress(data);
        expect(!compressed.empty(), "huffman compress pattern: not empty");
        auto decompressed = mpq::huffmanDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "huffman decompress pattern: correct size");
        expect(decompressed == data, "huffman round-trip pattern: data matches");
        if (decompressed == data) pass("huffman round-trip: 300 pattern bytes");
    }

    // Test with all zeros.
    {
        auto data = makeZeroData(256);
        auto compressed = mpq::huffmanCompress(data, 0);
        expect(!compressed.empty(), "huffman compress zeros: not empty");
        auto decompressed = mpq::huffmanDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "huffman decompress zeros: correct size");
        expect(decompressed == data, "huffman round-trip zeros: data matches");
        if (decompressed == data) pass("huffman round-trip: 256 zeros");
    }

    // Test with 1 byte.
    {
        std::vector<u8> data = {0xAB};
        auto compressed = mpq::huffmanCompress(data, 0);
        expect(!compressed.empty(), "huffman compress 1 byte: not empty");
        auto decompressed = mpq::huffmanDecompress(compressed, data.size());
        expect(decompressed == data, "huffman round-trip 1 byte: data matches");
        if (decompressed == data) pass("huffman round-trip: 1 byte");
    }

    // Test different compression types work with round-trip.
    for (int compType = 0; compType <= 8; compType++) {
        auto data = makeCompressibleData(200);
        auto compressed = mpq::huffmanCompress(data, static_cast<whiteout::u32>(compType));
        expect(!compressed.empty(), "huffman compress type %d: not empty", compType);
        auto decompressed = mpq::huffmanDecompress(compressed, data.size());
        bool ok = (decompressed == data);
        expect(ok, "huffman round-trip type %d: data matches", compType);
        if (ok) pass("huffman round-trip: type %d", compType);
    }
}

// ============================================================================
// BZip2 codec tests
// ============================================================================

TEST_CASE("testBzip2RoundTrip", "[mpq][codec]") {
    std::printf("\n=== BZip2 Codec Tests ===\n");

    // Test with compressible data.
    {
        auto data = makeCompressibleData(1024);
        auto compressed = common::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress text: not empty");
        expect(compressed.size() < data.size(), "bzip2 compress text: actually compressed");
        auto decompressed = common::bzip2Decompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "bzip2 decompress text: correct size");
        expect(decompressed == data, "bzip2 round-trip text: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 1024 text bytes");
    }

    // Test with pattern data.
    {
        auto data = makePatternData(500, 33);
        auto compressed = common::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress pattern: not empty");
        auto decompressed = common::bzip2Decompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "bzip2 decompress pattern: correct size");
        expect(decompressed == data, "bzip2 round-trip pattern: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 500 pattern bytes");
    }

    // Test with all zeros.
    {
        auto data = makeZeroData(512);
        auto compressed = common::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress zeros: not empty");
        auto decompressed = common::bzip2Decompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "bzip2 decompress zeros: correct size");
        expect(decompressed == data, "bzip2 round-trip zeros: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 512 zeros");
    }

    // Test with small data.
    {
        std::vector<u8> data = {0x01, 0x02, 0x03, 0x04, 0x05};
        auto compressed = common::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress 5 bytes: not empty");
        auto decompressed = common::bzip2Decompress(compressed, data.size());
        expect(decompressed == data, "bzip2 round-trip 5 bytes: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 5 bytes");
    }

    // Test larger data.
    {
        auto data = makeCompressibleData(4096);
        auto compressed = common::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress 4096: not empty");
        auto decompressed = common::bzip2Decompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "bzip2 decompress 4096: correct size");
        expect(decompressed == data, "bzip2 round-trip 4096: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 4096 bytes");
    }
}

// ============================================================================
// ADPCM codec tests
// ============================================================================

TEST_CASE("testAdpcmRoundTrip", "[mpq][codec]") {
    std::printf("\n=== ADPCM Codec Tests ===\n");

    // Note: ADPCM is lossy, so we test that compress→decompress produces
    // data of the correct size and that the values are reasonably close.

    // Mono round-trip.
    {
        auto pcm = makePcmData(256, 1);
        auto compressed = mpq::adpcmCompress(pcm, 1);
        expect(!compressed.empty(), "adpcm mono compress: not empty");
        expect(compressed.size() < pcm.size(), "adpcm mono compress: smaller than original");
        auto decompressed = mpq::adpcmDecompress(compressed, pcm.size(), 1);
        expect(decompressed.size() == pcm.size(), "adpcm mono decompress: correct size (%zu vs %zu)",
               decompressed.size(), pcm.size());
        if (decompressed.size() == pcm.size()) pass("adpcm mono round-trip: 256 samples, correct size");
    }

    // Stereo round-trip.
    {
        auto pcm = makePcmData(128, 2);
        auto compressed = mpq::adpcmCompress(pcm, 2);
        expect(!compressed.empty(), "adpcm stereo compress: not empty");
        expect(compressed.size() < pcm.size(), "adpcm stereo compress: smaller than original");
        auto decompressed = mpq::adpcmDecompress(compressed, pcm.size(), 2);
        expect(decompressed.size() == pcm.size(), "adpcm stereo decompress: correct size (%zu vs %zu)",
               decompressed.size(), pcm.size());
        if (decompressed.size() == pcm.size()) pass("adpcm stereo round-trip: 128 samples, correct size");
    }

    // Silence (all zeros) mono.
    {
        auto pcm = makeZeroData(512);
        auto compressed = mpq::adpcmCompress(pcm, 1);
        expect(!compressed.empty(), "adpcm mono silence compress: not empty");
        auto decompressed = mpq::adpcmDecompress(compressed, pcm.size(), 1);
        expect(decompressed.size() == pcm.size(), "adpcm mono silence: correct size");
        if (decompressed.size() == pcm.size()) pass("adpcm mono silence: correct size round-trip");
    }

    // Verify lossy accuracy: max sample error should be bounded.
    {
        auto pcm = makePcmData(200, 1);
        auto compressed = mpq::adpcmCompress(pcm, 1);
        auto decompressed = mpq::adpcmDecompress(compressed, pcm.size(), 1);
        if (decompressed.size() == pcm.size()) {
            int maxError = 0;
            size_t samples = pcm.size() / 2;
            for (size_t i = 0; i < samples; i++) {
                short orig = static_cast<short>(
                    static_cast<u16>(pcm[i*2]) | (static_cast<u16>(pcm[i*2+1]) << 8));
                short decoded = static_cast<short>(
                    static_cast<u16>(decompressed[i*2]) | (static_cast<u16>(decompressed[i*2+1]) << 8));
                int err = std::abs(static_cast<int>(orig) - static_cast<int>(decoded));
                if (err > maxError) maxError = err;
            }
            // With bitShift=1, error should be modest for our tame test waveform.
            expect(maxError < 4000, "adpcm mono accuracy: max error %d < 4000", maxError);
            if (maxError < 4000) pass("adpcm mono accuracy: max sample error = %d", maxError);
        }
    }
}

// ============================================================================
// PKware codec tests
// ============================================================================

TEST_CASE("testPkwareRoundTrip", "[mpq][codec]") {
    std::printf("\n=== PKware Codec Tests ===\n");

    // Test with compressible data.
    {
        auto data = makeCompressibleData(1024);
        auto compressed = mpq::pkwareImplode(data);
        expect(!compressed.empty(), "pkware compress text: not empty");
        expect(compressed.size() < data.size() / 4, "pkware compress text: actually compressed (%zu)",
               compressed.size());
        auto decompressed = mpq::pkwareExplode(compressed, data.size());
        expect(decompressed.size() == data.size(), "pkware decompress text: correct size");
        expect(decompressed == data, "pkware round-trip text: data matches");
        if (decompressed == data) pass("pkware round-trip: 1024 text bytes");
    }

    // Test with pattern data.
    {
        auto data = makePatternData(512, 99);
        auto compressed = mpq::pkwareImplode(data);
        expect(!compressed.empty(), "pkware compress pattern: not empty");
        auto decompressed = mpq::pkwareExplode(compressed, data.size());
        expect(decompressed.size() == data.size(), "pkware decompress pattern: correct size");
        expect(decompressed == data, "pkware round-trip pattern: data matches");
        if (decompressed == data) pass("pkware round-trip: 512 pattern bytes");
    }

    // Test larger data.
    {
        auto data = makeCompressibleData(4096);
        auto compressed = mpq::pkwareImplode(data);
        expect(!compressed.empty(), "pkware compress 4096: not empty");
        auto decompressed = mpq::pkwareExplode(compressed, data.size());
        expect(decompressed.size() == data.size(), "pkware decompress 4096: correct size");
        expect(decompressed == data, "pkware round-trip 4096: data matches");
        if (decompressed == data) pass("pkware round-trip: 4096 bytes");
    }
}

// ============================================================================
// Zlib codec tests
// ============================================================================

TEST_CASE("testZlibRoundTrip", "[mpq][codec]") {
    std::printf("\n=== Zlib Codec Tests ===\n");
    namespace zlib = whiteout::storages::common;

    // Test with compressible data.
    {
        auto data = makeCompressibleData(1024);
        auto compressed = zlib::zlibCompress(data);
        expect(!compressed.empty(), "zlib compress text: not empty");
        expect(compressed.size() < data.size(), "zlib compress text: actually compressed");
        auto decompressed = zlib::zlibDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "zlib decompress text: correct size");
        expect(decompressed == data, "zlib round-trip text: data matches");
        if (decompressed == data) pass("zlib round-trip: 1024 text bytes");
    }

    // Test with pattern data.
    {
        auto data = makePatternData(512, 55);
        auto compressed = zlib::zlibCompress(data);
        expect(!compressed.empty(), "zlib compress pattern: not empty");
        auto decompressed = zlib::zlibDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "zlib decompress pattern: correct size");
        expect(decompressed == data, "zlib round-trip pattern: data matches");
        if (decompressed == data) pass("zlib round-trip: 512 pattern bytes");
    }

    // Test with all zeros.
    {
        auto data = makeZeroData(1024);
        auto compressed = zlib::zlibCompress(data);
        expect(!compressed.empty(), "zlib compress zeros: not empty");
        auto decompressed = zlib::zlibDecompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "zlib decompress zeros: correct size");
        expect(decompressed == data, "zlib round-trip zeros: data matches");
        if (decompressed == data) pass("zlib round-trip: 1024 zeros");
    }
}

// ============================================================================
// mpqCompress / mpqDecompress integration tests
// ============================================================================

TEST_CASE("testMpqCompressDecompress", "[mpq][codec]") {
    std::printf("\n=== mpqCompress/mpqDecompress Integration Tests ===\n");

    auto data = makeCompressibleData(2048);
    std::string error;

    // Test each compression type through the dispatcher.
    struct TestCase {
        mpq::CompressionFlag flag;
        const char* name;
    };

    TestCase cases[] = {
        {mpq::CompressionFlag::kZlib,        "zlib"},
        {mpq::CompressionFlag::kPKware,      "pkware"},
        {mpq::CompressionFlag::kBZip2,       "bzip2"},
        {mpq::CompressionFlag::kHuffman,     "huffman"},
        {mpq::CompressionFlag::kSparse,      "sparse"},
    };

    for (const auto& tc : cases) {
        error.clear();
        auto compressed = mpq::mpqCompress(data, tc.flag, &error);
        if (compressed.empty()) {
            // Some algorithms may not compress if output >= input.
            // That's acceptable, just skip.
            std::printf("  SKIP: mpq %s: compress returned empty (error: %s)\n",
                        tc.name, error.c_str());
            continue;
        }
        expect(compressed[0] == static_cast<u8>(tc.flag),
               "mpq %s: compression byte is 0x%02X (expected 0x%02X)",
               tc.name, compressed[0], static_cast<u8>(tc.flag));

        error.clear();
        auto decompressed = mpq::mpqDecompress(compressed, data.size(), &error);
        expect(decompressed.size() == data.size(),
               "mpq %s decompress: correct size (%zu vs %zu, error: %s)",
               tc.name, decompressed.size(), data.size(), error.c_str());
        expect(decompressed == data,
               "mpq %s round-trip: data matches", tc.name);
        if (decompressed == data) pass("mpq round-trip: %s (2048 bytes)", tc.name);
    }

    // ADPCM mono through dispatcher.
    {
        auto pcm = makePcmData(256, 1);
        error.clear();
        auto compressed = mpq::mpqCompress(pcm, mpq::CompressionFlag::kAdpcmMono, &error);
        if (!compressed.empty()) {
            auto decompressed = mpq::mpqDecompress(compressed, pcm.size(), &error);
            expect(decompressed.size() == pcm.size(),
                   "mpq adpcm-mono: correct size (%zu vs %zu)",
                   decompressed.size(), pcm.size());
            if (decompressed.size() == pcm.size())
                pass("mpq round-trip: adpcm-mono");
        } else {
            std::printf("  SKIP: mpq adpcm-mono: %s\n", error.c_str());
        }
    }

    // ADPCM stereo through dispatcher.
    {
        auto pcm = makePcmData(128, 2);
        error.clear();
        auto compressed = mpq::mpqCompress(pcm, mpq::CompressionFlag::kAdpcmStereo, &error);
        if (!compressed.empty()) {
            auto decompressed = mpq::mpqDecompress(compressed, pcm.size(), &error);
            expect(decompressed.size() == pcm.size(),
                   "mpq adpcm-stereo: correct size (%zu vs %zu)",
                   decompressed.size(), pcm.size());
            if (decompressed.size() == pcm.size())
                pass("mpq round-trip: adpcm-stereo");
        } else {
            std::printf("  SKIP: mpq adpcm-stereo: %s\n", error.c_str());
        }
    }

    // LZMA decompress through dispatcher (compress should fail, decompress should work).
    {
        // Verify compress returns empty (LZMA compression not supported).
        error.clear();
        auto data = makeCompressibleData(2048);
        auto compressed = mpq::mpqCompress(data, mpq::CompressionFlag::kLZMA, &error);
        expect(compressed.empty(), "mpq lzma compress: returns empty (unsupported)");
        if (compressed.empty()) pass("mpq lzma compress correctly refused");

        // Verify decompress works with a known-good buffer ("Hello, World!").
        // An MPQ LZMA sector: compression byte (0x12), filter byte (0), the 5
        // property bytes, the 8-byte uncompressed size, then the stream.
        static const u8 rawLzma[] = {
            0x5D, 0x00, 0x00, 0x80, 0x00, 0x00, 0x24, 0x19,
            0x49, 0x98, 0x6F, 0x16, 0x02, 0x89, 0x0A, 0x98,
            0xE7, 0x3F, 0xA8, 0xC3, 0x95, 0x48, 0x4D, 0xFF,
            0xFF, 0x75, 0xF0, 0x00, 0x00,
        };
        std::vector<u8> mpqBuf{0x12, 0x00};
        mpqBuf.insert(mpqBuf.end(), std::begin(rawLzma), std::begin(rawLzma) + 5);
        mpqBuf.insert(mpqBuf.end(), {13, 0, 0, 0, 0, 0, 0, 0});
        mpqBuf.insert(mpqBuf.end(), std::begin(rawLzma) + 5, std::end(rawLzma));

        error.clear();
        auto decompressed = mpq::mpqDecompress(mpqBuf, 13, &error);
        expect(decompressed.size() == 13,
               "mpq lzma decompress: correct size (%zu vs 13, error: %s)",
               decompressed.size(), error.c_str());
        static const u8 expected[] = {'H','e','l','l','o',',',' ','W','o','r','l','d','!'};
        bool match = (decompressed.size() == 13 && std::memcmp(decompressed.data(), expected, 13) == 0);
        expect(match, "mpq lzma decompress: data matches");
        if (match) pass("mpq decompress: lzma via dispatcher");
    }
}

// ============================================================================
// LZMA codec tests (decode-only — test vectors generated by Python lzma module)
// ============================================================================

TEST_CASE("testLzmaDecompress", "[mpq][codec]") {
    std::printf("\n=== LZMA Codec Tests (decode-only) ===\n");

    // Test vector 1: "Hello, World!" (13 bytes)
    {
        static const u8 compressed[] = {
            0x5D, 0x00, 0x00, 0x80, 0x00, 0x00, 0x24, 0x19,
            0x49, 0x98, 0x6F, 0x16, 0x02, 0x89, 0x0A, 0x98,
            0xE7, 0x3F, 0xA8, 0xC3, 0x95, 0x48, 0x4D, 0xFF,
            0xFF, 0x75, 0xF0, 0x00, 0x00,
        };
        static const u8 expected[] = {
            'H','e','l','l','o',',',' ','W','o','r','l','d','!'
        };
        auto result = common::lzmaDecompress(compressed, 13);
        expect(result.size() == 13, "lzma hello: correct size (%zu vs 13)", result.size());
        bool match = (result.size() == 13 && std::memcmp(result.data(), expected, 13) == 0);
        expect(match, "lzma hello: data matches");
        if (match) pass("lzma decompress: \"Hello, World!\" (13 bytes)");
    }

    // Test vector 2: repeated "ABCDEFGH" * 128 = 1024 bytes
    {
        static const u8 compressed[] = {
            0x5D, 0x00, 0x00, 0x80, 0x00, 0x00, 0x20, 0x90,
            0x84, 0x76, 0xBA, 0x8A, 0x75, 0xCF, 0xBB, 0xA6,
            0x0C, 0x5B, 0x41, 0x40, 0x03, 0xB8, 0xA9, 0x2B,
            0x49, 0xFF, 0xFF, 0xDC, 0x90, 0x00, 0x00,
        };
        // Build expected: "ABCDEFGH" repeated 128 times.
        std::vector<u8> expected(1024);
        for (size_t i = 0; i < 1024; ++i)
            expected[i] = static_cast<u8>("ABCDEFGH"[i % 8]);

        auto result = common::lzmaDecompress(compressed, 1024);
        expect(result.size() == 1024, "lzma pattern: correct size (%zu vs 1024)", result.size());
        bool match = (result == expected);
        expect(match, "lzma pattern: data matches");
        if (match) pass("lzma decompress: repeated pattern (1024 bytes)");
    }

    // Test vector 3: all zeros (256 bytes)
    {
        static const u8 compressed[] = {
            0x5D, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x6F,
            0xD9, 0x48, 0x93, 0xDF, 0xFF, 0xFF, 0x80, 0x02,
            0x00, 0x00,
        };
        std::vector<u8> expected(256, 0);
        auto result = common::lzmaDecompress(compressed, 256);
        expect(result.size() == 256, "lzma zeros: correct size (%zu vs 256)", result.size());
        bool match = (result == expected);
        expect(match, "lzma zeros: data matches");
        if (match) pass("lzma decompress: all zeros (256 bytes)");
    }

    // Error case: empty input
    {
        auto result = common::lzmaDecompress({}, 100);
        expect(result.empty(), "lzma empty input: returns empty");
        if (result.empty()) pass("lzma error: empty input handled");
    }

    // Error case: truncated input (only properties, no stream)
    {
        static const u8 truncated[] = {0x5D, 0x00, 0x00, 0x80, 0x00};
        auto result = common::lzmaDecompress(truncated, 100);
        expect(result.empty(), "lzma truncated input: returns empty");
        if (result.empty()) pass("lzma error: truncated input handled");
    }
}

// ============================================================================
// Interoperability: sectors another MPQ tool wrote (WhiteoutLib issue #4)
// ============================================================================
// Each kStorm* array is the one sector of `t.bin` in a fixture attached to
// issue #4, written by StormLib (mpqcli 0.10.0), compression byte included.
// kAsciiAllBytes is bytes 0-255 as an ASCII-literal DCL stream; StormLib's
// explode decodes it to the same bytes.

static const u8 kStormPkwareSector[] = {
    0x08, 0x00, 0x04, 0xD8, 0xA4, 0x71, 0x53, 0x06, 0x04, 0x0C, 0x1D, 0x20, 0xE8, 0xA0, 0x6D, 0x2B,
    0x4E, 0x9D, 0x34, 0x63, 0xD6, 0x80, 0x10, 0x23, 0xE7, 0xCD, 0x1D, 0x37, 0x20, 0xCC, 0xBC, 0xC1,
    0x03, 0x42, 0x4D, 0x9D, 0x36, 0x70, 0x14, 0xBA, 0x40, 0x4C, 0x18, 0x0B, 0xC8, 0x84, 0xB1, 0xC0,
    0x4C, 0x18, 0x0B, 0xD0, 0x84, 0xB1, 0x40, 0x4D, 0x18, 0x0B, 0xD8, 0x84, 0xB1, 0xC0, 0x4D, 0xB4,
    0x82, 0x80, 0x7F,
};
static const u8 kStormImplodeSector[] = {
    0x00, 0x04, 0xD8, 0xA4, 0x71, 0x53, 0x06, 0x04, 0x0C, 0x1D, 0x20, 0xE8, 0xA0, 0x6D, 0x2B, 0x4E,
    0x9D, 0x34, 0x63, 0xD6, 0x80, 0x10, 0x23, 0xE7, 0xCD, 0x1D, 0x37, 0x20, 0xCC, 0xBC, 0xC1, 0x03,
    0x42, 0x4D, 0x9D, 0x36, 0x70, 0x14, 0xBA, 0x40, 0x4C, 0x18, 0x0B, 0xC8, 0x84, 0xB1, 0xC0, 0x4C,
    0x18, 0x0B, 0xD0, 0x84, 0xB1, 0x40, 0x4D, 0x18, 0x0B, 0xD8, 0x84, 0xB1, 0xC0, 0x4D, 0xB4, 0x82,
    0x80, 0x7F,
};
static const u8 kStormBzip2Sector[] = {
    0x10, 0x42, 0x5A, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x45, 0x7A, 0x7E, 0xAB, 0x00,
    0x00, 0x17, 0xC4, 0x00, 0x40, 0x00, 0x30, 0x00, 0x20, 0x00, 0x30, 0xCC, 0x05, 0x53, 0x6A, 0x42,
    0x04, 0x21, 0xD8, 0x78, 0xBB, 0x92, 0x29, 0xC2, 0x84, 0x82, 0x2B, 0xD3, 0xF5, 0x58,
};
static const u8 kStormLzmaSector[] = {
    0x12, 0x00, 0x5D, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x36, 0x1A, 0x4A, 0x1F, 0x08, 0xA0, 0x26, 0x56, 0x4E, 0x0D, 0x6C, 0xB8, 0xA5, 0xED, 0x63, 0x9C,
    0x8E, 0x7C, 0xDB, 0x4E, 0xF6, 0x9E, 0x4B, 0x78, 0x18, 0x56, 0x5C, 0xF7, 0x26, 0xEB, 0xD4, 0xA3,
    0x6D, 0xA5, 0xBC, 0x3A, 0xED, 0x0E, 0x96, 0x1A, 0x53, 0x11, 0xB7, 0x13, 0xD3, 0x8E, 0x9C, 0x0C,
    0x1E, 0x98, 0xED, 0xD9, 0x9F, 0x3F, 0x4C, 0xB5, 0x3A, 0x00,
};
static const u8 kStormHuffmanSector[] = {
    0x01, 0x00, 0x60, 0xF3, 0x34, 0x72, 0xF3, 0x65, 0x0B, 0xA4, 0xC3, 0x24, 0x9D, 0xF2, 0x74, 0x0A,
    0x4D, 0xC1, 0xE3, 0xBE, 0x3A, 0xBD, 0xC7, 0xB6, 0x35, 0xCC, 0x62, 0x9D, 0x5C, 0xFD, 0x96, 0xDD,
    0x55, 0xC5, 0x66, 0xC9, 0xC3, 0x93, 0xAD, 0xE6, 0xB9, 0x36, 0x19, 0x0E, 0x53, 0xA8, 0xBD, 0xDF,
    0xA7, 0x63, 0xEC, 0x73, 0x7E, 0x7D, 0x6F, 0xBC, 0x5F, 0xBF, 0x78, 0x62, 0x30, 0x5C, 0xAB, 0xD7,
    0xFD, 0x2C, 0xDB, 0x7C, 0x07, 0xBA, 0x92, 0xD9, 0xBB, 0xEF, 0xD0, 0xFD, 0xD2, 0xBE, 0x77, 0xDF,
    0xDF, 0x8E, 0xED, 0x5F, 0xBF, 0xFE, 0xF7, 0x7D, 0x5F, 0x01, 0xDE, 0xCA, 0x33, 0x79, 0x29, 0xC2,
    0xC0, 0x25, 0x4A, 0x06, 0xC1, 0xA5, 0x38, 0x72, 0xE1, 0xC0, 0xBB, 0xAF, 0xF7, 0x20, 0xFE, 0x5D,
    0xD1, 0x80, 0x01, 0x80, 0x03, 0x8F, 0x01, 0x18, 0x00, 0x0F, 0xD0, 0xC0, 0x03, 0x03, 0xFF, 0xFB,
    0xBE, 0x83, 0x1F, 0xD7, 0x5F, 0xD3, 0x56, 0x0B, 0x0E, 0x03, 0x07, 0x3C, 0xC3, 0xBF, 0xA2, 0xE1,
    0xCF, 0x86, 0x01, 0xFD, 0x5F, 0xC7, 0x8F, 0x3B, 0xB2, 0x69, 0xAB, 0x05, 0x87, 0x81, 0x03, 0x9E,
    0xE1, 0x5F, 0xD1, 0xF0, 0x67, 0xC3, 0x80, 0xF6, 0xAD, 0xE3, 0xC7, 0xDD, 0xBF, 0x69, 0xAB, 0x05,
    0x87, 0x81, 0x11, 0x1F, 0xC3, 0x1B, 0xD1, 0xF9, 0x6A,
};

static const u8 kAsciiAllBytes[] = {
    0x01, 0x06, 0x20, 0x09, 0xFC, 0x81, 0x1F, 0xF0, 0x05, 0x3E, 0xC0, 0x1B, 0x78, 0x01, 0x4F, 0xE0,
    0x01, 0x97, 0x18, 0xB8, 0x03, 0x37, 0x22, 0xE0, 0x0A, 0x5C, 0x80, 0x33, 0x70, 0x02, 0x8E, 0xC0,
    0x01, 0xD8, 0x03, 0x3B, 0x60, 0x0B, 0x6C, 0x80, 0x35, 0xB0, 0x02, 0x24, 0x81, 0x25, 0xB0, 0x00,
    0xE6, 0xC0, 0x0C, 0x98, 0x7A, 0x50, 0xC2, 0x01, 0x26, 0xA0, 0x00, 0xC6, 0x20, 0x87, 0x4D, 0x48,
    0x00, 0x0B, 0x6C, 0x7C, 0x29, 0x1E, 0x4C, 0x5C, 0x09, 0x0E, 0x36, 0x16, 0x26, 0x0C, 0x0C, 0x74,
    0xE8, 0xD0, 0x80, 0x11, 0x48, 0xA0, 0x81, 0x05, 0xE2, 0xC0, 0x50, 0x8C, 0x2A, 0x12, 0xBA, 0x28,
    0x50, 0xA1, 0x08, 0x40, 0x0C, 0x4C, 0x3E, 0x32, 0x8F, 0x8B, 0x04, 0xA2, 0x1C, 0x36, 0x0B, 0x11,
    0x0C, 0xC8, 0xA0, 0x83, 0x06, 0x22, 0x90, 0x40, 0x18, 0x54, 0x60, 0x00, 0x11, 0xE8, 0x3B, 0x4C,
    0x06, 0xDD, 0xA6, 0x51, 0x29, 0x16, 0x08, 0x21, 0x98, 0x64, 0x43, 0x27, 0x81, 0x4C, 0x53, 0x15,
    0x19, 0x02, 0x3C, 0x78, 0x70, 0x20, 0x05, 0x41, 0x10, 0x00, 0x7A, 0x40, 0x17, 0xE8, 0x00, 0x12,
    0x80, 0x38, 0x20, 0x06, 0x88, 0x02, 0x22, 0x80, 0x30, 0x20, 0x04, 0x08, 0x02, 0x02, 0x00, 0x3F,
    0xC0, 0x07, 0xF0, 0x02, 0x3C, 0x00, 0x37, 0xC0, 0x05, 0x70, 0x02, 0x1C, 0x00, 0x3B, 0xC0, 0x06,
    0xB0, 0x02, 0x2C, 0x00, 0x33, 0xC0, 0x04, 0x30, 0x02, 0x0C, 0x00, 0x3D, 0x40, 0x07, 0xD0, 0x02,
    0x34, 0x00, 0x35, 0x40, 0x05, 0x50, 0x02, 0x14, 0x00, 0x39, 0x40, 0x06, 0x90, 0x02, 0x24, 0x00,
    0x31, 0x40, 0x04, 0x10, 0x02, 0x04, 0x00, 0x3E, 0x80, 0x07, 0xE0, 0x02, 0x38, 0x00, 0x36, 0x80,
    0x05, 0x60, 0x02, 0x6D, 0xA0, 0x05, 0x34, 0x81, 0x06, 0x50, 0x07, 0x6A, 0x40, 0x15, 0xA8, 0x00,
    0x65, 0xA0, 0x04, 0x14, 0x81, 0x02, 0x90, 0x07, 0x72, 0x40, 0x16, 0xC8, 0x00, 0x69, 0x20, 0x05,
    0x24, 0x81, 0x04, 0x10, 0x07, 0x62, 0x40, 0x14, 0x88, 0x00, 0x61, 0x20, 0x04, 0x04, 0x81, 0x00,
    0xE0, 0x07, 0x7C, 0x80, 0x17, 0xF0, 0x00, 0x6E, 0xC0, 0x05, 0x38, 0x01, 0x07, 0x60, 0x07, 0x6C,
    0x80, 0x15, 0xB0, 0x00, 0x66, 0xC0, 0x04, 0x18, 0x01, 0x03, 0xA0, 0x07, 0x74, 0x80, 0x16, 0xD0,
    0x00, 0x18, 0x80, 0x1A, 0x40, 0x07, 0xD0, 0x00, 0x54, 0x40, 0x05, 0xA0, 0x00, 0xC8, 0x00, 0x12,
    0xA0, 0x04, 0x10, 0x01, 0x04, 0x00, 0x1E, 0x80, 0x03, 0x14, 0x00, 0x2C, 0x00, 0x03, 0x40, 0x03,
    0x72, 0x40, 0x06, 0x48, 0x01, 0x28, 0x00, 0x12, 0x80, 0x00, 0xC0, 0x01, 0x30, 0x00, 0x14, 0x00,
    0x01, 0x80, 0x01, 0x20, 0x00, 0x10, 0x00, 0x08, 0xF8, 0x07,
};

/// The 256-byte text the fixtures hold.
static std::vector<u8> issue4Text() {
    std::string text;
    for (int line = 0; line < 8; ++line)
        text += "line " + std::to_string(line) + ": the quick brown fox jump\n";
    return {text.begin(), text.begin() + 256};
}

/// "AAAABBBB" eight times: runs of exactly four, bzip2's run-length boundary.
static std::vector<u8> issue4Runs() {
    std::vector<u8> runs;
    for (int i = 0; i < 8; ++i)
        runs.insert(runs.end(), {'A', 'A', 'A', 'A', 'B', 'B', 'B', 'B'});
    return runs;
}

TEST_CASE("MPQ codecs read what StormLib writes", "[mpq][codec]") {
    std::string error;
    SECTION("PKware DCL") {
        CHECK(mpq::mpqDecompress(kStormPkwareSector, 256, &error) == issue4Text());
    }
    SECTION("BZip2, runs of four") {
        CHECK(mpq::mpqDecompress(kStormBzip2Sector, 64, &error) == issue4Runs());
    }
    SECTION("LZMA") {
        CHECK(mpq::mpqDecompress(kStormLzmaSector, 256, &error) == issue4Text());
    }
    SECTION("Huffman") {
        CHECK(mpq::mpqDecompress(kStormHuffmanSector, 256, &error) == issue4Text());
    }
    SECTION("PKware DCL, ASCII literals") {
        std::vector<u8> all(256);
        for (size_t i = 0; i < all.size(); ++i)
            all[i] = static_cast<u8>(i);
        CHECK(mpq::pkwareExplode(kAsciiAllBytes, all.size()) == all);
    }
    CHECK(error.empty());
}

/// @p sector as the one sector of a compressed multi-sector file.
static std::vector<u8> oneSectorFile(std::span<const u8> sector) {
    std::vector<u8> file(8);
    u32 const offsets[2] = {8, static_cast<u32>(8 + sector.size())};
    std::memcpy(file.data(), offsets, sizeof(offsets));
    file.insert(file.end(), sector.begin(), sector.end());
    return file;
}

TEST_CASE("LZMA copies 5 bytes back (distance slot 4)", "[mpq][codec]") {
    // An MPQ LZMA sector (Python's lzma, FORMAT_ALONE) of the text below. Its
    // first copy is 5 bytes back, the slot the old decoder indexed at -1.
    static const u8 kSector[] = {
    0x12, 0x00, 0x5D, 0x00, 0x00, 0x01, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x98, 0x88, 0x98, 0x3E, 0xD4, 0xFF, 0xE8, 0xCF, 0x2D, 0x69, 0x35, 0x8B, 0xB3, 0x76, 0x11,
    0x21, 0x07, 0x58, 0xAD, 0x6F, 0x63, 0x40, 0x83, 0xFF, 0xFF, 0x6C, 0xD8, 0x00, 0x00,
    };
    std::string text;
    for (int i = 0; i < 12; ++i)
        text += "abcde";
    for (int i = 0; i < 4; ++i)
        text += "0123456789";
    std::string error;
    auto const data = mpq::mpqDecompress(kSector, text.size(), &error);
    CHECK(std::string(data.begin(), data.end()) == text);
}

TEST_CASE("an imploded file's sectors are bare DCL streams", "[mpq][codec]") {
    // FILE_IMPLODE, not FILE_COMPRESS: no compression byte before the stream.
    auto const file = oneSectorFile(kStormImplodeSector);
    mpq::BlockEntry const block{0, static_cast<u32>(file.size()), 256,
                                mpq::FileFlag::kExists | mpq::FileFlag::kImplode};
    std::string error;
    auto data = mpq::extractFileData(file, 0, block, 4096, 0, &error);
    REQUIRE(data.has_value());
    CHECK(*data == issue4Text());
}

TEST_CASE("a block's high offset bits move where it is read", "[mpq][codec]") {
    auto const file = oneSectorFile(kStormPkwareSector);
    mpq::BlockEntry const block{0, static_cast<u32>(file.size()), 256,
                                mpq::FileFlag::kExists | mpq::FileFlag::kCompress};
    REQUIRE(mpq::extractFileData(file, 0, block, 4096, 0).has_value());

    // The same low bits 4 GiB further on lie past this buffer.
    std::string error;
    CHECK_FALSE(mpq::extractFileData(file, 0, block, 4096, 0, &error, nullptr, 1).has_value());
    CHECK(error == "data out of bounds");
}

TEST_CASE("a file that decodes short is a failed read", "[mpq][codec]") {
    // The stream holds 256 bytes; a block table claiming 300 must not get 256.
    auto const file = oneSectorFile(kStormPkwareSector);
    mpq::BlockEntry const block{0, static_cast<u32>(file.size()), 300,
                                mpq::FileFlag::kExists | mpq::FileFlag::kCompress};
    std::string error;
    CHECK_FALSE(mpq::extractFileData(file, 0, block, 4096, 0, &error).has_value());
    CHECK_FALSE(error.empty());

    // A compression bit no codec owns fails rather than passing data through.
    std::vector<u8> unknown{0x04, 'a', 'b', 'c'};
    CHECK(mpq::mpqDecompress(unknown, 16, &error).empty());
}

TEST_CASE("a WAV written with ADPCM keeps its first sector whole", "[mpq][codec]") {
    // ADPCM is lossy: Blizzard's tools and StormLib compress a wave's first
    // sector as data, so its RIFF header survives.
    std::vector<u8> wav = {'R', 'I', 'F', 'F', 0x24, 0x40, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                           16,  0,   0,   0,   1,    0,    1, 0, 0x22, 0x56, 0, 0, 0x44, 0xAC, 0, 0,
                           2,   0,   16,  0,   'd',  'a',  't', 'a', 0, 0x40, 0, 0};
    for (int i = 0; i < 6000; ++i) {
        auto const sample = static_cast<std::int16_t>(9000.0 * std::sin(i / 7.0));
        wav.push_back(static_cast<u8>(sample & 0xFF));
        wav.push_back(static_cast<u8>((sample >> 8) & 0xFF));
    }
    mpq::EncodeOptions opts;
    opts.compression = mpq::CompressionFlag::kAdpcmMono;
    opts.sectorSize = 4096;
    auto const encoded = mpq::encodeFileData(wav, opts);
    REQUIRE((encoded.flags & mpq::FileFlag::kCompress) == mpq::FileFlag::kCompress);
    mpq::BlockEntry const block{0, encoded.compressedSize, static_cast<u32>(wav.size()), encoded.flags};
    auto const decoded = mpq::extractFileData(encoded.data, 0, block, 4096, 0);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == wav.size());
    CHECK(std::equal(wav.begin(), wav.begin() + 4096, decoded->begin()));
}

TEST_CASE("bzip2 splits what it compresses into blocks of at most 900k", "[mpq][codec]") {
    // One block holds at most 900k bytes after its run-length pass: a larger
    // unit written as one block was a file no bzip2 decoder reads. Periodic
    // text also keeps the rotation sort honest: compared byte by byte it
    // takes quadratic time.
    std::vector<u8> data;
    char const* words[] = {"unit ", "footman ", "call ", "set ", "endfunction\r\n"};
    for (u32 i = 0; data.size() < 2'500'000; ++i) {
        char const* w = words[(i * 7 + i / 5) % 5];
        data.insert(data.end(), w, w + std::strlen(w));
    }
    auto const compressed = common::bzip2Compress(data);
    REQUIRE_FALSE(compressed.empty());
    CHECK(compressed.size() < data.size() / 4);
    CHECK(common::bzip2Decompress(compressed, data.size()) == data);
}

TEST_CASE("a version 2+ archive's sectors use only masks newer Storm reads", "[mpq][codec]") {
    // Newer Storm and StormLib read a v2+ archive's sectors with zlib, PKware,
    // bzip2, LZMA, sparse (alone or before zlib or bzip2) or ADPCM paired
    // with Huffman; Huffman or ADPCM alone is a sector they cannot open.
    std::vector<u8> wave(30000);
    for (size_t i = 0; i + 1 < wave.size(); i += 2) {
        auto const sample = static_cast<std::int16_t>(8000.0 * std::sin(static_cast<double>(i) / 11.0));
        wave[i] = static_cast<u8>(sample & 0xFF);
        wave[i + 1] = static_cast<u8>((sample >> 8) & 0xFF);
    }
    auto sectorMasks = [](const mpq::EncodedFile& encoded, size_t size, u32 sectorSize) {
        std::vector<u8> masks;
        u32 const sectors = static_cast<u32>((size + sectorSize - 1) / sectorSize);
        std::vector<u32> offsets(sectors + 1);
        std::memcpy(offsets.data(), encoded.data.data(), offsets.size() * 4);
        for (u32 i = 0; i < sectors; ++i) {
            size_t const plain = std::min<size_t>(sectorSize, size - size_t{i} * sectorSize);
            if (offsets[i + 1] - offsets[i] < plain)
                masks.push_back(encoded.data[offsets[i]]);
        }
        return masks;
    };
    auto const text = makeCompressibleData(30000);
    for (auto const codec : {mpq::CompressionFlag::kHuffman, mpq::CompressionFlag::kAdpcmMono}) {
        CAPTURE(static_cast<int>(codec));
        auto const& data = codec == mpq::CompressionFlag::kHuffman ? text : wave;
        mpq::EncodeOptions opts;
        opts.compression = codec;
        opts.sectorSize = 4096;
        opts.modernCodecs = true;
        auto const encoded = mpq::encodeFileData(data, opts);
        auto const masks = sectorMasks(encoded, data.size(), 4096);
        REQUIRE(masks.size() > 1);
        for (u8 const mask : masks)
            CHECK((mask == 0x02 || mask == 0x08 || mask == 0x41));
        if (codec == mpq::CompressionFlag::kAdpcmMono)
            CHECK(masks.back() == 0x41);
        mpq::BlockEntry const block{0, encoded.compressedSize, static_cast<u32>(data.size()), encoded.flags};
        auto const decoded = mpq::extractFileData(encoded.data, 0, block, 4096, 0);
        REQUIRE(decoded.has_value());
        if (codec == mpq::CompressionFlag::kHuffman)
            CHECK(*decoded == data);
    }
}

// ============================================================================
// Entry point
// ============================================================================


TEST_CASE("A byte buffer encrypts word by word, a short tail left as it is", "[mpq][crypto]") {
    std::vector<u8> plain(4099);
    for (size_t i = 0; i < plain.size(); ++i)
        plain[i] = static_cast<u8>(i * 7 + 3);
    constexpr u32 kKey = 0x9E3779B9;

    std::vector<u32> words(1024);
    std::memcpy(words.data(), plain.data(), 4096);
    mpq::encryptBlock(words.data(), words.size(), kKey);

    auto sealed = plain;
    mpq::encryptBytes(sealed, kKey);
    CHECK(std::memcmp(sealed.data(), words.data(), 4096) == 0);
    CHECK(std::equal(sealed.begin() + 4096, sealed.end(), plain.begin() + 4096));

    mpq::decryptBytes(sealed, kKey);
    CHECK(sealed == plain);
}

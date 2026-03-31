// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MPQ codec round-trip test suite.
// Tests individual compress/decompress functions for all codecs,
// plus the mpqCompress/mpqDecompress integration layer.

// Internal codec headers (not public API — test-only usage).
#include "../src/whiteout/storages/mpq/codecs/compression.h"
#include "../src/whiteout/storages/mpq/codecs/sparse.h"
#include "../src/whiteout/storages/mpq/codecs/huffman.h"
#include "../src/whiteout/storages/mpq/codecs/bzip2.h"
#include "../src/whiteout/storages/mpq/codecs/adpcm.h"
#include "../src/whiteout/storages/mpq/codecs/pkware.h"
#include "../src/whiteout/storages/mpq/codecs/lzma.h"
#include "../src/whiteout/storages/common/zlib.h"

#include <whiteout/common_types.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

using whiteout::u8;
using whiteout::u16;
namespace mpq = whiteout::storages::mpq;

// ============================================================================
// Test harness
// ============================================================================

static int g_passed = 0;
static int g_failed = 0;

static void pass(const char* fmt, ...) {
    g_passed++;
    std::printf("  PASS: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

static void fail(const char* fmt, ...) {
    g_failed++;
    std::printf("  FAIL: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

static void expect(bool condition, const char* fmt, ...) {
    if (condition) {
        g_passed++;
        return;
    }
    g_failed++;
    std::printf("  FAIL: ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
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

static void testSparseRoundTrip() {
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

static void testHuffmanRoundTrip() {
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

static void testBzip2RoundTrip() {
    std::printf("\n=== BZip2 Codec Tests ===\n");

    // Test with compressible data.
    {
        auto data = makeCompressibleData(1024);
        auto compressed = mpq::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress text: not empty");
        expect(compressed.size() < data.size(), "bzip2 compress text: actually compressed");
        auto decompressed = mpq::bzip2Decompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "bzip2 decompress text: correct size");
        expect(decompressed == data, "bzip2 round-trip text: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 1024 text bytes");
    }

    // Test with pattern data.
    {
        auto data = makePatternData(500, 33);
        auto compressed = mpq::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress pattern: not empty");
        auto decompressed = mpq::bzip2Decompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "bzip2 decompress pattern: correct size");
        expect(decompressed == data, "bzip2 round-trip pattern: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 500 pattern bytes");
    }

    // Test with all zeros.
    {
        auto data = makeZeroData(512);
        auto compressed = mpq::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress zeros: not empty");
        auto decompressed = mpq::bzip2Decompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "bzip2 decompress zeros: correct size");
        expect(decompressed == data, "bzip2 round-trip zeros: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 512 zeros");
    }

    // Test with small data.
    {
        std::vector<u8> data = {0x01, 0x02, 0x03, 0x04, 0x05};
        auto compressed = mpq::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress 5 bytes: not empty");
        auto decompressed = mpq::bzip2Decompress(compressed, data.size());
        expect(decompressed == data, "bzip2 round-trip 5 bytes: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 5 bytes");
    }

    // Test larger data.
    {
        auto data = makeCompressibleData(4096);
        auto compressed = mpq::bzip2Compress(data);
        expect(!compressed.empty(), "bzip2 compress 4096: not empty");
        auto decompressed = mpq::bzip2Decompress(compressed, data.size());
        expect(decompressed.size() == data.size(), "bzip2 decompress 4096: correct size");
        expect(decompressed == data, "bzip2 round-trip 4096: data matches");
        if (decompressed == data) pass("bzip2 round-trip: 4096 bytes");
    }
}

// ============================================================================
// ADPCM codec tests
// ============================================================================

static void testAdpcmRoundTrip() {
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

static void testPkwareRoundTrip() {
    std::printf("\n=== PKware Codec Tests ===\n");

    // Test with compressible data.
    {
        auto data = makeCompressibleData(1024);
        auto compressed = mpq::pkwareImplode(data);
        expect(!compressed.empty(), "pkware compress text: not empty");
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

static void testZlibRoundTrip() {
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

static void testMpqCompressDecompress() {
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
        // Build a full mpqDecompress input: compression byte (0x12) + raw LZMA data.
        static const u8 rawLzma[] = {
            0x5D, 0x00, 0x00, 0x80, 0x00, 0x00, 0x24, 0x19,
            0x49, 0x98, 0x6F, 0x16, 0x02, 0x89, 0x0A, 0x98,
            0xE7, 0x3F, 0xA8, 0xC3, 0x95, 0x48, 0x4D, 0xFF,
            0xFF, 0x75, 0xF0, 0x00, 0x00,
        };
        std::vector<u8> mpqBuf;
        mpqBuf.push_back(0x12); // LZMA compression byte
        mpqBuf.insert(mpqBuf.end(), std::begin(rawLzma), std::end(rawLzma));

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

static void testLzmaDecompress() {
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
        auto result = mpq::lzmaDecompress(compressed, 13);
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

        auto result = mpq::lzmaDecompress(compressed, 1024);
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
        auto result = mpq::lzmaDecompress(compressed, 256);
        expect(result.size() == 256, "lzma zeros: correct size (%zu vs 256)", result.size());
        bool match = (result == expected);
        expect(match, "lzma zeros: data matches");
        if (match) pass("lzma decompress: all zeros (256 bytes)");
    }

    // Error case: empty input
    {
        auto result = mpq::lzmaDecompress({}, 100);
        expect(result.empty(), "lzma empty input: returns empty");
        if (result.empty()) pass("lzma error: empty input handled");
    }

    // Error case: truncated input (only properties, no stream)
    {
        static const u8 truncated[] = {0x5D, 0x00, 0x00, 0x80, 0x00};
        auto result = mpq::lzmaDecompress(truncated, 100);
        expect(result.empty(), "lzma truncated input: returns empty");
        if (result.empty()) pass("lzma error: truncated input handled");
    }
}

// ============================================================================
// Entry point
// ============================================================================

int main() {
    std::printf("MPQ Codec Round-Trip Test Suite\n");
    std::printf("===============================\n");

    testSparseRoundTrip();
    testHuffmanRoundTrip();
    testBzip2RoundTrip();
    testAdpcmRoundTrip();
    testPkwareRoundTrip();
    testZlibRoundTrip();
    testMpqCompressDecompress();
    testLzmaDecompress();

    std::printf("\n===============================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);
    std::printf("===============================\n");

    return g_failed > 0 ? 1 : 0;
}

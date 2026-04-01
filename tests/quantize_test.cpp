// Wu quantizer test suite — validates correctness, serial-vs-parallel parity,
// dithering, async, edge cases, and sRGB mode.

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "../src/whiteout/textures/utils/quantize.h"
#include <whiteout/utils/simple_thread_pool.h>

#define TEST_FAIL(test, ...) \
    do { \
        char _msg[512]; \
        snprintf(_msg, sizeof(_msg), __VA_ARGS__); \
        FAIL("TEST " << test << " FAIL: " << _msg); \
    } while (0)

#define TEST_PASS(test, ...) \
    do { \
        (void)test; \
    } while (0)



using namespace whiteout;
using namespace whiteout::textures::wu;

// ============================================================================
// Helpers
// ============================================================================

/// Create an RGBA8 image filled by a callback (x, y) -> (r, g, b, a).
std::vector<u8> make_rgba(u32 w, u32 h,
                          std::function<void(u32 x, u32 y, u8* out)> fill) {
    std::vector<u8> pixels(static_cast<size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            fill(x, y, &pixels[(y * w + x) * 4]);
    return pixels;
}

/// Deterministic varied-content fill.
void hash_fill(u32 x, u32 y, u8* out) {
    out[0] = static_cast<u8>((x * 37 + y * 13) & 0xFF);
    out[1] = static_cast<u8>((x * 7 + y * 41) & 0xFF);
    out[2] = static_cast<u8>((x * 23 + y * 3) & 0xFF);
    out[3] = 255;
}

/// Solid-color fill.
void solid_fill(u8 r, u8 g, u8 b, u32 /*x*/, u32 /*y*/, u8* out) {
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = 255;
}

/// Compute mean squared error between original pixels and their palette
/// reconstruction through the given index map.
double compute_palette_mse(const u8* rgba, u32 pixel_count,
                           const u8* indices,
                           const std::array<u32, MAX_COLORS>& palette) {
    double total = 0;
    for (u32 i = 0; i < pixel_count; ++i) {
        const u32 base = i * 4;
        const u32 packed = palette[indices[i]];
        const int pr = static_cast<int>((packed >> 16) & 0xFF);
        const int pg = static_cast<int>((packed >> 8) & 0xFF);
        const int pb = static_cast<int>(packed & 0xFF);
        const int dr = static_cast<int>(rgba[base]) - pr;
        const int dg = static_cast<int>(rgba[base + 1]) - pg;
        const int db = static_cast<int>(rgba[base + 2]) - pb;
        total += dr * dr + dg * dg + db * db;
    }
    return total / (pixel_count * 3.0);
}

/// Compare two QuantizeResults for palette+tag equality.
bool results_equal(const QuantizeResult& a, const QuantizeResult& b) {
    if (a.color_count != b.color_count)
        return false;
    for (u32 i = 0; i < a.color_count; ++i)
        if (a.palette[i] != b.palette[i])
            return false;
    // Compare mapPixel outputs for a sampled subset of the color space
    for (int ri = 0; ri < 256; ri += 8)
        for (int gi = 0; gi < 256; gi += 8)
            for (int bi = 0; bi < 256; bi += 8)
                if (a.mapPixel(ri, gi, bi) != b.mapPixel(ri, gi, bi))
                    return false;
    return true;
}



// ============================================================================
// Tests
// ============================================================================

TEST_CASE("Wu quantizer", "[quantize]") {
    printf("=== Wu Quantizer Test Suite ===\n\n");

    // ====================================================================
    // Serial tests
    // ====================================================================

    printf("--- Serial tests ---\n");

    // Test 1: Solid color image -> 1 unique color, palette entry matches
    {
        auto pixels = make_rgba(64, 64, [](u32 x, u32 y, u8* out) {
            solid_fill(100, 150, 200, x, y, out);
        });

        auto result = Quantizer().maxColors(256).kmeansIterations(0).quantize(
            pixels.data(), 64 * 64);

        if (result.color_count < 1)
            TEST_FAIL(1, "color_count = %u", result.color_count);

        // The mapped pixel must round-trip to the original color
        const u8 idx = result.mapPixel(100, 150, 200);
        const u32 packed = result.palette[idx];
        const u8 pr = (packed >> 16) & 0xFF;
        const u8 pg = (packed >> 8) & 0xFF;
        const u8 pb = packed & 0xFF;
        if (std::abs(static_cast<int>(pr) - 100) > 4 ||
            std::abs(static_cast<int>(pg) - 150) > 4 ||
            std::abs(static_cast<int>(pb) - 200) > 4)
            TEST_FAIL(1, "palette color (%u,%u,%u) too far from (100,150,200)", pr, pg, pb);

        TEST_PASS(1, "solid color round-trip");
    }

    // Test 2: 2-color image -> palette has exactly 2 colors
    {
        auto pixels = make_rgba(64, 64, [](u32 x, u32 y, u8* out) {
            if ((x + y) % 2 == 0)
                solid_fill(255, 0, 0, x, y, out);
            else
                solid_fill(0, 0, 255, x, y, out);
        });

        auto result = Quantizer().maxColors(256).kmeansIterations(5).quantize(
            pixels.data(), 64 * 64);

        if (result.color_count != 2)
            TEST_FAIL(2, "expected 2 colors, got %u", result.color_count);

        TEST_PASS(2, "2-color image -> exactly 2 palette entries");
    }

    // Test 3: Hash-filled 128x128 -> low MSE with 256 colors + kmeans
    {
        auto pixels = make_rgba(128, 128, hash_fill);
        const u32 pc = 128 * 128;

        auto result = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(10)
                          .quantize(pixels.data(), pc);

        std::vector<u8> indices(pc);
        result.mapPixels(pixels.data(), pc, indices.data());
        double mse = compute_palette_mse(pixels.data(), pc, indices.data(), result.palette);

        if (mse > 400.0)
            TEST_FAIL(3, "MSE=%.2f too high (expected < 400)", mse);

        TEST_PASS(3, "128x128 hash image, 256 colors, MSE=%.2f", mse);
    }

    // Test 4: maxColors(16) constrains palette correctly
    {
        auto pixels = make_rgba(64, 64, hash_fill);
        auto result = Quantizer().maxColors(16).kmeansIterations(5).quantize(
            pixels.data(), 64 * 64);

        if (result.color_count > 16)
            TEST_FAIL(4, "color_count=%u exceeds max 16", result.color_count);

        TEST_PASS(4, "maxColors(16) -> %u palette entries", result.color_count);
    }

    // Test 5: kmeansIterations(0) still produces a valid result
    {
        auto pixels = make_rgba(64, 64, hash_fill);
        auto result = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(0)
                          .quantize(pixels.data(), 64 * 64);

        if (result.color_count == 0)
            TEST_FAIL(5, "color_count=0 with kmeansIterations=0");

        const u8 idx = result.mapPixel(128, 128, 128);
        if (idx >= result.color_count)
            TEST_FAIL(5, "mapPixel returned %u >= color_count %u", idx, result.color_count);

        TEST_PASS(5, "kmeansIterations=0 -> %u colors, valid mapPixel", result.color_count);
    }

    // Test 6: mapPixels consistency with mapPixel
    {
        auto pixels = make_rgba(32, 32, hash_fill);
        const u32 pc = 32 * 32;

        auto result = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(5)
                          .quantize(pixels.data(), pc);

        std::vector<u8> bulk(pc);
        result.mapPixels(pixels.data(), pc, bulk.data());

        for (u32 i = 0; i < pc; ++i) {
            const u32 base = i * 4;
            const u8 single = result.mapPixel(pixels[base], pixels[base + 1], pixels[base + 2]);
            if (single != bulk[i])
                TEST_FAIL(6, "pixel %u: mapPixel=%u vs mapPixels=%u", i, single, bulk[i]);
        }

        TEST_PASS(6, "mapPixels matches mapPixel for all pixels");
    }

    // Test 7: Dithered mapping indices stay in range
    {
        constexpr u32 W = 64, H = 64;
        auto pixels = make_rgba(W, H, hash_fill);

        auto result = Quantizer()
                          .maxColors(128)
                          .kmeansIterations(5)
                          .quantize(pixels.data(), W * H);

        std::vector<u8> dithered(W * H);
        result.mapPixelsDithered(pixels.data(), W, H, 0.5f, dithered.data());

        for (u32 i = 0; i < W * H; ++i) {
            if (dithered[i] >= result.color_count)
                TEST_FAIL(7, "dithered[%u]=%u >= color_count %u", i, dithered[i], result.color_count);
        }

        TEST_PASS(7, "dithered indices all in range [0, %u)", result.color_count);
    }

    // Test 8: refineDitherAware does not crash, indices still valid
    {
        constexpr u32 W = 64, H = 64;
        auto pixels = make_rgba(W, H, hash_fill);

        auto result = Quantizer()
                          .maxColors(64)
                          .kmeansIterations(5)
                          .quantize(pixels.data(), W * H);

        result.refineDitherAware(pixels.data(), W, H, 0.5f, 3);

        std::vector<u8> indices(W * H);
        result.mapPixels(pixels.data(), W * H, indices.data());
        for (u32 i = 0; i < W * H; ++i) {
            if (indices[i] >= result.color_count)
                TEST_FAIL(8, "after refine: index[%u]=%u >= color_count %u",
                     i, indices[i], result.color_count);
        }

        TEST_PASS(8, "refineDitherAware: indices valid, %u colors", result.color_count);
    }

    // Test 9: sRGB mode produces a valid result
    {
        auto pixels = make_rgba(64, 64, hash_fill);
        auto result = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(5)
                          .srgbInput(true)
                          .quantize(pixels.data(), 64 * 64);

        if (result.color_count == 0)
            TEST_FAIL(9, "sRGB mode produced 0 colors");

        std::vector<u8> indices(64 * 64);
        result.mapPixels(pixels.data(), 64 * 64, indices.data());
        double mse = compute_palette_mse(pixels.data(), 64 * 64, indices.data(), result.palette);

        TEST_PASS(9, "sRGB mode -> %u colors, MSE=%.2f", result.color_count, mse);
    }

    // Test 10: ditherAware builder mode (kmeans=0)
    {
        constexpr u32 W = 64, H = 64;
        auto pixels = make_rgba(W, H, hash_fill);

        auto result = Quantizer()
                          .maxColors(128)
                          .kmeansIterations(0)
                          .ditherAware(W, H, 0.5f, 3)
                          .quantize(pixels.data(), W * H);

        if (result.color_count == 0)
            TEST_FAIL(10, "ditherAware + kmeansIterations(0) produced 0 colors");

        std::vector<u8> indices(W * H);
        result.mapPixels(pixels.data(), W * H, indices.data());
        for (u32 i = 0; i < W * H; ++i)
            if (indices[i] >= result.color_count)
                TEST_FAIL(10, "index[%u]=%u >= %u", i, indices[i], result.color_count);

        TEST_PASS(10, "ditherAware + kmeansIterations(0): %u colors, indices valid",
             result.color_count);
    }

    // Test 11: 1-pixel image
    {
        u8 px[4] = {42, 128, 200, 255};
        auto result = Quantizer().maxColors(256).kmeansIterations(5).quantize(px, 1);

        if (result.color_count < 1)
            TEST_FAIL(11, "1-pixel: color_count=%u", result.color_count);

        u8 idx;
        result.mapPixels(px, 1, &idx);
        if (idx >= result.color_count)
            TEST_FAIL(11, "1-pixel: idx=%u >= %u", idx, result.color_count);

        TEST_PASS(11, "1-pixel image: %u colors", result.color_count);
    }

    // Test 12: Gradient image — low MSE
    {
        constexpr u32 W = 256, H = 1;
        auto pixels = make_rgba(W, H, [](u32 x, u32 /*y*/, u8* out) {
            out[0] = static_cast<u8>(x);
            out[1] = 0;
            out[2] = static_cast<u8>(255 - x);
            out[3] = 255;
        });

        auto result = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(10)
                          .quantize(pixels.data(), W);

        std::vector<u8> indices(W);
        result.mapPixels(pixels.data(), W, indices.data());
        double mse = compute_palette_mse(pixels.data(), W, indices.data(), result.palette);

        if (mse > 200.0)
            TEST_FAIL(12, "gradient MSE=%.2f too high", mse);

        TEST_PASS(12, "256-pixel gradient, MSE=%.2f", mse);
    }

    // ====================================================================
    // Parallel tests
    // ====================================================================

    printf("\n--- Parallel tests (SimpleThreadPool) ---\n");

    const size_t numThreads = std::max<size_t>(4, std::thread::hardware_concurrency());
    whiteout::utils::SimpleThreadPool pool(numThreads);
    printf("Using SimpleThreadPool with %zu threads\n", numThreads);

    // Test 13: Serial vs parallel — identical palette + tag mapping
    {
        auto pixels = make_rgba(128, 128, hash_fill);
        const u32 pc = 128 * 128;

        auto serial = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(10)
                          .quantize(pixels.data(), pc, nullptr);

        auto parallel = Quantizer()
                            .maxColors(256)
                            .kmeansIterations(10)
                            .quantize(pixels.data(), pc, &pool);

        if (!results_equal(serial, parallel))
            TEST_FAIL(13, "serial vs parallel results differ");

        TEST_PASS(13, "serial vs parallel: identical palette+tag (128x128, 256 colors)");
    }

    // Test 14: Serial vs parallel — identical with maxColors(32)
    {
        auto pixels = make_rgba(64, 64, hash_fill);
        const u32 pc = 64 * 64;

        auto serial = Quantizer()
                          .maxColors(32)
                          .kmeansIterations(5)
                          .quantize(pixels.data(), pc, nullptr);

        auto parallel = Quantizer()
                            .maxColors(32)
                            .kmeansIterations(5)
                            .quantize(pixels.data(), pc, &pool);

        if (!results_equal(serial, parallel))
            TEST_FAIL(14, "serial vs parallel differ for 32 colors");

        TEST_PASS(14, "serial vs parallel: identical (64x64, 32 colors)");
    }

    // Test 15: Serial vs parallel — sRGB mode parity
    {
        auto pixels = make_rgba(64, 64, hash_fill);
        const u32 pc = 64 * 64;

        auto serial = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(5)
                          .srgbInput(true)
                          .quantize(pixels.data(), pc, nullptr);

        auto parallel = Quantizer()
                            .maxColors(256)
                            .kmeansIterations(5)
                            .srgbInput(true)
                            .quantize(pixels.data(), pc, &pool);

        if (!results_equal(serial, parallel))
            TEST_FAIL(15, "serial vs parallel differ in sRGB mode");

        TEST_PASS(15, "serial vs parallel sRGB: identical");
    }

    // Test 16: Serial vs parallel — kmeansIterations(0)
    {
        auto pixels = make_rgba(64, 64, hash_fill);
        const u32 pc = 64 * 64;

        auto serial = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(0)
                          .quantize(pixels.data(), pc, nullptr);

        auto parallel = Quantizer()
                            .maxColors(256)
                            .kmeansIterations(0)
                            .quantize(pixels.data(), pc, &pool);

        if (!results_equal(serial, parallel))
            TEST_FAIL(16, "serial vs parallel differ with kmeansIterations=0");

        TEST_PASS(16, "serial vs parallel kmeansIter=0: identical");
    }

    // Test 17: Serial vs parallel — ditherAware mode
    {
        constexpr u32 W = 64, H = 64;
        auto pixels = make_rgba(W, H, hash_fill);
        const u32 pc = W * H;

        auto serial = Quantizer()
                          .maxColors(128)
                          .kmeansIterations(5)
                          .ditherAware(W, H, 0.5f, 3)
                          .quantize(pixels.data(), pc, nullptr);

        auto parallel = Quantizer()
                            .maxColors(128)
                            .kmeansIterations(5)
                            .ditherAware(W, H, 0.5f, 3)
                            .quantize(pixels.data(), pc, &pool);

        if (!results_equal(serial, parallel))
            TEST_FAIL(17, "serial vs parallel differ in ditherAware mode");

        TEST_PASS(17, "serial vs parallel ditherAware: identical");
    }

    // Test 18: quantizeAsync — result matches synchronous parallel
    {
        auto pixels = make_rgba(128, 128, hash_fill);
        const u32 pc = 128 * 128;

        auto syncResult = Quantizer()
                              .maxColors(256)
                              .kmeansIterations(10)
                              .quantize(pixels.data(), pc, &pool);

        auto sem = pool.createTimelineSemaphore();
        const auto startVal = sem->next();
        sem->signal(startVal);

        QuantizeResult asyncResult;
        auto endVal = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(10)
                          .quantizeAsync(pixels.data(), pc, &pool, sem.get(),
                                         startVal, &asyncResult);
        sem->wait(endVal);

        if (!results_equal(syncResult, asyncResult))
            TEST_FAIL(18, "async vs sync-parallel results differ");

        TEST_PASS(18, "quantizeAsync matches sync-parallel");
    }

    // Test 19: Parallel refineDitherAware — indices valid
    {
        constexpr u32 W = 64, H = 64;
        auto pixels = make_rgba(W, H, hash_fill);

        auto result = Quantizer()
                          .maxColors(64)
                          .kmeansIterations(5)
                          .quantize(pixels.data(), W * H, &pool);

        result.refineDitherAware(pixels.data(), W, H, 0.5f, 3, &pool);

        std::vector<u8> indices(W * H);
        result.mapPixels(pixels.data(), W * H, indices.data());
        for (u32 i = 0; i < W * H; ++i)
            if (indices[i] >= result.color_count)
                TEST_FAIL(19, "parallel refine: idx[%u]=%u >= %u", i, indices[i], result.color_count);

        TEST_PASS(19, "parallel refineDitherAware: indices valid, %u colors", result.color_count);
    }

    // Test 20: Serial vs parallel refineDitherAware — identical
    {
        constexpr u32 W = 64, H = 64;
        auto pixels = make_rgba(W, H, hash_fill);
        const u32 pc = W * H;

        auto serial = Quantizer()
                          .maxColors(64)
                          .kmeansIterations(5)
                          .quantize(pixels.data(), pc, nullptr);
        serial.refineDitherAware(pixels.data(), W, H, 0.5f, 3, nullptr);

        auto parallel = Quantizer()
                            .maxColors(64)
                            .kmeansIterations(5)
                            .quantize(pixels.data(), pc, &pool);
        parallel.refineDitherAware(pixels.data(), W, H, 0.5f, 3, &pool);

        if (!results_equal(serial, parallel))
            TEST_FAIL(20, "serial vs parallel refineDitherAware differ");

        TEST_PASS(20, "serial vs parallel refineDitherAware: identical");
    }

    // ====================================================================
    // Large / stress tests
    // ====================================================================

    printf("\n--- Stress tests ---\n");

    // Test 21: 512x512 parallel — runs without crash, low MSE
    {
        constexpr u32 W = 512, H = 512;
        auto pixels = make_rgba(W, H, hash_fill);
        const u32 pc = W * H;

        auto result = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(10)
                          .quantize(pixels.data(), pc, &pool);

        std::vector<u8> indices(pc);
        result.mapPixels(pixels.data(), pc, indices.data());
        double mse = compute_palette_mse(pixels.data(), pc, indices.data(), result.palette);

        if (mse > 400.0)
            TEST_FAIL(21, "512x512 MSE=%.2f too high", mse);

        TEST_PASS(21, "512x512 parallel: %u colors, MSE=%.2f", result.color_count, mse);
    }

    // Test 22: 512x512 with ditherAware parallel
    {
        constexpr u32 W = 512, H = 512;
        auto pixels = make_rgba(W, H, hash_fill);
        const u32 pc = W * H;

        auto result = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(5)
                          .ditherAware(W, H, 0.5f, 3)
                          .quantize(pixels.data(), pc, &pool);

        std::vector<u8> dithered(pc);
        result.mapPixelsDithered(pixels.data(), W, H, 0.5f, dithered.data());

        for (u32 i = 0; i < pc; ++i)
            if (dithered[i] >= result.color_count)
                TEST_FAIL(22, "512x512 dithered[%u]=%u >= %u", i, dithered[i], result.color_count);

        TEST_PASS(22, "512x512 ditherAware parallel: %u colors, dithered indices valid",
             result.color_count);
    }

    // Test 23: 1024x1024 serial vs parallel parity
    {
        constexpr u32 W = 1024, H = 1024;
        auto pixels = make_rgba(W, H, hash_fill);
        const u32 pc = W * H;

        auto serial = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(5)
                          .quantize(pixels.data(), pc, nullptr);

        auto parallel = Quantizer()
                            .maxColors(256)
                            .kmeansIterations(5)
                            .quantize(pixels.data(), pc, &pool);

        if (!results_equal(serial, parallel))
            TEST_FAIL(23, "1024x1024 serial vs parallel differ");

        TEST_PASS(23, "1024x1024 serial vs parallel: identical");
    }

    // ====================================================================
    // Benchmarks
    // ====================================================================

    printf("\n--- Benchmarks ---\n");

    {
        constexpr u32 W = 1024, H = 1024;
        auto pixels = make_rgba(W, H, hash_fill);
        const u32 pc = W * H;

        // Serial benchmark
        auto t0 = std::chrono::high_resolution_clock::now();
        auto serialResult = Quantizer()
                                .maxColors(256)
                                .kmeansIterations(10)
                                .quantize(pixels.data(), pc, nullptr);
        auto t1 = std::chrono::high_resolution_clock::now();
        double serialMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Parallel benchmark
        t0 = std::chrono::high_resolution_clock::now();
        auto parallelResult = Quantizer()
                                  .maxColors(256)
                                  .kmeansIterations(10)
                                  .quantize(pixels.data(), pc, &pool);
        t1 = std::chrono::high_resolution_clock::now();
        double parallelMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        printf("BENCH 1024x1024 quantize (256 colors, 10 kmeans):\n");
        printf("  Serial:   %.1f ms\n", serialMs);
        printf("  Parallel: %.1f ms (%zu threads)\n", parallelMs, numThreads);
        printf("  Speedup:  %.2fx\n", serialMs / parallelMs);

        // Dither-aware benchmark
        t0 = std::chrono::high_resolution_clock::now();
        auto ditherSerial = Quantizer()
                                .maxColors(256)
                                .kmeansIterations(5)
                                .ditherAware(W, H, 0.5f, 5)
                                .quantize(pixels.data(), pc, nullptr);
        t1 = std::chrono::high_resolution_clock::now();
        double ditherSerialMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        t0 = std::chrono::high_resolution_clock::now();
        auto ditherParallel = Quantizer()
                                  .maxColors(256)
                                  .kmeansIterations(5)
                                  .ditherAware(W, H, 0.5f, 5)
                                  .quantize(pixels.data(), pc, &pool);
        t1 = std::chrono::high_resolution_clock::now();
        double ditherParallelMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        printf("BENCH 1024x1024 quantize+ditherAware (256 colors, 5 kmeans, 5 dither):\n");
        printf("  Serial:   %.1f ms\n", ditherSerialMs);
        printf("  Parallel: %.1f ms (%zu threads)\n", ditherParallelMs, numThreads);
        printf("  Speedup:  %.2fx\n", ditherSerialMs / ditherParallelMs);

        // Async benchmark
        t0 = std::chrono::high_resolution_clock::now();
        auto sem = pool.createTimelineSemaphore();
        auto startVal = sem->next();
        sem->signal(startVal);
        QuantizeResult asyncResult;
        auto endVal = Quantizer()
                          .maxColors(256)
                          .kmeansIterations(10)
                          .quantizeAsync(pixels.data(), pc, &pool, sem.get(),
                                         startVal, &asyncResult);
        sem->wait(endVal);
        t1 = std::chrono::high_resolution_clock::now();
        double asyncMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        printf("BENCH 1024x1024 quantizeAsync (256 colors, 10 kmeans):\n");
        printf("  Async:  %.1f ms (%zu threads)\n", asyncMs, numThreads);
    }

    // ====================================================================
    // Summary
    // ====================================================================

    printf("\n=== All quantizer tests passed ===\n");
    }

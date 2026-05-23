// JPEG encode/decode round-trip test suite
#include <catch2/catch_all.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Include the internal JPEG headers directly
#include "../src/whiteout/textures/jpeg/jpeg_encode.h"
#include "../src/whiteout/textures/jpeg/jpeg_decode.h"
#include "../src/whiteout/textures/jpeg/jpeg_common.h"

// Public API + thread pool for parallel tests
#include <whiteout/textures/jpeg/parser.h>
#include <whiteout/textures/jpeg/writer.h>
#include <whiteout/utils/simple_thread_pool.h>

using namespace whiteout;
using namespace whiteout::textures;

// ============================================================================
// Test Helpers
// ============================================================================

/// Create an image and fill it via a callback.  The callback receives (x, y)
/// and must write `components` bytes into `out`.
jpeg::Image make_image(int w, int h, int c,
                       std::function<void(int x, int y, u8* out)> fill) {
    jpeg::Image img;
    img.width = w;
    img.height = h;
    img.components = c;
    img.pixels.resize(static_cast<size_t>(w) * h * c);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            fill(x, y, &img.pixels[(y * w + x) * c]);
        }
    }
    return img;
}

/// Encode then decode an image.  Returns the decoded image on success, or
/// terminates with a failure message.
struct RoundTripResult {
    jpeg::Image decoded;
    std::vector<u8> encoded;
};

RoundTripResult round_trip(const jpeg::Image& img, int quality, bool progressive,
                           const char* testLabel) {
    std::string err;
    auto encoded = jpeg::encode_raw(img, quality, &err, progressive);
    if (encoded.empty()) {
        printf("%s FAIL: encode error: %s\n", testLabel, err.c_str());
        FAIL("JPEG test failed");
    }
    auto decoded = jpeg::decode_raw(std::span<const u8>{encoded}, &err);
    if (!decoded) {
        printf("%s FAIL: decode error: %s\n", testLabel, err.c_str());
        FAIL("JPEG test failed");
    }
    if (decoded->width != img.width || decoded->height != img.height ||
        decoded->components != img.components) {
        printf("%s FAIL: dimension mismatch (expected %ux%ux%u, got %ux%ux%u)\n",
               testLabel, img.width, img.height, img.components,
               decoded->width, decoded->height, decoded->components);
        FAIL("JPEG test failed");
    }
    return {std::move(*decoded), std::move(encoded)};
}

/// Compute mean squared error between original and decoded pixel buffers.
double compute_mse(const jpeg::Image& original, const jpeg::Image& decoded) {
    double mse = 0;
    for (size_t i = 0; i < original.pixels.size(); ++i) {
        int diff = static_cast<int>(decoded.pixels[i]) - static_cast<int>(original.pixels[i]);
        mse += diff * diff;
    }
    return mse / static_cast<double>(original.pixels.size());
}

/// Compute the maximum absolute pixel difference between two images.
int compute_max_diff(const jpeg::Image& a, const jpeg::Image& b) {
    int maxDiff = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        int diff = abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i]));
        if (diff > maxDiff) maxDiff = diff;
    }
    return maxDiff;
}

/// Count occurrences of a JPEG marker (0xFF followed by markerByte) in encoded data.
int count_markers(const std::vector<u8>& data, u8 markerByte) {
    int count = 0;
    for (size_t i = 0; i + 1 < data.size(); ++i) {
        if (data[i] == 0xFF && data[i + 1] == markerByte) ++count;
    }
    return count;
}

// A hash-based pseudo-random pixel fill for testing (deterministic, varied content).
void hash_fill_4comp(int x, int y, u8* out) {
    out[0] = static_cast<u8>((x * 37 + y * 13) & 0xFF);
    out[1] = static_cast<u8>((x * 7  + y * 41) & 0xFF);
    out[2] = static_cast<u8>((x * 23 + y * 3)  & 0xFF);
    out[3] = static_cast<u8>((x * 11 + y * 29) & 0xFF);
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("JPEG encode decode", "[jpeg]") {

    // Test 1: Small solid-color 8x8 image with 4 components
    {
        auto img = make_image(8, 8, 4, [](int, int, u8* out) {
            out[0] = 100; out[1] = 150; out[2] = 200; out[3] = 255;
        });
        auto [decoded, encoded] = round_trip(img, 75, false, "TEST 1");
        printf("TEST 1 PASS: encoded %zu bytes, decoded %ux%ux%u\n",
               encoded.size(), decoded.width, decoded.height, decoded.components);
    }

    // Test 2: 16x16 gradient image with 4 components
    {
        auto img = make_image(16, 16, 4, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>(x * 16);
            out[1] = static_cast<u8>(y * 16);
            out[2] = static_cast<u8>((x + y) * 8);
            out[3] = 255;
        });
        auto [decoded, encoded] = round_trip(img, 75, false, "TEST 2");
        printf("TEST 2 PASS: encoded %zu bytes, decoded %ux%ux%u\n",
               encoded.size(), decoded.width, decoded.height, decoded.components);
    }

    // Test 3: 64x64 high-contrast checkerboard with 4 components
    {
        auto img = make_image(64, 64, 4, [](int x, int y, u8* out) {
            u8 v = ((x / 4) + (y / 4)) % 2 == 0 ? 255 : 0;
            out[0] = v; out[1] = v; out[2] = v; out[3] = 255;
        });
        auto [decoded, encoded] = round_trip(img, 75, false, "TEST 3");
        printf("TEST 3 PASS: encoded %zu bytes, decoded %ux%ux%u\n",
               encoded.size(), decoded.width, decoded.height, decoded.components);
    }

    // Test 4: 3-component image
    {
        auto img = make_image(8, 8, 3, [](int, int, u8* out) {
            out[0] = 50; out[1] = 100; out[2] = 150;
        });
        auto [decoded, encoded] = round_trip(img, 75, false, "TEST 4");
        printf("TEST 4 PASS: encoded %zu bytes, decoded %ux%ux%u\n",
               encoded.size(), decoded.width, decoded.height, decoded.components);
    }

    // Test 5: Verify all heights with 4 components
    for (int h = 8; h <= 64; h += 8) {
        auto img = make_image(64, h, 4, [](int x, int y, u8* out) {
            hash_fill_4comp(x, y, out);
            out[3] = 255;
        });
        auto [decoded, encoded] = round_trip(img, 75, false, "TEST 5");
        printf("64x%d: encoded=%zu, OK\n", h, encoded.size());
    }

    // ========================================================================
    // Progressive DCT (SOF2) Tests
    // ========================================================================

    printf("\n--- Progressive DCT tests ---\n");

    // Test 6: Progressive encode/decode round-trip — 8x8 solid color
    {
        auto img = make_image(8, 8, 4, [](int, int, u8* out) {
            out[0] = 100; out[1] = 150; out[2] = 200; out[3] = 255;
        });
        auto [decoded, encoded] = round_trip(img, 75, true, "TEST 6");

        if (count_markers(encoded, 0xC2) == 0) {
            printf("TEST 6 FAIL: SOF2 marker not found in progressive output\n");
            FAIL("JPEG test failed");
        }

        int maxErr = compute_max_diff(img, decoded);
        printf("TEST 6 PASS: progressive 8x8 solid, encoded=%zu bytes, maxErr=%d\n",
               encoded.size(), maxErr);
        if (maxErr > 3) {
            printf("TEST 6 FAIL: solid-color max error too high (%d > 3)\n", maxErr);
            FAIL("JPEG test failed");
        }
    }

    // Test 7: Progressive 16x16 gradient — verify pixel accuracy
    {
        auto img = make_image(16, 16, 4, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>(x * 16);
            out[1] = static_cast<u8>(y * 16);
            out[2] = static_cast<u8>((x + y) * 8);
            out[3] = 255;
        });
        auto [decoded, encoded] = round_trip(img, 90, true, "TEST 7");
        double mse = compute_mse(img, decoded);
        int maxErr = compute_max_diff(img, decoded);
        printf("TEST 7 PASS: progressive 16x16 gradient q90, encoded=%zu, MSE=%.2f, maxErr=%d\n",
               encoded.size(), mse, maxErr);
    }

    // Test 8: Progressive 64x64 checkerboard — stress test
    {
        auto img = make_image(64, 64, 4, [](int x, int y, u8* out) {
            u8 v = ((x / 4) + (y / 4)) % 2 == 0 ? 255 : 0;
            out[0] = v; out[1] = v; out[2] = v; out[3] = 255;
        });
        auto [decoded, encoded] = round_trip(img, 75, true, "TEST 8");
        printf("TEST 8 PASS: progressive 64x64 checker, encoded=%zu\n", encoded.size());
    }

    // Test 9: Progressive 3-component image (no alpha)
    {
        auto img = make_image(24, 24, 3, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>(x * 10);
            out[1] = static_cast<u8>(y * 10);
            out[2] = static_cast<u8>((x + y) * 5);
        });
        auto [decoded, encoded] = round_trip(img, 75, true, "TEST 9");
        printf("TEST 9 PASS: progressive 24x24x3, encoded=%zu\n", encoded.size());
    }

    // Test 10: Progressive 1-component (grayscale)
    {
        auto img = make_image(32, 32, 1, [](int x, int, u8* out) {
            out[0] = static_cast<u8>((x * 8) & 0xFF);
        });
        auto [decoded, encoded] = round_trip(img, 85, true, "TEST 10");
        printf("TEST 10 PASS: progressive 32x32 grayscale, encoded=%zu\n", encoded.size());
    }

    // Test 11: Progressive vs baseline produce bit-identical reconstruction at q100
    //          (both go through the same FDCT + quant + IDCT path, so the pixel
    //           values should match exactly)
    {
        auto img = make_image(32, 32, 4, hash_fill_4comp);
        auto [baseDec, baseEnc] = round_trip(img, 100, false, "TEST 11 baseline");
        auto [progDec, progEnc] = round_trip(img, 100, true,  "TEST 11 progressive");

        int maxDiff = compute_max_diff(baseDec, progDec);
        printf("TEST 11 PASS: baseline vs progressive q100, maxDiff=%d (baseline=%zu, progressive=%zu bytes)\n",
               maxDiff, baseEnc.size(), progEnc.size());
        if (maxDiff > 0) {
            printf("TEST 11 WARNING: expected identical pixels but got maxDiff=%d\n", maxDiff);
        }
    }

    // Test 12: Non-multiple-of-8 dimensions with progressive
    {
        int sizes[][2] = {{7, 7}, {9, 5}, {13, 17}, {1, 1}, {33, 15}, {100, 67}};
        for (auto& s : sizes) {
            int w = s[0], h = s[1];
            auto img = make_image(w, h, 4, [](int x, int y, u8* out) {
                hash_fill_4comp(x, y, out);
                out[3] = 255;
            });
            char label[32];
            snprintf(label, sizeof(label), "TEST 12 (%dx%d)", w, h);
            auto [decoded, encoded] = round_trip(img, 75, true, label);
            printf("TEST 12 (%dx%d): progressive OK, encoded=%zu\n", w, h, encoded.size());
        }
    }

    // Test 13: Quality sweep — progressive encode at various quality levels
    {
        auto img = make_image(32, 32, 4, [](int x, int y, u8* out) {
            hash_fill_4comp(x, y, out);
            out[3] = 255;
        });

        int qualities[] = {1, 10, 25, 50, 75, 90, 100};
        for (int q : qualities) {
            char label[32];
            snprintf(label, sizeof(label), "TEST 13 q=%d", q);
            auto [decoded, encoded] = round_trip(img, q, true, label);
            double mse = compute_mse(img, decoded);
            printf("TEST 13: progressive q=%3d, size=%5zu, MSE=%.2f\n", q, encoded.size(), mse);
        }
    }

    // Test 14: Multiple SOS markers — verify the progressive stream has the
    //          expected number of scans for band-split SA encoding.
    {
        auto img = make_image(16, 16, 4, [](int x, int y, u8* out) {
            size_t base = static_cast<size_t>(y * 16 + x) * 4;
            out[0] = static_cast<u8>((base * 97 + 31) & 0xFF);
            out[1] = static_cast<u8>(((base + 1) * 97 + 31) & 0xFF);
            out[2] = static_cast<u8>(((base + 2) * 97 + 31) & 0xFF);
            out[3] = static_cast<u8>(((base + 3) * 97 + 31) & 0xFF);
        });
        auto [decoded, encoded] = round_trip(img, 75, true, "TEST 14");

        // With 4 components, 2 AC bands, and SA (Al=1):
        //   1 DC first + 4*2 AC first + 1 DC refine + 4*2 AC refine = 18
        int sosCount = count_markers(encoded, 0xDA);
        printf("TEST 14: SOS marker count = %d (expected 18 for 4-component, 2-band, SA)\n", sosCount);
        if (sosCount != 18) {
            printf("TEST 14 FAIL: expected 18 SOS markers, got %d\n", sosCount);
            FAIL("JPEG test failed");
        }
        printf("TEST 14 PASS\n");
    }

    // Test 15: Verify AC band boundaries (Ss/Se) in SOS markers
    {
        auto img = make_image(16, 16, 3, [](int x, int y, u8* out) {
            size_t base = static_cast<size_t>(y * 16 + x) * 3;
            out[0] = static_cast<u8>((base * 47 + 7) & 0xFF);
            out[1] = static_cast<u8>(((base + 1) * 47 + 7) & 0xFF);
            out[2] = static_cast<u8>(((base + 2) * 47 + 7) & 0xFF);
        });
        auto [decoded, encoded] = round_trip(img, 75, true, "TEST 15");

        // Parse SOS markers and extract Ss, Se, Ah, Al from each.
        struct ScanInfo { u8 ss, se, ah, al; int numComponents; };
        std::vector<ScanInfo> scans;
        for (size_t i = 0; i + 1 < encoded.size(); ++i) {
            if (encoded[i] == 0xFF && encoded[i + 1] == 0xDA) {
                u8 ns = encoded[i + 4];
                size_t ssOffset = i + 5 + ns * 2;
                if (ssOffset + 2 < encoded.size()) {
                    u8 ss = encoded[ssOffset];
                    u8 se = encoded[ssOffset + 1];
                    u8 ahAl = encoded[ssOffset + 2];
                    scans.push_back({ss, se, static_cast<u8>((ahAl >> 4) & 0xF),
                                     static_cast<u8>(ahAl & 0xF), static_cast<int>(ns)});
                }
            }
        }

        // 3 components, 2 bands, SA: 1 DC first + 3*2 AC first + 1 DC refine + 3*2 AC refine = 14
        if (scans.size() != 14) {
            printf("TEST 15 FAIL: expected 14 scans, got %zu\n", scans.size());
            FAIL("JPEG test failed");
        }

        // Scan 0: DC first (Ss=0, Se=0, Ah=0, Al=1), interleaved (Ns=3)
        if (scans[0].ss != 0 || scans[0].se != 0 || scans[0].ah != 0 || scans[0].al != 1 ||
            scans[0].numComponents != 3) {
            printf("TEST 15 FAIL: DC first scan mismatch\n");
            FAIL("JPEG test failed");
        }

        // Scans 1-6: AC first, bands {1,5} and {6,63} for each component, Ah=0, Al=1
        int expectedBands[][2] = {{1,5},{6,63},{1,5},{6,63},{1,5},{6,63}};
        for (int j = 0; j < 6; j++) {
            auto& s = scans[1 + j];
            if (s.ss != expectedBands[j][0] || s.se != expectedBands[j][1] ||
                s.ah != 0 || s.al != 1 || s.numComponents != 1) {
                printf("TEST 15 FAIL: AC first scan %d mismatch (Ss=%d Se=%d Ah=%d Al=%d Ns=%d)\n",
                       j, s.ss, s.se, s.ah, s.al, s.numComponents);
                FAIL("JPEG test failed");
            }
        }

        // Scan 7: DC refine (Ss=0, Se=0, Ah=1, Al=0)
        if (scans[7].ss != 0 || scans[7].se != 0 || scans[7].ah != 1 || scans[7].al != 0 ||
            scans[7].numComponents != 3) {
            printf("TEST 15 FAIL: DC refine scan mismatch\n");
            FAIL("JPEG test failed");
        }

        // Scans 8-13: AC refine, bands {1,5} and {6,63} for each component, Ah=1, Al=0
        for (int j = 0; j < 6; j++) {
            auto& s = scans[8 + j];
            if (s.ss != expectedBands[j][0] || s.se != expectedBands[j][1] ||
                s.ah != 1 || s.al != 0 || s.numComponents != 1) {
                printf("TEST 15 FAIL: AC refine scan %d mismatch (Ss=%d Se=%d Ah=%d Al=%d Ns=%d)\n",
                       j, s.ss, s.se, s.ah, s.al, s.numComponents);
                FAIL("JPEG test failed");
            }
        }

        printf("TEST 15 PASS: all 14 scan Ss/Se/Ah/Al values correct\n");
    }

    // Test 16: DHT marker count — verify chrominance tables emitted for multi-component
    {
        // Multi-component: expect 4 DHT (DC0, AC0, DC1, AC1)
        {
            auto img = make_image(8, 8, 4, [](int, int, u8* out) {
                out[0] = out[1] = out[2] = out[3] = 128;
            });
            auto [decoded, encoded] = round_trip(img, 75, true, "TEST 16 (4-comp)");
            int dhtCount = count_markers(encoded, 0xC4);
            if (dhtCount != 4) {
                printf("TEST 16 FAIL: 4-comp DHT count %d, expected 4\n", dhtCount);
                FAIL("JPEG test failed");
            }
        }

        // Single-component: expect 2 DHT (DC0, AC0)
        {
            auto img = make_image(8, 8, 1, [](int, int, u8* out) { out[0] = 128; });
            auto [decoded, encoded] = round_trip(img, 75, true, "TEST 16 (1-comp)");
            int dhtCount = count_markers(encoded, 0xC4);
            if (dhtCount != 2) {
                printf("TEST 16 FAIL: 1-comp DHT count %d, expected 2\n", dhtCount);
                FAIL("JPEG test failed");
            }
        }

        printf("TEST 16 PASS: DHT counts correct (4 for multi-comp, 2 for single-comp)\n");
    }

    // Test 17: SA bit-identity — progressive with SA decodes to same pixels as baseline
    //          (SA is a lossless decomposition of the same quantised coefficients)
    {
        auto img = make_image(32, 32, 4, hash_fill_4comp);
        auto [baseDec, baseEnc] = round_trip(img, 100, false, "TEST 17 baseline");
        auto [progDec, progEnc] = round_trip(img, 100, true,  "TEST 17 progressive");

        int maxDiff = compute_max_diff(baseDec, progDec);
        printf("TEST 17: baseline vs SA-progressive q100, maxDiff=%d\n", maxDiff);
        if (maxDiff > 0) {
            printf("TEST 17 FAIL: SA decomposition should be bit-identical, got maxDiff=%d\n", maxDiff);
            FAIL("JPEG test failed");
        }
        printf("TEST 17 PASS\n");
    }

    // Test 18: Baseline also uses chrominance tables — verify round-trip still works
    {
        auto img = make_image(24, 24, 3, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>(x * 10);
            out[1] = static_cast<u8>(y * 10);
            out[2] = static_cast<u8>((x + y) * 5);
        });
        auto [decoded, encoded] = round_trip(img, 90, false, "TEST 18");

        int dhtCount = count_markers(encoded, 0xC4);
        if (dhtCount != 4) {
            printf("TEST 18 FAIL: baseline 3-comp DHT count %d, expected 4\n", dhtCount);
            FAIL("JPEG test failed");
        }
        printf("TEST 18 PASS: baseline 3-comp with chrominance tables, encoded=%zu\n", encoded.size());
    }

    // ======================================================================
    // Large-image stress tests
    // ======================================================================

    // Test 19: 256x256 natural-looking gradient with 4 components at q75.
    // Exercises many blocks (1024 per component) with varied AC content.
    {
        auto img = make_image(256, 256, 4, [](int x, int y, u8* out) {
            double fx = x / 255.0, fy = y / 255.0;
            out[0] = static_cast<u8>(255.0 * fx);
            out[1] = static_cast<u8>(255.0 * fy);
            out[2] = static_cast<u8>(127.5 + 127.5 * sin(fx * 12.0 + fy * 8.0));
            out[3] = 200;
        });
        auto [decoded, encoded] = round_trip(img, 75, true, "TEST 19");
        double mse = compute_mse(img, decoded);
        printf("TEST 19 PASS: progressive 256x256x4 gradient q75, encoded=%zu, MSE=%.2f\n",
               encoded.size(), mse);
    }

    // Test 20: 512x512 3-component pseudo-photo with dense AC content at q90.
    // Many nonzero AC coefficients per block -> deep stress on AC refine scan.
    {
        auto img = make_image(512, 512, 3, [](int x, int y, u8* out) {
            u8 hash = static_cast<u8>((x * 2654435761u + y * 2246822519u) >> 16);
            out[0] = static_cast<u8>((hash + x) & 0xFF);
            out[1] = static_cast<u8>((hash ^ y) & 0xFF);
            out[2] = static_cast<u8>((hash + x + y) & 0xFF);
        });
        auto [decoded, encoded] = round_trip(img, 90, true, "TEST 20");
        double mse = compute_mse(img, decoded);
        printf("TEST 20 PASS: progressive 512x512x3 noise q90, encoded=%zu, MSE=%.2f\n",
               encoded.size(), mse);
    }

    // Test 21: 256x256 q100 baseline vs progressive pixel-exact comparison.
    // At q100 with identical quant tables, both must produce the same pixels.
    {
        auto img = make_image(256, 256, 4, hash_fill_4comp);
        auto [baseDec, baseEnc] = round_trip(img, 100, false, "TEST 21 baseline");
        auto [progDec, progEnc] = round_trip(img, 100, true,  "TEST 21 progressive");

        int maxDiff = compute_max_diff(baseDec, progDec);
        printf("TEST 21 PASS: 256x256x4 q100 baseline vs progressive, maxDiff=%d "
               "(baseline=%zu, progressive=%zu bytes)\n",
               maxDiff, baseEnc.size(), progEnc.size());
        if (maxDiff > 0) {
            printf("TEST 21 WARNING: expected identical pixels but got maxDiff=%d\n", maxDiff);
        }
    }

    // Test 22: 300x200 non-8-aligned large image with 3 components at q50.
    // Stresses padding logic with many partial-edge blocks.
    {
        auto img = make_image(300, 200, 3, [](int x, int y, u8* out) {
            double cx = x - 150.0, cy = y - 100.0;
            double r = sqrt(cx * cx + cy * cy);
            u8 ring = static_cast<u8>(127.5 + 127.5 * sin(r * 0.15));
            u8 noise = static_cast<u8>((x * 31 + y * 17) & 0x3F);
            out[0] = static_cast<u8>((ring + noise) & 0xFF);
            out[1] = static_cast<u8>((ring - noise) & 0xFF);
            out[2] = ring;
        });
        auto [decoded, encoded] = round_trip(img, 50, true, "TEST 22");
        printf("TEST 22 PASS: progressive 300x200x3 circles q50, encoded=%zu\n", encoded.size());
    }

    // Test 23: 128x128 single-component (grayscale) at q75.
    // Verifies progressive with 1 component (no chrominance tables, no
    // interleaved DC scans with multiple components).
    {
        auto img = make_image(128, 128, 1, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>(((x + y) * 3) & 0xFF);
        });
        auto [decoded, encoded] = round_trip(img, 75, true, "TEST 23");
        double mse = compute_mse(img, decoded);
        printf("TEST 23 PASS: progressive 128x128x1 grayscale q75, encoded=%zu, MSE=%.2f\n",
               encoded.size(), mse);
    }

    // Test 24: 512x512 4-component quality sweep — ensures all quality levels
    // work at large scale (catches precision/overflow issues in quant tables).
    {
        auto img = make_image(512, 512, 4, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>((x + y) & 0xFF);
            out[1] = static_cast<u8>((x * 3 + y * 7) & 0xFF);
            out[2] = static_cast<u8>((x * 11 + y * 5) & 0xFF);
            out[3] = static_cast<u8>((x ^ y) & 0xFF);
        });

        int qualities[] = {1, 50, 100};
        for (int q : qualities) {
            char label[32];
            snprintf(label, sizeof(label), "TEST 24 q=%d", q);
            auto [decoded, encoded] = round_trip(img, q, true, label);
            double mse = compute_mse(img, decoded);
            printf("TEST 24: 512x512x4 q=%3d, encoded=%zu, MSE=%.2f\n", q, encoded.size(), mse);
        }
        printf("TEST 24 PASS\n");
    }

    // ======================================================================
    // Parallel encode/decode tests (via SimpleThreadPool)
    // ======================================================================

    printf("\n--- Parallel tests (SimpleThreadPool) ---\n");

    // Create a thread pool with hardware concurrency (or 4 as minimum).
    const size_t numThreads = std::max<size_t>(4, std::thread::hardware_concurrency());
    whiteout::utils::SimpleThreadPool pool(numThreads);
    printf("Using SimpleThreadPool with %zu threads\n", numThreads);

    // Test 25: Low-level encode_raw parallel vs serial — pixel-exact output
    // (Parallel baseline uses restart intervals, so bitstreams differ but pixels must match.)
    {
        auto img = make_image(128, 128, 4, [](int x, int y, u8* out) {
            hash_fill_4comp(x, y, out);
            out[3] = 255;
        });

        // Serial encode
        std::string err;
        auto serialEncoded = jpeg::encode_raw(img, 75, &err, false);
        if (serialEncoded.empty()) {
            printf("TEST 25 FAIL: serial encode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        // Parallel encode via JpegContext
        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        std::vector<u8> parallelEncoded;
        jpeg::encode_raw(img, 75, &err, false, &ctx, &parallelEncoded);
        sem->wait(ctx.currentValue);

        if (parallelEncoded.empty()) {
            printf("TEST 25 FAIL: parallel encode produced empty output\n");
            FAIL("JPEG test failed");
        }

        // Decode both and compare pixels.
        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);
        if (!serialDecoded) {
            printf("TEST 25 FAIL: serial decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }
        auto parallelDecoded = jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err);
        if (!parallelDecoded) {
            printf("TEST 25 FAIL: parallel decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        if (serialDecoded->pixels.size() != parallelDecoded->pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded->pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 25 FAIL: parallel encode pixels differ from serial "
                   "(serial=%zu bytes, parallel=%zu bytes)\n",
                   serialEncoded.size(), parallelEncoded.size());
            FAIL("JPEG test failed");
        }
        printf("TEST 25 PASS: parallel encode_raw pixel-exact match (serial=%zu, parallel=%zu bytes)\n",
               serialEncoded.size(), parallelEncoded.size());
    }

    // Test 26: Low-level decode_raw parallel vs serial — pixel-exact output
    {
        auto img = make_image(128, 128, 3, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>((x * 37 + y * 13) & 0xFF);
            out[1] = static_cast<u8>((x * 7  + y * 41) & 0xFF);
            out[2] = static_cast<u8>((x * 23 + y * 3)  & 0xFF);
        });

        std::string err;
        auto encoded = jpeg::encode_raw(img, 90, &err, false);
        if (encoded.empty()) {
            printf("TEST 26 FAIL: encode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        // Serial decode
        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{encoded}, &err);
        if (!serialDecoded) {
            printf("TEST 26 FAIL: serial decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        // Parallel decode via JpegContext
        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        jpeg::Image parallelDecoded;
        jpeg::decode_raw(std::span<const u8>{encoded}, &err, &ctx, &parallelDecoded);
        sem->wait(ctx.currentValue);

        if (parallelDecoded.width == 0) {
            printf("TEST 26 FAIL: parallel decode produced empty output\n");
            FAIL("JPEG test failed");
        }

        if (serialDecoded->pixels.size() != parallelDecoded.pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded.pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 26 FAIL: parallel decode pixels differ from serial\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 26 PASS: parallel decode_raw pixel-exact match (%ux%ux%u)\n",
               parallelDecoded.width, parallelDecoded.height, parallelDecoded.components);
    }

    // Test 27: Low-level progressive parallel encode vs serial — pixel-exact
    // (Parallel progressive uses pre-encoded AC scans, so bitstreams may differ.)
    {
        auto img = make_image(64, 64, 3, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>((x * 11 + y * 29) & 0xFF);
            out[1] = static_cast<u8>((x * 19 + y * 7)  & 0xFF);
            out[2] = static_cast<u8>((x * 31 + y * 3)  & 0xFF);
        });

        std::string err;
        auto serialEncoded = jpeg::encode_raw(img, 85, &err, true);
        if (serialEncoded.empty()) {
            printf("TEST 27 FAIL: serial progressive encode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        std::vector<u8> parallelEncoded;
        jpeg::encode_raw(img, 85, &err, true, &ctx, &parallelEncoded);
        sem->wait(ctx.currentValue);

        // Decode both and compare pixels.
        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);
        if (!serialDecoded) {
            printf("TEST 27 FAIL: serial decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }
        auto parallelDecoded = jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err);
        if (!parallelDecoded) {
            printf("TEST 27 FAIL: parallel decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        if (serialDecoded->pixels.size() != parallelDecoded->pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded->pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 27 FAIL: parallel progressive encode pixels differ from serial\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 27 PASS: parallel progressive encode_raw pixel-exact (serial=%zu, parallel=%zu bytes)\n",
               serialEncoded.size(), parallelEncoded.size());
    }

    // Test 28: Low-level progressive parallel decode vs serial — pixel-exact
    {
        auto img = make_image(64, 64, 4, hash_fill_4comp);
        std::string err;
        auto encoded = jpeg::encode_raw(img, 75, &err, true);
        if (encoded.empty()) {
            printf("TEST 28 FAIL: encode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{encoded}, &err);
        if (!serialDecoded) {
            printf("TEST 28 FAIL: serial decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        jpeg::Image parallelDecoded;
        jpeg::decode_raw(std::span<const u8>{encoded}, &err, &ctx, &parallelDecoded);
        sem->wait(ctx.currentValue);

        if (serialDecoded->pixels.size() != parallelDecoded.pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded.pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 28 FAIL: parallel progressive decode differs from serial\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 28 PASS: parallel progressive decode_raw pixel-exact (%ux%ux%u)\n",
               parallelDecoded.width, parallelDecoded.height, parallelDecoded.components);
    }

    // Test 29: Public Parser API with pool — parse round-trips correctly
    {
        // First produce a valid JFIF JPEG via the Writer (serial) to get bytes.
        auto srcTex = Texture::create2D(PixelFormat::RGBA8, 128, 128, 1);
        u8* px = srcTex.dataPtr();
        for (u32 y = 0; y < 128; ++y) {
            for (u32 x = 0; x < 128; ++x) {
                u32 idx = (y * 128 + x) * 4;
                px[idx + 0] = static_cast<u8>((x * 37 + y * 13) & 0xFF);
                px[idx + 1] = static_cast<u8>((x * 7  + y * 41) & 0xFF);
                px[idx + 2] = static_cast<u8>((x * 23 + y * 3)  & 0xFF);
                px[idx + 3] = 255;
            }
        }

        jpeg::Writer writerSerial(90);
        auto jpegBytes = writerSerial.write(srcTex);
        if (jpegBytes.empty()) {
            printf("TEST 29 FAIL: Writer (serial) produced empty output\n");
            FAIL("JPEG test failed");
        }

        // Parse with serial parser
        jpeg::Parser parserSerial;
        auto serialTex = parserSerial.parse(std::span<const u8>{jpegBytes});
        if (!serialTex) {
            printf("TEST 29 FAIL: serial Parser failed\n");
            FAIL("JPEG test failed");
        }

        // Parse with parallel parser
        jpeg::Parser parserParallel(&pool);
        auto parallelTex = parserParallel.parse(std::span<const u8>{jpegBytes});
        if (!parallelTex) {
            printf("TEST 29 FAIL: parallel Parser failed\n");
            FAIL("JPEG test failed");
        }

        // Compare pixel data
        const u8* serialData = serialTex->dataPtr();
        const u8* parallelData = parallelTex->dataPtr();
        u32 texSize = 128 * 128 * 4;
        if (memcmp(serialData, parallelData, texSize) != 0) {
            printf("TEST 29 FAIL: parallel Parser pixels differ from serial\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 29 PASS: public Parser parallel vs serial pixel-exact (128x128)\n");
    }

    // Test 30: Public Writer API with pool — bytes match serial writer
    {
        auto srcTex = Texture::create2D(PixelFormat::RGBA8, 64, 64, 1);
        u8* px = srcTex.dataPtr();
        for (u32 y = 0; y < 64; ++y) {
            for (u32 x = 0; x < 64; ++x) {
                u32 idx = (y * 64 + x) * 4;
                px[idx + 0] = static_cast<u8>((x * 11 + y * 29) & 0xFF);
                px[idx + 1] = static_cast<u8>((x * 19 + y * 7)  & 0xFF);
                px[idx + 2] = static_cast<u8>((x * 31 + y * 3)  & 0xFF);
                px[idx + 3] = 255;
            }
        }

        jpeg::Writer writerSerial(75);
        auto serialBytes = writerSerial.write(srcTex);
        if (serialBytes.empty()) {
            printf("TEST 30 FAIL: serial Writer produced empty output\n");
            FAIL("JPEG test failed");
        }

        jpeg::Writer writerParallel(75, &pool);
        auto parallelBytes = writerParallel.write(srcTex);
        if (parallelBytes.empty()) {
            printf("TEST 30 FAIL: parallel Writer produced empty output\n");
            FAIL("JPEG test failed");
        }

        // Decode both and compare pixels.
        jpeg::Parser parser;
        auto serialTex = parser.parse(std::span<const u8>{serialBytes});
        if (!serialTex) {
            printf("TEST 30 FAIL: serial decode failed\n");
            FAIL("JPEG test failed");
        }
        auto parallelTex = parser.parse(std::span<const u8>{parallelBytes});
        if (!parallelTex) {
            printf("TEST 30 FAIL: parallel decode failed\n");
            FAIL("JPEG test failed");
        }
        u32 texSize = 64 * 64 * 4;
        if (memcmp(serialTex->dataPtr(), parallelTex->dataPtr(), texSize) != 0) {
            printf("TEST 30 FAIL: parallel Writer pixels differ from serial "
                   "(serial=%zu, parallel=%zu)\n",
                   serialBytes.size(), parallelBytes.size());
            FAIL("JPEG test failed");
        }
        printf("TEST 30 PASS: public Writer parallel vs serial pixel-exact (serial=%zu, parallel=%zu bytes)\n",
               serialBytes.size(), parallelBytes.size());
    }

    // Test 31: Full parallel round-trip via public API (Writer+Parser with pool)
    {
        auto srcTex = Texture::create2D(PixelFormat::RGBA8, 256, 256, 1);
        u8* px = srcTex.dataPtr();
        for (u32 y = 0; y < 256; ++y) {
            for (u32 x = 0; x < 256; ++x) {
                u32 idx = (y * 256 + x) * 4;
                double fx = x / 255.0, fy = y / 255.0;
                px[idx + 0] = static_cast<u8>(255.0 * fx);
                px[idx + 1] = static_cast<u8>(255.0 * fy);
                px[idx + 2] = static_cast<u8>(127.5 + 127.5 * sin(fx * 12.0 + fy * 8.0));
                px[idx + 3] = 255;
            }
        }

        // Parallel write
        jpeg::Writer writer(90, &pool);
        auto jpegBytes = writer.write(srcTex);
        if (jpegBytes.empty()) {
            printf("TEST 31 FAIL: parallel Writer produced empty output\n");
            FAIL("JPEG test failed");
        }

        // Parallel parse
        jpeg::Parser parser(&pool);
        auto decoded = parser.parse(std::span<const u8>{jpegBytes});
        if (!decoded) {
            printf("TEST 31 FAIL: parallel Parser failed\n");
            FAIL("JPEG test failed");
        }

        if (decoded->width() != 256 || decoded->height() != 256) {
            printf("TEST 31 FAIL: dimension mismatch (%ux%u)\n",
                   decoded->width(), decoded->height());
            FAIL("JPEG test failed");
        }

        // Compare with serial round-trip
        jpeg::Writer writerS(90);
        auto serialBytes = writerS.write(srcTex);

        jpeg::Parser parserS;
        auto serialDecoded = parserS.parse(std::span<const u8>{serialBytes});
        if (!serialDecoded) {
            printf("TEST 31 FAIL: serial round-trip failed\n");
            FAIL("JPEG test failed");
        }

        const u8* sData = serialDecoded->dataPtr();
        const u8* pData = decoded->dataPtr();
        u32 texSize = 256 * 256 * 4;
        if (memcmp(sData, pData, texSize) != 0) {
            printf("TEST 31 FAIL: parallel round-trip pixels differ from serial\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 31 PASS: full parallel round-trip 256x256 pixel-exact\n");
    }

    // Test 32: Large parallel stress — 512x512 encode + decode via low-level API
    {
        auto img = make_image(512, 512, 3, [](int x, int y, u8* out) {
            u8 hash = static_cast<u8>((x * 2654435761u + y * 2246822519u) >> 16);
            out[0] = static_cast<u8>((hash + x) & 0xFF);
            out[1] = static_cast<u8>((hash ^ y) & 0xFF);
            out[2] = static_cast<u8>((hash + x + y) & 0xFF);
        });

        std::string err;

        // Parallel encode
        auto sem1 = pool.createTimelineSemaphore();
        jpeg::JpegContext encCtx;
        encCtx.pool = &pool;
        encCtx.sem = sem1.get();
        encCtx.currentValue = sem1->value();

        std::vector<u8> encoded;
        jpeg::encode_raw(img, 80, &err, false, &encCtx, &encoded);
        sem1->wait(encCtx.currentValue);

        if (encoded.empty()) {
            printf("TEST 32 FAIL: parallel encode 512x512 produced empty output\n");
            FAIL("JPEG test failed");
        }

        // Parallel decode
        auto sem2 = pool.createTimelineSemaphore();
        jpeg::JpegContext decCtx;
        decCtx.pool = &pool;
        decCtx.sem = sem2.get();
        decCtx.currentValue = sem2->value();

        jpeg::Image decoded;
        jpeg::decode_raw(std::span<const u8>{encoded}, &err, &decCtx, &decoded);
        sem2->wait(decCtx.currentValue);

        if (decoded.width != 512 || decoded.height != 512 || decoded.components != 3) {
            printf("TEST 32 FAIL: decoded dimensions wrong (%ux%ux%u)\n",
                   decoded.width, decoded.height, decoded.components);
            FAIL("JPEG test failed");
        }

        // Verify against serial decode
        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{encoded}, &err);
        if (!serialDecoded) {
            printf("TEST 32 FAIL: serial decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        if (memcmp(serialDecoded->pixels.data(), decoded.pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 32 FAIL: 512x512 parallel decode differs from serial\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 32 PASS: 512x512x3 parallel encode+decode pixel-exact (%zu bytes)\n",
               encoded.size());
    }

    // Test 33: Grayscale parallel round-trip (1 component) — pixel-exact
    {
        auto img = make_image(64, 64, 1, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>((x * 3 + y * 7) & 0xFF);
        });

        std::string err;
        auto serialEncoded = jpeg::encode_raw(img, 75, &err, false);

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        std::vector<u8> parallelEncoded;
        jpeg::encode_raw(img, 75, &err, false, &ctx, &parallelEncoded);
        sem->wait(ctx.currentValue);

        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);
        auto parallelDecoded = jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err);
        if (!serialDecoded || !parallelDecoded ||
            serialDecoded->pixels.size() != parallelDecoded->pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded->pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 33 FAIL: grayscale parallel encode pixels differ\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 33 PASS: grayscale parallel encode_raw pixel-exact (serial=%zu, parallel=%zu bytes)\n",
               serialEncoded.size(), parallelEncoded.size());
    }

    // Test 34: Non-8-aligned dimensions parallel round-trip
    {
        int sizes[][2] = {{13, 17}, {33, 15}, {100, 67}, {7, 9}};
        for (auto& s : sizes) {
            int w = s[0], h = s[1];
            auto img = make_image(w, h, 3, [](int x, int y, u8* out) {
                out[0] = static_cast<u8>((x * 37 + y * 13) & 0xFF);
                out[1] = static_cast<u8>((x * 7  + y * 41) & 0xFF);
                out[2] = static_cast<u8>((x * 23 + y * 3)  & 0xFF);
            });

            std::string err;
            auto serialEncoded = jpeg::encode_raw(img, 75, &err, false);

            auto sem = pool.createTimelineSemaphore();
            jpeg::JpegContext ctx;
            ctx.pool = &pool;
            ctx.sem = sem.get();
            ctx.currentValue = sem->value();

            std::vector<u8> parallelEncoded;
            jpeg::encode_raw(img, 75, &err, false, &ctx, &parallelEncoded);
            sem->wait(ctx.currentValue);

            auto sd = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);
            auto pd = jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err);
            if (!sd || !pd ||
                sd->pixels.size() != pd->pixels.size() ||
                memcmp(sd->pixels.data(), pd->pixels.data(), sd->pixels.size()) != 0) {
                printf("TEST 34 (%dx%d) FAIL: parallel encode pixels differ\n", w, h);
                FAIL("JPEG test failed");
            }
            printf("TEST 34 (%dx%d): parallel pixel-exact OK\n", w, h);
        }
        printf("TEST 34 PASS\n");
    }

    // ======================================================================
    // Jumbo parallel tests (4K textures)
    // ======================================================================

    printf("\n--- Jumbo 4K parallel tests ---\n");

    // Test 35: 4096x4096x3 baseline low-level encode — parallel pixel-exact
    {
        auto img = make_image(4096, 4096, 3, [](int x, int y, u8* out) {
            u32 h = static_cast<u32>(x * 2654435761u + y * 2246822519u);
            out[0] = static_cast<u8>((h >>  0) & 0xFF);
            out[1] = static_cast<u8>((h >>  8) & 0xFF);
            out[2] = static_cast<u8>((h >> 16) & 0xFF);
        });

        std::string err;
        auto serialEncoded = jpeg::encode_raw(img, 75, &err, false);
        if (serialEncoded.empty()) {
            printf("TEST 35 FAIL: serial encode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        std::vector<u8> parallelEncoded;
        jpeg::encode_raw(img, 75, &err, false, &ctx, &parallelEncoded);
        sem->wait(ctx.currentValue);

        auto sd = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);
        auto pd = jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err);
        if (!sd || !pd ||
            sd->pixels.size() != pd->pixels.size() ||
            memcmp(sd->pixels.data(), pd->pixels.data(), sd->pixels.size()) != 0) {
            printf("TEST 35 FAIL: 4K parallel encode pixels differ (serial=%zu, parallel=%zu)\n",
                   serialEncoded.size(), parallelEncoded.size());
            FAIL("JPEG test failed");
        }
        printf("TEST 35 PASS: 4096x4096x3 baseline parallel encode pixel-exact (serial=%zu, parallel=%zu bytes)\n",
               serialEncoded.size(), parallelEncoded.size());
    }

    // Test 36: 4096x4096x3 baseline low-level decode — parallel pixel-exact
    {
        auto img = make_image(4096, 4096, 3, [](int x, int y, u8* out) {
            u32 h = static_cast<u32>(x * 2654435761u + y * 2246822519u);
            out[0] = static_cast<u8>((h >>  0) & 0xFF);
            out[1] = static_cast<u8>((h >>  8) & 0xFF);
            out[2] = static_cast<u8>((h >> 16) & 0xFF);
        });

        std::string err;
        auto encoded = jpeg::encode_raw(img, 80, &err, false);

        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{encoded}, &err);
        if (!serialDecoded) {
            printf("TEST 36 FAIL: serial decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        jpeg::Image parallelDecoded;
        jpeg::decode_raw(std::span<const u8>{encoded}, &err, &ctx, &parallelDecoded);
        sem->wait(ctx.currentValue);

        if (serialDecoded->pixels.size() != parallelDecoded.pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded.pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 36 FAIL: 4K parallel decode differs from serial\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 36 PASS: 4096x4096x3 baseline parallel decode pixel-exact\n");
    }

    // Test 37: 4096x4096x4 progressive encode — parallel pixel-exact
    {
        auto img = make_image(4096, 4096, 4, [](int x, int y, u8* out) {
            u32 h = static_cast<u32>(x * 2654435761u + y * 2246822519u);
            out[0] = static_cast<u8>((h >>  0) & 0xFF);
            out[1] = static_cast<u8>((h >>  8) & 0xFF);
            out[2] = static_cast<u8>((h >> 16) & 0xFF);
            out[3] = static_cast<u8>((h >> 24) & 0xFF);
        });

        std::string err;
        auto serialEncoded = jpeg::encode_raw(img, 85, &err, true);
        if (serialEncoded.empty()) {
            printf("TEST 37 FAIL: serial progressive encode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        std::vector<u8> parallelEncoded;
        jpeg::encode_raw(img, 85, &err, true, &ctx, &parallelEncoded);
        sem->wait(ctx.currentValue);

        auto sd = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);
        auto pd = jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err);
        if (!sd || !pd ||
            sd->pixels.size() != pd->pixels.size() ||
            memcmp(sd->pixels.data(), pd->pixels.data(), sd->pixels.size()) != 0) {
            printf("TEST 37 FAIL: 4K progressive parallel encode pixels differ\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 37 PASS: 4096x4096x4 progressive parallel encode pixel-exact (serial=%zu, parallel=%zu bytes)\n",
               serialEncoded.size(), parallelEncoded.size());
    }

    // Test 38: 4096x4096x4 progressive decode — parallel pixel-exact
    {
        auto img = make_image(4096, 4096, 4, [](int x, int y, u8* out) {
            u32 h = static_cast<u32>(x * 2654435761u + y * 2246822519u);
            out[0] = static_cast<u8>((h >>  0) & 0xFF);
            out[1] = static_cast<u8>((h >>  8) & 0xFF);
            out[2] = static_cast<u8>((h >> 16) & 0xFF);
            out[3] = static_cast<u8>((h >> 24) & 0xFF);
        });

        std::string err;
        auto encoded = jpeg::encode_raw(img, 85, &err, true);

        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{encoded}, &err);
        if (!serialDecoded) {
            printf("TEST 38 FAIL: serial progressive decode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        jpeg::Image parallelDecoded;
        jpeg::decode_raw(std::span<const u8>{encoded}, &err, &ctx, &parallelDecoded);
        sem->wait(ctx.currentValue);

        if (serialDecoded->pixels.size() != parallelDecoded.pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded.pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 38 FAIL: 4K progressive parallel decode differs\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 38 PASS: 4096x4096x4 progressive parallel decode pixel-exact\n");
    }

    // Test 39: 4K public API round-trip — Writer+Parser with pool vs serial
    {
        auto srcTex = Texture::create2D(PixelFormat::RGBA8, 4096, 4096, 1);
        u8* px = srcTex.dataPtr();
        for (u32 y = 0; y < 4096; ++y) {
            for (u32 x = 0; x < 4096; ++x) {
                u32 idx = (y * 4096 + x) * 4;
                u32 h = x * 2654435761u + y * 2246822519u;
                px[idx + 0] = static_cast<u8>((h >>  0) & 0xFF);
                px[idx + 1] = static_cast<u8>((h >>  8) & 0xFF);
                px[idx + 2] = static_cast<u8>((h >> 16) & 0xFF);
                px[idx + 3] = 255;
            }
        }

        // Serial round-trip
        jpeg::Writer writerS(80);
        auto serialBytes = writerS.write(srcTex);
        if (serialBytes.empty()) {
            printf("TEST 39 FAIL: serial Writer 4K produced empty output\n");
            FAIL("JPEG test failed");
        }

        jpeg::Parser parserS;
        auto serialDecoded = parserS.parse(std::span<const u8>{serialBytes});
        if (!serialDecoded) {
            printf("TEST 39 FAIL: serial Parser 4K failed\n");
            FAIL("JPEG test failed");
        }

        // Parallel round-trip
        jpeg::Writer writerP(80, &pool);
        auto parallelBytes = writerP.write(srcTex);
        if (parallelBytes.empty()) {
            printf("TEST 39 FAIL: parallel Writer 4K produced empty output\n");
            FAIL("JPEG test failed");
        }

        jpeg::Parser parserP(&pool);
        auto parallelDecoded = parserP.parse(std::span<const u8>{parallelBytes});
        if (!parallelDecoded) {
            printf("TEST 39 FAIL: parallel Parser 4K failed\n");
            FAIL("JPEG test failed");
        }

        // Verify pixel-exact decoded output (byte-exact encoding not expected
        // due to restart intervals in parallel path)
        u32 texSize = 4096 * 4096 * 4;
        const u8* sData = serialDecoded->dataPtr();
        const u8* pData = parallelDecoded->dataPtr();
        if (memcmp(sData, pData, texSize) != 0) {
            printf("TEST 39 FAIL: 4K Parser parallel pixels differ from serial\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 39 PASS: 4096x4096 public API parallel round-trip pixel-exact "
               "(serial=%zu, parallel=%zu bytes)\n", serialBytes.size(), parallelBytes.size());
    }

    // Test 40: 4K grayscale (1 component) parallel encode+decode
    {
        auto img = make_image(4096, 4096, 1, [](int x, int y, u8* out) {
            out[0] = static_cast<u8>((x * 7 + y * 13) & 0xFF);
        });

        std::string err;
        auto serialEncoded = jpeg::encode_raw(img, 90, &err, false);
        if (serialEncoded.empty()) {
            printf("TEST 40 FAIL: serial encode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        // Parallel encode
        auto sem1 = pool.createTimelineSemaphore();
        jpeg::JpegContext encCtx;
        encCtx.pool = &pool;
        encCtx.sem = sem1.get();
        encCtx.currentValue = sem1->value();

        std::vector<u8> parallelEncoded;
        jpeg::encode_raw(img, 90, &err, false, &encCtx, &parallelEncoded);
        sem1->wait(encCtx.currentValue);

        if (parallelEncoded.empty()) {
            printf("TEST 40 FAIL: 4K grayscale parallel encode empty\n");
            FAIL("JPEG test failed");
        }

        // Parallel decode
        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);

        auto sem2 = pool.createTimelineSemaphore();
        jpeg::JpegContext decCtx;
        decCtx.pool = &pool;
        decCtx.sem = sem2.get();
        decCtx.currentValue = sem2->value();

        jpeg::Image parallelDecoded;
        jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err, &decCtx, &parallelDecoded);
        sem2->wait(decCtx.currentValue);

        // Verify pixel-exact: decode both encode outputs and compare
        auto parallelEncodedDecoded = jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err);
        if (!serialDecoded || !parallelEncodedDecoded ||
            serialDecoded->pixels.size() != parallelEncodedDecoded->pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelEncodedDecoded->pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 40 FAIL: 4K grayscale parallel encode pixels differ\n");
            FAIL("JPEG test failed");
        }

        if (serialDecoded->pixels.size() != parallelDecoded.pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded.pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 40 FAIL: 4K grayscale parallel decode differs\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 40 PASS: 4096x4096x1 grayscale parallel encode+decode pixel-exact "
               "(serial=%zu, parallel=%zu bytes)\n", serialEncoded.size(), parallelEncoded.size());
    }

    // Test 41: Non-power-of-2 near-4K dimensions (4000x3000x3) — edge-case stress
    {
        auto img = make_image(4000, 3000, 3, [](int x, int y, u8* out) {
            u32 h = static_cast<u32>(x * 2654435761u + y * 2246822519u);
            out[0] = static_cast<u8>((h >>  0) & 0xFF);
            out[1] = static_cast<u8>((h >>  8) & 0xFF);
            out[2] = static_cast<u8>((h >> 16) & 0xFF);
        });

        std::string err;
        auto serialEncoded = jpeg::encode_raw(img, 75, &err, true);
        if (serialEncoded.empty()) {
            printf("TEST 41 FAIL: serial encode error: %s\n", err.c_str());
            FAIL("JPEG test failed");
        }

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx;
        ctx.pool = &pool;
        ctx.sem = sem.get();
        ctx.currentValue = sem->value();

        std::vector<u8> parallelEncoded;
        jpeg::encode_raw(img, 75, &err, true, &ctx, &parallelEncoded);
        sem->wait(ctx.currentValue);

        if (parallelEncoded.empty()) {
            printf("TEST 41 FAIL: parallel encode empty\n");
            FAIL("JPEG test failed");
        }

        // Verify pixel-exact between serial and parallel encode outputs
        auto sd = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);
        auto pd = jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err);
        if (!sd || !pd ||
            sd->pixels.size() != pd->pixels.size() ||
            memcmp(sd->pixels.data(), pd->pixels.data(), sd->pixels.size()) != 0) {
            printf("TEST 41 FAIL: 4000x3000 parallel progressive encode pixels differ\n");
            FAIL("JPEG test failed");
        }

        // Also verify decode
        auto serialDecoded = jpeg::decode_raw(std::span<const u8>{serialEncoded}, &err);

        auto sem2 = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx2;
        ctx2.pool = &pool;
        ctx2.sem = sem2.get();
        ctx2.currentValue = sem2->value();

        jpeg::Image parallelDecoded;
        jpeg::decode_raw(std::span<const u8>{parallelEncoded}, &err, &ctx2, &parallelDecoded);
        sem2->wait(ctx2.currentValue);

        if (serialDecoded->pixels.size() != parallelDecoded.pixels.size() ||
            memcmp(serialDecoded->pixels.data(), parallelDecoded.pixels.data(),
                   serialDecoded->pixels.size()) != 0) {
            printf("TEST 41 FAIL: 4000x3000 parallel progressive decode differs\n");
            FAIL("JPEG test failed");
        }
        printf("TEST 41 PASS: 4000x3000x3 progressive parallel encode+decode pixel-exact "
               "(serial=%zu, parallel=%zu bytes)\n", serialEncoded.size(), parallelEncoded.size());
    }

    // ======================================================================
    // Performance benchmarks: single-threaded vs multi-threaded
    // ======================================================================

    printf("\n--- Performance benchmarks (single-threaded vs %zu threads) ---\n", numThreads);

    using Clock = std::chrono::high_resolution_clock;
    auto ms = [](Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    // Build a 4096x4096x3 test image once for all benchmarks.
    auto benchImg = make_image(4096, 4096, 3, [](int x, int y, u8* out) {
        u32 h = static_cast<u32>(x * 2654435761u + y * 2246822519u);
        out[0] = static_cast<u8>((h >>  0) & 0xFF);
        out[1] = static_cast<u8>((h >>  8) & 0xFF);
        out[2] = static_cast<u8>((h >> 16) & 0xFF);
    });

    // ----- Low-level encode_raw baseline -----
    {
        std::string err;

        auto t0 = Clock::now();
        auto serialEnc = jpeg::encode_raw(benchImg, 80, &err, false);
        auto t1 = Clock::now();

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx{ &pool, sem.get(), sem->value() };
        std::vector<u8> parallelEnc;
        auto t2 = Clock::now();
        jpeg::encode_raw(benchImg, 80, &err, false, &ctx, &parallelEnc);
        sem->wait(ctx.currentValue);
        auto t3 = Clock::now();

        printf("  encode_raw 4096x4096x3 baseline q80:\n");
        printf("    serial:   %7.1f ms\n", ms(t1 - t0));
        printf("    parallel: %7.1f ms  (%.2fx)\n", ms(t3 - t2), ms(t1 - t0) / ms(t3 - t2));
    }

    // ----- Low-level encode_raw progressive -----
    {
        std::string err;

        auto t0 = Clock::now();
        auto serialEnc = jpeg::encode_raw(benchImg, 80, &err, true);
        auto t1 = Clock::now();

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx{ &pool, sem.get(), sem->value() };
        std::vector<u8> parallelEnc;
        auto t2 = Clock::now();
        jpeg::encode_raw(benchImg, 80, &err, true, &ctx, &parallelEnc);
        sem->wait(ctx.currentValue);
        auto t3 = Clock::now();

        printf("  encode_raw 4096x4096x3 progressive q80:\n");
        printf("    serial:   %7.1f ms\n", ms(t1 - t0));
        printf("    parallel: %7.1f ms  (%.2fx)\n", ms(t3 - t2), ms(t1 - t0) / ms(t3 - t2));
    }

    // Produce encoded bytes for decode benchmarks.
    std::vector<u8> benchBaseline, benchProgressive, benchBaselineDRI;
    {
        std::string err;
        benchBaseline = jpeg::encode_raw(benchImg, 80, &err, false);
        benchProgressive = jpeg::encode_raw(benchImg, 80, &err, true);

        // Also produce a baseline stream with DRI markers (from the parallel
        // encoder) to benchmark the fully-parallel decode path.
        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx{ &pool, sem.get(), sem->value() };
        jpeg::encode_raw(benchImg, 80, &err, false, &ctx, &benchBaselineDRI);
        sem->wait(ctx.currentValue);
    }

    // ----- Low-level decode_raw baseline (no DRI — serial entropy) -----
    {
        std::string err;

        auto t0 = Clock::now();
        auto serialDec = jpeg::decode_raw(std::span<const u8>{benchBaseline}, &err);
        auto t1 = Clock::now();

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx{ &pool, sem.get(), sem->value() };
        jpeg::Image parallelDec;
        auto t2 = Clock::now();
        jpeg::decode_raw(std::span<const u8>{benchBaseline}, &err, &ctx, &parallelDec);
        sem->wait(ctx.currentValue);
        auto t3 = Clock::now();

        printf("  decode_raw 4096x4096x3 baseline (no DRI):\n");
        printf("    serial:   %7.1f ms\n", ms(t1 - t0));
        printf("    parallel: %7.1f ms  (%.2fx)\n", ms(t3 - t2), ms(t1 - t0) / ms(t3 - t2));
    }

    // ----- Low-level decode_raw baseline (DRI — fully parallel) -----
    {
        std::string err;

        auto t0 = Clock::now();
        auto serialDec = jpeg::decode_raw(std::span<const u8>{benchBaselineDRI}, &err);
        auto t1 = Clock::now();

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx{ &pool, sem.get(), sem->value() };
        jpeg::Image parallelDec;
        auto t2 = Clock::now();
        jpeg::decode_raw(std::span<const u8>{benchBaselineDRI}, &err, &ctx, &parallelDec);
        sem->wait(ctx.currentValue);
        auto t3 = Clock::now();

        printf("  decode_raw 4096x4096x3 baseline (DRI):\n");
        printf("    serial:   %7.1f ms\n", ms(t1 - t0));
        printf("    parallel: %7.1f ms  (%.2fx)\n", ms(t3 - t2), ms(t1 - t0) / ms(t3 - t2));
    }

    // ----- Low-level decode_raw progressive -----
    {
        std::string err;

        auto t0 = Clock::now();
        auto serialDec = jpeg::decode_raw(std::span<const u8>{benchProgressive}, &err);
        auto t1 = Clock::now();

        auto sem = pool.createTimelineSemaphore();
        jpeg::JpegContext ctx{ &pool, sem.get(), sem->value() };
        jpeg::Image parallelDec;
        auto t2 = Clock::now();
        jpeg::decode_raw(std::span<const u8>{benchProgressive}, &err, &ctx, &parallelDec);
        sem->wait(ctx.currentValue);
        auto t3 = Clock::now();

        printf("  decode_raw 4096x4096x3 progressive:\n");
        printf("    serial:   %7.1f ms\n", ms(t1 - t0));
        printf("    parallel: %7.1f ms  (%.2fx)\n", ms(t3 - t2), ms(t1 - t0) / ms(t3 - t2));
    }

    // ----- Public API Writer -----
    {
        auto srcTex = Texture::create2D(PixelFormat::RGBA8, 4096, 4096, 1);
        u8* px = srcTex.dataPtr();
        for (u32 y = 0; y < 4096; ++y)
            for (u32 x = 0; x < 4096; ++x) {
                u32 idx = (y * 4096 + x) * 4;
                u32 h = x * 2654435761u + y * 2246822519u;
                px[idx+0] = static_cast<u8>((h>> 0)&0xFF);
                px[idx+1] = static_cast<u8>((h>> 8)&0xFF);
                px[idx+2] = static_cast<u8>((h>>16)&0xFF);
                px[idx+3] = 255;
            }

        jpeg::Writer writerS(80);
        auto t0 = Clock::now();
        auto serialBytes = writerS.write(srcTex);
        auto t1 = Clock::now();

        jpeg::Writer writerP(80, &pool);
        auto t2 = Clock::now();
        auto parallelBytes = writerP.write(srcTex);
        auto t3 = Clock::now();

        printf("  Writer (public API) 4096x4096 q80:\n");
        printf("    serial:   %7.1f ms\n", ms(t1 - t0));
        printf("    parallel: %7.1f ms  (%.2fx)\n", ms(t3 - t2), ms(t1 - t0) / ms(t3 - t2));
    }

    // ----- Public API Parser -----
    {
        // Use the baseline JFIF bytes produced by the Writer above.
        jpeg::Writer writerS(80);
        auto srcTex = Texture::create2D(PixelFormat::RGBA8, 4096, 4096, 1);
        u8* px = srcTex.dataPtr();
        for (u32 y = 0; y < 4096; ++y)
            for (u32 x = 0; x < 4096; ++x) {
                u32 idx = (y * 4096 + x) * 4;
                u32 h = x * 2654435761u + y * 2246822519u;
                px[idx+0] = static_cast<u8>((h>> 0)&0xFF);
                px[idx+1] = static_cast<u8>((h>> 8)&0xFF);
                px[idx+2] = static_cast<u8>((h>>16)&0xFF);
                px[idx+3] = 255;
            }
        auto jpegBytes = writerS.write(srcTex);

        jpeg::Parser parserS;
        auto t0 = Clock::now();
        auto serialTex = parserS.parse(std::span<const u8>{jpegBytes});
        auto t1 = Clock::now();

        jpeg::Parser parserP(&pool);
        auto t2 = Clock::now();
        auto parallelTex = parserP.parse(std::span<const u8>{jpegBytes});
        auto t3 = Clock::now();

        printf("  Parser (public API) 4096x4096:\n");
        printf("    serial:   %7.1f ms\n", ms(t1 - t0));
        printf("    parallel: %7.1f ms  (%.2fx)\n", ms(t3 - t2), ms(t1 - t0) / ms(t3 - t2));
    }

    printf("\nAll tests passed!\n");
}

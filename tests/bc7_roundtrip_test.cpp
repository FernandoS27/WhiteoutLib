// BC7 encode/decode round-trip test — exercises all 8 modes at all quality levels.
// Encodes synthetic RGBA8 textures to BC7, decodes back, and checks PSNR.
#include <catch2/catch_all.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <whiteout/textures/texture.h>
#include "../src/whiteout/textures/bcn.h"
#include "../src/whiteout/textures/bcn/bc7.h"

using namespace whiteout;
using namespace whiteout::textures;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct TestResult {
    double psnr;
    double max_channel_err;
    bool ok;
};

static TestResult measure_quality(const Texture& src, const Texture& decoded) {
    auto s = src.mipData(0);
    auto d = decoded.mipData(0);

    u32 w = src.width();
    u32 h = src.height();
    double sse = 0;
    double max_err = 0;
    u32 n = w * h;

    for (u32 i = 0; i < n; ++i) {
        for (u32 ch = 0; ch < 4; ++ch) {
            double diff = static_cast<double>(s[i * 4 + ch]) - static_cast<double>(d[i * 4 + ch]);
            sse += diff * diff;
            if (std::abs(diff) > max_err)
                max_err = std::abs(diff);
        }
    }

    double mse = sse / (n * 4.0);
    double psnr = (mse > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
    return {psnr, max_err, true};
}

// ---------------------------------------------------------------------------
// Test cases — each creates a specific pixel pattern.
// ---------------------------------------------------------------------------

/// Solid colour block (should favour mode 6 — perfect encode).
static Texture make_solid(u32 w, u32 h) {
    Texture t = Texture::create2D(PixelFormat::RGBA8, w, h, 1);
    auto d = t.mipData(0);
    for (u32 i = 0; i < w * h; ++i) {
        d[i * 4 + 0] = 128;
        d[i * 4 + 1] = 64;
        d[i * 4 + 2] = 200;
        d[i * 4 + 3] = 255;
    }
    return t;
}

/// Smooth gradient — favours single-index modes (6, 5, 4).
static Texture make_gradient(u32 w, u32 h) {
    Texture t = Texture::create2D(PixelFormat::RGBA8, w, h, 1);
    auto d = t.mipData(0);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u32 i = (y * w + x) * 4;
            d[i + 0] = static_cast<u8>(x * 255 / (w > 1 ? w - 1 : 1));
            d[i + 1] = static_cast<u8>(y * 255 / (h > 1 ? h - 1 : 1));
            d[i + 2] = static_cast<u8>((x + y) * 127 / ((w > 1 ? w - 1 : 1) + (h > 1 ? h - 1 : 1)));
            d[i + 3] = static_cast<u8>(128 + x * 64 / (w > 1 ? w - 1 : 1));
        }
    }
    return t;
}

/// Two-region checkerboard (diagonal) — favours 2-subset modes (1, 3, 7).
static Texture make_checkerboard(u32 w, u32 h) {
    Texture t = Texture::create2D(PixelFormat::RGBA8, w, h, 1);
    auto d = t.mipData(0);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u32 i = (y * w + x) * 4;
            bool region = ((x / 2) + (y / 2)) & 1;
            if (region) {
                d[i + 0] = 200;
                d[i + 1] = 30;
                d[i + 2] = 30;
                d[i + 3] = 255;
            } else {
                d[i + 0] = 30;
                d[i + 1] = 200;
                d[i + 2] = 30;
                d[i + 3] = 255;
            }
        }
    }
    return t;
}

/// Varying alpha with flat colour — exercises alpha handling (modes 5, 4, 6, 7).
static Texture make_alpha_ramp(u32 w, u32 h) {
    Texture t = Texture::create2D(PixelFormat::RGBA8, w, h, 1);
    auto d = t.mipData(0);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u32 i = (y * w + x) * 4;
            d[i + 0] = 100;
            d[i + 1] = 150;
            d[i + 2] = 200;
            d[i + 3] = static_cast<u8>(x * 255 / (w > 1 ? w - 1 : 1));
        }
    }
    return t;
}

/// High-frequency noise — stresses all modes.
static Texture make_noise(u32 w, u32 h) {
    Texture t = Texture::create2D(PixelFormat::RGBA8, w, h, 1);
    auto d = t.mipData(0);
    u32 seed = 42;
    for (u32 i = 0; i < w * h * 4; ++i) {
        seed = seed * 1664525u + 1013904223u; // LCG
        d[i] = static_cast<u8>(seed >> 24);
    }
    return t;
}

/// Three-band horizontal stripes — favours 3-subset modes (0, 2).
static Texture make_three_bands(u32 w, u32 h) {
    Texture t = Texture::create2D(PixelFormat::RGBA8, w, h, 1);
    auto d = t.mipData(0);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u32 i = (y * w + x) * 4;
            u32 band = (y * 3) / h;
            switch (band) {
            case 0:
                d[i + 0] = 220;
                d[i + 1] = 20;
                d[i + 2] = 20;
                d[i + 3] = 255;
                break;
            case 1:
                d[i + 0] = 20;
                d[i + 1] = 220;
                d[i + 2] = 20;
                d[i + 3] = 255;
                break;
            default:
                d[i + 0] = 20;
                d[i + 1] = 20;
                d[i + 2] = 220;
                d[i + 3] = 255;
                break;
            }
        }
    }
    return t;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

TEST_CASE("BC7 roundtrip quality", "[bc7][roundtrip]") {
    struct TestCase {
        const char* name;
        Texture (*make)(u32, u32);
        u32 width;
        u32 height;
    };

    TestCase cases[] = {
        {"solid_4x4", make_solid, 4, 4},
        {"gradient_16x16", make_gradient, 16, 16},
        {"gradient_8x8", make_gradient, 8, 8},
        {"checkerboard_16x16", make_checkerboard, 16, 16},
        {"alpha_ramp_16x16", make_alpha_ramp, 16, 16},
        {"noise_16x16", make_noise, 16, 16},
        {"noise_32x32", make_noise, 32, 32},
        {"three_bands_16x16", make_three_bands, 16, 16},
        {"three_bands_4x12", make_three_bands, 4, 12},
        // Non-multiple-of-4 sizes (edge clamping)
        {"gradient_5x5", make_gradient, 5, 5},
        {"noise_7x3", make_noise, 7, 3},
    };

    const char* quality_names[] = {"Fast", "Normal", "High"};
    bc7::Quality qualities[] = {bc7::Quality::Fast, bc7::Quality::Normal, bc7::Quality::High};

    int total = 0, passed = 0, failed = 0;

    for (auto& tc : cases) {
        Texture src = tc.make(tc.width, tc.height);

        for (int qi = 0; qi < 3; ++qi) {
            total++;
            std::string err;

            // Encode
            auto enc = bc7::encodeTexture(src, qualities[qi], &err);
            if (!enc) {
                printf("FAIL  %-30s [%-6s] encode error: %s\n", tc.name, quality_names[qi],
                       err.c_str());
                failed++;
                continue;
            }

            // Verify compressed texture properties
            if (enc->format() != PixelFormat::BC7 || enc->width() != tc.width ||
                enc->height() != tc.height) {
                printf("FAIL  %-30s [%-6s] format/dimension mismatch\n", tc.name, quality_names[qi]);
                failed++;
                continue;
            }

            // Decode
            auto dec = bc7::decodeTexture(*enc, &err);
            if (!dec) {
                printf("FAIL  %-30s [%-6s] decode error: %s\n", tc.name, quality_names[qi],
                       err.c_str());
                failed++;
                continue;
            }

            if (dec->format() != PixelFormat::RGBA8 || dec->width() != tc.width ||
                dec->height() != tc.height) {
                printf("FAIL  %-30s [%-6s] decoded format/dimension mismatch\n", tc.name,
                       quality_names[qi]);
                failed++;
                continue;
            }

            // Measure quality
            auto result = measure_quality(src, *dec);

            // Thresholds:
            // - Solid blocks: near-lossless (quantization rounding only)
            // - Structured content (gradient, checker, bands, alpha ramp):
            //   basic PCA encoder should achieve ~15+ dB.
            // - Pure noise: worst case for any block compressor; just verify
            //   no catastrophic failure (PSNR > 8 dB, maxErr < 255).
            double min_psnr = 8.0;
            double max_allowed_err = 254.0;

            if (std::strcmp(tc.name, "solid_4x4") == 0) {
                // Solid block: endpoints match, only P-bit rounding.
                min_psnr = 45.0;
                max_allowed_err = 2.0;
            } else if (std::strstr(tc.name, "noise") == nullptr) {
                // Non-noise structured content
                min_psnr = 12.0;
                max_allowed_err = 230.0;
            }

            bool pass = result.psnr >= min_psnr && result.max_channel_err <= max_allowed_err;
            if (pass) {
                printf("OK    %-30s [%-6s] PSNR=%6.2f dB  maxErr=%3.0f\n", tc.name,
                       quality_names[qi], result.psnr, result.max_channel_err);
                passed++;
            } else {
                printf("FAIL  %-30s [%-6s] PSNR=%6.2f dB  maxErr=%3.0f  (min PSNR=%.0f, "
                       "max err=%.0f)\n",
                       tc.name, quality_names[qi], result.psnr, result.max_channel_err, min_psnr,
                       max_allowed_err);
                failed++;
            }
        }
    }

    printf("\n=== BC7 Roundtrip: %d/%d passed, %d failed ===\n", passed, total, failed);
    CHECK(failed == 0);
}

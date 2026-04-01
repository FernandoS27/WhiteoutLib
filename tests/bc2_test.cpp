// Quick BC2 encode/decode round-trip test
#include <catch2/catch_all.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <whiteout/textures/texture.h>
#include <whiteout/textures/dds/dds.h>
#include <whiteout/textures/blp/blp.h>
#include "../src/whiteout/textures/bcn.h"
#include "../src/whiteout/textures/bcn/bc2.h"

using namespace whiteout;
using namespace whiteout::textures;

// ---- BLP file helpers (class-based API requires manual file I/O) ----

static std::optional<Texture> load_blp_file(const std::string& path, std::string* error) {
    blp::Parser parser;
    auto result = parser.parse(path);
    if (!result && error) {
        if (parser.hasIssues()) {
            *error = parser.getIssues().front();
        } else {
            *error = "Unknown BLP parse error";
        }
    }
    return result;
}

static bool save_blp_file(const Texture& tex, const std::string& path,
                          const blp::SaveOptions& opts, std::string* error) {
    blp::Writer writer;
    writer.write(path, tex, opts);
    if (writer.hasIssues()) {
        if (error) *error = writer.getIssues().front();
        return false;
    }
    return true;
}

static std::vector<u8> save_blp_bytes(const Texture& tex, const blp::SaveOptions& opts,
                                      std::string* error) {
    blp::Writer writer;
    auto result = writer.write(tex, opts);
    if (writer.hasIssues()) {
        if (error) *error = writer.getIssues().front();
    }
    return result;
}

// ---- DDS file helpers (class-based API requires manual file I/O) ----

static std::optional<Texture> load_dds_file(const std::string& path, std::string* error) {
    dds::Parser parser;
    auto result = parser.parse(path);
    if (parser.hasIssues()) {
        if (error) *error = parser.getIssues().front();
        return std::nullopt;
    }
    return result;
}

static bool save_dds_file(const Texture& tex, const std::string& path, std::string* error) {
    dds::Writer writer;
    writer.write(path, tex);
    if (writer.hasIssues()) {
        if (error) *error = writer.getIssues().front();
        return false;
    }
    return true;
}

bool test_bc2_roundtrip(u32 width, u32 height, const char* name) {
    // Create an RGBA8 texture with varying content
    Texture src = Texture::create2D(PixelFormat::RGBA8, width, height, 1);
    auto data = src.mipData(0);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u32 i = (y * width + x) * 4;
            data[i + 0] = static_cast<u8>((x * 37 + y * 13) & 0xFF); // R
            data[i + 1] = static_cast<u8>((x * 7 + y * 41) & 0xFF);  // G
            data[i + 2] = static_cast<u8>((x * 23 + y * 3) & 0xFF);  // B
            data[i + 3] = static_cast<u8>((x * 11 + y * 29) & 0xFF); // A (varying!)
        }
    }

    // Encode RGBA8 -> BC2
    std::string err;
    auto bc2_opt = textures::bcn::encode(src, PixelFormat::BC2, &err);
    if (!bc2_opt) {
        printf("%s: encode failed: %s\n", name, err.c_str());
        return false;
    }

    // Decode BC2 -> RGBA8
    auto decoded_opt = textures::bcn::decode(*bc2_opt, &err);
    if (!decoded_opt) {
        printf("%s: decode failed: %s\n", name, err.c_str());
        return false;
    }

    // Check dimensions
    if (decoded_opt->width() != width || decoded_opt->height() != height) {
        printf("%s: dimension mismatch: got %ux%u, expected %ux%u\n",
               name, decoded_opt->width(), decoded_opt->height(), width, height);
        return false;
    }

    // Compare - BC2 is lossy, so use a tolerance
    auto src_data = src.mipData(0);
    auto dst_data = decoded_opt->mipData(0);
    double max_err_r = 0, max_err_g = 0, max_err_b = 0, max_err_a = 0;
    double sum_err = 0;
    u32 pixel_count = width * height;

    for (u32 i = 0; i < pixel_count; ++i) {
        double dr = std::abs((double)src_data[i*4+0] - (double)dst_data[i*4+0]);
        double dg = std::abs((double)src_data[i*4+1] - (double)dst_data[i*4+1]);
        double db = std::abs((double)src_data[i*4+2] - (double)dst_data[i*4+2]);
        double da = std::abs((double)src_data[i*4+3] - (double)dst_data[i*4+3]);
        max_err_r = std::max(max_err_r, dr);
        max_err_g = std::max(max_err_g, dg);
        max_err_b = std::max(max_err_b, db);
        max_err_a = std::max(max_err_a, da);
        sum_err += dr + dg + db + da;
    }

    double avg_err = sum_err / (pixel_count * 4);

    printf("%s: max_err RGBA=(%.0f, %.0f, %.0f, %.0f) avg=%.2f",
           name, max_err_r, max_err_g, max_err_b, max_err_a, avg_err);

    // BC2 colour error should be reasonable (< 30 per channel typical)
    // Alpha error should be small (4-bit quantization: max ~8)
    bool ok = max_err_a <= 17.0 && avg_err < 50.0;
    printf(" %s\n", ok ? "OK" : "FAIL");

    if (max_err_a > 17.0) {
        // Dump first few pixels with large alpha error
        printf("  Alpha errors > 17:\n");
        int shown = 0;
        for (u32 i = 0; i < pixel_count && shown < 10; ++i) {
            double da = std::abs((double)src_data[i*4+3] - (double)dst_data[i*4+3]);
            if (da > 17.0) {
                printf("    pixel %u: src_a=%u dst_a=%u diff=%.0f\n",
                       i, src_data[i*4+3], dst_data[i*4+3], da);
                shown++;
            }
        }
    }

    return ok;
}

bool test_bc2_solid_alpha() {
    // Test with solid alpha = 255 (common case)
    Texture src = Texture::create2D(PixelFormat::RGBA8, 8, 8, 1);
    auto data = src.mipData(0);
    for (u32 i = 0; i < 8 * 8; ++i) {
        data[i*4+0] = 100;
        data[i*4+1] = 150;
        data[i*4+2] = 200;
        data[i*4+3] = 255;
    }

    std::string err;
    auto bc2 = textures::bcn::encode(src, PixelFormat::BC2, &err);
    if (!bc2) {
        printf("solid_alpha: encode failed: %s\n", err.c_str());
        return false;
    }
    auto decoded = textures::bcn::decode(*bc2, &err);
    if (!decoded) {
        printf("solid_alpha: decode failed: %s\n", err.c_str());
        return false;
    }

    auto dst = decoded->mipData(0);
    bool ok = true;
    for (u32 i = 0; i < 8 * 8; ++i) {
        if (dst[i*4+3] != 255) {
            printf("solid_alpha: pixel %u alpha=%u (expected 255)\n", i, dst[i*4+3]);
            ok = false;
            break;
        }
    }
    printf("solid_alpha: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool test_bc2_block_level() {
    // Test a single 4x4 block encode/decode at the raw level
    // Create known RGBA data
    u8 rgba[64]; // 16 texels * 4 bytes
    for (int i = 0; i < 16; ++i) {
        rgba[i*4+0] = static_cast<u8>(i * 16);   // R
        rgba[i*4+1] = static_cast<u8>(255 - i*16); // G
        rgba[i*4+2] = 128;                         // B
        rgba[i*4+3] = static_cast<u8>(i * 17);    // A: 0,17,34,...,255
    }

    // Encode to BC2 via texture API
    Texture src = Texture::create2D(PixelFormat::RGBA8, 4, 4, 1);
    std::memcpy(src.mipData(0).data(), rgba, 64);

    std::string err;
    auto bc2 = textures::bcn::encode(src, PixelFormat::BC2, &err);
    if (!bc2) {
        printf("block_level: encode failed: %s\n", err.c_str());
        return false;
    }

    // Print the raw BC2 block (16 bytes)
    auto bc2_data = bc2->mipData(0);
    printf("block_level: BC2 block = ");
    for (size_t i = 0; i < bc2_data.size(); ++i) {
        printf("%02X ", bc2_data[i]);
    }
    printf("\n");

    // Alpha bytes (first 8 bytes): each byte has 2 nibbles for 2 texels
    printf("block_level: Alpha nibbles = ");
    for (int i = 0; i < 8; ++i) {
        u8 lo = bc2_data[i] & 0x0F;
        u8 hi = (bc2_data[i] >> 4) & 0x0F;
        printf("[%u,%u] ", lo, hi);
    }
    printf("\n");

    // Expected alpha values (i*17 quantized to 4 bits):
    // i*17: 0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255
    // quantize: (val * 15 + 127) / 255
    printf("block_level: Expected alpha nibbles = ");
    for (int i = 0; i < 16; i += 2) {
        u8 a0 = static_cast<u8>((rgba[i*4+3] * 15 + 127) / 255);
        u8 a1 = static_cast<u8>((rgba[(i+1)*4+3] * 15 + 127) / 255);
        printf("[%u,%u] ", a0, a1);
    }
    printf("\n");

    // Decode back
    auto decoded = textures::bcn::decode(*bc2, &err);
    if (!decoded) {
        printf("block_level: decode failed: %s\n", err.c_str());
        return false;
    }

    auto out = decoded->mipData(0);
    printf("block_level: Decoded alpha = ");
    for (int i = 0; i < 16; ++i) {
        printf("%u ", out[i*4+3]);
    }
    printf("\n");

    printf("block_level: Source alpha   = ");
    for (int i = 0; i < 16; ++i) {
        printf("%u ", rgba[i*4+3]);
    }
    printf("\n");

    // Check alpha round-trip error
    double max_alpha_err = 0;
    for (int i = 0; i < 16; ++i) {
        double da = std::abs((double)rgba[i*4+3] - (double)out[i*4+3]);
        max_alpha_err = std::max(max_alpha_err, da);
    }
    bool ok = max_alpha_err <= 17.0;
    printf("block_level: max_alpha_err=%.0f %s\n", max_alpha_err, ok ? "OK" : "FAIL");
    return ok;
}

bool test_bc2_dds_roundtrip() {
    // Create a known RGBA8 texture
    Texture src = Texture::create2D(PixelFormat::RGBA8, 16, 16, 1);
    auto data = src.mipData(0);
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            u32 i = (y * 16 + x) * 4;
            data[i + 0] = static_cast<u8>(x * 16);
            data[i + 1] = static_cast<u8>(y * 16);
            data[i + 2] = 128;
            data[i + 3] = static_cast<u8>((x + y) * 8);
        }
    }

    // Encode RGBA8 -> BC2
    std::string err;
    auto bc2 = textures::bcn::encode(src, PixelFormat::BC2, &err);
    if (!bc2) {
        printf("dds_roundtrip: encode failed: %s\n", err.c_str());
        return false;
    }

    // Save as DDS
    bool save_ok = save_dds_file(*bc2, "test_bc2.dds", &err);
    if (!save_ok) {
        printf("dds_roundtrip: DDS save failed: %s\n", err.c_str());
        return false;
    }

    // Load the DDS back
    auto loaded = load_dds_file("test_bc2.dds", &err);
    if (!loaded) {
        printf("dds_roundtrip: DDS load failed: %s\n", err.c_str());
        return false;
    }

    // Check format
    if (loaded->format() != PixelFormat::BC2) {
        printf("dds_roundtrip: loaded format is not BC2!\n");
        return false;
    }

    // Compare raw BC2 data
    auto bc2_data = bc2->mipData(0);
    auto loaded_data = loaded->mipData(0);
    if (bc2_data.size() != loaded_data.size()) {
        printf("dds_roundtrip: size mismatch: %zu vs %zu\n", bc2_data.size(), loaded_data.size());
        return false;
    }

    bool data_match = std::memcmp(bc2_data.data(), loaded_data.data(), bc2_data.size()) == 0;
    printf("dds_roundtrip: raw data match=%s size=%zu\n", data_match ? "true" : "false", bc2_data.size());

    // Decode both and compare
    auto dec1 = textures::bcn::decode(*bc2, &err);
    auto dec2 = textures::bcn::decode(*loaded, &err);
    if (!dec1 || !dec2) {
        printf("dds_roundtrip: decode failed\n");
        return false;
    }

    auto d1 = dec1->mipData(0);
    auto d2 = dec2->mipData(0);
    bool decode_match = std::memcmp(d1.data(), d2.data(), d1.size()) == 0;
    printf("dds_roundtrip: decoded pixel match=%s\n", decode_match ? "true" : "false");

    printf("dds_roundtrip: %s\n", (data_match && decode_match) ? "OK" : "FAIL");
    return data_match && decode_match;
}

bool test_bc2_blp_roundtrip() {
    // Create a known RGBA8 texture
    Texture src = Texture::create2D(PixelFormat::RGBA8, 16, 16, 1);
    auto data = src.mipData(0);
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            u32 i = (y * 16 + x) * 4;
            data[i + 0] = static_cast<u8>(x * 16);
            data[i + 1] = static_cast<u8>(y * 16);
            data[i + 2] = 128;
            data[i + 3] = static_cast<u8>((x + y) * 8);
        }
    }

    // Encode to BC2
    std::string err;
    auto bc2 = textures::bcn::encode(src, PixelFormat::BC2, &err);
    if (!bc2) {
        printf("blp_roundtrip: encode failed: %s\n", err.c_str());
        return false;
    }

    // Save as BLP
    blp::SaveOptions opts;
    opts.version = blp::BlpVersion::BLP2;
    bool save_ok = save_blp_file(*bc2, "test_bc2.blp", opts, &err);
    if (!save_ok) {
        printf("blp_roundtrip: BLP save failed: %s\n", err.c_str());
        return false;
    }

    // Load back
    auto loaded = load_blp_file("test_bc2.blp", &err);
    if (!loaded) {
        printf("blp_roundtrip: BLP load failed: %s\n", err.c_str());
        return false;
    }

    if (loaded->format() != PixelFormat::BC2) {
        printf("blp_roundtrip: loaded format is not BC2, got format id=%d\n", (int)loaded->format());
        return false;
    }

    auto bc2_data = bc2->mipData(0);
    auto loaded_data = loaded->mipData(0);
    bool data_match = (bc2_data.size() == loaded_data.size()) &&
                      (std::memcmp(bc2_data.data(), loaded_data.data(), bc2_data.size()) == 0);
    printf("blp_roundtrip: raw data match=%s encoded_size=%zu loaded_size=%zu\n",
           data_match ? "true" : "false", bc2_data.size(), loaded_data.size());

    printf("blp_roundtrip: %s\n", data_match ? "OK" : "FAIL");
    return data_match;
}

bool test_bc2_copy_as_format() {
    // Test the copyAsFormat path: RGBA8 -> BC2 -> RGBA8
    Texture src = Texture::create2D(PixelFormat::RGBA8, 16, 16, 1);
    auto data = src.mipData(0);
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            u32 i = (y * 16 + x) * 4;
            data[i + 0] = static_cast<u8>(x * 16);
            data[i + 1] = static_cast<u8>(y * 16);
            data[i + 2] = 128;
            data[i + 3] = 200;
        }
    }

    try {
        Texture bc2 = src.copyAsFormat(PixelFormat::BC2);
        if (bc2.format() != PixelFormat::BC2) {
            printf("copyAsFormat: wrong format after encode\n");
            return false;
        }

        Texture decoded = bc2.copyAsFormat(PixelFormat::RGBA8);
        if (decoded.format() != PixelFormat::RGBA8) {
            printf("copyAsFormat: wrong format after decode\n");
            return false;
        }

        // Check alpha is correct (all pixels should be 200 or close)
        auto out = decoded.mipData(0);
        double max_alpha_err = 0;
        for (u32 i = 0; i < 16 * 16; ++i) {
            double da = std::abs((double)200 - (double)out[i * 4 + 3]);
            max_alpha_err = std::max(max_alpha_err, da);
        }
        printf("copyAsFormat: max_alpha_err=%.0f\n", max_alpha_err);
        printf("copyAsFormat: %s\n", max_alpha_err <= 17.0 ? "OK" : "FAIL");
        return max_alpha_err <= 17.0;
    } catch (const std::exception& e) {
        printf("copyAsFormat: exception: %s\n", e.what());
        return false;
    }
}

bool test_bc2_blp_from_rgba8() {
    // This tests the most common real-world path:
    // User has an RGBA8 texture, encodes it to BC2, saves as BLP
    // Then loads the BLP back and decodes to RGBA8
    Texture src = Texture::create2D(PixelFormat::RGBA8, 64, 64, 1);
    auto data = src.mipData(0);
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            u32 i = (y * 64 + x) * 4;
            data[i + 0] = static_cast<u8>(x * 4);
            data[i + 1] = static_cast<u8>(y * 4);
            data[i + 2] = 128;
            data[i + 3] = static_cast<u8>((x + y) * 2);
        }
    }

    std::string err;

    // Step 1: Convert RGBA8 -> BC2
    Texture bc2 = src.copyAsFormat(PixelFormat::BC2);
    printf("blp_from_rgba8: bc2 format=%d size=%zu\n", (int)bc2.format(), bc2.mipData(0).size());

    // Step 2: Save as BLP with DXT encoding
    blp::SaveOptions opts;
    opts.version = blp::BlpVersion::BLP2;
    opts.encoding = blp::BlpEncoding::DXT;
    bool save_ok = save_blp_file(bc2, "test_bc2_from_rgba.blp", opts, &err);
    if (!save_ok) {
        printf("blp_from_rgba8: BLP save failed: %s\n", err.c_str());
        return false;
    }

    // Step 3: Load back as BC2
    auto loaded = load_blp_file("test_bc2_from_rgba.blp", &err);
    if (!loaded) {
        printf("blp_from_rgba8: BLP load failed: %s\n", err.c_str());
        return false;
    }
    printf("blp_from_rgba8: loaded format=%d\n", (int)loaded->format());

    if (loaded->format() != PixelFormat::BC2) {
        printf("blp_from_rgba8: FAIL: loaded as %d, expected BC2\n", (int)loaded->format());
        return false;
    }

    // Step 4: Decode to RGBA8
    Texture decoded = loaded->copyAsFormat(PixelFormat::RGBA8);

    // Step 5: Also decode the original BC2 (before BLP save) to get reference
    Texture ref = bc2.copyAsFormat(PixelFormat::RGBA8);

    auto ref_data = ref.mipData(0);
    auto dec_data = decoded.mipData(0);

    // The decoded-from-BLP should match the decoded-from-original-BC2 exactly
    bool exact_match = std::memcmp(ref_data.data(), dec_data.data(), ref_data.size()) == 0;
    printf("blp_from_rgba8: exact match with pre-BLP decode = %s\n", exact_match ? "true" : "false");

    // Also compare decoded vs original source
    auto src_data = src.mipData(0);
    double max_err = 0;
    for (u32 i = 0; i < 64 * 64 * 4; ++i) {
        double d = std::abs((double)src_data[i] - (double)dec_data[i]);
        max_err = std::max(max_err, d);
    }
    printf("blp_from_rgba8: max error vs source = %.0f\n", max_err);
    printf("blp_from_rgba8: %s\n", exact_match ? "OK" : "FAIL");
    return exact_match;
}

bool test_bc2_blp_infer_encoding() {
    // Test what happens when using BlpEncoding::Infer with a BC2 texture
    Texture src = Texture::create2D(PixelFormat::RGBA8, 16, 16, 1);
    auto data = src.mipData(0);
    for (u32 i = 0; i < 16 * 16 * 4; i += 4) {
        data[i + 0] = 100;
        data[i + 1] = 150;
        data[i + 2] = 200;
        data[i + 3] = 180;
    }

    std::string err;
    Texture bc2 = src.copyAsFormat(PixelFormat::BC2);

    // Save with Infer encoding (should auto-detect BC2 -> DXT)
    blp::SaveOptions opts;
    opts.version = blp::BlpVersion::BLP2;
    // encoding defaults to Infer
    bool save_ok = save_blp_file(bc2, "test_bc2_infer.blp", opts, &err);
    if (!save_ok) {
        printf("blp_infer: BLP save failed: %s\n", err.c_str());
        return false;
    }

    auto loaded = load_blp_file("test_bc2_infer.blp", &err);
    if (!loaded) {
        printf("blp_infer: BLP load failed: %s\n", err.c_str());
        return false;
    }

    if (loaded->format() != PixelFormat::BC2) {
        printf("blp_infer: FAIL: loaded as format %d, expected BC2\n", (int)loaded->format());
        return false;
    }

    auto orig_data = bc2.mipData(0);
    auto load_data = loaded->mipData(0);
    bool match = (orig_data.size() == load_data.size()) &&
                 std::memcmp(orig_data.data(), load_data.data(), orig_data.size()) == 0;
    printf("blp_infer: raw data match = %s\n", match ? "true" : "false");
    printf("blp_infer: %s\n", match ? "OK" : "FAIL");
    return match;
}

bool test_bc2_blp_header() {
    // Verify the raw BLP2 header is correct for BC2 data
    Texture src = Texture::create2D(PixelFormat::RGBA8, 16, 16, 1);
    auto data = src.mipData(0);
    for (u32 i = 0; i < 16 * 16 * 4; i += 4) {
        data[i] = 100; data[i+1] = 150; data[i+2] = 200; data[i+3] = 180;
    }

    std::string err;
    Texture bc2 = src.copyAsFormat(PixelFormat::BC2);

    // Save as BLP2 with DXT encoding
    auto blp_bytes = save_blp_bytes(bc2, blp::SaveOptions{blp::BlpVersion::BLP2, blp::BlpEncoding::DXT}, &err);
    if (blp_bytes.empty()) {
        printf("blp_header: save failed: %s\n", err.c_str());
        return false;
    }

    // Dump the BLP2 header (first ~20 bytes)
    printf("blp_header: raw header bytes = ");
    for (size_t i = 0; i < std::min<size_t>(20, blp_bytes.size()); ++i) {
        printf("%02X ", blp_bytes[i]);
    }
    printf("\n");

    // BLP2 header structure:
    // offset 0: u32 magic = 0x32504C42 ("BLP2" LE)
    // offset 4: u32 version = 1
    // offset 8: u8 colorEncoding (should be 2 for DXT)
    // offset 9: u8 alphaBitDepth (should be 4 for BC2?)
    // offset 10: u8 alphaType (should be 1 for BC2)
    // offset 11: u8 hasMipmaps
    // offset 12: u32 width
    // offset 16: u32 height

    u32 magic = *reinterpret_cast<u32*>(blp_bytes.data());
    u32 version = *reinterpret_cast<u32*>(blp_bytes.data() + 4);
    u8 colorEncoding = blp_bytes[8];
    u8 alphaBitDepth = blp_bytes[9];
    u8 alphaType = blp_bytes[10];
    u8 hasMipmaps = blp_bytes[11];
    u32 width = *reinterpret_cast<u32*>(blp_bytes.data() + 12);
    u32 height = *reinterpret_cast<u32*>(blp_bytes.data() + 16);

    printf("blp_header: magic=%08X version=%u colorEncoding=%u alphaBitDepth=%u alphaType=%u hasMips=%u %ux%u\n",
           magic, version, colorEncoding, alphaBitDepth, alphaType, hasMipmaps, width, height);

    bool ok = (magic == 0x32504C42) && (version == 1) && (colorEncoding == 2) &&
              (alphaType == 1) && (width == 16) && (height == 16);
    printf("blp_header: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool test_bc2_blp_mipmaps() {
    // Real BLP files have multiple mip levels. Test BC2 with mipmaps.
    Texture src = Texture::create2D(PixelFormat::RGBA8, 64, 64, 0); // 0 = auto mip count
    printf("blp_mipmaps: src mipCount=%u\n", src.mipCount());
    auto data = src.mipData(0);
    for (u32 i = 0; i < 64 * 64 * 4; i += 4) {
        data[i + 0] = static_cast<u8>(i & 0xFF);
        data[i + 1] = static_cast<u8>((i >> 2) & 0xFF);
        data[i + 2] = 128;
        data[i + 3] = static_cast<u8>((i >> 4) & 0xFF);
    }

    // Fill other mip levels with known data
    for (u32 mip = 1; mip < src.mipCount(); ++mip) {
        auto md = src.mipData(mip);
        for (size_t i = 0; i < md.size(); i += 4) {
            md[i + 0] = 100;
            md[i + 1] = 150;
            md[i + 2] = 200;
            md[i + 3] = 128;
        }
    }

    std::string err;
    auto bc2_opt = textures::bcn::encode(src, PixelFormat::BC2, &err);
    if (!bc2_opt) {
        printf("blp_mipmaps: encode failed: %s\n", err.c_str());
        return false;
    }
    printf("blp_mipmaps: bc2 mipCount=%u\n", bc2_opt->mipCount());

    // Print mip sizes
    for (u32 m = 0; m < bc2_opt->mipCount(); ++m) {
        auto md = bc2_opt->mipData(m);
        auto ml = bc2_opt->mipLevel(m);
        printf("  mip %u: %ux%u size=%zu\n", m, ml.width, ml.height, md.size());
    }

    // Save as BLP
    blp::SaveOptions opts;
    opts.version = blp::BlpVersion::BLP2;
    opts.encoding = blp::BlpEncoding::DXT;
    bool saved = save_blp_file(*bc2_opt, "test_bc2_mipmaps.blp", opts, &err);
    if (!saved) {
        printf("blp_mipmaps: BLP save failed: %s\n", err.c_str());
        return false;
    }

    // Load back
    auto loaded = load_blp_file("test_bc2_mipmaps.blp", &err);
    if (!loaded) {
        printf("blp_mipmaps: BLP load failed: %s\n", err.c_str());
        return false;
    }
    printf("blp_mipmaps: loaded mipCount=%u format=%d\n", loaded->mipCount(), (int)loaded->format());

    // Compare all mip levels
    bool all_match = true;
    for (u32 m = 0; m < std::min(bc2_opt->mipCount(), loaded->mipCount()); ++m) {
        auto orig = bc2_opt->mipData(m);
        auto rld = loaded->mipData(m);
        bool match = (orig.size() == rld.size()) &&
                     std::memcmp(orig.data(), rld.data(), orig.size()) == 0;
        if (!match) {
            printf("  mip %u: MISMATCH orig_size=%zu loaded_size=%zu\n", m, orig.size(), rld.size());
            all_match = false;
        }
    }
    printf("blp_mipmaps: %s\n", all_match ? "OK" : "FAIL");
    return all_match;
}


// ============================================================================
// Catch2 test cases
// ============================================================================

TEST_CASE("BC2 solid alpha", "[bc2]") { REQUIRE(test_bc2_solid_alpha()); }
TEST_CASE("BC2 block level", "[bc2]") { REQUIRE(test_bc2_block_level()); }
TEST_CASE("BC2 roundtrip 4x4", "[bc2]") { REQUIRE(test_bc2_roundtrip(4, 4, "4x4")); }
TEST_CASE("BC2 roundtrip 8x8", "[bc2]") { REQUIRE(test_bc2_roundtrip(8, 8, "8x8")); }
TEST_CASE("BC2 roundtrip 16x16", "[bc2]") { REQUIRE(test_bc2_roundtrip(16, 16, "16x16")); }
TEST_CASE("BC2 roundtrip 64x64", "[bc2]") { REQUIRE(test_bc2_roundtrip(64, 64, "64x64")); }
TEST_CASE("BC2 roundtrip 100x100", "[bc2]") { REQUIRE(test_bc2_roundtrip(100, 100, "100x100")); }
TEST_CASE("BC2 DDS roundtrip", "[bc2]") { REQUIRE(test_bc2_dds_roundtrip()); }
TEST_CASE("BC2 BLP roundtrip", "[bc2]") { REQUIRE(test_bc2_blp_roundtrip()); }
TEST_CASE("BC2 copy as format", "[bc2]") { REQUIRE(test_bc2_copy_as_format()); }
TEST_CASE("BC2 BLP from RGBA8", "[bc2]") { REQUIRE(test_bc2_blp_from_rgba8()); }
TEST_CASE("BC2 BLP infer encoding", "[bc2]") { REQUIRE(test_bc2_blp_infer_encoding()); }
TEST_CASE("BC2 BLP header", "[bc2]") { REQUIRE(test_bc2_blp_header()); }
TEST_CASE("BC2 BLP mipmaps", "[bc2]") { REQUIRE(test_bc2_blp_mipmaps()); }

TEST_CASE("BC2 real DDS file", "[bc2][corpus]") {
    std::string err;
    auto tex = load_dds_file("../Models/TexSimple/chickenFeather.dds", &err);
    if (!tex) SKIP("chickenFeather.dds not found");

    Texture rgba = tex->copyAsFormat(PixelFormat::RGBA8);
    Texture bc2 = rgba.copyAsFormat(PixelFormat::BC2);
    REQUIRE(bc2.mipData(0).size() > 0);

    blp::SaveOptions blp_opts;
    blp_opts.version = blp::BlpVersion::BLP2;
    blp_opts.encoding = blp::BlpEncoding::DXT;
    REQUIRE(save_blp_file(bc2, "test_real_bc2.blp", blp_opts, &err));

    auto loaded = load_blp_file("test_real_bc2.blp", &err);
    REQUIRE(loaded.has_value());

    auto orig = bc2.mipData(0);
    auto rld = loaded->mipData(0);
    CHECK(orig.size() == rld.size());
    CHECK(std::memcmp(orig.data(), rld.data(), orig.size()) == 0);
}

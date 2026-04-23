// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Tests for unicode (UTF-8) path support across all file I/O helpers and
// format-level parser/writer round-trips.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/parser.h>
#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/mdx/writer.h>
#include <whiteout/textures/bmp/parser.h>
#include <whiteout/textures/bmp/writer.h>
#include <whiteout/textures/texture.h>

// Internal helpers accessed directly (same pattern as jpeg_test.cpp)
#include "../src/whiteout/common/unicode_path.h"
#include "../src/whiteout/textures/io_helpers.h"
#include "../src/whiteout/textures/issue_sink.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::textures;

// ============================================================================
// Helpers
// ============================================================================

// UTF-8 encoding of "测试" (two CJK characters, 6 bytes).
static const char kChinese[] = "\xE6\xB5\x8B\xE8\xAF\x95";

// Build a UTF-8 string path.  Uses u8string() so the temp directory path is
// itself treated as UTF-8 on all platforms.
static std::string makeTempDir(const char* suffix) {
    auto base = fs::temp_directory_path();
    auto u8 = base.u8string();
    // u8string() returns std::u8string in C++20 / std::string in C++17.
    // Either way, iterating char8_t is safe to copy into std::string chars.
    std::string result(u8.begin(), u8.end());
    result += "/";
    result += suffix;
    return result;
}

// Create a directory from a UTF-8 path string.
static void createDirs(const std::string& utf8Path) {
    fs::create_directories(fs::u8path(utf8Path));
}

// Remove a directory tree from a UTF-8 path string.
static void removeDirs(const std::string& utf8Path) {
    std::error_code ec;
    fs::remove_all(fs::u8path(utf8Path), ec);
}

// Check file existence using a UTF-8 path string.
static bool fileExists(const std::string& utf8Path) {
    return fs::exists(fs::u8path(utf8Path));
}

// ============================================================================
// Phase 1 — open_ifstream / open_ofstream primitives
// ============================================================================

TEST_CASE("open_ofstream ASCII path", "[unicode_path]") {
    std::string dir  = makeTempDir("whiteout_uni_ascii");
    std::string path = dir + "/test.bin";
    createDirs(dir);

    const std::vector<u8> expected = {0x01, 0x02, 0x03, 0x04, 0x05};
    {
        auto out = whiteout::common::open_ofstream(path, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(expected.data()),
                  static_cast<std::streamsize>(expected.size()));
    }

    CHECK(fileExists(path));
    removeDirs(dir);
}

TEST_CASE("open_ifstream ASCII path round-trip", "[unicode_path]") {
    std::string dir  = makeTempDir("whiteout_uni_ascii_rt");
    std::string path = dir + "/test.bin";
    createDirs(dir);

    const std::vector<u8> expected = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    {
        auto out = whiteout::common::open_ofstream(path, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(expected.data()),
                  static_cast<std::streamsize>(expected.size()));
    }

    {
        auto in = whiteout::common::open_ifstream(path, std::ios::binary);
        REQUIRE(in.is_open());
        std::vector<u8> buf(expected.size());
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(expected.size()));
        CHECK(buf == expected);
    }

    removeDirs(dir);
}

TEST_CASE("open_ofstream unicode path", "[unicode_path]") {
    std::string dir  = makeTempDir(std::string(kChinese).append("_whiteout_ofstream").c_str());
    std::string path = dir + "/test.bin";
    createDirs(dir);

    const std::vector<u8> data = {0x11, 0x22, 0x33};
    {
        auto out = whiteout::common::open_ofstream(path, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    CHECK(fileExists(path));
    removeDirs(dir);
}

TEST_CASE("open_ifstream unicode path round-trip", "[unicode_path]") {
    std::string dir  = makeTempDir(std::string(kChinese).append("_whiteout_ifstream").c_str());
    std::string path = dir + "/roundtrip.bin";
    createDirs(dir);

    const std::vector<u8> expected = {0xAA, 0xBB, 0xCC, 0xDD};
    {
        auto out = whiteout::common::open_ofstream(path, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(expected.data()),
                  static_cast<std::streamsize>(expected.size()));
    }

    {
        auto in = whiteout::common::open_ifstream(path, std::ios::binary);
        REQUIRE(in.is_open());
        std::vector<u8> buf(expected.size());
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(expected.size()));
        CHECK(buf == expected);
    }

    removeDirs(dir);
}

// ============================================================================
// Phase 2 — io_helpers (read_file_bytes / write_file_bytes)
// ============================================================================

TEST_CASE("io_helpers round-trip unicode path", "[unicode_path][io_helpers]") {
    std::string dir  = makeTempDir(std::string(kChinese).append("_whiteout_io").c_str());
    std::string path = dir + "/payload.bin";
    createDirs(dir);

    const std::vector<u8> original = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

    {
        textures::IssueSink sink;
        bool ok = textures::write_file_bytes(path,
                                              std::span<const u8>(original.data(), original.size()),
                                              sink);
        REQUIRE(ok);
        REQUIRE(sink.issues.empty());
    }

    {
        textures::IssueSink sink;
        auto result = textures::read_file_bytes(path, sink);
        REQUIRE(result.has_value());
        REQUIRE(sink.issues.empty());
        CHECK(*result == original);
    }

    removeDirs(dir);
}

// ============================================================================
// Phase 3 — BMP format round-trip via unicode path
// ============================================================================

TEST_CASE("BMP round-trip unicode path", "[unicode_path][bmp]") {
    std::string dir  = makeTempDir(std::string(kChinese).append("_whiteout_bmp").c_str());
    std::string path = dir + "/test.bmp";
    createDirs(dir);

    // Create a 4×4 RGBA8 texture with known pixel data.
    constexpr u32 W = 4, H = 4;
    auto tex = Texture::create2D(PixelFormat::RGBA8, W, H, 1);
    {
        auto pixels = tex.mipData(0);
        for (u32 i = 0; i < W * H; ++i) {
            pixels[i * 4 + 0] = static_cast<u8>(i * 10);       // R
            pixels[i * 4 + 1] = static_cast<u8>(i * 10 + 1);   // G
            pixels[i * 4 + 2] = static_cast<u8>(i * 10 + 2);   // B
            pixels[i * 4 + 3] = 0xFF;                           // A
        }
    }
    const u8 firstR = tex.mipData(0)[0];

    {
        bmp::Writer writer;
        REQUIRE_NOTHROW(writer.write(path, tex));
        REQUIRE_FALSE(writer.hasIssues());
    }
    REQUIRE(fileExists(path));

    {
        bmp::Parser parser;
        auto result = parser.parse(path);
        REQUIRE(result.has_value());
        REQUIRE_FALSE(parser.hasIssues());

        CHECK(result->width()  == W);
        CHECK(result->height() == H);
        CHECK(result->format() == PixelFormat::RGBA8);

        // First pixel red channel must survive the round-trip.
        CHECK(result->mipData(0)[0] == firstR);
    }

    removeDirs(dir);
}

// ============================================================================
// Phase 4 — MDX format round-trip via unicode path
// ============================================================================

TEST_CASE("MDX round-trip unicode path", "[unicode_path][mdx]") {
    std::string dir  = makeTempDir(std::string(kChinese).append("_whiteout_mdx").c_str());
    std::string path = dir + "/test.mdx";
    createDirs(dir);

    // Build a minimal Model.
    mdx::Model model;
    model.version   = 800;
    model.modelName = "UnicodeTest";

    {
        mdx::Writer writer;
        REQUIRE_NOTHROW(writer.write(path, model));
    }
    REQUIRE(fileExists(path));

    {
        mdx::Parser parser(mdx::Parser::ParseMode::Lenient,
                           mdx::Parser::UpgradeMode::PreserveOriginal);
        mdx::Model loaded = parser.parse(path);
        REQUIRE_FALSE(parser.hasIssues());

        CHECK(loaded.version   == 800);
        CHECK(loaded.modelName == "UnicodeTest");
    }

    removeDirs(dir);
}

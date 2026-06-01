// SPDX-License-Identifier: BSD-3-Clause
// TIFF parser & writer tests — driven by the TDD plan in docs/TIFF.md.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/textures/texture.h>
#include <whiteout/textures/tiff/tiff.h>

using namespace whiteout;
using namespace whiteout::textures;

namespace {

// Locate Corpus/TIFF/ref/<name> relative to the working directory CMake set
// via catch_discover_tests (CMAKE_SOURCE_DIR). The same approach the BLP and
// MDL corpus tests use.
std::vector<u8> readFixture(const std::string& name) {
    const std::string path = "Corpus/TIFF/ref/" + name;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.is_open());
    const auto end = f.tellg();
    REQUIRE(end >= 0);
    f.seekg(0, std::ios::beg);
    std::vector<u8> buf(static_cast<size_t>(end));
    if (!buf.empty()) {
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(buf.size()));
    }
    return buf;
}

bool anyIssueContains(const std::vector<std::string>& issues,
                      const std::string& substr) {
    return std::any_of(issues.begin(), issues.end(),
                       [&](const std::string& s) {
                           return s.find(substr) != std::string::npos;
                       });
}

// One spot-check record matching the .golden.json schema.
// Channels uses the SOURCE channel count (1 for gray, 3 for RGB, 4 for RGBA).
// The parser is expected to lift everything to RGBA8 — we compare against
// the spot's source channels and ignore alpha for sub-4-channel sources.
struct Spot {
    u32 x;
    u32 y;
    std::array<u8, 4> rgba; // unused channels = 0
};

// Read the 4-byte pixel at (x, y) from an RGBA8 Texture's base mip.
std::array<u8, 4> readRgba8Pixel(const Texture& tex, u32 x, u32 y) {
    REQUIRE(tex.format() == PixelFormat::RGBA8);
    auto span = tex.mipData(0, 0);
    const u8* p = span.data() + (y * tex.width() + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

void expectRgb8Spot(const Texture& tex, const Spot& s) {
    auto px = readRgba8Pixel(tex, s.x, s.y);
    INFO("spot @ (" << s.x << "," << s.y << ")");
    REQUIRE(px[0] == s.rgba[0]);
    REQUIRE(px[1] == s.rgba[1]);
    REQUIRE(px[2] == s.rgba[2]);
}

void expectRgba8Spot(const Texture& tex, const Spot& s) {
    auto px = readRgba8Pixel(tex, s.x, s.y);
    INFO("spot @ (" << s.x << "," << s.y << ")");
    REQUIRE(px[0] == s.rgba[0]);
    REQUIRE(px[1] == s.rgba[1]);
    REQUIRE(px[2] == s.rgba[2]);
    REQUIRE(px[3] == s.rgba[3]);
}

void expectGray8Spot(const Texture& tex, const Spot& s) {
    auto px = readRgba8Pixel(tex, s.x, s.y);
    INFO("spot @ (" << s.x << "," << s.y << ")");
    REQUIRE(px[0] == s.rgba[0]);
    REQUIRE(px[1] == s.rgba[0]); // gray lifted: R==G==B
    REQUIRE(px[2] == s.rgba[0]);
}

} // namespace

// ============================================================================
// Phase 1.1 — class exists & links
// ============================================================================

TEST_CASE("tiff_test::class_exists_and_links", "[tiff][phase1]") {
    tiff::Parser parser;
    SUCCEED("Parser instantiates without throwing");
}

// ============================================================================
// Phase 1.2 — header-size validation
// ============================================================================

TEST_CASE("tiff_test::rejects_buffer_smaller_than_header", "[tiff][phase1]") {
    tiff::Parser parser;

    // A 4-byte buffer cannot contain even the 8-byte classic TIFF header.
    const std::array<u8, 4> tiny{'I', 'I', '*', 0};
    auto result = parser.parse(std::span<const u8>{tiny});

    REQUIRE_FALSE(result.has_value());
    REQUIRE(parser.hasIssues());
    REQUIRE(anyIssueContains(parser.getIssues(), "small"));
}

// ============================================================================
// Phase 1.3 — uncompressed RGB, little-endian
// ============================================================================
// Spots taken from Corpus/TIFF/ref/uncompressed_rgb_le_32.golden.json
TEST_CASE("tiff_test::parses_uncompressed_rgb_le_32", "[tiff][phase1]") {
    auto buf = readFixture("uncompressed_rgb_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);
    REQUIRE(result->format() == PixelFormat::RGBA8);

    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},      // top-left red
        {24, 0, {0, 255, 0, 0}},     // top-right green
        {0, 24, {0, 0, 255, 0}},     // bot-left blue
        {24, 24, {192, 192, 192, 0}},
        {15, 15, {255, 0, 0, 0}},
        {16, 16, {128, 128, 128, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

// ============================================================================
// Phase 1.4 — big-endian (MM) byte order
// ============================================================================
TEST_CASE("tiff_test::parses_uncompressed_rgb_be_32", "[tiff][phase1]") {
    auto buf = readFixture("uncompressed_rgb_be_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);

    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},
        {24, 0, {0, 255, 0, 0}},
        {0, 24, {0, 0, 255, 0}},
        {24, 24, {192, 192, 192, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

// ============================================================================
// Phase 1.5 — RGBA8 + gray photometric (LE only — BE was already pinned by 1.4)
// ============================================================================
TEST_CASE("tiff_test::parses_uncompressed_rgba_le_32", "[tiff][phase1]") {
    auto buf = readFixture("uncompressed_rgba_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);
    REQUIRE(result->format() == PixelFormat::RGBA8);

    // Alpha is meaningful in this fixture (128 for green quadrant).
    const Spot spots[] = {
        {0, 0, {255, 0, 0, 255}},
        {24, 0, {0, 255, 0, 128}},
        {0, 24, {0, 0, 255, 255}},
        {24, 24, {192, 192, 192, 255}},
        {16, 16, {128, 128, 128, 255}},
    };
    for (const auto& s : spots)
        expectRgba8Spot(*result, s);
}

TEST_CASE("tiff_test::parses_uncompressed_gray_le_32", "[tiff][phase1]") {
    auto buf = readFixture("uncompressed_gray_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);
    REQUIRE(result->format() == PixelFormat::RGBA8);

    // Gray spot values are luma of the RGB pattern (rec.601 from the regen
    // script). The parser lifts R=G=B=gray, alpha=255.
    const Spot spots[] = {
        {0, 0, {76, 0, 0, 0}},     // luma(255,0,0)
        {24, 0, {150, 0, 0, 0}},   // luma(0,255,0)
        {0, 24, {29, 0, 0, 0}},    // luma(0,0,255)
        {24, 24, {192, 0, 0, 0}},  // luma(192,192,192)
    };
    for (const auto& s : spots)
        expectGray8Spot(*result, s);
}

// ============================================================================
// Phase 1.6 — multi-strip RGB
// ============================================================================
TEST_CASE("tiff_test::parses_multi_strip_rgb_le_32", "[tiff][phase1]") {
    auto buf = readFixture("multistrip_rgb_le_32_rps8.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);

    // Same pattern as uncompressed_rgb_le_32, but split across 4 strips.
    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},
        {24, 0, {0, 255, 0, 0}},
        {0, 24, {0, 0, 255, 0}},
        {24, 24, {192, 192, 192, 0}},
        // Spot on a strip boundary (y=8 is the first row of strip 1).
        {0, 8, {255, 0, 0, 0}},
        {0, 16, {0, 0, 255, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

// ============================================================================
// Phase 2.6 — LZW integration (exercises the early-change quirk on a real
// PIL-encoded strip; corrupt pixels at later spots indicate a boundary bug).
// ============================================================================
TEST_CASE("tiff_test::parses_lzw_rgb_le_32", "[tiff][phase2]") {
    auto buf = readFixture("lzw_rgb_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);

    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},
        {24, 0, {0, 255, 0, 0}},
        {0, 24, {0, 0, 255, 0}},
        {24, 24, {192, 192, 192, 0}}, // beyond the 9→10 LZW boundary
        {16, 16, {128, 128, 128, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

// ============================================================================
// Phase 2.3 — Deflate compression
// ============================================================================
TEST_CASE("tiff_test::parses_deflate_rgb_le_32", "[tiff][phase2]") {
    auto buf = readFixture("deflate_rgb_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);

    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},
        {24, 0, {0, 255, 0, 0}},
        {0, 24, {0, 0, 255, 0}},
        {24, 24, {192, 192, 192, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

// ============================================================================
// Phase 3.4 — 8-bit palette photometric
// ============================================================================
TEST_CASE("tiff_test::parses_palette_8bit_le_32", "[tiff][phase3]") {
    auto buf = readFixture("palette_8bit_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);
    REQUIRE(result->format() == PixelFormat::RGBA8);

    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},
        {24, 0, {0, 255, 0, 0}},
        {0, 24, {0, 0, 255, 0}},
        {24, 24, {192, 192, 192, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

// ============================================================================
// Phase 2.7 — Predictor 2 (horizontal differencing) with LZW and Deflate
// ============================================================================
TEST_CASE("tiff_test::parses_lzw_predictor2_rgb_le_32", "[tiff][phase2]") {
    auto buf = readFixture("lzw_predictor2_rgb_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);

    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},
        {24, 0, {0, 255, 0, 0}},
        {0, 24, {0, 0, 255, 0}},
        {24, 24, {192, 192, 192, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

TEST_CASE("tiff_test::parses_deflate_predictor2_rgb_le_32", "[tiff][phase2]") {
    auto buf = readFixture("deflate_predictor2_rgb_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);

    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},
        {24, 0, {0, 255, 0, 0}},
        {0, 24, {0, 0, 255, 0}},
        {24, 24, {192, 192, 192, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

// ============================================================================
// Phase 2.2 — PackBits compression integration
// ============================================================================
TEST_CASE("tiff_test::parses_packbits_rgb_le_32", "[tiff][phase2]") {
    auto buf = readFixture("packbits_rgb_le_32.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});

    INFO("issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(result.has_value());
    REQUIRE(result->width() == 32);
    REQUIRE(result->height() == 32);
    REQUIRE(result->format() == PixelFormat::RGBA8);

    const Spot spots[] = {
        {0, 0, {255, 0, 0, 0}},
        {24, 0, {0, 255, 0, 0}},
        {0, 24, {0, 0, 255, 0}},
        {24, 24, {192, 192, 192, 0}},
        {16, 16, {128, 128, 128, 0}},
    };
    for (const auto& s : spots)
        expectRgb8Spot(*result, s);
}

// ============================================================================
// Phase 4.1 — writer class exists
// ============================================================================
TEST_CASE("tiff_test::writer_class_exists", "[tiff][phase4]") {
    tiff::Writer writer;
    SUCCEED("Writer instantiates without throwing");
}

// ============================================================================
// Phase 4.2 — uncompressed round-trip
// ============================================================================
TEST_CASE("tiff_test::round_trip_uncompressed_rgba8", "[tiff][phase4]") {
    // Source: 8x8 checkerboard of red/green.
    auto src = Texture::create2D(PixelFormat::RGBA8, 8, 8, 1);
    auto data = src.dataPtr();
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool checker = ((x + y) & 1) != 0;
            u8* px = data + (y * 8 + x) * 4;
            if (checker) {
                px[0] = 255; px[1] = 0;   px[2] = 0;   px[3] = 255;
            } else {
                px[0] = 0;   px[1] = 255; px[2] = 0;   px[3] = 200;
            }
        }
    }

    tiff::Writer writer;
    auto bytes = writer.write(src);
    INFO("write issues: " << (writer.hasIssues() ? writer.getIssues()[0] : ""));
    REQUIRE_FALSE(bytes.empty());

    tiff::Parser parser;
    auto roundTripped = parser.parse(std::span<const u8>{bytes});
    INFO("parse issues: " << (parser.hasIssues() ? parser.getIssues()[0] : ""));
    REQUIRE(roundTripped.has_value());
    REQUIRE(roundTripped->width() == 8);
    REQUIRE(roundTripped->height() == 8);
    REQUIRE(roundTripped->format() == PixelFormat::RGBA8);

    // Pixel-equal at every position.
    auto srcData = src.mipData(0, 0);
    auto rtData = roundTripped->mipData(0, 0);
    REQUIRE(srcData.size() == rtData.size());
    for (size_t i = 0; i < srcData.size(); ++i) {
        INFO("byte " << i);
        REQUIRE(srcData[i] == rtData[i]);
    }
}

// ============================================================================
// Phase 4.5 — our output is parseable as TIFF by an external reference
// implementation. We write to a temp file and shell out to libtiff's tiffinfo
// (if available), otherwise we settle for re-parsing under our own parser.
// ============================================================================
// ============================================================================
// Phase 5 — Named diagnostics for unsupported features
// ============================================================================

TEST_CASE("tiff_test::rejects_bigtiff_with_named_diagnostic", "[tiff][phase5]") {
    auto buf = readFixture("bigtiff_header.tif");
    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(parser.hasIssues());
    REQUIRE(anyIssueContains(parser.getIssues(), "BigTIFF"));
}

TEST_CASE("tiff_test::rejects_ccitt_with_named_diagnostic", "[tiff][phase5]") {
    // Synthesize a header pointing to an IFD that declares Compression=4 (CCITT
    // Group 4). The parser shouldn't crash and should emit a recognizable
    // diagnostic naming the compression code.
    std::vector<u8> buf;
    // Header: II + 42 + IFD offset = 8.
    buf = {'I', 'I', 42, 0, 8, 0, 0, 0};
    // IFD: 6 entries (just enough to look valid until compression rejects).
    auto pushLE16 = [&](u16 v) { buf.push_back(u8(v)); buf.push_back(u8(v >> 8)); };
    auto pushLE32 = [&](u32 v) {
        buf.push_back(u8(v)); buf.push_back(u8(v >> 8));
        buf.push_back(u8(v >> 16)); buf.push_back(u8(v >> 24));
    };
    pushLE16(6);
    auto addEntry = [&](u16 tag, u16 type, u32 count, u32 v) {
        pushLE16(tag); pushLE16(type); pushLE32(count); pushLE32(v);
    };
    addEntry(256, 3, 1, 16);   // ImageWidth 16
    addEntry(257, 3, 1, 16);   // ImageLength 16
    addEntry(258, 3, 1, 1);    // BitsPerSample 1
    addEntry(259, 3, 1, 4);    // Compression 4 (CCITT Group 4)
    addEntry(262, 3, 1, 1);    // Photometric BlackIsZero
    addEntry(277, 3, 1, 1);    // SamplesPerPixel 1
    pushLE32(0); // next IFD

    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(parser.hasIssues());
    REQUIRE(anyIssueContains(parser.getIssues(), "compression"));
    REQUIRE(anyIssueContains(parser.getIssues(), "4"));
}

TEST_CASE("tiff_test::rejects_jpeg_in_tiff_with_named_diagnostic",
          "[tiff][phase5]") {
    // Same shape as the CCITT test, but with Compression=7 (NewJPEG).
    std::vector<u8> buf = {'I', 'I', 42, 0, 8, 0, 0, 0};
    auto pushLE16 = [&](u16 v) { buf.push_back(u8(v)); buf.push_back(u8(v >> 8)); };
    auto pushLE32 = [&](u32 v) {
        buf.push_back(u8(v)); buf.push_back(u8(v >> 8));
        buf.push_back(u8(v >> 16)); buf.push_back(u8(v >> 24));
    };
    pushLE16(5);
    auto addEntry = [&](u16 tag, u16 type, u32 count, u32 v) {
        pushLE16(tag); pushLE16(type); pushLE32(count); pushLE32(v);
    };
    addEntry(256, 3, 1, 16);   // ImageWidth
    addEntry(257, 3, 1, 16);   // ImageLength
    addEntry(259, 3, 1, 7);    // Compression 7 (NewJPEG)
    addEntry(262, 3, 1, 2);    // Photometric RGB
    addEntry(277, 3, 1, 3);    // SamplesPerPixel 3
    pushLE32(0);

    tiff::Parser parser;
    auto result = parser.parse(std::span<const u8>{buf});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(parser.hasIssues());
    REQUIRE(anyIssueContains(parser.getIssues(), "7"));
}

// ============================================================================
// Wild corpus: real assets (LZW + Predictor 2 + RGBA + ExtraSamples + many
// strips). These shouldn't need any new code beyond what Phase 2 already
// landed; the test exists to catch regressions and to prove the parser
// handles real game-asset TIFFs.
// ============================================================================
TEST_CASE("tiff_test::parses_koltira_wild_corpus", "[tiff][corpus]") {
    namespace fs = std::filesystem;
    const std::string root =
        "Corpus/MDL/Koltira Deathweaver (High Elf Death Knight)";

    if (!fs::exists(root)) {
        SKIP("Koltira corpus not present");
        return;
    }

    int count = 0;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".tif" && ext != ".tiff")
            continue;

        ++count;
        const auto path = entry.path().string();
        INFO("file: " << path);

        tiff::Parser parser;
        auto result = parser.parse(path);

        if (!result) {
            FAIL("parse failed; first issue: " <<
                 (parser.hasIssues() ? parser.getIssues()[0] : "<none>"));
        }
        REQUIRE(result->width() > 0);
        REQUIRE(result->height() > 0);
        REQUIRE(result->format() == PixelFormat::RGBA8);
        // Sanity: total bytes match width * height * 4.
        REQUIRE(result->dataSize() ==
                static_cast<u64>(result->width()) * result->height() * 4);
    }
    REQUIRE(count > 0); // ensure we actually exercised the corpus
}

TEST_CASE("tiff_test::writer_emits_valid_header_layout", "[tiff][phase4]") {
    // Verify our writer output matches our parser's idea of a valid TIFF
    // by directly inspecting a few header bytes.
    auto src = Texture::create2D(PixelFormat::RGBA8, 4, 4, 1);
    tiff::Writer writer;
    auto bytes = writer.write(src);

    REQUIRE(bytes.size() >= 8);
    REQUIRE(bytes[0] == 'I');
    REQUIRE(bytes[1] == 'I');
    REQUIRE(bytes[2] == 42);  // magic LE low byte
    REQUIRE(bytes[3] == 0);   // magic LE high byte
    // IFD offset (LE u32) should point past the pixel data.
    const u32 ifdOffset = u32(bytes[4]) | (u32(bytes[5]) << 8) |
                          (u32(bytes[6]) << 16) | (u32(bytes[7]) << 24);
    // pixelOffset(8) + 4*4*4 pixel bytes = 72.
    REQUIRE(ifdOffset == 72);
}

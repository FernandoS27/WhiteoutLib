// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
#include <catch2/catch_all.hpp>

#include <whiteout/textures/d2r_texture/d2r_texture.h>
#include <whiteout/textures/texture.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::textures;

static std::vector<u8> readFileBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamsize size = file.tellg();
    if (size < 0)
        return {};
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size))
        return {};
    return data;
}

TEST_CASE("D2R texture synthetic round-trip", "[d2r][texture]") {
    // A 4x4 BC3 texture with two mips, exercises the self-relative mip table.
    auto tex = Texture::create2D(PixelFormat::BC3, 4, 4, 2);
    auto data = tex.data();
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<u8>(i * 7 + 1);

    d2r_texture::Writer writer;
    auto bytes = writer.write(tex);
    REQUIRE_FALSE(bytes.empty());
    REQUIRE_FALSE(writer.hasIssues());

    // Header sanity: magic + linear BC3 code.
    REQUIRE(bytes.size() >= 0x24);
    CHECK(std::memcmp(bytes.data(), "<DE(", 4) == 0);

    d2r_texture::Parser parser;
    d2r_texture::D2rTextureInfo info{};
    auto parsed = parser.parse(std::span<const u8>{bytes}, &info);
    REQUIRE(parsed.has_value());
    CHECK(info.formatCode == 61);
    CHECK(info.pixelFormat == PixelFormat::BC3);
    CHECK(parsed->width() == 4);
    CHECK(parsed->height() == 4);
    CHECK(parsed->mipCount() == 2);

    // Block data must survive the round-trip verbatim.
    REQUIRE(parsed->dataSize() == tex.dataSize());
    CHECK(std::memcmp(parsed->dataPtr(), tex.dataPtr(), tex.dataSize()) == 0);
}

TEST_CASE("D2R texture sRGB selects BC3 code 62", "[d2r][texture]") {
    auto tex = Texture::create2D(PixelFormat::BC3, 8, 8, 1);
    tex.setSrgb(true);

    d2r_texture::Writer writer;
    auto bytes = writer.write(tex);
    REQUIRE_FALSE(bytes.empty());

    d2r_texture::Parser parser;
    d2r_texture::D2rTextureInfo info{};
    auto parsed = parser.parse(std::span<const u8>{bytes}, &info);
    REQUIRE(parsed.has_value());
    CHECK(info.formatCode == 62);
    CHECK(parsed->isSrgb());
}

TEST_CASE("D2R texture rejects unsupported pixel format", "[d2r][texture]") {
    auto tex = Texture::create2D(PixelFormat::BC7, 8, 8, 1);
    d2r_texture::Writer writer;
    auto bytes = writer.write(tex);
    CHECK(bytes.empty());
    CHECK(writer.hasIssues());
}

TEST_CASE("D2R texture corpus byte-exact round-trip", "[d2r][texture][corpus]") {
    const fs::path corpusRoot = fs::path("Corpus/data/hd/vfx/textures");
    if (!fs::exists(corpusRoot) || !fs::is_directory(corpusRoot))
        SKIP("Missing corpus directory: " + corpusRoot.string());

    d2r_texture::Parser parser;
    d2r_texture::Writer writer;

    size_t parsed = 0;
    size_t exact = 0;
    size_t skipped = 0;

    for (const auto& e : fs::recursive_directory_iterator(corpusRoot)) {
        if (!e.is_regular_file() || e.path().extension() != ".texture")
            continue;

        const auto original = readFileBytes(e.path());
        if (original.empty())
            continue;

        d2r_texture::D2rTextureInfo info{};
        auto tex = parser.parse(std::span<const u8>{original}, &info);
        if (!tex) {
            // Non-power-of-two uncompressed surfaces have no mip chain assumption
            // beyond one level; any genuine parse failure should be rare.
            INFO("parse failed: " << e.path().string());
            for (const auto& msg : parser.getIssues())
                INFO(msg);
            FAIL_CHECK("parse failure");
            continue;
        }
        ++parsed;

        // Re-emit with the exact original format code to guarantee a faithful
        // byte-for-byte reconstruction.
        d2r_texture::SaveOptions opts;
        opts.formatCode = info.formatCode;
        auto rewritten = writer.write(*tex, opts);
        if (rewritten.empty()) {
            ++skipped;
            continue;
        }

        if (rewritten == original) {
            ++exact;
        } else {
            INFO("round-trip mismatch: " << e.path().string()
                                         << " orig=" << original.size()
                                         << " new=" << rewritten.size());
            CHECK(rewritten == original);
        }
    }

    INFO("parsed=" << parsed << " exact=" << exact << " skipped=" << skipped);
    CHECK(parsed > 0);
    CHECK(exact == parsed);
}

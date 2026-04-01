// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
#include <catch2/catch_all.hpp>

#include <whiteout/textures/tex/tex.h>
#include <whiteout/textures/texture.h>

#include <fstream>
#include <filesystem>
#include <map>
#include <string>
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

TEST_CASE("D4 TEX corpus verify", "[d4][tex][corpus]") {
    const fs::path corpusRoot = fs::path("Models/D4");
    const fs::path metaDir = corpusRoot / "meta" / "Texture";
    const fs::path payloadDir = corpusRoot / "payload" / "Texture";

    if (!fs::exists(metaDir) || !fs::is_directory(metaDir))
        SKIP("Missing meta directory: " + metaDir.string());
    if (!fs::exists(payloadDir) || !fs::is_directory(payloadDir))
        SKIP("Missing payload directory: " + payloadDir.string());

    std::vector<fs::path> metaFiles;
    for (const auto& e : fs::directory_iterator(metaDir)) {
        if (!e.is_regular_file())
            continue;
        const auto ext = e.path().extension().string();
        if (ext == ".tex" || ext == ".TEX")
            metaFiles.push_back(e.path());
    }

    REQUIRE_FALSE(metaFiles.empty());

    tex::Parser parser;

    size_t parsedOk = 0;
    size_t parsedFail = 0;

    for (const auto& metaPath : metaFiles) {
        const fs::path payloadPath = payloadDir / metaPath.filename();
        if (!fs::exists(payloadPath))
            continue;

        const auto metaBytes = readFileBytes(metaPath);
        const auto payloadBytes = readFileBytes(payloadPath);
        if (metaBytes.empty() || payloadBytes.empty()) {
            ++parsedFail;
            continue;
        }

        tex::D4TexInfo info{};
        auto tex = parser.parse(std::span<const u8>{metaBytes},
                                std::span<const u8>{payloadBytes}, &info);
        if (!tex) {
            ++parsedFail;
            continue;
        }

        ++parsedOk;
        CHECK(tex->width() > 0);
        CHECK(tex->height() > 0);
        CHECK(tex->mipCount() > 0);
    }

    CHECK(parsedFail == 0);
    CHECK(parsedOk > 0);
}

TEST_CASE("D4 TEX format 49/50 decode spot-check", "[d4][tex][corpus]") {
    const fs::path corpusRoot = fs::path("Models/D4");
    const fs::path metaDir = corpusRoot / "meta" / "Texture";
    const fs::path payloadDir = corpusRoot / "payload" / "Texture";

    if (!fs::exists(metaDir) || !fs::is_directory(metaDir))
        SKIP("Missing meta directory: " + metaDir.string());
    if (!fs::exists(payloadDir) || !fs::is_directory(payloadDir))
        SKIP("Missing payload directory: " + payloadDir.string());

    tex::Parser parser;
    size_t checked = 0;
    constexpr size_t kMaxSpot = 10;

    for (const auto& e : fs::directory_iterator(metaDir)) {
        if (checked >= kMaxSpot)
            break;
        if (!e.is_regular_file())
            continue;
        const auto ext = e.path().extension().string();
        if (ext != ".tex" && ext != ".TEX")
            continue;

        const fs::path payloadPath = payloadDir / e.path().filename();
        if (!fs::exists(payloadPath))
            continue;

        const auto metaBytes = readFileBytes(e.path());
        const auto payloadBytes = readFileBytes(payloadPath);
        if (metaBytes.empty() || payloadBytes.empty())
            continue;

        tex::D4TexInfo info{};
        auto result = parser.parse(std::span<const u8>{metaBytes},
                                   std::span<const u8>{payloadBytes}, &info);
        if (!result)
            continue;

        constexpr u32 D4_FMT_49 = 49;
        constexpr u32 D4_FMT_50 = 50;
        if (info.texFormat != D4_FMT_49 && info.texFormat != D4_FMT_50)
            continue;

        auto decoded = result->copyAsFormat(PixelFormat::RGBA8);
        REQUIRE(decoded.width() > 0);
        ++checked;
    }
}

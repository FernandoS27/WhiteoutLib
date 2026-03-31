// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/textures/tex/tex.h>
#include <whiteout/textures/texture.h>

#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<whiteout::u8> readFileBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamsize size = file.tellg();
    if (size < 0)
        return {};
    file.seekg(0, std::ios::beg);
    std::vector<whiteout::u8> data(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size))
        return {};
    return data;
}

int main(int argc, char** argv) {
    const fs::path corpusRoot = (argc >= 2) ? fs::path(argv[1]) : fs::path("Models/D4");
    const fs::path metaDir = corpusRoot / "meta" / "Texture";
    const fs::path payloadDir = corpusRoot / "payload" / "Texture";

    if (!fs::exists(metaDir) || !fs::is_directory(metaDir)) {
        std::cerr << "Missing meta directory: " << metaDir.string() << "\n";
        return 2;
    }
    if (!fs::exists(payloadDir) || !fs::is_directory(payloadDir)) {
        std::cerr << "Missing payload directory: " << payloadDir.string() << "\n";
        return 2;
    }

    std::vector<fs::path> metaFiles;
    for (const auto& e : fs::directory_iterator(metaDir)) {
        if (!e.is_regular_file())
            continue;
        const auto ext = e.path().extension().string();
        if (ext == ".tex" || ext == ".TEX")
            metaFiles.push_back(e.path());
    }

    std::cout << "D4 TEX corpus verify\n";
    std::cout << "meta dir:    " << metaDir.string() << "\n";
    std::cout << "payload dir: " << payloadDir.string() << "\n";
    std::cout << "meta files:  " << metaFiles.size() << "\n\n";

    whiteout::textures::tex::Parser parser;

    size_t parsedOk = 0;
    size_t parsedFail = 0;
    size_t missingPayload = 0;
    size_t hasFrames = 0;
    size_t twoTier = 0;

    std::map<whiteout::u32, size_t> fmtCounts;  // eTexFormat → count

    // Format-49/50 decode spot-check
    size_t fmt4950DecodeOk = 0;
    size_t fmt4950DecodeFail = 0;
    constexpr size_t kMaxFmt4950Spot = 10; // only check first N format-49/50 textures

    struct Failure {
        std::string file;
        std::string reason;
    };
    std::vector<Failure> failures;
    failures.reserve(64);

    for (const auto& metaPath : metaFiles) {
        const fs::path payloadPath = payloadDir / metaPath.filename();
        if (!fs::exists(payloadPath)) {
            ++missingPayload;
            continue;
        }

        const auto metaBytes = readFileBytes(metaPath);
        const auto payloadBytes = readFileBytes(payloadPath);
        if (metaBytes.empty() || payloadBytes.empty()) {
            ++parsedFail;
            if (failures.size() < 30) {
                failures.push_back(Failure{metaPath.filename().string(),
                                           "Failed to read meta/payload file bytes"});
            }
            continue;
        }

        whiteout::textures::tex::D4TexInfo info{};
        auto tex = parser.parse(std::span<const whiteout::u8>{metaBytes},
                                std::span<const whiteout::u8>{payloadBytes}, &info);
        if (!tex) {
            ++parsedFail;
            std::string reason = "unknown parse failure";
            if (parser.hasIssues() && !parser.getIssues().empty())
                reason = parser.getIssues().front();
            if (failures.size() < 30)
                failures.push_back(Failure{metaPath.filename().string(), reason});
            continue;
        }

        ++parsedOk;
        if (!info.frames.empty())
            ++hasFrames;
        if (info.isTwoTier)
            ++twoTier;

        fmtCounts[info.texFormat]++;

        // Format-49/50 decode spot-check: try converting to RGBA8 and validate result.
        constexpr whiteout::u32 D4_FMT_49 = 49;
        constexpr whiteout::u32 D4_FMT_50 = 50;
        bool isFmt4950 = (info.texFormat == D4_FMT_49 || info.texFormat == D4_FMT_50);
        if (isFmt4950 && (fmt4950DecodeOk + fmt4950DecodeFail) < kMaxFmt4950Spot) {
            auto decoded = tex->copyAsFormat(whiteout::textures::PixelFormat::RGBA8);
            if (decoded.width() == 0) {
                ++fmt4950DecodeFail;
                if (failures.size() < 30)
                    failures.push_back({metaPath.filename().string(), "Format-49/50 decode to RGBA8 failed"});
            } else {
                // Sanity: check pixel diversity (not all identical) in mip0
                auto px = decoded.mipData(0);
                bool anyNonUniform = false;
                if (px.size() >= 8) {
                    for (size_t k = 4; k + 3 < px.size(); k += 4) {
                        if (px[k] != px[0] || px[k+1] != px[1] || px[k+2] != px[2] || px[k+3] != px[3]) {
                            anyNonUniform = true;
                            break;
                        }
                    }
                }
                if (!anyNonUniform && px.size() > 64) {
                    ++fmt4950DecodeFail;
                    if (failures.size() < 30)
                        failures.push_back({metaPath.filename().string(),
                                            "Format-49/50 decode produced all-uniform pixels"});
                } else {
                    ++fmt4950DecodeOk;
                }
            }
        }

        if (tex->width() == 0 || tex->height() == 0 || tex->mipCount() == 0) {
            ++parsedFail;
            --parsedOk;
            if (failures.size() < 30) {
                failures.push_back(Failure{metaPath.filename().string(),
                                           "Parsed but produced invalid dimensions/mips"});
            }
        }
    }

    std::cout << "Parsed OK:       " << parsedOk << "\n";
    std::cout << "Parsed Failed:   " << parsedFail << "\n";
    std::cout << "Missing payload: " << missingPayload << "\n";
    std::cout << "Has frames:      " << hasFrames << "\n";
    std::cout << "Two-tier meta:   " << twoTier << "\n";

    std::cout << "\nFormat breakdown (eTexFormat counts):\n";
    for (auto& [fmt, cnt] : fmtCounts)
        std::cout << "  fmt " << fmt << ": " << cnt << "\n";

    std::cout << "\nFormat-49/50 decode spot-check (first " << kMaxFmt4950Spot
              << " format-49/50 textures):\n";
    std::cout << "  OK:   " << fmt4950DecodeOk << "\n";
    std::cout << "  FAIL: " << fmt4950DecodeFail << "\n";

    if (!failures.empty()) {
        std::cout << "\nSample failures (up to 30):\n";
        for (const auto& f : failures)
            std::cout << "  - " << f.file << ": " << f.reason << "\n";
    }

    // Return non-zero only if entries with available payload failed to parse or decode.
    return (parsedFail == 0 && fmt4950DecodeFail == 0) ? 0 : 1;
}

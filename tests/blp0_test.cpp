// BLP0 parser validation test
#include <catch2/catch_all.hpp>

#include <whiteout/textures/blp/blp.h>
#include <whiteout/textures/bmp/bmp.h>
#include <whiteout/textures/texture.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace whiteout;
using namespace whiteout::textures;

/// Return true if the buffer is not all zeroes.
static bool has_nonzero(std::span<const u8> data) {
    for (auto b : data) {
        if (b != 0)
            return true;
    }
    return false;
}

static bool is_blp0(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char magic[4] = {};
    fread(magic, 1, 4, f);
    fclose(f);
    return std::memcmp(magic, "BLP0", 4) == 0;
}

TEST_CASE("BLP0 corpus parse", "[blp][blp0][corpus]") {
    std::string searchDir = "Models/Alpha&BetaModels/Units";
    if (!fs::exists(searchDir))
        SKIP("BLP0 corpus directory not found: " + searchDir);

    // Collect all BLP0 files.
    std::vector<std::string> blp0Files;
    for (const auto& entry : fs::recursive_directory_iterator(searchDir)) {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".blp" && ext != ".BLP")
            continue;
        const auto path = entry.path().string();
        if (is_blp0(path))
            blp0Files.push_back(path);
    }

    REQUIRE_FALSE(blp0Files.empty());

    for (const auto& path : blp0Files) {
        DYNAMIC_SECTION("File: " << fs::path(path).filename().string()) {
            blp::Parser parser(blp::Parser::ParseMode::Lenient);
            auto tex = parser.parse(path);

            REQUIRE(tex.has_value());

            const u32 w = tex->width();
            const u32 h = tex->height();
            const u32 mips = tex->mipCount();

            CHECK(w > 0);
            CHECK(h > 0);
            CHECK(mips >= 1);

            for (u32 m = 0; m < mips; ++m) {
                const auto& ml = tex->mipLevel(m);
                const u32 expected_w = std::max(1u, w >> m);
                const u32 expected_h = std::max(1u, h >> m);
                CHECK(ml.width == expected_w);
                CHECK(ml.height == expected_h);

                auto data = tex->mipData(m);
                CHECK(has_nonzero(data));
            }
        }
    }
}

// BLP0 parser validation test
//
// Parses BLP0 files from Models/Alpha&BetaModels/Units and validates that:
//   1. The file is recognized as BLP0 and parses successfully
//   2. Dimensions and mip counts are reasonable
//   3. Mip data is non-empty (not all zeroes)
//   4. All mip levels have expected dimensions (halving each level)
//   5. Optionally writes mip 0 to BMP for visual inspection

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

// ============================================================================
// Test Helpers
// ============================================================================

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label, const std::string& detail = {}) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  FAIL: %s", label);
        if (!detail.empty())
            printf(" (%s)", detail.c_str());
        printf("\n");
    }
}

/// Return true if the buffer is not all zeroes.
static bool has_nonzero(std::span<const u8> data) {
    for (auto b : data) {
        if (b != 0)
            return true;
    }
    return false;
}

// ============================================================================
// Per-file test
// ============================================================================

static void test_blp0_file(const std::string& path, bool writeBmp) {
    const std::string name = fs::path(path).filename().string();
    printf("[%s]\n", name.c_str());

    blp::Parser parser(blp::Parser::ParseMode::Lenient);
    auto tex = parser.parse(path);

    // Check that parsing succeeded.
    check(tex.has_value(), "parse succeeds", path);
    if (!tex) {
        for (const auto& issue : parser.getIssues())
            printf("  issue: %s\n", issue.c_str());
        return;
    }

    // Print any non-fatal issues.
    if (parser.hasIssues()) {
        for (const auto& issue : parser.getIssues())
            printf("  warning: %s\n", issue.c_str());
    }

    // Basic dimension sanity.
    const u32 w = tex->width();
    const u32 h = tex->height();
    const u32 mips = tex->mipCount();
    printf("  %ux%u, %u mip(s), format=%u\n", w, h, mips,
           static_cast<unsigned>(tex->format()));

    check(w > 0 && h > 0, "non-zero dimensions");
    check(mips >= 1, "at least 1 mip level");

    // Validate each mip level.
    for (u32 m = 0; m < mips; ++m) {
        const auto& ml = tex->mipLevel(m);
        const u32 expected_w = std::max(1u, w >> m);
        const u32 expected_h = std::max(1u, h >> m);

        check(ml.width == expected_w && ml.height == expected_h, "mip dimensions",
              "mip " + std::to_string(m) + ": expected " + std::to_string(expected_w) + "x" +
                  std::to_string(expected_h) + ", got " + std::to_string(ml.width) + "x" +
                  std::to_string(ml.height));

        auto data = tex->mipData(m);
        check(has_nonzero(data), "mip data non-zero",
              "mip " + std::to_string(m) + " (" + std::to_string(data.size()) + " bytes)");
    }

    // Optionally write mip 0 to BMP.
    if (writeBmp) {
        const std::string outPath =
            (fs::path(path).parent_path() / (fs::path(path).stem().string() + "_blp0_out.bmp"))
                .string();
        bmp::Writer bmpWriter;
        bmpWriter.write(outPath, *tex);
        printf("  wrote: %s\n", outPath.c_str());
    }

    printf("  OK\n");
}

// ============================================================================
// Corpus scan — find all .blp files that are BLP0 under a directory tree
// ============================================================================

static bool is_blp0(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char magic[4] = {};
    fread(magic, 1, 4, f);
    fclose(f);
    return std::memcmp(magic, "BLP0", 4) == 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    // Default search directory relative to the executable's working directory.
    std::string searchDir = "Models/Alpha&BetaModels/Units";
    bool writeBmp = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--write-bmp") {
            writeBmp = true;
        } else {
            searchDir = arg;
        }
    }

    if (!fs::exists(searchDir)) {
        printf("Search directory not found: %s\n", searchDir.c_str());
        return 1;
    }

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

    printf("Found %zu BLP0 file(s) under %s\n\n", blp0Files.size(), searchDir.c_str());

    if (blp0Files.empty()) {
        printf("No BLP0 files found.\n");
        return 1;
    }

    for (const auto& path : blp0Files) {
        test_blp0_file(path, writeBmp);
        printf("\n");
    }

    printf("========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

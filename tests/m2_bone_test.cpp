// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// BONE validation: every `.bone` in the corpus is parsed and written back, and
// the write has to reproduce the original bytes exactly. The two chunks are
// paired into one vector in memory, so a byte-exact write is what proves the
// split is being reversed correctly.

#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <whiteout/models/m2/m2.h>
#include <whiteout/utils/os_file_system.h>

namespace fs = std::filesystem;
using namespace whiteout;

namespace {

std::vector<u8> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string corpusDir() {
    if (const char* env = std::getenv("M2_CORPUS_DIR"); env && fs::is_directory(env)) {
        return env;
    }
    for (auto candidate : {"Corpus/WoW", "../Corpus/WoW", "../../Corpus/WoW"}) {
        if (fs::is_directory(candidate)) {
            return candidate;
        }
    }
    return {};
}

} // namespace

TEST_CASE("BONE round-trips a synthetic file", "[m2][bone]") {
    m2::BoneOverrideSet source;
    source.version = 1;
    for (u16 i = 0; i < 5; ++i) {
        m2::BoneOverride entry;
        entry.boneIndex = static_cast<u16>(i * 7 + 3);
        entry.matrix = Matrix44f::identity();
        entry.matrix.data[3][0] = static_cast<f32>(i) * 0.25f;
        entry.matrix.data[3][2] = static_cast<f32>(i) * -0.5f;
        source.overrides.push_back(entry);
    }

    const auto bytes = m2::writeBoneOverrides(source);
    // 4 version + two chunk headers + 5 ids + 5 matrices.
    REQUIRE(bytes.size() == 4 + 8 + 5 * 2 + 8 + 5 * 64);

    std::vector<std::string> issues;
    const auto parsed = m2::parseBoneOverrides(bytes, &issues);
    CAPTURE(issues);
    REQUIRE(parsed);
    CHECK(parsed->version == source.version);
    REQUIRE(parsed->overrides.size() == source.overrides.size());
    for (size_t i = 0; i < source.overrides.size(); ++i) {
        CHECK(parsed->overrides[i].boneIndex == source.overrides[i].boneIndex);
        CHECK(parsed->overrides[i].matrix.data == source.overrides[i].matrix.data);
    }
    CHECK(m2::writeBoneOverrides(*parsed) == bytes);
}

TEST_CASE("BONE rejects mismatched chunk lengths", "[m2][bone]") {
    m2::BoneOverrideSet source;
    source.overrides.resize(3);
    auto bytes = m2::writeBoneOverrides(source);

    // Shrink BIDA by one id without touching BOMT: the pairing no longer holds,
    // and silently keeping the shorter of the two would corrupt every entry.
    bytes[4 + 4] = 4;
    std::vector<std::string> issues;
    CHECK_FALSE(m2::parseBoneOverrides(bytes, &issues).has_value());
    CHECK_FALSE(issues.empty());
}

TEST_CASE("BONE corpus round-trip", "[m2][bone][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }

    size_t files = 0, parsed = 0, byteExact = 0, entries = 0;
    std::map<u32, size_t> versions;

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".bone") {
            continue;
        }
        files++;
        const auto original = readFile(entry.path());
        const std::string name = entry.path().filename().string();

        std::vector<std::string> issues;
        const auto overrides = m2::parseBoneOverrides(original, &issues);
        INFO(name);
        CAPTURE(issues);
        REQUIRE(overrides);
        CHECK(issues.empty());
        parsed++;
        versions[overrides->version]++;
        entries += overrides->overrides.size();

        // Ids are ascending and unique in every corpus file; the client relies
        // on that ordering to find a bone.
        for (size_t i = 1; i < overrides->overrides.size(); ++i) {
            REQUIRE(overrides->overrides[i - 1].boneIndex <
                    overrides->overrides[i].boneIndex);
        }
        // Affine, translation in the last row.
        for (const auto& over : overrides->overrides) {
            REQUIRE(over.matrix.data[0][3] == 0.0f);
            REQUIRE(over.matrix.data[1][3] == 0.0f);
            REQUIRE(over.matrix.data[2][3] == 0.0f);
            REQUIRE(over.matrix.data[3][3] == 1.0f);
        }

        if (m2::writeBoneOverrides(*overrides) == original) {
            byteExact++;
        } else {
            FAIL_CHECK(name << ": write did not reproduce the original bytes");
        }
    }

    std::cout << "=== BONE corpus ===\n";
    std::cout << "  " << parsed << "/" << files << " parsed, " << byteExact
              << " byte-exact round-trips, " << entries << " bone overrides\n";
    std::cout << "  versions:";
    for (const auto& [v, count] : versions) {
        std::cout << " v" << v << "=" << count;
    }
    std::cout << std::endl;

    REQUIRE(files > 0);
    CHECK(parsed == files);
    CHECK(byteExact == files);
}

// The whole point of the BFID/sibling plumbing: a model has to reach its
// `.bone` files without the caller naming them.
TEST_CASE("M2 resolves .bone siblings into the model", "[m2][bone][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }

    size_t models = 0, withOverrides = 0, checkedIds = 0, noSkeleton = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".m2") {
            continue;
        }
        const fs::path base = entry.path();
        if (!fs::exists(base.parent_path() / (base.stem().string() + "_00.bone"))) {
            continue;
        }
        if (models >= 12) {
            break;
        }
        models++;

        utils::OsFileSystem vfs(base.parent_path().string());
        m2::Parser parser;
        m2::Model model = parser.parse(vfs, base.string());
        INFO(base.filename().string());
        REQUIRE_FALSE(model.boneOverrides.empty());
        withOverrides++;

        // Models whose `.skel` defers to a parent skeleton by file id resolve no
        // bones in path mode, so there is nothing to check the ids against.
        if (model.bones.empty()) {
            noSkeleton++;
            continue;
        }

        // Every override has to name a bone the model actually has, or the
        // customization would be writing into thin air.
        for (const auto& set : model.boneOverrides) {
            for (const auto& over : set.overrides) {
                REQUIRE(over.boneIndex < model.bones.size());
                checkedIds++;
            }
        }
    }

    std::cout << "=== M2 .bone resolution ===\n  " << withOverrides << "/" << models
              << " models carried bone overrides, " << checkedIds << " ids checked against bones, "
              << noSkeleton << " skipped (unresolved parent skeleton)" << std::endl;
    REQUIRE(models > 0);
    CHECK(withOverrides == models);
    CHECK(checkedIds > 0);
}
